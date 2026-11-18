/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nt_thread_supervisor.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/elf.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "nt_thread_supervisor requires Linux AMD64"
#endif

static int read_identity(pid_t process, pid_t tid, pid_t *ppid, pid_t *tgid)
{
    char path[96], line[160];
    if (process <= 1 || tid <= 1) { errno = EINVAL; return -1; }
    int size = snprintf(path, sizeof(path), "/proc/%ld/task/%ld/status", (long)process, (long)tid);
    if (size < 0 || (size_t)size >= sizeof(path)) { errno = EOVERFLOW; return -1; }
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int have_parent = 0, have_group = 0;
    while (fgets(line, sizeof(line), file)) {
        long value;
        if (sscanf(line, "PPid:%ld", &value) == 1 && value > 0 && value <= INT_MAX) {
            *ppid = (pid_t)value; have_parent = 1;
        }
        if (sscanf(line, "Tgid:%ld", &value) == 1 && value > 0 && value <= INT_MAX) {
            *tgid = (pid_t)value; have_group = 1;
        }
    }
    int close_error = fclose(file);
    if (close_error || !have_parent || !have_group) { errno = EPROTO; return -1; }
    return 0;
}

static int child_is_direct(pid_t child, pid_t tid)
{
    pid_t parent = 0, group = 0;
    if (read_identity(child, child, &parent, &group) || parent != getpid() || group != child) {
        errno = EPERM;
        return -1;
    }
    if (read_identity(child, tid, &parent, &group) || group != child) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

void nt_supervisor_init(struct nt_thread_supervisor *sup)
{
    if (sup) *sup = (struct nt_thread_supervisor){.pidfd = -1};
}

int nt_supervisor_seize_child(struct nt_thread_supervisor *sup, pid_t child, pid_t tid)
{
    if (!sup || sup->seized || sup->pidfd != -1) { errno = EINVAL; return -1; }
    if (child_is_direct(child, tid)) return -1;
#ifdef SYS_pidfd_open
    int pidfd = (int)syscall(SYS_pidfd_open, child, 0);
#else
    int pidfd = -1; errno = ENOTSUP;
#endif
    if (pidfd < 0) return -1;
    struct pollfd pfd = {.fd = pidfd, .events = POLLIN};
    if (poll(&pfd, 1, 0) != 0) { close(pidfd); errno = ESRCH; return -1; }
    if (child_is_direct(child, tid)) { int saved = errno; close(pidfd); errno = saved; return -1; }
    if (ptrace(PTRACE_SEIZE, tid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL) == -1) {
        int saved = errno; close(pidfd); errno = saved; return -1;
    }
    *sup = (struct nt_thread_supervisor){.child = child, .tid = tid, .pidfd = pidfd,
                                         .seized = true, .stopped = false};
    return 0;
}

static uint64_t mono_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

int nt_supervisor_interrupt(struct nt_thread_supervisor *sup, unsigned timeout_ms)
{
    if (!sup || !sup->seized) { errno = EINVAL; return -1; }
    if (sup->stopped) return 0;
    if (timeout_ms == 0 || timeout_ms > 10000) { errno = EINVAL; return -1; }
    if (ptrace(PTRACE_INTERRUPT, sup->tid, NULL, NULL) == -1) return -1;
    uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (nt_supervisor_apc_interrupted(cancel, peer_fd))
            return nt_supervisor_apc_fail_closed(sup, ECANCELED);
        int status = 0;
        pid_t observed = waitpid(sup->tid, &status, __WALL | WNOHANG);
        if (observed == sup->tid) {
            if (WIFSTOPPED(status)) {
                sup->stopped = true; /* Even an unexpected signal-stop needs cleanup. */
                const unsigned event = (unsigned)status >> 16;
                if (event == PTRACE_EVENT_STOP && WSTOPSIG(status) == SIGTRAP) {
                    sup->pending_signal = 0;
                    return 0;
                }
                /* For actual signal-delivery stops retain the signal so
                 * CONT/DETACH can deliver it to the guest exactly once. */
                sup->pending_signal = event == 0 ? WSTOPSIG(status) : 0;
                errno = EPROTO;
                return -1;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                sup->exited = true;
                sup->seized = false;
                errno = ESRCH;
                return -1;
            }
            errno = EPROTO;
            return -1;
        }
        if (observed < 0) {
            if (errno == ECHILD || errno == ESRCH) {
                sup->exited = true;
                sup->seized = false;
            }
            return -1;
        }
        if (mono_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&pause, NULL);
    }
}

int nt_supervisor_get_registers(struct nt_thread_supervisor *sup, struct user_regs_struct *out)
{
    if (!sup || !sup->seized || !sup->stopped || !out) { errno = EINVAL; return -1; }
    struct iovec vec = {.iov_base = out, .iov_len = sizeof(*out)};
    if (ptrace(PTRACE_GETREGSET, sup->tid, (void *)(uintptr_t)NT_PRSTATUS, &vec) == -1) return -1;
    if (vec.iov_len != sizeof(*out)) { errno = EPROTO; return -1; }
    return 0;
}

int nt_supervisor_set_rax(struct nt_thread_supervisor *sup, uint64_t value)
{
    if (sup && sup->compromised) { errno = EPERM; return -1; }
    struct user_regs_struct regs;
    if (nt_supervisor_get_registers(sup, &regs)) return -1;
    regs.rax = value;
    struct iovec vec = {.iov_base = &regs, .iov_len = sizeof(regs)};
    if (ptrace(PTRACE_SETREGSET, sup->tid, (void *)(uintptr_t)NT_PRSTATUS, &vec) == -1) return -1;
    if (vec.iov_len != sizeof(regs)) { errno = EPROTO; return -1; }
    return 0;
}

int nt_supervisor_continue(struct nt_thread_supervisor *sup)
{
    if (sup && sup->compromised) { errno = EPERM; return -1; }
    if (!sup || !sup->seized || !sup->stopped) { errno = EINVAL; return -1; }
    if (ptrace(PTRACE_CONT, sup->tid, NULL,
               (void *)(uintptr_t)sup->pending_signal) == -1)
        return -1;
    sup->stopped = false;
    sup->pending_signal = 0;
    return 0;
}

int nt_supervisor_detach(struct nt_thread_supervisor *sup)
{
    if (sup && sup->compromised) { errno = EPERM; return -1; }
    if (!sup || !sup->seized || !sup->stopped) { errno = EINVAL; return -1; }
    if (ptrace(PTRACE_DETACH, sup->tid, NULL,
               (void *)(uintptr_t)sup->pending_signal) == -1)
        return -1;
    sup->seized = false;
    sup->stopped = false;
    sup->pending_signal = 0;
    return 0;
}


/* The direct parent owns these immutable code ranges. Never fill them
 * from phase flags or a range supplied in an unauthenticated guest RPC.
 * A loaded-after-fork DLL and RW->RX JIT mapping are excluded. */
static bool trusted_guest_rip(const struct nt_supervisor_guest_scope *scope,
                               uintptr_t pc)
{
    if (!scope || !scope->code_begin ||
        scope->code_end <= scope->code_begin ||
        scope->additional_code_count > NT_GUEST_ADDITIONAL_CODE_MAX ||
        (scope->additional_code_count && !scope->additional_code))
        return false;
    if (pc >= scope->code_begin && pc < scope->code_end)
        return true;
    for (size_t i = 0; i < scope->additional_code_count; ++i) {
        const struct nt_guest_code_range range = scope->additional_code[i];
        if (!range.begin || range.end <= range.begin)
            return false;
        if (pc >= range.begin && pc < range.end)
            return true;
    }
    return false;
}

/* Check an already-stopped, real Linux AMD64 CPU context. The phase is a
 * trusted boundary protocol, *not* sufficient proof on its own: verify
 * actual RIP and GS_BASE supplied by the kernel. Do not read guest memory
 * or take locks in the tracee while it is stopped. */
int nt_supervisor_get_guest_context(struct nt_thread_supervisor *sup,
                                    const struct nt_supervisor_guest_scope *scope,
                                    struct user_regs_struct *out)
{
    if (!scope || !scope->boundary || !out || scope->code_begin == 0 ||
        scope->code_end <= scope->code_begin || scope->teb == 0) {
        errno = EINVAL;
        return -1;
    }
    const uint32_t transition_before =
        atomic_load_explicit(&scope->boundary->transition, memory_order_acquire);
    const uint32_t phase_before =
        atomic_load_explicit(&scope->boundary->phase, memory_order_acquire);
    if (nt_supervisor_get_registers(sup, out))
        return -1;
    const uint32_t phase_after =
        atomic_load_explicit(&scope->boundary->phase, memory_order_acquire);
    const uint32_t transition_after =
        atomic_load_explicit(&scope->boundary->transition, memory_order_acquire);
    if (phase_before != NT_GUEST_BOUNDARY_GUEST ||
        phase_after != NT_GUEST_BOUNDARY_GUEST ||
        transition_before != transition_after ||
        !trusted_guest_rip(scope, out->rip) ||
        out->rsp == 0 || out->gs_base != scope->teb) {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

int nt_supervisor_interrupt_guest(struct nt_thread_supervisor *sup,
                                  const struct nt_supervisor_guest_scope *scope,
                                  unsigned timeout_ms, struct user_regs_struct *out)
{
    if (!out || !scope || !scope->boundary) { errno = EINVAL; return -1; }
    if (nt_supervisor_interrupt(sup, timeout_ms))
        return -1;
    if (nt_supervisor_get_guest_context(sup, scope, out) == 0)
        return 0;
    /* A host transition may hold a mutex: never report a successful
     * suspension and never leave it stopped. Return the original failure
     * only if PTRACE_CONT succeeds. On resume error, keep failure visible
     * and let the caller terminate its own child, not continue unsafely. */
    const int saved = errno;
    if (nt_supervisor_continue(sup))
        return -1;
    errno = saved;
    return -1;
}

int nt_supervisor_set_guest_rax(struct nt_thread_supervisor *sup,
                                const struct nt_supervisor_guest_scope *scope,
                                uint64_t value)
{
    struct user_regs_struct regs;
    if (nt_supervisor_get_guest_context(sup, scope, &regs))
        return -1;
    return nt_supervisor_set_rax(sup, value);
}


/* An APC has changed the interrupted guest's CPU state. If anything
 * after that write is ambiguous, it is unsafe to DETACH or CONT: only the
 * exact direct child identified by pidfd may be terminated. */
static int nt_supervisor_apc_fail_closed(struct nt_thread_supervisor *sup,
                                         int failure)
{
    if (sup && sup->seized && !sup->exited)
        (void)nt_supervisor_terminate_compromised_child(sup);
    errno = failure ? failure : EPROTO;
    return -1;
}

/* The cancellation flag and socket belong to the trusted supervisor, not
 * to the untrusted PE. POLLHUP/POLLRDHUP indicate that the caller vanished;
 * we never consume a request packet or reinterpret user memory as policy.
 * This is an opt-in fixture guard, NOT a general-purpose APC cancel API. */
static bool nt_supervisor_apc_interrupted(const _Atomic uint32_t *cancel,
                                          int peer_fd)
{
    if (cancel && atomic_load_explicit(cancel, memory_order_acquire) != 0)
        return true;
    if (peer_fd < 0)
        return false;
    struct pollfd watcher = {.fd = peer_fd, .events = POLLIN | POLLRDHUP};
    int ready = poll(&watcher, 1, 0);
    if (ready < 0)
        return errno != EINTR;
    return ready > 0 &&
           (watcher.revents & (POLLHUP | POLLRDHUP | POLLERR | POLLNVAL)) != 0;
}

/* INT3-terminated, trusted-fixture-only special APC proof. A complete
 * register+XSTATE round trip is required: restoring integer registers
 * alone after a callback would silently corrupt interrupted AVX code.
 * This is not safe for arbitrary Windows images, SEH, or host callbacks;
 * only a reviewed, import-free PE with an isolated APC stack may call it.
 * Any unprovable mutation terminates ONLY the pidfd-pinned child. */
int nt_supervisor_invoke_trusted_guest_apc_guarded(
    struct nt_thread_supervisor *sup,
    const struct nt_supervisor_guest_scope *scope,
    uintptr_t guest_entry, uintptr_t guest_stack_top, uintptr_t guest_argument,
    const _Atomic uint32_t *guest_completion, unsigned timeout_ms,
    const _Atomic uint32_t *cancel, int peer_fd)
{
    if (sup && sup->compromised) { errno = EPERM; return -1; }
    if (!sup || !sup->stopped || !scope || !guest_completion ||
        guest_entry < scope->code_begin || guest_entry >= scope->code_end ||
        guest_stack_top < (uintptr_t)0x10000 || timeout_ms == 0 ||
        timeout_ms > 10000u) {
        errno = EINVAL;
        return -1;
    }
    struct user_regs_struct original;
    if (nt_supervisor_get_guest_context(sup, scope, &original))
        return -1;
    if ((original.eflags & UINT64_C(0x100)) != 0) {
        /* Single-step/debugger nested execution is outside this contract. */
        errno = ENOTSUP;
        return -1;
    }
    /* Before any state-changing ptrace, a canceled job can fail cleanly.
     * After a mutation, cancellation instead requires quarantine. */
    if (nt_supervisor_apc_interrupted(cancel, peer_fd)) {
        errno = ECANCELED;
        return -1;
    }
    const uint32_t before =
        atomic_load_explicit(guest_completion, memory_order_acquire);
    /* Bound state capture by 64 KiB, aligned for XSAVE. An unavailable
     * XSTATE GET/SET is a hard failure; do not silently degrade to FPREGS. */
    _Alignas(64) unsigned char xstate[65536];
    _Alignas(64) unsigned char observed_xstate[65536];
    struct iovec state = {.iov_base = xstate, .iov_len = sizeof(xstate)};
    if (ptrace(PTRACE_GETREGSET, sup->tid, (void *)(uintptr_t)NT_X86_XSTATE,
               &state) == -1)
        return -1;
    if (state.iov_len < 512 || state.iov_len > sizeof(xstate)) {
        errno = EPROTO;
        return -1;
    }
    if (nt_supervisor_apc_interrupted(cancel, peer_fd)) {
        errno = ECANCELED;
        return -1;
    }
    const size_t xstate_size = state.iov_len;
    struct iovec restore = {.iov_base = xstate, .iov_len = xstate_size};
    /* This first SETREGSET is itself potentially mutating, even though the
     * value came from the kernel: verify exact readback before changing RIP.
     * A short transfer, normalization or unavailable XSTATE is not PASS. */
    if (ptrace(PTRACE_SETREGSET, sup->tid, (void *)(uintptr_t)NT_X86_XSTATE,
               &restore) == -1 || restore.iov_len != xstate_size)
        return nt_supervisor_apc_fail_closed(sup, EPROTO);
    struct iovec read_xstate = {.iov_base = observed_xstate, .iov_len = xstate_size};
    if (ptrace(PTRACE_GETREGSET, sup->tid, (void *)(uintptr_t)NT_X86_XSTATE,
               &read_xstate) == -1 || read_xstate.iov_len != xstate_size ||
        memcmp(observed_xstate, xstate, xstate_size) != 0)
        return nt_supervisor_apc_fail_closed(sup, EPROTO);

    struct user_regs_struct inserted = original;
    inserted.rip = (uint64_t)guest_entry;
    /* MS x64 entry: RSP%16==8, with 32 bytes shadow space above RSP.
     * The test owns a separate >=64-KiB mapped guest stack. */
    inserted.rsp = ((uint64_t)guest_stack_top & ~UINT64_C(15)) - UINT64_C(72);
    inserted.rcx = (uint64_t)guest_argument;
    struct iovec gpr = {.iov_base = &inserted, .iov_len = sizeof(inserted)};
    if (ptrace(PTRACE_SETREGSET, sup->tid, (void *)(uintptr_t)NT_PRSTATUS,
               &gpr) == -1 || gpr.iov_len != sizeof(inserted))
        return nt_supervisor_apc_fail_closed(sup, EPROTO);
    struct user_regs_struct verified_regs;
    if (nt_supervisor_get_registers(sup, &verified_regs) ||
        memcmp(&verified_regs, &inserted, sizeof(inserted)) != 0)
        return nt_supervisor_apc_fail_closed(sup, EPROTO);
    if (nt_supervisor_apc_interrupted(cancel, peer_fd))
        return nt_supervisor_apc_fail_closed(sup, ECANCELED);
    if (nt_supervisor_continue(sup))
        return nt_supervisor_apc_fail_closed(sup, errno);

    const uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        int status = 0;
        pid_t observed = waitpid(sup->tid, &status, __WALL | WNOHANG);
        if (observed == sup->tid) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                sup->exited = true;
                sup->seized = false;
                errno = ESRCH;
                return -1;
            }
            if (WIFSTOPPED(status) && WSTOPSIG(status) == SIGTRAP &&
                ((unsigned)status >> 16) == 0) {
                sup->stopped = true;
                sup->pending_signal = 0; /* Synthetic INT3 stays in supervisor. */
                struct user_regs_struct trap;
                if (nt_supervisor_get_guest_context(sup, scope, &trap))
                    return nt_supervisor_apc_fail_closed(sup, errno);
                uint32_t delivered =
                    atomic_load_explicit(guest_completion, memory_order_acquire);
#ifdef NT_SUP_TEST_APC_FORCE_COMPLETION_MISMATCH
                /* Test-only injection after a REAL PE INT3 and mutable
                 * GPR/XSTATE; never enabled in runtime production builds. */
                delivered ^= UINT32_C(1);
#endif
                if (delivered != before + UINT32_C(1))
                    return nt_supervisor_apc_fail_closed(sup, EPROTO);

                /* Restore XSTATE FIRST, then the original GPRs, and prove
                 * exact readback of both before allowing guest execution. */
                restore.iov_len = xstate_size;
                if (ptrace(PTRACE_SETREGSET, sup->tid,
                           (void *)(uintptr_t)NT_X86_XSTATE, &restore) == -1 ||
                    restore.iov_len != xstate_size)
                    return nt_supervisor_apc_fail_closed(sup, EPROTO);
                gpr.iov_base = &original;
                gpr.iov_len = sizeof(original);
                if (ptrace(PTRACE_SETREGSET, sup->tid,
                           (void *)(uintptr_t)NT_PRSTATUS, &gpr) == -1 ||
                    gpr.iov_len != sizeof(original))
                    return nt_supervisor_apc_fail_closed(sup, EPROTO);
                if (nt_supervisor_get_registers(sup, &verified_regs) ||
                    memcmp(&verified_regs, &original, sizeof(original)) != 0)
                    return nt_supervisor_apc_fail_closed(sup, EPROTO);
                read_xstate.iov_len = xstate_size;
                if (ptrace(PTRACE_GETREGSET, sup->tid,
                           (void *)(uintptr_t)NT_X86_XSTATE, &read_xstate) == -1 ||
                    read_xstate.iov_len != xstate_size)
                    return nt_supervisor_apc_fail_closed(sup, EPROTO);
#ifdef NT_SUP_TEST_APC_FORCE_RESTORE_XSTATE_MISMATCH
                /* Test-only, injected after real XSTATE restoration/readback.
                 * The guest must not resume if even one byte differs. */
                observed_xstate[0] ^= UINT8_C(1);
#endif
                if (memcmp(observed_xstate, xstate, xstate_size) != 0)
                    return nt_supervisor_apc_fail_closed(sup, EPROTO);
                if (nt_supervisor_apc_interrupted(cancel, peer_fd))
                    return nt_supervisor_apc_fail_closed(sup, ECANCELED);
                if (nt_supervisor_continue(sup))
                    return nt_supervisor_apc_fail_closed(sup, errno);
                return 0;
            }
            /* Unexpected signal or ptrace event while running injected
             * code. Never deliver it to an overwritten RIP/RSP. */
            if (WIFSTOPPED(status))
                sup->stopped = true;
            return nt_supervisor_apc_fail_closed(sup, EPROTO);
        }
        if (observed < 0) {
            if (errno == EINTR)
                continue;
            return nt_supervisor_apc_fail_closed(sup, errno);
        }
        if (mono_ms() >= deadline)
            return nt_supervisor_apc_fail_closed(sup, ETIMEDOUT);
        const struct timespec pause = {.tv_nsec = 1000000L};
        (void)nanosleep(&pause, NULL);
    }
}

/* Backwards-compatible trusted fixture API: no cancellation source. */
int nt_supervisor_invoke_trusted_guest_apc(
    struct nt_thread_supervisor *sup,
    const struct nt_supervisor_guest_scope *scope,
    uintptr_t guest_entry, uintptr_t guest_stack_top, uintptr_t guest_argument,
    const _Atomic uint32_t *guest_completion, unsigned timeout_ms)
{
    return nt_supervisor_invoke_trusted_guest_apc_guarded(
        sup, scope, guest_entry, guest_stack_top, guest_argument,
        guest_completion, timeout_ms, NULL, -1);
}

/* Both normal release and error cleanup obey the same rule: never
 * discard the tracer while a live thread is still PTRACE_SEIZE'd.
 * A raw close(pidfd) does NOT detach the thread. */
static void nt_supervisor_reset_owned(struct nt_thread_supervisor *sup)
{
    if (sup->pidfd >= 0)
        (void)close(sup->pidfd);
    nt_supervisor_init(sup);
}
int nt_supervisor_terminate_compromised_child(struct nt_thread_supervisor *sup)
{
    if (!sup || !sup->seized || sup->pidfd < 0) {
        errno = EINVAL;
        return -1;
    }
    /* Do this BEFORE issuing the signal; even an error must not allow
     * the caller to resume regs whose rollback was never verified. */
    sup->compromised = true;
#if defined(SYS_pidfd_send_signal)
    return (int)syscall(SYS_pidfd_send_signal, sup->pidfd, SIGKILL, NULL, 0);
#else
    errno = ENOTSUP;
    return -1; /* Never substitute a recyclable raw Linux PID. */
#endif
}

int nt_supervisor_abort_child(struct nt_thread_supervisor *sup)
{
    if (!sup) { errno = EINVAL; return -1; }
    if (!sup->seized || sup->exited) {
        nt_supervisor_reset_owned(sup);
        return 0;
    }
    if (sup->compromised) {
        /* SIGKILL is already pending (or must be re-issued) to a PID
         * fixed by pidfd. PTRACE_DETACH would otherwise permit execution
         * of registers that we have NOT been able to restore.
         * Poll a kernel-owned pidfd, never infer exit from a PID value. */
        struct pollfd watcher = {.fd = sup->pidfd, .events = POLLIN};
        if (sup->pidfd >= 0 && poll(&watcher, 1, 0) == 1 &&
            (watcher.revents & POLLIN) != 0) {
            nt_supervisor_reset_owned(sup);
            return 0;
        }
        (void)nt_supervisor_terminate_compromised_child(sup);
        errno = EBUSY;
        return -1; /* Keep PTRACE_O_EXITKILL armed until tracer exits. */
    }
    if (!sup->stopped)
        (void)nt_supervisor_interrupt(sup, 500);
    if (sup->stopped && nt_supervisor_detach(sup) == 0) {
        nt_supervisor_reset_owned(sup);
        return 0;
    }
    /* If the stop state is unknown and detach cannot be verified,
     * terminate only the direct child whose identity is pinned by pidfd.
     * Never signal a raw potentially-recycled Linux TID/PID. */
#if defined(SYS_pidfd_send_signal)
    if (sup->pidfd >= 0)
        (void)syscall(SYS_pidfd_send_signal, sup->pidfd, SIGKILL, NULL, 0);
#endif
    nt_supervisor_reset_owned(sup);
    errno = EPROTO;
    return -1;
}

void nt_supervisor_release(struct nt_thread_supervisor *sup)
{
    if (sup)
        (void)nt_supervisor_abort_child(sup);
}
