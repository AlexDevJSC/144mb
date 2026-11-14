/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <stdint.h>

#ifndef NT_CHILD_FIXTURE
__declspec(dllimport) LONG NTAPI NtTerminateProcess(HANDLE, LONG);
__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
#endif

#ifdef NT_CHILD_FIXTURE
static BOOL contains_command_marker(const WCHAR *command, const WCHAR *marker)
{
    for (const WCHAR *p = command; *p != 0; ++p) {
        const WCHAR *a = p, *b = marker;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0)
            return TRUE;
    }
    return FALSE;
}
static BOOL has_hold_argument(const WCHAR *command)
{
    static const WCHAR marker[] = L"/nt-hold";
    for (const WCHAR *p = command; *p != 0; ++p) {
        const WCHAR *a = p, *b = marker;
        while (*a && *b && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == 0)
            return TRUE;
    }
    return FALSE;
}

int import_fixture_entry(void)
{
    const WCHAR *command = GetCommandLineW();
    if (has_hold_argument(command)) {
        Sleep(30000);
        return 0x766u;
    }
    WCHAR env[80];
    if (contains_command_marker(command, L"/nt-env-custom")) {
        if (GetEnvironmentVariableW(L"NT_CHILD_CUSTOM", env, 80) != 5 ||
            lstrcmpW(env, L"árbol") != 0 ||
            GetEnvironmentVariableW(L"NT_CHILD_INHERITED", env, 80) != 0 ||
            GetLastError() != ERROR_ENVVAR_NOT_FOUND)
            return 96;
    } else {
        if (GetEnvironmentVariableW(L"NT_CHILD_INHERITED", env, 80) != 5 ||
            lstrcmpW(env, L"Grüße") != 0)
            return 97;
    }
    BOOL found = FALSE;
    for (const WCHAR *p = command; *p; ++p)
        if (*p == 0xe4)
            found = TRUE;
    WCHAR directory[32];
    if (!found || GetCurrentDirectoryW(32, directory) != 11 ||
        directory[0] != L'C' || directory[3] != L'C')
        return 91;
    return 0x12345678;
}
#else
int import_fixture_entry(void)
{
    WCHAR command[] = L"\"C:\\Test child.exe\" \x00e4";
    const WCHAR directory[] = L"C:\\ChildDir";
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    if (!SetEnvironmentVariableW(L"NT_CHILD_INHERITED", L"Grüße"))
        return 98;
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 20;
    DWORD code = 0;
    if (GetProcessId(process.hProcess) != process.dwProcessId ||
        GetProcessIdOfThread(process.hThread) != process.dwProcessId ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678)
        return 21;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (CreateProcessW(L"C:\\ChildDir", NULL, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process) ||
        GetLastError() != ERROR_FILE_NOT_FOUND)
        return 22;

    /* The child receives a private startup gate: not even its PE entry
     * executes until the initial thread's suspension count reaches zero. */
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, directory, &startup, &process))
        return 31;
    if (WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT ||
        GetProcessIdOfThread(process.hThread) != process.dwProcessId)
        return 32;
    ULONG prior = 0;
    if (NtSuspendThread(process.hThread, &prior) != 0 || prior != 1 ||
        ResumeThread(process.hThread) != 2 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_TIMEOUT ||
        ResumeThread(process.hThread) != 1 ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u)
        return 33;
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 34;

    WCHAR hold[] = L"\"C:\\Test child.exe\" /nt-hold";
    if (!CreateProcessW(NULL, hold, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 23;
    if (GetProcessId(process.hProcess) != process.dwProcessId ||
        GetProcessIdOfThread(process.hThread) != process.dwProcessId ||
        GetProcessId(GetCurrentProcess()) != GetCurrentProcessId())
        return 24;
    SetLastError(0x770u);
    if (NtTerminateProcess(process.hProcess, (LONG)0x12345678u) != 0 ||
        GetLastError() != 0x770u ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678)
        return 25;
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 26;

    if (!CreateProcessW(NULL, hold, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 27;
    if (!TerminateProcess(process.hProcess, 0x98765432u) ||
        WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x98765432u)
        return 28;
    if (!CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 29;

    /* The custom environment replaces, rather than extends, the inherited
     * guest snapshot. The Linux runner's host envp remains untouched. */
    WCHAR custom_command[] = L"\"C:\\Test child.exe\" /nt-env-custom ä";
    static const WCHAR environment[] =
        L"NT_CHILD_CUSTOM=árbol\0NT_CHILD_OTHER=ok\0";
    if (!CreateProcessW(NULL, custom_command, NULL, NULL, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, (void *)environment,
                        directory, &startup, &process))
        return 99;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678u ||
        !CloseHandle(process.hThread) || !CloseHandle(process.hProcess))
        return 100;
    SetLastError(0);
    if (CreateProcessW(NULL, custom_command, NULL, NULL, FALSE, 0,
                       (void *)environment, directory, &startup, &process) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 101;
    if (!SetEnvironmentVariableW(L"NT_CHILD_INHERITED", NULL))
        return 102;
    return 0;
}
#endif
