/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/guest_thread.h"

#include <asm/prctl.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

static uintptr_t from_gs_30(void)
{
    uintptr_t pointer;
    __asm__ volatile("movq %%gs:0x30, %0" : "=r"(pointer));
    return pointer;
}
static uintptr_t from_gs_58(void)
{
    uintptr_t pointer;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(pointer));
    return pointer;
}
static uintptr_t from_gs_60(void)
{
    uintptr_t pointer;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(pointer));
    return pointer;
}

int main(void)
{
    uintptr_t previous = 0;
    assert(syscall(SYS_arch_prctl, ARCH_GET_GS, &previous) == 0);
    struct nt_guest_thread thread = {0};
    const uintptr_t image = UINT64_C(0x140000000);
    assert(nt_guest_thread_enter(&thread, image));
    assert(from_gs_30() == (uintptr_t)thread.teb);
    assert(nt_guest_thread_teb() == thread.teb);
    assert(nt_guest_thread_current() == &thread);
    assert(from_gs_58() == (uintptr_t)thread.tls_vector);
    assert(from_gs_60() == (uintptr_t)thread.standalone.peb);
    assert(thread.tls_vector[0] == (uintptr_t)thread.tls_block);
    uintptr_t base = 0;
    __builtin_memcpy(&base, thread.standalone.peb + 0x10, sizeof(base));
    assert(base == image);
    uintptr_t parameters = 0;
    __builtin_memcpy(&parameters, thread.standalone.peb + 0x20, sizeof(parameters));
    assert(parameters == (uintptr_t)&thread.standalone.parameters && thread.standalone.parameters.flags == 1);
    assert(!nt_guest_thread_enter(&thread, image));
    struct nt_guest_thread nested = {0};
    assert(nt_guest_thread_enter(&nested, image));
    assert(nt_guest_thread_teb() == nested.teb && nt_guest_thread_current() == &nested &&
           !nt_guest_thread_leave(&thread));
    assert(nt_guest_thread_leave(&nested));
    assert(nt_guest_thread_teb() == thread.teb && nt_guest_thread_current() == &thread &&
           from_gs_30() == (uintptr_t)thread.teb);
    assert(nt_guest_thread_leave(&thread));
    assert(nt_guest_thread_teb() == NULL && nt_guest_thread_current() == NULL);
    uintptr_t current = UINTPTR_MAX;
    assert(syscall(SYS_arch_prctl, ARCH_GET_GS, &current) == 0);
    assert(current == previous);
    assert(!nt_guest_thread_leave(&thread));
    const unsigned char seed[] = {0xc2, 0x39, 0x42};
    const struct nt_guest_tls template = {seed, sizeof(seed), 6};
    assert(nt_guest_thread_enter_with_tls(&thread, image, &template));
    assert(__builtin_memcmp((const void *)thread.tls_vector[0], seed, sizeof(seed)) == 0);
    assert(thread.tls_block[3] == 0 && thread.tls_block[8] == 0);
    thread.tls_block[0] = 0;
    assert(nt_guest_thread_leave(&thread));
    assert(nt_guest_thread_enter_with_tls(&thread, image, &template));
    assert(thread.tls_block[0] == seed[0]);
    assert(nt_guest_thread_leave(&thread));
    puts("PASS Linux GS/TEB/PEB mapping, nested thread state and static TLS copy");
    return 0;
}
