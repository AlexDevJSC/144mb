/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <cderr.h>
#include <stddef.h>

_Static_assert(offsetof(OPENFILENAMEA, pvReserved) == 136 && sizeof(OPENFILENAMEA) == 152,
               "AMD64 OPENFILENAMEA layout");
_Static_assert(offsetof(DISPATCHER_CONTEXT, ScopeIndex) == 72 && sizeof(DISPATCHER_CONTEXT) == 80 &&
               sizeof(EXCEPTION_RECORD) == 152, "AMD64 SEH record layouts");

static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(window, &paint);
        if (dc == NULL)
            return -1;
        const COLORREF color = RGB(32, 90, 210);
        const BOOL drawn = SetPixel(dc, 8, 8, color) == color;
        return EndPaint(window, &paint) && drawn ? 73 : -1;
    }
    case WM_TIMER:
        if (wparam != 2 || !KillTimer(window, wparam))
            return -1;
        SendMessageA(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(17);
        return 0;
    default:
        return DefWindowProcA(window, message, wparam, lparam);
    }
}

int import_fixture_entry(void)
{
    OPENFILENAMEA request = {.lStructSize = 1};
    if (GetOpenFileNameA(&request) || CommDlgExtendedError() != CDERR_STRUCTSIZE)
        return 6;
    WNDCLASSA definition = {0};
    definition.lpfnWndProc = procedure;
    definition.lpszClassName = "NTNativeDesktopFixture";
    definition.hIcon = LoadIconA(NULL, IDI_APPLICATION);
    if (definition.hIcon == NULL)
        return 7;
    if (RegisterClassA(&definition) == 0)
        return 1;
    HWND window = CreateWindowExA(0, definition.lpszClassName, "NT Native PE fixture",
                                  WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 160, 120,
                                  NULL, NULL, NULL, NULL);
    if (window == NULL)
        return 2;
    InitCommonControls();
    if (CreateWindowExA(0, STATUSCLASSNAMEA, "Ready", WS_CHILD | WS_VISIBLE,
                        0, 0, 0, 0, window, (HMENU)99, NULL, NULL) == NULL ||
        CreateWindowExA(0, "BUTTON", "Mark", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                        20, 30, 84, 20, window, (HMENU)23, NULL, NULL) == NULL ||
        !CheckDlgButton(window, 23, BST_CHECKED) || IsDlgButtonChecked(window, 23) != BST_CHECKED ||
        SendDlgItemMessageA(window, 23, BM_GETCHECK, 0, 0) != BST_CHECKED)
        return 8;
    char label[3];
    if (GetDlgItemTextA(window, 23, label, sizeof(label)) != 2 ||
        label[0] != 'M' || label[1] != 'a' || label[2] != 0)
        return 9;
    ShowWindow(window, SW_SHOW);
    MSG message;
    if (GetMessageA(&message, window, WM_PAINT, WM_PAINT) != 1 ||
        DispatchMessageA(&message) != 73)
        return 3;
    if (SetTimer(window, 2, 10, NULL) != 2 ||
        GetMessageA(&message, window, WM_TIMER, WM_TIMER) != 1 ||
        DispatchMessageA(&message) != 0)
        return 4;
    return GetMessageA(&message, NULL, 0, 0) == 0 && message.wParam == 17 ? 0 : 5;
}
