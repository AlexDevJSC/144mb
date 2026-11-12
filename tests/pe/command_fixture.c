/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <shellapi.h>

static BOOL equal(const WCHAR *left, const WCHAR *right)
{
    while (*left && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

int import_fixture_entry(void)
{
    const WCHAR *expected[] = {L"C:\\command-fixture.exe", L"with space", L"", L"a\\\"b", L"trail\\", L"caf\u00e9", L"--game-option"};
    int count = 0;
    WCHAR **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments || count != (int)(sizeof(expected) / sizeof(expected[0])))
        return 1;
    for (int i = 0; i < count; ++i)
        if (!equal(arguments[i], expected[i]))
            return i + 2;
    return LocalFree(arguments) != NULL;
}
