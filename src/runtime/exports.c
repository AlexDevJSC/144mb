/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/counter.h"
#include "compat.h"
#include "desktop.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local uint32_t last_error;

static nt_status NT_MS_ABI nt_counter(int64_t *counter, int64_t *frequency)
{
    return nt_query_performance_counter(counter, frequency);
}

static int32_t NT_MS_ABI win_counter(int64_t *counter)
{
    return nt_query_performance_counter(counter, NULL) == NT_STATUS_SUCCESS;
}

static uint32_t NT_MS_ABI win_process_id(void)
{
    return (uint32_t)getpid();
}

static uint32_t NT_MS_ABI win_thread_id(void)
{
    return (uint32_t)gettid();
}

static uint32_t NT_MS_ABI win_get_last_error(void)
{
    return last_error;
}

void NT_MS_ABI nt_SetLastError(uint32_t error)
{
    last_error = error;
}

static int32_t NT_MS_ABI win_mul_div(int32_t left, int32_t right, int32_t divisor)
{
    if (divisor == 0)
        return -1;

    const int64_t product = (int64_t)left * right;
    const bool negative = (product < 0) != (divisor < 0);
    const int64_t numerator = product < 0 ? -product : product;
    const int64_t denominator = divisor < 0 ? -(int64_t)divisor : divisor;
    int64_t result = (numerator + denominator / 2) / denominator;
    if (negative)
        result = -result;

    return result < INT32_MIN || result > INT32_MAX ? -1 : (int32_t)result;
}

static void *NT_MS_ABI crt_memchr(const void *data, int byte, size_t count)
{
    /* The guest CRT returns an unqualified pointer into the same buffer. */
    return (void *)memchr(data, byte, count);
}

static int NT_MS_ABI crt_memcmp(const void *left, const void *right, size_t count)
{
    return memcmp(left, right, count);
}

static void *NT_MS_ABI crt_memcpy(void *destination, const void *source, size_t count)
{
    return memcpy(destination, source, count);
}

static void *NT_MS_ABI crt_memmove(void *destination, const void *source, size_t count)
{
    return memmove(destination, source, count);
}

static void *NT_MS_ABI crt_memset(void *destination, int byte, size_t count)
{
    return memset(destination, byte, count);
}

static char *NT_MS_ABI crt_strchr(const char *string, int byte)
{
    return (char *)strchr(string, byte);
}

static char *NT_MS_ABI crt_strrchr(const char *string, int byte)
{
    return (char *)strrchr(string, byte);
}

static void *NT_MS_ABI crt_malloc(size_t size)
{
    return malloc(size == 0 ? 1 : size);
}

static void NT_MS_ABI crt_free(void *memory)
{
    free(memory);
}

static void *NT_MS_ABI crt_realloc(void *memory, size_t size)
{
    if (memory == NULL)
        return crt_malloc(size);
    if (size == 0) {
        free(memory);
        return NULL;
    }
    return realloc(memory, size);
}

static double NT_MS_ABI crt_ceil(double value)
{
    return ceil(value);
}

static double NT_MS_ABI crt_floor(double value)
{
    return floor(value);
}

static float NT_MS_ABI crt_roundf(float value)
{
    return roundf(value);
}

static double NT_MS_ABI crt_sqrt(double value)
{
    return nt_runtime_sqrt(value);
}

static float NT_MS_ABI crt_sqrtf(float value)
{
    return sqrtf(value);
}

static char *NT_MS_ABI crt_strcat(char *destination, const char *source)
{
    return strcat(destination, source);
}

static int NT_MS_ABI crt_strcmp(const char *left, const char *right)
{
    return strcmp(left, right);
}

static char *NT_MS_ABI crt_strcpy(char *destination, const char *source)
{
    return strcpy(destination, source);
}

static size_t NT_MS_ABI crt_strcspn(const char *string, const char *reject)
{
    return strcspn(string, reject);
}

static size_t NT_MS_ABI crt_strlen(const char *string)
{
    return strlen(string);
}

static char *NT_MS_ABI crt_strncpy(char *destination, const char *source, size_t count)
{
    return strncpy(destination, source, count);
}

static size_t NT_MS_ABI crt_strspn(const char *string, const char *accept)
{
    return strspn(string, accept);
}

static size_t NT_MS_ABI crt_wcslen(const uint16_t *string)
{
    const uint16_t *end = string;
    while (*end != 0)
        ++end;
    return (size_t)(end - string);
}

typedef int (NT_MS_ABI *guest_compare)(const void *, const void *);

static int compare_for_linux(const void *left, const void *right, void *context)
{
    const guest_compare *compare = context;
    return (*compare)(left, right);
}

static void NT_MS_ABI crt_qsort(void *base, size_t count, size_t width, guest_compare compare)
{
    if (count > 1)
        qsort_r(base, count, width, compare_for_linux, &compare);
}

struct runtime_export {
    const char *name;
    nt_guest_proc target;
};

#define EXPORT(name, function) {name, (nt_guest_proc)function}
#define WIN_EXPORT(name) EXPORT(#name, nt_##name)

static const struct runtime_export ntdll_exports[] = {
    EXPORT("NtQueryPerformanceCounter", nt_counter)
};

static const struct runtime_export kernel32_exports[] = {
    WIN_EXPORT(CloseHandle),
    WIN_EXPORT(CreateDirectoryA),
    WIN_EXPORT(CreateFileA),
    WIN_EXPORT(DeleteFileA),
    WIN_EXPORT(FormatMessageA),
    WIN_EXPORT(FreeLibrary),
    WIN_EXPORT(GetCurrentProcess),
    EXPORT("GetCurrentProcessId", win_process_id),
    EXPORT("GetCurrentThreadId", win_thread_id),
    EXPORT("GetLastError", win_get_last_error),
    WIN_EXPORT(GetLocalTime),
    WIN_EXPORT(GetModuleFileNameA),
    WIN_EXPORT(GetModuleHandleW),
    WIN_EXPORT(GetProcAddress),
    WIN_EXPORT(GetStartupInfoW),
    WIN_EXPORT(GetSystemTimeAsFileTime),
    WIN_EXPORT(GetTickCount),
    WIN_EXPORT(GlobalAlloc),
    WIN_EXPORT(GlobalFree),
    WIN_EXPORT(GlobalLock),
    WIN_EXPORT(GlobalUnlock),
    WIN_EXPORT(InitializeSListHead),
    WIN_EXPORT(IsDebuggerPresent),
    WIN_EXPORT(IsProcessorFeaturePresent),
    WIN_EXPORT(LoadLibraryA),
    WIN_EXPORT(LocalFree),
    WIN_EXPORT(MoveFileExA),
    EXPORT("MulDiv", win_mul_div),
    WIN_EXPORT(MultiByteToWideChar),
    EXPORT("QueryPerformanceCounter", win_counter),
    WIN_EXPORT(RemoveDirectoryA),
    WIN_EXPORT(RtlCaptureContext),
    WIN_EXPORT(RtlLookupFunctionEntry),
    WIN_EXPORT(RtlVirtualUnwind),
    WIN_EXPORT(SetLastError),
    WIN_EXPORT(SetUnhandledExceptionFilter),
    WIN_EXPORT(TerminateProcess),
    WIN_EXPORT(UnhandledExceptionFilter)
};

static const struct runtime_export user32_exports[] = {
    WIN_EXPORT(AdjustWindowRectEx),
    WIN_EXPORT(AppendMenuA),
    WIN_EXPORT(BeginPaint),
    WIN_EXPORT(CheckMenuItem),
    WIN_EXPORT(CloseClipboard),
    WIN_EXPORT(CreateMenu),
    WIN_EXPORT(CreateWindowExA),
    WIN_EXPORT(DefWindowProcA),
    WIN_EXPORT(DestroyMenu),
    WIN_EXPORT(DestroyWindow),
    WIN_EXPORT(DispatchMessageA),
    WIN_EXPORT(DrawMenuBar),
    WIN_EXPORT(EmptyClipboard),
    WIN_EXPORT(EnableMenuItem),
    WIN_EXPORT(EnableWindow),
    WIN_EXPORT(EndPaint),
    WIN_EXPORT(GetClientRect),
    WIN_EXPORT(GetDC),
    WIN_EXPORT(GetKeyboardState),
    WIN_EXPORT(GetMenu),
    WIN_EXPORT(GetMenuItemCount),
    WIN_EXPORT(GetMenuItemRect),
    WIN_EXPORT(GetMessageA),
    WIN_EXPORT(GetSysColor),
    WIN_EXPORT(GetWindowLongPtrA),
    WIN_EXPORT(GetWindowRect),
    WIN_EXPORT(IntersectRect),
    WIN_EXPORT(InvalidateRect),
    WIN_EXPORT(KillTimer),
    WIN_EXPORT(MessageBeep),
    WIN_EXPORT(OffsetRect),
    WIN_EXPORT(OpenClipboard),
    WIN_EXPORT(PostQuitMessage),
    WIN_EXPORT(RegisterClassA),
    WIN_EXPORT(ReleaseDC),
    WIN_EXPORT(SendMessageA),
    WIN_EXPORT(SetClipboardData),
    WIN_EXPORT(SetMenu),
    WIN_EXPORT(SetRect),
    WIN_EXPORT(SetTimer),
    WIN_EXPORT(SetWindowLongPtrA),
    WIN_EXPORT(SetWindowPos),
    WIN_EXPORT(SetWindowTextA),
    WIN_EXPORT(ShowWindow),
    WIN_EXPORT(SystemParametersInfoA),
    WIN_EXPORT(TranslateMessage),
    WIN_EXPORT(UnionRect)
};

static const struct runtime_export gdi32_exports[] = {
    WIN_EXPORT(BitBlt),
    WIN_EXPORT(CreateCompatibleBitmap),
    WIN_EXPORT(CreateCompatibleDC),
    WIN_EXPORT(CreateFontA),
    WIN_EXPORT(CreateFontIndirectA),
    WIN_EXPORT(CreateHatchBrush),
    WIN_EXPORT(CreatePen),
    WIN_EXPORT(CreateSolidBrush),
    WIN_EXPORT(DeleteDC),
    WIN_EXPORT(DeleteObject),
    WIN_EXPORT(Ellipse),
    WIN_EXPORT(EndDoc),
    WIN_EXPORT(EndPage),
    WIN_EXPORT(ExtTextOutW),
    WIN_EXPORT(GetDeviceCaps),
    WIN_EXPORT(GetStockObject),
    WIN_EXPORT(GetTextExtentPoint32A),
    WIN_EXPORT(GetTextExtentPoint32W),
    WIN_EXPORT(GetTextMetricsA),
    WIN_EXPORT(IntersectClipRect),
    WIN_EXPORT(Polygon),
    WIN_EXPORT(Polyline),
    WIN_EXPORT(Rectangle),
    WIN_EXPORT(SelectClipRgn),
    WIN_EXPORT(SelectObject),
    WIN_EXPORT(SetBkMode),
    WIN_EXPORT(SetMapMode),
    WIN_EXPORT(SetPixel),
    WIN_EXPORT(SetTextColor),
    WIN_EXPORT(StartDocA),
    WIN_EXPORT(StartPage)
};

static const struct runtime_export comdlg32_exports[] = {
    WIN_EXPORT(PrintDlgA)
};

static const struct runtime_export vcruntime_exports[] = {
    EXPORT("__current_exception", nt_current_exception),
    EXPORT("__current_exception_context", nt_current_exception_context),
    EXPORT("memchr", crt_memchr),
    EXPORT("memcmp", crt_memcmp),
    EXPORT("memcpy", crt_memcpy),
    EXPORT("memmove", crt_memmove),
    EXPORT("memset", crt_memset),
    EXPORT("strchr", crt_strchr),
    EXPORT("strrchr", crt_strrchr)
};

static const struct runtime_export heap_exports[] = {
    EXPORT("_set_new_mode", nt_set_new_mode),
    EXPORT("free", crt_free),
    EXPORT("malloc", crt_malloc),
    EXPORT("realloc", crt_realloc)
};

static const struct runtime_export math_exports[] = {
    EXPORT("__setusermatherr", nt_setusermatherr),
    EXPORT("_fdopen", nt_fdopen),
    EXPORT("ceil", crt_ceil),
    EXPORT("floor", crt_floor),
    EXPORT("roundf", crt_roundf),
    EXPORT("sqrt", crt_sqrt),
    EXPORT("sqrtf", crt_sqrtf)
};

static const struct runtime_export string_exports[] = {
    EXPORT("isspace", nt_isspace),
    EXPORT("strcat", crt_strcat),
    EXPORT("strcmp", crt_strcmp),
    EXPORT("strcpy", crt_strcpy),
    EXPORT("strcspn", crt_strcspn),
    EXPORT("strlen", crt_strlen),
    EXPORT("strncpy", crt_strncpy),
    EXPORT("strspn", crt_strspn),
    EXPORT("tolower", nt_tolower),
    EXPORT("toupper", nt_toupper),
    EXPORT("wcslen", crt_wcslen)
};

static const struct runtime_export convert_exports[] = {
    EXPORT("atof", nt_atof),
    EXPORT("atoi", nt_atoi)
};

static const struct runtime_export environment_exports[] = {
    EXPORT("getenv", nt_getenv)
};

static const struct runtime_export locale_exports[] = {
    EXPORT("_configthreadlocale", nt_configthreadlocale)
};

static const struct runtime_export crt_runtime_exports[] = {
    EXPORT("_c_exit", nt_c_exit),
    EXPORT("_cexit", nt_cexit),
    EXPORT("_configure_narrow_argv", nt_configure_narrow_argv),
    EXPORT("_crt_atexit", nt_crt_atexit),
    EXPORT("_exit", nt_quick_exit),
    EXPORT("_get_narrow_winmain_command_line", nt_get_narrow_winmain_command_line),
    EXPORT("_initialize_narrow_environment", nt_initialize_narrow_environment),
    EXPORT("_initialize_onexit_table", nt_initialize_onexit_table),
    EXPORT("_initterm", nt_initterm),
    EXPORT("_initterm_e", nt_initterm_e),
    EXPORT("_is_c_termination_complete", nt_is_c_termination_complete),
    EXPORT("_register_onexit_function", nt_register_onexit_function),
    EXPORT("_register_thread_local_exe_atexit_callback", nt_register_thread_local_exe_atexit_callback),
    EXPORT("_seh_filter_exe", nt_seh_filter_exe),
    EXPORT("_set_app_type", nt_set_app_type),
    EXPORT("_wassert", nt_wassert),
    EXPORT("exit", nt_exit),
    EXPORT("signal", nt_signal),
    EXPORT("terminate", nt_terminate)
};

static const struct runtime_export stdio_exports[] = {
    EXPORT("__acrt_iob_func", nt_acrt_iob_func),
    EXPORT("__p__commode", nt_p_commode),
    EXPORT("__stdio_common_vfprintf", nt_stdio_vfprintf),
    EXPORT("__stdio_common_vsprintf", nt_stdio_vsprintf),
    EXPORT("__stdio_common_vsscanf", nt_stdio_vsscanf),
    EXPORT("_close", nt_close),
    EXPORT("_fdopen", nt_fdopen),
    EXPORT("_open_osfhandle", nt_open_osfhandle),
    EXPORT("_set_fmode", nt_set_fmode),
    EXPORT("fclose", nt_fclose),
    EXPORT("fopen", nt_fopen),
    EXPORT("fread", nt_fread),
    EXPORT("fwrite", nt_fwrite)
};

static const struct runtime_export utility_exports[] = {
    EXPORT("qsort", crt_qsort)
};

static const struct {
    const char *name;
    const struct runtime_export *exports;
    size_t count;
} modules[] = {
#define MODULE(name, exports) {name, exports, sizeof(exports) / sizeof(exports[0])}
    MODULE("ntdll.dll", ntdll_exports),
    MODULE("kernel32.dll", kernel32_exports),
    MODULE("user32.dll", user32_exports),
    MODULE("gdi32.dll", gdi32_exports),
    MODULE("comdlg32.dll", comdlg32_exports),
    MODULE("vcruntime140.dll", vcruntime_exports),
    MODULE("api-ms-win-crt-convert-l1-1-0.dll", convert_exports),
    MODULE("api-ms-win-crt-environment-l1-1-0.dll", environment_exports),
    MODULE("api-ms-win-crt-heap-l1-1-0.dll", heap_exports),
    MODULE("api-ms-win-crt-math-l1-1-0.dll", math_exports),
    MODULE("api-ms-win-crt-locale-l1-1-0.dll", locale_exports),
    MODULE("api-ms-win-crt-runtime-l1-1-0.dll", crt_runtime_exports),
    MODULE("api-ms-win-crt-stdio-l1-1-0.dll", stdio_exports),
    MODULE("api-ms-win-crt-string-l1-1-0.dll", string_exports),
    MODULE("api-ms-win-crt-utility-l1-1-0.dll", utility_exports)
#undef MODULE
};

enum { MODULE_COUNT = sizeof(modules) / sizeof(modules[0]), MODULE_BASE = 0x10000000 };
static uint32_t module_references[MODULE_COUNT];
static pthread_mutex_t module_lock = PTHREAD_MUTEX_INITIALIZER;

static size_t module_index(const char *name)
{
    for (size_t i = 0; i < MODULE_COUNT; ++i) {
        if (nt_module_name_equal(name, modules[i].name))
            return i;
    }
    return MODULE_COUNT;
}

static nt_guest_proc find_export(size_t module, const char *name)
{
    if (module < MODULE_COUNT) {
        for (size_t i = 0; i < modules[module].count; ++i) {
            if (strcmp(name, modules[module].exports[i].name) == 0)
                return modules[module].exports[i].target;
        }
    }
    return NULL;
}

nt_guest_proc nt_runtime_resolve(const char *module, const char *name)
{
    if (module == NULL || name == NULL)
        return NULL;
    return find_export(module_index(module), name);
}

static size_t requested_module(const char *name)
{
    if (name == NULL || *name == '\0')
        return MODULE_COUNT;
    if (strchr(name, '.') != NULL)
        return module_index(name);
    char with_extension[128];
    const size_t length = strlen(name);
    if (length >= sizeof(with_extension) - sizeof(".dll") + 1)
        return MODULE_COUNT;
    memcpy(with_extension, name, length);
    memcpy(with_extension + length, ".dll", sizeof(".dll"));
    return module_index(with_extension);
}

uintptr_t NT_MS_ABI nt_LoadLibraryA(const char *name)
{
    const size_t module = requested_module(name);
    if (module == MODULE_COUNT) {
        nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
        return 0;
    }
    pthread_mutex_lock(&module_lock);
    const bool acquired = module_references[module] < UINT32_MAX;
    if (acquired)
        ++module_references[module];
    pthread_mutex_unlock(&module_lock);
    if (!acquired) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return MODULE_BASE + module;
}

uintptr_t NT_MS_ABI nt_GetModuleHandleW(const uint16_t *name)
{
    char narrow[128];
    if (name == NULL) {
        const uintptr_t module = nt_runtime_guest_module();
        if (module != 0)
            return module;
    } else {
        for (size_t i = 0; i < sizeof(narrow) && name[i] <= 0x7f; ++i) {
            narrow[i] = (char)name[i];
            if (name[i] == 0) {
                const size_t module = requested_module(narrow);
                if (module != MODULE_COUNT)
                    return MODULE_BASE + module;
                break;
            }
        }
    }
    nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
    return 0;
}

static size_t module_handle_index(uintptr_t handle)
{
    return handle >= MODULE_BASE && handle - MODULE_BASE < MODULE_COUNT ?
           handle - MODULE_BASE : MODULE_COUNT;
}

int32_t NT_MS_ABI nt_FreeLibrary(uintptr_t handle)
{
    const size_t module = module_handle_index(handle);
    if (module == MODULE_COUNT) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    pthread_mutex_lock(&module_lock);
    const bool acquired = module_references[module] != 0;
    if (acquired)
        --module_references[module];
    pthread_mutex_unlock(&module_lock);
    if (!acquired)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return acquired;
}

nt_guest_proc NT_MS_ABI nt_GetProcAddress(uintptr_t handle, const char *name)
{
    const size_t module = module_handle_index(handle);
    if (module == MODULE_COUNT) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return NULL;
    }
    const nt_guest_proc target = (uintptr_t)name > UINT16_MAX ? find_export(module, name) : NULL;
    if (target == NULL)
        nt_SetLastError(NT_ERROR_PROC_NOT_FOUND);
    return target;
}
