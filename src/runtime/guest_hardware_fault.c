/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "guest_hardware_fault.h"
#include "guest_xstate_policy.h"
#include "compat.h"
#include "ntnative/guest_boundary_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__x86_64__) || !defined(REG_RIP) || !defined(REG_ERR)
#error The hardware exception bridge requires Linux AMD64 ucontext
#endif

enum { SIGNAL_STACK_BYTES = 65536, DISPATCH_STACK_BYTES = 131072,
       XSAVE_MAX_BYTES = 16384, FAULT_SIGNAL_COUNT = 5 };
static const int fault_signals[FAULT_SIGNAL_COUNT] = {
    SIGSEGV, SIGILL, SIGFPE, SIGBUS, SIGTRAP
};
static struct sigaction previous_actions[FAULT_SIGNAL_COUNT];
static pthread_once_t install_once = PTHREAD_ONCE_INIT;
static bool installed;

/* The only code executed IN a Linux signal handler copies registers,
 * siginfo and the kernel-owned XSAVE frame into TLS, then modifies the
 * ucontext to return onto a dedicated preallocated host dispatch stack.
 * No malloc, pthread lock, printf, PE callback or VEH is called by a
 * signal handler. The target must already have passed the guest-only
 * boundary check; host or user-generated signals fail closed. */
struct pending_guest_fault {
    volatile sig_atomic_t armed, pending;
    int signal, code;
    uintptr_t address;
    unsigned long error;
    greg_t gregs[NGREG];
    _Alignas(64) unsigned char xstate[XSAVE_MAX_BYTES];
    size_t xstate_size;
    bool has_xsave;
    void *mapping;
    stack_t previous_altstack;
};
static _Thread_local struct pending_guest_fault pending;

extern _Noreturn void nt_dispatch_hardware_exception(
    struct nt_context64 *, uint32_t, uint32_t, const uintptr_t *);

static uint32_t little32(const volatile unsigned char *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool nt_guest_hardware_fault_translate(int signal, int code, unsigned long err,
                                       uintptr_t address, uint32_t *nt_code,
                                       uint32_t *argument_count,
                                       uintptr_t arguments[2])
{
    if (code <= 0 || !nt_code || !argument_count || !arguments)
        return false; /* SI_USER / SI_TKILL cannot forge a CPU exception */
    *argument_count = 0;
    arguments[0] = arguments[1] = 0;
    switch (signal) {
    case SIGSEGV:
        if (code != SEGV_MAPERR && code != SEGV_ACCERR)
            return false;
        *nt_code = UINT32_C(0xc0000005);
        *argument_count = 2;
        arguments[0] = (err & 16u) ? 8u : (err & 2u) ? 1u : 0u;
        arguments[1] = address;
        return true;
    case SIGILL:
        if (code == ILL_PRVOPC || code == ILL_PRVREG)
            *nt_code = UINT32_C(0xc0000096);
        else if (code == ILL_ILLOPC || code == ILL_ILLOPN ||
                 code == ILL_ILLADR || code == ILL_ILLTRP)
            *nt_code = UINT32_C(0xc000001d);
        else
            return false;
        return true;
    case SIGFPE:
        switch (code) {
        case FPE_INTDIV: *nt_code = UINT32_C(0xc0000094); break;
        case FPE_INTOVF: *nt_code = UINT32_C(0xc0000095); break;
        case FPE_FLTDIV: *nt_code = UINT32_C(0xc000008e); break;
        case FPE_FLTOVF: *nt_code = UINT32_C(0xc0000091); break;
        case FPE_FLTUND: *nt_code = UINT32_C(0xc0000093); break;
        case FPE_FLTRES: *nt_code = UINT32_C(0xc000008f); break;
        case FPE_FLTINV: *nt_code = UINT32_C(0xc0000090); break;
        case FPE_FLTSUB: *nt_code = UINT32_C(0xc000008d); break;
        default: return false;
        }
        return true;
    case SIGBUS:
        if (code == BUS_ADRALN)
            *nt_code = UINT32_C(0x80000002);
        else if (code == BUS_ADRERR)
            *nt_code = UINT32_C(0xc0000005);
        else
            return false;
        /* Linux SIGBUS does not reliably provide x86 PF access kind. */
        return true;
    case SIGTRAP:
        if (code == TRAP_BRKPT)
            *nt_code = UINT32_C(0x80000003);
        else if (code == TRAP_TRACE)
            *nt_code = UINT32_C(0x80000004);
        else
            return false;
        return true;
    default:
        return false;
    }
}

/* Diagnostic markers are emitted only by the isolated regression
 * build. Signal handlers must never call stdio, backtrace or malloc. */
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
static _Thread_local volatile sig_atomic_t denied_at;
#endif
static _Noreturn void fail_closed(int signal)
{
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
    char message[] = "hardware signal denied at gate 0\\n";
    message[31] = (char)('0' + denied_at);
    const ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
#endif
    /* _Exit is async-signal-safe. Do not reinstall handlers, detach a
     * ptrace target or retry the instruction with ambiguous registers. */
    _Exit(128 + signal);
}

static _Noreturn void dispatch_guest_fault(void)
{
    struct pending_guest_fault *slot = &pending;
    _Alignas(16) struct nt_context64 context = {0};
    const greg_t *g = slot->gregs;
    context.flags = 0x10000fu; /* AMD64 CONTROL|INTEGER|SEGMENTS|FLOAT */
    context.rax = (uint64_t)g[REG_RAX];
    context.rcx = (uint64_t)g[REG_RCX];
    context.rdx = (uint64_t)g[REG_RDX];
    context.rbx = (uint64_t)g[REG_RBX];
    context.rsp = (uint64_t)g[REG_RSP];
    context.rbp = (uint64_t)g[REG_RBP];
    context.rsi = (uint64_t)g[REG_RSI];
    context.rdi = (uint64_t)g[REG_RDI];
    context.r8 = (uint64_t)g[REG_R8];
    context.r9 = (uint64_t)g[REG_R9];
    context.r10 = (uint64_t)g[REG_R10];
    context.r11 = (uint64_t)g[REG_R11];
    context.r12 = (uint64_t)g[REG_R12];
    context.r13 = (uint64_t)g[REG_R13];
    context.r14 = (uint64_t)g[REG_R14];
    context.r15 = (uint64_t)g[REG_R15];
    context.rip = (uint64_t)g[REG_RIP];
    context.eflags = (uint32_t)g[REG_EFL];
    context.cs = (uint16_t)g[REG_CSGSFS];
    context.gs = (uint16_t)((uint64_t)g[REG_CSGSFS] >> 16);
    context.fs = (uint16_t)((uint64_t)g[REG_CSGSFS] >> 32);
    memcpy(context.floating, slot->xstate, 512);
    memcpy(&context.mxcsr, context.floating + 24, sizeof(context.mxcsr));
    uint32_t nt_code = 0, count = 0;
    uintptr_t arguments[2] = {0};
    if (!nt_guest_hardware_fault_translate(slot->signal, slot->code,
            slot->error, slot->address, &nt_code, &count, arguments))
        fail_closed(slot->signal);
    nt_dispatch_hardware_exception(&context, nt_code, count, arguments);
}

static void guest_fault_signal(int sig, siginfo_t *information, void *frame)
{
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
    denied_at = 1;
#endif
    if (!information || !frame || !pending.armed || pending.pending ||
        !nt_guest_boundary_signal_guest_phase())
        fail_closed(sig);
    ucontext_t *ucontext = frame;
    const uintptr_t pc = (uintptr_t)ucontext->uc_mcontext.gregs[REG_RIP];
    uintptr_t start = 0, end = 0;
    /* The image grants are installed and immutable before any guest
     * instruction executes. Reject a host ELF PC even if the phase was
     * corrupted; this guard NEVER trusts an arbitrary guest-provided FD. */
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
    denied_at = 2;
#endif
    if (!nt_guest_boundary_get_image_range(&start, &end) ||
        pc < start || pc >= end || information->si_code <= 0 ||
        !ucontext->uc_mcontext.fpregs)
        fail_closed(sig);
    uint32_t code = 0, count = 0;
    uintptr_t arguments[2] = {0};
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
    denied_at = 3;
#endif
    if (!nt_guest_hardware_fault_translate(sig, information->si_code,
           (unsigned long)ucontext->uc_mcontext.gregs[REG_ERR],
           (uintptr_t)information->si_addr, &code, &count, arguments))
        fail_closed(sig);

    struct pending_guest_fault *slot = &pending;
    const volatile unsigned char *fp =
        (const volatile unsigned char *)ucontext->uc_mcontext.fpregs;
    size_t xstate_size = 512;
    bool has_xsave = false;
    /* Linux _fpx_sw_bytes uses the kernel-defined FP_XSTATE_MAGIC1 at
     * legacy FXSAVE offset 464; bytes 480..483 are xstate_size.
     * Copy *all* enabled XSTATE, not merely XMM; before the final
     * NT CONTEXT restore, XRSTOR restores guest YMM/AVX registers. */
    if (little32(fp + 464) == UINT32_C(0x46505853)) {
        const uint32_t bytes = little32(fp + 480);
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
        denied_at = 4;
#endif
        if (bytes < 576 || bytes > XSAVE_MAX_BYTES)
            fail_closed(sig);
        /* Linux's XSAVE header starts immediately after the 512-byte
         * FXSAVE legacy area. XCOMP_BV bit63 means a compacted layout,
         * which XRSTOR (as opposed to XRSTORS) cannot consume. An
         * active PKRU, AMX, MPX or unknown feature must never be restored
         * on a host dispatch stack with different memory permissions. */
        uint64_t active = 0, compacted = 0;
        for (unsigned j = 0; j < 8; ++j) {
            active |= (uint64_t)fp[512 + j] << (8 * j);
            compacted |= (uint64_t)fp[520 + j] << (8 * j);
        }
        const uint64_t supported =
            UINT64_C(0x3) | UINT64_C(1) << 2 |
            UINT64_C(1) << 5 | UINT64_C(1) << 6 |
            UINT64_C(1) << 7 | UINT64_C(1) << 9;
        /* PKRU is an OS-managed XSTATE feature: permit it only when
         * the saved kernel value exactly matches the running thread.
         * NEVER restore PKRU on a host dispatcher stack. */
#ifdef NT_HARDWARE_SIGNAL_TEST_DIAGNOSTICS
        denied_at = 5;
#endif
        if ((active & ~supported) ||
            (compacted & UINT64_C(0x8000000000000000)) ||
            ((active & (UINT64_C(1) << 9)) &&
             !nt_guest_xstate_pkru_matches(fp, bytes)))
            fail_closed(sig);
        xstate_size = bytes;
        has_xsave = true;
    }
    for (size_t i = 0; i < xstate_size; ++i)
        slot->xstate[i] = fp[i];
    for (size_t i = 0; i < NGREG; ++i)
        slot->gregs[i] = ucontext->uc_mcontext.gregs[i];
    slot->signal = sig;
    slot->code = information->si_code;
    slot->error = (unsigned long)slot->gregs[REG_ERR];
    slot->address = (uintptr_t)information->si_addr;
    slot->xstate_size = xstate_size;
    slot->has_xsave = has_xsave;
    slot->pending = 1;
    /* Return from rt_sigreturn on a separate host-only dispatch stack,
     * NEVER call non-async-safe VEH/SEH code on the Linux signal stack. */
    const uintptr_t top = (uintptr_t)slot->mapping +
                          SIGNAL_STACK_BYTES + DISPATCH_STACK_BYTES;
    const uintptr_t aligned = top & ~(uintptr_t)15;
    ((uintptr_t *)aligned)[-1] = 0; /* synthetic SysV return slot */
    ucontext->uc_mcontext.gregs[REG_RSP] = (greg_t)(aligned - 8);
    ucontext->uc_mcontext.gregs[REG_RIP] =
        (greg_t)(uintptr_t)dispatch_guest_fault;
}

static void install_handlers_once(void)
{
    struct sigaction action = {0};
    action.sa_sigaction = guest_fault_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (unsigned i = 0; i < FAULT_SIGNAL_COUNT; ++i) {
        if (sigaction(fault_signals[i], &action, &previous_actions[i]) != 0) {
            while (i != 0) {
                --i;
                (void)sigaction(fault_signals[i], &previous_actions[i], NULL);
            }
            return;
        }
    }
    installed = true;
}

bool nt_runtime_guest_hardware_fault_install(void)
{
    return pthread_once(&install_once, install_handlers_once) == 0 && installed;
}

bool nt_runtime_guest_hardware_fault_thread_attach(void)
{
    if (!installed)
        return true; /* ordinary tests without registered PE unwind */
    if (pending.armed)
        return false;
    void *mapping = mmap(NULL, SIGNAL_STACK_BYTES + DISPATCH_STACK_BYTES,
                         PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED)
        return false;
    stack_t previous = {0};
    if (sigaltstack(NULL, &previous) != 0 ||
        (previous.ss_flags & SS_ONSTACK)) {
        (void)munmap(mapping, SIGNAL_STACK_BYTES + DISPATCH_STACK_BYTES);
        return false;
    }
    stack_t alternate = {.ss_sp = mapping, .ss_size = SIGNAL_STACK_BYTES};
    if (sigaltstack(&alternate, NULL) != 0) {
        (void)munmap(mapping, SIGNAL_STACK_BYTES + DISPATCH_STACK_BYTES);
        return false;
    }
    pending.mapping = mapping;
    pending.previous_altstack = previous;
    pending.pending = 0;
    pending.armed = 1;
    return true;
}

void nt_runtime_guest_hardware_fault_thread_detach(void)
{
    if (!pending.armed)
        return;
    pending.armed = 0;
    if (pending.pending)
        fail_closed(SIGSEGV); /* never release an active fault stack */
    if (sigaltstack(&pending.previous_altstack, NULL) != 0)
        fail_closed(SIGSEGV);
    (void)munmap(pending.mapping, SIGNAL_STACK_BYTES + DISPATCH_STACK_BYTES);
    pending.mapping = NULL;
}

/* This function is called from the existing native SEH resume path
 * AFTER all VEH/SEH callbacks and BEFORE its final assembly register
 * restoration. Restore the kernel-captured XSAVE state, including
 * extended YMM/ZMM components, then let nt_restore_software_context
 * restore the (possibly VEH-modified) Win64 FXSAVE and GPR values.
 * No user code may select an arbitrary XSAVE pointer or xstate mask. */
void nt_runtime_guest_hardware_fault_restore_extended(void)
{
    struct pending_guest_fault *slot = &pending;
    if (!slot->pending)
        return;
    if (slot->has_xsave) {
        /* A callback may execute WRPKRU. Resume is forbidden if it
         * changes guest/host memory-access rights. Only SIMD can be
         * restored on this temporary ELF dispatcher stack. */
        const volatile unsigned char *fp = slot->xstate;
        uint64_t features = 0;
        for (unsigned j = 0; j < 8; ++j)
            features |= (uint64_t)fp[512 + j] << (8 * j);
        if ((features & (UINT64_C(1) << 9)) &&
            !nt_guest_xstate_pkru_matches(fp, slot->xstate_size))
            fail_closed(SIGSEGV);
        uint32_t xcr0_low = 0, xcr0_high = 0;
        __asm__ __volatile__("xgetbv" : "=a"(xcr0_low),
                             "=d"(xcr0_high) : "c"(0));
        /* Never restore PKRU or other access-control features while
         * still running on the host SEH dispatch stack. FXRSTOR in the
         * final assembly gate restores x87 and XMM, while XRSTOR here
         * handles only upper YMM/ZMM/AVX512 registers. */
        const uint32_t simd_mask =
            (1u << 2) | (1u << 5) | (1u << 6) | (1u << 7);
        const uint32_t eax = xcr0_low & simd_mask;
        (void)xcr0_high;
        if (eax)
            __asm__ __volatile__("xrstor64 (%0)" : :
                                 "r"(slot->xstate), "a"(eax), "d"(0u) :
                                 "memory");
    }
    slot->pending = 0;
}
