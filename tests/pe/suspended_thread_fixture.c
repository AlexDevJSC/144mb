/* SPDX-License-Identifier: MIT */
/* Build-generated Win64 PE: CREATE_SUSPENDED must block guest entry. */
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) LONG NTAPI NtResumeThread(HANDLE, ULONG *);
static volatile DWORD entered;

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    entered = 1;
    return 0x144;
}

int import_fixture_entry(void)
{
    DWORD thread_id = 0, exit_code = 0;
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, &thread_id);
    if (thread == NULL || thread_id == 0)
        return 10;
    if (entered != 0 || WaitForSingleObject(thread, 0) != WAIT_TIMEOUT)
        return 11;
    ULONG previous = 999;
    if (NtResumeThread(thread, &previous) != 0 || previous != 1)
        return 12;
    if (ResumeThread(thread) != 0)
        return 13;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || entered != 1 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144)
        return 14;
    if (!CloseHandle(thread))
        return 15;
    SetLastError(0);
    if (ResumeThread(NULL) != 0xffffffffu || GetLastError() != ERROR_INVALID_HANDLE)
        return 16;
    if (CreateThread(NULL, 0, worker, NULL, 0x10000u, NULL) != NULL ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 17;
    return 0;
}
