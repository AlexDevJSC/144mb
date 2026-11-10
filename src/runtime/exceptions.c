/* SPDX-License-Identifier: MIT */
#include "compat.h"

#include <stdatomic.h>

_Static_assert(sizeof(struct nt_context64) == 1232, "AMD64 CONTEXT size");
_Static_assert(_Alignof(struct nt_context64) == 16, "AMD64 CONTEXT alignment");
_Static_assert(offsetof(struct nt_context64, flags) == 0x30, "AMD64 CONTEXT flags");
_Static_assert(offsetof(struct nt_context64, rax) == 0x78, "AMD64 CONTEXT Rax");
_Static_assert(offsetof(struct nt_context64, rsp) == 0x98, "AMD64 CONTEXT Rsp");
_Static_assert(offsetof(struct nt_context64, rip) == 0xf8, "AMD64 CONTEXT Rip");
_Static_assert(offsetof(struct nt_context64, floating) == 0x100, "AMD64 CONTEXT FltSave");
_Static_assert(sizeof(struct nt_runtime_function) == 12, "RUNTIME_FUNCTION size");

/* The launcher sets the immutable image view before running the guest. */
static struct {
    uintptr_t base;
    size_t image_size;
    const struct nt_runtime_function *table;
    size_t count;
} image;

bool nt_runtime_set_runtime_functions(uintptr_t base, size_t image_size,
                                      const struct nt_runtime_function *table, size_t count)
{
    if (base == 0) {
        if (count != 0)
            return false;
        image.base = 0;
        image.image_size = 0;
        image.table = NULL;
        image.count = 0;
        return true;
    }
    if (image_size < 4 || image_size > UINT32_MAX || image_size > UINTPTR_MAX - base ||
        table == NULL || count == 0 || count > 65536)
        return false;
    uint32_t previous_end = 0;
    for (size_t i = 0; i < count; ++i) {
        const struct nt_runtime_function *entry = &table[i];
        if (entry->begin < previous_end || entry->begin >= entry->end || entry->end > image_size ||
            entry->unwind == 0 || (entry->unwind & 3) != 0 || entry->unwind > image_size - 4)
            return false;
        previous_end = entry->end;
    }
    image.base = base;
    image.image_size = image_size;
    image.table = table;
    image.count = count;
    return true;
}

const struct nt_runtime_function *NT_MS_ABI nt_RtlLookupFunctionEntry(
    uint64_t pc, uint64_t *base, void *history)
{
    (void)history;
    if (base == NULL)
        return NULL;
    *base = 0;
    if (pc < image.base || pc - image.base >= image.image_size)
        return NULL;
    const uint64_t rva = pc - image.base;
    size_t left = 0, right = image.count;
    while (left < right) {
        const size_t middle = left + (right - left) / 2;
        const struct nt_runtime_function *entry = &image.table[middle];
        if (rva < entry->begin) {
            right = middle;
        } else if (rva >= entry->end) {
            left = middle + 1;
        } else {
            *base = image.base;
            return entry;
        }
    }
    return NULL;
}

static _Atomic(nt_exception_filter) exception_filter;
static _Thread_local void *current_record, *current_context;

void **NT_MS_ABI nt_current_exception(void)
{
    return &current_record;
}

void **NT_MS_ABI nt_current_exception_context(void)
{
    return &current_context;
}

nt_exception_filter NT_MS_ABI nt_SetUnhandledExceptionFilter(nt_exception_filter filter)
{
    return atomic_exchange(&exception_filter, filter);
}

int32_t NT_MS_ABI nt_UnhandledExceptionFilter(struct nt_exception_pointers *pointers)
{
    if (pointers == NULL || pointers->record == NULL || pointers->context == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (nt_IsDebuggerPresent())
        return 0;
    const nt_exception_filter filter = atomic_load(&exception_filter);
    if (filter != NULL) {
        void *saved_record = current_record, *saved_context = current_context;
        current_record = pointers->record;
        current_context = pointers->context;
        const int32_t result = filter(pointers);
        current_record = saved_record;
        current_context = saved_context;
        if (result == -1 || result == 1)
            return result;
    }
    /* The worker profile has no interactive crash reporter. */
    return 1;
}
