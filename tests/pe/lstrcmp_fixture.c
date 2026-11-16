/* SPDX-License-Identifier: MIT */
/* Real MinGW-linked PE32+ import test, executed through the native loader.
 * This fixture verifies both IAT resolution and the Windows x64 calling
 * convention without relying on direct native C calls. */
#include <windows.h>

int import_fixture_entry(void)
{
    if (lstrcmpW(L"árbol", L"árbol") != 0)
        return 11;
    if (lstrcmpW(L"niño", L"niño") != 0 ||
        lstrcmpW(L"Grüße", L"Grüße") != 0)
        return 12;
    if (lstrcmpW(L"", L"") != 0 ||
        lstrcmpW(L"abc", L"abcd") >= 0 ||
        lstrcmpW(L"abcd", L"abc") <= 0)
        return 13;
    if (lstrcmpW(L"cat", L"bat") <= 0 ||
        lstrcmpW(L"bat", L"cat") >= 0)
        return 14;
    if (lstrcmpA("Caf\xE9", "Caf\xE9") != 0 ||
        lstrcmpA("hello", "helloa") >= 0 ||
        lstrcmpA("helloa", "hello") <= 0)
        return 15;
    return 0;
}
