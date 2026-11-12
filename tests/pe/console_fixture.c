/* SPDX-License-Identifier: MIT */
#define _WIN32_WINNT 0x0a00
#include <windows.h>

/* Exercises actual PE32+ IAT and Microsoft ABI. Positive terminal I/O
 * is checked separately against a Linux PTY by tests/console_test.c. */
int import_fixture_entry(void)
{
    WCHAR buffer[8] = {0};
    DWORD done = 99;
    if (ReadConsoleW(INVALID_HANDLE_VALUE, buffer, 8, &done, NULL) ||
        done != 0 || GetLastError() != ERROR_INVALID_HANDLE)
        return 11;
    if (SetConsoleTitleA(NULL) || GetLastError() != ERROR_INVALID_PARAMETER)
        return 12;
    return 0;
}
