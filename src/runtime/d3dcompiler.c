/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "graphics_linux.h"
#include "shader.h"
#include "winbase.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>

enum { MAX_INCLUDE_SIZE = 64 * 1024 * 1024 };

typedef HRESULT (NT_MS_ABI *compile_shader)(const void *, size_t, const char *,
        const void *, void *, const char *, const char *, UINT, UINT, ID3DBlob **, ID3DBlob **);
static compile_shader native_compile;
static pthread_once_t compiler_once = PTHREAD_ONCE_INIT;

static void load_compiler(void)
{
    void *symbol = dlsym(RTLD_DEFAULT, "D3DCompile");
    _Static_assert(sizeof(symbol) == sizeof(native_compile), "POSIX dlsym ABI");
    memcpy(&native_compile, &symbol, sizeof(symbol));
}

bool nt_runtime_d3dcompiler_available(void)
{
    pthread_once(&compiler_once, load_compiler);
    return native_compile != NULL;
}

struct include_file {
    struct include_file *next;
    void *data;
    char path[PATH_MAX];
};
struct shader_include {
    const struct include_api *vtable;
    const char *source;
    struct include_file *files;
};
struct include_api {
    HRESULT (NT_MS_ABI *open)(struct shader_include *, UINT, const char *, const void *, const void **, UINT *);
    HRESULT (NT_MS_ABI *close)(struct shader_include *, const void *);
};

static bool include_path(char out[PATH_MAX], const char *parent, const char *filename)
{
    size_t prefix = 0, length = strlen(filename);
    if (parent && filename[0] != '/' && filename[0] != '\\' && filename[1] != ':')
        for (const char *p = parent; *p; ++p)
            if (*p == '/' || *p == '\\')
                prefix = (size_t)(p - parent) + 1;
    if (prefix + length >= PATH_MAX)
        return false;
    if (prefix)
        memcpy(out, parent, prefix);
    memcpy(out + prefix, filename, length + 1);
    return true;
}

static HRESULT NT_MS_ABI include_open(struct shader_include *include, UINT kind,
        const char *name, const void *parent, const void **data, UINT *size)
{
    if (!name || !*name || !data || !size || kind > 1 || strnlen(name, PATH_MAX) == PATH_MAX)
        return E_INVALIDARG;
    *data = NULL;
    *size = 0;
    char *filename = nt_runtime_ansi_to_utf8(name, strlen(name));
    struct include_file *file = calloc(1, sizeof(*file));
    if (!filename || !file) {
        free(filename);
        free(file);
        return E_OUTOFMEMORY;
    }
    const char *relative_to = include->source;
    if (kind == 0 && parent) {
        for (struct include_file *entry = include->files; entry; entry = entry->next) {
            if (entry->data == parent) {
                relative_to = entry->path;
                break;
            }
        }
    }
    bool valid = include_path(file->path, relative_to, filename);
    free(filename);
    size_t length;
    int error = valid ? nt_runtime_read_guest_file(file->path, MAX_INCLUDE_SIZE, &file->data, &length) : EINVAL;
    if (error) {
        free(file);
        return error == ENOMEM ? E_OUTOFMEMORY : E_FAIL;
    }
    file->next = include->files;
    include->files = file;
    *data = file->data;
    *size = (UINT)length;
    return S_OK;
}

static HRESULT NT_MS_ABI include_close(struct shader_include *include, const void *data)
{
    struct include_file **entry = &include->files;
    while (*entry && (*entry)->data != data)
        entry = &(*entry)->next;
    if (!*entry)
        return E_INVALIDARG;
    struct include_file *file = *entry;
    *entry = file->next;
    free(file->data);
    free(file);
    return S_OK;
}

int32_t NT_MS_ABI nt_D3DCompile(const void *source, size_t length, const char *filename,
                              const void *defines, void *include, const char *entry,
                              const char *profile, uint32_t flags, uint32_t effect_flags,
                              void **shader, void **errors)
{
    static const struct include_api vtable = {include_open, include_close};
    if (shader)
        *shader = NULL;
    if (errors)
        *errors = NULL;
    if (!source || !length || !profile || !shader)
        return E_INVALIDARG;
    if (!nt_runtime_d3dcompiler_available())
        return E_NOTIMPL;
    struct shader_include guest_include = {.vtable = &vtable};
    char *path = NULL;
    if ((uintptr_t)include == 1) { /* D3D_COMPILE_STANDARD_FILE_INCLUDE */
        if (!filename || strnlen(filename, PATH_MAX) == PATH_MAX)
            return E_INVALIDARG;
        path = nt_runtime_ansi_to_utf8(filename, strlen(filename));
        if (!path)
            return E_OUTOFMEMORY;
        guest_include.source = path;
        include = &guest_include;
    }
    ID3DBlob *bytecode = NULL, *messages = NULL;
    HRESULT hr = native_compile(source, length, filename, defines, include, entry, profile,
                                flags, effect_flags, &bytecode, errors ? &messages : NULL);
    while (guest_include.files)
        include_close(&guest_include, guest_include.files->data);
    free(path);
    *shader = bytecode;
    if (errors)
        *errors = messages;
    return hr;
}
