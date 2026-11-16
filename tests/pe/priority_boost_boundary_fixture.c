/* SPDX-License-Identifier: MIT */
/* PE32+ ABI / IAT acceptance for fail-closed priority boost controls.
 * Native Windows is expected to support these functions; the Linux
 * runtime must not report a fictional policy while it only maps nice. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    return 0x144u;
}

int import_fixture_entry(void)
{
    BOOL disabled = (BOOL)0x5151;
    SetLastError(0);
    if (GetProcessPriorityBoost(GetCurrentProcess(), &disabled) ||
        GetLastError() != ERROR_NOT_SUPPORTED || disabled != (BOOL)0x5151)
        return 10;
    SetLastError(0);
    if (SetProcessPriorityBoost(GetCurrentProcess(), TRUE) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 11;
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    if (!thread)
        return 12;
    SetLastError(0);
    if (GetThreadPriorityBoost(thread, &disabled) ||
        GetLastError() != ERROR_NOT_SUPPORTED || disabled != (BOOL)0x5151)
        return 13;
    SetLastError(0);
    if (SetThreadPriorityBoost(thread, TRUE) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 14;
    if (ResumeThread(thread) != 1 ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 15;
    DWORD code = 0;
    if (!GetExitCodeThread(thread, &code) || code != 0x144u ||
        !CloseHandle(thread))
        return 16;
    return 0;
}
