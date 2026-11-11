/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "winbase.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { MAX_ENV_ENTRIES = 4096, MAX_ENV_UNITS = 32767 };
static const char *const *environment_source;
static char **variables;
static size_t count;
static pthread_mutex_t environment_lock = PTHREAD_MUTEX_INITIALIZER;

void nt_runtime_set_win_environment(const char *const *source)
{
    pthread_mutex_lock(&environment_lock);
    for (size_t i = 0; i < count; ++i)
        free(variables[i]);
    free(variables);
    variables = NULL;
    count = 0;
    environment_source = source;
    pthread_mutex_unlock(&environment_lock);
}

static bool prepare_environment(void)
{
    if (variables != NULL)
        return true;
    variables = calloc(MAX_ENV_ENTRIES, sizeof(*variables));
    if (variables == NULL)
        return false;
    if (environment_source != NULL) {
        for (size_t i = 0; environment_source[i] != NULL; ++i) {
            if (count == MAX_ENV_ENTRIES)
                break;
            if (strchr(environment_source[i], '=') == NULL)
                continue;
            char *entry = strdup(environment_source[i]);
            if (entry == NULL) {
                while (count != 0)
                    free(variables[--count]);
                free(variables);
                variables = NULL;
                return false;
            }
            variables[count++] = entry;
        }
    }
    return true;
}

static bool valid_name(const char *name)
{
    return name != NULL && *name != '\0' && strchr(name, '=') == NULL;
}

static size_t locate(const char *name, size_t length)
{
    for (size_t i = 0; i < count; ++i) {
        if (strncasecmp(variables[i], name, length) == 0 && variables[i][length] == '=')
            return i;
    }
    return count;
}

static char *copy_value(const char *name)
{
    if (!valid_name(name)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const size_t length = strlen(name);
    pthread_mutex_lock(&environment_lock);
    char *value = NULL;
    if (!prepare_environment()) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    } else {
        const size_t index = locate(name, length);
        if (index == count)
            nt_SetLastError(NT_ERROR_ENVVAR_NOT_FOUND);
        else if ((value = strdup(variables[index] + length + 1)) == NULL)
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    }
    pthread_mutex_unlock(&environment_lock);
    return value;
}

static char *utf8_string(const uint16_t *value)
{
    if (value == NULL)
        return NULL;
    const size_t units = nt_runtime_wide_length(value, MAX_ENV_UNITS);
    if (units == MAX_ENV_UNITS) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const int32_t bytes = nt_WideCharToMultiByte(65001, 0x80, value, (int32_t)units + 1,
                                                NULL, 0, NULL, NULL);
    if (bytes == 0)
        return NULL;
    char *text = malloc((size_t)bytes);
    if (text == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    nt_WideCharToMultiByte(65001, 0x80, value, (int32_t)units + 1, text, bytes, NULL, NULL);
    return text;
}

uint32_t NT_MS_ABI nt_GetEnvironmentVariableW(const uint16_t *name, uint16_t *buffer, uint32_t size)
{
    if (name == NULL || (size != 0 && buffer == NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *key = utf8_string(name);
    if (key == NULL)
        return 0;
    char *value = copy_value(key);
    free(key);
    if (value == NULL)
        return 0;
    const int32_t needed = nt_MultiByteToWideChar(65001, 0, value, -1, NULL, 0);
    const bool fits = needed > 0 && (uint32_t)needed <= size;
    if (fits)
        nt_MultiByteToWideChar(65001, 0, value, -1, buffer, needed);
    free(value);
    if (fits)
        nt_SetLastError(NT_ERROR_SUCCESS);
    return fits ? (uint32_t)needed - 1 : (uint32_t)needed;
}

uint32_t NT_MS_ABI nt_GetEnvironmentVariableA(const char *name, char *buffer, uint32_t size)
{
    if (name == NULL || (size != 0 && buffer == NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const size_t length = strnlen(name, MAX_ENV_UNITS);
    char *key = length < MAX_ENV_UNITS ? nt_runtime_ansi_to_utf8(name, length) : NULL;
    if (key == NULL) {
        nt_SetLastError(length == MAX_ENV_UNITS ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    char *value = copy_value(key);
    free(key);
    if (value == NULL)
        return 0;
    uint16_t *wide = nt_runtime_utf8_to_wide(value);
    free(value);
    if (wide == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const int32_t needed = nt_WideCharToMultiByte(1252, 0, wide, -1, NULL, 0, NULL, NULL);
    const bool fits = needed > 0 && (uint32_t)needed <= size;
    if (fits)
        nt_WideCharToMultiByte(1252, 0, wide, -1, buffer, needed, NULL, NULL);
    free(wide);
    if (fits)
        nt_SetLastError(NT_ERROR_SUCCESS);
    return fits ? (uint32_t)needed - 1 : (uint32_t)needed;
}

int32_t NT_MS_ABI nt_SetEnvironmentVariableW(const uint16_t *name, const uint16_t *value)
{
    if (name == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *key = utf8_string(name);
    if (key == NULL)
        return 0;
    if (!valid_name(key)) {
        free(key);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char *text = value != NULL ? utf8_string(value) : NULL;
    if (value != NULL && text == NULL) {
        free(key);
        return 0;
    }
    const size_t length = strlen(key);
    char *entry = NULL;
    if (text != NULL) {
        const size_t bytes = strlen(text) + 1;
        entry = malloc(length + 1 + bytes);
        if (entry != NULL) {
            memcpy(entry, key, length);
            entry[length] = '=';
            memcpy(entry + length + 1, text, bytes);
        }
        free(text);
        if (entry == NULL) {
            free(key);
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
    }
    int32_t ok = 0;
    pthread_mutex_lock(&environment_lock);
    if (!prepare_environment()) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    } else {
        const size_t index = locate(key, length);
        if (index < count) {
            free(variables[index]);
            if (entry != NULL)
                variables[index] = entry;
            else {
                --count;
                memmove(variables + index, variables + index + 1,
                        (count - index) * sizeof(*variables));
            }
            ok = 1;
        } else if (entry == NULL) {
            nt_SetLastError(NT_ERROR_ENVVAR_NOT_FOUND);
        } else if (count < MAX_ENV_ENTRIES) {
            variables[count++] = entry;
            ok = 1;
        } else {
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        }
    }
    pthread_mutex_unlock(&environment_lock);
    free(key);
    if (!ok)
        free(entry);
    return ok;
}

uint16_t *NT_MS_ABI nt_GetEnvironmentStringsW(void)
{
    pthread_mutex_lock(&environment_lock);
    uint16_t *block = NULL;
    if (prepare_environment()) {
        size_t units = count != 0 ? 1 : 2;
        for (size_t i = 0; i < count; ++i)
            units += (size_t)nt_MultiByteToWideChar(65001, 0, variables[i], -1, NULL, 0);
        block = (uint16_t *)nt_LocalAlloc(0x40, units * sizeof(*block));
        if (block != NULL) {
            size_t at = 0;
            for (size_t i = 0; i < count; ++i)
                at += (size_t)nt_MultiByteToWideChar(65001, 0, variables[i], -1,
                                                     block + at, (int32_t)(units - at));
        }
    } else {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    }
    pthread_mutex_unlock(&environment_lock);
    return block;
}

int32_t NT_MS_ABI nt_FreeEnvironmentStringsW(uint16_t *block)
{
    return block != NULL && nt_LocalFree((uintptr_t)block) == 0;
}

static uint32_t wide_path_result(const uint16_t *path, uint16_t *buffer, uint32_t capacity)
{
    const size_t length = nt_runtime_wide_length(path, MAX_ENV_UNITS);
    if (capacity <= length)
        return (uint32_t)length + 1;
    if (buffer == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(buffer, path, (length + 1) * sizeof(*buffer));
    return (uint32_t)length;
}

uint32_t NT_MS_ABI nt_GetSystemDirectoryW(uint16_t *buffer, uint32_t capacity)
{
    /* The virtual system module namespace is under this DOS directory. */
    static const uint16_t path[] = u"C:\\Windows\\System32";
    return wide_path_result(path, buffer, capacity);
}

uint32_t NT_MS_ABI nt_GetTempPathW(uint32_t capacity, uint16_t *buffer)
{
    const char *const names[] = {"TMP", "TEMP", "USERPROFILE"};
    char *value = NULL;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        value = copy_value(names[i]);
        if (value != NULL && *value != '\0')
            break;
        free(value);
        value = NULL;
    }
    if (value == NULL)
        return wide_path_result(u"C:\\Windows\\", buffer, capacity);
    size_t length = strlen(value);
    const bool trailing = value[length - 1] == '\\' || value[length - 1] == '/';
    char *path = realloc(value, length + 2);
    if (path == NULL) {
        free(value);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (!trailing)
        path[length++] = '\\';
    path[length] = '\0';
    uint16_t *wide = nt_runtime_utf8_to_wide(path);
    free(path);
    if (wide == NULL)
        return 0;
    const uint32_t result = wide_path_result(wide, buffer, capacity);
    free(wide);
    return result;
}
