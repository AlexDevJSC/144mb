/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/guest_thread.h"

#include <asm/prctl.h>
#include <pthread.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void put_pointer(unsigned char *base, size_t offset, uintptr_t value)
{
    memcpy(base + offset, &value, sizeof(value));
}

_Static_assert(offsetof(struct nt_guest_process_parameters, flags) == 0x08, "ProcessParameters Flags");
_Static_assert(offsetof(struct nt_guest_process_parameters, image_path) == 0x60, "ProcessParameters ImagePathName");
_Static_assert(offsetof(struct nt_guest_process_parameters, command_line) == 0x70, "ProcessParameters CommandLine");
_Static_assert(offsetof(struct nt_guest_process_parameters, environment) == 0x80, "ProcessParameters Environment");

void nt_guest_process_init(struct nt_guest_process *process, uintptr_t image, uintptr_t heap)
{
    memset(process, 0, sizeof(*process));
    process->parameters.maximum_length = process->parameters.length = sizeof(process->parameters);
    process->parameters.flags = 1; /* PROCESS_PARAMS_FLAG_NORMALIZED */
    put_pointer(process->peb, 0x10, image);
    put_pointer(process->peb, 0x20, (uintptr_t)&process->parameters);
    put_pointer(process->peb, 0x30, heap);
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

bool nt_guest_thread_leave(struct nt_guest_thread *state)
{
    if (state == NULL || !state->entered)
        return false;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, state->saved_gs) != 0)
        return false;
    state->entered = false;
    return true;
}
