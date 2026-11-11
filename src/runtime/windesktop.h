/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_WINDESKTOP_H
#define NTNATIVE_WINDESKTOP_H

#include "desktop.h"

struct nt_window_class_ex {
    uint32_t size, style;
    nt_window_proc proc;
    int32_t class_extra, window_extra;
    uintptr_t instance, icon, cursor, background;
    const uint16_t *menu, *name;
    uintptr_t small_icon;
};
struct nt_window_placement {
    uint32_t length, flags, show;
    struct nt_point minimum, maximum;
    struct nt_rect normal;
};
struct nt_monitor_info { uint32_t size; struct nt_rect bounds, work; uint32_t flags; };
struct nt_track_mouse { uint32_t size, flags; uintptr_t window; uint32_t milliseconds; };

uint16_t NT_MS_ABI nt_RegisterClassExW(const struct nt_window_class_ex *definition);
int32_t NT_MS_ABI nt_UnregisterClassW(const uint16_t *name, uintptr_t instance);
uintptr_t NT_MS_ABI nt_CreateWindowExW(uint32_t exstyle, const uint16_t *class_name, const uint16_t *title,
                                       uint32_t style, int32_t x, int32_t y, int32_t width, int32_t height,
                                       uintptr_t parent, uintptr_t menu, uintptr_t instance, void *parameter);
intptr_t NT_MS_ABI nt_DefWindowProcW(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam);
int32_t NT_MS_ABI nt_SetWindowTextW(uintptr_t window, const uint16_t *text);
int32_t NT_MS_ABI nt_IsWindowUnicode(uintptr_t window);
int32_t NT_MS_ABI nt_IsWindowVisible(uintptr_t window);
int32_t NT_MS_ABI nt_IsIconic(uintptr_t window);
int32_t NT_MS_ABI nt_GetWindowPlacement(uintptr_t window, struct nt_window_placement *placement);
int32_t NT_MS_ABI nt_SetWindowPlacement(uintptr_t window, const struct nt_window_placement *placement);
int32_t NT_MS_ABI nt_UpdateWindow(uintptr_t window);
uintptr_t NT_MS_ABI nt_GetActiveWindow(void);
uintptr_t NT_MS_ABI nt_GetForegroundWindow(void);
uintptr_t NT_MS_ABI nt_GetCapture(void);
uint32_t NT_MS_ABI nt_GetWindowThreadProcessId(uintptr_t window, uint32_t *process);
int32_t NT_MS_ABI nt_ClientToScreen(uintptr_t window, struct nt_point *point);
int32_t NT_MS_ABI nt_ScreenToClient(uintptr_t window, struct nt_point *point);
int32_t NT_MS_ABI nt_GetCursorPos(struct nt_point *point);
int32_t NT_MS_ABI nt_SetCursorPos(int32_t x, int32_t y);
uintptr_t NT_MS_ABI nt_SetCursor(uintptr_t cursor);
uintptr_t NT_MS_ABI nt_LoadCursorW(uintptr_t instance, const uint16_t *name);
uintptr_t NT_MS_ABI nt_LoadImageW(uintptr_t instance, const uint16_t *name, uint32_t type,
                                  int32_t width, int32_t height, uint32_t flags);
int32_t NT_MS_ABI nt_TrackMouseEvent(struct nt_track_mouse *track);
int16_t NT_MS_ABI nt_GetKeyState(int32_t key);
int16_t NT_MS_ABI nt_GetAsyncKeyState(int32_t key);
uintptr_t NT_MS_ABI nt_GetKeyboardLayout(uint32_t thread);
uintptr_t NT_MS_ABI nt_MonitorFromWindow(uintptr_t window, uint32_t flags);
uintptr_t NT_MS_ABI nt_MonitorFromPoint(struct nt_point point, uint32_t flags);
int32_t NT_MS_ABI nt_GetMonitorInfoW(uintptr_t monitor, struct nt_monitor_info *info);
int32_t NT_MS_ABI nt_GetSystemMetrics(int32_t index);
int32_t NT_MS_ABI nt_SetProcessDPIAware(void);
int32_t NT_MS_ABI nt_AdjustWindowRect(struct nt_rect *rect, uint32_t style, int32_t menu);
bool nt_runtime_post_window_message(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam);
int nt_runtime_peek_window_message(struct nt_message *message, uintptr_t window, uint32_t first,
                                   uint32_t last, bool remove);
int32_t NT_MS_ABI nt_GetMessageW(struct nt_message *message, uintptr_t window, uint32_t first, uint32_t last);
int32_t NT_MS_ABI nt_PeekMessageW(struct nt_message *message, uintptr_t window, uint32_t first,
                                 uint32_t last, uint32_t flags);
intptr_t NT_MS_ABI nt_DispatchMessageW(const struct nt_message *message);
int32_t NT_MS_ABI nt_PostMessageW(uintptr_t window, uint32_t message, uintptr_t wparam, intptr_t lparam);
intptr_t NT_MS_ABI nt_GetMessageExtraInfo(void);
intptr_t NT_MS_ABI nt_SetMessageExtraInfo(intptr_t info);
uintptr_t NT_MS_ABI nt_GetClipboardData(uint32_t format);

#endif
