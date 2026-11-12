/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "nt_services.h"
#include "winbase.h"

#include <errno.h>
#include <sched.h>
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

uint32_t nt_runtime_nt_error(nt_status status)
{
    switch (status) {
    case NT_STATUS_SUCCESS: return NT_ERROR_SUCCESS;
    case NT_STATUS_INVALID_HANDLE:
    case NT_STATUS_OBJECT_TYPE_MISMATCH: return NT_ERROR_INVALID_HANDLE;
    case NT_STATUS_INVALID_PARAMETER: return NT_ERROR_INVALID_PARAMETER;
    case NT_STATUS_ACCESS_DENIED: return NT_ERROR_ACCESS_DENIED;
    case NT_STATUS_NO_MEMORY: return NT_ERROR_NOT_ENOUGH_MEMORY;
    case NT_STATUS_OBJECT_NAME_EXISTS: return NT_ERROR_ALREADY_EXISTS;
    case NT_STATUS_OBJECT_NAME_NOT_FOUND: return NT_ERROR_FILE_NOT_FOUND;
    case NT_STATUS_SEMAPHORE_LIMIT_EXCEEDED: return NT_ERROR_TOO_MANY_POSTS;
    case NT_STATUS_NOT_SUPPORTED: return NT_ERROR_NOT_SUPPORTED;
    case NT_STATUS_ACCESS_VIOLATION: return 998; /* ERROR_NOACCESS */
    default: return NT_ERROR_GEN_FAILURE;
    }
}

static void get_system_time(struct nt_system_time *result, bool local_time)
{
    if (result == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return;
    }
    struct timespec now;
    struct tm local;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        (local_time ? localtime_r(&now.tv_sec, &local) : gmtime_r(&now.tv_sec, &local)) == NULL ||
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

void NT_MS_ABI nt_GetLocalTime(struct nt_system_time *result) { get_system_time(result, true); }
void NT_MS_ABI nt_GetSystemTime(struct nt_system_time *result) { get_system_time(result, false); }

static const int64_t epoch_offset = INT64_C(11644473600);
static const uint64_t ticks_per_second = UINT64_C(10000000);

static nt_status system_time_ticks(uint64_t *ticks)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
        now.tv_sec < -epoch_offset || now.tv_sec > INT64_MAX - epoch_offset)
        return NT_STATUS_UNSUCCESSFUL;
    const uint64_t seconds = (uint64_t)(now.tv_sec + epoch_offset);
    const uint64_t fraction = (uint64_t)now.tv_nsec / 100;
    if (seconds > (UINT64_MAX - fraction) / ticks_per_second)
        return NT_STATUS_UNSUCCESSFUL;
    *ticks = seconds * ticks_per_second + fraction;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtQuerySystemTime(int64_t *result)
{
    if (!result)
        return NT_STATUS_ACCESS_VIOLATION;
    uint64_t ticks;
    if (system_time_ticks(&ticks) != NT_STATUS_SUCCESS || ticks > INT64_MAX)
        return NT_STATUS_UNSUCCESSFUL;
    *result = (int64_t)ticks;
    return NT_STATUS_SUCCESS;
}

void NT_MS_ABI nt_GetSystemTimeAsFileTime(struct nt_file_time *result)
{
    if (!result) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return;
    }
    uint64_t ticks;
    if (system_time_ticks(&ticks) != NT_STATUS_SUCCESS) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return;
    }
    *result = (struct nt_file_time){(uint32_t)ticks, (uint32_t)(ticks >> 32)};
}

nt_status nt_runtime_wait_deadline(int64_t ticks, struct nt_wait_deadline *deadline)
{
    if (ticks >= 0) {
        deadline->clock = CLOCK_REALTIME;
        deadline->time.tv_sec = ticks / (int64_t)ticks_per_second - epoch_offset;
        deadline->time.tv_nsec = (ticks % (int64_t)ticks_per_second) * 100;
        return NT_STATUS_SUCCESS;
    }
    /* The magnitude of INT64_MIN is representable as uint64_t. */
    const uint64_t duration = (uint64_t)(-(ticks + 1)) + 1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return NT_STATUS_UNSUCCESSFUL;
    const long nanoseconds = now.tv_nsec + (long)(duration % ticks_per_second) * 100;
    const uint64_t seconds = duration / ticks_per_second + (nanoseconds / 1000000000L);
    if (now.tv_sec < 0 || seconds > (uint64_t)(INT64_MAX - now.tv_sec))
        return NT_STATUS_UNSUCCESSFUL;
    deadline->clock = CLOCK_MONOTONIC;
    deadline->time = (struct timespec){now.tv_sec + (time_t)seconds, nanoseconds % 1000000000L};
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtDelayExecution(uint8_t alertable, const int64_t *interval)
{
    (void)alertable; /* No guest APC producer is registered. */
    if (!interval)
        return NT_STATUS_ACCESS_VIOLATION;
    struct nt_wait_deadline deadline;
    const nt_status status = nt_runtime_wait_deadline(*interval, &deadline);
    if (status != NT_STATUS_SUCCESS || deadline.time.tv_sec < 0)
        return status;
    int error;
    do {
        error = clock_nanosleep(deadline.clock, TIMER_ABSTIME, &deadline.time, NULL);
    } while (error == EINTR);
    return error ? NT_STATUS_UNSUCCESSFUL : NT_STATUS_SUCCESS;
}

uint64_t NT_MS_ABI nt_GetTickCount64(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0 || now.tv_sec < 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

uint32_t NT_MS_ABI nt_GetTickCount(void) { return (uint32_t)nt_GetTickCount64(); }

void NT_MS_ABI nt_Sleep(uint32_t milliseconds)
{
    if (milliseconds == 0) {
        sched_yield();
        return;
    }
    if (milliseconds == UINT32_MAX) {
        const int64_t interval = -INT64_C(3600) * 10000000;
        for (;;)
            (void)nt_NtDelayExecution(0, &interval);
    }
    const int64_t interval = -(int64_t)milliseconds * 10000;
    (void)nt_NtDelayExecution(0, &interval);
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
