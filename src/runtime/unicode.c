/* SPDX-License-Identifier: MIT */
#include "compat.h"

#include <stdlib.h>
#include <string.h>

enum { CP_ACP = 0, CP_1252 = 1252, CP_UTF8 = 65001, MB_PRECOMPOSED = 1, MB_ERR_INVALID_CHARS = 8 };

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

static size_t convert(bool utf8, bool strict, const unsigned char *cursor,
                      const unsigned char *end, uint16_t *output)
{
    size_t count = 0;
    while (cursor < end) {
        bool valid = true;
        uint32_t code = utf8 ? utf8_next(&cursor, end, &valid) : cp1252(*cursor++);
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

int32_t NT_MS_ABI nt_MultiByteToWideChar(uint32_t code_page, uint32_t flags,
                                      const char *input, int32_t input_length,
                                      uint16_t *output, int32_t capacity)
{
    const bool utf8 = code_page == CP_UTF8;
    if (input == NULL || input_length < -1 || input_length == 0 || capacity < 0 ||
        (capacity != 0 && output == NULL) ||
        (!utf8 && code_page != CP_ACP && code_page != CP_1252)) {
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
    const size_t count = convert(utf8, strict, bytes, bytes + length, NULL);
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
    convert(utf8, strict, bytes, bytes + length, output);
    return (int32_t)count;
}
