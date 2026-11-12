/* SPDX-License-Identifier: MIT */
/* A hard EXE import must load and attach its local dependent PE DLL first. */
#include <windows.h>
__declspec(dllimport) int WINAPI GuestAnswer(int value);
int import_fixture_entry(void)
{
    if (GuestAnswer(2) != 44)
        return 10;
    HMODULE module = GetModuleHandleA("guest_companion.dll");
    if (!module)
        return 11;
    FARPROC proc = GetProcAddress(module, "GuestAnswer");
    if (!proc)
        return 12;
    return 0;
}
