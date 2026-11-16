/* SPDX-License-Identifier: MIT */
/* Genuine freestanding Win64 PE: process class maps to host Linux nice,
 * and ThreadPriority remains relative to the chosen process class. */
#include <windows.h>
#include <stdint.h>

typedef struct {
    LONG status;
    void *peb;
    uintptr_t affinity;
    LONG base_priority;
    uintptr_t pid, parent;
} PROCESS_BASIC;

__declspec(dllimport) LONG NTAPI NtQueryInformationProcess(HANDLE, ULONG, void *, ULONG, ULONG *);

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    if (GetPriorityClass(GetCurrentProcess()) != IDLE_PRIORITY_CLASS ||
        GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_BELOW_NORMAL)
        return 0x1441u;
    return 0x144u;
}

int import_fixture_entry(void)
{
    if (GetPriorityClass(GetCurrentProcess()) != NORMAL_PRIORITY_CLASS)
        return 10;
    HANDLE query_only = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    GetCurrentProcessId());
    HANDLE set_only = OpenProcess(PROCESS_SET_INFORMATION, FALSE,
                                  GetCurrentProcessId());
    if (!query_only || !set_only)
        return 11;
    SetLastError(0);
    if (SetPriorityClass(query_only, BELOW_NORMAL_PRIORITY_CLASS) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 12;
    SetLastError(0);
    if (GetPriorityClass(set_only) || GetLastError() != ERROR_ACCESS_DENIED)
        return 13;
    SetLastError(0);
    if (SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 14;
    SetLastError(0x1444u);
    if (!SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) ||
        GetPriorityClass(GetCurrentProcess()) != BELOW_NORMAL_PRIORITY_CLASS ||
        GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_NORMAL ||
        GetLastError() != 0x1444u)
        return 15;
    PROCESS_BASIC basic;
    ULONG length = 0;
    SetLastError(0x1445u);
    LONG result = NtQueryInformationProcess(GetCurrentProcess(), 0, &basic, sizeof(basic), &length);
    /* A host with CPU IDs outside the legacy group-0 range may reject
     * ProcessBasicInformation; never treat that as a false scheduling
     * result. We can still verify Win32 priority and rights. */
    if ((result == 0 && (basic.base_priority != 6 || length != sizeof(basic))) ||
        (result != 0 && result != (LONG)0xc00000bbu) ||
        GetLastError() != 0x1445u)
        return 16;
    if (!SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS) ||
        GetPriorityClass(GetCurrentProcess()) != IDLE_PRIORITY_CLASS ||
        GetThreadPriority(GetCurrentThread()) != THREAD_PRIORITY_NORMAL)
        return 17;
    SetLastError(0x1446u);
    result = NtQueryInformationProcess(GetCurrentProcess(), 0, &basic, sizeof(basic), &length);
    if ((result == 0 && basic.base_priority != 4) ||
        (result != 0 && result != (LONG)0xc00000bbu) ||
        GetLastError() != 0x1446u)
        return 18;

    HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    if (!thread)
        return 19;
    SetLastError(0);
    if (SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 20;
    if (GetThreadPriority(thread) != THREAD_PRIORITY_NORMAL ||
        !SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL) ||
        GetThreadPriority(thread) != THREAD_PRIORITY_BELOW_NORMAL ||
        ResumeThread(thread) != 1 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 21;
    DWORD exit_code = 0;
    if (!GetExitCodeThread(thread, &exit_code) || exit_code != 0x144u)
        return 22;
    if (!CloseHandle(thread) || !CloseHandle(query_only) || !CloseHandle(set_only))
        return 23;
    return 0;
}
