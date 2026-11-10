/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_RUNTIME_COMPAT_H
#define NTNATIVE_RUNTIME_COMPAT_H

#include "ntnative/runtime.h"

#include <stddef.h>
#include <stdint.h>

enum {
    NT_ERROR_SUCCESS = 0,
    NT_ERROR_FILE_NOT_FOUND = 2,
    NT_ERROR_PATH_NOT_FOUND = 3,
    NT_ERROR_ACCESS_DENIED = 5,
    NT_ERROR_INVALID_HANDLE = 6,
    NT_ERROR_NOT_ENOUGH_MEMORY = 8,
    NT_ERROR_GEN_FAILURE = 31,
    NT_ERROR_NOT_SUPPORTED = 50,
    NT_ERROR_FILE_EXISTS = 80,
    NT_ERROR_INVALID_PARAMETER = 87,
    NT_ERROR_INSUFFICIENT_BUFFER = 122,
    NT_ERROR_MOD_NOT_FOUND = 126,
    NT_ERROR_PROC_NOT_FOUND = 127,
    NT_ERROR_DIR_NOT_EMPTY = 145,
    NT_ERROR_DISCARDED = 157,
    NT_ERROR_NOT_LOCKED = 158,
    NT_ERROR_LOCK_FAILED = 167,
    NT_ERROR_ALREADY_EXISTS = 183,
    NT_ERROR_MR_MID_NOT_FOUND = 317,
    NT_ERROR_INVALID_FLAGS = 1004,
    NT_ERROR_NO_UNICODE_TRANSLATION = 1113
};

void NT_MS_ABI nt_SetLastError(uint32_t error);

struct nt_file_time {
    uint32_t low;
    uint32_t high;
};

struct nt_system_time {
    uint16_t year, month, day_of_week, day, hour, minute, second, milliseconds;
};

struct nt_startup_info_w {
    uint32_t size;
    uint16_t *reserved, *desktop, *title;
    uint32_t x, y, x_size, y_size, x_count, y_count, fill_attribute, flags;
    uint16_t show_window, reserved_size;
    unsigned char *reserved_bytes;
    uintptr_t input, output, error;
};

struct nt_rect {
    int32_t left, top, right, bottom;
};

uintptr_t NT_MS_ABI nt_GetCurrentProcess(void);
void NT_MS_ABI nt_GetLocalTime(struct nt_system_time *result);
void NT_MS_ABI nt_GetSystemTimeAsFileTime(struct nt_file_time *result);
uint32_t NT_MS_ABI nt_GetTickCount(void);
void NT_MS_ABI nt_GetStartupInfoW(struct nt_startup_info_w *result);
void NT_MS_ABI nt_InitializeSListHead(void *head);
int32_t NT_MS_ABI nt_IsDebuggerPresent(void);
int32_t NT_MS_ABI nt_IsProcessorFeaturePresent(uint32_t feature);

int32_t NT_MS_ABI nt_SetRect(struct nt_rect *result, int32_t left, int32_t top,
                            int32_t right, int32_t bottom);
int32_t NT_MS_ABI nt_OffsetRect(struct nt_rect *rect, int32_t dx, int32_t dy);
int32_t NT_MS_ABI nt_IntersectRect(struct nt_rect *result, const struct nt_rect *a,
                                  const struct nt_rect *b);
int32_t NT_MS_ABI nt_UnionRect(struct nt_rect *result, const struct nt_rect *a,
                              const struct nt_rect *b);

int32_t NT_MS_ABI nt_MultiByteToWideChar(uint32_t code_page, uint32_t flags,
                                      const char *input, int32_t input_length,
                                      uint16_t *output, int32_t capacity);

uintptr_t NT_MS_ABI nt_GlobalAlloc(uint32_t flags, size_t size);
uintptr_t NT_MS_ABI nt_GlobalFree(uintptr_t handle);
void *NT_MS_ABI nt_GlobalLock(uintptr_t handle);
int32_t NT_MS_ABI nt_GlobalUnlock(uintptr_t handle);
uintptr_t NT_MS_ABI nt_LocalFree(uintptr_t handle);
uint32_t NT_MS_ABI nt_FormatMessageA(uint32_t flags, const void *source,
                                    uint32_t code, uint32_t language,
                                    char *buffer, uint32_t size, void *arguments);

uintptr_t NT_MS_ABI nt_GetModuleHandleW(const uint16_t *name);
uintptr_t NT_MS_ABI nt_LoadLibraryA(const char *name);
int32_t NT_MS_ABI nt_FreeLibrary(uintptr_t module);
nt_guest_proc NT_MS_ABI nt_GetProcAddress(uintptr_t module, const char *name);

typedef void (NT_MS_ABI *nt_void_callback)(void);
typedef int (NT_MS_ABI *nt_int_callback)(void);

struct nt_onexit_table {
    nt_guest_proc *first, *last, *end;
};

int NT_MS_ABI nt_atoi(const char *text);
double NT_MS_ABI nt_atof(const char *text);
int NT_MS_ABI nt_isspace(int byte);
int NT_MS_ABI nt_tolower(int byte);
int NT_MS_ABI nt_toupper(int byte);
int NT_MS_ABI nt_configthreadlocale(int mode);
int NT_MS_ABI nt_set_new_mode(int mode);
char *NT_MS_ABI nt_getenv(const char *name);
void NT_MS_ABI nt_initterm(const nt_void_callback *first, const nt_void_callback *last);
int NT_MS_ABI nt_initterm_e(const nt_int_callback *first, const nt_int_callback *last);
int NT_MS_ABI nt_initialize_onexit_table(struct nt_onexit_table *table);
int NT_MS_ABI nt_register_onexit_function(struct nt_onexit_table *table,
                                         nt_int_callback callback);
int NT_MS_ABI nt_crt_atexit(nt_void_callback callback);
void NT_MS_ABI nt_cexit(void);

/* The launcher owns this immutable environment for the guest lifetime. */
void nt_runtime_set_guest_environment(const char *const *environment);
void nt_runtime_release_onexit_table(struct nt_onexit_table *table);

/* The launcher supplies the host directory for the guest C: drive. */
bool nt_runtime_set_file_root(const char *directory);
int32_t NT_MS_ABI nt_CreateDirectoryA(const char *path, const void *security);
int32_t NT_MS_ABI nt_DeleteFileA(const char *path);
int32_t NT_MS_ABI nt_RemoveDirectoryA(const char *path);
int32_t NT_MS_ABI nt_MoveFileExA(const char *from, const char *to, uint32_t flags);
void *NT_MS_ABI nt_fopen(const char *path, const char *mode);
int NT_MS_ABI nt_fclose(void *stream);
size_t NT_MS_ABI nt_fread(void *buffer, size_t size, size_t count, void *stream);
size_t NT_MS_ABI nt_fwrite(const void *buffer, size_t size, size_t count, void *stream);
int NT_MS_ABI nt_set_fmode(int mode);
void nt_runtime_flush_streams(void);

/* A host surface owns its bitmap and can be used as the reference for memory DCs. */
uintptr_t nt_runtime_create_surface(int32_t width, int32_t height);
uint32_t nt_runtime_read_pixel(uintptr_t dc, int32_t x, int32_t y);
uintptr_t NT_MS_ABI nt_CreateCompatibleDC(uintptr_t dc);
uintptr_t NT_MS_ABI nt_CreateCompatibleBitmap(uintptr_t dc, int32_t width, int32_t height);
uintptr_t NT_MS_ABI nt_CreatePen(int32_t style, int32_t width, uint32_t color);
uintptr_t NT_MS_ABI nt_CreateSolidBrush(uint32_t color);
uintptr_t NT_MS_ABI nt_CreateHatchBrush(int32_t hatch, uint32_t color);
uintptr_t NT_MS_ABI nt_GetStockObject(int32_t index);
uintptr_t NT_MS_ABI nt_SelectObject(uintptr_t dc, uintptr_t object);
int32_t NT_MS_ABI nt_DeleteObject(uintptr_t object);
int32_t NT_MS_ABI nt_DeleteDC(uintptr_t dc);
int32_t NT_MS_ABI nt_SetBkMode(uintptr_t dc, int32_t mode);
int32_t NT_MS_ABI nt_SetMapMode(uintptr_t dc, int32_t mode);
uint32_t NT_MS_ABI nt_SetTextColor(uintptr_t dc, uint32_t color);
uint32_t NT_MS_ABI nt_SetPixel(uintptr_t dc, int32_t x, int32_t y, uint32_t color);
int32_t NT_MS_ABI nt_IntersectClipRect(uintptr_t dc, int32_t left, int32_t top,
                                      int32_t right, int32_t bottom);
int32_t NT_MS_ABI nt_SelectClipRgn(uintptr_t dc, uintptr_t region);
int32_t NT_MS_ABI nt_Rectangle(uintptr_t dc, int32_t left, int32_t top,
                              int32_t right, int32_t bottom);
int32_t NT_MS_ABI nt_BitBlt(uintptr_t dest, int32_t x, int32_t y,
                           int32_t width, int32_t height, uintptr_t source,
                           int32_t source_x, int32_t source_y, uint32_t rop);

uintptr_t NT_MS_ABI nt_CreateMenu(void);
int32_t NT_MS_ABI nt_AppendMenuA(uintptr_t menu, uint32_t flags, uintptr_t value,
                                 const char *label);
int32_t NT_MS_ABI nt_DestroyMenu(uintptr_t menu);
int32_t NT_MS_ABI nt_GetMenuItemCount(uintptr_t menu);
uint32_t NT_MS_ABI nt_CheckMenuItem(uintptr_t menu, uint32_t item, uint32_t flags);
int32_t NT_MS_ABI nt_EnableMenuItem(uintptr_t menu, uint32_t item, uint32_t flags);

struct nt_message {
    uintptr_t window;
    uint32_t message;
    uintptr_t wparam;
    intptr_t lparam;
    uint32_t time;
    int32_t x, y;
    uint32_t private_data;
};

typedef void (NT_MS_ABI *nt_timer_callback)(uintptr_t, uint32_t, uintptr_t, uint32_t);
uintptr_t NT_MS_ABI nt_SetTimer(uintptr_t window, uintptr_t id, uint32_t milliseconds,
                               nt_timer_callback callback);
int32_t NT_MS_ABI nt_KillTimer(uintptr_t window, uintptr_t id);
void NT_MS_ABI nt_PostQuitMessage(int32_t code);
int32_t NT_MS_ABI nt_GetMessageA(struct nt_message *message, uintptr_t window,
                                 uint32_t first, uint32_t last);
intptr_t NT_MS_ABI nt_DispatchMessageA(const struct nt_message *message);

#endif
