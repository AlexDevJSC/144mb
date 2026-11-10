/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(struct nt_file_time) == 8, "FILETIME layout");
_Static_assert(sizeof(struct nt_system_time) == 16, "SYSTEMTIME layout");
_Static_assert(sizeof(struct nt_startup_info_w) == 104, "STARTUPINFOW layout");
_Static_assert(offsetof(struct nt_startup_info_w, input) == 80, "startup handle layout");
_Static_assert(sizeof(struct nt_rect) == 16, "RECT layout");

uintptr_t NT_MS_ABI nt_GetCurrentProcess(void)
{
    return UINTPTR_MAX;
}

void NT_MS_ABI nt_GetLocalTime(struct nt_system_time *result)
{
    if (result == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return;
    }
    struct timespec now;
    struct tm local;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        localtime_r(&now.tv_sec, &local) == NULL ||
        local.tm_year < -1900 || local.tm_year > UINT16_MAX - 1900) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return;
    }
    *result = (struct nt_system_time){
        (uint16_t)(local.tm_year + 1900), (uint16_t)(local.tm_mon + 1),
        (uint16_t)local.tm_wday, (uint16_t)local.tm_mday, (uint16_t)local.tm_hour,
        (uint16_t)local.tm_min, (uint16_t)local.tm_sec, (uint16_t)(now.tv_nsec / 1000000)
    };
}

void NT_MS_ABI nt_GetSystemTimeAsFileTime(struct nt_file_time *result)
{
    const int64_t epoch_offset = INT64_C(11644473600);
    const uint64_t ticks_per_second = UINT64_C(10000000);
    if (result == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return;
    }
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        now.tv_sec < -epoch_offset || now.tv_sec > INT64_MAX - epoch_offset) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return;
    }
    const uint64_t seconds = (uint64_t)(now.tv_sec + epoch_offset);
    const uint64_t fraction = (uint64_t)now.tv_nsec / 100;
    if (seconds > (UINT64_MAX - fraction) / ticks_per_second) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return;
    }
    const uint64_t ticks = seconds * ticks_per_second + fraction;
    *result = (struct nt_file_time){(uint32_t)ticks, (uint32_t)(ticks >> 32)};
}

uint32_t NT_MS_ABI nt_GetTickCount(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0 || now.tv_sec < 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return (uint32_t)((uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000);
}

void NT_MS_ABI nt_GetStartupInfoW(struct nt_startup_info_w *result)
{
    if (result == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return;
    }
    memset(result, 0, sizeof(*result));
    result->size = sizeof(*result);
}

void NT_MS_ABI nt_InitializeSListHead(void *head)
{
    if (head == NULL || (uintptr_t)head % 16 != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return;
    }
    memset(head, 0, 16);
}

int32_t NT_MS_ABI nt_IsDebuggerPresent(void)
{
    FILE *status = fopen("/proc/self/status", "re");
    if (status == NULL)
        return 0;
    char line[256];
    int tracer = 0;
    while (fgets(line, sizeof(line), status) != NULL) {
        if (sscanf(line, "TracerPid: %d", &tracer) == 1)
            break;
    }
    fclose(status);
    return tracer > 0;
}

int32_t NT_MS_ABI nt_IsProcessorFeaturePresent(uint32_t feature)
{
    switch (feature) {
    case 3:  /* PF_MMX_INSTRUCTIONS_AVAILABLE */
        return __builtin_cpu_supports("mmx") != 0;
    case 6:  /* PF_XMMI_INSTRUCTIONS_AVAILABLE */
        return __builtin_cpu_supports("sse") != 0;
    case 10: /* PF_XMMI64_INSTRUCTIONS_AVAILABLE */
        return __builtin_cpu_supports("sse2") != 0;
    case 13: /* PF_SSE3_INSTRUCTIONS_AVAILABLE */
        return __builtin_cpu_supports("sse3") != 0;
    case 39: /* PF_AVX_INSTRUCTIONS_AVAILABLE */
        return __builtin_cpu_supports("avx") != 0;
    case 40: /* PF_AVX2_INSTRUCTIONS_AVAILABLE */
        return __builtin_cpu_supports("avx2") != 0;
    default:
        return 0;
    }
}

static bool empty_rect(const struct nt_rect *rect)
{
    return rect->left >= rect->right || rect->top >= rect->bottom;
}

static int32_t min_i32(int32_t a, int32_t b)
{
    return a < b ? a : b;
}

static int32_t max_i32(int32_t a, int32_t b)
{
    return a > b ? a : b;
}

int32_t NT_MS_ABI nt_SetRect(struct nt_rect *result, int32_t left, int32_t top,
                            int32_t right, int32_t bottom)
{
    if (result == NULL)
        return 0;
    *result = (struct nt_rect){left, top, right, bottom};
    return 1;
}

static int32_t offset_coordinate(int32_t coordinate, int32_t delta)
{
    const uint32_t bits = (uint32_t)coordinate + (uint32_t)delta;
    int32_t result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

int32_t NT_MS_ABI nt_OffsetRect(struct nt_rect *rect, int32_t dx, int32_t dy)
{
    if (rect == NULL)
        return 0;
    *rect = (struct nt_rect){
        offset_coordinate(rect->left, dx), offset_coordinate(rect->top, dy),
        offset_coordinate(rect->right, dx), offset_coordinate(rect->bottom, dy)
    };
    return 1;
}

int32_t NT_MS_ABI nt_IntersectRect(struct nt_rect *result, const struct nt_rect *a,
                                  const struct nt_rect *b)
{
    if (result == NULL || a == NULL || b == NULL)
        return 0;
    const struct nt_rect overlap = {max_i32(a->left, b->left), max_i32(a->top, b->top),
                                    min_i32(a->right, b->right), min_i32(a->bottom, b->bottom)};
    const bool empty = empty_rect(&overlap);
    *result = empty ? (struct nt_rect){0} : overlap;
    return !empty;
}

int32_t NT_MS_ABI nt_UnionRect(struct nt_rect *result, const struct nt_rect *a,
                              const struct nt_rect *b)
{
    if (result == NULL || a == NULL || b == NULL)
        return 0;
    if (empty_rect(a)) {
        const bool empty = empty_rect(b);
        *result = empty ? (struct nt_rect){0} : *b;
        return !empty;
    }
    *result = empty_rect(b) ? *a :
        (struct nt_rect){min_i32(a->left, b->left), min_i32(a->top, b->top),
                         max_i32(a->right, b->right), max_i32(a->bottom, b->bottom)};
    return 1;
}
