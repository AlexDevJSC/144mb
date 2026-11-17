/* SPDX-License-Identifier: MIT */
#ifndef NT_THREAD_SUPERVISOR_H
#define NT_THREAD_SUPERVISOR_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/user.h>
#include "ntnative/guest_boundary.h"
#include "ntnative/guest_boundary_runtime.h"

/* Experimental Linux AMD64 supervisor, not a Win32 API implementation.
 * Only a direct, live child of this caller can be supervised; `tid` must
 * belong to that child. All operations are synchronous and single-owner.
 * Suspend only guest-code windows: stopping a thread while it owns a host
 * mutex is NOT safe, regardless of ptrace capabilities. */
struct nt_thread_supervisor {
    pid_t child;
    pid_t tid;
    int pidfd;
    bool seized;
    bool stopped;
    /* Non-PTRACE_EVENT_STOP signal observed while tracing; deliver it
     * exactly once on CONT or DETACH, never silently suppress it. */
    int pending_signal;
    bool exited;
};

void nt_supervisor_init(struct nt_thread_supervisor *sup);
/* 0 success, -1 errno; ENOTSUP means host feature unavailable. */
int nt_supervisor_seize_child(struct nt_thread_supervisor *sup, pid_t child, pid_t tid);
int nt_supervisor_interrupt(struct nt_thread_supervisor *sup, unsigned timeout_ms);
int nt_supervisor_get_registers(struct nt_thread_supervisor *sup, struct user_regs_struct *out);
/* Limited proof of real SETREGSET. Does not change RIP/RSP or guest memory. */
int nt_supervisor_set_rax(struct nt_thread_supervisor *sup, uint64_t value);
int nt_supervisor_continue(struct nt_thread_supervisor *sup);
/* Detach while stopped; does not kill or wait for the child. */
int nt_supervisor_detach(struct nt_thread_supervisor *sup);
/* Read-only supervisor-side description: only call with a trustworthy
 * supervisor-owned mapping of the child's boundary state. Exact TID
 * ownership is independently checked by nt_supervisor_seize_child. */
struct nt_supervisor_guest_scope {
    const struct nt_guest_boundary *boundary;
    uintptr_t code_begin, code_end, teb;
    const struct nt_guest_code_range *additional_code;
    size_t additional_code_count;
};

/* Experimental fail-closed guest stop: the target MUST be in guest code
 * with the correct TEB, outside the host critical state. If ptrace stopped
 * it in the host, the function immediately resumes it and returns EBUSY.
 * No Win32 API is routed through this path until guest/host transitions
 * can be mediated for every relevant import and callback. */
int nt_supervisor_interrupt_guest(struct nt_thread_supervisor *sup,
                                  const struct nt_supervisor_guest_scope *scope,
                                  unsigned timeout_ms, struct user_regs_struct *out);
int nt_supervisor_get_guest_context(struct nt_thread_supervisor *sup,
                                    const struct nt_supervisor_guest_scope *scope,
                                    struct user_regs_struct *out);
int nt_supervisor_set_guest_rax(struct nt_thread_supervisor *sup,
                                const struct nt_supervisor_guest_scope *scope,
                                uint64_t value);
/* Trusted PE-only APC proof: inject a synthetic ms_abi callback on a
 * dedicated, already-mapped guest stack, intercept its deliberate INT3,
 * restore kernel-supplied general registers AND full NT_X86_XSTATE, then
 * resume at the original interrupted PE instruction. Not a general
 * NtQueueApcThreadEx implementation. No host locks/callback nesting. */
int nt_supervisor_invoke_trusted_guest_apc(
    struct nt_thread_supervisor *sup,
    const struct nt_supervisor_guest_scope *scope,
    uintptr_t guest_entry, uintptr_t guest_stack_top, uintptr_t guest_argument,
    const _Atomic uint32_t *guest_completion, unsigned timeout_ms);
/* Cleanup used on error, EOF, stale capability or process teardown. If a
 * tracee cannot safely be detached, SIGKILL is sent ONLY through the
 * pidfd pinned to this exact direct child. Returns 0 on clean detach,
 * -1 if emergency termination was required or could not be confirmed. */
/* A failed CONTEXT rollback cannot be safely detached. Send SIGKILL to
 * the exact process already pinned by this tracer's pidfd. Does not
 * discard ptrace ownership; caller must reap/detach after termination. */
int nt_supervisor_terminate_compromised_child(struct nt_thread_supervisor *sup);
int nt_supervisor_abort_child(struct nt_thread_supervisor *sup);
void nt_supervisor_release(struct nt_thread_supervisor *sup);
#endif
