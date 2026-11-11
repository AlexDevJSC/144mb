/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_NLS_H
#define NTNATIVE_NLS_H
#include "compat.h"

struct nt_cp_info { uint32_t maximum; unsigned char replacement[2], lead[12]; };
struct nt_timezone_info {
    int32_t bias;
    uint16_t standard_name[32];
    struct nt_system_time standard_date;
    int32_t standard_bias;
    uint16_t daylight_name[32];
    struct nt_system_time daylight_date;
    int32_t daylight_bias;
};

int32_t NT_MS_ABI nt_GetCPInfo(uint32_t page, struct nt_cp_info *info);
uint32_t NT_MS_ABI nt_GetUserDefaultLCID(void);
int32_t NT_MS_ABI nt_IsValidLocale(uint32_t locale, uint32_t flags);
typedef int32_t (NT_MS_ABI *nt_locale_callback)(uint16_t *);
int32_t NT_MS_ABI nt_EnumSystemLocalesW(nt_locale_callback callback, uint32_t flags);
int32_t NT_MS_ABI nt_GetLocaleInfoA(uint32_t locale, uint32_t type, char *out, int32_t capacity);
int32_t NT_MS_ABI nt_GetLocaleInfoW(uint32_t locale, uint32_t type, uint16_t *out, int32_t capacity);
int32_t NT_MS_ABI nt_GetLocaleInfoEx(const uint16_t *locale, uint32_t type, uint16_t *out, int32_t capacity);
int32_t NT_MS_ABI nt_CompareStringW(uint32_t locale, uint32_t flags, const uint16_t *left, int32_t left_size,
                                    const uint16_t *right, int32_t right_size);
int32_t NT_MS_ABI nt_LCMapStringW(uint32_t locale, uint32_t flags, const uint16_t *source, int32_t count,
                                  uint16_t *target, int32_t capacity);
int32_t NT_MS_ABI nt_LCMapStringEx(const uint16_t *locale, uint32_t flags, const uint16_t *source,
                                   int32_t count, uint16_t *target, int32_t capacity,
                                   void *version, void *reserved, intptr_t sort_handle);
int32_t NT_MS_ABI nt_GetStringTypeW(uint32_t type, const uint16_t *source, int32_t count, uint16_t *result);
int32_t NT_MS_ABI nt_GetDateFormatW(uint32_t locale, uint32_t flags, const struct nt_system_time *date,
                                   const uint16_t *format, uint16_t *target, int32_t capacity);
int32_t NT_MS_ABI nt_GetTimeFormatW(uint32_t locale, uint32_t flags, const struct nt_system_time *time,
                                   const uint16_t *format, uint16_t *target, int32_t capacity);
uint32_t NT_MS_ABI nt_GetTimeZoneInformation(struct nt_timezone_info *result);
#endif
