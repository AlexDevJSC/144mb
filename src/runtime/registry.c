/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "registry.h"
#include "winbase.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAX_KEYS = 128, MAX_VALUE = 64 * 1024, REG_SZ = 1, REG_BINARY = 3, REG_DWORD = 4 };
struct key { uintptr_t handle; char *path; uint32_t access; };
static struct key keys[MAX_KEYS];
static uintptr_t next_handle = 0x7a000001;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *predefined(uintptr_t handle)
{
    if (handle == UINT32_C(0x80000001) || handle == (uintptr_t)(int32_t)UINT32_C(0x80000001))
        return "C:\\Registry\\HKCU";
    if (handle == UINT32_C(0x80000002) || handle == (uintptr_t)(int32_t)UINT32_C(0x80000002))
        return "C:\\Registry\\HKLM";
    return NULL;
}

static struct key *find_key(uintptr_t handle)
{
    for (size_t i = 0; i < MAX_KEYS; ++i) {
        if (keys[i].handle == handle)
            return &keys[i];
    }
    return NULL;
}

static int32_t copy_key(uintptr_t handle, uint32_t access, char path[PATH_MAX])
{
    const char *root = predefined(handle);
    if (root != NULL) {
        strcpy(path, root);
        return NT_ERROR_SUCCESS;
    }
    pthread_mutex_lock(&registry_lock);
    const struct key *key = handle != 0 ? find_key(handle) : NULL;
    int32_t status = key == NULL ? NT_ERROR_INVALID_HANDLE :
                     (access & ~key->access) != 0 ? NT_ERROR_ACCESS_DENIED : NT_ERROR_SUCCESS;
    if (status == NT_ERROR_SUCCESS)
        strcpy(path, key->path);
    pthread_mutex_unlock(&registry_lock);
    return status;
}

int32_t NT_MS_ABI nt_RegOpenKeyExW(uintptr_t base, const uint16_t *subkey, uint32_t options,
                                   uint32_t access, uintptr_t *result)
{
    if (result == NULL || options != 0)
        return NT_ERROR_INVALID_PARAMETER;
    /* Read-only guest hive. WOW64 view flags have no effect in this AMD64 runtime. */
    const uint32_t allowed = 0x20319;
    if ((access & ~allowed) != 0 || (access & 0x300) == 0x300)
        return NT_ERROR_ACCESS_DENIED;
    const uint32_t effective = access & ~0x300u;
    char path[PATH_MAX], canonical[PATH_MAX];
    const int32_t status = copy_key(base, effective, path);
    if (status != NT_ERROR_SUCCESS)
        return status;
    if (subkey != NULL && *subkey != 0) {
        const size_t prefix = strlen(path);
        if (prefix >= sizeof(path) - 2)
            return NT_ERROR_INVALID_NAME;
        path[prefix] = '\\';
        if (nt_runtime_wide_length(subkey, PATH_MAX) == PATH_MAX ||
            nt_WideCharToMultiByte(65001, 0x80, subkey, -1, path + prefix + 1,
                                   (int32_t)(PATH_MAX - prefix - 1), NULL, NULL) == 0)
            return NT_ERROR_INVALID_PARAMETER;
    } else if (predefined(base) != NULL) {
        *result = base;
        return NT_ERROR_SUCCESS;
    }
    const int directory = nt_runtime_open_guest_directory(path, canonical, sizeof(canonical));
    if (directory < 0)
        return errno == ENOENT || errno == ENOTDIR ? NT_ERROR_FILE_NOT_FOUND : NT_ERROR_ACCESS_DENIED;
    close(directory);
    char *name = strdup(canonical);
    if (name == NULL)
        return NT_ERROR_NOT_ENOUGH_MEMORY;
    pthread_mutex_lock(&registry_lock);
    struct key *entry = find_key(0);
    const bool ok = entry != NULL && next_handle != UINTPTR_MAX;
    if (ok) {
        *entry = (struct key){next_handle++, name, effective};
        *result = entry->handle;
    }
    pthread_mutex_unlock(&registry_lock);
    if (!ok)
        free(name);
    return ok ? NT_ERROR_SUCCESS : NT_ERROR_NOT_ENOUGH_MEMORY;
}

int32_t NT_MS_ABI nt_RegCloseKey(uintptr_t handle)
{
    if (predefined(handle) != NULL)
        return NT_ERROR_SUCCESS;
    pthread_mutex_lock(&registry_lock);
    struct key *entry = handle != 0 ? find_key(handle) : NULL;
    if (entry != NULL) {
        free(entry->path);
        *entry = (struct key){0};
    }
    pthread_mutex_unlock(&registry_lock);
    return entry != NULL ? NT_ERROR_SUCCESS : NT_ERROR_INVALID_HANDLE;
}

static int read_value(const char *path, unsigned char content[MAX_VALUE + 1], uint32_t *length)
{
    const int fd = nt_runtime_open_guest_file_readonly(path);
    if (fd < 0)
        return errno;
    size_t total = 0;
    int error = 0;
    while (total < MAX_VALUE + 1) {
        const ssize_t count = read(fd, content + total, MAX_VALUE + 1 - total);
        if (count > 0) {
            total += (size_t)count;
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            error = errno;
            break;
        }
    }
    close(fd);
    if (total > MAX_VALUE)
        return EFBIG;
    content[total] = 0;
    *length = (uint32_t)total;
    return error;
}

int32_t NT_MS_ABI nt_RegQueryValueExW(uintptr_t handle, const uint16_t *name, uint32_t *reserved,
                                      uint32_t *type, unsigned char *data, uint32_t *size)
{
    if (reserved != NULL || (data != NULL && size == NULL))
        return NT_ERROR_INVALID_PARAMETER;
    char path[PATH_MAX], leaf[512] = "";
    const int32_t status = copy_key(handle, 1, path);
    if (status != NT_ERROR_SUCCESS)
        return status;
    if (name != NULL && (nt_runtime_wide_length(name, 512) == 512 ||
        nt_WideCharToMultiByte(65001, 0x80, name, -1, leaf, sizeof(leaf), NULL, NULL) == 0 ||
        strpbrk(leaf, "\\/:*?\"<>|") != NULL || strcmp(leaf, ".") == 0 || strcmp(leaf, "..") == 0))
        return NT_ERROR_INVALID_NAME;

    const struct { const char *suffix; uint32_t type; } formats[] = {
        {".sz", REG_SZ}, {".dword", REG_DWORD}, {".bin", REG_BINARY}
    };
    unsigned char *content = malloc(MAX_VALUE + 1);
    if (content == NULL)
        return NT_ERROR_NOT_ENOUGH_MEMORY;
    uint32_t length = 0, kind = 0;
    int error = ENOENT;
    for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
        char filename[PATH_MAX];
        if (snprintf(filename, sizeof(filename), "%s\\%s%s", path, leaf, formats[i].suffix) >=
            (int)sizeof(filename)) {
            free(content);
            return NT_ERROR_INVALID_NAME;
        }
        error = read_value(filename, content, &length);
        if (error == 0) {
            kind = formats[i].type;
            break;
        }
        if (error != ENOENT && error != ENOTDIR)
            break;
    }
    if (kind == 0) {
        free(content);
        return error == ENOENT || error == ENOTDIR ? NT_ERROR_FILE_NOT_FOUND :
               error == EFBIG ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_ACCESS_DENIED;
    }
    uint16_t *wide = NULL;
    uint32_t number;
    const void *bytes = content;
    if (kind == REG_SZ) {
        const int32_t units = nt_MultiByteToWideChar(65001, 8, (const char *)content, (int32_t)length + 1, NULL, 0);
        if (units <= 0 || memchr(content, '\0', length) != NULL) {
            free(content);
            return 13; /* ERROR_INVALID_DATA */
        }
        length = (uint32_t)units * sizeof(*wide);
        wide = malloc(length);
        if (wide == NULL) {
            free(content);
            return NT_ERROR_NOT_ENOUGH_MEMORY;
        }
        nt_MultiByteToWideChar(65001, 8, (const char *)content, -1, wide, units);
        bytes = wide;
    } else if (kind == REG_DWORD) {
        const char *start = (const char *)content + strspn((const char *)content, " \t");
        char *end;
        errno = 0;
        const unsigned long value = strtoul(start, &end, 0);
        if (*start == '-' || start == end || errno != 0 || value > UINT32_MAX ||
            end[strspn(end, " \t\r\n")] != 0 || memchr(content, '\0', length) != NULL) {
            free(content);
            return 13;
        }
        number = (uint32_t)value;
        length = sizeof(number);
        bytes = &number;
    }
    const bool fits = data == NULL || *size >= length;
    if (size != NULL)
        *size = length;
    if (type != NULL)
        *type = kind;
    if (data != NULL && fits)
        memcpy(data, bytes, length);
    free(wide);
    free(content);
    return fits ? NT_ERROR_SUCCESS : 234; /* ERROR_MORE_DATA */
}
