/* SPDX-License-Identifier: MIT */
/* Genuine Windows scheduler observation, not a Windows-emulation fixture.
 * Compile and RUN on Windows 11 / Server 2022+ with MSVC. Never changes
 * affinities or priorities outside this short-lived test process. */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <stdint.h>

static DWORD WINAPI probe_worker(void *unused)
{
    (void)unused;
    Sleep(2);
    return 0x144u;
}

static int fail(const char *which, DWORD error)
{
    printf("%s=FAIL\n%s_ERROR=%lu\n", which, which, (unsigned long)error);
    return 1;
}

int main(void)
{
    const WORD groups = GetActiveProcessorGroupCount();
    const DWORD total = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD sum = 0;
    WORD group;
    printf("SCHEMA=ntnative.stage4.scheduler.v1\n");
    printf("PLATFORM=WINDOWS_NATIVE\n");
    printf("GROUPS=%u\nACTIVE_CPUS=%lu\n", (unsigned)groups, (unsigned long)total);
    for (group = 0; group < groups; ++group)
        sum += GetActiveProcessorCount(group);
    printf("TOPOLOGY=%s\n", groups && sum == total ? "PASS" : "FAIL");

    GROUP_AFFINITY primary = {0};
    BOOL primary_ok = GetThreadGroupAffinity(GetCurrentThread(), &primary);
    printf("PRIMARY_GROUP_QUERY=%s\n", primary_ok ? "PASS" : "FAIL");
    if (primary_ok)
        printf("PRIMARY_GROUP=%u\n", (unsigned)primary.Group);

    DWORD_PTR process_mask = 0, system_mask = 0;
    SetLastError(0);
    const BOOL process_ok = GetProcessAffinityMask(GetCurrentProcess(),
                                                  &process_mask, &system_mask);
    printf("PROCESS_AFFINITY_QUERY=%s\n", process_ok ? "PASS" : "FAIL");
    printf("PROCESS_AFFINITY_ERROR=%lu\n", (unsigned long)(process_ok ? 0 : GetLastError()));

    WORD process_count = 256;
    USHORT process_groups[256] = {0};
    BOOL process_groups_ok = GetProcessGroupAffinity(GetCurrentProcess(),
                                                      &process_count, process_groups);
    printf("PROCESS_GROUP_QUERY=%s\n", process_groups_ok ? "PASS" : "FAIL");
    if (process_groups_ok)
        printf("PROCESS_GROUPS=%u\n", (unsigned)process_count);

    BOOL process_boost_disabled = FALSE;
    BOOL process_boost_ok = GetProcessPriorityBoost(GetCurrentProcess(),
                                                    &process_boost_disabled);
    if (process_boost_ok) {
        const BOOL flipped = !process_boost_disabled;
        process_boost_ok = SetProcessPriorityBoost(GetCurrentProcess(), flipped);
        if (process_boost_ok) {
            BOOL readback = FALSE;
            process_boost_ok = GetProcessPriorityBoost(GetCurrentProcess(),
                                                      &readback) && readback == flipped;
            if (!SetProcessPriorityBoost(GetCurrentProcess(), process_boost_disabled))
                process_boost_ok = FALSE;
        }
    }
    printf("PROCESS_BOOST_TOGGLE=%s\n", process_boost_ok ? "PASS" : "FAIL");
    printf("PROCESS_PRIORITY_CLASS=%lu\n",
           (unsigned long)GetPriorityClass(GetCurrentProcess()));

    DWORD tid = 0;
    HANDLE thread = CreateThread(NULL, 0, probe_worker, NULL, CREATE_SUSPENDED, &tid);
    if (!thread)
        return fail("CREATE_THREAD", GetLastError());
    printf("CREATE_THREAD=PASS\n");
    GROUP_AFFINITY before = {0};
    BOOL before_ok = GetThreadGroupAffinity(thread, &before);
    printf("THREAD_PRIMARY_QUERY=%s\n", before_ok ? "PASS" : "FAIL");

    BOOL thread_boost_disabled = FALSE;
    BOOL thread_boost_ok = GetThreadPriorityBoost(thread, &thread_boost_disabled);
    if (thread_boost_ok) {
        BOOL flipped = !thread_boost_disabled;
        thread_boost_ok = SetThreadPriorityBoost(thread, flipped);
        if (thread_boost_ok) {
            BOOL actual = FALSE;
            thread_boost_ok = GetThreadPriorityBoost(thread, &actual) && actual == flipped;
            if (!SetThreadPriorityBoost(thread, thread_boost_disabled))
                thread_boost_ok = FALSE;
        }
    }
    printf("THREAD_BOOST_TOGGLE=%s\n", thread_boost_ok ? "PASS" : "FAIL");

    /* Windows-native reference independent of NT144's fixture assumption
     * that the process initially has NORMAL_PRIORITY_CLASS. Test on our
     * own suspended worker; restore before the first instruction runs. */
    const int original_thread_priority = GetThreadPriority(thread);
    BOOL thread_priority_roundtrip = FALSE;
    if (original_thread_priority != THREAD_PRIORITY_ERROR_RETURN) {
        const int requested_thread_priority =
            original_thread_priority == THREAD_PRIORITY_BELOW_NORMAL ?
            THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL;
        if (SetThreadPriority(thread, requested_thread_priority)) {
            const BOOL changed =
                GetThreadPriority(thread) == requested_thread_priority;
            const BOOL restored =
                SetThreadPriority(thread, original_thread_priority);
            thread_priority_roundtrip = changed && restored &&
                GetThreadPriority(thread) == original_thread_priority;
        }
    }
    printf("THREAD_PRIORITY_ROUNDTRIP=%s\n",
           thread_priority_roundtrip ? "PASS" : "FAIL");

    int cross_ok = 0;
    int legacy_ok = 0;
    WORD chosen_group = before_ok ? before.Group : 0;
    GROUP_AFFINITY selected = {0};
    /* An explicitly pinned test-only suspended thread can safely probe
     * all group-local bits, without assuming that CPU 0 is online. */
    if (before_ok && groups) {
        WORD start = groups > 1 ? (WORD)((before.Group + 1u) % groups) : before.Group;
        for (WORD attempt = 0; attempt < groups && !selected.Mask; ++attempt) {
            WORD target_group = (WORD)((start + attempt) % groups);
            if (GetActiveProcessorCount(target_group) == 0)
                continue;
            for (unsigned bit = 0; bit < 64; ++bit) {
                GROUP_AFFINITY request = {0};
                request.Group = target_group;
                request.Mask = (KAFFINITY)(UINT64_C(1) << bit);
                if (SetThreadGroupAffinity(thread, &request, NULL)) {
                    selected = request;
                    chosen_group = target_group;
                    break;
                }
            }
        }
    }
    if (selected.Mask) {
        GROUP_AFFINITY observed = {0};
        KAFFINITY old = SetThreadAffinityMask(thread, selected.Mask);
        legacy_ok = old != 0 && GetThreadGroupAffinity(thread, &observed) &&
                    observed.Group == selected.Group && observed.Mask == selected.Mask;
        cross_ok = groups > 1 && chosen_group != before.Group &&
                   legacy_ok;
    }
    printf("THREAD_GROUP=%s\n", selected.Mask && legacy_ok ? "PASS" : "FAIL");
    printf("LEGACY_MASK_PRIMARY_GROUP=%s\n", legacy_ok ? "PASS" : "FAIL");
    printf("CROSS_GROUP=%s\n", groups <= 1 ? "SKIP" : cross_ok ? "PASS" : "FAIL");
    /* The same PE fixture historically demanded ERROR_INVALID_PARAMETER
     * for NULL GROUP_AFFINITY. Win32 docs do not promise that particular
     * LastError for a bad pointer. Record the observed native result. */
    SetLastError(0);
    const BOOL null_set_result = SetThreadGroupAffinity(thread, NULL, NULL);
    const DWORD null_set_error = GetLastError();
    printf("NULL_SET_GROUP_SUCCESS=%s\n", null_set_result ? "YES" : "NO");
    printf("NULL_SET_GROUP_ERROR=%lu\n", (unsigned long)null_set_error);
    SetLastError(0);
    const BOOL null_get_result = GetThreadGroupAffinity(thread, NULL);
    const DWORD null_get_error = GetLastError();
    printf("NULL_GET_GROUP_SUCCESS=%s\n", null_get_result ? "YES" : "NO");
    printf("NULL_GET_GROUP_ERROR=%lu\n", (unsigned long)null_get_error);

    DWORD_PTR foreign_tid_as_handle = (DWORD_PTR)tid;
    /* This is NOT a broker test and deliberately makes no Win32 operation
     * that would interpret a raw Linux TID/PID as an authorization. */
    (void)foreign_tid_as_handle;
    printf("EXTERNAL_THREADS=NOT_TESTED\n");

    DWORD status = 0;
    BOOL completed = ResumeThread(thread) == 1 &&
                     WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 &&
                     GetExitCodeThread(thread, &status) && status == 0x144u;
    ULONG64 cycles = 0;
    BOOL cycles_ok = QueryThreadCycleTime(thread, &cycles);
    FILETIME created, exited, kernel, user;
    BOOL times_ok = GetThreadTimes(thread, &created, &exited, &kernel, &user);
    printf("THREAD_COMPLETION=%s\n", completed ? "PASS" : "FAIL");
    printf("CYCLE_COUNTER=%s\n", cycles_ok ? "PASS" : "FAIL");
    printf("THREAD_CPU_TIME=%s\n", times_ok ? "PASS" : "FAIL");
    /* CPU time and cycle counters do not expose the NT scheduler quantum. */
    printf("QUANTUM=UNVERIFIED\n");
    printf("DYNAMIC_BOOST=%s\n", thread_boost_ok && process_boost_ok ?
           "CONTROL_AVAILABLE" : "CONTROL_FAILED");
    CloseHandle(thread);

    /* Reversible priority class transaction on THIS disposable probe
     * process. Never request REALTIME, privileged Linux nice, or touch a
     * foreign PID. Record failure rather than guessing host capabilities. */
    const DWORD original_class = GetPriorityClass(GetCurrentProcess());
    const DWORD requested_class =
        original_class == BELOW_NORMAL_PRIORITY_CLASS ?
        NORMAL_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS;
    BOOL process_class_roundtrip = FALSE;
    if (original_class != 0 && SetPriorityClass(GetCurrentProcess(), requested_class)) {
        const BOOL changed = GetPriorityClass(GetCurrentProcess()) == requested_class;
        const BOOL restored = SetPriorityClass(GetCurrentProcess(), original_class);
        process_class_roundtrip = changed && restored &&
            GetPriorityClass(GetCurrentProcess()) == original_class;
    }
    printf("PROCESS_PRIORITY_CLASS_ROUNDTRIP=%s\n",
           process_class_roundtrip ? "PASS" : "FAIL");

    /* Test a real mask narrowing and verified restoration. The legacy
     * process mask API is ambiguous for Windows 11 multi-group policy;
     * never treat lack of a representable group as an observed pass. */
    const char *affinity_roundtrip = "SKIP";
    if (groups == 1 && process_ok && process_mask != 0) {
        const DWORD_PTR single_cpu = process_mask & (~process_mask + 1);
        BOOL changed = FALSE, restored = FALSE;
        if (SetProcessAffinityMask(GetCurrentProcess(), single_cpu)) {
            DWORD_PTR observed = 0, available = 0;
            changed = GetProcessAffinityMask(GetCurrentProcess(), &observed,
                                             &available) &&
                      observed == single_cpu;
            restored = SetProcessAffinityMask(GetCurrentProcess(), process_mask);
            if (restored) {
                observed = available = 0;
                restored = GetProcessAffinityMask(GetCurrentProcess(), &observed,
                                                   &available) &&
                           observed == process_mask;
            }
        }
        affinity_roundtrip = changed && restored ? "PASS" : "FAIL";
    }
    printf("PROCESS_AFFINITY_ROUNDTRIP=%s\n", affinity_roundtrip);
    printf("NT_SCHEDULER_POLICY_PARITY=UNVERIFIED\n");
    return !groups || !total || sum != total || !before_ok || !selected.Mask ||
           !legacy_ok || !completed || !thread_boost_ok || !process_boost_ok ||
           (groups > 1 && !cross_ok);
}
