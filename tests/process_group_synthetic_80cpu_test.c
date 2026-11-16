/* SPDX-License-Identifier: MIT */
/* Synthetic, SPARSE 80-CPU topology: wrappers expose Linux IDs
 * 24..63, 72..103 and 200..207, which Windows must compact into
 * group0 slots 0..63 and group1 slots 0..15. NOT physical acceptance.
 * Affinity mutations are trapped within the test; no host task changes. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"
#include <sched.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
static bool synthetic_pinned;
static cpu_set_t synthetic_affinity;
static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "synthetic 80-CPU group model: %s\n", message);
        ++failures;
    }
}
static void fake_topology(cpu_set_t *cpus)
{
    CPU_ZERO(cpus);
    for (int i = 24; i <= 63; ++i)
        CPU_SET(i, cpus);
    for (int i = 72; i <= 103; ++i)
        CPU_SET(i, cpus);
    for (int i = 200; i <= 207; ++i)
        CPU_SET(i, cpus);
}
int __wrap_sched_getaffinity(pid_t pid, size_t bytes, cpu_set_t *cpus)
{
    (void)pid;
    if (bytes < sizeof(*cpus))
        return -1;
    fake_topology(cpus);
    return 0;
}
int __wrap_pthread_getaffinity_np(pthread_t thread, size_t bytes, cpu_set_t *cpus)
{
    (void)thread;
    if (bytes < sizeof(*cpus))
        return EINVAL;
    if (synthetic_pinned)
        *cpus = synthetic_affinity;
    else
        fake_topology(cpus);
    return 0;
}
int __wrap_pthread_setaffinity_np(pthread_t thread, size_t bytes,
                                  const cpu_set_t *cpus)
{
    (void)thread;
    if (bytes < sizeof(*cpus) || CPU_COUNT(cpus) == 0)
        return EINVAL;
    cpu_set_t permitted;
    fake_topology(&permitted);
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, cpus) && !CPU_ISSET(cpu, &permitted))
            return EINVAL;
    synthetic_affinity = *cpus;
    synthetic_pinned = true;
    return 0;
}
int __wrap_sched_getcpu(void)
{
    return 200; /* guest group 1, ordinal bit 8 */
}

int main(void)
{
    check(nt_GetActiveProcessorGroupCount() == 2,
          "80 synthetic logical CPUs occupy two groups");
    check(nt_GetActiveProcessorCount(0) == 64 &&
          nt_GetActiveProcessorCount(1) == 16 &&
          nt_GetActiveProcessorCount(UINT16_MAX) == 80,
          "exact active CPU counts without physical 80-CPU host");
    struct nt_processor_number current = {0};
    nt_GetCurrentProcessorNumberEx(&current);
    check(current.group == 1 && current.number == 8 &&
          nt_GetCurrentProcessorNumber() == 8,
          "Linux CPU 200 becomes guest CPU 72 (group 1, bit 8)");
    struct nt_group_affinity primary = {0};
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &primary) &&
          primary.group == 0 && primary.mask == UINT64_MAX,
          "primary group query works with a cross-group allowed cpuset");
    uint16_t groups[2] = {UINT16_MAX, UINT16_MAX}, capacity = 2;
    check(nt_GetProcessGroupAffinity(UINTPTR_MAX, &capacity, groups) &&
          capacity == 2 && groups[0] == 0 && groups[1] == 1,
          "process group enumeration reports both groups");
    uintptr_t process = 0, system = 0;
    check(nt_GetProcessAffinityMask(UINTPTR_MAX, &process, &system) &&
          process == UINT64_MAX && system == UINT64_MAX,
          "current process mask reports the primary group without truncation");
    struct nt_group_affinity previous = {0};
    const struct nt_group_affinity desired = {.group = 1, .mask = 1};
    check(nt_SetThreadGroupAffinity(nt_GetCurrentThread(), &desired, &previous) &&
          previous.group == 0 && previous.mask == UINT64_MAX,
          "synthetic affinity transaction returns compact previous group");
    check(synthetic_pinned && CPU_COUNT(&synthetic_affinity) == 1 &&
          CPU_ISSET(96, &synthetic_affinity),
          "group1/bit0 translates to actual sparse Linux CPU 96");
    struct nt_group_affinity after = {0};
    check(nt_GetThreadGroupAffinity(nt_GetCurrentThread(), &after) &&
          after.group == 1 && after.mask == 1,
          "primary group persists after synthetic Linux pinning");
    if (!failures)
        puts("PASS sparse synthetic 80-CPU compact guest groups (NOT physical hardware)");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
