/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nls.h"

#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wctype.h>

enum { EN_US = 0x409, INVARIANT = 0x7f, USER_DEFAULT = 0x400, SYSTEM_DEFAULT = 0x800,
       MAX_TEXT_UNITS = 65535 };
static const char *const weekdays[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July", "August",
    "September", "October", "November", "December"};

static bool supported_locale(uint32_t locale)
{
    return locale == EN_US || locale == INVARIANT || locale == USER_DEFAULT || locale == SYSTEM_DEFAULT;
}

static uint32_t locale_from_name(const uint16_t *name)
{
    if (name == NULL)
        return USER_DEFAULT;
    if (*name == 0)
        return INVARIANT;
    const uint16_t en_us[] = u"en-US";
    for (size_t i = 0; i < sizeof(en_us) / sizeof(*en_us); ++i) {
        const uint16_t lower = name[i] >= 'A' && name[i] <= 'Z' ? (uint16_t)(name[i] + 32) : name[i];
        const uint16_t expected = en_us[i] >= 'A' && en_us[i] <= 'Z' ? (uint16_t)(en_us[i] + 32) : en_us[i];
        if (lower != expected)
            return 0;
    }
    return EN_US;
}

uint32_t NT_MS_ABI nt_GetUserDefaultLCID(void) { return EN_US; }

int32_t NT_MS_ABI nt_IsValidLocale(uint32_t locale, uint32_t flags)
{
    if (flags == 0 || (flags & ~3u) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_FLAGS);
        return 0;
    }
    return supported_locale(locale);
}

int32_t NT_MS_ABI nt_EnumSystemLocalesW(nt_locale_callback callback, uint32_t flags)
{
    if (callback == NULL || flags == 0 || (flags & ~3u) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint16_t locales[][9] = {u"00000409", u"0000007f"};
    for (size_t i = 0; i < sizeof(locales) / sizeof(locales[0]); ++i) {
        if (!callback(locales[i]))
            break;
    }
    return 1;
}

int32_t NT_MS_ABI nt_GetCPInfo(uint32_t page, struct nt_cp_info *info)
{
    if (page == 0)
        page = nt_GetACP();
    else if (page == 1)
        page = nt_GetOEMCP();
    if (info == NULL || !nt_IsValidCodePage(page)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint32_t width = page == 65001 ? 4u :
                           (page == 932 || page == 936 || page == 949 || page == 950) ? 2u : 1u;
    *info = (struct nt_cp_info){.maximum = width, .replacement = {'?', 0}};
    return 1;
}

static const char *locale_value(uint32_t locale, uint32_t type, char abbreviation[4])
{
    static const struct { uint16_t type; const char *value; } values[] = {
        {2, "English (United States)"}, {3, "ENU"}, {4, "English"}, {5, "1"}, {6, "United States"},
        {7, "USA"}, {8, "United States"}, {10, "1"}, {11, "437"}, {12, ","}, {13, "1"},
        {14, "."}, {15, ","}, {16, "3;0"}, {17, "2"}, {18, "1"}, {19, "0123456789"},
        {20, "$"}, {21, "USD"}, {22, "."}, {23, ","}, {24, "3;0"}, {25, "2"}, {26, "2"},
        {27, "0"}, {28, "0"}, {29, "/"}, {30, ":"}, {31, "M/d/yyyy"}, {32, "dddd, MMMM d, yyyy"},
        {33, "0"}, {34, "0"}, {35, "0"}, {36, "1"}, {37, "0"}, {38, "0"}, {39, "0"},
        {40, "AM"}, {41, "PM"}, {0x50, ""}, {0x51, "-"}, {0x59, "en"}, {0x5a, "US"},
        {0x1001, "English"}, {0x1002, "United States"}, {0x1003, "h:mm:ss tt"},
        {0x1004, "1252"}, {0x1005, "0"}, {0x1006, "MMMM yyyy"}, {0x1007, "US Dollar"}, {0x1008, "US Dollar"},
        {0x1009, "1"}, {0x100a, "1"}, {0x100b, "0"}, {0x100c, "6"}, {0x100d, "0"},
        {0x100e, ""}, {0x100f, ""}, {0x1011, "10000"}
    };
    if (type == 1 || type == 9)
        return locale == INVARIANT ? "007f" : "0409";
    if (type == 0x5c)
        return locale == INVARIANT ? "" : "en-US";
    if (type >= 0x2a && type <= 0x30)
        return weekdays[type - 0x2a];
    if (type >= 0x38 && type <= 0x43)
        return months[type - 0x38];
    const char *short_name = type >= 0x31 && type <= 0x37 ? weekdays[type - 0x31] :
                             type >= 0x44 && type <= 0x4f ? months[type - 0x44] : NULL;
    if (short_name != NULL) {
        memcpy(abbreviation, short_name, 3);
        abbreviation[3] = '\0';
        return abbreviation;
    }
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
        if (values[i].type == type)
            return values[i].value;
    return NULL;
}

static int32_t locale_info(uint32_t locale, uint32_t type, void *output, int32_t capacity, bool wide)
{
    const bool number = (type & UINT32_C(0x20000000)) != 0;
    const uint32_t property = type & ~UINT32_C(0xe0000000);
    char abbreviation[4];
    const char *value = supported_locale(locale) ? locale_value(locale, property, abbreviation) : NULL;
    if (value == NULL || capacity < 0 || (capacity > 0 && output == NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint32_t numeric = 0;
    if (number) {
        char *end;
        numeric = (uint32_t)strtoul(value, &end, property == 1 || property == 9 ? 16 : 10);
        if (end == value || *end != '\0') {
            nt_SetLastError(NT_ERROR_INVALID_FLAGS);
            return 0;
        }
    }
    const int32_t count = number ? wide ? 2 : 4 : (int32_t)strlen(value) + 1;
    if (capacity != 0) {
        if (capacity < count) {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (number)
            memcpy(output, &numeric, sizeof(numeric));
        else if (wide)
            nt_MultiByteToWideChar(1252, 0, value, count, output, capacity);
        else
            memcpy(output, value, (size_t)count);
    }
    return count;
}

int32_t NT_MS_ABI nt_GetLocaleInfoA(uint32_t locale, uint32_t type, char *out, int32_t capacity)
{
    return locale_info(locale, type, out, capacity, false);
}

int32_t NT_MS_ABI nt_GetLocaleInfoW(uint32_t locale, uint32_t type, uint16_t *out, int32_t capacity)
{
    return locale_info(locale, type, out, capacity, true);
}

int32_t NT_MS_ABI nt_GetLocaleInfoEx(const uint16_t *locale, uint32_t type, uint16_t *out, int32_t capacity)
{
    return nt_GetLocaleInfoW(locale_from_name(locale), type, out, capacity);
}

static pthread_once_t locale_once = PTHREAD_ONCE_INIT;
static locale_t unicode_locale;
static void initialize_locale(void) { unicode_locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", NULL); }

static bool locale_available(void)
{
    if (pthread_once(&locale_once, initialize_locale) == 0 && unicode_locale != (locale_t)0)
        return true;
    nt_SetLastError(NT_ERROR_GEN_FAILURE);
    return false;
}

static int32_t source_size(const uint16_t *text, int32_t count, bool terminator)
{
    if (text == NULL || count < -1 || count > MAX_TEXT_UNITS)
        return -1;
    if (count != -1)
        return count;
    const size_t length = nt_runtime_wide_length(text, MAX_TEXT_UNITS);
    return length < MAX_TEXT_UNITS ? (int32_t)length + terminator : -1;
}

static uint32_t read_codepoint(const uint16_t *text, int32_t length, int32_t *at)
{
    const uint32_t lead = text[(*at)++];
    if (lead >= 0xd800 && lead <= 0xdbff && *at < length &&
        text[*at] >= 0xdc00 && text[*at] <= 0xdfff)
        return 0x10000 + ((lead - 0xd800) << 10) + text[(*at)++] - 0xdc00;
    return lead;
}

int32_t NT_MS_ABI nt_CompareStringW(uint32_t locale, uint32_t flags, const uint16_t *left, int32_t left_size,
                                    const uint16_t *right, int32_t right_size)
{
    left_size = source_size(left, left_size, false);
    right_size = source_size(right, right_size, false);
    if (!supported_locale(locale) || (flags & ~UINT32_C(0x11)) != 0 || left_size < 0 || right_size < 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags != 0 && !locale_available())
        return 0;
    int32_t l = 0, r = 0;
    while (l < left_size && r < right_size) {
        uint32_t a = read_codepoint(left, left_size, &l), b = read_codepoint(right, right_size, &r);
        if (flags != 0) {
            a = towlower_l(a, unicode_locale);
            b = towlower_l(b, unicode_locale);
        }
        if (a != b)
            return a < b ? 1 : 3;
    }
    return l < left_size ? 3 : r < right_size ? 1 : 2;
}

int32_t NT_MS_ABI nt_LCMapStringW(uint32_t locale, uint32_t flags, const uint16_t *source, int32_t count,
                                  uint16_t *target, int32_t capacity)
{
    const uint32_t operation = flags & ~UINT32_C(0x01000000);
    count = source_size(source, count, true);
    if (!supported_locale(locale) || (operation != 0x100 && operation != 0x200) || count <= 0 ||
        capacity < 0 || (capacity > 0 && target == NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!locale_available())
        return 0;
    size_t at = 0;
    for (int pass = 0; pass < (capacity != 0 ? 2 : 1); ++pass) {
        at = 0;
        for (int32_t i = 0; i < count;) {
            uint32_t code = read_codepoint(source, count, &i);
            code = operation == 0x100 ? towlower_l(code, unicode_locale) : towupper_l(code, unicode_locale);
            if (code > 0xffff) {
                if (pass != 0) {
                    target[at] = (uint16_t)(0xd800 + ((code - 0x10000) >> 10));
                    target[at + 1] = (uint16_t)(0xdc00 + ((code - 0x10000) & 1023));
                }
                at += 2;
            } else {
                if (pass != 0)
                    target[at] = (uint16_t)code;
                ++at;
            }
        }
        if (capacity != 0 && at > (size_t)capacity) {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (capacity != 0 && source == target && at != (size_t)count) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    return (int32_t)at;
}

int32_t NT_MS_ABI nt_LCMapStringEx(const uint16_t *locale, uint32_t flags, const uint16_t *source,
                                   int32_t count, uint16_t *target, int32_t capacity,
                                   void *version, void *reserved, intptr_t sort_handle)
{
    if (version != NULL || reserved != NULL || sort_handle != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return nt_LCMapStringW(locale_from_name(locale), flags, source, count, target, capacity);
}

int32_t NT_MS_ABI nt_GetStringTypeW(uint32_t type, const uint16_t *source, int32_t count, uint16_t *result)
{
    count = source_size(source, count, true);
    if (type != 1 || count <= 0 || result == NULL || source == result) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!locale_available())
        return 0;
    for (int32_t i = 0; i < count;) {
        const int32_t start = i;
        const uint32_t code = read_codepoint(source, count, &i);
        uint16_t flags = 0;
        if (iswupper_l(code, unicode_locale)) flags |= 1;
        if (iswlower_l(code, unicode_locale)) flags |= 2;
        if (iswdigit_l(code, unicode_locale)) flags |= 4;
        if (iswspace_l(code, unicode_locale)) flags |= 8;
        if (iswpunct_l(code, unicode_locale)) flags |= 16;
        if (iswcntrl_l(code, unicode_locale)) flags |= 32;
        if (iswblank_l(code, unicode_locale)) flags |= 64;
        if (iswxdigit_l(code, unicode_locale)) flags |= 128;
        if (iswalpha_l(code, unicode_locale)) flags |= 256;
        if (iswprint_l(code, unicode_locale) || iswcntrl_l(code, unicode_locale)) flags |= 512;
        for (int32_t j = start; j < i; ++j)
            result[j] = flags;
    }
    return 1;
}

static const char *format_field(uint16_t token, size_t width, bool date, const struct nt_system_time *value,
                                 int weekday, char digits[12])
{
    unsigned int number;
    if (date) {
        switch (token) {
        case 'd':
            if (width > 2)
                return weekdays[weekday];
            number = value->day;
            break;
        case 'M':
            if (width > 2)
                return months[value->month - 1];
            number = value->month;
            break;
        case 'y': number = width <= 2 ? value->year % 100u : value->year; break;
        default: return NULL;
        }
    } else {
        switch (token) {
        case 'h': number = value->hour % 12u != 0 ? value->hour % 12u : 12; break;
        case 'H': number = value->hour; break;
        case 'm': number = value->minute; break;
        case 's': number = value->second; break;
        case 't': return value->hour < 12 ? "AM" : "PM";
        default: return NULL;
        }
    }
    snprintf(digits, 12, "%0*u", (int)(width < 5 ? width : 4), number);
    return digits;
}

static int32_t format_date_time(const struct nt_system_time *input, const uint16_t *format,
                                 bool date, uint16_t *target, int32_t capacity)
{
    struct nt_system_time now = {0};
    if (input == NULL) {
        nt_GetLocalTime(&now);
        input = &now;
    }
    if (capacity < 0 || (capacity != 0 && target == NULL) || input->hour >= 24 ||
        input->minute >= 60 || input->second >= 60 || (date &&
        (input->year < 1601 || input->month < 1 || input->month > 12 || input->day < 1 || input->day > 31))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int weekday = 0;
    if (date) {
        struct tm day = {.tm_year = input->year - 1900, .tm_mon = input->month - 1, .tm_mday = input->day};
        (void)timegm(&day);
        if (day.tm_mday != input->day || day.tm_mon != input->month - 1) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        weekday = (day.tm_wday + 6) % 7;
    }
    const size_t format_size = nt_runtime_wide_length(format, 256);
    if (format_size == 256) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint16_t result[256 * 12 + 1];
    size_t length = 0;
    bool quoted = false;
    for (size_t i = 0; i < format_size;) {
        const uint16_t token = format[i++];
        if (token == '\'') {
            if (format[i] == '\'') {
                result[length++] = token;
                ++i;
            } else {
                quoted = !quoted;
            }
            continue;
        }
        if (quoted) {
            result[length++] = token;
            continue;
        }
        size_t width = 1;
        while (format[i + width - 1] == token)
            ++width;
        char digits[12];
        const char *text = format_field(token, width, date, input, weekday, digits);
        if (text == NULL) {
            result[length++] = token;
            continue;
        }
        i += width - 1;
        const size_t count = ((token == 'd' || token == 'M') && width == 3) ? 3 :
                             token == 't' && width == 1 ? 1 : strlen(text);
        for (size_t j = 0; j < count; ++j)
            result[length++] = (unsigned char)text[j];
    }
    result[length++] = 0;
    if (capacity != 0 && (size_t)capacity < length) {
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (capacity != 0)
        memcpy(target, result, length * sizeof(*target));
    return (int32_t)length;
}

int32_t NT_MS_ABI nt_GetDateFormatW(uint32_t locale, uint32_t flags, const struct nt_system_time *date,
                                   const uint16_t *format, uint16_t *target, int32_t capacity)
{
    flags &= ~UINT32_C(0x80000000);
    if (!supported_locale(locale) || (flags != 0 && flags != 1 && flags != 2 && flags != 8) ||
        (format != NULL && flags != 0)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (format == NULL)
        format = flags == 2 ? u"dddd, MMMM d, yyyy" : flags == 8 ? u"MMMM yyyy" : u"M/d/yyyy";
    return format_date_time(date, format, true, target, capacity);
}

int32_t NT_MS_ABI nt_GetTimeFormatW(uint32_t locale, uint32_t flags, const struct nt_system_time *time,
                                   const uint16_t *format, uint16_t *target, int32_t capacity)
{
    flags &= ~UINT32_C(0x80000000);
    if (!supported_locale(locale) || (flags & ~15u) != 0 || (format != NULL && flags != 0)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (format == NULL) {
        static const uint16_t *const formats[4][3] = {
            {u"h:mm:ss tt", u"h:mm tt", u"h tt"}, {u"h:mm:ss", u"h:mm", u"h"},
            {u"HH:mm:ss tt", u"HH:mm tt", u"HH tt"}, {u"HH:mm:ss", u"HH:mm", u"HH"}
        };
        format = formats[(flags >> 2) & 3][(flags & 1) != 0 ? 2 : (flags & 2) != 0 ? 1 : 0];
    }
    return format_date_time(time, format, false, target, capacity);
}

static struct nt_system_time transition_date(time_t when, long offset)
{
    when += offset;
    struct tm date;
    if (gmtime_r(&when, &date) == NULL)
        return (struct nt_system_time){0};
    const int day = (date.tm_mday + 6) / 7;
    struct tm next_week = date;
    next_week.tm_mday += 7;
    (void)timegm(&next_week);
    return (struct nt_system_time){0, (uint16_t)(date.tm_mon + 1), (uint16_t)date.tm_wday,
        (uint16_t)(next_week.tm_mon != date.tm_mon ? 5 : day),
        (uint16_t)date.tm_hour, (uint16_t)date.tm_min, (uint16_t)date.tm_sec, 0};
}

static void copy_timezone_name(uint16_t target[32], const char *name)
{
    if (name != NULL)
        nt_MultiByteToWideChar(65001, 0, name, -1, target, 32);
}

uint32_t NT_MS_ABI nt_GetTimeZoneInformation(struct nt_timezone_info *result)
{
    _Static_assert(sizeof(struct nt_timezone_info) == 172, "TIME_ZONE_INFORMATION layout");
    if (result == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return UINT32_MAX;
    }
    const time_t now = time(NULL);
    struct tm current;
    if (localtime_r(&now, &current) == NULL) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return UINT32_MAX;
    }
    *result = (struct nt_timezone_info){.bias = (int32_t)(-current.tm_gmtoff / 60)};
    copy_timezone_name(result->standard_name, current.tm_zone);
    struct tm january = {.tm_year = current.tm_year, .tm_mon = 0, .tm_mday = 1, .tm_isdst = -1};
    const time_t first = mktime(&january);
    struct tm previous = january;
    long offsets[2] = {current.tm_gmtoff, current.tm_gmtoff};
    for (int day = 1; day <= 366; ++day) {
        const time_t sample = first + day * INT64_C(86400);
        struct tm local;
        if (localtime_r(&sample, &local) == NULL || local.tm_year != current.tm_year)
            break;
        if ((local.tm_isdst > 0) != (previous.tm_isdst > 0)) {
            time_t low = sample - 86400, high = sample;
            while (high - low > 1) {
                const time_t mid = low + (high - low) / 2;
                struct tm check;
                if (localtime_r(&mid, &check) != NULL && (check.tm_isdst > 0) == (previous.tm_isdst > 0))
                    low = mid;
                else
                    high = mid;
            }
            const bool summer = local.tm_isdst > 0;
            offsets[summer] = local.tm_gmtoff;
            if (summer) {
                result->daylight_date = transition_date(high, previous.tm_gmtoff);
                copy_timezone_name(result->daylight_name, local.tm_zone);
            } else {
                result->standard_date = transition_date(high, previous.tm_gmtoff);
                copy_timezone_name(result->standard_name, local.tm_zone);
            }
        }
        previous = local;
    }
    if (result->standard_date.month == 0 || result->daylight_date.month == 0) {
        result->standard_date = result->daylight_date = (struct nt_system_time){0};
        result->daylight_name[0] = 0;
        return 0;
    }
    result->bias = (int32_t)(-offsets[0] / 60);
    result->daylight_bias = (int32_t)((offsets[0] - offsets[1]) / 60);
    return current.tm_isdst > 0 ? 2 : 1;
}
