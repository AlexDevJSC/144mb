/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "compat.h"

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static pthread_once_t c_locale_once = PTHREAD_ONCE_INIT;
static locale_t c_locale;
static _Thread_local int thread_locale_mode = 2;
static _Atomic int new_mode;
static _Atomic int app_type;
static _Atomic(nt_matherr_callback) math_error;
static _Atomic(const char *const *) guest_environment;
static pthread_mutex_t environment_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *const *environment_source;
static char **environment_copy;

static void create_c_locale(void)
{
    c_locale = newlocale(LC_ALL_MASK, "C", NULL);
}

int NT_MS_ABI nt_atoi(const char *text)
{
    pthread_once(&c_locale_once, create_c_locale);
    if (c_locale == (locale_t)0) {
        errno = ENOMEM;
        return 0;
    }
    const long value = strtol_l(text, NULL, 10, c_locale);
    if (value > INT32_MAX || value < INT32_MIN) {
        errno = ERANGE;
        return value > 0 ? INT32_MAX : INT32_MIN;
    }
    return (int)value;
}

double NT_MS_ABI nt_atof(const char *text)
{
    pthread_once(&c_locale_once, create_c_locale);
    if (c_locale == (locale_t)0) {
        errno = ENOMEM;
        return 0;
    }
    return strtod_l(text, NULL, c_locale);
}

int NT_MS_ABI nt_isspace(int byte)
{
    return byte == ' ' || (byte >= '\t' && byte <= '\r');
}

static int lower_ascii(int byte)
{
    return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
}

int NT_MS_ABI nt_tolower(int byte)
{
    return lower_ascii(byte);
}

int NT_MS_ABI nt_toupper(int byte)
{
    return byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
}

int NT_MS_ABI nt_configthreadlocale(int mode)
{
    if (mode < 0 || mode > 2) {
        errno = EINVAL;
        return -1;
    }
    const int previous = thread_locale_mode;
    if (mode != 0)
        thread_locale_mode = mode;
    return previous;
}

int NT_MS_ABI nt_set_new_mode(int mode)
{
    if (mode != 0 && mode != 1) {
        errno = EINVAL;
        return -1;
    }
    return atomic_exchange(&new_mode, mode);
}

void NT_MS_ABI nt_set_app_type(int type)
{
    if (type < 0 || type > 2) {
        errno = EINVAL;
        return;
    }
    atomic_store(&app_type, type);
}

static char *assertion_text(const uint16_t *text)
{
    if (text == NULL)
        return NULL;
    size_t length = 0;
    while (length < 2048 && text[length] != 0)
        ++length;
    return nt_runtime_utf16_to_utf8(text, length);
}

void NT_MS_ABI nt_wassert(const uint16_t *expression, const uint16_t *file, uint32_t line)
{
    char *expr = assertion_text(expression), *path = assertion_text(file);
    char message[8192];
    const int length = snprintf(message, sizeof(message), "Assertion failed: %s, file %s, line %u\n",
                                expr != NULL ? expr : "?", path != NULL ? path : "?", line);
    free(expr);
    free(path);
    if (length > 0 && !(atomic_load(&app_type) == 2 && nt_runtime_show_assertion(message))) {
        const size_t size = (size_t)length < sizeof(message) ? (size_t)length : sizeof(message) - 1;
        nt_fwrite(message, 1, size, nt_acrt_iob_func(2));
        nt_runtime_flush_streams();
    }
    nt_terminate();
}

void NT_MS_ABI nt_setusermatherr(nt_matherr_callback callback)
{
    atomic_store(&math_error, callback);
}

double nt_runtime_sqrt(double number)
{
    const int previous = errno;
    double result = sqrt(number);
    if (number < 0) {
        const nt_matherr_callback callback = atomic_load(&math_error);
        if (callback != NULL) {
            struct nt_math_exception exception = {1, "sqrt", number, 0, result};
            errno = previous;
            const bool handled = callback(&exception) != 0;
            result = exception.result;
            errno = handled ? previous : EDOM;
        }
    }
    return result;
}

void nt_runtime_set_guest_environment(const char *const *environment)
{
    pthread_mutex_lock(&environment_lock);
    if (environment_copy != NULL) {
        for (size_t i = 0; environment_copy[i] != NULL; ++i)
            free(environment_copy[i]);
        free(environment_copy);
        environment_copy = NULL;
    }
    environment_source = environment;
    atomic_store(&guest_environment, environment);
    pthread_mutex_unlock(&environment_lock);
}

int NT_MS_ABI nt_initialize_narrow_environment(void)
{
    enum { MAX_ENTRIES = 4096, MAX_ENV_BYTES = 128 * 1024 };
    pthread_mutex_lock(&environment_lock);
    if (environment_copy != NULL) {
        pthread_mutex_unlock(&environment_lock);
        return 0;
    }
    size_t count = 0, bytes = 0;
    while (environment_source != NULL && environment_source[count] != NULL) {
        const size_t length = strnlen(environment_source[count], MAX_ENV_BYTES);
        if (count == MAX_ENTRIES || length >= MAX_ENV_BYTES - bytes) {
            pthread_mutex_unlock(&environment_lock);
            errno = ENOMEM;
            return -1;
        }
        bytes += length + 1;
        ++count;
    }
    char **copy = calloc(count + 1, sizeof(*copy));
    if (copy == NULL) {
        pthread_mutex_unlock(&environment_lock);
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        copy[i] = strdup(environment_source[i]);
        if (copy[i] == NULL) {
            while (i != 0)
                free(copy[--i]);
            free(copy);
            pthread_mutex_unlock(&environment_lock);
            errno = ENOMEM;
            return -1;
        }
    }
    environment_copy = copy;
    atomic_store(&guest_environment, (const char *const *)copy);
    pthread_mutex_unlock(&environment_lock);
    return 0;
}

char *NT_MS_ABI nt_getenv(const char *name)
{
    const char *const *environment = atomic_load(&guest_environment);
    if (name == NULL || *name == '\0' || strchr(name, '=') != NULL) {
        errno = EINVAL;
        return NULL;
    }
    if (environment == NULL)
        return NULL;
    for (size_t i = 0; environment[i] != NULL; ++i) {
        const char *entry = environment[i];
        size_t offset = 0;
        while (name[offset] != '\0' &&
               lower_ascii((unsigned char)entry[offset]) == lower_ascii((unsigned char)name[offset]))
            ++offset;
        if (name[offset] == '\0' && entry[offset] == '=')
            return (char *)(entry + offset + 1);
    }
    return NULL;
}

void NT_MS_ABI nt_initterm(const nt_void_callback *first, const nt_void_callback *last)
{
    for (const nt_void_callback *entry = first; entry != last; ++entry) {
        if (*entry != NULL)
            (*entry)();
    }
}

int NT_MS_ABI nt_initterm_e(const nt_int_callback *first, const nt_int_callback *last)
{
    for (const nt_int_callback *entry = first; entry != last; ++entry) {
        if (*entry != NULL) {
            const int result = (*entry)();
            if (result != 0)
                return result;
        }
    }
    return 0;
}

enum { MAX_CALLBACKS = 4096 };
_Static_assert(sizeof(struct nt_onexit_table) == 24, "on-exit table layout");

static pthread_mutex_t callback_lock = PTHREAD_MUTEX_INITIALIZER;
static struct nt_onexit_table exit_callbacks;
static bool exit_running;
static nt_tls_exit_callback tls_exit;

int NT_MS_ABI nt_initialize_onexit_table(struct nt_onexit_table *table)
{
    if (table == NULL) {
        errno = EINVAL;
        return -1;
    }
    *table = (struct nt_onexit_table){0};
    return 0;
}

static int append_callback(struct nt_onexit_table *table, nt_guest_proc callback)
{
    if (table == NULL || callback == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&callback_lock);
    const size_t used = table->first != NULL ? (size_t)(table->last - table->first) : 0;
    const size_t capacity = table->first != NULL ? (size_t)(table->end - table->first) : 0;
    if (used == MAX_CALLBACKS) {
        pthread_mutex_unlock(&callback_lock);
        errno = ENOMEM;
        return -1;
    }
    if (used == capacity) {
        const size_t next = capacity == 0 ? 8 : capacity * 2;
        nt_guest_proc *entries = realloc(table->first, next * sizeof(*entries));
        if (entries == NULL) {
            pthread_mutex_unlock(&callback_lock);
            return -1;
        }
        *table = (struct nt_onexit_table){entries, entries + used, entries + next};
    }
    *table->last++ = callback;
    pthread_mutex_unlock(&callback_lock);
    return 0;
}

int NT_MS_ABI nt_register_onexit_function(struct nt_onexit_table *table,
                                         nt_int_callback callback)
{
    return append_callback(table, (nt_guest_proc)callback);
}

int NT_MS_ABI nt_crt_atexit(nt_void_callback callback)
{
    return append_callback(&exit_callbacks, (nt_guest_proc)callback);
}

void NT_MS_ABI nt_register_thread_local_exe_atexit_callback(nt_tls_exit_callback callback)
{
    pthread_mutex_lock(&callback_lock);
    if (callback == NULL || tls_exit != NULL || exit_running)
        errno = EINVAL;
    else
        tls_exit = callback;
    pthread_mutex_unlock(&callback_lock);
}

static void release_table(struct nt_onexit_table *table)
{
    free(table->first);
    *table = (struct nt_onexit_table){0};
}

void nt_runtime_release_onexit_table(struct nt_onexit_table *table)
{
    pthread_mutex_lock(&callback_lock);
    release_table(table);
    pthread_mutex_unlock(&callback_lock);
}

void NT_MS_ABI nt_cexit(void)
{
    pthread_mutex_lock(&callback_lock);
    if (exit_running) {
        pthread_mutex_unlock(&callback_lock);
        return;
    }
    exit_running = true;
    if (tls_exit != NULL) {
        const nt_tls_exit_callback callback = tls_exit;
        tls_exit = NULL;
        pthread_mutex_unlock(&callback_lock);
        callback(NULL, 0, NULL); /* DLL_PROCESS_DETACH */
        pthread_mutex_lock(&callback_lock);
    }
    while (exit_callbacks.last != exit_callbacks.first) {
        const nt_void_callback callback = (nt_void_callback)*--exit_callbacks.last;
        pthread_mutex_unlock(&callback_lock);
        callback();
        pthread_mutex_lock(&callback_lock);
    }
    release_table(&exit_callbacks);
    exit_running = false;
    pthread_mutex_unlock(&callback_lock);
    nt_runtime_flush_streams();
}
