/* SPDX-License-Identifier: MIT */
/* Problem 6: SetProcessAffinityMask must change a verified Linux CPU set
 * only in an actual single-thread runtime, never just the caller among
 * several runnable Win64 threads. This executable is disposable. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static unsigned failures;
static cpu_set_t expected;
static void check(int success, const char *message)
{
    if (!success) {
        fprintf(stderr, "process affinity test failure: %s\n", message);
        ++failures;
    }
}

static uint32_t NT_MS_ABI affinity_worker(void *unused)
{
    (void)unused;
    cpu_set_t actual;
    return sched_getaffinity(0, sizeof(actual), &actual) == 0 &&
           CPU_EQUAL(&actual, &expected) ? 0x144u : 0x1441u;
}

int main(void)
{
    cpu_set_t original;
    if (sched_getaffinity(0, sizeof(original), &original) != 0)
        return EXIT_FAILURE;
    int first = -1, second = -1;
    for (int cpu = 0; cpu < 64; ++cpu) {
        if (!CPU_ISSET(cpu, &original))
            continue;
        if (first == -1)
            first = cpu;
        else if (second == -1)
            second = cpu;
    }
    if (first == -1) {
        uintptr_t process = 0x144u, system = 0x145u;
        nt_SetLastError(0);
        check(!nt_GetProcessAffinityMask(UINTPTR_MAX, &process, &system) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED &&
              process == 0x144u && system == 0x145u,
              "group0 cannot be fabricated from high Linux CPU IDs");
        nt_SetLastError(0);
        check(!nt_SetProcessAffinityMask(UINTPTR_MAX, 1u) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
              "group0 mask cannot be applied over higher-only cpuset");
        puts("SKIP positive process affinity: no allowed CPU in group 0");
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    /* Restrict this disposable host process before the runtime ever
     * captures its startup sandbox CPU entitlement. */
    cpu_set_t baseline;
    CPU_ZERO(&baseline);
    CPU_SET(first, &baseline);
    if (second >= 0)
        CPU_SET(second, &baseline);
    check(sched_setaffinity(0, sizeof(baseline), &baseline) == 0,
          "prepare one-group Linux cpuset");
    const uintptr_t first_mask = (uintptr_t)UINT64_C(1) << first;
    const uintptr_t baseline_mask = first_mask |
        (second < 0 ? 0 : (uintptr_t)UINT64_C(1) << second);
    uintptr_t process_mask = 0, system_mask = 0;
    nt_SetLastError(0x5151u);
    check(nt_GetProcessAffinityMask(UINTPTR_MAX, &process_mask, &system_mask) &&
          process_mask == baseline_mask && system_mask == baseline_mask &&
          nt_GetLastError() == 0x5151u,
          "initial process and sandbox system affinity agree");

    uintptr_t query_only = nt_OpenProcess(0x1000u, 0, (uint32_t)getpid());
    uintptr_t set_only = nt_OpenProcess(0x200u, 0, (uint32_t)getpid());
    check(query_only && set_only, "open independently permissioned process handles");
    nt_SetLastError(0);
    check(!nt_SetProcessAffinityMask(query_only, first_mask) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "PROCESS_QUERY_LIMITED_INFORMATION cannot set affinity");
    nt_SetLastError(0);
    check(!nt_GetProcessAffinityMask(set_only, &process_mask, &system_mask) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "PROCESS_SET_INFORMATION alone cannot query affinity");
    nt_SetLastError(0);
    check(!nt_SetProcessAffinityMask(UINTPTR_MAX, 0) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
          "zero CPU mask rejected");
    nt_SetLastError(0);
    check(!nt_SetProcessAffinityMask((uintptr_t)getpid(), first_mask) &&
          nt_GetLastError() == NT_ERROR_INVALID_HANDLE,
          "raw Linux PID cannot be used as a Windows process HANDLE");

    nt_SetLastError(0x6161u);
    check(nt_SetProcessAffinityMask(set_only, first_mask) &&
          nt_GetLastError() == 0x6161u,
          "apply exact first-CPU mask using a real authorized handle");
    nt_SetLastError(0x7171u);
    check(nt_GetProcessAffinityMask(UINTPTR_MAX, &process_mask, &system_mask) &&
          process_mask == first_mask && system_mask == baseline_mask &&
          nt_GetLastError() == 0x7171u,
          "system mask retains original sandbox CPUs after process pinning");
    cpu_set_t after;
    CPU_ZERO(&after);
    CPU_SET(first, &after);
    cpu_set_t observed;
    check(sched_getaffinity(0, sizeof(observed), &observed) == 0 &&
          CPU_EQUAL(&observed, &after),
          "Linux kernel confirms exact process mask");

    if (second >= 0) {
        const uintptr_t second_mask = (uintptr_t)UINT64_C(1) << second;
        nt_SetLastError(0);
        check(nt_SetProcessAffinityMask(UINTPTR_MAX, second_mask),
              "single-thread process may move to another permitted CPU");
        check(nt_GetProcessAffinityMask(UINTPTR_MAX, &process_mask, &system_mask) &&
              process_mask == second_mask && system_mask == baseline_mask,
              "process mask moves while system mask stays constant");
        check(nt_SetProcessAffinityMask(UINTPTR_MAX, first_mask),
              "return to first CPU before starting guest thread");
    }

    CPU_ZERO(&expected);
    CPU_SET(first, &expected);
    uintptr_t worker = nt_CreateThread(NULL, 0, affinity_worker, NULL, 4u, NULL);
    check(worker != 0, "create a suspended runtime-owned thread");
    if (worker) {
        nt_SetLastError(0);
        check(!nt_SetProcessAffinityMask(set_only, baseline_mask) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
              "reject process-wide mutation while another pthread exists");
        check(sched_getaffinity(0, sizeof(observed), &observed) == 0 &&
              CPU_EQUAL(&observed, &expected),
              "failed multithread request did not partially change current TID");
        check(nt_ResumeThread(worker) == 1 &&
              nt_WaitForSingleObject(worker, 5000) == 0,
              "resume and join the unchanged worker");
        uint32_t exit_code = 0;
        check(nt_GetExitCodeThread(worker, &exit_code) && exit_code == 0x144u,
              "worker observed its originally inherited kernel affinity");
        check(nt_runtime_close_waitable(worker), "close worker handle");
    }

    check(nt_runtime_close_waitable(query_only) &&
          nt_runtime_close_waitable(set_only), "close permissioned process handles");
    check(sched_setaffinity(0, sizeof(original), &original) == 0,
          "restore original host cpuset");
    if (!failures)
        puts("PASS SetProcessAffinityMask: real Linux affinity, stable system mask, rights, multithread rejection and restoration");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
