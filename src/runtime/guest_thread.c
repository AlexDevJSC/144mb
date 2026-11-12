/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/guest_thread.h"

#include <asm/prctl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static _Thread_local unsigned char *current_teb;
static _Thread_local struct nt_guest_thread *current_guest_thread;

struct nt_guest_thread *nt_guest_thread_current(void)
{
    return current_guest_thread;
}

unsigned char *nt_guest_thread_teb(void)
{
    return current_teb;
}

static void put_pointer(unsigned char *base, size_t offset, uintptr_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

_Static_assert(offsetof(struct nt_guest_process_parameters, flags) == 0x08, "ProcessParameters Flags");
_Static_assert(offsetof(struct nt_guest_process_parameters, image_path) == 0x60, "ProcessParameters ImagePathName");
_Static_assert(offsetof(struct nt_guest_process_parameters, command_line) == 0x70, "ProcessParameters CommandLine");
_Static_assert(offsetof(struct nt_guest_process_parameters, environment) == 0x80, "ProcessParameters Environment");
_Static_assert(offsetof(struct nt_guest_ldr_data, load_order) == 0x10,
               "PEB_LDR_DATA InLoadOrderModuleList offset");
_Static_assert(offsetof(struct nt_guest_ldr_data, memory_order) == 0x20,
               "PEB_LDR_DATA InMemoryOrderModuleList offset");
_Static_assert(offsetof(struct nt_guest_ldr_data, initialization_order) == 0x30,
               "PEB_LDR_DATA InInitializationOrderModuleList offset");
_Static_assert(offsetof(struct nt_guest_ldr_entry, dll_base) == 0x30,
               "LDR_DATA_TABLE_ENTRY DllBase offset");
_Static_assert(offsetof(struct nt_guest_ldr_entry, full_name) == 0x48,
               "LDR_DATA_TABLE_ENTRY FullDllName offset");
_Static_assert(offsetof(struct nt_guest_ldr_entry, base_name) == 0x58,
               "LDR_DATA_TABLE_ENTRY BaseDllName offset");

static void link_primary(struct nt_guest_list_entry *head, struct nt_guest_list_entry *entry)
{
    head->flink = head->blink = (uintptr_t)entry;
    entry->flink = entry->blink = (uintptr_t)head;
}

void nt_guest_process_bind_image(struct nt_guest_process *process,
                                 const struct nt_guest_unicode_string *path,
                                 size_t image_bytes)
{
    if (process == NULL || path == NULL || path->buffer == NULL ||
        path->length % sizeof(uint16_t) != 0 || path->length > path->maximum_length)
        return;
    process->image_entry.full_name = *path;
    const size_t count = path->length / sizeof(uint16_t);
    size_t leaf = 0;
    for (size_t i = 0; i < count; ++i)
        if (path->buffer[i] == '\\' || path->buffer[i] == '/')
            leaf = i + 1;
    process->image_entry.base_name = (struct nt_guest_unicode_string){
        (uint16_t)((count - leaf) * sizeof(uint16_t)),
        (uint16_t)(path->maximum_length - leaf * sizeof(uint16_t)),
        path->buffer + leaf
    };
    process->image_entry.image_size = image_bytes <= UINT32_MAX ?
                                     (uint32_t)image_bytes : 0;
}


void nt_guest_process_init(struct nt_guest_process *process, uintptr_t image, uintptr_t heap)
{
    memset(process, 0, sizeof(*process));
    process->parameters.maximum_length = process->parameters.length = sizeof(process->parameters);
    process->parameters.flags = 1; /* PROCESS_PARAMS_FLAG_NORMALIZED */
    put_pointer(process->peb, 0x10, image);
    put_pointer(process->peb, 0x18, (uintptr_t)&process->loader);
    put_pointer(process->peb, 0x20, (uintptr_t)&process->parameters);
    put_pointer(process->peb, 0x30, heap);
    process->loader.length = sizeof(process->loader);
    process->loader.initialized = 1;
    process->image_entry.dll_base = image;
    link_primary(&process->loader.load_order, &process->image_entry.load_links);
    link_primary(&process->loader.memory_order, &process->image_entry.memory_links);
    link_primary(&process->loader.initialization_order, &process->image_entry.initialization_links);
}

bool nt_guest_thread_enter_in_process(struct nt_guest_thread *state, uintptr_t image_base,
        const struct nt_guest_tls *initial, const struct nt_guest_process *process)
{
    if (state == NULL || state->entered || image_base == 0 ||
        (initial != NULL && (initial->bytes > NT_GUEST_TLS_MAX ||
          initial->zero_fill > NT_GUEST_TLS_MAX - initial->bytes ||
          (initial->bytes != 0 && initial->data == NULL))))
        return false;
    memset(state, 0, sizeof(*state));
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0)
        return false;
    void *stack_low = NULL;
    size_t stack_size = 0;
    const int stack_result = pthread_attr_getstack(&attr, &stack_low, &stack_size);
    pthread_attr_destroy(&attr);
    if (stack_result != 0 || stack_low == NULL || stack_size == 0 ||
        (uintptr_t)stack_low > UINTPTR_MAX - stack_size)
        return false;
    if (initial != NULL && initial->bytes != 0)
        memcpy(state->tls_block, initial->data, initial->bytes);
    state->tls_vector[0] = (uintptr_t)state->tls_block;
    put_pointer(state->teb, 0x08, (uintptr_t)stack_low + stack_size); /* StackBase */
    put_pointer(state->teb, 0x10, (uintptr_t)stack_low); /* StackLimit */
    put_pointer(state->teb, 0x30, (uintptr_t)state->teb); /* NT_TIB.Self */
    put_pointer(state->teb, 0x40, (uintptr_t)getpid()); /* ClientId.UniqueProcess */
    put_pointer(state->teb, 0x48, (uintptr_t)syscall(SYS_gettid)); /* UniqueThread */
    put_pointer(state->teb, 0x58, (uintptr_t)state->tls_vector);
    if (!process) {
        nt_guest_process_init(&state->standalone, image_base, 0);
        process = &state->standalone;
    }
    put_pointer(state->teb, 0x60, (uintptr_t)process->peb);
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &state->saved_gs) != 0)
        return false;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, state->teb) != 0)
        return false;
    state->saved_teb = current_teb;
    state->saved_thread = current_guest_thread;
    current_teb = state->teb;
    current_guest_thread = state;
    state->entered = true;
    return true;
}

bool nt_guest_thread_enter_with_tls(struct nt_guest_thread *state, uintptr_t image_base,
                                    const struct nt_guest_tls *initial)
{
    return nt_guest_thread_enter_in_process(state, image_base, initial, NULL);
}

bool nt_guest_thread_enter(struct nt_guest_thread *state, uintptr_t image_base)
{
    return nt_guest_thread_enter_with_tls(state, image_base, NULL);
}

bool nt_guest_thread_set_static_tls(struct nt_guest_thread *state, uint32_t index,
                                     const unsigned char *data, size_t bytes, size_t zero_fill)
{
    if (state == NULL || !state->entered || current_guest_thread != state ||
        index == 0 || index >= 64 || state->tls_vector[index] != 0 ||
        bytes > NT_GUEST_TLS_MAX || zero_fill > NT_GUEST_TLS_MAX - bytes ||
        (bytes != 0 && data == NULL))
        return false;
    const size_t size = bytes + zero_fill;
    unsigned char *block = calloc(1, size ? size : 1);
    if (block == NULL)
        return false;
    if (bytes != 0)
        memcpy(block, data, bytes);
    state->static_tls_blocks[index] = block;
    state->tls_vector[index] = (uintptr_t)block;
    return true;
}

bool nt_guest_thread_leave(struct nt_guest_thread *state)
{
    if (state == NULL || !state->entered || current_teb != state->teb)
        return false;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, state->saved_gs) != 0)
        return false;
    current_teb = state->saved_teb;
    current_guest_thread = state->saved_thread;
    free(state->tls_expansion);
    state->tls_expansion = NULL;
    for (size_t i = 1; i < 64; ++i) {
        free(state->static_tls_blocks[i]);
        state->static_tls_blocks[i] = NULL;
        state->tls_vector[i] = 0;
    }
    state->entered = false;
    return true;
}
