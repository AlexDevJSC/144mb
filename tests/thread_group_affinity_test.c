/* SPDX-License-Identifier: MIT */
/* Priority 6: real Linux-backed Win64 GROUP_AFFINITY regression.
 * Only alter this test's own thread and a runtime-created child thread.
 * Always restore the original host affinity before leaving. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum { THREAD_SET_INFORMATION = 0x20, THREAD_QUERY_LIMITED_INFORMATION = 0x800 };
static cpu_set_t expected;

static void check(bool condition, const char *message, int *failures)
{
    if (!condition) {
        fprintf(stderr, "group affinity failure: %s\n", message);
        ++*failures;
    }
}

static uint32_t NT_MS_ABI verify_worker(void *unused)
{
    (void)unused;
    cpu_set_t actual;
    return sched_getaffinity(0, sizeof(actual), &actual) == 0 &&
           CPU_EQUAL(&actual, &expected) ? 0x144u : 0x1441u;
}

int main(void)
{
    int failures = 0;
    cpu_set_t original;
    if (sched_getaffinity(0, sizeof(original), &original) != 0)
        return 1;
    int first = -1, different = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &original))
            continue;
        if (first < 0)
            first = cpu;
        else if (cpu / 64 != first / 64 && different < 0)
            different = cpu;
    }
    if (first < 0)
        return 1;

    const struct nt_group_affinity wanted = {
        .mask = (uintptr_t)UINT64_C(1) << (first % 64),
        .group = (uint16_t)(first / 64)
    };
    struct nt_group_affinity observed = {.mask = 0x55u, .group = 0x777u};
    const bool multiple_groups = different >= 0;
    nt_SetLastError(0);
    if (multiple_groups) {
        check(!nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED &&
              observed.mask == 0x55u && observed.group == 0x777u,
              "cannot pretend a multigroup Linux cpuset has one Windows group",
              &failures);
        struct nt_group_affinity previous = {.mask = 0x55u, .group = 0x777u};
        check(!nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &wanted, &previous) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED &&
              previous.mask == 0x55u && previous.group == 0x777u,
              "do not mutate a multigroup cpuset when caller requests unrepresentable previous",
              &failures);
        cpu_set_t check_original;
        check(sched_getaffinity(0, sizeof(check_original), &check_original) == 0 &&
              CPU_EQUAL(&check_original, &original),
              "failure leaves the complete host cpuset unchanged", &failures);
    } else {
        check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
              observed.group == wanted.group,
              "read real single-group current thread", &failures);
    }

    nt_SetLastError(0x5151u);
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &wanted, NULL) &&
          nt_GetLastError() == 0x5151u,
          "pin current thread to a real CPU in the specified processor group",
          &failures);
    observed = (struct nt_group_affinity){0};
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
          observed.mask == wanted.mask && observed.group == wanted.group &&
          observed.reserved[0] == 0 && observed.reserved[1] == 0 &&
          observed.reserved[2] == 0,
          "roundtrip Win64 GROUP_AFFINITY, including reserved fields", &failures);

    struct nt_group_affinity previous = {.mask = 0x66u, .group = 0x777u};
    struct nt_group_affinity next = wanted;
    if (different >= 0) {
        next.group = (uint16_t)(different / 64);
        next.mask = (uintptr_t)UINT64_C(1) << (different % 64);
    }
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &next, &previous) &&
          previous.mask == wanted.mask && previous.group == wanted.group,
          "change across groups and report the actual prior group", &failures);
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
          observed.mask == next.mask && observed.group == next.group,
          "observe group transition from the Linux kernel", &failures);

    /* Legacy APIs cannot describe CPUs outside group 0; no truncation. */
    if (next.group > 0) {
        uintptr_t process = 0x5151u, system = 0x6161u;
        nt_SetLastError(0);
        check(!nt_GetProcessAffinityMask(UINTPTR_MAX, &process, &system) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED &&
              process == 0x5151u && system == 0x6161u,
              "legacy GetProcessAffinityMask refuses a group-1 mask",
              &failures);
    }

    struct nt_group_affinity bad = next;
    bad.reserved[1] = 1;
    nt_SetLastError(0);
    check(!nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &bad, NULL) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
          "reject reserved bits", &failures);
    bad = next;
    bad.mask = 0;
    nt_SetLastError(0);
    check(!nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &bad, NULL) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
          "reject empty mask", &failures);
    bad = next;
    bad.group = 65535u;
    nt_SetLastError(0);
    check(!nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &bad, NULL) &&
          nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
          "reject group beyond representable Linux CPU set", &failures);
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
          observed.mask == next.mask && observed.group == next.group,
          "invalid requests do not modify group affinity", &failures);

    check(sched_setaffinity(0, sizeof(original), &original) == 0,
          "restore test thread original affinity", &failures);

    CPU_ZERO(&expected);
    CPU_SET(first, &expected);
    uintptr_t thread = nt_CreateThread(NULL, 0, verify_worker, NULL, 4u, NULL);
    check(thread != 0, "create runtime-owned suspended guest thread", &failures);
    if (thread) {
        uintptr_t restricted_query = 0, restricted_set = 0;
        const uint32_t id = nt_GetThreadId(thread);
        restricted_query = nt_OpenThread(THREAD_QUERY_LIMITED_INFORMATION, 0, id);
        restricted_set = nt_OpenThread(THREAD_SET_INFORMATION, 0, id);
        check(restricted_query && restricted_set,
              "open independent query/set handles for the same canonical thread", &failures);
        nt_SetLastError(0);
        check(!nt_SetThreadGroupAffinity(restricted_query, &wanted, NULL) &&
              nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
              "query-only handle cannot change another thread's CPU placement", &failures);
        nt_SetLastError(0);
        check(!nt_GetThreadGroupAffinity(restricted_set, &observed) &&
              nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
              "set-only handle cannot read another thread's CPU placement", &failures);
        nt_SetLastError(0x5151u);
        check(nt_SetThreadGroupAffinity(thread, &wanted, NULL) &&
              nt_GetLastError() == 0x5151u,
              "pin suspended runtime-owned thread using canonical handle", &failures);
        check(nt_GetThreadGroupAffinity(restricted_query, &observed) &&
              observed.mask == wanted.mask && observed.group == wanted.group,
              "restricted query observes same live Linux thread", &failures);
        check(nt_ResumeThread(thread) == 1 &&
              nt_WaitForSingleObject(thread, 5000) == 0,
              "run and join guest after group pinning", &failures);
        uint32_t code = 0;
        check(nt_GetExitCodeThread(thread, &code) && code == 0x144u,
              "guest verified actual kernel CPU affinity", &failures);
        check(nt_runtime_close_waitable(restricted_query) &&
              nt_runtime_close_waitable(restricted_set) &&
              nt_runtime_close_waitable(thread),
              "release all thread aliases", &failures);
    }
    if (failures == 0)
        printf("PASS group affinity: real Linux group=%u, cross-group=%s, rights and restore\n",
               wanted.group, different >= 0 ? "yes" : "SKIP (one group permitted)");
    return failures != 0;
}
