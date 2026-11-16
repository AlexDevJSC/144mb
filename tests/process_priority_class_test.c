/* SPDX-License-Identifier: MIT */
/* Scheduler-backed priority classes, single-thread transition guarantee.
 * This executable is disposable: lowering Linux nice is not reversible
 * without CAP_SYS_NICE. Never mutate another process or host TID. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { PROCESS_SET_INFORMATION = 0x200, PROCESS_QUERY_LIMITED_INFORMATION = 0x1000,
       NORMAL_PRIORITY_CLASS = 0x20, IDLE_PRIORITY_CLASS = 0x40,
       REALTIME_PRIORITY_CLASS = 0x100, BELOW_NORMAL_PRIORITY_CLASS = 0x4000 };

static int failures;
static void expect(int value, const char *description)
{
    if (!value) {
        ++failures;
        fprintf(stderr, "priority class failure: %s\n", description);
    }
}

static uint32_t NT_MS_ABI worker(void *parameter)
{
    (void)parameter;
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    return errno == 0 && nice == 12 &&
           nt_GetThreadPriority(nt_GetCurrentThread()) == -1 &&
           nt_GetPriorityClass(UINTPTR_MAX) == IDLE_PRIORITY_CLASS ?
           0x144u : 0x1441u;
}

int main(void)
{
    errno = 0;
    const int initial = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno != 0 || initial != 0) {
        puts("SKIP process class positive test: initial host nice is not 0");
        return 0;
    }
    uintptr_t read_only = nt_OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0,
                                         (uint32_t)getpid());
    uintptr_t write_only = nt_OpenProcess(PROCESS_SET_INFORMATION, 0,
                                          (uint32_t)getpid());
    expect(read_only && write_only, "open independent process handles");
    nt_SetLastError(0x5151u);
    expect(nt_GetPriorityClass(read_only) == NORMAL_PRIORITY_CLASS &&
           nt_GetLastError() == 0x5151u, "normal class and unchanged LastError");
    nt_SetLastError(0);
    expect(!nt_SetPriorityClass(read_only, BELOW_NORMAL_PRIORITY_CLASS) &&
           nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
           "query-only handle cannot set class");
    nt_SetLastError(0);
    expect(!nt_GetPriorityClass(write_only) &&
           nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
           "write-only handle cannot query class");
    nt_SetLastError(0);
    expect(!nt_SetPriorityClass(UINTPTR_MAX, REALTIME_PRIORITY_CLASS) &&
           nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
           "reject Windows realtime scheduling, never fake it");
    nt_SetLastError(0);
    expect(!nt_SetPriorityClass(UINTPTR_MAX, 0x123456u) &&
           nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
           "reject an unknown priority class");
    nt_SetLastError(0x5151u);
    expect(nt_SetPriorityClass(UINTPTR_MAX, BELOW_NORMAL_PRIORITY_CLASS) &&
           nt_GetLastError() == 0x5151u &&
           nt_GetPriorityClass(UINTPTR_MAX) == BELOW_NORMAL_PRIORITY_CLASS &&
           nt_runtime_current_process_base_priority() == 6 &&
           nt_GetThreadPriority(nt_GetCurrentThread()) == 0,
           "real BELOW_NORMAL priority: Linux nice 4, relative thread 0");
    errno = 0;
    expect(getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)) == 4 &&
           errno == 0, "read actual nice 4 in host kernel");

    nt_SetLastError(0x6161u);
    expect(nt_SetPriorityClass(UINTPTR_MAX, IDLE_PRIORITY_CLASS) &&
           nt_GetLastError() == 0x6161u &&
           nt_GetPriorityClass(UINTPTR_MAX) == IDLE_PRIORITY_CLASS &&
           nt_runtime_current_process_base_priority() == 4 &&
           nt_GetThreadPriority(nt_GetCurrentThread()) == 0,
           "real IDLE priority: Linux nice 10, relative thread 0");
    errno = 0;
    expect(getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)) == 10 &&
           errno == 0, "read actual nice 10 in host kernel");

    uintptr_t thread = nt_CreateThread(NULL, 0, worker, NULL, 4, NULL);
    expect(thread != 0, "create suspended worker under IDLE class");
    if (thread) {
        nt_SetLastError(0);
        expect(!nt_SetPriorityClass(UINTPTR_MAX, NORMAL_PRIORITY_CLASS) &&
               nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
               "deny multi-thread class change before it can partially apply");
        expect(nt_GetThreadPriority(thread) == 0,
               "new thread inherited process class base nice");
        nt_SetLastError(0x7171u);
        expect(nt_SetThreadPriority(thread, -1) &&
               nt_GetLastError() == 0x7171u &&
               nt_GetThreadPriority(thread) == -1,
               "set relative BELOW_NORMAL on inherited nice=10 thread");
        expect(nt_ResumeThread(thread) == 1 &&
               nt_WaitForSingleObject(thread, 5000) == 0,
               "execute and join new guest thread");
        uint32_t result = 0;
        expect(nt_GetExitCodeThread(thread, &result) && result == 0x144u,
               "worker observed actual nice 12 and relative priority -1");
        expect(nt_runtime_close_waitable(thread), "close thread handle");
    }
    expect(nt_runtime_close_waitable(read_only) &&
           nt_runtime_close_waitable(write_only), "close process handles");
    if (failures == 0)
        puts("PASS priority classes: real nice 0->4->10, rights, no partial multithread updates, relative thread nice");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
