/* SPDX-License-Identifier: MIT */
#include <windows.h>

__declspec(dllimport) BOOL WINAPI NtUserGetCursorPos(POINT *);
__declspec(dllimport) HWND WINAPI NtUserGetForegroundWindow(void);
__declspec(dllimport) SHORT WINAPI NtUserGetKeyState(INT);

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

int import_fixture_entry(void)
{
    const WNDCLASSA definition = {.lpfnWndProc = window_proc, .lpszClassName = "Shared.User.Contract"};
    if (!RegisterClassA(&definition))
        return 10;
    HWND window = CreateWindowExA(0, definition.lpszClassName, "PE desktop", WS_OVERLAPPEDWINDOW,
                                  0, 0, 200, 120, NULL, NULL, NULL, NULL);
    RECT client;
    if (!window || !GetClientRect(window, &client) || client.right <= 50 || client.bottom <= 50)
        return 11;
    RECT subregion = {10, 15, 50, 50}, overlap;
    if (!IntersectRect(&overlap, &client, &subregion) || overlap.left != 10 || overlap.top != 15 ||
        overlap.right != 50 || overlap.bottom != 50)
        return 12;
    POINT win32, native;
    if (!GetCursorPos(&win32) || !NtUserGetCursorPos(&native) || win32.x != native.x || win32.y != native.y ||
        GetKeyState(VK_SHIFT) != NtUserGetKeyState(VK_SHIFT) || GetForegroundWindow() != NtUserGetForegroundWindow())
        return 13;

    HMODULE alias = LoadLibraryW(L"Api-Ms-Win-NtUser-Rectangle-L1-1-0");
    HMODULE user = GetModuleHandleW(L"USER32.dll");
    HMODULE system = LoadLibraryExW(L"C:\\Windows\\System32\\kernelbase.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE core_alias = LoadLibraryA("api-ms-win-core-handle-l1-1-0.dll");
    if (!alias || alias != user || !GetProcAddress(alias, "IntersectRect") ||
        !system || system != core_alias || !GetProcAddress(system, "CloseHandle"))
        return 14;
    if (GetProcAddress(system, "UnimplementedFixtureSymbol") || GetLastError() != ERROR_PROC_NOT_FOUND ||
        LoadLibraryA("api-ms-win-core-not-a-real-contract-l1-1-0.dll") || GetLastError() != ERROR_MOD_NOT_FOUND ||
        LoadLibraryW(L"C:\\Elsewhere\\kernelbase.dll") || GetLastError() != ERROR_MOD_NOT_FOUND)
        return 15;
    return DestroyWindow(window) && FreeLibrary(alias) && FreeLibrary(core_alias) && FreeLibrary(system) ? 0 : 16;
}
