/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_supervisor_control_server.h"
#include "ntnative/guest_boundary_runtime.h"
#include "../supervisor/nt_supervisor_nt_bridge.h"
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <stdatomic.h>

/* Bounded by per-process guest thread slots, not a global host PID table. */
enum { MAX_SUPERVISED_THREADS = 128 };
struct supervisor_state {
    bool active;
    pid_t tid;
    uint64_t generation;
    struct nt_thread_supervisor tracer;
    struct nt_supervisor_nt_thread nt;
};
struct nt_supervisor_control_server {
    struct supervisor_state threads[MAX_SUPERVISED_THREADS];
};

struct nt_supervisor_control_server *nt_supervisor_control_server_create(void)
{
    return calloc(1, sizeof(struct nt_supervisor_control_server));
}

static void release_state(struct supervisor_state *state)
{
    if (!state || !state->active) return;
    /* Requires the same broker pthread that originally called SEIZE;
     * ptrace requests are NOT transferable to another Linux thread. */
    if (state->nt.unsafe_register_state ||
        state->tracer.compromised) {
        /* A successful SIGKILL send is NOT proof that this traced
         * thread has exited: do not PTRACE_DETACH it in that window.
         * Keep EXITKILL and the pidfd pinned until death is visible. */
        (void)nt_supervisor_terminate_compromised_child(&state->tracer);
        if (nt_supervisor_abort_child(&state->tracer) != 0)
            return;
    } else {
        (void)nt_supervisor_abort_child(&state->tracer);
    }
    *state = (struct supervisor_state){0};
}

void nt_supervisor_control_server_revoke_tid(
    struct nt_supervisor_control_server *owner, uint32_t tid)
{
    if (!owner || !tid) return;
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i)
        if (owner->threads[i].active && owner->threads[i].tid == (pid_t)tid)
            release_state(&owner->threads[i]);
}

void nt_supervisor_control_server_destroy(
    struct nt_supervisor_control_server *owner)
{
    if (!owner) return;
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i) {
        struct supervisor_state *state = &owner->threads[i];
        release_state(state);
        if (state->active && state->tracer.compromised) {
            /* Last resort on broker EOF: do not detach. This broker
             * pthread exits immediately after destroy(), so the kernel
             * enforces PTRACE_O_EXITKILL if SIGKILL was delayed. Close
             * our owned fd without releasing ptrace ownership. */
            (void)nt_supervisor_terminate_compromised_child(&state->tracer);
            if (state->tracer.pidfd >= 0) {
                (void)close(state->tracer.pidfd);
                state->tracer.pidfd = -1;
            }
        }
    }
    free(owner);
}

static struct supervisor_state *find_state(
    struct nt_supervisor_control_server *owner,
    const struct nt_broker_thread_info *identity)
{
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i) {
        struct supervisor_state *state = &owner->threads[i];
        if (state->active &&
            state->tid == (pid_t)identity->guest_thread_id &&
            state->generation == identity->generation)
            return state;
    }
    return NULL;
}

void nt_supervisor_control_server_abort_unacked_context(
    struct nt_supervisor_control_server *owner,
    const struct nt_broker_thread_info *identity)
{
    if (!owner || !identity)
        return;
    struct supervisor_state *state = find_state(owner, identity);
    if (!state || !state->tracer.seized || state->tracer.exited)
        return;
    /* The kernel had accepted and verified a changed NT CONTEXT, but
     * the requester disconnected before acknowledging it. Detaching
     * would resume user code with a mutation the caller cannot confirm.
     * Bind SIGKILL to the original direct child pidfd, never a raw TID. */
    state->nt.unsafe_register_state = true;
    (void)nt_supervisor_terminate_compromised_child(&state->tracer);
}

static struct supervisor_state *allocate_state(
    struct nt_supervisor_control_server *owner)
{
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i)
        if (!owner->threads[i].active)
            return &owner->threads[i];
    return NULL;
}

static bool owned_guest_scope(
    const struct nt_thread_broker_registered_peer *peer,
    const struct nt_broker_thread_info *identity,
    struct nt_supervisor_guest_scope *scope)
{
    if (!peer || !peer->guest_session || !scope || !identity ||
        identity->guest_process_id != (uint32_t)peer->child_pid ||
        peer->trusted_code_begin == 0 ||
        peer->trusted_code_end <= peer->trusted_code_begin)
        return false;
    const struct nt_guest_boundary_slot *slot =
        nt_guest_boundary_find_thread(peer->guest_session,
                                      (pid_t)identity->guest_thread_id);
    if (!slot) return false;
    const uintptr_t teb = atomic_load_explicit(&slot->teb, memory_order_acquire);
    if (!teb) return false;
    *scope = (struct nt_supervisor_guest_scope){
        .boundary = &slot->boundary,
        .teb = teb,
        .code_begin = peer->trusted_code_begin,
        .code_end = peer->trusted_code_end,
        .additional_code = peer->trusted_startup_dlls,
        .additional_code_count = peer->trusted_startup_dll_count
    };
    return true;
}

static bool supported_flags(uint32_t flags, bool writing)
{
    const uint32_t base = NT_SUP_CONTEXT_AMD64;
    const uint32_t readers = NT_SUP_CONTEXT_INTEGER |
        NT_SUP_CONTEXT_CONTROL | NT_SUP_CONTEXT_SEGMENTS |
        NT_SUP_CONTEXT_FLOATING_POINT;
    /* Writing a legacy FXSAVE image is a separate, verified kernel
     * transaction. Never permit mixed INTEGER|FP writes or control,
     * segment and extended XSTATE groups via this RPC gate. */
    if (writing)
        return flags == (base | NT_SUP_CONTEXT_INTEGER) ||
               flags == (base | NT_SUP_CONTEXT_FLOATING_POINT);
    return (flags & base) == base && (flags & readers) != 0 &&
           (flags & ~(base | readers)) == 0;
}

nt_status nt_supervisor_control_server_call(
    struct nt_supervisor_control_server *owner,
    const struct nt_thread_broker_registered_peer *peer,
    const struct nt_broker_thread_info *identity,
    uint32_t operation, uint32_t context_flags,
    const struct nt_context64 *input,
    struct nt_context64 *output, uint32_t *previous)
{
    if (!owner || !peer || !identity ||
        identity->guest_process_id != (uint32_t)peer->child_pid)
        return NT_STATUS_INVALID_PARAMETER;
    if ((peer->allowed_access & NT_BROKER_SERVER_ENABLE_THREAD_CONTROL) == 0)
        return NT_STATUS_NOT_SUPPORTED;
    if (operation == NT_BROKER_RPC_SUP_GET_CONTEXT &&
        (!output || !supported_flags(context_flags, false)))
        return NT_STATUS_INVALID_PARAMETER;
    if (operation == NT_BROKER_RPC_SUP_SET_CONTEXT &&
        (!input || !supported_flags(input->flags, true)))
        return NT_STATUS_NOT_SUPPORTED;
    if (operation != NT_BROKER_RPC_SUP_GET_CONTEXT &&
        operation != NT_BROKER_RPC_SUP_SET_CONTEXT &&
        operation != NT_BROKER_RPC_SUP_SUSPEND &&
        operation != NT_BROKER_RPC_SUP_RESUME)
        return NT_STATUS_INVALID_PARAMETER;

    struct supervisor_state *state = find_state(owner, identity);
    if (state && state->nt.unsafe_register_state) {
        release_state(state);
        return NT_STATUS_THREAD_IS_TERMINATING;
    }
    if (!state && operation != NT_BROKER_RPC_SUP_SUSPEND)
        return NT_STATUS_NOT_SUPPORTED;
    if (!state) {
        struct nt_supervisor_guest_scope scope;
        if (!owned_guest_scope(peer, identity, &scope))
            return NT_STATUS_NOT_SUPPORTED;
        state = allocate_state(owner);
        if (!state)
            return NT_STATUS_NO_MEMORY;
        memset(state, 0, sizeof(*state));
        nt_supervisor_init(&state->tracer);
        if (nt_supervisor_seize_child(&state->tracer, peer->child_pid,
                                      (pid_t)identity->guest_thread_id) != 0)
            return NT_STATUS_NOT_SUPPORTED;
        state->active = true;
        state->tid = (pid_t)identity->guest_thread_id;
        state->generation = identity->generation;
        state->nt = (struct nt_supervisor_nt_thread){
            .tracer = &state->tracer,
            .scope = scope,
            .granted_access = NT_SUP_THREAD_SUSPEND_RESUME |
                              NT_SUP_THREAD_GET_CONTEXT |
                              NT_SUP_THREAD_SET_CONTEXT
        };
    }
    /* Revoked/reused thread slots or a moved TEB invalidate an already
     * suspended tracee, even when the client retains an old capability. */
    struct nt_supervisor_guest_scope verified;
    if (!owned_guest_scope(peer, identity, &verified) ||
        verified.boundary != state->nt.scope.boundary ||
        verified.teb != state->nt.scope.teb ||
        verified.additional_code != state->nt.scope.additional_code ||
        verified.additional_code_count != state->nt.scope.additional_code_count) {
        release_state(state);
        return NT_STATUS_THREAD_IS_TERMINATING;
    }

    nt_status result = NT_STATUS_NOT_SUPPORTED;
    switch (operation) {
    case NT_BROKER_RPC_SUP_SUSPEND:
        result = nt_supervisor_nt_suspend_thread(&state->nt, previous);
        if (result != NT_STATUS_SUCCESS && state->nt.suspend_count == 0)
            release_state(state);
        break;
    case NT_BROKER_RPC_SUP_RESUME:
        if (!state->nt.suspend_count) return NT_STATUS_NOT_SUPPORTED;
        if (state->nt.suspend_count == 1) {
            /* Final resume MUST detach while the target is stopped.
             * A PTRACE_CONT followed by a fresh INTERRUPT would race
             * host-lock acquisition and prevent clean ownership release. */
            if (nt_supervisor_detach(&state->tracer) == 0) {
                if (previous) *previous = 1;
                state->nt.suspend_count = 0;
                release_state(state);
                result = NT_STATUS_SUCCESS;
            } else {
                release_state(state);
            }
        } else {
            result = nt_supervisor_nt_resume_thread(&state->nt, previous);
        }
        break;
    case NT_BROKER_RPC_SUP_GET_CONTEXT: {
        _Alignas(16) struct nt_context64 local = {0};
        local.flags = context_flags;
        result = nt_supervisor_nt_get_context(&state->nt, &local);
        if (result == NT_STATUS_SUCCESS)
            *output = local;
        break;
    }
    case NT_BROKER_RPC_SUP_SET_CONTEXT:
        result = nt_supervisor_nt_set_context(&state->nt, input);
        if (state->nt.unsafe_register_state) {
            release_state(state); /* PIDFD kill; never unsafe DETACH. */
            result = NT_STATUS_THREAD_IS_TERMINATING;
        }
        break;
    default: break;
    }
    return result;
}
