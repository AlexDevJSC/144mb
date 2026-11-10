/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_DESKTOP_H
#define NTNATIVE_DESKTOP_H

#include "compat.h"

enum {
    NT_WM_CREATE = 0x01, NT_WM_DESTROY = 0x02, NT_WM_MOVE = 0x03, NT_WM_SIZE = 0x05,
    NT_WM_ENABLE = 0x0a, NT_WM_SETTEXT = 0x0c, NT_WM_GETTEXT = 0x0d,
    NT_WM_GETTEXTLENGTH = 0x0e, NT_WM_PAINT = 0x0f, NT_WM_CLOSE = 0x10,
    NT_WM_ERASEBKGND = 0x14, NT_WM_SHOWWINDOW = 0x18, NT_WM_CANCELMODE = 0x1f,
    NT_WM_NCCREATE = 0x81, NT_WM_NCDESTROY = 0x82, NT_WM_KEYDOWN = 0x100,
    NT_WM_KEYUP = 0x101, NT_WM_CHAR = 0x102, NT_WM_TIMER = 0x113,
    NT_WM_COMMAND = 0x111,
    NT_WM_MOUSEMOVE = 0x200, NT_WM_LBUTTONDOWN = 0x201, NT_WM_LBUTTONUP = 0x202,
    NT_WM_LBUTTONDBLCLK = 0x203, NT_WM_RBUTTONDOWN = 0x204,
    NT_WM_MBUTTONDOWN = 0x207, NT_WM_CAPTURECHANGED = 0x215
};

typedef intptr_t (NT_MS_ABI *nt_window_proc)(uintptr_t window, uint32_t message,
                                           uintptr_t wparam, intptr_t lparam);

struct nt_window_class {
    uint32_t style;
    nt_window_proc proc;
    int32_t class_extra, window_extra;
    uintptr_t instance, icon, cursor, background;
    const char *menu, *name;
};

struct nt_create_window {
    void *parameter;
    uintptr_t instance, menu, parent;
    int32_t height, width, y, x;
    int32_t style;
    const char *title, *class_name;
    uint32_t extended_style;
};

struct nt_paint {
    uintptr_t dc;
    int32_t erase;
    struct nt_rect rect;
    int32_t restore, update;
    unsigned char reserved[32];
};

bool nt_runtime_open_desktop(void);
void nt_runtime_close_desktop(void);
bool nt_runtime_desktop_active(void);
bool nt_runtime_window_valid(uintptr_t window);
void nt_runtime_remove_window_timers(uintptr_t window);
int nt_runtime_take_window_message(struct nt_message *message, uintptr_t window,
                                   uint32_t first, uint32_t last);
void nt_runtime_desktop_wait(int milliseconds);
bool nt_runtime_window_pixel(uintptr_t window, int x, int y, uint32_t *color);
uint32_t nt_runtime_window_id(uintptr_t window);
void nt_runtime_clipboard_window_closed(uintptr_t window);
void nt_runtime_close_clipboard(void);

uint16_t NT_MS_ABI nt_RegisterClassA(const struct nt_window_class *definition);
uintptr_t NT_MS_ABI nt_CreateWindowExA(uint32_t exstyle, const char *class_name, const char *title,
                                       uint32_t style, int32_t x, int32_t y, int32_t width,
                                       int32_t height, uintptr_t parent, uintptr_t menu,
                                       uintptr_t instance, void *parameter);
int32_t NT_MS_ABI nt_DestroyWindow(uintptr_t window);
intptr_t NT_MS_ABI nt_DefWindowProcA(uintptr_t window, uint32_t message,
                                    uintptr_t wparam, intptr_t lparam);
intptr_t NT_MS_ABI nt_SendMessageA(uintptr_t window, uint32_t message,
                                  uintptr_t wparam, intptr_t lparam);
int32_t NT_MS_ABI nt_GetClientRect(uintptr_t window, struct nt_rect *rect);
int32_t NT_MS_ABI nt_GetWindowRect(uintptr_t window, struct nt_rect *rect);
int32_t NT_MS_ABI nt_InvalidateRect(uintptr_t window, const struct nt_rect *rect, int32_t erase);
uintptr_t NT_MS_ABI nt_BeginPaint(uintptr_t window, struct nt_paint *paint);
int32_t NT_MS_ABI nt_EndPaint(uintptr_t window, const struct nt_paint *paint);
uintptr_t NT_MS_ABI nt_GetDC(uintptr_t window);
int32_t NT_MS_ABI nt_ReleaseDC(uintptr_t window, uintptr_t dc);
int32_t NT_MS_ABI nt_ShowWindow(uintptr_t window, int32_t show);
int32_t NT_MS_ABI nt_EnableWindow(uintptr_t window, int32_t enable);
uintptr_t NT_MS_ABI nt_SetCapture(uintptr_t window);
int32_t NT_MS_ABI nt_ReleaseCapture(void);
int32_t NT_MS_ABI nt_SetForegroundWindow(uintptr_t window);
int32_t NT_MS_ABI nt_SetWindowTextA(uintptr_t window, const char *title);
intptr_t NT_MS_ABI nt_GetWindowLongPtrA(uintptr_t window, int32_t index);
intptr_t NT_MS_ABI nt_SetWindowLongPtrA(uintptr_t window, int32_t index, intptr_t value);
int32_t NT_MS_ABI nt_SetWindowPos(uintptr_t window, uintptr_t after, int32_t x, int32_t y,
                                 int32_t width, int32_t height, uint32_t flags);
int32_t NT_MS_ABI nt_GetKeyboardState(unsigned char *keys);
int32_t NT_MS_ABI nt_TranslateMessage(const struct nt_message *message);
int32_t NT_MS_ABI nt_SystemParametersInfoA(uint32_t action, uint32_t parameter,
                                          void *result, uint32_t flags);
int32_t NT_MS_ABI nt_OpenClipboard(uintptr_t window);
int32_t NT_MS_ABI nt_CloseClipboard(void);
int32_t NT_MS_ABI nt_EmptyClipboard(void);
uintptr_t NT_MS_ABI nt_SetClipboardData(uint32_t format, uintptr_t memory);
int32_t NT_MS_ABI nt_SetMenu(uintptr_t window, uintptr_t menu);
uintptr_t NT_MS_ABI nt_GetMenu(uintptr_t window);
int32_t NT_MS_ABI nt_DrawMenuBar(uintptr_t window);
int32_t NT_MS_ABI nt_GetMenuItemRect(uintptr_t window, uintptr_t menu, uint32_t item,
                                     struct nt_rect *rect);
int32_t NT_MS_ABI nt_AdjustWindowRectEx(struct nt_rect *rect, uint32_t style, int32_t menu,
                                       uint32_t exstyle);
uint32_t NT_MS_ABI nt_GetSysColor(int32_t index);
int32_t NT_MS_ABI nt_MessageBeep(uint32_t type);

#endif
