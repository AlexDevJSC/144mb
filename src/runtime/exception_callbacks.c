/* SPDX-License-Identifier: MIT */
#include "exception_callbacks.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

/* Windows VEH handlers are process-scoped and ordered. Dispatch callbacks
 * outside the lock so nested RaiseException and self-removal are possible.
 * The snapshot also avoids invoking a freed callback-entry allocation.
 * This module does not attempt async Linux signal handling. */
enum { VECTORED_HANDLER_MAX = 64 };
struct vectored_slot {
    uintptr_t token;
    nt_vectored_exception_handler callback;
};

static struct vectored_slot handlers[VECTORED_HANDLER_MAX];
static size_t handler_count;
static uintptr_t next_token = 0x70000000;
static pthread_mutex_t handler_lock = PTHREAD_MUTEX_INITIALIZER;

void *NT_MS_ABI nt_AddVectoredExceptionHandler(uint32_t first, nt_vectored_exception_handler callback)
{
    if (callback == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    pthread_mutex_lock(&handler_lock);
    if (handler_count == VECTORED_HANDLER_MAX || next_token == UINTPTR_MAX) {
        pthread_mutex_unlock(&handler_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    const struct vectored_slot next = {next_token++, callback};
    if (first != 0) {
        memmove(handlers + 1, handlers, handler_count * sizeof(*handlers));
        handlers[0] = next;
    } else {
        handlers[handler_count] = next;
    }
    ++handler_count;
    pthread_mutex_unlock(&handler_lock);
    return (void *)next.token;
}

uint32_t NT_MS_ABI nt_RemoveVectoredExceptionHandler(void *handle)
{
    const uintptr_t token = (uintptr_t)handle;
    pthread_mutex_lock(&handler_lock);
    size_t index = 0;
    while (index < handler_count && handlers[index].token != token)
        ++index;
    const bool found = token != 0 && index < handler_count;
    if (found) {
        memmove(handlers + index, handlers + index + 1,
                (handler_count - index - 1) * sizeof(*handlers));
        --handler_count;
        handlers[handler_count] = (struct vectored_slot){0};
    }
    pthread_mutex_unlock(&handler_lock);
    return found;
}

int32_t nt_runtime_dispatch_vectored_exception(struct nt_exception_pointers *pointers)
{
    nt_vectored_exception_handler snapshot[VECTORED_HANDLER_MAX];
    pthread_mutex_lock(&handler_lock);
    const size_t count = handler_count;
    for (size_t i = 0; i < count; ++i)
        snapshot[i] = handlers[i].callback;
    pthread_mutex_unlock(&handler_lock);
    for (size_t i = 0; i < count; ++i) {
        if (snapshot[i](pointers) == -1) /* EXCEPTION_CONTINUE_EXECUTION */
            return -1;
    }
    return 0; /* EXCEPTION_CONTINUE_SEARCH */
}
