/* SPDX-License-Identifier: MIT */
/* Real PE32+ AMD64, linked with -lkernel32: lstrcmpW is resolved through
 * the guest IAT; no host-native bypass or helper DLL is permitted. */
#include <windows.h>

int import_fixture_entry(void)
{
    SetLastError(0x1445);
    if (lstrcmpW(L"", L"") != 0 ||
        lstrcmpW(L"árbol", L"árbol") != 0 ||
        lstrcmpW(L"niño", L"niño") != 0 ||
        lstrcmpW(L"Grüße", L"Grüße") != 0)
        return 71;
    if (lstrcmpW(L"", L"x") >= 0 ||
        lstrcmpW(L"x", L"") <= 0 ||
        lstrcmpW(L"abc", L"abcd") >= 0 ||
        lstrcmpW(L"abcd", L"abc") <= 0)
        return 72;
    if (lstrcmpW(L"abc", L"abd") >= 0 ||
        lstrcmpW(L"abd", L"abc") <= 0 ||
        lstrcmpW(L"A", L"a") == 0)
        return 73;
    if (GetLastError() != 0x1445)
        return 74;
    return 0;
}
