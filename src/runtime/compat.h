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
    NT_ERROR_NOT_SAME_DEVICE = 17,
    NT_ERROR_NO_MORE_FILES = 18,
    NT_ERROR_GEN_FAILURE = 31,
    NT_ERROR_SHARING_VIOLATION = 32,
    NT_ERROR_LOCK_VIOLATION = 33,
    NT_ERROR_HANDLE_EOF = 38,
    NT_ERROR_IO_INCOMPLETE = 996,
    NT_ERROR_IO_PENDING = 997,
    NT_ERROR_NOT_SUPPORTED = 50,
    NT_ERROR_FILE_EXISTS = 80,
    NT_ERROR_INVALID_PARAMETER = 87,
    NT_ERROR_BROKEN_PIPE = 109,
    NT_ERROR_INSUFFICIENT_BUFFER = 122,
    NT_ERROR_INVALID_NAME = 123,
    NT_ERROR_MOD_NOT_FOUND = 126,
    NT_ERROR_PROC_NOT_FOUND = 127,
    NT_ERROR_NEGATIVE_SEEK = 131,
    NT_ERROR_DIR_NOT_EMPTY = 145,
    NT_ERROR_DISCARDED = 157,
    NT_ERROR_NOT_LOCKED = 158,
    NT_ERROR_LOCK_FAILED = 167,
    NT_ERROR_ENVVAR_NOT_FOUND = 203,
    NT_ERROR_ALREADY_EXISTS = 183,
    NT_ERROR_MR_MID_NOT_FOUND = 317,
    NT_ERROR_TOO_MANY_POSTS = 298,
    NT_ERROR_INVALID_ADDRESS = 487,
    NT_ERROR_INVALID_FLAGS = 1004,
    NT_ERROR_OPERATION_ABORTED = 995,
    NT_ERROR_NO_UNICODE_TRANSLATION = 1113,
    NT_ERROR_TIMEOUT = 1460,
    NT_ERROR_INVALID_INDEX = 1413
};

void NT_MS_ABI nt_SetLastError(uint32_t error);
uint32_t NT_MS_ABI nt_GetLastError(void);

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
int32_t NT_MS_ABI nt_WideCharToMultiByte(uint32_t code_page, uint32_t flags,
                                       const uint16_t *input, int32_t input_length,
                                       char *output, int32_t capacity,
                                       const char *default_character, int32_t *used_default);
uint32_t NT_MS_ABI nt_GetACP(void);
uint32_t NT_MS_ABI nt_GetOEMCP(void);
int32_t NT_MS_ABI nt_IsValidCodePage(uint32_t page);
int32_t NT_MS_ABI nt_IsDBCSLeadByte(uint8_t byte);
size_t nt_runtime_wide_length(const uint16_t *text, size_t limit);
uint16_t *nt_runtime_utf8_to_wide(const char *text);

uintptr_t NT_MS_ABI nt_GlobalAlloc(uint32_t flags, size_t size);
uintptr_t NT_MS_ABI nt_GlobalFree(uintptr_t handle);
void *NT_MS_ABI nt_GlobalLock(uintptr_t handle);
int32_t NT_MS_ABI nt_GlobalUnlock(uintptr_t handle);
uintptr_t NT_MS_ABI nt_LocalFree(uintptr_t handle);
bool nt_runtime_claim_clipboard_memory(uintptr_t handle, unsigned char **data, size_t *size);
void nt_runtime_release_clipboard_memory(uintptr_t handle, bool release);
uint32_t NT_MS_ABI nt_FormatMessageA(uint32_t flags, const void *source,
                                    uint32_t code, uint32_t language,
                                    char *buffer, uint32_t size, void *arguments);

uintptr_t NT_MS_ABI nt_GetModuleHandleW(const uint16_t *name);
uint32_t NT_MS_ABI nt_GetModuleFileNameA(uintptr_t module, char *buffer, uint32_t size);
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
void NT_MS_ABI nt_c_exit(void);
int NT_MS_ABI nt_is_c_termination_complete(void);
typedef void (NT_MS_ABI *nt_tls_exit_callback)(void *, uint32_t, void *);
void NT_MS_ABI nt_register_thread_local_exe_atexit_callback(nt_tls_exit_callback callback);
void NT_MS_ABI nt_set_app_type(int type);
void NT_MS_ABI nt_wassert(const uint16_t *expression, const uint16_t *file, uint32_t line);
bool nt_runtime_show_assertion(const char *message);
struct nt_math_exception {
    int type;
    const char *name;
    double arg1, arg2, result;
};
typedef int (NT_MS_ABI *nt_matherr_callback)(struct nt_math_exception *);
void NT_MS_ABI nt_setusermatherr(nt_matherr_callback callback);
double nt_runtime_sqrt(double number);

/* The launcher owns this immutable environment for the guest lifetime. */
void nt_runtime_set_guest_environment(const char *const *environment);
void nt_runtime_release_onexit_table(struct nt_onexit_table *table);
int NT_MS_ABI nt_initialize_narrow_environment(void);

/* The single-threaded launcher sets these borrowed strings before entry. */
bool nt_runtime_set_guest_image(uintptr_t module, const char *path, const char *command_line);
uintptr_t nt_runtime_guest_module(void);
const char *const *nt_runtime_guest_argv(int *count);
int NT_MS_ABI nt_configure_narrow_argv(int mode);
char *NT_MS_ABI nt_get_narrow_winmain_command_line(void);

/* Call from a single-threaded test host. The child runs only an embedded fixture. */
bool nt_runtime_run_fixture(nt_int_callback entry, uint32_t *exit_code);
int32_t NT_MS_ABI nt_TerminateProcess(uintptr_t process, uint32_t exit_code);
void NT_MS_ABI nt_exit(int code);
void NT_MS_ABI nt_quick_exit(int code);
void NT_MS_ABI nt_terminate(void);

struct nt_context64 {
    uint64_t homes[6];
    uint32_t flags, mxcsr;
    uint16_t cs, ds, es, fs, gs, ss;
    uint32_t eflags;
    uint64_t debug[6];
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15, rip;
    _Alignas(16) unsigned char floating[512], vectors[26 * 16];
    uint64_t vector_control, debug_control, last_branch_to, last_branch_from;
    uint64_t last_exception_to, last_exception_from;
};

struct nt_runtime_function { uint32_t begin, end, unwind; };
struct nt_exception_pointers {
    void *record;
    struct nt_context64 *context;
};

struct nt_exception_record {
    uint32_t code, flags;
    struct nt_exception_record *nested;
    void *address;
    uint32_t count;
    uintptr_t information[15];
};

struct nt_dispatcher_context {
    uint64_t pc, base;
    const struct nt_runtime_function *function;
    uint64_t frame, target;
    struct nt_context64 *context;
    void *handler;
    const void *handler_data;
    void *history;
    uint32_t scope_index;
};

/* The trusted dispatcher provides the executable range and owns the
 * non-returning transfer after a filter selects a handler. */
typedef void (*nt_seh_transfer)(struct nt_exception_record *, uint64_t frame,
                                uint64_t target, const struct nt_dispatcher_context *);
bool nt_runtime_set_seh_dispatch(uint32_t code_begin, uint32_t code_end, nt_seh_transfer transfer);
int32_t NT_MS_ABI nt_C_specific_handler(struct nt_exception_record *record, uint64_t frame,
                                       struct nt_context64 *context,
                                       struct nt_dispatcher_context *dispatch);

typedef int32_t (NT_MS_ABI *nt_exception_filter)(struct nt_exception_pointers *);
void NT_MS_ABI nt_RtlCaptureContext(struct nt_context64 *context);
void NT_MS_ABI nt_RaiseException(uint32_t code, uint32_t flags, uint32_t count, const uintptr_t *arguments);
void NT_MS_ABI nt_RtlUnwind(void *frame, void *target, struct nt_exception_record *record, void *value);
void NT_MS_ABI nt_RtlUnwindEx(void *frame, void *target, struct nt_exception_record *record,
                            void *value, struct nt_context64 *context, void *history);
bool nt_runtime_enable_software_seh(uint32_t begin, uint32_t end,
                                   bool (*readable)(uint32_t, size_t, const void *), const void *opaque);
bool nt_runtime_set_runtime_functions(uintptr_t base, size_t image_size,
                                      const struct nt_runtime_function *table, size_t count);
const struct nt_runtime_function *NT_MS_ABI nt_RtlLookupFunctionEntry(
    uint64_t pc, uint64_t *base, void *history);
struct nt_unwind_pointers { void *floating[16], *integer[16]; };
bool nt_runtime_set_unwind_stack(const void *start, size_t length);
void *NT_MS_ABI nt_RtlVirtualUnwind(uint32_t type, uint64_t base, uint64_t pc,
                                   const struct nt_runtime_function *function,
                                   struct nt_context64 *context, void **handler_data,
                                   uint64_t *frame, struct nt_unwind_pointers *pointers);
nt_exception_filter NT_MS_ABI nt_SetUnhandledExceptionFilter(nt_exception_filter filter);
int32_t NT_MS_ABI nt_UnhandledExceptionFilter(struct nt_exception_pointers *pointers);
void **NT_MS_ABI nt_current_exception(void);
void **NT_MS_ABI nt_current_exception_context(void);
typedef void (NT_MS_ABI *nt_signal_handler)(int);
nt_signal_handler NT_MS_ABI nt_signal(int signal, nt_signal_handler handler);
int32_t NT_MS_ABI nt_seh_filter_exe(uint32_t code, struct nt_exception_pointers *pointers);

/* The launcher supplies the host directory for the guest C: drive. */
bool nt_runtime_set_file_root(const char *directory);
bool nt_runtime_set_drive_root(char letter, const char *directory);
bool nt_runtime_set_unc_root(const char *server, const char *share, const char *directory);
int nt_runtime_open_guest_directory(const char *path, char *dos_path, size_t capacity);
bool nt_runtime_check_guest_file(const char *path, char *dos_path, size_t capacity, bool *exists);
bool nt_runtime_guest_directory_entry(int fd, const char *name, bool *directory);
int32_t NT_MS_ABI nt_CreateDirectoryA(const char *path, const void *security);
uintptr_t NT_MS_ABI nt_CreateFileA(const char *path, uint32_t access, uint32_t share,
                                  const void *security, uint32_t disposition, uint32_t flags,
                                  uintptr_t template_file);
int32_t NT_MS_ABI nt_CloseHandle(uintptr_t handle);
int32_t NT_MS_ABI nt_DeleteFileA(const char *path);
int32_t NT_MS_ABI nt_RemoveDirectoryA(const char *path);
int32_t NT_MS_ABI nt_MoveFileExA(const char *from, const char *to, uint32_t flags);
int32_t NT_MS_ABI nt_MoveFileA(const char *from, const char *to);
int32_t NT_MS_ABI nt_CopyFileA(const char *from, const char *to, int32_t fail_if_exists);
uint32_t NT_MS_ABI nt_GetCurrentDirectoryA(uint32_t size, char *buffer);
int32_t NT_MS_ABI nt_SetCurrentDirectoryA(const char *path);
uint32_t NT_MS_ABI nt_GetFullPathNameA(const char *path, uint32_t size, char *buffer, char **leaf);
uint32_t NT_MS_ABI nt_GetFileAttributesA(const char *path);
int32_t NT_MS_ABI nt_SetFileAttributesA(const char *path, uint32_t attributes);
struct nt_file_attribute_data;
int32_t NT_MS_ABI nt_GetFileAttributesExA(const char *path, int32_t level,
                                         struct nt_file_attribute_data *info);
uint32_t NT_MS_ABI nt_GetTempPathA(uint32_t size, char *buffer);
void *NT_MS_ABI nt_fopen(const char *path, const char *mode);
int NT_MS_ABI nt_open_osfhandle(intptr_t handle, int flags);
void *NT_MS_ABI nt_fdopen(int descriptor, const char *mode);
int NT_MS_ABI nt_close(int descriptor);
int NT_MS_ABI nt_fclose(void *stream);
size_t NT_MS_ABI nt_fread(void *buffer, size_t size, size_t count, void *stream);
size_t NT_MS_ABI nt_fwrite(const void *buffer, size_t size, size_t count, void *stream);
int NT_MS_ABI nt_set_fmode(int mode);
void nt_runtime_flush_streams(void);
bool nt_runtime_set_standard_streams(int input, int output, int error);
void *NT_MS_ABI nt_acrt_iob_func(uint32_t index);
int *NT_MS_ABI nt_p_commode(void);
int NT_MS_ABI nt_stdio_vsprintf(uint64_t options, char *buffer, size_t length,
                                const char *format, void *locale, const unsigned char *args);
int NT_MS_ABI nt_stdio_vfprintf(uint64_t options, void *stream, const char *format,
                                void *locale, const unsigned char *args);
int NT_MS_ABI nt_stdio_vsscanf(uint64_t options, const char *buffer, size_t length,
                               const char *format, void *locale, const unsigned char *args);

/* A host surface owns its bitmap and can be used as the reference for memory DCs. */
uintptr_t nt_runtime_create_surface(int32_t width, int32_t height);
uint32_t nt_runtime_read_pixel(uintptr_t dc, int32_t x, int32_t y);
typedef bool (*nt_surface_reader)(const uint32_t *pixels, int32_t width, int32_t height,
                                  void *context);
bool nt_runtime_read_surface(uintptr_t dc, nt_surface_reader reader, void *context);
struct _cairo;
bool nt_runtime_apply_font(uintptr_t font, struct _cairo *target);
char *nt_runtime_ansi_to_utf8(const char *text, size_t length);
char *nt_runtime_utf16_to_utf8(const uint16_t *text, size_t length);
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

struct nt_point { int32_t x, y; };
struct nt_size { int32_t width, height; };
struct nt_logfont {
    int32_t height, width, escapement, orientation, weight;
    uint8_t italic, underline, strike, charset, out_precision, clip_precision, quality, pitch;
    char face[32];
};
struct nt_text_metrics {
    int32_t height, ascent, descent, internal_leading, external_leading;
    int32_t average_width, max_width, weight, overhang, dpi_x, dpi_y;
    uint8_t first, last, replacement, break_char, italic, underline, strike, pitch, charset;
};

uintptr_t NT_MS_ABI nt_CreateFontA(int32_t height, int32_t width, int32_t escapement,
                                 int32_t orientation, int32_t weight, uint32_t italic,
                                 uint32_t underline, uint32_t strike, uint32_t charset,
                                 uint32_t out_precision, uint32_t clip_precision, uint32_t quality,
                                 uint32_t pitch, const char *face);
uintptr_t NT_MS_ABI nt_CreateFontIndirectA(const struct nt_logfont *font);
int32_t NT_MS_ABI nt_GetTextMetricsA(uintptr_t dc, struct nt_text_metrics *metrics);
int32_t NT_MS_ABI nt_GetTextExtentPoint32A(uintptr_t dc, const char *text, int32_t count,
                                         struct nt_size *size);
int32_t NT_MS_ABI nt_GetTextExtentPoint32W(uintptr_t dc, const uint16_t *text, int32_t count,
                                         struct nt_size *size);
int32_t NT_MS_ABI nt_ExtTextOutW(uintptr_t dc, int32_t x, int32_t y, uint32_t options,
                                const struct nt_rect *rect, const uint16_t *text,
                                uint32_t count, const int32_t *advances);
int32_t NT_MS_ABI nt_Ellipse(uintptr_t dc, int32_t left, int32_t top,
                            int32_t right, int32_t bottom);
int32_t NT_MS_ABI nt_Polygon(uintptr_t dc, const struct nt_point *points, int32_t count);
int32_t NT_MS_ABI nt_Polyline(uintptr_t dc, const struct nt_point *points, int32_t count);
int32_t NT_MS_ABI nt_GetDeviceCaps(uintptr_t dc, int32_t index);

struct nt_docinfo {
    int32_t size;
    const char *name, *output, *data_type;
    uint32_t type;
};
struct nt_print_dialog {
    uint32_t size;
    uintptr_t owner, device_mode, device_names, dc;
    uint32_t flags;
    uint16_t first, last, min_page, max_page, copies;
    uintptr_t instance;
    intptr_t custom_data;
    uintptr_t print_hook, setup_hook;
    const char *print_template, *setup_template;
    uintptr_t print_template_handle, setup_template_handle;
};
bool nt_runtime_set_pdf_printer(int fd, int32_t width, int32_t height);
uintptr_t nt_runtime_create_pdf_printer(int fd, int32_t width, int32_t height);
int32_t NT_MS_ABI nt_PrintDlgA(struct nt_print_dialog *dialog);
int32_t NT_MS_ABI nt_StartDocA(uintptr_t dc, const struct nt_docinfo *info);
int32_t NT_MS_ABI nt_StartPage(uintptr_t dc);
int32_t NT_MS_ABI nt_EndPage(uintptr_t dc);
int32_t NT_MS_ABI nt_EndDoc(uintptr_t dc);

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
