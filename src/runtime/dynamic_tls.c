/* SPDX-License-Identifier: MIT */
#include "dynamic_tls.h"
#include "ntnative/guest_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The 64 inline Win64 TEB slots and the optional 1024 expansion slots are
 * process-allocated indices holding thread-private values. FLS and static
 * PE TLS are different mechanisms and keep their existing implementations. */
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static bool allocated[NT_GUEST_DYNAMIC_TLS_SLOTS];
static uint64_t generations[NT_GUEST_DYNAMIC_TLS_SLOTS];

_Static_assert(NT_TEB_TLS_SLOTS + NT_GUEST_DYNAMIC_TLS_INLINE * sizeof(uintptr_t) <=
               NT_GUEST_TEB_BYTES, "Win64 TEB inline TLS slot range must be mapped");
_Static_assert(NT_TEB_TLS_EXPANSION + sizeof(uintptr_t) <= NT_GUEST_TEB_BYTES,
               "Win64 TEB expansion pointer must be mapped");

/* The optional expansion array is allocated per guest thread only when the
 * application first stores a non-NULL value in a slot beyond index 63. */
static bool slot_access(struct nt_guest_thread *thread, uint32_t index,
                        uintptr_t *value, bool write)
{
    if (index < NT_GUEST_DYNAMIC_TLS_INLINE) {
        unsigned char *slot = thread->teb + NT_TEB_TLS_SLOTS + (size_t)index * sizeof(*value);
        if (write)
            memcpy(slot, value, sizeof(*value));
        else
            memcpy(value, slot, sizeof(*value));
        return true;
    }
    if (thread->tls_expansion == NULL) {
        if (!write || *value == 0) {
            if (!write)
                *value = 0;
            return true;
        }
        uintptr_t *slots = calloc(NT_GUEST_DYNAMIC_TLS_EXPANDED, sizeof(*slots));
        if (slots == NULL)
            return false;
        thread->tls_expansion = slots;
        const uintptr_t pointer = (uintptr_t)slots;
        memcpy(thread->teb + NT_TEB_TLS_EXPANSION, &pointer, sizeof(pointer));
    }
    uintptr_t *slot = thread->tls_expansion + index - NT_GUEST_DYNAMIC_TLS_INLINE;
    if (write)
        memcpy(slot, value, sizeof(*value));
    else
        memcpy(value, slot, sizeof(*value));
    return true;
}

static bool valid_locked(uint32_t index)
{
    return index < NT_GUEST_DYNAMIC_TLS_SLOTS && allocated[index];
}

uint32_t NT_MS_ABI nt_TlsAlloc(void)
{
    pthread_mutex_lock(&tls_lock);
    uint32_t index = 0;
    while (index < NT_GUEST_DYNAMIC_TLS_SLOTS && allocated[index])
        ++index;
    if (index < NT_GUEST_DYNAMIC_TLS_SLOTS) {
        allocated[index] = true;
        if (generations[index] == 0)
            generations[index] = 1;
    }
    pthread_mutex_unlock(&tls_lock);
    if (index == NT_GUEST_DYNAMIC_TLS_SLOTS) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINT32_MAX; /* TLS_OUT_OF_INDEXES */
    }
    return index;
}

int32_t NT_MS_ABI nt_TlsFree(uint32_t index)
{
    pthread_mutex_lock(&tls_lock);
    const bool valid = valid_locked(index);
    if (valid) {
        allocated[index] = false;
        ++generations[index];
        /* A wrapped generation remains distinguishable from the original
         * zero/uninitialized state, but this case is unattainable in practice. */
        if (generations[index] == 0)
            generations[index] = 1;
    }
    pthread_mutex_unlock(&tls_lock);
    if (!valid)
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
    return valid;
}

void *NT_MS_ABI nt_TlsGetValue(uint32_t index)
{
    struct nt_guest_thread *thread = nt_guest_thread_current();
    pthread_mutex_lock(&tls_lock);
    if (thread == NULL || !valid_locked(index)) {
        pthread_mutex_unlock(&tls_lock);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (thread->dynamic_tls_epochs[index] != generations[index]) {
        uintptr_t zero = 0;
        (void)slot_access(thread, index, &zero, true);
        thread->dynamic_tls_epochs[index] = generations[index];
    }
    uintptr_t raw = 0;
    (void)slot_access(thread, index, &raw, false);
    pthread_mutex_unlock(&tls_lock);
    /* Win32 distinguishes an empty slot from a failed TlsGetValue by
     * clearing LastError even when the result is NULL. */
    nt_SetLastError(NT_ERROR_SUCCESS);
    return (void *)raw;
}

int32_t NT_MS_ABI nt_TlsSetValue(uint32_t index, void *value)
{
    struct nt_guest_thread *thread = nt_guest_thread_current();
    pthread_mutex_lock(&tls_lock);
    if (thread == NULL || !valid_locked(index)) {
        pthread_mutex_unlock(&tls_lock);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uintptr_t raw = (uintptr_t)value;
    const bool ok = slot_access(thread, index, &raw, true);
    if (ok)
        thread->dynamic_tls_epochs[index] = generations[index];
    pthread_mutex_unlock(&tls_lock);
    if (!ok)
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    return ok;
}
