/* SPDX-License-Identifier: MIT */
#include "compat.h"

#include <unicode/ucnv.h>
#include <unicode/ucnv_err.h>

#include <stdlib.h>
#include <string.h>

enum { CP_ACP = 0, CP_OEMCP = 1, CP_437 = 437, CP_1252 = 1252, CP_UTF8 = 65001,
       MB_PRECOMPOSED = 1, MB_ERR_INVALID_CHARS = 8,
       WC_ERR_INVALID_CHARS = 0x80, WC_NO_BEST_FIT_CHARS = 0x400 };

static uint32_t cp1252(unsigned char byte)
{
    static const uint16_t extension[] = {
        0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
        0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
        0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
        0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
    };
    return byte >= 0x80 && byte <= 0x9f ? extension[byte - 0x80] : byte;
}

static uint32_t cp437(unsigned char byte)
{
    static const uint16_t extension[] = {
        0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
        0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
        0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
        0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
        0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
        0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
        0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
        0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
        0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
        0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
        0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
        0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
        0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
        0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
        0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
        0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
    };
    return byte < 0x80 ? byte : extension[byte - 0x80];
}

uint32_t NT_MS_ABI nt_GetACP(void) { return CP_1252; }
uint32_t NT_MS_ABI nt_GetOEMCP(void) { return CP_437; }

static const char *converter_name(uint32_t page)
{
    switch (page) {
    case 850: return "ibm-850";
    case 866: return "ibm-866";
    case 874: return "windows-874";
    case 932: return "windows-932";
    case 936: return "windows-936";
    case 949: return "windows-949";
    case 950: return "windows-950";
    case 1250: return "windows-1250";
    case 1251: return "windows-1251";
    case 1253: return "windows-1253";
    case 1254: return "windows-1254";
    case 1255: return "windows-1255";
    case 1256: return "windows-1256";
    case 1257: return "windows-1257";
    case 1258: return "windows-1258";
    default: return NULL;
    }
}

int32_t NT_MS_ABI nt_IsValidCodePage(uint32_t page)
{
    if (page == CP_1252 || page == CP_437 || page == CP_UTF8)
        return 1;
    const char *name = converter_name(page);
    if (!name)
        return 0;
    UErrorCode error = U_ZERO_ERROR;
    UConverter *converter = ucnv_open(name, &error);
    if (converter)
        ucnv_close(converter);
    return U_SUCCESS(error) && converter != NULL;
}

int32_t NT_MS_ABI nt_IsDBCSLeadByte(uint8_t byte)
{
    /* The guest ANSI code page is single-byte. */
    (void)byte;
    return 0;
}

static uint32_t effective_page(uint32_t page)
{
    if (page == CP_ACP)
        return CP_1252;
    if (page == CP_OEMCP)
        return CP_437;
    return page;
}

static char *append_utf8(char *output, uint32_t code)
{
    if (code < 0x80) {
        *output++ = (char)code;
    } else if (code < 0x800) {
        *output++ = (char)(0xc0 | (code >> 6));
        *output++ = (char)(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        *output++ = (char)(0xe0 | (code >> 12));
        *output++ = (char)(0x80 | ((code >> 6) & 0x3f));
        *output++ = (char)(0x80 | (code & 0x3f));
    } else {
        *output++ = (char)(0xf0 | (code >> 18));
        *output++ = (char)(0x80 | ((code >> 12) & 0x3f));
        *output++ = (char)(0x80 | ((code >> 6) & 0x3f));
        *output++ = (char)(0x80 | (code & 0x3f));
    }
    return output;
}

char *nt_runtime_ansi_to_utf8(const char *text, size_t length)
{
    if (length > (SIZE_MAX - 1) / 3)
        return NULL;
    char *converted = malloc(length * 3 + 1);
    if (converted == NULL)
        return NULL;
    char *out = converted;
    for (size_t i = 0; i < length; ++i)
        out = append_utf8(out, cp1252((unsigned char)text[i]));
    *out = '\0';
    return converted;
}

char *nt_runtime_utf16_to_utf8(const uint16_t *text, size_t length)
{
    if (length > (SIZE_MAX - 1) / 3)
        return NULL;
    char *converted = malloc(length * 3 + 1);
    if (converted == NULL)
        return NULL;
    char *out = converted;
    for (size_t i = 0; i < length; ++i) {
        uint32_t code = text[i];
        if (code >= 0xd800 && code <= 0xdfff) {
            if (code <= 0xdbff && i + 1 < length && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
                code = 0x10000 + ((code - 0xd800) << 10) + (text[++i] - 0xdc00);
            else
                code = 0xfffd;
        }
        out = append_utf8(out, code);
    }
    *out = '\0';
    return converted;
}

static uint32_t utf8_next(const unsigned char **cursor, const unsigned char *end, bool *valid)
{
    const unsigned char lead = *(*cursor)++;
    if (lead < 0x80)
        return lead;

    unsigned int remaining;
    uint32_t code;
    unsigned char low = 0x80;
    unsigned char high = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf) {
        remaining = 1;
        code = lead & 0x1f;
    } else if (lead >= 0xe0 && lead <= 0xef) {
        remaining = 2;
        code = lead & 0x0f;
        if (lead == 0xe0)
            low = 0xa0;
        if (lead == 0xed)
            high = 0x9f;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
        remaining = 3;
        code = lead & 0x07;
        if (lead == 0xf0)
            low = 0x90;
        if (lead == 0xf4)
            high = 0x8f;
    } else {
        *valid = false;
        return 0xfffd;
    }

    for (; remaining != 0; --remaining) {
        if (*cursor == end || **cursor < low || **cursor > high) {
            *valid = false;
            return 0xfffd;
        }
        code = (code << 6) | (*(*cursor)++ & 0x3f);
        low = 0x80;
        high = 0xbf;
    }
    return code;
}

static size_t convert(uint32_t page, bool strict, const unsigned char *cursor,
                      const unsigned char *end, uint16_t *output)
{
    size_t count = 0;
    while (cursor < end) {
        bool valid = true;
        uint32_t code;
        if (page == CP_UTF8)
            code = utf8_next(&cursor, end, &valid);
        else
            code = page == CP_437 ? cp437(*cursor++) : cp1252(*cursor++);
        if (!valid && strict)
            return SIZE_MAX;
        if (code > 0xffff) {
            code -= 0x10000;
            if (output != NULL) {
                output[count] = (uint16_t)(0xd800 + (code >> 10));
                output[count + 1] = (uint16_t)(0xdc00 + (code & 0x3ff));
            }
            count += 2;
        } else {
            if (output != NULL)
                output[count] = (uint16_t)code;
            ++count;
        }
    }
    return count;
}

static int32_t icu_to_wide(uint32_t page, uint32_t flags, const char *input,
                            int32_t input_length, uint16_t *output, int32_t capacity)
{
    if (!input || input_length < -1 || input_length == 0 || capacity < 0 ||
        (capacity > 0 && !output) || (flags & ~(MB_ERR_INVALID_CHARS | MB_PRECOMPOSED))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    UErrorCode error = U_ZERO_ERROR;
    UConverter *converter = ucnv_open(converter_name(page), &error);
    if (!converter || U_FAILURE(error)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (flags & MB_ERR_INVALID_CHARS)
        ucnv_setToUCallBack(converter, UCNV_TO_U_CALLBACK_STOP, NULL, NULL, NULL, &error);
    const int32_t count = input_length == -1 ? (int32_t)strlen(input) + 1 : input_length;
    if (capacity > 0) {
        const uintptr_t src = (uintptr_t)input, dst = (uintptr_t)output;
        if ((dst >= src && dst - src < (size_t)count) ||
            (src > dst && src - dst < (size_t)capacity * sizeof(*output))) {
            ucnv_close(converter);
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    const int32_t result = U_FAILURE(error) ? 0 :
        ucnv_toUChars(converter, (UChar *)output, capacity, input, count, &error);
    ucnv_close(converter);
    if (error == U_BUFFER_OVERFLOW_ERROR && capacity == 0)
        return result;
    if (U_FAILURE(error)) {
        nt_SetLastError(error == U_BUFFER_OVERFLOW_ERROR ? NT_ERROR_INSUFFICIENT_BUFFER :
                        NT_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    return result;
}

int32_t NT_MS_ABI nt_MultiByteToWideChar(uint32_t code_page, uint32_t flags,
                                      const char *input, int32_t input_length,
                                      uint16_t *output, int32_t capacity)
{
    code_page = effective_page(code_page);
    if (converter_name(code_page))
        return icu_to_wide(code_page, flags, input, input_length, output, capacity);
    const bool utf8 = code_page == CP_UTF8;
    if (input == NULL || input_length < -1 || input_length == 0 || capacity < 0 ||
        (capacity != 0 && output == NULL) ||
        !nt_IsValidCodePage(code_page)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint32_t supported = utf8 ? MB_ERR_INVALID_CHARS : MB_ERR_INVALID_CHARS | MB_PRECOMPOSED;
    if ((flags & ~supported) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_FLAGS);
        return 0;
    }
    const size_t length = input_length == -1 ? strlen(input) + 1 : (size_t)input_length;
    if (length > INT32_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const bool strict = (flags & MB_ERR_INVALID_CHARS) != 0;
    const unsigned char *bytes = (const unsigned char *)input;
    const size_t count = convert(code_page, strict, bytes, bytes + length, NULL);
    if (count == SIZE_MAX) {
        nt_SetLastError(NT_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (capacity == 0)
        return (int32_t)count;
    if (count > (size_t)capacity) {
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    const uintptr_t src = (uintptr_t)input;
    const uintptr_t dst = (uintptr_t)output;
    if ((dst >= src && dst - src < length) ||
        (src > dst && src - dst < count * sizeof(*output))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    convert(code_page, strict, bytes, bytes + length, output);
    return (int32_t)count;
}

size_t nt_runtime_wide_length(const uint16_t *text, size_t limit)
{
    size_t length = 0;
    while (length < limit && text[length] != 0)
        ++length;
    return length;
}

uint16_t *nt_runtime_utf8_to_wide(const char *text)
{
    const int32_t count = nt_MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
    if (count == 0)
        return NULL;
    uint16_t *result = malloc((size_t)count * sizeof(*result));
    if (result != NULL)
        nt_MultiByteToWideChar(CP_UTF8, 0, text, -1, result, count);
    return result;
}

static unsigned char narrow_byte(uint32_t code, uint32_t page, unsigned char fallback, bool *used)
{
    if (code < 0x80)
        return (unsigned char)code;
    if (page == CP_1252 && code >= 0xa0 && code <= 0xff)
        return (unsigned char)code;
    const unsigned int end = page == CP_1252 ? 0xa0 : 256;
    for (unsigned int i = 0x80; i < end; ++i) {
        if (code == (page == CP_1252 ? cp1252((unsigned char)i) : cp437((unsigned char)i)))
            return (unsigned char)i;
    }
    *used = true;
    return fallback;
}

static size_t convert_wide(uint32_t page, bool strict, const uint16_t *input,
                           size_t length, char *output, unsigned char fallback, bool *used)
{
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        uint32_t code = input[i];
        if (code >= 0xd800 && code <= 0xdfff) {
            if (code <= 0xdbff && i + 1 < length && input[i + 1] >= 0xdc00 && input[i + 1] <= 0xdfff)
                code = 0x10000 + ((code - 0xd800) << 10) + (input[++i] - 0xdc00);
            else if (strict)
                return SIZE_MAX;
            else
                code = 0xfffd;
        }
        if (page == CP_UTF8) {
            char bytes[4];
            const size_t size = (size_t)(append_utf8(bytes, code) - bytes);
            if (output != NULL)
                memcpy(output + count, bytes, size);
            count += size;
        } else {
            const unsigned char byte = narrow_byte(code, page, fallback, used);
            if (output != NULL)
                output[count] = (char)byte;
            ++count;
        }
    }
    return count;
}

static void U_CALLCONV count_substitutions(const void *context,
    UConverterFromUnicodeArgs *arguments, const UChar *units, int32_t length,
    UChar32 scalar, UConverterCallbackReason reason, UErrorCode *error)
{
    if (reason == UCNV_UNASSIGNED || reason == UCNV_ILLEGAL || reason == UCNV_IRREGULAR)
        *(bool *)context = true;
    UCNV_FROM_U_CALLBACK_SUBSTITUTE(NULL, arguments, units, length, scalar, reason, error);
}

static int32_t icu_from_wide(uint32_t page, uint32_t flags, const uint16_t *input,
    int32_t input_length, char *output, int32_t capacity,
    const char *default_character, int32_t *used_default)
{
    if (!input || input_length < -1 || input_length == 0 || capacity < 0 ||
        (capacity && !output) || (flags & ~WC_NO_BEST_FIT_CHARS)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    UErrorCode error = U_ZERO_ERROR;
    UConverter *converter = ucnv_open(converter_name(page), &error);
    if (!converter || U_FAILURE(error)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    bool substituted = false;
    ucnv_setFromUCallBack(converter, count_substitutions, &substituted, NULL, NULL, &error);
    if (default_character)
        ucnv_setSubstChars(converter, default_character, 1, &error);
    const int32_t count = input_length == -1 ?
        (int32_t)nt_runtime_wide_length(input, INT32_MAX) + 1 : input_length;
    if (capacity > 0) {
        const uintptr_t src = (uintptr_t)input, dst = (uintptr_t)output;
        if ((dst >= src && dst - src < (size_t)count * sizeof(*input)) ||
            (src > dst && src - dst < (size_t)capacity)) {
            ucnv_close(converter);
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    const int32_t required = U_FAILURE(error) ? 0 :
        ucnv_fromUChars(converter, output, capacity, (const UChar *)input, count, &error);
    ucnv_close(converter);
    if (error == U_BUFFER_OVERFLOW_ERROR && capacity == 0) {
        if (used_default)
            *used_default = substituted;
        return required;
    }
    if (U_FAILURE(error)) {
        nt_SetLastError(error == U_BUFFER_OVERFLOW_ERROR ? NT_ERROR_INSUFFICIENT_BUFFER :
                        NT_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (used_default)
        *used_default = substituted;
    return required;
}

int32_t NT_MS_ABI nt_WideCharToMultiByte(uint32_t code_page, uint32_t flags,
                                       const uint16_t *input, int32_t input_length,
                                       char *output, int32_t capacity,
                                       const char *default_character, int32_t *used_default)
{
    code_page = effective_page(code_page);
    if (converter_name(code_page))
        return icu_from_wide(code_page, flags, input, input_length, output, capacity,
                             default_character, used_default);
    const bool utf8 = code_page == CP_UTF8;
    if (input == NULL || input_length < -1 || input_length == 0 || capacity < 0 ||
        (capacity != 0 && output == NULL) || !nt_IsValidCodePage(code_page) ||
        (utf8 && (default_character != NULL || used_default != NULL))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ~(uint32_t)(utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS)) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_FLAGS);
        return 0;
    }
    const size_t length = input_length == -1 ?
        nt_runtime_wide_length(input, INT32_MAX) + 1 : (size_t)input_length;
    if (length > INT32_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    bool used = false;
    const unsigned char fallback = default_character != NULL ? (unsigned char)*default_character : '?';
    const bool strict = (flags & WC_ERR_INVALID_CHARS) != 0;
    const size_t count = convert_wide(code_page, strict, input, length, NULL, fallback, &used);
    if (count == SIZE_MAX || count > INT32_MAX) {
        nt_SetLastError(NT_ERROR_NO_UNICODE_TRANSLATION);
        return 0;
    }
    if (capacity != 0) {
        if (count > (size_t)capacity) {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        const uintptr_t src = (uintptr_t)input, dst = (uintptr_t)output;
        if ((dst >= src && dst - src < length * sizeof(*input)) ||
            (src > dst && src - dst < count)) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        convert_wide(code_page, strict, input, length, output, fallback, &used);
    }
    if (used_default != NULL)
        *used_default = used;
    return (int32_t)count;
}
