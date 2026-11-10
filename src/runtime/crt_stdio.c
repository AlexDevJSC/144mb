/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <errno.h>
#include <ffi.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_FORMAT = 4096, MAX_ARGUMENTS = 128, MAX_TRANSFER = 1024 * 1024,
    LEGACY_TERMINATION = 1, STANDARD_SNPRINTF = 2, LEGACY_WIDE = 4, STANDARD_ROUNDING = 32
};

union argument {
    void *pointer;
    uint64_t bits;
    int32_t integer;
    double real;
};

struct format_call {
    ffi_type *types[3 + MAX_ARGUMENTS];
    void *values[3 + MAX_ARGUMENTS];
    union argument args[3 + MAX_ARGUMENTS];
    const unsigned char *input;
    unsigned int count, fixed;
    char format[2 * MAX_FORMAT + 1];
};

static bool push(struct format_call *call, ffi_type *type, union argument arg)
{
    if (call->count == call->fixed + MAX_ARGUMENTS)
        return false;
    const unsigned int index = call->count++;
    call->types[index] = type;
    call->args[index] = arg;
    call->values[index] = &call->args[index];
    return true;
}

static bool fetch(struct format_call *call, ffi_type *type, bool bounded)
{
    if (call->input == NULL)
        return false;
    union argument value;
    memcpy(&value, call->input, sizeof(value));
    call->input += sizeof(value);
    if (bounded && (value.integer < -MAX_TRANSFER || value.integer > MAX_TRANSFER))
        return false;
    return push(call, type, value);
}

enum length_kind { NORMAL, BYTE, SHORT, LONG, LONGLONG, SIZE, PTRDIFF, INTMAX, LONGDOUBLE, WIDE };

static enum length_kind read_length(const char **input)
{
    const char *p = *input;
    enum length_kind length = NORMAL;
    switch (*p) {
    case 'h': length = p[1] == 'h' ? BYTE : SHORT; p += p[1] == 'h' ? 2 : 1; break;
    case 'l': length = p[1] == 'l' ? LONGLONG : LONG; p += p[1] == 'l' ? 2 : 1; break;
    case 'I':
        if (strncmp(p, "I32", 3) == 0)
            p += 3;
        else {
            length = LONGLONG;
            p += strncmp(p, "I64", 3) == 0 ? 3 : 1;
        }
        break;
    case 'z': length = SIZE; ++p; break;
    case 't': length = PTRDIFF; ++p; break;
    case 'j': length = INTMAX; ++p; break;
    case 'L': length = LONGDOUBLE; ++p; break;
    case 'w': length = WIDE; ++p; break;
    default: break;
    }
    *input = p;
    return length;
}

static bool digits(const char **input, char **output)
{
    unsigned int number = 0;
    while (**input >= '0' && **input <= '9') {
        number = number * 10 + (unsigned int)(**input - '0');
        if (number > MAX_TRANSFER)
            return false;
        *(*output)++ = *(*input)++;
    }
    return true;
}

static bool normalize(struct format_call *call, const char *format, bool scan)
{
    if (format == NULL || strnlen(format, MAX_FORMAT) == MAX_FORMAT)
        return false;
    const char *input = format;
    char *output = call->format;
    while (*input != '\0') {
        const char token = *input++;
        *output++ = token;
        if (token != '%')
            continue;
        if (*input == '%') {
            *output++ = *input++;
            continue;
        }
        const bool suppress = scan && *input == '*';
        if (suppress)
            *output++ = *input++;
        if (!scan) {
            while (*input != '\0' && strchr("-+ #0", *input) != NULL)
                *output++ = *input++;
            if (*input == '*') {
                if (!fetch(call, &ffi_type_sint32, true))
                    return false;
                *output++ = *input++;
            }
        }
        const char *width = input;
        if (!digits(&input, &output))
            return false;
        const bool has_width = input != width;
        if (!scan && *input == '.') {
            *output++ = *input++;
            if (*input == '*') {
                if (!fetch(call, &ffi_type_sint32, true))
                    return false;
                *output++ = *input++;
            } else if (!digits(&input, &output)) {
                return false;
            }
        }
        const enum length_kind length = read_length(&input);
        const char kind = *input;
        if (kind == '\0')
            return false;
        ++input;
        ffi_type *type = &ffi_type_pointer;
        if (strchr("diuoxX", kind) != NULL || (scan && kind == 'n' && !suppress && !has_width)) {
            const bool signed_value = kind == 'd' || kind == 'i';
            type = signed_value ? &ffi_type_sint32 : &ffi_type_uint32;
            switch (length) {
            case BYTE: *output++ = 'h'; *output++ = 'h'; break;
            case SHORT: *output++ = 'h'; break;
            case NORMAL: case LONG: break; /* Windows long is 32-bit. */
            case LONGLONG: *output++ = 'l'; *output++ = 'l'; break;
            case SIZE: *output++ = 'z'; break;
            case PTRDIFF: *output++ = 't'; break;
            case INTMAX: *output++ = 'j'; break;
            default: return false;
            }
            if (length >= LONGLONG)
                type = signed_value ? &ffi_type_sint64 : &ffi_type_uint64;
        } else if (strchr("aAeEfFgG", kind) != NULL) {
            if (length != NORMAL && length != LONG && length != LONGDOUBLE)
                return false;
            if (scan && length != NORMAL)
                *output++ = 'l'; /* MS long double has the representation of double. */
            type = &ffi_type_double;
        } else if (kind == 'c' || kind == 's' || (scan && kind == '[')) {
            if (length != NORMAL && length != SHORT)
                return false;
            if (scan && kind != 'c' && !has_width)
                return false;
            if (kind == 'c')
                type = &ffi_type_sint32;
        } else if (kind != 'p' || length != NORMAL) {
            return false;
        }
        *output++ = kind;
        if (kind == '[') {
            if (*input == '^')
                *output++ = *input++;
            if (*input == ']')
                *output++ = *input++;
            while (*input != '\0' && *input != ']')
                *output++ = *input++;
            if (*input == '\0')
                return false;
            *output++ = *input++;
        }
        if (!suppress && !fetch(call, scan ? &ffi_type_pointer : type, false))
            return false;
    }
    *output = '\0';
    return true;
}

static pthread_once_t locale_once = PTHREAD_ONCE_INIT;
static locale_t c_locale;

static void initialize_locale(void)
{
    c_locale = newlocale(LC_ALL_MASK, "C", NULL);
}

static int invoke(struct format_call *call, void (*function)(void))
{
    ffi_cif cif;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, call->fixed, call->count,
                         &ffi_type_sint32, call->types) != FFI_OK) {
        errno = EINVAL;
        return -1;
    }
    pthread_once(&locale_once, initialize_locale);
    if (c_locale == (locale_t)0) {
        errno = ENOMEM;
        return -1;
    }
    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0)
        return -1;
    ffi_sarg result;
    ffi_call(&cif, function, &result, call->values);
    const int error = errno;
    uselocale(previous);
    errno = error;
    return (int)result;
}

static char *render(const char *format, const unsigned char *args, int *length)
{
    struct format_call call = {.input = args, .fixed = 3};
    push(&call, &ffi_type_pointer, (union argument){.pointer = NULL});
    push(&call, &ffi_type_uint64, (union argument){.bits = 0});
    push(&call, &ffi_type_pointer, (union argument){.pointer = call.format});
    if (!normalize(&call, format, false)) {
        errno = EINVAL;
        return NULL;
    }
    const int needed = invoke(&call, FFI_FN(snprintf));
    if (needed < 0 || needed > MAX_TRANSFER) {
        if (needed > MAX_TRANSFER)
            errno = E2BIG;
        return NULL;
    }
    char *buffer = malloc((size_t)needed + 1);
    if (buffer == NULL)
        return NULL;
    call.args[0].pointer = buffer;
    call.args[1].bits = (size_t)needed + 1;
    if (invoke(&call, FFI_FN(snprintf)) != needed) {
        free(buffer);
        errno = EINVAL;
        return NULL;
    }
    *length = needed;
    return buffer;
}

int NT_MS_ABI nt_stdio_vsprintf(uint64_t options, char *output, size_t length,
                                const char *format, void *locale, const unsigned char *args)
{
    const uint64_t supported = LEGACY_TERMINATION | STANDARD_SNPRINTF | LEGACY_WIDE | STANDARD_ROUNDING;
    if (locale != NULL || (options & ~supported) != 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (output == NULL && length != 0) {
        errno = EINVAL;
        return -1;
    }
    int needed;
    char *rendered = render(format, args, &needed);
    if (rendered == NULL)
        return -1;
    const bool standard = (options & STANDARD_SNPRINTF) != 0;
    const bool legacy = !standard && (options & LEGACY_TERMINATION) != 0;
    const size_t bytes = (size_t)needed;
    if (length != 0) {
        const size_t limit = legacy ? length : length - 1;
        const size_t copied = bytes < limit ? bytes : limit;
        memcpy(output, rendered, copied);
        if (copied < length)
            output[copied] = '\0';
    }
    free(rendered);
    return output == NULL || standard || (legacy ? bytes <= length : bytes < length) ? needed : -1;
}

int NT_MS_ABI nt_stdio_vfprintf(uint64_t options, void *stream, const char *format,
                                void *locale, const unsigned char *args)
{
    if (locale != NULL || (options & ~(uint64_t)(LEGACY_WIDE | STANDARD_ROUNDING)) != 0) {
        errno = ENOTSUP;
        return -1;
    }
    int length;
    char *rendered = render(format, args, &length);
    if (rendered == NULL)
        return -1;
    const bool written = nt_fwrite(rendered, 1, (size_t)length, stream) == (size_t)length;
    free(rendered);
    return written ? length : -1;
}

int NT_MS_ABI nt_stdio_vsscanf(uint64_t options, const char *buffer, size_t length,
                               const char *format, void *locale, const unsigned char *args)
{
    if (locale != NULL || (options & ~UINT64_C(2)) != 0) {
        errno = ENOTSUP;
        return -1;
    }
    if (buffer == NULL) {
        errno = EINVAL;
        return -1;
    }
    const size_t limit = length > MAX_TRANSFER ? MAX_TRANSFER + 1 : length;
    const size_t bytes = strnlen(buffer, limit);
    if (bytes > MAX_TRANSFER) {
        errno = E2BIG;
        return -1;
    }
    char *copy = malloc(bytes + 1);
    if (copy == NULL)
        return -1;
    memcpy(copy, buffer, bytes);
    copy[bytes] = '\0';
    struct format_call call = {.input = args, .fixed = 2};
    push(&call, &ffi_type_pointer, (union argument){.pointer = copy});
    push(&call, &ffi_type_pointer, (union argument){.pointer = call.format});
    int result = -1;
    if (normalize(&call, format, true))
        result = invoke(&call, FFI_FN(sscanf));
    else
        errno = EINVAL;
    free(copy);
    return result;
}
