/* SPDX-License-Identifier: MIT */
/* Freestanding PE32+ AMD64: observable process affinity semantics.
 * Only mutates this guest's current Linux process. */
#include <windows.h>
#include <stdint.h>

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    DWORD_PTR process = 0, system = 0;
    return GetProcessAffinityMask(GetCurrentProcess(), &process, &system) &&
           process != 0 && (system & process) == process ? 0x144u : 0x1441u;
}

int import_fixture_entry(void)
{
    DWORD_PTR original = 0, system = 0;
    SetLastError(0x5151u);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &original, &system)) {
        if (GetLastError() != ERROR_NOT_SUPPORTED)
            return 10;
        SetLastError(0);
        if (SetProcessAffinityMask(GetCurrentProcess(), 1) ||
            GetLastError() != ERROR_NOT_SUPPORTED)
            return 11;
        return 0; /* Bounded legacy Win64 group-0 API is not representable. */
    }
    if (original == 0 || system == 0 ||
        (original & system) != original || GetLastError() != 0x5151u)
        return 12;

    const DWORD_PTR target = original & (~original + 1);
    HANDLE query_only = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                    GetCurrentProcessId());
    HANDLE set_only = OpenProcess(PROCESS_SET_INFORMATION, FALSE,
                                  GetCurrentProcessId());
    if (!query_only || !set_only)
        return 13;
    SetLastError(0);
    if (SetProcessAffinityMask(query_only, target) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 14;
    SetLastError(0);
    if (GetProcessAffinityMask(set_only, &original, &system) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 15;
    SetLastError(0);
    if (SetProcessAffinityMask(GetCurrentProcess(), 0) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 16;

    DWORD_PTR before = 0, available = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &before, &available))
        return 17;
    SetLastError(0x5152u);
    if (!SetProcessAffinityMask(set_only, target) || GetLastError() != 0x5152u)
        return 18;
    DWORD_PTR after = 0, available_after = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &after, &available_after) ||
        after != target || available_after != available)
        return 19;

    HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    if (!thread)
        return 20;
    SetLastError(0);
    if (SetProcessAffinityMask(GetCurrentProcess(), before) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 21;
    if (ResumeThread(thread) != 1 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 22;
    DWORD code = 0;
    if (!GetExitCodeThread(thread, &code) || code != 0x144u)
        return 23;
    if (!CloseHandle(thread) || !CloseHandle(query_only) ||
        !CloseHandle(set_only))
        return 24;
    return 0;
}
