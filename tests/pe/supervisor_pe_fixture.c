/* SPDX-License-Identifier: MIT */
/* Real freestanding PE32+ AMD64; no CRT or Windows imports.
 * A test-specific Microsoft x64 entry argument carries shared counters. */
#include "supervisor_pe_shared.h"
#if !defined(__x86_64__)
#error "PE32+ AMD64 required"
#endif

/* Experimental APC target: NO imports or calls into the host. It
 * signals completion and raises INT3 so the supervisor can restore the
 * original interrupted PC/stack/XSTATE before returning to guest code.
 * Executing it as an ordinary callback would not be safe. */
__attribute__((ms_abi, noinline, noreturn))
void supervisor_pe_apc_callback(struct nt_supervisor_pe_shared *shared)
{
    atomic_fetch_add_explicit(&shared->apc_delivered, 1u, memory_order_release);
    __asm__ volatile ("int3");
    __builtin_unreachable();
}

int __attribute__((ms_abi)) supervisor_pe_entry(struct nt_supervisor_pe_shared *shared)
{
    if (shared == 0)
        return 17;
    /* RIP-relative LEA survives converting an ELF .text fixture to PE
     * COFF without requiring an absolute relocation in the PE image. */
    uintptr_t callback_entry = 0;
    __asm__ volatile ("leaq supervisor_pe_apc_callback(%%rip), %0"
                      : "=r"(callback_entry));
    atomic_store_explicit(&shared->apc_entry, callback_entry, memory_order_release);
    while (!atomic_load_explicit(&shared->stop_requested, memory_order_acquire)) {
        if (atomic_exchange_explicit(&shared->request_host_call, 0u, memory_order_acq_rel)) {
            const uintptr_t address =
                atomic_load_explicit(&shared->host_callback, memory_order_acquire);
            /* This is a deliberately controlled test import, not a general
             * mechanism for entering Linux from an arbitrary PE image. */
            void (__attribute__((ms_abi)) *callback)(struct nt_supervisor_pe_shared *);
            if (!address)
                return 18;
            __builtin_memcpy(&callback, &address, sizeof(callback));
            callback(shared);
        }
        atomic_fetch_add_explicit(&shared->guest_ticks, UINT64_C(1), memory_order_relaxed);
    }
    atomic_store_explicit(&shared->guest_finished, 1u, memory_order_release);
    return 0;
}
