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
    /* Process-visible group topology is a startup cpuset snapshot, not
     * recomputed from whichever thread happened to be pinned last. */
    uint32_t expected_per_group[CPU_SETSIZE / 64] = {0};
    uint16_t expected_groups = 0;
    uint32_t expected_total = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &original))
            continue;
        const uint16_t group = (uint16_t)(cpu / 64);
        ++expected_per_group[group];
        ++expected_total;
        if (expected_groups <= group)
            expected_groups = group + 1;
    }
    check(nt_GetActiveProcessorGroupCount() == expected_groups,
          "actual initial Linux cpuset defines processor group count", &failures);
    check(nt_GetActiveProcessorCount(UINT16_MAX) == expected_total,
          "ALL_PROCESSOR_GROUPS returns all permitted sandbox CPUs", &failures);
    for (uint16_t group = 0; group < expected_groups; ++group)
        check(nt_GetActiveProcessorCount(group) == expected_per_group[group],
              "processor group count is exact for initial Linux CPU set", &failures);
    check(nt_GetActiveProcessorCount(expected_groups) == 0,
          "group outside the startup topology reports no processors", &failures);

    uint16_t wanted_count = 0;
    uint16_t reported_groups[CPU_SETSIZE / 64];
    for (size_t i = 0; i < CPU_SETSIZE / 64; ++i)
        reported_groups[i] = 0x7777u;
    uint16_t active_nonempty = 0;
    for (uint16_t group = 0; group < expected_groups; ++group)
        if (expected_per_group[group] != 0)
            ++active_nonempty;
    nt_SetLastError(0);
    check(!nt_GetProcessGroupAffinity(UINTPTR_MAX, &wanted_count, reported_groups) &&
          wanted_count == active_nonempty &&
          nt_GetLastError() == NT_ERROR_INSUFFICIENT_BUFFER &&
          reported_groups[0] == 0x7777u,
          "group query reports required capacity without partial output", &failures);
    wanted_count = CPU_SETSIZE / 64;
    nt_SetLastError(0x3434u);
    check(nt_GetProcessGroupAffinity(UINTPTR_MAX, &wanted_count, reported_groups) &&
          wanted_count == active_nonempty && nt_GetLastError() == 0x3434u,
          "process group query returns permitted guest group list", &failures);
    uint16_t index = 0;
    for (uint16_t group = 0; group < expected_groups; ++group)
        if (expected_per_group[group] != 0)
            check(index < wanted_count && reported_groups[index++] == group,
                  "process group list is sorted and includes active groups",
                  &failures);
    uintptr_t process_set_only = nt_OpenProcess(0x200u, 0, (uint32_t)getpid());
    nt_SetLastError(0);
    check(process_set_only && !nt_GetProcessGroupAffinity(process_set_only,
             &wanted_count, reported_groups) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "process handle without query rights cannot inspect group membership",
          &failures);
    check(nt_runtime_close_waitable(process_set_only),
          "close set-only group-query handle", &failures);

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
    uintptr_t initial_primary_mask = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, &original) && cpu / 64 == wanted.group)
            initial_primary_mask |= (uintptr_t)UINT64_C(1) << (cpu % 64);
    nt_SetLastError(0x3434u);
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
          observed.group == wanted.group &&
          observed.mask == initial_primary_mask &&
          nt_GetLastError() == 0x3434u,
          "primary affinity reports its group even with multigroup host cpuset",
          &failures);
    if (multiple_groups) {
        /* With a multi-group initial host cpuset, an old-style 64-bit mask
         * applies only to the primary group, not to a truncated full cpuset. */
        nt_SetLastError(0x4545u);
        check(nt_SetThreadAffinityMask(nt_GetCurrentThread(), wanted.mask) ==
              initial_primary_mask && nt_GetLastError() == 0x4545u,
              "legacy affinity returns primary-group mask from multi-group host",
              &failures);
        cpu_set_t narrowed;
        check(sched_getaffinity(0, sizeof(narrowed), &narrowed) == 0 &&
              CPU_COUNT(&narrowed) == 1 && CPU_ISSET(first, &narrowed),
              "legacy affinity confined actual Linux cpuset to primary group",
              &failures);
        check(sched_setaffinity(0, sizeof(original), &original) == 0,
              "restore original multi-group test cpuset", &failures);
    }
    struct nt_group_affinity original_group = {0};
    nt_SetLastError(0x4141u);
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &wanted, &original_group) &&
          original_group.group == wanted.group &&
          original_group.mask == initial_primary_mask &&
          nt_GetLastError() == 0x4141u,
          "SetThreadGroupAffinity reports prior primary group even if multi-group",
          &failures);
    nt_SetLastError(0x5151u);
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &wanted, NULL) &&
          nt_GetLastError() == 0x5151u,
          "pin current thread to a real CPU in the specified processor group",
          &failures);
    check(nt_GetActiveProcessorGroupCount() == expected_groups &&
          nt_GetActiveProcessorCount(UINT16_MAX) == expected_total,
          "pinning a thread does not rewrite global processor topology", &failures);
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

    /* The legacy 64-bit mask is group-local once the thread has an
     * unambiguous group. In particular, group 1 must not be confused with
     * Linux CPU indices 0..63. The underlying affinity remains exact. */
    nt_SetLastError(0x7777u);
    check(nt_SetThreadAffinityMask(nt_GetCurrentThread(), next.mask) ==
          next.mask && nt_GetLastError() == 0x7777u,
          "legacy SetThreadAffinityMask operates in the current group",
          &failures);
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
          observed.group == next.group && observed.mask == next.mask,
          "legacy group-local operation preserves the processor group",
          &failures);
    uintptr_t native_mask = next.mask;
    check(nt_NtSetInformationThread(nt_GetCurrentThread(), 4u, &native_mask,
                                    sizeof(native_mask)) == NT_STATUS_SUCCESS,
          "native ThreadAffinityMask also uses current group-relative bits",
          &failures);
    cpu_set_t group_actual;
    check(sched_getaffinity(0, sizeof(group_actual), &group_actual) == 0 &&
          CPU_COUNT(&group_actual) == 1 &&
          CPU_ISSET((int)next.group * 64 +
                    (int)__builtin_ctzll((unsigned long long)next.mask), &group_actual),
          "kernel confirms exact CPU after legacy and NT group-local calls",
          &failures);

    /* A mask that names a CPU outside the sandbox's group entitlement
     * must fail without changing the thread. */
    unsigned forbidden_bit = 64;
    for (unsigned bit = 0; bit < 64; ++bit) {
        if (!CPU_ISSET((int)next.group * 64 + (int)bit, &original)) {
            forbidden_bit = bit;
            break;
        }
    }
    if (forbidden_bit < 64) {
        const uintptr_t invalid_mask = (uintptr_t)UINT64_C(1) << forbidden_bit;
        nt_SetLastError(0);
        check(nt_SetThreadAffinityMask(nt_GetCurrentThread(), invalid_mask) == 0 &&
              nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
              "reject unavailable group-local CPU without changing affinity",
              &failures);
        check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &observed) &&
              observed.group == next.group && observed.mask == next.mask,
              "invalid legacy group-local mask preserves real CPU placement",
              &failures);
    }

    /* Windows 11: the current process is viewed through the calling
     * thread's primary group, including when that group is above 0. */
    uintptr_t process = 0, system = 0;
    nt_SetLastError(0x6161u);
    check(nt_GetProcessAffinityMask(UINTPTR_MAX, &process, &system) &&
          system != 0 && (system & next.mask) == next.mask &&
          (process & system) == process &&
          nt_GetLastError() == 0x6161u,
          "process mask query uses caller primary group without truncation",
          &failures);

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
