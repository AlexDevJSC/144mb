/* SPDX-License-Identifier: MIT */
#include "dynamic_tls.h"
#include "ntnative/guest_thread.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/* The initial implementation deliberately supports only the first 64
 * in-TEB Win64 TLS slots. ExpansionSlots at TEB+0x1780 and FLS have
 * separate semantics and are not silently emulated by these slots. */
static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
static bool allocated[NT_GUEST_DYNAMIC_TLS_SLOTS];
static uint64_t generations[NT_GUEST_DYNAMIC_TLS_SLOTS];

_Static_assert(NT_TEB_TLS_SLOTS + NT_GUEST_DYNAMIC_TLS_SLOTS * sizeof(uintptr_t) <=
               NT_GUEST_TEB_BYTES, "Win64 TEB TLS slot range must be mapped");

static void slot_access(struct nt_guest_thread *thread, uint32_t index, void *value, bool write)
{
    unsigned char *slot = thread->teb + NT_TEB_TLS_SLOTS + (size_t)index * sizeof(uintptr_t);
    if (write)
        memcpy(slot, value, sizeof(uintptr_t));
    else
        memcpy(value, slot, sizeof(uintptr_t));
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
        slot_access(thread, index, &zero, true);
        thread->dynamic_tls_epochs[index] = generations[index];
    }
    uintptr_t raw = 0;
    slot_access(thread, index, &raw, false);
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
    slot_access(thread, index, &raw, true);
    thread->dynamic_tls_epochs[index] = generations[index];
    pthread_mutex_unlock(&tls_lock);
    return 1;
}
