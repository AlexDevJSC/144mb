/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "section_memory.h"
#include "object_broker.h"
#include "nt_services.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* First general section foundation: pagefile-backed Win32 mappings from
 * memfd, with per-process handles/names and real MAP_SHARED views. It is
 * deliberately NOT a shared, global NT object namespace yet. */
enum {
    SECTION_LIMIT = 128, SECTION_HANDLE_LIMIT = 256, SECTION_VIEW_LIMIT = 256,
    SECTION_NAME_LIMIT = 127, SECTION_PAGE = 4096,
    SECTION_PAGE_READONLY = 2, SECTION_PAGE_READWRITE = 4,
    SECTION_MAP_COPY = 1, SECTION_MAP_WRITE = 2, SECTION_MAP_READ = 4,
    SECTION_HANDLE_INHERIT = 1, SECTION_HANDLE_PROTECT = 2
};
#define SECTION_MAX_SIZE UINT64_C(0x80000000)
#define SECTION_HANDLE_BASE UINT64_C(0x6200000000000000)

struct section_object {
    bool alive;
    int fd;
    uint64_t size;
    uint32_t protect;
    uint16_t name[SECTION_NAME_LIMIT + 1];
    bool named;
    bool brokered;
    struct nt_broker_resource broker;
    size_t references;
};
struct section_handle {
    uintptr_t number;
    struct section_object *object;
    uint32_t allowed;
    uint32_t flags;
};
struct section_view {
    void *base;
    size_t size;
    uint32_t protect;
    struct section_object *object;
};

static pthread_mutex_t section_lock = PTHREAD_MUTEX_INITIALIZER;
static struct section_object objects[SECTION_LIMIT];
static struct section_handle handles[SECTION_HANDLE_LIMIT];
static struct section_view views[SECTION_VIEW_LIMIT];
static uintptr_t next_number = (uintptr_t)SECTION_HANDLE_BASE;

static int mapping_error(int error)
{
    switch (error) {
    case ENOMEM: case EMFILE: case ENFILE: return NT_ERROR_NOT_ENOUGH_MEMORY;
    case EACCES: case EPERM: return NT_ERROR_ACCESS_DENIED;
    case EINVAL: case EFBIG: return NT_ERROR_INVALID_PARAMETER;
    default: return NT_ERROR_GEN_FAILURE;
    }
}
static bool copy_name(const uint16_t *name, uint16_t target[SECTION_NAME_LIMIT + 1])
{
    if (!name || !*name) {
        target[0] = 0;
        return true;
    }
    /* Bounded memory reads; the object namespace is local and excludes
     * \Global\, session paths and arbitrary object-manager syntax. */
    for (size_t i = 0; i < SECTION_NAME_LIMIT + 1; ++i) {
        uint16_t c = name[i];
        if (!c) {
            target[i] = 0;
            return true;
        }
        if (c == '/' || c == '\\' || c < 0x20 || (c >= 0xd800 && c <= 0xdfff))
            return false;
        target[i] = c;
    }
    return false;
}
static struct section_handle *lookup_handle(uintptr_t handle)
{
    for (size_t i = 0; i < SECTION_HANDLE_LIMIT; ++i)
        if (handles[i].number == handle && handles[i].object)
            return &handles[i];
    return NULL;
}
static struct section_object *lookup_named(const uint16_t *name)
{
    for (size_t i = 0; i < SECTION_LIMIT; ++i)
        if (objects[i].alive && objects[i].named &&
            memcmp(objects[i].name, name, sizeof(objects[i].name)) == 0)
            return &objects[i];
    return NULL;
}
static struct section_handle *allocate_handle(struct section_object *object,
                                             uint32_t allowed, uint32_t flags)
{
    for (size_t i = 0; i < SECTION_HANDLE_LIMIT; ++i) {
        if (!handles[i].object) {
            if (next_number > UINTPTR_MAX - 16u)
                return NULL;
            handles[i] = (struct section_handle){
                .number = next_number, .object = object,
                .allowed = allowed, .flags = flags
            };
            next_number += 16u;
            ++object->references;
            return &handles[i];
        }
    }
    return NULL;
}
static void release_object(struct section_object *object)
{
    if (object && --object->references == 0) {
        if (object->brokered)
            nt_broker_release(&object->broker);
        else
            close(object->fd);
        memset(object, 0, sizeof(*object));
        object->fd = -1;
    }
}
static bool accept_access(uint32_t access, uint32_t *normalized)
{
    /* FILE_MAP_ALL_ACCESS is a common Windows map request. Execute and
     * unknown bits are rejected instead of creating executable mappings. */
    if (access == 0xf001fu) {
        *normalized = SECTION_MAP_READ | SECTION_MAP_WRITE;
        return true;
    }
    if (access == SECTION_MAP_READ ||
        access == SECTION_MAP_WRITE ||
        access == (SECTION_MAP_READ | SECTION_MAP_WRITE) ||
        access == SECTION_MAP_COPY) {
        *normalized = access;
        return true;
    }
    return false;
}
static uint32_t section_rights(uint32_t protect)
{
    return protect == SECTION_PAGE_READONLY ? SECTION_MAP_READ :
           SECTION_MAP_READ | SECTION_MAP_WRITE | SECTION_MAP_COPY;
}

static uint32_t broker_win_error(int status)
{
    switch (status) {
    case NT_BROKER_MISSING: return NT_ERROR_FILE_NOT_FOUND;
    case NT_BROKER_TYPE_MISMATCH: return NT_ERROR_INVALID_HANDLE;
    case NT_BROKER_ACCESS_DENIED: return NT_ERROR_ACCESS_DENIED;
    case NT_BROKER_RESOURCE_ERROR: return NT_ERROR_NOT_ENOUGH_MEMORY;
    case NT_BROKER_INVALID: return NT_ERROR_INVALID_PARAMETER;
    default: return NT_ERROR_GEN_FAILURE;
    }
}

static uintptr_t broker_section(const uint16_t *name, uint32_t action,
                                 uint32_t rights, uint32_t protect, uint64_t size,
                                 bool inherit)
{
    struct nt_broker_request request = {
        .magic = NT_BROKER_MAGIC, .version = NT_BROKER_VERSION,
        .action = action, .kind = NT_BROKER_SECTION,
        .rights = rights, .protect = protect, .size = size
    };
    if (!nt_WideCharToMultiByte(65001, 0, name, -1, request.name,
                                 sizeof(request.name), NULL, NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_broker_resource broker;
    int status = nt_broker_acquire(&request, &broker);
    if (status != NT_BROKER_CREATED && status != NT_BROKER_EXISTS) {
        nt_SetLastError(broker_win_error(status));
        return 0;
    }
    pthread_mutex_lock(&section_lock);
    struct section_object *object = NULL;
    for (size_t i = 0; i < SECTION_LIMIT; ++i)
        if (!objects[i].alive) {
            object = &objects[i];
            break;
        }
    if (!object) {
        pthread_mutex_unlock(&section_lock);
        nt_broker_release(&broker);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    *object = (struct section_object){
        .alive = true, .fd = broker.fd,
        .size = broker.size, .protect = broker.protect,
        .brokered = true, .broker = broker,
        .named = true
    };
    const size_t name_length = nt_runtime_wide_length(name, SECTION_NAME_LIMIT);
    memcpy(object->name, name, (name_length + 1) * sizeof(uint16_t));
    struct section_handle *slot = allocate_handle(object, rights,
                                      inherit ? SECTION_HANDLE_INHERIT : 0);
    if (!slot) {
        nt_broker_release(&object->broker);
        memset(object, 0, sizeof(*object));
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const uintptr_t result = slot->number;
    pthread_mutex_unlock(&section_lock);
    nt_SetLastError(status == NT_BROKER_EXISTS ?
                    NT_ERROR_ALREADY_EXISTS : NT_ERROR_SUCCESS);
    return result;
}

uintptr_t NT_MS_ABI nt_CreateFileMappingW(uintptr_t file, const void *security,
    uint32_t protect, uint32_t high, uint32_t low, const uint16_t *name)
{
    uint16_t local_name[SECTION_NAME_LIMIT + 1] = {0};
    const uint64_t size = (uint64_t)high << 32 | low;
    if (file != UINTPTR_MAX || security != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (!copy_name(name, local_name) || (protect != SECTION_PAGE_READONLY &&
        protect != SECTION_PAGE_READWRITE) || size == 0 || size > SECTION_MAX_SIZE) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (local_name[0] && nt_runtime_broker_enabled())
        return broker_section(local_name, NT_BROKER_CREATE,
                              section_rights(protect), protect, size, false);
    pthread_mutex_lock(&section_lock);
    const bool named = local_name[0] != 0;
    struct section_object *obj = named ? lookup_named(local_name) : NULL;
    if (obj) {
        struct section_handle *slot = allocate_handle(obj, section_rights(obj->protect), 0);
        pthread_mutex_unlock(&section_lock);
        if (!slot) {
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        nt_SetLastError(NT_ERROR_ALREADY_EXISTS);
        return slot->number;
    }
    obj = NULL;
    for (size_t i = 0; i < SECTION_LIMIT; ++i)
        if (!objects[i].alive) {
            obj = &objects[i];
            break;
        }
    if (!obj) {
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    int fd = memfd_create("ntnative-section", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) != 0) {
        const int error = errno;
        if (fd >= 0)
            close(fd);
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(mapping_error(error));
        return 0;
    }
    *obj = (struct section_object){
        .alive = true, .fd = fd, .size = size,
        .protect = protect, .named = named
    };
    memcpy(obj->name, local_name, sizeof(local_name));
    struct section_handle *slot = allocate_handle(obj, section_rights(protect), 0);
    if (!slot) {
        close(fd);
        memset(obj, 0, sizeof(*obj));
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const uintptr_t result = slot->number;
    pthread_mutex_unlock(&section_lock);
    nt_SetLastError(NT_ERROR_SUCCESS);
    return result;
}

uintptr_t NT_MS_ABI nt_OpenFileMappingW(uint32_t access, int32_t inherit,
                                        const uint16_t *name)
{
    uint16_t local_name[SECTION_NAME_LIMIT + 1] = {0};
    uint32_t allowed;
    if (!name || !*name || !copy_name(name, local_name) ||
        !accept_access(access, &allowed)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (nt_runtime_broker_enabled())
        return broker_section(local_name, NT_BROKER_OPEN,
                              allowed, 0, 0, inherit != 0);
    pthread_mutex_lock(&section_lock);
    struct section_object *obj = lookup_named(local_name);
    struct section_handle *slot = NULL;
    uint32_t error = NT_ERROR_FILE_NOT_FOUND;
    if (obj) {
        if ((allowed & ~section_rights(obj->protect)) != 0)
            error = NT_ERROR_ACCESS_DENIED;
        else {
            slot = allocate_handle(obj, allowed, inherit ? SECTION_HANDLE_INHERIT : 0);
            error = NT_ERROR_NOT_ENOUGH_MEMORY;
        }
    }
    const uintptr_t result = slot ? slot->number : 0;
    pthread_mutex_unlock(&section_lock);
    if (!result)
        nt_SetLastError(error);
    return result;
}

/* The A API always uses the declared process ACP, not a heuristic
 * UTF-8/CP1252 fallback. Names are converted once at the Win32 boundary. */
uintptr_t NT_MS_ABI nt_CreateFileMappingA(uintptr_t file, const void *security,
    uint32_t protect, uint32_t high, uint32_t low, const char *name)
{
    uint16_t wide[SECTION_NAME_LIMIT + 1];
    if (name && !nt_MultiByteToWideChar(nt_GetACP(), 0, name, -1,
                                         wide, SECTION_NAME_LIMIT + 1))
        return 0;
    return nt_CreateFileMappingW(file, security, protect, high, low,
                                 name ? wide : NULL);
}
uintptr_t NT_MS_ABI nt_OpenFileMappingA(uint32_t access, int32_t inherit,
                                        const char *name)
{
    uint16_t wide[SECTION_NAME_LIMIT + 1];
    if (name && !nt_MultiByteToWideChar(nt_GetACP(), 0, name, -1,
                                         wide, SECTION_NAME_LIMIT + 1))
        return 0;
    return nt_OpenFileMappingW(access, inherit, name ? wide : NULL);
}

void *NT_MS_ABI nt_MapViewOfFile(uintptr_t mapping, uint32_t access,
                                uint32_t offset_high, uint32_t offset_low,
                                size_t length)
{
    const uint64_t offset = (uint64_t)offset_high << 32 | offset_low;
    uint32_t requested;
    if (!accept_access(access, &requested) || offset % 65536u != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    pthread_mutex_lock(&section_lock);
    struct section_handle *handle = lookup_handle(mapping);
    if (!handle) {
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return NULL;
    }
    struct section_object *object = handle->object;
    if ((requested & ~handle->allowed) != 0 ||
        (requested & ~section_rights(object->protect)) != 0) {
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return NULL;
    }
    const uint64_t remaining = offset < object->size ? object->size - offset : 0;
    if (!remaining || (length != 0 && length > remaining) ||
        (length == 0 && remaining > SIZE_MAX)) {
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const size_t view_length = length ? length : (size_t)remaining;
    struct section_view *entry = NULL;
    for (size_t i = 0; i < SECTION_VIEW_LIMIT; ++i)
        if (!views[i].base) {
            entry = &views[i];
            break;
        }
    if (!entry) {
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    const bool writable = (requested & (SECTION_MAP_WRITE | SECTION_MAP_COPY)) != 0;
    const int prot = PROT_READ | (writable ? PROT_WRITE : 0);
    const int flags = requested == SECTION_MAP_COPY ? MAP_PRIVATE : MAP_SHARED;
    void *base = mmap(NULL, view_length, prot, flags, object->fd, (off_t)offset);
    if (base == MAP_FAILED) {
        const int error = errno;
        pthread_mutex_unlock(&section_lock);
        nt_SetLastError(mapping_error(error));
        return NULL;
    }
    *entry = (struct section_view){
        .base = base, .size = view_length, .object = object,
        .protect = writable ? SECTION_PAGE_READWRITE : SECTION_PAGE_READONLY
    };
    ++object->references;
    pthread_mutex_unlock(&section_lock);
    return base;
}

int32_t NT_MS_ABI nt_UnmapViewOfFile(const void *address)
{
    if (!address) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&section_lock);
    for (size_t i = 0; i < SECTION_VIEW_LIMIT; ++i) {
        if (views[i].base == address) {
            if (munmap(views[i].base, views[i].size) != 0) {
                pthread_mutex_unlock(&section_lock);
                nt_SetLastError(NT_ERROR_GEN_FAILURE);
                return 0;
            }
            struct section_object *obj = views[i].object;
            memset(&views[i], 0, sizeof(views[i]));
            release_object(obj);
            pthread_mutex_unlock(&section_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&section_lock);
    nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
    return 0;
}

int32_t NT_MS_ABI nt_FlushViewOfFile(const void *address, size_t length)
{
    const uintptr_t at = (uintptr_t)address;
    pthread_mutex_lock(&section_lock);
    for (size_t i = 0; i < SECTION_VIEW_LIMIT; ++i) {
        struct section_view *v = &views[i];
        const uintptr_t start = (uintptr_t)v->base;
        if (v->base && at >= start && at - start < v->size) {
            const size_t remaining = v->size - (at - start);
            if (length > remaining) {
                pthread_mutex_unlock(&section_lock);
                nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
                return 0;
            }
            const uintptr_t aligned = at & ~(uintptr_t)(SECTION_PAGE - 1);
            const size_t amount = length ? length : remaining;
            const size_t actual = amount + (at - aligned);
            int result = msync((void *)aligned, actual, MS_SYNC);
            const int error = errno;
            pthread_mutex_unlock(&section_lock);
            if (result != 0)
                nt_SetLastError(mapping_error(error));
            return result == 0;
        }
    }
    pthread_mutex_unlock(&section_lock);
    nt_SetLastError(NT_ERROR_INVALID_ADDRESS);
    return 0;
}

bool nt_runtime_section_get_flags(uintptr_t number, uint32_t *flags)
{
    bool found = false;
    pthread_mutex_lock(&section_lock);
    struct section_handle *h = lookup_handle(number);
    if (h) {
        if (flags)
            *flags = h->flags;
        found = true;
    }
    pthread_mutex_unlock(&section_lock);
    return found;
}
bool nt_runtime_section_set_flags(uintptr_t number, uint32_t mask, uint32_t flags)
{
    bool found = false;
    pthread_mutex_lock(&section_lock);
    struct section_handle *h = lookup_handle(number);
    if (h) {
        h->flags = (h->flags & ~mask) | (flags & mask);
        found = true;
    }
    pthread_mutex_unlock(&section_lock);
    return found;
}
size_t nt_runtime_section_handle_count(void)
{
    size_t count = 0;
    pthread_mutex_lock(&section_lock);
    for (size_t i = 0; i < SECTION_HANDLE_LIMIT; ++i)
        count += handles[i].object != NULL;
    pthread_mutex_unlock(&section_lock);
    return count;
}
int nt_runtime_section_close(uintptr_t number)
{
    int result = EBADF;
    pthread_mutex_lock(&section_lock);
    struct section_handle *h = lookup_handle(number);
    if (h) {
        if (h->flags & SECTION_HANDLE_PROTECT)
            result = EACCES;
        else {
            struct section_object *obj = h->object;
            memset(h, 0, sizeof(*h));
            release_object(obj);
            result = 0;
        }
    }
    pthread_mutex_unlock(&section_lock);
    return result;
}
nt_status nt_runtime_section_duplicate(uintptr_t source, uint32_t access,
    uint32_t attributes, uint32_t options, uintptr_t *target)
{
    pthread_mutex_lock(&section_lock);
    struct section_handle *original = lookup_handle(source);
    if (!original) {
        pthread_mutex_unlock(&section_lock);
        return NT_STATUS_INVALID_HANDLE;
    }
    /* SAME_ACCESS preserves the source grant. Otherwise attenuate
     * only to one of the access profiles already accepted by
     * OpenFileMappingW; a duplicate never acquires additional rights. */
    uint32_t rights = original->allowed;
    if (!(options & 2u)) {
        if (!accept_access(access, &rights)) {
            pthread_mutex_unlock(&section_lock);
            return NT_STATUS_INVALID_PARAMETER;
        }
        if ((rights & ~original->allowed) != 0) {
            pthread_mutex_unlock(&section_lock);
            return NT_STATUS_ACCESS_DENIED;
        }
    }
    struct section_handle *copy = allocate_handle(original->object,
        rights, (options & 4u) ? original->flags :
        (attributes & 2u) ? SECTION_HANDLE_INHERIT : 0);
    if (!copy) {
        pthread_mutex_unlock(&section_lock);
        return NT_STATUS_NO_MEMORY;
    }
    *target = copy->number;
    pthread_mutex_unlock(&section_lock);
    (void)access;
    return NT_STATUS_SUCCESS;
}
bool nt_runtime_section_query(const void *address,
                              struct nt_memory_basic_information *info)
{
    const uintptr_t at = (uintptr_t)address;
    bool found = false;
    pthread_mutex_lock(&section_lock);
    for (size_t i = 0; i < SECTION_VIEW_LIMIT; ++i) {
        struct section_view *v = &views[i];
        if (v->base && at >= (uintptr_t)v->base &&
            at - (uintptr_t)v->base < v->size) {
            memset(info, 0, sizeof(*info));
            info->base_address = v->base;
            info->allocation_base = v->base;
            info->allocation_protect = v->protect;
            info->region_size = v->size;
            info->state = 0x1000; /* MEM_COMMIT */
            info->protect = v->protect;
            info->type = 0x40000; /* MEM_MAPPED */
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&section_lock);
    return found;
}


/* NTDLL uses the same section-object table as KERNEL32, with NTSTATUS and
 * an independent Win32 LastError. This is a deliberately bounded
 * NtCreateSection/NtMapViewOfSection base, not a general Ldr section server. */
static nt_status section_status_from_last_error(uint32_t error)
{
    switch (error) {
    case NT_ERROR_INVALID_HANDLE: return NT_STATUS_INVALID_HANDLE;
    case NT_ERROR_INVALID_ADDRESS: return NT_STATUS_CONFLICTING_ADDRESSES;
    case NT_ERROR_NOT_ENOUGH_MEMORY: return NT_STATUS_NO_MEMORY;
    case NT_ERROR_ACCESS_DENIED: return NT_STATUS_ACCESS_DENIED;
    case NT_ERROR_NOT_SUPPORTED: return NT_STATUS_NOT_SUPPORTED;
    default: return NT_STATUS_INVALID_PARAMETER;
    }
}

nt_status NT_MS_ABI nt_NtCreateSection(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, const int64_t *maximum,
    uint32_t protect, uint32_t allocation, uintptr_t file)
{
    if (!handle)
        return NT_STATUS_INVALID_PARAMETER;
    *handle = 0;
    uint32_t normalized;
    if (attributes != NULL || file != 0 || !maximum || *maximum <= 0 ||
        (allocation != 0 && allocation != 0x08000000u) ||
        !accept_access(access, &normalized) ||
        (protect != SECTION_PAGE_READONLY &&
         protect != SECTION_PAGE_READWRITE))
        return NT_STATUS_NOT_SUPPORTED;
    if ((normalized & ~section_rights(protect)) != 0)
        return NT_STATUS_ACCESS_DENIED;
    const uint32_t saved = nt_GetLastError();
    const uint64_t amount = (uint64_t)*maximum;
    uintptr_t result = nt_CreateFileMappingW(UINTPTR_MAX, NULL, protect,
                                              (uint32_t)(amount >> 32),
                                              (uint32_t)amount, NULL);
    const uint32_t mapping_error_value = nt_GetLastError();
    nt_SetLastError(saved);
    if (!result)
        return section_status_from_last_error(mapping_error_value);
    pthread_mutex_lock(&section_lock);
    struct section_handle *entry = lookup_handle(result);
    if (entry)
        entry->allowed = normalized;
    pthread_mutex_unlock(&section_lock);
    *handle = result;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtMapViewOfSection(uintptr_t section, uintptr_t process,
    void **base, uintptr_t zero_bits, size_t commit_size, const int64_t *offset,
    size_t *view_size, uint32_t inherit, uint32_t allocation, uint32_t protect)
{
    if (!base || !view_size)
        return NT_STATUS_INVALID_PARAMETER;
    if (process != nt_GetCurrentProcess())
        return NT_STATUS_NOT_SUPPORTED;
    if (*base || zero_bits || commit_size || allocation ||
        (inherit != 1 && inherit != 2) ||
        (protect != SECTION_PAGE_READONLY && protect != SECTION_PAGE_READWRITE) ||
        (offset && *offset < 0))
        return NT_STATUS_NOT_SUPPORTED;
    const uint64_t start = offset ? (uint64_t)*offset : 0;
    const uint32_t access = protect == SECTION_PAGE_READONLY ?
        SECTION_MAP_READ : SECTION_MAP_WRITE;
    const uint32_t saved = nt_GetLastError();
    void *mapped = nt_MapViewOfFile(section, access,
            (uint32_t)(start >> 32), (uint32_t)start, *view_size);
    const uint32_t mapping_error_value = nt_GetLastError();
    nt_SetLastError(saved);
    if (!mapped)
        return section_status_from_last_error(mapping_error_value);
    struct nt_memory_basic_information info;
    if (!nt_runtime_section_query(mapped, &info)) {
        (void)nt_UnmapViewOfFile(mapped);
        nt_SetLastError(saved);
        return NT_STATUS_INVALID_PARAMETER;
    }
    *base = mapped;
    *view_size = info.region_size;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtUnmapViewOfSection(uintptr_t process, const void *base)
{
    if (process != nt_GetCurrentProcess())
        return NT_STATUS_NOT_SUPPORTED;
    const uint32_t saved = nt_GetLastError();
    int32_t result = nt_UnmapViewOfFile(base);
    const uint32_t error = nt_GetLastError();
    nt_SetLastError(saved);
    return result ? NT_STATUS_SUCCESS : section_status_from_last_error(error);
}
