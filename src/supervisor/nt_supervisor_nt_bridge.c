/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nt_supervisor_nt_bridge.h"
#include <errno.h>
#include <linux/elf.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>

enum { NT_SUP_MAX_SUSPEND = 127 };

static nt_status translate_ptrace_error(int error)
{
    switch (error) {
    case EINVAL: return NT_STATUS_INVALID_PARAMETER;
    case EACCES: return NT_STATUS_ACCESS_DENIED;
    case ESRCH: return NT_STATUS_THREAD_IS_TERMINATING;
    default: return NT_STATUS_NOT_SUPPORTED;
    }
}

static bool has_right(const struct nt_supervisor_nt_thread *state, uint32_t right)
{
    return state != NULL && state->tracer != NULL &&
           (state->granted_access & right) == right;
}

nt_status nt_supervisor_nt_suspend_thread(struct nt_supervisor_nt_thread *state,
                                          uint32_t *previous)
{
    if (!state || !state->tracer) return NT_STATUS_INVALID_PARAMETER;
    if (!has_right(state, NT_SUP_THREAD_SUSPEND_RESUME))
        return NT_STATUS_ACCESS_DENIED;
    if (state->suspend_count == NT_SUP_MAX_SUSPEND)
        return NT_STATUS_SUSPEND_COUNT_EXCEEDED;
    if (state->suspend_count == 0) {
        struct user_regs_struct captured;
        if (nt_supervisor_interrupt_guest(state->tracer, &state->scope, 1500, &captured))
            return translate_ptrace_error(errno);
    } else if (!state->tracer->stopped) {
        return NT_STATUS_NOT_SUPPORTED;
    }
    if (previous) *previous = state->suspend_count;
    ++state->suspend_count;
    return NT_STATUS_SUCCESS;
}

nt_status nt_supervisor_nt_resume_thread(struct nt_supervisor_nt_thread *state,
                                         uint32_t *previous)
{
    if (!state || !state->tracer) return NT_STATUS_INVALID_PARAMETER;
    if (!has_right(state, NT_SUP_THREAD_SUSPEND_RESUME))
        return NT_STATUS_ACCESS_DENIED;
    if (state->suspend_count != 0 && !state->tracer->stopped)
        return NT_STATUS_NOT_SUPPORTED;
    if (state->suspend_count == 1 && nt_supervisor_continue(state->tracer))
        return translate_ptrace_error(errno);
    if (previous) *previous = state->suspend_count;
    if (state->suspend_count > 0)
        --state->suspend_count;
    return NT_STATUS_SUCCESS;
}

static bool valid_context(const struct nt_context64 *context, uint32_t supported)
{
    if (!context || ((uintptr_t)context & 15u) != 0) return false;
    const uint32_t flags = context->flags;
    return (flags & NT_SUP_CONTEXT_AMD64) != 0 &&
           (flags & supported) != 0 &&
           (flags & ~(NT_SUP_CONTEXT_AMD64 | supported)) == 0;
}

nt_status nt_supervisor_nt_get_context(struct nt_supervisor_nt_thread *state,
                                       struct nt_context64 *context)
{
    if (!state || !state->tracer || !context) return NT_STATUS_INVALID_PARAMETER;
    if (!has_right(state, NT_SUP_THREAD_GET_CONTEXT))
        return NT_STATUS_ACCESS_DENIED;
    const uint32_t supported = NT_SUP_CONTEXT_CONTROL |
                               NT_SUP_CONTEXT_INTEGER | NT_SUP_CONTEXT_SEGMENTS |
                               NT_SUP_CONTEXT_FLOATING_POINT;
    if (!valid_context(context, supported)) return NT_STATUS_INVALID_PARAMETER;
    if (state->suspend_count == 0) return NT_STATUS_NOT_SUPPORTED;
    struct user_regs_struct regs;
    if (nt_supervisor_get_guest_context(state->tracer, &state->scope, &regs))
        return translate_ptrace_error(errno);
    const uint32_t flags = context->flags;
    /* The NT_CONTEXT output is committed atomically only on complete
     * success. A failed second GETREGSET must not leak half a snapshot
     * into caller-owned memory. */
    _Alignas(16) struct nt_context64 snapshot = {0};
    snapshot.flags = flags;
    /* Windows CONTEXT x64 ABI fields, populated only for requested groups.
     * Neither debug registers nor FP/XSTATE are promised here. */
    if (flags & NT_SUP_CONTEXT_CONTROL) {
        snapshot.rip = regs.rip;
        snapshot.rsp = regs.rsp;
        snapshot.rbp = regs.rbp;
        snapshot.cs = (uint16_t)regs.cs;
        snapshot.ss = (uint16_t)regs.ss;
        snapshot.eflags = (uint32_t)regs.eflags;
    }
    if (flags & NT_SUP_CONTEXT_INTEGER) {
        snapshot.rax = regs.rax;
        snapshot.rbx = regs.rbx;
        snapshot.rcx = regs.rcx;
        snapshot.rdx = regs.rdx;
        snapshot.rsi = regs.rsi;
        snapshot.rdi = regs.rdi;
        snapshot.r8 = regs.r8;
        snapshot.r9 = regs.r9;
        snapshot.r10 = regs.r10;
        snapshot.r11 = regs.r11;
        snapshot.r12 = regs.r12;
        snapshot.r13 = regs.r13;
        snapshot.r14 = regs.r14;
        snapshot.r15 = regs.r15;
    }
    if (flags & NT_SUP_CONTEXT_SEGMENTS) {
        snapshot.ds = (uint16_t)regs.ds;
        snapshot.es = (uint16_t)regs.es;
        snapshot.fs = (uint16_t)regs.fs;
        snapshot.gs = (uint16_t)regs.gs;
    }
    if (flags & NT_SUP_CONTEXT_FLOATING_POINT) {
        /* Linux AMD64 NT_PRFPREG is a 512-byte FXSAVE-compatible image
         * (x87 + XMM0..XMM15 + MXCSR). CONTEXT.FltSave has the same
         * packed 512-byte storage. This is READ ONLY: writes, AVX/YMM
         * and extended XSTATE are deliberately not claimed. */
        _Static_assert(sizeof(struct user_fpregs_struct) == 512,
                       "AMD64 NT_PRFPREG FXSAVE layout");
        _Static_assert(sizeof(snapshot.floating) == 512,
                       "Windows AMD64 FltSave layout");
        _Alignas(16) struct user_fpregs_struct fp = {0};
        struct iovec fp_vector = {.iov_base = &fp, .iov_len = sizeof(fp)};
        if (ptrace(PTRACE_GETREGSET, state->tracer->tid,
                   (void *)(uintptr_t)NT_PRFPREG, &fp_vector) == -1)
            return translate_ptrace_error(errno);
        if (fp_vector.iov_len != sizeof(fp))
            return NT_STATUS_NOT_SUPPORTED;
        memcpy(snapshot.floating, &fp, sizeof(fp));
        snapshot.mxcsr = fp.mxcsr;
    }
    *context = snapshot;
    return NT_STATUS_SUCCESS;
}

nt_status nt_supervisor_nt_set_context(struct nt_supervisor_nt_thread *state,
                                       const struct nt_context64 *context)
{
    if (!state || !state->tracer || !context) return NT_STATUS_INVALID_PARAMETER;
    if (!has_right(state, NT_SUP_THREAD_SET_CONTEXT))
        return NT_STATUS_ACCESS_DENIED;
    /* A general CONTEXT_CONTROL/SEGMENTS/FP rewrite is not safe yet:
     * never allow the guest to change RIP/RSP/GS or the host unwind chain.
     * This first contract supports only the integer group, excluding
     * RBP/RSP which must remain unchanged by design. */
    if (!valid_context(context, NT_SUP_CONTEXT_INTEGER))
        return NT_STATUS_NOT_SUPPORTED;
    if (state->suspend_count == 0) return NT_STATUS_NOT_SUPPORTED;
    struct user_regs_struct original;
    if (nt_supervisor_get_guest_context(state->tracer, &state->scope, &original))
        return translate_ptrace_error(errno);
    struct user_regs_struct altered = original;
    altered.rax = context->rax;
    altered.rbx = context->rbx;
    altered.rcx = context->rcx;
    altered.rdx = context->rdx;
    altered.rsi = context->rsi;
    altered.rdi = context->rdi;
    altered.r8 = context->r8;
    altered.r9 = context->r9;
    altered.r10 = context->r10;
    altered.r11 = context->r11;
    altered.r12 = context->r12;
    altered.r13 = context->r13;
    altered.r14 = context->r14;
    altered.r15 = context->r15;
    /* No reported success without a second kernel-owned GETREGSET.
     * A write can fail after an ambiguous partial mutation, or the
     * kernel can normalise a requested register value. Both require
     * verification before the child is ever allowed to run again. */
    state->unsafe_register_state = true;
    struct iovec io = {.iov_base = &altered, .iov_len = sizeof(altered)};
    const bool wrote = ptrace(PTRACE_SETREGSET, state->tracer->tid,
                   (void *)(uintptr_t)NT_PRSTATUS, &io) == 0 &&
                   io.iov_len == sizeof(altered);
    struct user_regs_struct verified = {0};
    const bool readback = wrote &&
        nt_supervisor_get_registers(state->tracer, &verified) == 0;
#ifdef NT_SUP_TEST_FORCE_GPR_VERIFY_MISMATCH
    /* Compiled ONLY into the dedicated negative-test binary, not exposed
     * as a Windows API/environment switch or a production build flag.
     * The kernel really received the write; simulate a corrupt readback. */
    if (readback)
        verified.rax ^= UINT64_C(1);
#endif
    if (readback &&
        memcmp(&verified, &altered, sizeof(altered)) == 0) {
        state->unsafe_register_state = false;
        return NT_STATUS_SUCCESS;
    }

    /* Best effort rollback while the same tracee is still STOPPED.
     * The result is safe only if the *original* entire register set
     * can be read back byte-for-byte. Otherwise the parent must
     * terminate this exact pidfd-pinned guest, never PTRACE_DETACH. */
#ifndef NT_SUP_TEST_FORCE_GPR_ROLLBACK_FAILURE
    struct iovec undo = {.iov_base = &original, .iov_len = sizeof(original)};
    if (ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRSTATUS, &undo) == 0 &&
        undo.iov_len == sizeof(original) &&
        nt_supervisor_get_registers(state->tracer, &verified) == 0 &&
        memcmp(&verified, &original, sizeof(original)) == 0) {
        state->unsafe_register_state = false;
    }
#else
    /* Separate fatal-path test: simulate both a corrupt readback and
     * an impossible rollback. No resume is permitted after this point. */
#endif
    return NT_STATUS_NOT_SUPPORTED;
}

nt_status nt_supervisor_nt_queue_trusted_apc(struct nt_supervisor_nt_thread *state,
                                             uintptr_t entry, uintptr_t stack_top,
                                             uintptr_t argument,
                                             const _Atomic uint32_t *completion)
{
    if (!state || !state->tracer || !completion) return NT_STATUS_INVALID_PARAMETER;
    if (!has_right(state, NT_SUP_THREAD_SET_CONTEXT))
        return NT_STATUS_ACCESS_DENIED;
    if (state->suspend_count != 0) return NT_STATUS_NOT_SUPPORTED;
    struct user_regs_struct regs;
    if (nt_supervisor_interrupt_guest(state->tracer, &state->scope, 1500, &regs))
        return translate_ptrace_error(errno);
    if (nt_supervisor_invoke_trusted_guest_apc(state->tracer, &state->scope,
                                              entry, stack_top, argument,
                                              completion, 1500))
        return translate_ptrace_error(errno);
    return NT_STATUS_SUCCESS;
}
