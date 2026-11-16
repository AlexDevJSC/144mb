/* SPDX-License-Identifier: MIT */
/* Synthetic 80-logical-CPU topology: read-only injection into the REAL
 * runtime's Linux-affinity query paths. This is not hardware acceptance.
 * No sched_setaffinity, real CPU migration, or external TID modification. */
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
    for (int i = 0; i < 80; ++i)
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
    fake_topology(cpus);
    return 0;
}

int main(void)
{
    check(nt_GetActiveProcessorGroupCount() == 2,
          "80 synthetic logical CPUs occupy two groups");
    check(nt_GetActiveProcessorCount(0) == 64 &&
          nt_GetActiveProcessorCount(1) == 16 &&
          nt_GetActiveProcessorCount(UINT16_MAX) == 80,
          "exact active CPU counts without physical 80-CPU host");
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
    if (!failures)
        puts("PASS synthetic 80-CPU primary/group queries (NOT physical hardware)");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
