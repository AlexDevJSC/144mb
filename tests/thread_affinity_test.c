/* SPDX-License-Identifier: MIT */
/* Stage-4 real Linux pthread affinity and HANDLE-lifetime regression.
 * This test narrows its *own* test process's CPU affinity; never edits a
 * host process other than itself. It restores the original cpuset. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"

#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { THREAD_SET_INFORMATION = 0x20, THREAD_QUERY_LIMITED_INFORMATION = 0x800,
       THREAD_ALL_ACCESS = 0x1fffff, SYNCHRONIZE = 0x100000 };

static cpu_set_t expected;
static int failures;

static void check(int value, const char *label)
{
    if (!value) {
        fprintf(stderr, "thread affinity failure: %s\n", label);
        ++failures;
    }
}

static uint32_t NT_MS_ABI affinity_worker(void *unused)
{
    (void)unused;
    cpu_set_t current;
    if (sched_getaffinity(0, sizeof(current), &current) != 0 ||
        !CPU_EQUAL(&current, &expected))
        return 0x1441u;
    return 0x144u;
}

int main(void)
{
    cpu_set_t original;
    if (sched_getaffinity(0, sizeof(original), &original) != 0)
        return EXIT_FAILURE;
    int selected = -1;
    for (int i = 0; i < 64; ++i)
        if (CPU_ISSET(i, &original)) {
            selected = i;
            break;
        }
    if (selected == -1) {
        /* No CPU fits the Windows x64 processor-group profile. Verify
         * honest failure, not guessed/truncated success. */
        uintptr_t process_mask = 42, system_mask = 43;
        nt_SetLastError(0);
        check(!nt_GetProcessAffinityMask(UINTPTR_MAX, &process_mask, &system_mask) &&
              nt_GetLastError() == NT_ERROR_NOT_SUPPORTED &&
              process_mask == 42 && system_mask == 43,
              "reject unrepresentable host group");
        puts("SKIP positive processor-group case: host cpuset has no CPUs below 64");
        return failures ? EXIT_FAILURE : EXIT_SUCCESS;
    }

    CPU_ZERO(&expected);
    CPU_SET(selected, &expected);
    check(sched_setaffinity(0, sizeof(expected), &expected) == 0,
          "limit test process affinity");
    uintptr_t cpu_mask = (uintptr_t)UINT64_C(1) << selected;
    uintptr_t process_mask = 0, system_mask = 0;
    nt_SetLastError(0x1440u);
    check(nt_GetProcessAffinityMask(UINTPTR_MAX, &process_mask, &system_mask) &&
          process_mask == cpu_mask && system_mask == cpu_mask &&
          nt_GetLastError() == 0x1440u,
          "report actual sandbox CPU mask");
    uintptr_t self_query = nt_OpenProcess(0x1000u, 0, (uint32_t)getpid());
    check(self_query && nt_GetProcessAffinityMask(self_query, &process_mask, &system_mask),
          "query affinity with restricted process handle");
    uintptr_t self_terminate = nt_OpenProcess(1, 0, (uint32_t)getpid());
    nt_SetLastError(0);
    check(self_terminate != 0 &&
          !nt_GetProcessAffinityMask(self_terminate, &process_mask, &system_mask) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "deny process affinity query without rights");
    check(nt_runtime_close_waitable(self_query) &&
          nt_runtime_close_waitable(self_terminate), "close process aliases");
    nt_SetLastError(0);
    check(!nt_GetProcessAffinityMask(UINTPTR_MAX, NULL, &system_mask) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER, "reject null mask output");

    uint32_t id = 0;
    uintptr_t thread = nt_CreateThread(NULL, 0, affinity_worker, NULL, 4, &id);
    check(thread && id, "create suspended worker");
    uintptr_t query_only = nt_OpenThread(THREAD_QUERY_LIMITED_INFORMATION, 0, id);
    nt_SetLastError(0);
    check(query_only && !nt_SetThreadAffinityMask(query_only, cpu_mask) &&
          nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
          "deny affinity modification without set rights");

    nt_SetLastError(0);
    check(nt_SetThreadAffinityMask(thread, cpu_mask) == cpu_mask,
          "apply exact Linux processor set to suspended worker");
    check(!nt_SetThreadAffinityMask(thread, 0) &&
          nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
          "reject zero affinity mask");
    nt_SetLastError(0x9777u);
    check(nt_NtSetInformationThread(thread, 4, &cpu_mask, sizeof(cpu_mask)) ==
          NT_STATUS_SUCCESS && nt_GetLastError() == 0x9777u,
          "NT ThreadAffinityMask preserves LastError");
    check(nt_NtSetInformationThread(thread, 4, &cpu_mask, 1) ==
          NT_STATUS_INFO_LENGTH_MISMATCH &&
          nt_NtSetInformationThread(thread, 77, &cpu_mask, sizeof(cpu_mask)) ==
          NT_STATUS_INVALID_INFO_CLASS &&
          nt_NtSetInformationThread(thread, 4, NULL, sizeof(cpu_mask)) ==
          NT_STATUS_ACCESS_VIOLATION,
          "native affinity validation");

    uintptr_t alias = 0;
    check(nt_runtime_duplicate_waitable(thread, THREAD_SET_INFORMATION,
          0, 0, &alias) == NT_STATUS_SUCCESS && alias,
          "duplicate set-only thread handle");
    check(nt_SetThreadAffinityMask(alias, cpu_mask) == cpu_mask,
          "apply affinity via alias of same host thread");
    check(nt_runtime_close_waitable(alias) &&
          nt_runtime_close_waitable(query_only), "release restricted handles");
    check(nt_ResumeThread(thread) == 1 &&
          nt_WaitForSingleObject(thread, 5000) == 0,
          "thread executes on requested host CPU");
    uint32_t code = 0;
    check(nt_GetExitCodeThread(thread, &code) && code == 0x144u,
          "worker observed real CPU affinity");
    check(nt_runtime_close_waitable(thread), "release first thread");

    /* Drop the original handle while a worker still owns the canonical
     * thread state. Open/duplicate/close/wait cannot recycle a live slot. */
    for (int i = 0; i < 48; ++i) {
        id = 0;
        thread = nt_CreateThread(NULL, 0, affinity_worker, NULL, 4, &id);
        if (!thread || !id) { check(0, "stress: create"); break; }
        alias = 0;
        if (nt_runtime_duplicate_waitable(thread, THREAD_ALL_ACCESS,
              0, 0, &alias) != NT_STATUS_SUCCESS || !alias) {
            check(0, "stress: duplicate");
            break;
        }
        check(nt_runtime_close_waitable(thread), "stress: close original");
        check(nt_ResumeThread(alias) == 1, "stress: resume via alias");
        check(nt_WaitForSingleObject(alias, 5000) == 0, "stress: join via alias");
        check(nt_GetExitCodeThread(alias, &code) && code == 0x144u,
              "stress: real host affinity");
        check(nt_runtime_close_waitable(alias), "stress: close last alias");
    }
    check(sched_setaffinity(0, sizeof(original), &original) == 0,
          "restore original host affinity");
    if (!failures)
        puts("PASS stage-4 real CPU affinity, access rights, NTSTATUS and 48 thread lifetimes");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
