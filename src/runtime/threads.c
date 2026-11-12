/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sync.h"
#include "child_process.h"
#include "nt_services.h"
#include "winbase.h"
#include "ntnative/guest_thread.h"

#include <errno.h>
#include <ctype.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WAITABLE_COUNT = 256, MAXIMUM_WAIT_OBJECTS = 64, FLS_COUNT = 128, STILL_ACTIVE = 259 };
enum { EVENT_QUERY_STATE = 1, EVENT_MODIFY_STATE = 2, READ_CONTROL = 0x20000,
       SYNCHRONIZE = 0x100000, EVENT_ALL_ACCESS = 0x1f0003,
       THREAD_SUSPEND_RESUME = 2, THREAD_QUERY_INFORMATION = 0x40,
       THREAD_QUERY_LIMITED_INFORMATION = 0x800, THREAD_ALL_ACCESS = 0x1fffff };
enum waitable_kind { EVENT, THREAD, PROCESS, SEMAPHORE };
struct waitable {
    uintptr_t handle;
    unsigned int refs;
    /* Independent handles can refer to one event/thread/process state.
     * The canonical slot cannot be recycled while an alias references it. */
    struct waitable *shared;
    unsigned int links;
    uint32_t handle_flags;
    enum waitable_kind kind;
    bool manual, signaled, started;
    int32_t semaphore_count, semaphore_maximum;
    uintptr_t teb_base;
    uint32_t suspend_count;
    uint32_t access;
    pthread_t host;
    uint32_t thread_id, exit_code;
    nt_thread_start start;
    void *parameter;
    /* Canonical named event/semaphore entries own a name. Aliases share
     * the same state, and the object vanishes after the last close. */
    char object_name[128];
};

static struct waitable waitables[WAITABLE_COUNT];
static uintptr_t next_handle = 0x76000001;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local struct waitable *running_thread;
static _Thread_local uint32_t thread_result;
static _Thread_local sigjmp_buf thread_exit;

static struct waitable *find_waitable(uintptr_t handle)
{
    if (handle != 0) {
        for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
            if (waitables[i].handle == handle)
                return &waitables[i];
        }
    }
    return NULL;
}

static struct waitable *new_waitable(enum waitable_kind kind)
{
    if (next_handle == UINTPTR_MAX)
        return NULL;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        if (waitables[i].refs == 0 && waitables[i].links == 0) {
            waitables[i] = (struct waitable){.handle = next_handle++, .refs = 1, .kind = kind,
                                             .access = SYNCHRONIZE};
            return &waitables[i];
        }
    }
    return NULL;
}

static struct waitable *wait_state(struct waitable *handle)
{
    return handle->shared ? handle->shared : handle;
}

/* Releasing an alias after the final in-flight wait removes its reference
 * from the canonical slot. All mutations occur under wait_lock. */
static void release_waitable(struct waitable *object)
{
    if (--object->refs == 0 && object->shared)
        --object->shared->links;
}

bool nt_runtime_close_waitable(uintptr_t handle)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    if (object != NULL) {
        object->handle = 0;
        release_waitable(object);
        pthread_cond_broadcast(&wait_changed);
    }
    pthread_mutex_unlock(&wait_lock);
    return object != NULL;
}

bool nt_runtime_get_waitable_flags(uintptr_t handle, uint32_t *flags)
{
    pthread_mutex_lock(&wait_lock);
    const struct waitable *object = find_waitable(handle);
    if (object && flags)
        *flags = object->handle_flags;
    pthread_mutex_unlock(&wait_lock);
    return object != NULL;
}

_Static_assert(sizeof(struct nt_object_attributes) == 48 &&
               offsetof(struct nt_object_attributes, name) == 16, "AMD64 OBJECT_ATTRIBUTES layout");

/* Bounded ASCII-only names under the local BaseNamedObjects namespace.
 * Do not claim global/session, nested directories or Unicode case folding
 * until a cross-process NT object namespace broker exists. */
static nt_status named_object_name(const struct nt_guest_unicode_string *unicode, char output[128])
{
    output[0] = 0;
    if (unicode == NULL)
        return NT_STATUS_SUCCESS;
    if (unicode->buffer == NULL || unicode->length == 0 ||
        unicode->length % 2 != 0 ||
        unicode->length > unicode->maximum_length ||
        unicode->length >= 128 * sizeof(uint16_t))
        return NT_STATUS_INVALID_PARAMETER;
    const size_t length = unicode->length / sizeof(uint16_t);
    for (size_t i = 0; i < length; ++i) {
        const uint16_t code = unicode->buffer[i];
        if (code < 0x20 || code > 0x7e)
            return NT_STATUS_NOT_SUPPORTED;
        output[i] = (char)code;
    }
    output[length] = 0;
    const char *normalized = output;
    if (strncasecmp(output, "Local\\", 6) == 0)
        normalized += 6;
    else if (strncasecmp(output, "\\BaseNamedObjects\\", 18) == 0)
        normalized += 18;
    else if (strncasecmp(output, "Global\\", 7) == 0 || strchr(output, '\\') != NULL)
        return NT_STATUS_NOT_SUPPORTED;
    if (*normalized == 0 || strchr(normalized, '\\') != NULL)
        return NT_STATUS_INVALID_PARAMETER;
    if (normalized != output)
        memmove(output, normalized, strlen(normalized) + 1);
    return NT_STATUS_SUCCESS;
}

static bool event_name_equal(const char *left, const char *right, bool insensitive)
{
    if (!insensitive)
        return strcmp(left, right) == 0;
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

static struct waitable *find_named_object(const char *name, bool insensitive)
{
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        struct waitable *entry = &waitables[i];
        if ((entry->kind == EVENT || entry->kind == SEMAPHORE) &&
            entry->shared == NULL && entry->object_name[0] &&
            (entry->refs != 0 || entry->links != 0) &&
            event_name_equal(entry->object_name, name, insensitive))
            return entry;
    }
    return NULL;
}

/* Allocate an independent handle referring to a named object's state. */
static struct waitable *open_named_alias(struct waitable *state, uint32_t access, bool inherit)
{
    struct waitable *alias = new_waitable(state->kind);
    if (alias == NULL)
        return NULL;
    ++state->links;
    alias->shared = state;
    alias->access = access;
    alias->handle_flags = inherit ? 1u : 0u;
    return alias;
}

static uint32_t event_access(uint32_t access)
{
    uint32_t rights = access & ~UINT32_C(0xf2000000);
    if (access & UINT32_C(0x80000000)) /* GENERIC_READ */
        rights |= READ_CONTROL | EVENT_QUERY_STATE;
    if (access & UINT32_C(0x40000000)) /* GENERIC_WRITE */
        rights |= READ_CONTROL | EVENT_MODIFY_STATE;
    if (access & UINT32_C(0x20000000)) /* GENERIC_EXECUTE */
        rights |= READ_CONTROL | SYNCHRONIZE;
    if (access & UINT32_C(0x12000000)) /* GENERIC_ALL or MAXIMUM_ALLOWED */
        rights |= EVENT_ALL_ACCESS;
    return rights;
}

nt_status nt_runtime_duplicate_waitable(uintptr_t source, uint32_t access,
                                         uint32_t attributes, uint32_t options,
                                         uintptr_t *target)
{
    if (!target)
        return NT_STATUS_ACCESS_VIOLATION;
    *target = 0;
    if ((options & ~7u) != 0 || (attributes & ~2u) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *original = find_waitable(source);
    nt_status status = NT_STATUS_SUCCESS;
    if (!original) {
        status = NT_STATUS_INVALID_HANDLE;
    } else {
        uint32_t rights = (options & 2u) ? original->access :
                          (original->kind == EVENT || original->kind == SEMAPHORE) ?
                          event_access(access) : access;
        if (rights & ~original->access) {
            status = NT_STATUS_ACCESS_DENIED;
        } else {
            struct waitable *alias = new_waitable(original->kind);
            if (!alias) {
                status = NT_STATUS_NO_MEMORY;
            } else {
                struct waitable *state = wait_state(original);
                ++state->links;
                alias->shared = state;
                alias->access = rights;
                alias->handle_flags = (options & 4u) ? original->handle_flags :
                                      (attributes & 2u) ? 1u : 0u;
                *target = alias->handle;
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtCreateEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes,
                                   uint32_t type, uint8_t initial_state)
{
    if (!handle)
        return NT_STATUS_ACCESS_VIOLATION;
    if (type > 1 || (attributes && attributes->size != sizeof(*attributes)))
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes && (attributes->root || attributes->security || attributes->qos ||
                       (attributes->attributes & ~(0x40u | 2u))))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status name_status = named_object_name(attributes ? attributes->name : NULL, normalized);
    if (name_status != NT_STATUS_SUCCESS)
        return name_status;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    pthread_mutex_lock(&wait_lock);
    nt_status status = NT_STATUS_SUCCESS;
    struct waitable *event = normalized[0] ?
        find_named_object(normalized, attributes && (attributes->attributes & 0x40u)) : NULL;
    if (event != NULL) {
        if (event->kind != EVENT) {
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        } else if (rights & ~event->access) {
            status = NT_STATUS_ACCESS_DENIED;
        } else {
            event = open_named_alias(event, rights, attributes && (attributes->attributes & 2u));
            status = event ? NT_STATUS_OBJECT_NAME_EXISTS : NT_STATUS_NO_MEMORY;
        }
    } else {
        event = new_waitable(EVENT);
        if (event != NULL) {
            event->manual = type == 0; /* NotificationEvent */
            event->signaled = initial_state != 0;
            event->access = rights;
            event->handle_flags = attributes && (attributes->attributes & 2u) ? 1u : 0u;
            if (normalized[0])
                memcpy(event->object_name, normalized, strlen(normalized) + 1);
        } else {
            status = NT_STATUS_NO_MEMORY;
        }
    }
    if (status == NT_STATUS_SUCCESS || status == NT_STATUS_OBJECT_NAME_EXISTS)
        *handle = event->handle;
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtOpenEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (attributes == NULL || attributes->size != sizeof(*attributes) ||
        attributes->name == NULL)
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes->root || attributes->security || attributes->qos ||
        (attributes->attributes & ~(0x40u | 2u)))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status name_status = named_object_name(attributes->name, normalized);
    if (name_status != NT_STATUS_SUCCESS)
        return name_status;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    pthread_mutex_lock(&wait_lock);
    struct waitable *state = find_named_object(normalized, (attributes->attributes & 0x40u) != 0);
    nt_status status = NT_STATUS_SUCCESS;
    if (state == NULL) {
        status = NT_STATUS_OBJECT_NAME_NOT_FOUND;
    } else if (state->kind != EVENT) {
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    } else if (rights & ~state->access) {
        status = NT_STATUS_ACCESS_DENIED;
    } else {
        struct waitable *alias = open_named_alias(state, rights, (attributes->attributes & 2u) != 0);
        if (alias == NULL)
            status = NT_STATUS_NO_MEMORY;
        else
            *handle = alias->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

static struct nt_object_attributes win_event_attributes(const struct nt_guest_unicode_string *name,
                                                         bool inherit)
{
    struct nt_object_attributes attributes = {0};
    attributes.size = sizeof(attributes);
    attributes.name = name;
    attributes.attributes = 0x40u | (inherit ? 2u : 0u);
    return attributes;
}

static struct nt_guest_unicode_string win_event_string(const uint16_t *name)
{
    size_t length = 0;
    if (name != NULL)
        while (length < 127 && name[length])
            ++length;
    const uint16_t bytes = (uint16_t)(length * sizeof(uint16_t));
    return (struct nt_guest_unicode_string){bytes, bytes, (uint16_t *)name};
}

/* Native Win64 Semaphore objects share the same waitable table and the
 * process-local object namespace as named events. Cross-process objects and
 * security descriptors still require a dedicated broker. */
nt_status NT_MS_ABI nt_NtCreateSemaphore(uintptr_t *handle, uint32_t access,
                                         const struct nt_object_attributes *attributes,
                                         int32_t initial, int32_t maximum)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (initial < 0 || maximum <= 0 || initial > maximum ||
        (attributes && attributes->size != sizeof(*attributes)))
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes && (attributes->root || attributes->security || attributes->qos ||
                       (attributes->attributes & ~(0x40u | 2u))))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status name_status = named_object_name(attributes ? attributes->name : NULL, normalized);
    if (name_status != NT_STATUS_SUCCESS)
        return name_status;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = normalized[0] ?
        find_named_object(normalized, attributes && (attributes->attributes & 0x40u)) : NULL;
    nt_status status = NT_STATUS_SUCCESS;
    if (object != NULL) {
        if (object->kind != SEMAPHORE)
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if (rights & ~object->access)
            status = NT_STATUS_ACCESS_DENIED;
        else {
            object = open_named_alias(object, rights, attributes && (attributes->attributes & 2u));
            status = object ? NT_STATUS_OBJECT_NAME_EXISTS : NT_STATUS_NO_MEMORY;
        }
    } else {
        object = new_waitable(SEMAPHORE);
        if (object == NULL) {
            status = NT_STATUS_NO_MEMORY;
        } else {
            object->access = rights;
            object->semaphore_count = initial;
            object->semaphore_maximum = maximum;
            object->signaled = initial > 0;
            object->handle_flags = attributes && (attributes->attributes & 2u) ? 1u : 0u;
            if (normalized[0])
                memcpy(object->object_name, normalized, strlen(normalized) + 1);
        }
    }
    if (status == NT_STATUS_SUCCESS || status == NT_STATUS_OBJECT_NAME_EXISTS)
        *handle = object->handle;
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtOpenSemaphore(uintptr_t *handle, uint32_t access,
                                       const struct nt_object_attributes *attributes)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (attributes == NULL || attributes->size != sizeof(*attributes) || attributes->name == NULL)
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes->root || attributes->security || attributes->qos ||
        (attributes->attributes & ~(0x40u | 2u)))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status parsed = named_object_name(attributes->name, normalized);
    if (parsed != NT_STATUS_SUCCESS)
        return parsed;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    pthread_mutex_lock(&wait_lock);
    struct waitable *state = find_named_object(normalized, (attributes->attributes & 0x40u) != 0);
    nt_status status = NT_STATUS_SUCCESS;
    if (state == NULL)
        status = NT_STATUS_OBJECT_NAME_NOT_FOUND;
    else if (state->kind != SEMAPHORE)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (rights & ~state->access)
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *alias = open_named_alias(state, rights, (attributes->attributes & 2u) != 0);
        if (alias == NULL)
            status = NT_STATUS_NO_MEMORY;
        else
            *handle = alias->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

struct nt_semaphore_basic_information {
    int32_t current_count, maximum_count;
};
_Static_assert(sizeof(struct nt_semaphore_basic_information) == 8, "SEMAPHORE_BASIC_INFORMATION ABI");

nt_status NT_MS_ABI nt_NtQuerySemaphore(uintptr_t handle, uint32_t information_class,
                                        void *buffer, uint32_t length, uint32_t *returned_length)
{
    if (information_class != 0)
        return NT_STATUS_INVALID_INFO_CLASS;
    if (returned_length != NULL)
        *returned_length = sizeof(struct nt_semaphore_basic_information);
    if (buffer == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length < sizeof(struct nt_semaphore_basic_information))
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (entry == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != SEMAPHORE)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(entry->access & EVENT_QUERY_STATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        const struct waitable *state = wait_state(entry);
        const struct nt_semaphore_basic_information info = {
            state->semaphore_count, state->semaphore_maximum
        };
        memcpy(buffer, &info, sizeof(info));
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtReleaseSemaphore(uintptr_t handle, int32_t count, int32_t *previous)
{
    if (count <= 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (entry == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != SEMAPHORE)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(entry->access & EVENT_MODIFY_STATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(entry);
        if (count > state->semaphore_maximum - state->semaphore_count) {
            status = NT_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        } else {
            if (previous != NULL)
                *previous = state->semaphore_count;
            state->semaphore_count += count;
            state->signaled = true;
            pthread_cond_broadcast(&wait_changed);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

uintptr_t NT_MS_ABI nt_CreateSemaphoreW(const void *security, int32_t initial,
                                         int32_t maximum, const uint16_t *name)
{
    if (security != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (name != NULL && *name && unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, false);
    uintptr_t handle = 0;
    const nt_status status = nt_NtCreateSemaphore(&handle, EVENT_ALL_ACCESS,
                                                  name && *name ? &attrs : NULL,
                                                  initial, maximum);
    nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

uintptr_t NT_MS_ABI nt_OpenSemaphoreW(uint32_t access, int32_t inherit, const uint16_t *name)
{
    if (name == NULL || !*name) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, inherit != 0);
    uintptr_t handle = 0;
    const nt_status status = nt_NtOpenSemaphore(&handle, access, &attrs);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

int32_t NT_MS_ABI nt_ReleaseSemaphore(uintptr_t handle, int32_t count, int32_t *previous)
{
    const nt_status status = nt_NtReleaseSemaphore(handle, count, previous);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

uintptr_t NT_MS_ABI nt_CreateEventW(const void *security, int32_t manual, int32_t initial,
                                    const uint16_t *name)
{
    if (security != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (name != NULL && name[0] != 0 && unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, false);
    uintptr_t handle = 0;
    const nt_status status = nt_NtCreateEvent(&handle, EVENT_ALL_ACCESS,
                                               name && *name ? &attrs : NULL,
                                               manual ? 0 : 1, initial != 0);
    nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

uintptr_t NT_MS_ABI nt_OpenEventW(uint32_t access, int32_t inherit, const uint16_t *name)
{
    if (name == NULL || !*name) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, inherit != 0);
    uintptr_t handle = 0;
    const nt_status status = nt_NtOpenEvent(&handle, access, &attrs);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

struct nt_event_basic_information {
    uint32_t type;
    int32_t state;
};

nt_status NT_MS_ABI nt_NtQueryEvent(uintptr_t handle, uint32_t information_class,
                                    void *buffer, uint32_t length, uint32_t *returned_length)
{
    if (information_class != 0)
        return NT_STATUS_INVALID_INFO_CLASS;
    if (returned_length)
        *returned_length = sizeof(struct nt_event_basic_information);
    if (buffer == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length < sizeof(struct nt_event_basic_information))
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (entry == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != EVENT)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(entry->access & EVENT_QUERY_STATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(entry);
        const struct nt_event_basic_information information =
            {(uint32_t)(state->manual ? 0 : 1), state->signaled ? 1 : 0};
        memcpy(buffer, &information, sizeof(information));
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

static nt_status change_event(uintptr_t handle, bool signaled, int32_t *previous)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *event = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (!event) {
        status = NT_STATUS_INVALID_HANDLE;
    } else if (event->kind != EVENT) {
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    } else if (!(event->access & EVENT_MODIFY_STATE)) {
        status = NT_STATUS_ACCESS_DENIED;
    } else {
        struct waitable *state = wait_state(event);
        if (previous)
            *previous = state->signaled;
        state->signaled = signaled;
        if (signaled)
            pthread_cond_broadcast(&wait_changed);
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtSetEvent(uintptr_t handle, int32_t *previous) { return change_event(handle, true, previous); }
nt_status NT_MS_ABI nt_NtResetEvent(uintptr_t handle, int32_t *previous) { return change_event(handle, false, previous); }

static int32_t win_change_event(uintptr_t handle, bool signaled)
{
    const nt_status status = change_event(handle, signaled, NULL);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

int32_t NT_MS_ABI nt_SetEvent(uintptr_t handle) { return win_change_event(handle, true); }
int32_t NT_MS_ABI nt_ResetEvent(uintptr_t handle) { return win_change_event(handle, false); }

/* Called with wait_lock held. References keep closed slots from being reused. */
static nt_status acquire_wait_objects(struct waitable **objects, uint32_t count, const uintptr_t *handles)
{
    for (uint32_t i = 0; i < count; ++i) {
        struct waitable *object = find_waitable(handles[i]);
        if (!object)
            return NT_STATUS_INVALID_HANDLE;
        if (!(object->access & SYNCHRONIZE))
            return NT_STATUS_ACCESS_DENIED;
        for (uint32_t j = 0; j < i; ++j) {
            if (wait_state(objects[j]) == wait_state(object))
                return NT_STATUS_INVALID_PARAMETER;
        }
        objects[i] = object;
    }
    for (uint32_t i = 0; i < count; ++i)
        ++objects[i]->refs;
    return NT_STATUS_SUCCESS;
}

static nt_status check_wait_objects(struct waitable *const *objects, uint32_t count, bool all)
{
    uint32_t first = count, signaled = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!objects[i]->handle)
            return NT_STATUS_INVALID_HANDLE;
        if (wait_state(objects[i])->signaled) {
            if (!signaled)
                first = i;
            ++signaled;
        }
    }
    if (all ? signaled != count : signaled == 0)
        return NT_STATUS_TIMEOUT;
    const uint32_t start = all ? 0 : first;
    const uint32_t end = all ? count : first + 1;
    for (uint32_t i = start; i < end; ++i) {
        struct waitable *state = wait_state(objects[i]);
        if (objects[i]->kind == EVENT && !state->manual)
            state->signaled = false;
        if (objects[i]->kind == SEMAPHORE) {
            --state->semaphore_count;
            state->signaled = state->semaphore_count > 0;
        }
    }
    return all ? NT_STATUS_SUCCESS : (nt_status)first;
}

nt_status NT_MS_ABI nt_NtWaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                              uint32_t type, uint8_t alertable, const int64_t *timeout)
{
    (void)alertable; /* No guest APC producer is registered. */
    if (!count || count > MAXIMUM_WAIT_OBJECTS || type > 1)
        return NT_STATUS_INVALID_PARAMETER;
    if (!handles)
        return NT_STATUS_ACCESS_VIOLATION;
    struct nt_wait_deadline deadline;
    if (timeout) {
        const nt_status status = nt_runtime_wait_deadline(*timeout, &deadline);
        if (status != NT_STATUS_SUCCESS)
            return status;
    }
    struct waitable *objects[MAXIMUM_WAIT_OBJECTS];
    pthread_mutex_lock(&wait_lock);
    nt_status result = acquire_wait_objects(objects, count, handles);
    if (result == NT_STATUS_SUCCESS) {
        int error = timeout && deadline.time.tv_sec < 0 ? ETIMEDOUT : 0;
        for (;;) {
            result = check_wait_objects(objects, count, type == 0);
            if (result != NT_STATUS_TIMEOUT || error)
                break;
            error = timeout ? pthread_cond_clockwait(&wait_changed, &wait_lock, deadline.clock, &deadline.time) :
                              pthread_cond_wait(&wait_changed, &wait_lock);
        }
        if (result == NT_STATUS_TIMEOUT && error != ETIMEDOUT)
            result = NT_STATUS_UNSUCCESSFUL;
        for (uint32_t i = 0; i < count; ++i)
            release_waitable(objects[i]);
    }
    pthread_mutex_unlock(&wait_lock);
    return result;
}

nt_status NT_MS_ABI nt_NtWaitForSingleObject(uintptr_t handle, uint8_t alertable, const int64_t *timeout)
{
    return nt_NtWaitForMultipleObjects(1, &handle, 1, alertable, timeout);
}

uint32_t NT_MS_ABI nt_WaitForMultipleObjectsEx(uint32_t count, const uintptr_t *handles,
                                            int32_t all, uint32_t milliseconds, int32_t alertable)
{
    const int64_t ticks = -(int64_t)milliseconds * 10000;
    const nt_status status = nt_NtWaitForMultipleObjects(count, handles, all ? 0 : 1, alertable != 0,
                                                        milliseconds == UINT32_MAX ? NULL : &ticks);
    if (status >= 0)
        return (uint32_t)status;
    nt_SetLastError(nt_runtime_nt_error(status));
    return UINT32_MAX;
}

uint32_t NT_MS_ABI nt_WaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                          int32_t all, uint32_t milliseconds)
{
    return nt_WaitForMultipleObjectsEx(count, handles, all, milliseconds, 0);
}

uint32_t NT_MS_ABI nt_WaitForSingleObjectEx(uintptr_t handle, uint32_t milliseconds, int32_t alertable)
{
    return nt_WaitForMultipleObjectsEx(1, &handle, 0, milliseconds, alertable);
}

uint32_t NT_MS_ABI nt_WaitForSingleObject(uintptr_t handle, uint32_t milliseconds)
{
    return nt_WaitForSingleObjectEx(handle, milliseconds, 0);
}

struct child_watch {
    pid_t pid;
    int status_fd;
    struct waitable *process, *thread;
};

static void *reap_child(void *context)
{
    struct child_watch *watch = context;
    int status = 0;
    pid_t result;
    do {
        result = waitpid(watch->pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    uint32_t code = NT_ERROR_GEN_FAILURE;
    if (result == watch->pid && WIFEXITED(status)) {
        ssize_t bytes;
        do {
            bytes = read(watch->status_fd, &code, sizeof(code));
        } while (bytes < 0 && errno == EINTR);
        if (bytes != (ssize_t)sizeof(code))
            code = (uint32_t)WEXITSTATUS(status);
    } else if (result == watch->pid && WIFSIGNALED(status)) {
        code = (uint32_t)(128 + WTERMSIG(status));
    }
    close(watch->status_fd);
    pthread_mutex_lock(&wait_lock);
    watch->process->exit_code = watch->thread->exit_code = code;
    watch->process->signaled = watch->thread->signaled = true;
    --watch->process->refs;
    --watch->thread->refs;
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
    free(watch);
    return NULL;
}

bool nt_runtime_watch_child(pid_t pid, int status_fd, struct nt_process_information *info)
{
    struct child_watch *watch = malloc(sizeof(*watch));
    pthread_attr_t attr;
    if (!watch)
        return false;
    if (pthread_attr_init(&attr) != 0) {
        free(watch);
        return false;
    }
    int error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&wait_lock);
    struct waitable *process = error == 0 ? new_waitable(PROCESS) : NULL;
    struct waitable *thread = process ? new_waitable(THREAD) : NULL;
    if (thread) {
        ++process->refs;
        ++thread->refs;
        thread->thread_id = (uint32_t)pid;
        thread->access = THREAD_ALL_ACCESS;
        *watch = (struct child_watch){pid, status_fd, process, thread};
        pthread_t worker;
        error = pthread_create(&worker, &attr, reap_child, watch);
        if (!error)
            *info = (struct nt_process_information){process->handle, thread->handle,
                                                   (uint32_t)pid, (uint32_t)pid};
    } else {
        error = ENOMEM;
    }
    if (error) {
        if (process)
            *process = (struct waitable){0};
        if (thread)
            *thread = (struct waitable){0};
        free(watch);
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_attr_destroy(&attr);
    return error == 0;
}

int32_t NT_MS_ABI nt_GetExitCodeProcess(uintptr_t handle, uint32_t *code)
{
    if (!code) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (handle == UINTPTR_MAX) { /* GetCurrentProcess */
        *code = STILL_ACTIVE;
        return 1;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    bool valid = object && object->kind == PROCESS;
    if (valid) {
        const struct waitable *state = wait_state(object);
        *code = state->signaled ? state->exit_code : STILL_ACTIVE;
    }
    pthread_mutex_unlock(&wait_lock);
    if (!valid)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return valid;
}

uintptr_t NT_MS_ABI nt_GetCurrentThread(void) { return UINTPTR_MAX - 1; }
/* Thread metadata is kept in the same canonical table as wait handles, so
 * OpenThread and native queries observe the same guest TEB and thread ID.
 * No host pthread_t is returned as a Win32 HANDLE. */
nt_status nt_runtime_query_thread(uintptr_t handle, uint32_t *thread_id,
                                   uintptr_t *teb, uint32_t *exit_code)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(object->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        const struct waitable *state = wait_state(object);
        if (thread_id != NULL)
            *thread_id = state->thread_id;
        if (teb != NULL)
            *teb = state->teb_base;
        if (exit_code != NULL)
            *exit_code = state->signaled ? state->exit_code : STILL_ACTIVE;
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

uint32_t NT_MS_ABI nt_GetThreadId(uintptr_t handle)
{
    if (handle == nt_GetCurrentThread())
        return (uint32_t)gettid();
    uint32_t id = 0;
    const nt_status status = nt_runtime_query_thread(handle, &id, NULL, NULL);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS ? id : 0;
}

uintptr_t NT_MS_ABI nt_OpenThread(uint32_t access, int32_t inherit, uint32_t id)
{
    if (id == 0 || (access & ~THREAD_ALL_ACCESS) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *original = NULL;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        struct waitable *item = &waitables[i];
        if (item->kind == THREAD && item->shared == NULL &&
            (item->refs != 0 || item->links != 0) && item->thread_id == id) {
            original = item;
            break;
        }
    }
    uintptr_t result = 0;
    uint32_t error = NT_ERROR_INVALID_PARAMETER;
    if (original != NULL) {
        if (access & ~original->access) {
            error = NT_ERROR_ACCESS_DENIED;
        } else {
            struct waitable *alias = new_waitable(THREAD);
            if (alias == NULL)
                error = NT_ERROR_NOT_ENOUGH_MEMORY;
            else {
                ++original->links;
                alias->shared = original;
                alias->access = access;
                alias->handle_flags = inherit != 0 ? 1u : 0u;
                result = alias->handle;
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (result == 0)
        nt_SetLastError(error);
    return result;
}


static void finish_thread(void *state)
{
    struct nt_guest_thread *guest = state;
    nt_runtime_run_fls_cleanup();
    if (guest->entered) {
        nt_runtime_notify_tls(3); /* DLL_THREAD_DETACH */
        nt_guest_thread_leave(guest);
    }
    pthread_mutex_lock(&wait_lock);
    running_thread->exit_code = thread_result;
    running_thread->teb_base = 0; /* stack-allocated TEB is no longer live */
    running_thread->signaled = true;
    --running_thread->refs;
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
}

static void *thread_main(void *context)
{
    running_thread = context;
    struct nt_guest_thread guest = {0};
    const uintptr_t image = nt_runtime_guest_module();
    const bool ready = image == 0 || nt_runtime_enter_guest_thread(&guest, image, nt_runtime_guest_tls());
    pthread_mutex_lock(&wait_lock);
    running_thread->thread_id = (uint32_t)gettid();
    running_thread->teb_base = guest.entered ? (uintptr_t)guest.teb : 0;
    running_thread->started = true;
    pthread_cond_broadcast(&wait_changed);
    /* CREATE_SUSPENDED must prevent guest instructions, TLS callbacks and
     * the user's thread entry from running until ResumeThread. */
    while (running_thread->suspend_count != 0)
        pthread_cond_wait(&wait_changed, &wait_lock);
    pthread_mutex_unlock(&wait_lock);
    if (sigsetjmp(thread_exit, 1) == 0) {
        if (guest.entered)
            nt_runtime_notify_tls(2); /* DLL_THREAD_ATTACH */
        thread_result = ready ? running_thread->start(running_thread->parameter) : NT_ERROR_GEN_FAILURE;
    }
    finish_thread(&guest);
    return NULL;
}

uintptr_t NT_MS_ABI nt_CreateThread(const void *security, size_t stack, nt_thread_start start,
                                   void *parameter, uint32_t flags, uint32_t *thread_id)
{
    if (start == NULL || security != NULL || (flags & ~4u) != 0) {
        nt_SetLastError(start == NULL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    int error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (error == 0 && stack != 0) {
        const size_t minimum = (size_t)PTHREAD_STACK_MIN;
        error = pthread_attr_setstacksize(&attr, stack < minimum ? minimum : stack);
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *thread = error == 0 ? new_waitable(THREAD) : NULL;
    uintptr_t handle = 0;
    if (thread != NULL) {
        thread->start = start;
        thread->parameter = parameter;
        thread->access = THREAD_ALL_ACCESS;
        thread->suspend_count = (flags & 4u) ? 1u : 0u;
        ++thread->refs;
        error = pthread_create(&thread->host, &attr, thread_main, thread);
        if (error == 0) {
            while (!thread->started)
                pthread_cond_wait(&wait_changed, &wait_lock);
            handle = thread->handle;
            if (thread_id != NULL)
                *thread_id = thread->thread_id;
        } else {
            thread->handle = 0;
            thread->refs = 0;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_attr_destroy(&attr);
    if (handle == 0)
        nt_SetLastError(error == EINVAL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_NOT_ENOUGH_MEMORY);
    return handle;
}

/* A bounded startup-suspension contract. Running-thread asynchronous
 * suspension needs a separate guest scheduler and is not implemented. */
static nt_status resume_guest_thread(uintptr_t handle, uint32_t *previous)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *thread = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (thread == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (thread->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(thread->access & THREAD_SUSPEND_RESUME)) {
        status = NT_STATUS_ACCESS_DENIED;
    } else {
        struct waitable *state = wait_state(thread);
        if (previous != NULL)
            *previous = state->suspend_count;
        if (state->suspend_count != 0) {
            --state->suspend_count;
            pthread_cond_broadcast(&wait_changed);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

uint32_t NT_MS_ABI nt_ResumeThread(uintptr_t handle)
{
    uint32_t previous = UINT32_MAX;
    const nt_status status = resume_guest_thread(handle, &previous);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS ? previous : UINT32_MAX;
}

nt_status NT_MS_ABI nt_NtResumeThread(uintptr_t handle, uint32_t *previous)
{
    return resume_guest_thread(handle, previous);
}

void NT_MS_ABI nt_ExitThread(uint32_t code)
{
    if (running_thread != NULL) {
        thread_result = code;
        siglongjmp(thread_exit, 1);
    }
    nt_runtime_run_fls_cleanup();
    nt_ExitProcess(code);
}

void NT_MS_ABI nt_FreeLibraryAndExitThread(uintptr_t module, uint32_t code)
{
    nt_FreeLibrary(module);
    nt_ExitThread(code);
}

int32_t NT_MS_ABI nt_GetExitCodeThread(uintptr_t handle, uint32_t *code)
{
    if (code == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (handle == nt_GetCurrentThread()) {
        *code = STILL_ACTIVE;
        return 1;
    }
    pthread_mutex_lock(&wait_lock);
    const struct waitable *thread = find_waitable(handle);
    const bool valid = thread != NULL && thread->kind == THREAD;
    if (valid) {
        const struct waitable *state = wait_state((struct waitable *)thread);
        *code = state->signaled ? state->exit_code : STILL_ACTIVE;
    }
    pthread_mutex_unlock(&wait_lock);
    if (!valid)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return valid;
}

int32_t NT_MS_ABI nt_SetThreadDescription(uintptr_t handle, const uint16_t *description)
{
    if (description == NULL)
        return (int32_t)UINT32_C(0x80070057);
    const size_t length = nt_runtime_wide_length(description, 32767);
    if (length == 32767)
        return (int32_t)UINT32_C(0x80070057);
    char *name = nt_runtime_utf16_to_utf8(description, length);
    if (name == NULL)
        return (int32_t)UINT32_C(0x8007000e);
    size_t bytes = strlen(name);
    if (bytes > 15) {
        bytes = 15;
        while (((unsigned char)name[bytes] & 0xc0) == 0x80)
            --bytes;
        name[bytes] = '\0';
    }
    pthread_mutex_lock(&wait_lock);
    const struct waitable *thread = find_waitable(handle);
    int error = ESRCH;
    if (handle == nt_GetCurrentThread())
        error = pthread_setname_np(pthread_self(), name);
    else if (thread != NULL && thread->kind == THREAD &&
             !wait_state((struct waitable *)thread)->signaled)
        error = pthread_setname_np(wait_state((struct waitable *)thread)->host, name);
    pthread_mutex_unlock(&wait_lock);
    free(name);
    return error == 0 ? 0 : (int32_t)UINT32_C(0x80070006);
}

struct fls_slot { nt_fls_callback callback; bool used; };
struct fls_thread { struct fls_thread *next; void *values[FLS_COUNT]; };
static struct fls_slot slots[FLS_COUNT];
static struct fls_thread *fls_threads;
static pthread_once_t fls_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t fls_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t fls_key;
static int fls_error;

static void release_fls_thread(void *context)
{
    struct fls_thread *thread = context;
    nt_fls_callback callbacks[FLS_COUNT];
    pthread_mutex_lock(&fls_lock);
    struct fls_thread **link = &fls_threads;
    while (*link != thread)
        link = &(*link)->next;
    *link = thread->next;
    for (size_t i = 0; i < FLS_COUNT; ++i)
        callbacks[i] = slots[i].callback;
    pthread_mutex_unlock(&fls_lock);
    for (size_t i = 0; i < FLS_COUNT; ++i) {
        if (thread->values[i] != NULL && callbacks[i] != NULL)
            callbacks[i](thread->values[i]);
    }
    free(thread);
}

static void create_fls_key(void) { fls_error = pthread_key_create(&fls_key, release_fls_thread); }

static bool fls_ready(void)
{
    return pthread_once(&fls_once, create_fls_key) == 0 && fls_error == 0;
}

void nt_runtime_run_fls_cleanup(void)
{
    if (!fls_ready())
        return;
    struct fls_thread *thread = pthread_getspecific(fls_key);
    for (int attempt = 0; thread != NULL && attempt < 4; ++attempt) {
        pthread_setspecific(fls_key, NULL);
        release_fls_thread(thread);
        thread = pthread_getspecific(fls_key);
    }
}

uint32_t NT_MS_ABI nt_FlsAlloc(nt_fls_callback callback)
{
    if (!fls_ready()) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINT32_MAX;
    }
    pthread_mutex_lock(&fls_lock);
    uint32_t index = 0;
    while (index < FLS_COUNT && slots[index].used)
        ++index;
    if (index < FLS_COUNT)
        slots[index] = (struct fls_slot){callback, true};
    pthread_mutex_unlock(&fls_lock);
    if (index == FLS_COUNT) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINT32_MAX;
    }
    return index;
}

int32_t NT_MS_ABI nt_FlsFree(uint32_t index)
{
    pthread_mutex_lock(&fls_lock);
    if (index >= FLS_COUNT || !slots[index].used) {
        pthread_mutex_unlock(&fls_lock);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const nt_fls_callback callback = slots[index].callback;
    size_t count = 0;
    if (callback != NULL) {
        for (const struct fls_thread *t = fls_threads; t != NULL; t = t->next)
            count += t->values[index] != NULL;
    }
    void **values = count != 0 ? malloc(count * sizeof(*values)) : NULL;
    if (count != 0 && values == NULL) {
        pthread_mutex_unlock(&fls_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    size_t at = 0;
    for (struct fls_thread *t = fls_threads; t != NULL; t = t->next) {
        if (callback != NULL && t->values[index] != NULL)
            values[at++] = t->values[index];
        t->values[index] = NULL;
    }
    slots[index] = (struct fls_slot){0};
    pthread_mutex_unlock(&fls_lock);
    for (size_t i = 0; i < count; ++i)
        callback(values[i]);
    free(values);
    return 1;
}

void *NT_MS_ABI nt_FlsGetValue(uint32_t index)
{
    pthread_mutex_lock(&fls_lock);
    const bool valid = index < FLS_COUNT && slots[index].used && fls_ready();
    const struct fls_thread *thread = valid ? pthread_getspecific(fls_key) : NULL;
    void *value = thread != NULL ? thread->values[index] : NULL;
    pthread_mutex_unlock(&fls_lock);
    nt_SetLastError(valid ? NT_ERROR_SUCCESS : NT_ERROR_INVALID_PARAMETER);
    return value;
}

int32_t NT_MS_ABI nt_FlsSetValue(uint32_t index, void *value)
{
    pthread_mutex_lock(&fls_lock);
    uint32_t error = NT_ERROR_SUCCESS;
    if (index >= FLS_COUNT || !slots[index].used || !fls_ready()) {
        error = NT_ERROR_INVALID_PARAMETER;
    } else {
        struct fls_thread *thread = pthread_getspecific(fls_key);
        if (thread == NULL && value != NULL) {
            thread = calloc(1, sizeof(*thread));
            if (thread == NULL || pthread_setspecific(fls_key, thread) != 0) {
                free(thread);
                thread = NULL;
                error = NT_ERROR_NOT_ENOUGH_MEMORY;
            } else {
                thread->next = fls_threads;
                fls_threads = thread;
            }
        }
        if (thread != NULL)
            thread->values[index] = value;
    }
    pthread_mutex_unlock(&fls_lock);
    if (error != NT_ERROR_SUCCESS)
        nt_SetLastError(error);
    return error == NT_ERROR_SUCCESS;
}
