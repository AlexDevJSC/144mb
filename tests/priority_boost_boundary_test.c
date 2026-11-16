/* SPDX-License-Identifier: MIT */
/* A dynamic Windows boost is not a Linux nice value. Exported functions
 * must fail with real access checks, never silently claim a policy exists. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;
static void check(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "boost boundary: %s\n", what);
        ++failures;
    }
}
static uint32_t NT_MS_ABI worker(void *unused)
{
    (void)unused;
    return 0x144u;
}
int main(void)
{
    const int before = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    uintptr_t query = nt_OpenProcess(0x1000u, 0, (uint32_t)getpid());
    uintptr_t set = nt_OpenProcess(0x200u, 0, (uint32_t)getpid());
    check(query != 0 && set != 0, "open permissioned process aliases");
    int32_t disabled = 0x5151;
    nt_SetLastError(0);
    check(!nt_GetProcessPriorityBoost(UINTPTR_MAX, &disabled) &&
          nt_GetLastError() == NT_ERROR_NOT_SUPPORTED && disabled == 0x5151,
          "process boost read cannot claim a fictitious kernel control");
    nt_SetLastError(0);
    check(!nt_SetProcessPriorityBoost(UINTPTR_MAX, 1) &&
          nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
          "process boost set cannot claim a fictitious policy");
    nt_SetLastError(0);
    check(!nt_SetProcessPriorityBoost(query, 1) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "process query-only handle denies boost set");
    nt_SetLastError(0);
    check(!nt_GetProcessPriorityBoost(set, &disabled) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED && disabled == 0x5151,
          "process set-only handle denies boost query");
    nt_SetLastError(0);
    check(!nt_GetProcessPriorityBoost(UINTPTR_MAX, NULL) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
          "null process output is rejected");

    uint32_t id = 0;
    const uintptr_t thread = nt_CreateThread(NULL, 0, worker, NULL, 4u, &id);
    check(thread && id, "create guest suspended thread");
    if (thread) {
        const uintptr_t tq = nt_OpenThread(0x800u, 0, id);
        const uintptr_t ts = nt_OpenThread(0x20u, 0, id);
        check(tq && ts, "create independent restricted thread aliases");
        nt_SetLastError(0);
        check(!nt_GetThreadPriorityBoost(thread, &disabled) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED && disabled == 0x5151,
              "thread boost get fails without fabricated state");
        nt_SetLastError(0);
        check(!nt_SetThreadPriorityBoost(thread, 1) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
              "thread boost set fails without host scheduling mutation");
        nt_SetLastError(0);
        check(!nt_SetThreadPriorityBoost(tq, 1) &&
              nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
              "query-only thread cannot set boost policy");
        nt_SetLastError(0);
        check(!nt_GetThreadPriorityBoost(ts, &disabled) &&
              nt_GetLastError() == NT_ERROR_ACCESS_DENIED && disabled == 0x5151,
              "set-only thread cannot query boost policy");
        nt_SetLastError(0);
        check(!nt_GetThreadPriorityBoost(thread, NULL) &&
              nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
              "null thread output rejected");
        check(nt_ResumeThread(thread) == 1 &&
              nt_WaitForSingleObject(thread, 5000) == 0,
              "the suspended guest thread is not frozen by failed boost requests");
        uint32_t exit_code = 0;
        check(nt_GetExitCodeThread(thread, &exit_code) && exit_code == 0x144u,
              "guest completed normally");
        check(nt_runtime_close_waitable(tq) &&
              nt_runtime_close_waitable(ts) &&
              nt_runtime_close_waitable(thread), "close thread handles");
    }
    check(nt_runtime_close_waitable(query) &&
          nt_runtime_close_waitable(set), "close process handles");
    errno = 0;
    check(getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid)) == before &&
          errno == 0, "no Linux nice mutation");
    if (!failures)
        puts("PASS priority boost: rights checked, unsupported truthfully, no host mutation");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
