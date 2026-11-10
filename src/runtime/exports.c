/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/counter.h"
#include "ntnative/runtime.h"

#include <math.h>
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

static void NT_MS_ABI win_set_last_error(uint32_t error)
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
    return memchr(data, byte, count);
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
    return strchr(string, byte);
}

static char *NT_MS_ABI crt_strrchr(const char *string, int byte)
{
    return strrchr(string, byte);
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
    return sqrt(value);
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

static const struct runtime_export ntdll_exports[] = {
    EXPORT("NtQueryPerformanceCounter", nt_counter)
};

static const struct runtime_export kernel32_exports[] = {
    EXPORT("GetCurrentProcessId", win_process_id),
    EXPORT("GetCurrentThreadId", win_thread_id),
    EXPORT("GetLastError", win_get_last_error),
    EXPORT("MulDiv", win_mul_div),
    EXPORT("QueryPerformanceCounter", win_counter),
    EXPORT("SetLastError", win_set_last_error)
};

static const struct runtime_export vcruntime_exports[] = {
    EXPORT("memchr", crt_memchr),
    EXPORT("memcmp", crt_memcmp),
    EXPORT("memcpy", crt_memcpy),
    EXPORT("memmove", crt_memmove),
    EXPORT("memset", crt_memset),
    EXPORT("strchr", crt_strchr),
    EXPORT("strrchr", crt_strrchr)
};

static const struct runtime_export heap_exports[] = {
    EXPORT("free", crt_free),
    EXPORT("malloc", crt_malloc),
    EXPORT("realloc", crt_realloc)
};

static const struct runtime_export math_exports[] = {
    EXPORT("ceil", crt_ceil),
    EXPORT("floor", crt_floor),
    EXPORT("roundf", crt_roundf),
    EXPORT("sqrt", crt_sqrt),
    EXPORT("sqrtf", crt_sqrtf)
};

static const struct runtime_export string_exports[] = {
    EXPORT("strcat", crt_strcat),
    EXPORT("strcmp", crt_strcmp),
    EXPORT("strcpy", crt_strcpy),
    EXPORT("strcspn", crt_strcspn),
    EXPORT("strlen", crt_strlen),
    EXPORT("strncpy", crt_strncpy),
    EXPORT("strspn", crt_strspn),
    EXPORT("wcslen", crt_wcslen)
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
    MODULE("vcruntime140.dll", vcruntime_exports),
    MODULE("api-ms-win-crt-heap-l1-1-0.dll", heap_exports),
    MODULE("api-ms-win-crt-math-l1-1-0.dll", math_exports),
    MODULE("api-ms-win-crt-string-l1-1-0.dll", string_exports),
    MODULE("api-ms-win-crt-utility-l1-1-0.dll", utility_exports)
#undef MODULE
};

nt_guest_proc nt_runtime_resolve(const char *module, const char *name)
{
    if (module == NULL || name == NULL)
        return NULL;

    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); ++i) {
        if (!nt_module_name_equal(module, modules[i].name))
            continue;
        for (size_t j = 0; j < modules[i].count; ++j) {
            if (strcmp(name, modules[i].exports[j].name) == 0)
                return modules[i].exports[j].target;
        }
        break;
    }
    return NULL;
}
