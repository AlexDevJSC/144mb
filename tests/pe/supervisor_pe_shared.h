/* SPDX-License-Identifier: MIT */
#ifndef NT_SUPERVISOR_PE_SHARED_H
#define NT_SUPERVISOR_PE_SHARED_H
#include <stdatomic.h>
#include <stdint.h>

/* Test-only ABI: pointers in the shared MAP_SHARED mapping are passed as
 * a special Microsoft x64 entry argument. Normal Windows EXEs have no
 * such entry-point contract. Both sides run on AMD64 (64-bit atomics). */
struct nt_supervisor_pe_shared {
    _Atomic uint64_t guest_ticks;
    _Atomic uint64_t host_ticks;
    _Atomic uint32_t stop_requested;
    _Atomic uint32_t guest_finished;
    _Atomic uint32_t guest_exit_code;
    _Atomic uintptr_t guest_teb;
    _Atomic uintptr_t code_begin;
    _Atomic uintptr_t code_end;
};
#endif
