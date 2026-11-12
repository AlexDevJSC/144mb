/* SPDX-License-Identifier: MIT */
#include <windows.h>

#ifdef NT_CHILD_FIXTURE
int import_fixture_entry(void)
{
    const WCHAR *command = GetCommandLineW();
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
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process))
        return 20;
    DWORD code = 0;
    if (WaitForSingleObject(process.hProcess, 10000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(process.hThread, 0) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &code) || code != 0x12345678)
        return 21;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (CreateProcessW(L"C:\\ChildDir", NULL, NULL, NULL, FALSE, 0, NULL, directory, &startup, &process) ||
        GetLastError() != ERROR_FILE_NOT_FOUND)
        return 22;
    return 0;
}
#endif
