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
                               NT_SUP_CONTEXT_INTEGER | NT_SUP_CONTEXT_SEGMENTS;
    if (!valid_context(context, supported)) return NT_STATUS_INVALID_PARAMETER;
    if (state->suspend_count == 0) return NT_STATUS_NOT_SUPPORTED;
    struct user_regs_struct regs;
    if (nt_supervisor_get_guest_context(state->tracer, &state->scope, &regs))
        return translate_ptrace_error(errno);
    const uint32_t flags = context->flags;
    /* Windows CONTEXT x64 ABI fields, populated only for requested groups.
     * Neither debug registers nor FP/XSTATE are promised here. */
    if (flags & NT_SUP_CONTEXT_CONTROL) {
        context->rip = regs.rip;
        context->rsp = regs.rsp;
        context->rbp = regs.rbp;
        context->cs = (uint16_t)regs.cs;
        context->ss = (uint16_t)regs.ss;
        context->eflags = (uint32_t)regs.eflags;
    }
    if (flags & NT_SUP_CONTEXT_INTEGER) {
        context->rax = regs.rax;
        context->rbx = regs.rbx;
        context->rcx = regs.rcx;
        context->rdx = regs.rdx;
        context->rsi = regs.rsi;
        context->rdi = regs.rdi;
        context->r8 = regs.r8;
        context->r9 = regs.r9;
        context->r10 = regs.r10;
        context->r11 = regs.r11;
        context->r12 = regs.r12;
        context->r13 = regs.r13;
        context->r14 = regs.r14;
        context->r15 = regs.r15;
    }
    if (flags & NT_SUP_CONTEXT_SEGMENTS) {
        context->ds = (uint16_t)regs.ds;
        context->es = (uint16_t)regs.es;
        context->fs = (uint16_t)regs.fs;
        context->gs = (uint16_t)regs.gs;
    }
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
    struct iovec io = {.iov_base = &altered, .iov_len = sizeof(altered)};
    if (ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRSTATUS, &io) == -1)
        return translate_ptrace_error(errno);
    return NT_STATUS_SUCCESS;
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
