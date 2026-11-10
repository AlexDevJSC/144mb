/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
    WNDCLASSA definition = {0};
    definition.lpfnWndProc = procedure;
    definition.lpszClassName = "NTNativeDesktopFixture";
    if (RegisterClassA(&definition) == 0)
        return 1;
    HWND window = CreateWindowExA(0, definition.lpszClassName, "NT Native PE fixture",
                                  WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 160, 120,
                                  NULL, NULL, NULL, NULL);
    if (window == NULL)
        return 2;
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
