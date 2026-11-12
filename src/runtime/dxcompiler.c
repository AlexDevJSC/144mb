/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "shader.h"
#include "dxcompiler_path.h"
#include "graphics_linux.h"
#include "winbase.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <wchar.h>

enum { CP_ACP = 0, CP_UTF16 = 1200, DXC_MAX_SOURCE = 64 * 1024 * 1024,
       DXC_MAX_ARGS = 4096, DXC_MAX_WIDE = 32768 };
enum object_kind { COMPILER, UTILS, RESULT, BLOB, ENCODED, UTF8, WIDE, INCLUDE };

static const GUID iid_unknown = {0,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_operation_result = {0xcedb484a,0xd4e9,0x445a,{0xb9,0x91,0xca,0x21,0xca,0x15,0x7d,0xc2}};
static const GUID class_compiler = {0x73e22d93,0xe6ce,0x47f3,{0xb5,0xbf,0xf0,0x66,0x4f,0x39,0xc1,0xb0}};
static const GUID class_utils = {0x6245d6af,0x66e0,0x48fd,{0x80,0xb4,0x4d,0x27,0x17,0x96,0x74,0x8c}};
static const GUID interfaces[] = {
    [COMPILER] = {0x228b4687,0x5a6a,0x4730,{0x90,0x0c,0x97,0x02,0xb2,0x20,0x3f,0x54}},
    [UTILS] = {0x4605c4cb,0x2019,0x492a,{0xad,0xa4,0x65,0xf2,0x0b,0xb7,0xd6,0x7f}},
    [RESULT] = {0x58346cda,0xdde7,0x4497,{0x94,0x61,0x6f,0x87,0xaf,0x5e,0x06,0x59}},
    [BLOB] = {0x8ba5fb08,0x5195,0x40e2,{0xac,0x58,0x0d,0x98,0x9c,0x3a,0x01,0x02}},
    [ENCODED] = {0x7241d424,0x2646,0x4191,{0x97,0xc0,0x98,0xe9,0x6e,0x42,0xfc,0x68}},
    [UTF8] = {0x3da636c9,0xba71,0x4024,{0xa3,0x01,0x30,0xcb,0xf1,0x25,0x30,0x5b}},
    [WIDE] = {0xa3f84eab,0x0faa,0x497e,{0xa3,0x9c,0xee,0x6e,0xd6,0x0b,0x2d,0x84}},
    [INCLUDE] = {0x7f61fc7d,0x950d,0x467f,{0xb3,0xe3,0x3c,0x02,0xfb,0x49,0x18,0x7c}},
};

struct dxc_buffer {
    const void *data;
    size_t size;
    uint32_t encoding;
};
_Static_assert(sizeof(struct dxc_buffer) == 24, "DxcBuffer ABI");
_Static_assert(sizeof(wchar_t) == 4, "Native DXC uses UTF-32 wchar_t");

/* The native C++ interface uses SysV calls, unsigned long references and bool. */
#define HOST_UNKNOWN \
    HRESULT (*query)(void *, REFIID, void **); \
    unsigned long (*add_ref)(void *); \
    unsigned long (*release)(void *)

struct host_unknown { HOST_UNKNOWN; };
struct host_compiler {
    HOST_UNKNOWN;
    HRESULT (*compile)(void *, const struct dxc_buffer *, const wchar_t **, UINT, void *, REFIID, void **);
    HRESULT (*disassemble)(void *, const struct dxc_buffer *, REFIID, void **);
};
struct host_blob {
    HOST_UNKNOWN;
    void *(*data)(void *);
    size_t (*size)(void *);
    HRESULT (*encoding)(void *, bool *, UINT *);
    const void *(*string)(void *);
    size_t (*length)(void *);
};
struct host_result {
    HOST_UNKNOWN;
    HRESULT (*status)(void *, HRESULT *);
    HRESULT (*result)(void *, void **);
    HRESULT (*errors)(void *, void **);
    bool (*has_output)(void *, UINT);
    HRESULT (*output)(void *, UINT, REFIID, void **, void **);
    UINT (*count)(void *);
    UINT (*at)(void *, UINT);
    UINT (*primary)(void *);
};
struct host_utils {
    HOST_UNKNOWN;
    HRESULT (*slice)(void *, void *, UINT, UINT, void **);
    HRESULT (*pinned)(void *, const void *, UINT, UINT, void **);
    HRESULT (*move)(void *, const void *, void *, UINT, UINT, void **);
    HRESULT (*blob)(void *, const void *, UINT, UINT, void **);
    HRESULT (*load)(void *, const wchar_t *, UINT *, void **);
    HRESULT (*stream)(void *, void *, void **);
    HRESULT (*include)(void *, void **);
    HRESULT (*utf8)(void *, void *, void **);
    HRESULT (*wide)(void *, void *, void **);
    HRESULT (*part)(void *, const struct dxc_buffer *, UINT, void **, UINT *);
};
struct host_include_api {
    HOST_UNKNOWN;
    HRESULT (*load)(void *, const wchar_t *, void **);
};
#undef HOST_UNKNOWN
#define HOST(object, type) (*(const struct host_##type *const *)(object))

static HRESULT (*native_create)(REFIID, REFIID, void **);
static pthread_once_t dxc_once = PTHREAD_ONCE_INIT;

static void *open_dxc(void)
{
    void *library = dlopen("libdxcompiler.so", RTLD_NOW | RTLD_LOCAL);
    if (library)
        return library;
    char executable[PATH_MAX], directory[PATH_MAX], path[PATH_MAX];
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size <= 0 || (size_t)size >= sizeof(executable) - 1)
        return NULL;
    executable[size] = 0;
    if (!nt_dxc_install_path(executable, directory, sizeof(directory)))
        return NULL;
    int length = snprintf(path, sizeof(path), "%s/libdxcompiler.so", directory);
    return length >= 0 && (size_t)length < sizeof(path) ? dlopen(path, RTLD_NOW | RTLD_LOCAL) : NULL;
}

static void load_dxc(void)
{
    void *library = open_dxc();
    if (!library)
        return;
    void *symbol = dlsym(library, "DxcCreateInstance");
    _Static_assert(sizeof(symbol) == sizeof(native_create), "POSIX dlsym ABI");
    if (!symbol) {
        dlclose(library);
        return;
    }
    memcpy(&native_create, &symbol, sizeof(symbol));
    void *compiler = NULL, *utils = NULL;
    bool available =
        SUCCEEDED(native_create(&class_compiler, &interfaces[COMPILER], &compiler)) && compiler &&
        SUCCEEDED(native_create(&class_utils, &interfaces[UTILS], &utils)) && utils;
    if (compiler)
        HOST(compiler, unknown)->release(compiler);
    if (utils)
        HOST(utils, unknown)->release(utils);
    if (!available) {
        native_create = NULL;
        dlclose(library);
    }
    /* Keep the ELF mapped for the lifetime of the COM objects it creates. */
}

bool nt_runtime_dxcompiler_available(void)
{
    pthread_once(&dxc_once, load_dxc);
    return native_create != NULL;
}

struct dxc_object {
    const void *vtable;
    atomic_uint refs;
    enum object_kind kind;
    void *native;
    void *converted;
    size_t converted_size;
};

#define GUEST_UNKNOWN \
    HRESULT (NT_MS_ABI *query)(struct dxc_object *, REFIID, void **); \
    ULONG (NT_MS_ABI *add_ref)(struct dxc_object *); \
    ULONG (NT_MS_ABI *release)(struct dxc_object *)

struct compiler_api {
    GUEST_UNKNOWN;
    HRESULT (NT_MS_ABI *compile)(struct dxc_object *, const struct dxc_buffer *,
                                 const uint16_t **, UINT, IUnknown *, REFIID, void **);
    HRESULT (NT_MS_ABI *disassemble)(struct dxc_object *, const struct dxc_buffer *, REFIID, void **);
};
struct utils_api {
    GUEST_UNKNOWN;
    HRESULT (NT_MS_ABI *slice)(struct dxc_object *, void *, UINT, UINT, void **);
    HRESULT (NT_MS_ABI *pinned)(struct dxc_object *, const void *, UINT, UINT, void **);
    HRESULT (NT_MS_ABI *move)(struct dxc_object *, const void *, void *, UINT, UINT, void **);
    HRESULT (NT_MS_ABI *blob)(struct dxc_object *, const void *, UINT, UINT, void **);
    HRESULT (NT_MS_ABI *load)(struct dxc_object *, const uint16_t *, UINT *, void **);
    HRESULT (NT_MS_ABI *stream)(struct dxc_object *, void *, void **);
    HRESULT (NT_MS_ABI *include)(struct dxc_object *, void **);
    HRESULT (NT_MS_ABI *utf8)(struct dxc_object *, void *, void **);
    HRESULT (NT_MS_ABI *wide)(struct dxc_object *, void *, void **);
    HRESULT (NT_MS_ABI *part)(struct dxc_object *, const struct dxc_buffer *, UINT, void **, UINT *);
    HRESULT (NT_MS_ABI *reflection)(struct dxc_object *, const struct dxc_buffer *, REFIID, void **);
    HRESULT (NT_MS_ABI *arguments)(struct dxc_object *, const uint16_t *, const uint16_t *, const uint16_t *,
                                   const uint16_t **, UINT, const void *, UINT, void **);
    HRESULT (NT_MS_ABI *pdb)(struct dxc_object *, void *, void **, void **);
};
struct blob_api {
    GUEST_UNKNOWN;
    void *(NT_MS_ABI *data)(struct dxc_object *);
    size_t (NT_MS_ABI *size)(struct dxc_object *);
    HRESULT (NT_MS_ABI *encoding)(struct dxc_object *, BOOL *, UINT *);
    const void *(NT_MS_ABI *string)(struct dxc_object *);
    size_t (NT_MS_ABI *length)(struct dxc_object *);
};
struct result_api {
    GUEST_UNKNOWN;
    HRESULT (NT_MS_ABI *status)(struct dxc_object *, HRESULT *);
    HRESULT (NT_MS_ABI *result)(struct dxc_object *, void **);
    HRESULT (NT_MS_ABI *errors)(struct dxc_object *, void **);
    BOOL (NT_MS_ABI *has_output)(struct dxc_object *, UINT);
    HRESULT (NT_MS_ABI *output)(struct dxc_object *, UINT, REFIID, void **, void **);
    UINT (NT_MS_ABI *count)(struct dxc_object *);
    UINT (NT_MS_ABI *at)(struct dxc_object *, UINT);
    UINT (NT_MS_ABI *primary)(struct dxc_object *);
};
struct include_api {
    GUEST_UNKNOWN;
    HRESULT (NT_MS_ABI *load)(struct dxc_object *, const uint16_t *, void **);
};
#undef GUEST_UNKNOWN

static const struct compiler_api compiler_vtable;
static const struct utils_api utils_vtable;
static const struct blob_api blob_vtable;
static const struct result_api result_vtable;
static const struct include_api include_vtable;

static uint16_t *utf32_to_utf16(const wchar_t *wide, size_t units, size_t *length)
{
    if (units > DXC_MAX_SOURCE / sizeof(wchar_t))
        return NULL;
    uint16_t *result = malloc((units * 2 + 1) * sizeof(*result));
    if (!result)
        return NULL;
    size_t count = 0;
    for (size_t i = 0; i < units; ++i) {
        uint32_t cp = (uint32_t)wide[i];
        if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
            free(result);
            return NULL;
        }
        if (cp > 0xffff) {
            cp -= 0x10000;
            result[count++] = (uint16_t)(0xd800 + (cp >> 10));
            cp = 0xdc00 + (cp & 0x3ff);
        }
        result[count++] = (uint16_t)cp;
    }
    result[count] = 0;
    *length = count;
    return result;
}

static wchar_t *utf16_to_utf32(const uint16_t *wide)
{
    if (!wide)
        return NULL;
    size_t units = nt_runtime_wide_length(wide, DXC_MAX_WIDE);
    if (units == DXC_MAX_WIDE)
        return NULL;
    wchar_t *result = malloc((units + 1) * sizeof(*result));
    if (!result)
        return NULL;
    size_t count = 0;
    for (size_t i = 0; i < units; ++i) {
        uint32_t cp = wide[i];
        if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < units &&
            wide[i + 1] >= 0xdc00 && wide[i + 1] <= 0xdfff)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (wide[++i] - 0xdc00);
        if (cp >= 0xd800 && cp <= 0xdfff) {
            free(result);
            return NULL;
        }
        result[count++] = (wchar_t)cp;
    }
    result[count] = 0;
    return result;
}

static HRESULT wrap_native(enum object_kind kind, void *native, void **out)
{
    *out = NULL;
    if (!native)
        return S_OK;
    const void *tables[] = {
        [COMPILER] = &compiler_vtable, [UTILS] = &utils_vtable, [RESULT] = &result_vtable,
        [BLOB] = &blob_vtable, [ENCODED] = &blob_vtable, [UTF8] = &blob_vtable,
        [WIDE] = &blob_vtable, [INCLUDE] = &include_vtable,
    };
    struct dxc_object *object = calloc(1, sizeof(*object));
    if (!object) {
        HOST(native, unknown)->release(native);
        return E_OUTOFMEMORY;
    }
    object->vtable = tables[kind];
    atomic_init(&object->refs, 1);
    object->kind = kind;
    object->native = native;
    if (kind == WIDE) {
        size_t units = HOST(native, blob)->length(native), length = 0;
        object->converted = utf32_to_utf16(HOST(native, blob)->string(native), units, &length);
        if (!object->converted) {
            HOST(native, unknown)->release(native);
            free(object);
            return E_OUTOFMEMORY;
        }
        object->converted_size = (length + 1) * sizeof(uint16_t);
    }
    *out = object;
    return S_OK;
}

static ULONG NT_MS_ABI object_add_ref(struct dxc_object *object)
{
    return atomic_fetch_add(&object->refs, 1) + 1;
}
static ULONG NT_MS_ABI object_release(struct dxc_object *object)
{
    ULONG left = atomic_fetch_sub(&object->refs, 1) - 1;
    if (!left) {
        HOST(object->native, unknown)->release(object->native);
        free(object->converted);
        free(object);
    }
    return left;
}
static HRESULT NT_MS_ABI object_query(struct dxc_object *object, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!iid)
        return E_INVALIDARG;
    enum object_kind kind = object->kind;
    bool blob = kind >= BLOB && kind <= WIDE;
    if (!IsEqualGUID(iid, &iid_unknown) && !IsEqualGUID(iid, &interfaces[kind]) &&
        !(kind == RESULT && IsEqualGUID(iid, &iid_operation_result)) &&
        !(blob && IsEqualGUID(iid, &interfaces[BLOB])) &&
        !(blob && kind != BLOB && IsEqualGUID(iid, &interfaces[ENCODED])))
        return E_NOINTERFACE;
    object_add_ref(object);
    *out = object;
    return S_OK;
}

static void *NT_MS_ABI blob_data(struct dxc_object *object)
{
    return object->converted ? object->converted : HOST(object->native, blob)->data(object->native);
}
static size_t NT_MS_ABI blob_size(struct dxc_object *object)
{
    return object->converted ? object->converted_size : HOST(object->native, blob)->size(object->native);
}
static HRESULT NT_MS_ABI blob_encoding(struct dxc_object *object, BOOL *known, UINT *encoding)
{
    if (!known || !encoding)
        return E_POINTER;
    *known = false;
    *encoding = CP_ACP;
    if (object->kind == BLOB)
        return E_NOINTERFACE;
    if (object->kind == WIDE) {
        *known = true;
        *encoding = CP_UTF16;
        return S_OK;
    }
    bool host_known = false;
    HRESULT hr = HOST(object->native, blob)->encoding(object->native, &host_known, encoding);
    *known = host_known;
    return hr;
}
static const void *NT_MS_ABI blob_string(struct dxc_object *object)
{
    if (object->kind == WIDE)
        return object->converted;
    return object->kind == UTF8 ? HOST(object->native, blob)->string(object->native) : NULL;
}
static size_t NT_MS_ABI blob_length(struct dxc_object *object)
{
    if (object->kind == WIDE)
        return object->converted_size / sizeof(uint16_t) - 1;
    return object->kind == UTF8 ? HOST(object->native, blob)->length(object->native) : 0;
}
static const struct blob_api blob_vtable = {
    object_query, object_add_ref, object_release, blob_data, blob_size, blob_encoding, blob_string, blob_length
};

static HRESULT NT_MS_ABI result_status(struct dxc_object *object, HRESULT *status)
{
    return status ? HOST(object->native, result)->status(object->native, status) : E_POINTER;
}
static HRESULT NT_MS_ABI result_blob(struct dxc_object *object, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    void *native = NULL;
    HRESULT hr = HOST(object->native, result)->result(object->native, &native);
    return SUCCEEDED(hr) ? wrap_native(BLOB, native, out) : hr;
}
static HRESULT NT_MS_ABI result_errors(struct dxc_object *object, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    void *native = NULL;
    HRESULT hr = HOST(object->native, result)->errors(object->native, &native);
    return SUCCEEDED(hr) ? wrap_native(ENCODED, native, out) : hr;
}
static BOOL NT_MS_ABI result_has_output(struct dxc_object *object, UINT kind)
{
    return HOST(object->native, result)->has_output(object->native, kind);
}
static HRESULT NT_MS_ABI result_output(struct dxc_object *object, UINT kind, REFIID iid,
                                      void **out, void **name)
{
    if (out)
        *out = NULL;
    if (name)
        *name = NULL;
    if (!out && !name)
        return E_INVALIDARG;
    enum object_kind requested = BLOB;
    if (out) {
        while (requested <= WIDE && (!iid || !IsEqualGUID(iid, &interfaces[requested])))
            ++requested;
        if (requested > WIDE)
            return E_NOINTERFACE;
    }
    void *native = NULL, *native_name = NULL;
    HRESULT hr = HOST(object->native, result)->output(object->native, kind, &interfaces[requested],
                                                      out ? &native : NULL, name ? &native_name : NULL);
    if (SUCCEEDED(hr) && out)
        hr = wrap_native(requested, native, out);
    if (SUCCEEDED(hr) && name) {
        hr = wrap_native(WIDE, native_name, name);
        native_name = NULL;
    }
    if (native_name)
        HOST(native_name, unknown)->release(native_name);
    if (FAILED(hr) && out && *out) {
        object_release(*out);
        *out = NULL;
    }
    return hr;
}
static UINT NT_MS_ABI result_count(struct dxc_object *object)
{
    return HOST(object->native, result)->count(object->native);
}
static UINT NT_MS_ABI result_at(struct dxc_object *object, UINT index)
{
    return HOST(object->native, result)->at(object->native, index);
}
static UINT NT_MS_ABI result_primary(struct dxc_object *object)
{
    return HOST(object->native, result)->primary(object->native);
}
static const struct result_api result_vtable = {
    object_query, object_add_ref, object_release, result_status, result_blob, result_errors,
    result_has_output, result_output, result_count, result_at, result_primary
};

static HRESULT copy_guest_blob(void *utils, void *guest, void **out)
{
    const struct blob_api *blob = *(const struct blob_api **)guest;
    struct dxc_object *encoded = NULL;
    BOOL known = false;
    UINT encoding = CP_ACP;
    size_t size = blob->size(guest);
    if (size > DXC_MAX_SOURCE)
        return E_INVALIDARG;
    if (SUCCEEDED(blob->query(guest, &interfaces[ENCODED], (void **)&encoded)) && encoded) {
        const struct blob_api *vtable = encoded->vtable;
        vtable->encoding(encoded, &known, &encoding);
        vtable->release(encoded);
    }
    return HOST(utils, utils)->blob(utils, blob->data(guest), (UINT)size, known ? encoding : CP_ACP, out);
}

static HRESULT create_blob(struct dxc_object *utils, const void *data, UINT size, UINT encoding,
                           bool pin, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if ((!data && size) || size > DXC_MAX_SOURCE)
        return E_INVALIDARG;
    const struct host_utils *vtable = HOST(utils->native, utils);
    void *native = NULL;
    HRESULT hr = pin ? vtable->pinned(utils->native, data, size, encoding, &native) :
                       vtable->blob(utils->native, data, size, encoding, &native);
    return SUCCEEDED(hr) ? wrap_native(ENCODED, native, out) : hr;
}
static HRESULT NT_MS_ABI utils_blob(struct dxc_object *utils, const void *data, UINT size, UINT encoding, void **out)
{
    return create_blob(utils, data, size, encoding, false, out);
}
static HRESULT NT_MS_ABI utils_pinned(struct dxc_object *utils, const void *data, UINT size, UINT encoding, void **out)
{
    return create_blob(utils, data, size, encoding, true, out);
}
static HRESULT NT_MS_ABI utils_slice(struct dxc_object *utils, void *guest, UINT offset, UINT length, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!guest)
        return E_INVALIDARG;
    void *source = NULL, *native = NULL;
    HRESULT hr = copy_guest_blob(utils->native, guest, &source);
    if (SUCCEEDED(hr)) {
        hr = HOST(utils->native, utils)->slice(utils->native, source, offset, length, &native);
        HOST(source, unknown)->release(source);
    }
    return SUCCEEDED(hr) ? wrap_native(BLOB, native, out) : hr;
}
static HRESULT NT_MS_ABI utils_load(struct dxc_object *utils, const uint16_t *filename, UINT *encoding, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    size_t units = filename ? nt_runtime_wide_length(filename, PATH_MAX) : 0;
    if (!units || units == PATH_MAX)
        return E_INVALIDARG;
    char *path = nt_runtime_utf16_to_utf8(filename, units);
    if (!path)
        return E_INVALIDARG;
    void *data = NULL;
    size_t size = 0;
    int error = nt_runtime_read_guest_file(path, DXC_MAX_SOURCE, &data, &size);
    free(path);
    if (error)
        return error == ENOMEM ? E_OUTOFMEMORY : error == ENOENT ? (HRESULT)0x80070002 : E_FAIL;
    HRESULT hr = create_blob(utils, data, (UINT)size, encoding ? *encoding : CP_ACP, false, out);
    free(data);
    return hr;
}
static HRESULT NT_MS_ABI include_load(struct dxc_object *include, const uint16_t *filename, void **out)
{
    return utils_load(include, filename, NULL, out);
}
static const struct include_api include_vtable = {
    object_query, object_add_ref, object_release, include_load
};
static HRESULT NT_MS_ABI utils_include(struct dxc_object *utils, void **out)
{
    if (!out)
        return E_POINTER;
    HOST(utils->native, unknown)->add_ref(utils->native);
    return wrap_native(INCLUDE, utils->native, out);
}

static HRESULT utils_text(struct dxc_object *utils, void *guest, void **out, enum object_kind encoding)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!guest)
        return E_INVALIDARG;
    void *source = NULL, *native = NULL;
    HRESULT hr = copy_guest_blob(utils->native, guest, &source);
    if (SUCCEEDED(hr)) {
        const struct host_utils *vtable = HOST(utils->native, utils);
        hr = encoding == UTF8 ? vtable->utf8(utils->native, source, &native) :
                               vtable->wide(utils->native, source, &native);
        HOST(source, unknown)->release(source);
    }
    return SUCCEEDED(hr) ? wrap_native(encoding, native, out) : hr;
}
static HRESULT NT_MS_ABI utils_utf8(struct dxc_object *utils, void *blob, void **out)
{
    return utils_text(utils, blob, out, UTF8);
}
static HRESULT NT_MS_ABI utils_wide(struct dxc_object *utils, void *blob, void **out)
{
    return utils_text(utils, blob, out, WIDE);
}
static HRESULT NT_MS_ABI utils_part(struct dxc_object *utils, const struct dxc_buffer *blob,
                                     UINT kind, void **data, UINT *length)
{
    if (!blob || !data || !length)
        return E_INVALIDARG;
    *data = NULL;
    *length = 0;
    return HOST(utils->native, utils)->part(utils->native, blob, kind, data, length);
}

static HRESULT unavailable(void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    return E_NOTIMPL;
}
static HRESULT NT_MS_ABI utils_move(struct dxc_object *utils, const void *data, void *allocator,
                                     UINT size, UINT encoding, void **out)
{
    (void)utils; (void)data; (void)allocator; (void)size; (void)encoding;
    return unavailable(out);
}
static HRESULT NT_MS_ABI utils_stream(struct dxc_object *utils, void *blob, void **out)
{
    (void)utils; (void)blob;
    return unavailable(out);
}
static HRESULT NT_MS_ABI utils_reflection(struct dxc_object *utils, const struct dxc_buffer *blob,
                                          REFIID iid, void **out)
{
    (void)utils; (void)blob; (void)iid;
    return unavailable(out);
}
static HRESULT NT_MS_ABI utils_arguments(struct dxc_object *utils, const uint16_t *source,
        const uint16_t *entry, const uint16_t *target, const uint16_t **arguments, UINT count,
        const void *defines, UINT define_count, void **out)
{
    (void)utils; (void)source; (void)entry; (void)target; (void)arguments;
    (void)count; (void)defines; (void)define_count;
    return unavailable(out);
}
static HRESULT NT_MS_ABI utils_pdb(struct dxc_object *utils, void *blob, void **hash, void **container)
{
    (void)utils; (void)blob;
    if (hash)
        *hash = NULL;
    return unavailable(container);
}
static const struct utils_api utils_vtable = {
    object_query, object_add_ref, object_release, utils_slice, utils_pinned, utils_move, utils_blob,
    utils_load, utils_stream, utils_include, utils_utf8, utils_wide, utils_part, utils_reflection,
    utils_arguments, utils_pdb
};

struct include_bridge {
    const struct host_include_api *vtable;
    atomic_ulong refs;
    struct dxc_object *guest;
    void *utils;
};
static unsigned long bridge_add_ref(void *object)
{
    struct include_bridge *bridge = object;
    return atomic_fetch_add(&bridge->refs, 1) + 1;
}
static unsigned long bridge_release(void *object)
{
    struct include_bridge *bridge = object;
    unsigned long left = atomic_fetch_sub(&bridge->refs, 1) - 1;
    if (!left) {
        IUnknown_Release((IUnknown *)bridge->guest);
        HOST(bridge->utils, unknown)->release(bridge->utils);
        free(bridge);
    }
    return left;
}
static HRESULT bridge_query(void *object, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!iid || (!IsEqualGUID(iid, &iid_unknown) && !IsEqualGUID(iid, &interfaces[INCLUDE])))
        return E_NOINTERFACE;
    bridge_add_ref(object);
    *out = object;
    return S_OK;
}
static HRESULT bridge_load(void *object, const wchar_t *filename, void **out)
{
    struct include_bridge *bridge = object;
    size_t length;
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!filename)
        return E_INVALIDARG;
    uint16_t *guest_name = utf32_to_utf16(filename, wcslen(filename), &length);
    if (!guest_name)
        return E_OUTOFMEMORY;
    const struct include_api *vtable = bridge->guest->vtable;
    void *guest_blob = NULL;
    HRESULT hr = vtable->load(bridge->guest, guest_name, &guest_blob);
    free(guest_name);
    if (SUCCEEDED(hr))
        hr = guest_blob ? copy_guest_blob(bridge->utils, guest_blob, out) : E_FAIL;
    if (guest_blob)
        IUnknown_Release((IUnknown *)guest_blob);
    return hr;
}
static HRESULT create_bridge(IUnknown *guest, struct include_bridge **out)
{
    static const struct host_include_api vtable = {bridge_query, bridge_add_ref, bridge_release, bridge_load};
    *out = NULL;
    if (!guest)
        return S_OK;
    struct include_bridge *bridge = calloc(1, sizeof(*bridge));
    if (!bridge)
        return E_OUTOFMEMORY;
    HRESULT hr = IUnknown_QueryInterface(guest, &interfaces[INCLUDE], (void **)&bridge->guest);
    if (SUCCEEDED(hr))
        hr = native_create(&class_utils, &interfaces[UTILS], &bridge->utils);
    if (FAILED(hr)) {
        if (bridge->guest)
            IUnknown_Release((IUnknown *)bridge->guest);
        free(bridge);
        return hr;
    }
    bridge->vtable = &vtable;
    atomic_init(&bridge->refs, 1);
    *out = bridge;
    return S_OK;
}

static bool valid_result_iid(REFIID iid)
{
    return iid && (IsEqualGUID(iid, &interfaces[RESULT]) || IsEqualGUID(iid, &iid_operation_result) ||
                   IsEqualGUID(iid, &iid_unknown));
}
static HRESULT NT_MS_ABI compiler_compile(struct dxc_object *object, const struct dxc_buffer *source,
        const uint16_t **arguments, UINT count, IUnknown *include, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!source || !source->data || source->size > DXC_MAX_SOURCE ||
        count > DXC_MAX_ARGS || (count && !arguments))
        return E_INVALIDARG;
    if (!valid_result_iid(iid))
        return E_NOINTERFACE;
    wchar_t **native_arguments = count ? calloc(count, sizeof(*native_arguments)) : NULL;
    if (count && !native_arguments)
        return E_OUTOFMEMORY;
    HRESULT hr = S_OK;
    for (UINT i = 0; i < count; ++i) {
        native_arguments[i] = utf16_to_utf32(arguments[i]);
        if (!native_arguments[i]) {
            hr = E_INVALIDARG;
            break;
        }
    }
    struct include_bridge *bridge = NULL;
    if (SUCCEEDED(hr))
        hr = create_bridge(include, &bridge);
    void *native = NULL;
    if (SUCCEEDED(hr))
        hr = HOST(object->native, compiler)->compile(object->native, source,
                (const wchar_t **)native_arguments, count, bridge, &interfaces[RESULT], &native);
    if (bridge)
        bridge_release(bridge);
    for (UINT i = 0; i < count; ++i)
        free(native_arguments[i]);
    free(native_arguments);
    return SUCCEEDED(hr) ? wrap_native(RESULT, native, out) : hr;
}
static HRESULT NT_MS_ABI compiler_disassemble(struct dxc_object *object,
        const struct dxc_buffer *source, REFIID iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!source || !source->data || source->size > DXC_MAX_SOURCE)
        return E_INVALIDARG;
    if (!valid_result_iid(iid))
        return E_NOINTERFACE;
    void *native = NULL;
    HRESULT hr = HOST(object->native, compiler)->disassemble(object->native, source,
                                                             &interfaces[RESULT], &native);
    return SUCCEEDED(hr) ? wrap_native(RESULT, native, out) : hr;
}
static const struct compiler_api compiler_vtable = {
    object_query, object_add_ref, object_release, compiler_compile, compiler_disassemble
};

int32_t NT_MS_ABI nt_DxcCreateInstance(const void *class_id, const void *iid, void **out)
{
    if (!out)
        return E_POINTER;
    *out = NULL;
    if (!class_id || !iid)
        return E_INVALIDARG;
    enum object_kind kind;
    if (IsEqualGUID(class_id, &class_compiler))
        kind = COMPILER;
    else if (IsEqualGUID(class_id, &class_utils))
        kind = UTILS;
    else
        return (HRESULT)0x80040154;
    if (!IsEqualGUID(iid, &interfaces[kind]) && !IsEqualGUID(iid, &iid_unknown))
        return E_NOINTERFACE;
    if (!nt_runtime_dxcompiler_available())
        return E_NOTIMPL;
    void *native = NULL;
    HRESULT hr = native_create(class_id, &interfaces[kind], &native);
    return SUCCEEDED(hr) ? wrap_native(kind, native, out) : hr;
}
