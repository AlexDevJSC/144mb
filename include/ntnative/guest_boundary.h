/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GUEST_BOUNDARY_H
#define NTNATIVE_GUEST_BOUNDARY_H

#include <stdatomic.h>
#include <stdint.h>

/* Experimental *trusted* host/guest gate, shared with a supervising parent.
 * This is NOT a security boundary against a malicious PE: arbitrary native
 * guest instructions can write its own address space. The image-range check
 * in the external supervisor is independently mandatory.
 *
 * Unregistered/host states are NOT suspension-safe. A trusted runtime
 * must publish HOST before taking locks or calling host services, and
 * publish GUEST only after the last host lock is released and control is
 * returning to guest code. Guest callbacks under host locks are forbidden.
 */
enum nt_guest_boundary_phase {
    NT_GUEST_BOUNDARY_UNREGISTERED = 0,
    NT_GUEST_BOUNDARY_GUEST = 1,
    NT_GUEST_BOUNDARY_HOST = 2,
    NT_GUEST_BOUNDARY_FINISHED = 3
};

struct nt_guest_boundary {
    _Atomic uint32_t phase;
    _Atomic uint32_t transition; /* monotone, useful for test diagnostics */
};

static inline void nt_guest_boundary_publish(struct nt_guest_boundary *boundary,
                                               enum nt_guest_boundary_phase phase)
{
    atomic_store_explicit(&boundary->phase, (uint32_t)phase, memory_order_release);
    atomic_fetch_add_explicit(&boundary->transition, 1u, memory_order_release);
}

#endif
