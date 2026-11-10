/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    GMEM_MOVEABLE = 0x0002,
    GMEM_ZEROINIT = 0x0040,
    GMEM_DISCARDABLE = 0x0100,
    GMEM_DDESHARE = 0x2000,
    FORMAT_ALLOCATE = 0x0100,
    FORMAT_IGNORE_INSERTS = 0x0200,
    FORMAT_FROM_STRING = 0x0400,
    FORMAT_FROM_SYSTEM = 0x1000,
    FORMAT_MAX_MESSAGE = 128 * 1024
};

enum memory_kind { GLOBAL_FIXED, GLOBAL_MOVABLE, LOCAL_FIXED };

struct memory_object {
    struct memory_object *next;
    uintptr_t handle;
    void *data;
    unsigned int locks;
    enum memory_kind kind;
};

static pthread_mutex_t objects_lock = PTHREAD_MUTEX_INITIALIZER;
static struct memory_object *objects;
static uintptr_t next_movable = 1;

static struct memory_object **find_object(uintptr_t handle)
{
    struct memory_object **link = &objects;
    while (*link != NULL && (*link)->handle != handle)
        link = &(*link)->next;
    return link;
}

static uintptr_t allocate_object(enum memory_kind kind, size_t size, bool zero)
{
    struct memory_object *object = malloc(sizeof(*object));
    const bool discarded = kind == GLOBAL_MOVABLE && size == 0;
    const size_t bytes = size == 0 ? 1 : size;
    void *data = discarded ? NULL : (zero ? calloc(1, bytes) : malloc(bytes));
    if (object == NULL || (!discarded && data == NULL)) {
        free(object);
        free(data);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    pthread_mutex_lock(&objects_lock);
    if (kind == GLOBAL_MOVABLE && next_movable > (UINTPTR_MAX >> 4)) {
        pthread_mutex_unlock(&objects_lock);
        free(data);
        free(object);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const uintptr_t handle = kind == GLOBAL_MOVABLE ?
                             (next_movable++ << 4) | 1 : (uintptr_t)data;
    *object = (struct memory_object){objects, handle, data, 0, kind};
    objects = object;
    pthread_mutex_unlock(&objects_lock);
    return handle;
}

uintptr_t NT_MS_ABI nt_GlobalAlloc(uint32_t flags, size_t size)
{
    const uint32_t supported = GMEM_MOVEABLE | GMEM_ZEROINIT | GMEM_DISCARDABLE | GMEM_DDESHARE;
    if ((flags & ~supported) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return allocate_object((flags & GMEM_MOVEABLE) != 0 ? GLOBAL_MOVABLE : GLOBAL_FIXED,
                           size, (flags & GMEM_ZEROINIT) != 0);
}

static uintptr_t release_object(uintptr_t handle, bool local)
{
    if (handle == 0)
        return 0;
    pthread_mutex_lock(&objects_lock);
    struct memory_object **link = find_object(handle);
    struct memory_object *object = *link;
    if (object == NULL || (object->kind == LOCAL_FIXED) != local) {
        pthread_mutex_unlock(&objects_lock);
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return handle;
    }
    *link = object->next;
    pthread_mutex_unlock(&objects_lock);
    free(object->data);
    free(object);
    return 0;
}

uintptr_t NT_MS_ABI nt_GlobalFree(uintptr_t handle)
{
    return release_object(handle, false);
}

uintptr_t NT_MS_ABI nt_LocalFree(uintptr_t handle)
{
    return release_object(handle, true);
}

void *NT_MS_ABI nt_GlobalLock(uintptr_t handle)
{
    pthread_mutex_lock(&objects_lock);
    struct memory_object *object = *find_object(handle);
    uint32_t error = NT_ERROR_SUCCESS;
    void *data = NULL;
    if (object == NULL || object->kind == LOCAL_FIXED)
        error = NT_ERROR_INVALID_HANDLE;
    else if (object->data == NULL)
        error = NT_ERROR_DISCARDED;
    else if (object->locks == UINT_MAX)
        error = NT_ERROR_LOCK_FAILED;
    else {
        if (object->kind == GLOBAL_MOVABLE)
            ++object->locks;
        data = object->data;
    }
    pthread_mutex_unlock(&objects_lock);
    if (error != NT_ERROR_SUCCESS)
        nt_SetLastError(error);
    return data;
}

int32_t NT_MS_ABI nt_GlobalUnlock(uintptr_t handle)
{
    pthread_mutex_lock(&objects_lock);
    struct memory_object *object = *find_object(handle);
    uint32_t error = NT_ERROR_SUCCESS;
    bool locked = false;
    if (object == NULL || object->kind == LOCAL_FIXED)
        error = NT_ERROR_INVALID_HANDLE;
    else if (object->kind == GLOBAL_FIXED)
        locked = true;
    else if (object->locks == 0)
        error = NT_ERROR_NOT_LOCKED;
    else
        locked = --object->locks != 0;
    pthread_mutex_unlock(&objects_lock);
    if (!locked)
        nt_SetLastError(error);
    return locked;
}

static const char *system_message(uint32_t code)
{
    static const struct {
        uint32_t code;
        const char *message;
    } messages[] = {
        {NT_ERROR_SUCCESS, "The operation completed successfully.\r\n"},
        {NT_ERROR_FILE_NOT_FOUND, "The system cannot find the file specified.\r\n"},
        {NT_ERROR_PATH_NOT_FOUND, "The system cannot find the path specified.\r\n"},
        {NT_ERROR_ACCESS_DENIED, "Access is denied.\r\n"},
        {NT_ERROR_INVALID_HANDLE, "The handle is invalid.\r\n"},
        {NT_ERROR_NOT_ENOUGH_MEMORY, "Not enough memory resources are available to process this command.\r\n"},
        {NT_ERROR_INVALID_PARAMETER, "The parameter is incorrect.\r\n"},
        {NT_ERROR_INSUFFICIENT_BUFFER, "The data area passed to a system call is too small.\r\n"},
        {NT_ERROR_MOD_NOT_FOUND, "The specified module could not be found.\r\n"},
        {NT_ERROR_PROC_NOT_FOUND, "The specified procedure could not be found.\r\n"},
        {NT_ERROR_DIR_NOT_EMPTY, "The directory is not empty.\r\n"},
        {NT_ERROR_ALREADY_EXISTS, "Cannot create a file when that file already exists.\r\n"}
    };
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
        if (messages[i].code == code)
            return messages[i].message;
    }
    return NULL;
}

uint32_t NT_MS_ABI nt_FormatMessageA(uint32_t flags, const void *source,
                                    uint32_t code, uint32_t language,
                                    char *buffer, uint32_t size, void *arguments)
{
    (void)arguments;
    const uint32_t supported = FORMAT_ALLOCATE | FORMAT_IGNORE_INSERTS |
                               FORMAT_FROM_STRING | FORMAT_FROM_SYSTEM;
    const bool from_string = (flags & FORMAT_FROM_STRING) != 0;
    const bool from_system = (flags & FORMAT_FROM_SYSTEM) != 0;
    if (buffer == NULL || from_string == from_system || (flags & ~supported) != 0 ||
        (from_string && (source == NULL || (flags & FORMAT_IGNORE_INSERTS) == 0))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const char *message = from_string ? source : system_message(code);
    if (message == NULL || (from_system && language != 0 && language != 0x0409)) {
        nt_SetLastError(NT_ERROR_MR_MID_NOT_FOUND);
        return 0;
    }
    const size_t length = strnlen(message, FORMAT_MAX_MESSAGE);
    if (length == FORMAT_MAX_MESSAGE || ((flags & FORMAT_ALLOCATE) != 0 &&
                                         size > FORMAT_MAX_MESSAGE)) {
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (from_string && strchr(message, '%') != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if ((flags & FORMAT_ALLOCATE) != 0) {
        const size_t bytes = size > length + 1 ? size : length + 1;
        const uintptr_t handle = allocate_object(LOCAL_FIXED, bytes, false);
        if (handle == 0)
            return 0;
        char *allocated = (char *)handle;
        memcpy(allocated, message, length + 1);
        memcpy(buffer, &allocated, sizeof(allocated));
    } else {
        if (size <= length) {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memmove(buffer, message, length + 1);
    }
    return (uint32_t)length;
}
