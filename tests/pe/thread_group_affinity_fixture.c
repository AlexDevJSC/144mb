/* SPDX-License-Identifier: MIT */
/* Freestanding Windows x64 PE: test the exported group-affinity ABI
 * through the actual Linux-backed scheduler on a controlled guest thread. */
#include <windows.h>
#include <stdint.h>

static GROUP_AFFINITY chosen;

static DWORD WINAPI test_worker(void *unused)
{
    (void)unused;
    GROUP_AFFINITY observed;
    if (!GetThreadGroupAffinity(GetCurrentThread(), &observed) ||
        observed.Group != chosen.Group || observed.Mask != chosen.Mask ||
        observed.Reserved[0] || observed.Reserved[1] || observed.Reserved[2])
        return 0x182u;
    return 0x144u;
}

int import_fixture_entry(void)
{
    const WORD active_groups = GetActiveProcessorGroupCount();
    const DWORD total = GetActiveProcessorCount((WORD)0xffffu);
    if (active_groups == 0 || total == 0)
        return 25;
    DWORD sum = 0;
    for (WORD group = 0; group < active_groups; ++group)
        sum += GetActiveProcessorCount(group);
    if (sum != total || GetActiveProcessorCount(active_groups) != 0)
        return 26;
    HANDLE thread = CreateThread(NULL, 0, test_worker, NULL, CREATE_SUSPENDED, NULL);
    if (thread == NULL)
        return 10;

    /* Try high Windows groups first; Linux cpuset restrictions may allow
     * e.g. CPU 64+ but no CPU below 64. Do not assume CPU0 is available.
     * Only the fixture-owned thread is modified. */
    BOOL found = FALSE;
    for (int group = 15; group >= 0 && !found; --group) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            GROUP_AFFINITY request = {
                .Mask = (KAFFINITY)(UINT64_C(1) << bit), .Group = (WORD)group
            };
            if (SetThreadGroupAffinity(thread, &request, NULL)) {
                chosen = request;
                found = TRUE;
                break;
            }
        }
    }
    if (!found)
        return 11;
    GROUP_AFFINITY observed;
    SetLastError(0x5151u);
    if (!GetThreadGroupAffinity(thread, &observed) ||
        observed.Group != chosen.Group || observed.Mask != chosen.Mask ||
        GetLastError() != 0x5151u)
        return 12;

    GROUP_AFFINITY previous;
    if (!SetThreadGroupAffinity(thread, &chosen, &previous) ||
        previous.Group != chosen.Group || previous.Mask != chosen.Mask)
        return 13;

    const DWORD tid = GetThreadId(thread);
    HANDLE query_only = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    HANDLE set_only = OpenThread(THREAD_SET_INFORMATION, FALSE, tid);
    if (query_only == NULL || set_only == NULL)
        return 14;
    SetLastError(0);
    if (SetThreadGroupAffinity(query_only, &chosen, NULL) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 15;
    SetLastError(0);
    if (GetThreadGroupAffinity(set_only, &observed) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 16;
    if (!GetThreadGroupAffinity(query_only, &observed) ||
        observed.Group != chosen.Group || observed.Mask != chosen.Mask)
        return 17;

    GROUP_AFFINITY invalid = chosen;
    invalid.Reserved[0] = 1;
    SetLastError(0);
    if (SetThreadGroupAffinity(thread, &invalid, NULL) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 18;
    SetLastError(0);
    if (SetThreadGroupAffinity(thread, NULL, NULL) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 19;
    SetLastError(0);
    if (GetThreadGroupAffinity(thread, NULL) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 20;
    if (!GetThreadGroupAffinity(thread, &observed) ||
        observed.Mask != chosen.Mask || observed.Group != chosen.Group)
        return 21;

    if (chosen.Group != 0) {
        SetLastError(0);
        if (SetThreadAffinityMask(thread, chosen.Mask) != 0 ||
            GetLastError() != ERROR_NOT_SUPPORTED)
            return 22; /* Legacy group-0 API must not truncate. */
    }

    if (GetActiveProcessorGroupCount() != active_groups ||
        GetActiveProcessorCount((WORD)0xffffu) != total)
        return 27; /* Pinning a thread must not reduce the topology. */
    if (ResumeThread(thread) != 1 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 23;
    DWORD code = 0;
    if (!GetExitCodeThread(thread, &code) || code != 0x144u ||
        !CloseHandle(query_only) || !CloseHandle(set_only) ||
        !CloseHandle(thread))
        return 24;
    return 0;
}
