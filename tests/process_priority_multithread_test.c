/* SPDX-License-Identifier: MIT */
/* Full-process priority-class transition. This executable is disposable
 * because Linux nice downgrades may be irreversible without privileges.
 * A changed class is accepted only if ALL tracked guest TIDs were
 * actually updated; a denied request must leave ALL TIDs untouched. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { NORMAL_PRIORITY_CLASS = 0x20u,
       BELOW_NORMAL_PRIORITY_CLASS = 0x4000u };
static uint32_t expected_class;
static int expected_nice;
static int failures;

static void expect(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "process class multithread: %s\n", message);
        ++failures;
    }
}

static uint32_t NT_MS_ABI worker(void *unused)
{
    (void)unused;
    errno = 0;
    const int actual = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    return errno == 0 && actual == expected_nice &&
           nt_GetPriorityClass(UINTPTR_MAX) == expected_class &&
           nt_GetThreadPriority(nt_GetCurrentThread()) == 0 ?
           0x144u : 0x1441u;
}

int main(void)
{
    errno = 0;
    const int initial = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno != 0 || initial != 0) {
        puts("SKIP multithread priority test: initial host nice != 0");
        return EXIT_SUCCESS;
    }
    uint32_t id = 0;
    uintptr_t thread = nt_CreateThread(NULL, 0, worker, NULL, 4u, &id);
    expect(thread != 0 && id != 0, "create owned suspended guest");
    if (!thread)
        return EXIT_FAILURE;
    errno = 0;
    expect(getpriority(PRIO_PROCESS, (id_t)id) == 0 && errno == 0,
           "worker starts with actual nice 0");

    nt_SetLastError(0x7171u);
    const bool changed = nt_SetPriorityClass(UINTPTR_MAX,
                                               BELOW_NORMAL_PRIORITY_CLASS) != 0;
    if (changed) {
        expected_class = BELOW_NORMAL_PRIORITY_CLASS;
        expected_nice = 4;
        expect(nt_GetLastError() == 0x7171u,
               "successful transition preserves LastError");
        puts("PASS privileged multithread priority transition 0->4");
    } else {
        expected_class = NORMAL_PRIORITY_CLASS;
        expected_nice = 0;
        expect(nt_GetLastError() == NT_ERROR_NOT_SUPPORTED ||
               nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
               "unsupported host rollback or privilege yields explicit error");
        puts("SKIP positive multithread priority transition: host cannot safely restore nice");
    }
    errno = 0;
    expect(getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)) == expected_nice &&
           errno == 0,
           "main TID retained or changed to exactly the agreed nice");
    errno = 0;
    expect(getpriority(PRIO_PROCESS, (id_t)id) == expected_nice && errno == 0,
           "worker TID retained or changed to exactly the agreed nice");
    expect(nt_GetPriorityClass(UINTPTR_MAX) == expected_class,
           "Win32 class is committed only after verified Linux change");
    expect(nt_GetThreadPriority(thread) == 0,
           "worker relative priority remains normal after class transition");

    expect(nt_ResumeThread(thread) == 1 &&
           nt_WaitForSingleObject(thread, 5000) == 0,
           "release worker after scheduler update");
    uint32_t exit_code = 0;
    expect(nt_GetExitCodeThread(thread, &exit_code) &&
           exit_code == 0x144u,
           "worker observes correct class and kernel nice");
    expect(nt_runtime_close_waitable(thread),
           "close canonical worker handle");
    if (failures == 0)
        puts("PASS multithread class scheduling, verified rollback safety and guest observation");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
