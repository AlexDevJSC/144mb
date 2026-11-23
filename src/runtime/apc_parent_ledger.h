/* SPDX-License-Identifier: MIT */
#ifndef NT_APC_PARENT_LEDGER_H
#define NT_APC_PARENT_LEDGER_H
#include <stdbool.h>
#include <stdint.h>

/* Parent-only, bounded, per-authenticated-child-session replay guard.
 * A receipt proves an authenticated runtime transition, NOT that ptrace
 * independently watched the guest execute the callback instructions.
 * At most one non-nested special APC can run on each guest thread. */
enum { NT_APC_PARENT_THREADS = 512 };
enum {
    NT_APC_PHASE_AUTHORIZE = 0,
    NT_APC_PHASE_START = 1,
    NT_APC_PHASE_DONE = 2,
    NT_APC_PHASE_ABORT = 3
};
struct nt_apc_parent_thread {
    uint32_t thread_id;
    uint64_t generation;
    uint64_t highest_start;
    uint64_t active_ticket;
    uint64_t active_callback;
};
struct nt_apc_parent_ledger {
    struct nt_apc_parent_thread threads[NT_APC_PARENT_THREADS];
};

/* Authenticated UNREGISTER/child session revocation invalidates even a
 * callback that vanished mid-flight. Never reuse its generation as proof
 * of completion; further messages require a fresh broker lease. */
static inline void nt_apc_parent_revoke_thread(
    struct nt_apc_parent_ledger *ledger, uint32_t thread_id)
{
    if (!ledger || !thread_id) return;
    for (unsigned i = 0; i < NT_APC_PARENT_THREADS; ++i) {
        struct nt_apc_parent_thread *slot = &ledger->threads[i];
        if (slot->thread_id == thread_id)
            *slot = (struct nt_apc_parent_thread){0};
    }
}

static inline bool nt_apc_parent_transition(
    struct nt_apc_parent_ledger *ledger,
    uint32_t thread_id, uint64_t generation,
    uint64_t callback, uint64_t ticket, uint32_t phase)
{
    if (!ledger || !thread_id || !generation || !callback ||
        !ticket || (phase != NT_APC_PHASE_START &&
                    phase != NT_APC_PHASE_DONE &&
                    phase != NT_APC_PHASE_ABORT))
        return false;
    struct nt_apc_parent_thread *slot = NULL, *spare = NULL;
    for (unsigned i = 0; i < NT_APC_PARENT_THREADS; ++i) {
        struct nt_apc_parent_thread *candidate = &ledger->threads[i];
        if (candidate->thread_id == thread_id &&
            candidate->generation == generation) {
            slot = candidate;
            break;
        }
        /* Only reclaim a retired generation; an active generation
         * must keep its high-water mark to reject replay. */
        if (!spare && (candidate->thread_id == 0 ||
                       (candidate->thread_id == thread_id &&
                        candidate->generation != generation &&
                        candidate->active_ticket == 0)))
            spare = candidate;
    }
    if (!slot && phase == NT_APC_PHASE_START) {
        if (!spare) return false;
        slot = spare;
        slot->thread_id = thread_id;
        slot->generation = generation;
        slot->highest_start = 0;
        slot->active_ticket = 0;
        slot->active_callback = 0;
    }
    if (!slot) return false;
    if (phase == NT_APC_PHASE_START) {
        if (slot->active_ticket || ticket <= slot->highest_start)
            return false;
        slot->highest_start = ticket;
        slot->active_ticket = ticket;
        slot->active_callback = callback;
        return true;
    }
    if (slot->active_ticket != ticket || slot->active_callback != callback)
        return false;
    slot->active_ticket = 0;
    slot->active_callback = 0;
    return true;
}
#endif
