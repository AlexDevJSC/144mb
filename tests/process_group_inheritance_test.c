/* SPDX-License-Identifier: MIT */
/* Windows 11 multigroup process-primary transition and inherited secondary
 * thread guard, using ONLY this disposable process's owned guest threads. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
static int expected_cpu;
static uint16_t expected_group;
static uintptr_t expected_mask;

static void check(int value, const char *message)
{
    if (!value) {
        fprintf(stderr, "process group inheritance: %s\n", message);
        ++failures;
    }
}
static uint32_t NT_MS_ABI worker(void *unused)
{
    (void)unused;
    cpu_set_t actual;
    struct nt_group_affinity group = {0};
    return sched_getaffinity(0, sizeof(actual), &actual) == 0 &&
           CPU_COUNT(&actual) == 1 &&
           CPU_ISSET(expected_cpu, &actual) &&
           nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &group) &&
           group.group == expected_group && group.mask == expected_mask ?
           0x144u : 0x1441u;
}

int main(void)
{
    cpu_set_t original;
    if (sched_getaffinity(0, sizeof(original), &original) != 0)
        return EXIT_FAILURE;
    int first = -1, secondary = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &original))
            continue;
        if (first < 0)
            first = cpu;
        else if (secondary < 0 && cpu / 64 != first / 64)
            secondary = cpu;
    }
    if (secondary < 0) {
        puts("SKIP process group inheritance: host has one allowed Linux CPU group");
        return EXIT_SUCCESS;
    }
    struct nt_group_affinity primary = {
        .group = (uint16_t)(first / 64),
        .mask = (uintptr_t)UINT64_C(1) << (first % 64)
    };
    struct nt_group_affinity other = {
        .group = (uint16_t)(secondary / 64),
        .mask = (uintptr_t)UINT64_C(1) << (secondary % 64)
    };
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &primary, NULL),
          "pin current thread to original primary group");
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &other, NULL),
          "temporarily move parent to alternate group");
    uintptr_t child = nt_CreateThread(NULL, 0, worker, NULL, 4u, NULL);
    check(child != 0, "create suspended child in alternate primary group");
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &primary, NULL),
          "restore parent to primary group before process-wide transition");

    if (child != 0) {
        struct nt_group_affinity actual = {0};
        check(nt_GetThreadGroupAffinity(child, &actual) &&
              actual.group == other.group && actual.mask == other.mask,
              "child inherited explicit nonprimary group across CreateThread");
        uint16_t count = 16, groups[16] = {0};
        check(nt_GetProcessGroupAffinity(UINTPTR_MAX, &count, groups) &&
              count >= 2, "live guest process really spans multiple groups");
        nt_SetLastError(0);
        check(!nt_SetProcessAffinityMask(UINTPTR_MAX, primary.mask) &&
              nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
              "process transition denied with live child outside primary group");
        check(nt_GetThreadGroupAffinity(child, &actual) &&
              actual.group == other.group && actual.mask == other.mask,
              "rejected transition did not alter secondary thread");
        check(nt_SetThreadGroupAffinity(child, &primary, NULL),
              "move child back to process primary group");
        expected_cpu = first;
        expected_group = primary.group;
        expected_mask = primary.mask;
        check(nt_SetProcessAffinityMask(UINTPTR_MAX, primary.mask),
              "process transition now succeeds after secondary child returned");
        check(nt_ResumeThread(child) == 1 &&
              nt_WaitForSingleObject(child, 5000) == 0,
              "run and join child after verified process-wide transition");
        uint32_t exit_code = 0;
        check(nt_GetExitCodeThread(child, &exit_code) && exit_code == 0x144u,
              "child observed primary group and real Linux CPU at entry");
        check(nt_runtime_close_waitable(child), "close canonical child handle");
    }
    check(sched_setaffinity(0, sizeof(original), &original) == 0,
          "restore original host cpuset");
    if (!failures)
        puts("PASS multigroup process policy: inherited secondary group denied, restored and committed");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
