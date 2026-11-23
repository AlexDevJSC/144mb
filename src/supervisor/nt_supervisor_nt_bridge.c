/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nt_supervisor_nt_bridge.h"
#include <errno.h>
#include <linux/elf.h>
#include <stddef.h>
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
    if (state->tracer->compromised || state->unsafe_register_state)
        return NT_STATUS_THREAD_IS_TERMINATING;
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
    if (state->tracer->compromised || state->unsafe_register_state)
        return NT_STATUS_THREAD_IS_TERMINATING;
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

/* Read kernel DR0–DR3,DR6,DR7 only on a stopped authenticated
 * guest. PEEKUSER requires the exact tracer-owned PID/TID; never trust
 * guest-supplied Linux offsets or kernel register values. */
static nt_status read_guest_debug_regs(struct nt_supervisor_nt_thread *state,
                                       uint64_t out[6])
{
    static const unsigned debug_index[6] = {0, 1, 2, 3, 6, 7};
    for (unsigned i = 0; i < 6; ++i) {
        const uintptr_t offset = offsetof(struct user, u_debugreg[0]) +
            debug_index[i] * sizeof(((struct user *)0)->u_debugreg[0]);
        errno = 0;
        const long observed = ptrace(PTRACE_PEEKUSER, state->tracer->tid,
                                     (void *)offset, NULL);
        if (errno != 0)
            return translate_ptrace_error(errno);
        out[i] = (uint64_t)(unsigned long)observed;
    }
    return NT_STATUS_SUCCESS;
}

/* Address-only DR0..DR3 writes while DR7 is disabled. No guest-triggered
 * hardware watchpoints, global breakpoints, DR6 status mutation or host
 * debugger interference. This does NOT implement full Windows DEBUG group
 * write semantics. Any ambiguous write quarantines the pinned guest. */
static nt_status set_inactive_guest_debug_addresses(
    struct nt_supervisor_nt_thread *state, const struct nt_context64 *context)
{
    uint64_t before[6] = {0}, actual[6] = {0};
    nt_status status = read_guest_debug_regs(state, before);
    if (status != NT_STATUS_SUCCESS) return status;
    if (before[5] != 0 || context->debug[5] != before[5] ||
        context->debug[4] != before[4])
        return NT_STATUS_NOT_SUPPORTED;
    for (unsigned i = 0; i < 4; ++i) {
        const uintptr_t pc = (uintptr_t)context->debug[i];
        bool valid = pc == 0 ||
            (pc >= state->scope.code_begin && pc < state->scope.code_end);
        for (size_t j = 0; !valid &&
             j < state->scope.additional_code_count; ++j)
            valid = pc >= state->scope.additional_code[j].begin &&
                    pc < state->scope.additional_code[j].end;
        if (!valid) return NT_STATUS_NOT_SUPPORTED;
    }
    state->unsafe_register_state = true;
    bool wrote = true;
    for (unsigned i = 0; i < 4; ++i) {
        const uintptr_t offset = offsetof(struct user, u_debugreg[0]) +
                                 i * sizeof(((struct user *)0)->u_debugreg[0]);
        if (ptrace(PTRACE_POKEUSER, state->tracer->tid, (void *)offset,
                   (void *)(uintptr_t)context->debug[i]) == -1) {
            wrote = false;
            break;
        }
    }
    const bool verified = wrote &&
        read_guest_debug_regs(state, actual) == NT_STATUS_SUCCESS &&
        memcmp(actual, context->debug, sizeof(actual)) == 0;
    if (verified) {
        state->unsafe_register_state = false;
        return NT_STATUS_SUCCESS;
    }
    bool restored = true;
    for (unsigned i = 0; i < 4; ++i) {
        const uintptr_t offset = offsetof(struct user, u_debugreg[0]) +
                                 i * sizeof(((struct user *)0)->u_debugreg[0]);
        if (ptrace(PTRACE_POKEUSER, state->tracer->tid, (void *)offset,
                   (void *)(uintptr_t)before[i]) == -1)
            restored = false;
    }
    if (restored && read_guest_debug_regs(state, actual) == NT_STATUS_SUCCESS &&
        memcmp(actual, before, sizeof(actual)) == 0)
        state->unsafe_register_state = false;
    else
        state->tracer->compromised = true;
    return NT_STATUS_NOT_SUPPORTED;
}

nt_status nt_supervisor_nt_get_context(struct nt_supervisor_nt_thread *state,
                                       struct nt_context64 *context)
{
    if (!state || !state->tracer || !context) return NT_STATUS_INVALID_PARAMETER;
    if (state->tracer->compromised || state->unsafe_register_state)
        return NT_STATUS_THREAD_IS_TERMINATING;
    if (!has_right(state, NT_SUP_THREAD_GET_CONTEXT))
        return NT_STATUS_ACCESS_DENIED;
    const uint32_t supported = NT_SUP_CONTEXT_CONTROL |
                               NT_SUP_CONTEXT_INTEGER | NT_SUP_CONTEXT_SEGMENTS |
                               NT_SUP_CONTEXT_FLOATING_POINT |
                               NT_SUP_CONTEXT_DEBUG_REGISTERS;
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
     * DEBUG and extended XSTATE are not promised; the 512-byte legacy FP
     * group has its own separately verified STOPPED-only write path. */
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
    if (flags & NT_SUP_CONTEXT_DEBUG_REGISTERS) {
        const nt_status debug_status =
            read_guest_debug_regs(state, snapshot.debug);
        if (debug_status != NT_STATUS_SUCCESS)
            return debug_status; /* never partially commit output */
    }
    if (flags & NT_SUP_CONTEXT_FLOATING_POINT) {
        /* Linux AMD64 NT_PRFPREG is a 512-byte FXSAVE-compatible image
         * (x87 + XMM0..XMM15 + MXCSR). CONTEXT.FltSave has the same
         * packed 512-byte storage. Legacy FP write transactions use
         * PTRACE_SETREGSET separately; AVX/YMM and XSTATE are not claimed. */
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

/* The legacy Windows CONTEXT_FLOATING_POINT group (x87/XMM0..15)
 * corresponds to Linux NT_PRFPREG, not to AVX/XSAVE or a guest FPU
 * context owned by the host. Permit writes ONLY on an authenticated,
 * kernel-stopped PE thread, with an exact readback/rollback transaction.
 * Never update control registers, RIP/RSP/GS or extended XSTATE here. */
static nt_status set_legacy_fp_context(struct nt_supervisor_nt_thread *state,
                                      const struct nt_context64 *context)
{
    _Static_assert(sizeof(struct user_fpregs_struct) == 512,
                   "Linux AMD64 NT_PRFPREG layout");
    _Static_assert(sizeof(context->floating) == 512,
                   "Windows AMD64 FltSave layout");
    uint32_t fp_mxcsr = 0;
    memcpy(&fp_mxcsr, context->floating + 24, sizeof(fp_mxcsr));
    if (fp_mxcsr != context->mxcsr ||
        (fp_mxcsr & ~UINT32_C(0x0000ffbf)) != 0)
        return NT_STATUS_INVALID_PARAMETER;

    _Alignas(16) struct user_fpregs_struct original = {0};
    struct iovec io = {.iov_base = &original, .iov_len = sizeof(original)};
    if (ptrace(PTRACE_GETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == -1)
        return translate_ptrace_error(errno);
    if (io.iov_len != sizeof(original))
        return NT_STATUS_NOT_SUPPORTED;

    _Alignas(16) struct user_fpregs_struct changed = {0};
    memcpy(&changed, context->floating, sizeof(changed));
    _Alignas(16) struct user_fpregs_struct verified = {0};

    /* From this point a partial kernel write is possible. Keep the
     * tracee stopped and quarantined until an exact state is observed. */
    state->unsafe_register_state = true;
    io = (struct iovec){.iov_base = &changed, .iov_len = sizeof(changed)};
    const bool wrote = ptrace(PTRACE_SETREGSET, state->tracer->tid,
                             (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
                       io.iov_len == sizeof(changed);
    io = (struct iovec){.iov_base = &verified, .iov_len = sizeof(verified)};
    const bool readback = wrote &&
        ptrace(PTRACE_GETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
        io.iov_len == sizeof(verified);
#ifdef NT_SUP_TEST_FORCE_FP_VERIFY_MISMATCH
    extern _Atomic uint32_t nt_sup_test_fp_verify_corrupted;
    if (readback) {
        verified.mxcsr ^= UINT32_C(0x2000);
        atomic_fetch_add_explicit(&nt_sup_test_fp_verify_corrupted, 1,
                                  memory_order_relaxed);
    }
#endif
    if (readback && memcmp(&verified, &changed, sizeof(changed)) == 0) {
        state->unsafe_register_state = false;
        return NT_STATUS_SUCCESS;
    }

    /* An unverified write is not a Windows success. Even rollback must
     * be verified against the original whole 512-byte kernel image. */
#ifndef NT_SUP_TEST_FORCE_FP_ROLLBACK_FAILURE
    io = (struct iovec){.iov_base = &original, .iov_len = sizeof(original)};
    if (ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
        io.iov_len == sizeof(original)) {
        io = (struct iovec){.iov_base = &verified, .iov_len = sizeof(verified)};
        if (ptrace(PTRACE_GETREGSET, state->tracer->tid,
                   (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
            io.iov_len == sizeof(verified) &&
            memcmp(&verified, &original, sizeof(original)) == 0)
            state->unsafe_register_state = false;
    }
#endif
    if (state->unsafe_register_state)
        state->tracer->compromised = true;
    return NT_STATUS_NOT_SUPPORTED;
}

/* Atomic-to-the-guest transaction for TWO reviewed NT CONTEXT groups.
 * The Linux kernel has no atomic multi-regset ptrace operation; keep the
 * guest stopped, verify both reads and compensate both writes on error.
 * A failed compensation quarantines the exact pidfd-pinned child.
 * Never extend this path to RIP/RSP/segments/debug or variable XSTATE
 * without a separate proof of stack, privilege and regset ownership. */
static nt_status set_integer_fp_context(struct nt_supervisor_nt_thread *state,
                                        const struct nt_context64 *context)
{
    uint32_t image_mxcsr = 0;
    memcpy(&image_mxcsr, context->floating + 24, sizeof(image_mxcsr));
    if (image_mxcsr != context->mxcsr ||
        (image_mxcsr & ~UINT32_C(0x0000ffbf)) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    struct user_regs_struct original_regs = {0};
    if (nt_supervisor_get_guest_context(state->tracer, &state->scope,
                                        &original_regs))
        return translate_ptrace_error(errno);
    _Static_assert(sizeof(struct user_fpregs_struct) == 512,
                   "AMD64 FXSAVE must be exactly 512 bytes");
    _Alignas(16) struct user_fpregs_struct original_fp = {0};
    struct iovec io = {.iov_base = &original_fp,
                       .iov_len = sizeof(original_fp)};
    if (ptrace(PTRACE_GETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == -1)
        return translate_ptrace_error(errno);
    if (io.iov_len != sizeof(original_fp))
        return NT_STATUS_NOT_SUPPORTED;

    struct user_regs_struct updated_regs = original_regs;
    updated_regs.rax = context->rax;
    updated_regs.rbx = context->rbx;
    updated_regs.rcx = context->rcx;
    updated_regs.rdx = context->rdx;
    updated_regs.rsi = context->rsi;
    updated_regs.rdi = context->rdi;
    updated_regs.r8 = context->r8;
    updated_regs.r9 = context->r9;
    updated_regs.r10 = context->r10;
    updated_regs.r11 = context->r11;
    updated_regs.r12 = context->r12;
    updated_regs.r13 = context->r13;
    updated_regs.r14 = context->r14;
    updated_regs.r15 = context->r15;
    _Alignas(16) struct user_fpregs_struct updated_fp = {0};
    memcpy(&updated_fp, context->floating, sizeof(updated_fp));

    struct user_regs_struct read_regs = {0};
    _Alignas(16) struct user_fpregs_struct read_fp = {0};
    state->unsafe_register_state = true;
    io = (struct iovec){.iov_base = &updated_fp, .iov_len = sizeof(updated_fp)};
    const bool wrote_fp =
        ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
        io.iov_len == sizeof(updated_fp);
    io = (struct iovec){.iov_base = &updated_regs,
                        .iov_len = sizeof(updated_regs)};
    const bool wrote_regs = wrote_fp &&
        ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRSTATUS, &io) == 0 &&
        io.iov_len == sizeof(updated_regs);
    io = (struct iovec){.iov_base = &read_fp, .iov_len = sizeof(read_fp)};
    const bool checked = wrote_regs &&
        nt_supervisor_get_registers(state->tracer, &read_regs) == 0 &&
        ptrace(PTRACE_GETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
        io.iov_len == sizeof(read_fp);
#ifdef NT_SUP_TEST_FORCE_MIXED_VERIFY_MISMATCH
    if (checked)
        read_fp.mxcsr ^= UINT32_C(0x2000);
#endif
    if (checked &&
        memcmp(&updated_regs, &read_regs, sizeof(read_regs)) == 0 &&
        memcmp(&updated_fp, &read_fp, sizeof(read_fp)) == 0) {
        state->unsafe_register_state = false;
        return NT_STATUS_SUCCESS;
    }

    /* The first kernel write might have taken effect even if the next
     * operation failed. Restore BOTH groups and verify the kernel owns
     * the exact original image before any future resume/detach. */
    io = (struct iovec){.iov_base = &original_fp,
                        .iov_len = sizeof(original_fp)};
    const bool restored_fp =
        ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
        io.iov_len == sizeof(original_fp);
    io = (struct iovec){.iov_base = &original_regs,
                        .iov_len = sizeof(original_regs)};
    const bool restored_regs =
        ptrace(PTRACE_SETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRSTATUS, &io) == 0 &&
        io.iov_len == sizeof(original_regs);
    io = (struct iovec){.iov_base = &read_fp, .iov_len = sizeof(read_fp)};
    const bool restoration_verified = restored_fp && restored_regs &&
        nt_supervisor_get_registers(state->tracer, &read_regs) == 0 &&
        ptrace(PTRACE_GETREGSET, state->tracer->tid,
               (void *)(uintptr_t)NT_PRFPREG, &io) == 0 &&
        io.iov_len == sizeof(read_fp) &&
        memcmp(&original_regs, &read_regs, sizeof(read_regs)) == 0 &&
        memcmp(&original_fp, &read_fp, sizeof(read_fp)) == 0;
#ifdef NT_SUP_TEST_FORCE_MIXED_ROLLBACK_FAILURE
    (void)restoration_verified;
    state->tracer->compromised = true;
#else
    if (restoration_verified)
        state->unsafe_register_state = false;
    else
        state->tracer->compromised = true;
#endif
    return NT_STATUS_NOT_SUPPORTED;
}

/* Bounded real CONTEXT_CONTROL write for a suspended, authenticated
 * owned PE thread. A guest may change arithmetic condition codes but
 * may NOT move RIP/RSP/RBP, selectors, IF, TF or privileged flags.
 * Arbitrary control-flow rewrites lack a stack/unwind ownership proof
 * and remain unsupported. The complete kernel PRSTATUS is verified. */
static nt_status set_control_condition_flags(
    struct nt_supervisor_nt_thread *state, const struct nt_context64 *context)
{
    struct user_regs_struct before = {0};
    if (nt_supervisor_get_guest_context(state->tracer, &state->scope, &before))
        return translate_ptrace_error(errno);
    enum { CONDITION_MASK = 0x8d5u };
    if (context->rip != before.rip || context->rsp != before.rsp ||
        context->rbp != before.rbp || context->cs != (uint16_t)before.cs ||
        context->ss != (uint16_t)before.ss ||
        ((uint64_t)context->eflags & ~(uint64_t)CONDITION_MASK) !=
        (before.eflags & ~(uint64_t)CONDITION_MASK))
        return NT_STATUS_NOT_SUPPORTED;
    struct user_regs_struct changed = before;
    changed.eflags = (before.eflags & ~(uint64_t)CONDITION_MASK) |
                     (context->eflags & CONDITION_MASK);
    if (changed.eflags == before.eflags)
        return NT_STATUS_SUCCESS;
    state->unsafe_register_state = true;
    struct iovec io = {.iov_base = &changed, .iov_len = sizeof(changed)};
    const bool wrote = ptrace(PTRACE_SETREGSET, state->tracer->tid,
        (void *)(uintptr_t)NT_PRSTATUS, &io) == 0 &&
        io.iov_len == sizeof(changed);
    struct user_regs_struct actual = {0};
    const bool verified = wrote &&
        nt_supervisor_get_registers(state->tracer, &actual) == 0 &&
        memcmp(&actual, &changed, sizeof(changed)) == 0;
    if (verified) {
        state->unsafe_register_state = false;
        return NT_STATUS_SUCCESS;
    }
    io = (struct iovec){.iov_base = &before, .iov_len = sizeof(before)};
    const bool rolled_back = ptrace(PTRACE_SETREGSET, state->tracer->tid,
        (void *)(uintptr_t)NT_PRSTATUS, &io) == 0 &&
        io.iov_len == sizeof(before) &&
        nt_supervisor_get_registers(state->tracer, &actual) == 0 &&
        memcmp(&actual, &before, sizeof(before)) == 0;
    if (rolled_back)
        state->unsafe_register_state = false;
    else
        state->tracer->compromised = true;
    return NT_STATUS_NOT_SUPPORTED;
}

nt_status nt_supervisor_nt_set_context(struct nt_supervisor_nt_thread *state,
                                       const struct nt_context64 *context)
{
    if (!state || !state->tracer || !context) return NT_STATUS_INVALID_PARAMETER;
    if (state->tracer->compromised || state->unsafe_register_state)
        return NT_STATUS_THREAD_IS_TERMINATING;
    if (!has_right(state, NT_SUP_THREAD_SET_CONTEXT))
        return NT_STATUS_ACCESS_DENIED;
    /* Arithmetic-only CONTROL, INTEGER and legacy FXSAVE are real
     * bounded transactions. Other CONTROL changes, SEGMENTS, DEBUG
     * and variable XSTATE still fail closed. */
    if (!valid_context(context, NT_SUP_CONTEXT_CONTROL |
                                NT_SUP_CONTEXT_INTEGER |
                                NT_SUP_CONTEXT_FLOATING_POINT |
                                NT_SUP_CONTEXT_DEBUG_REGISTERS))
        return NT_STATUS_NOT_SUPPORTED;
    if (state->suspend_count == 0) return NT_STATUS_NOT_SUPPORTED;
    const uint32_t groups = context->flags & ~NT_SUP_CONTEXT_AMD64;
    if (groups == NT_SUP_CONTEXT_CONTROL)
        return set_control_condition_flags(state, context);
    if (groups == NT_SUP_CONTEXT_DEBUG_REGISTERS)
        return set_inactive_guest_debug_addresses(state, context);
    if (groups == (NT_SUP_CONTEXT_INTEGER | NT_SUP_CONTEXT_FLOATING_POINT))
        return set_integer_fp_context(state, context);
    if (groups == NT_SUP_CONTEXT_FLOATING_POINT)
        return set_legacy_fp_context(state, context);
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
    extern _Atomic uint32_t nt_sup_test_verify_corrupted;
    if (readback) {
        verified.rax ^= UINT64_C(1);
        atomic_fetch_add_explicit(&nt_sup_test_verify_corrupted, 1,
                                  memory_order_relaxed);
    }
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
    if (state->unsafe_register_state)
        state->tracer->compromised = true;
    return NT_STATUS_NOT_SUPPORTED;
}

nt_status nt_supervisor_nt_queue_trusted_apc_guarded(
    struct nt_supervisor_nt_thread *state,
    uintptr_t entry, uintptr_t stack_top, uintptr_t argument,
    const _Atomic uint32_t *completion,
    const _Atomic uint32_t *cancel, int peer_fd)
{
    if (!state || !state->tracer || !completion) return NT_STATUS_INVALID_PARAMETER;
    if (state->tracer->compromised || state->unsafe_register_state)
        return NT_STATUS_THREAD_IS_TERMINATING;
    if (!has_right(state, NT_SUP_THREAD_SET_CONTEXT))
        return NT_STATUS_ACCESS_DENIED;
    if (state->suspend_count != 0) return NT_STATUS_NOT_SUPPORTED;
    struct user_regs_struct regs;
    if (nt_supervisor_interrupt_guest(state->tracer, &state->scope, 1500, &regs))
        return translate_ptrace_error(errno);
    if (nt_supervisor_invoke_trusted_guest_apc_guarded(
            state->tracer, &state->scope, entry, stack_top, argument,
            completion, 1500, cancel, peer_fd))
        return translate_ptrace_error(errno);
    return NT_STATUS_SUCCESS;
}

/* Existing experimental caller retains its original, unguarded contract. */
nt_status nt_supervisor_nt_queue_trusted_apc(struct nt_supervisor_nt_thread *state,
                                             uintptr_t entry, uintptr_t stack_top,
                                             uintptr_t argument,
                                             const _Atomic uint32_t *completion)
{
    return nt_supervisor_nt_queue_trusted_apc_guarded(
        state, entry, stack_top, argument, completion, NULL, -1);
}
