/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "guest_async_apc.h"
#include "guest_xstate_policy.h"
#include "compat.h"
#include "ntnative/guest_boundary_runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <ucontext.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__) || !defined(REG_RIP)
#error Guest special APC kernel signal dispatcher requires Linux AMD64
#endif

enum { APC_STACK_BYTES = 131072, APC_XSAVE_MAX = 16384 };
static pthread_once_t install_once = PTHREAD_ONCE_INIT;
static int apc_signal_number;
static bool installed;
static size_t page_size;

/* No PE-provided address, FD, kernel TID or signal number enters this
 * interface: the Win32 runtime only signals a previously created and
 * validated pthread owned by the same guest process. Signals carry NO
 * callback addresses or guest-provided arguments.
 *
 * A hardware exception can interrupt the callback on the private APC
 * stack. It uses its own independent per-thread exception stack; this
 * saved original guest context is not overwritten by that fault. */
struct async_apc_slot {
    volatile sig_atomic_t armed, pending;
    void *mapping;
    size_t mapping_bytes;
    uintptr_t stack_low;
    size_t stack_bytes;
    greg_t gregs[NGREG];
    _Alignas(64) unsigned char xstate[APC_XSAVE_MAX];
    bool has_xstate;
    size_t xstate_bytes;
};
static _Thread_local struct async_apc_slot slot;

extern bool nt_runtime_special_apc_pending_signal(void);
extern void nt_runtime_special_apc_checkpoint(void);
extern _Noreturn void nt_restore_software_context(const struct nt_context64 *);
extern void nt_runtime_guest_hardware_fault_restore_extended(void);

/* Only the SIMD components may be safely restored from a temporary host
 * stack. XRSTOR of PKRU/AMX/MPX is not permitted here, even if the kernel
 * captured it; this would change the host's access permissions/state. */
static uint32_t word32(const volatile unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool capture_signal_context(const ucontext_t *uc)
{
    const volatile unsigned char *fp =
        (const volatile unsigned char *)uc->uc_mcontext.fpregs;
    if (!fp)
        return false;
    size_t count = 512;
    bool extended = false;
    if (word32(fp + 464) == UINT32_C(0x46505853)) {
        count = word32(fp + 480);
        if (count < 576 || count > APC_XSAVE_MAX)
            return false;
        uint64_t features = 0, layout = 0;
        for (unsigned i = 0; i < 8; ++i) {
            features |= (uint64_t)fp[512 + i] << (8 * i);
            layout |= (uint64_t)fp[520 + i] << (8 * i);
        }
        const uint64_t allowed =
            UINT64_C(0x3) | (UINT64_C(1) << 2) |
            (UINT64_C(1) << 5) | (UINT64_C(1) << 6) |
            (UINT64_C(1) << 7) | (UINT64_C(1) << 9);
        if ((features & ~allowed) != 0 || layout != 0 ||
            ((features & (UINT64_C(1) << 9)) &&
             !nt_guest_xstate_pkru_matches(fp, count)))
            return false;
        extended = true;
    }
    for (size_t i = 0; i < count; ++i)
        slot.xstate[i] = fp[i];
    for (size_t i = 0; i < NGREG; ++i)
        slot.gregs[i] = uc->uc_mcontext.gregs[i];
    slot.has_xstate = extended;
    slot.xstate_bytes = count;
    return true;
}

static _Noreturn void restore_guest_context(const struct nt_context64 *context)
{
    if (slot.has_xstate) {
        /* Guest callbacks are not allowed to change PKRU while the
         * original PE context is suspended on an isolated host stack. */
        const volatile unsigned char *fp = slot.xstate;
        uint64_t features = 0;
        for (unsigned j = 0; j < 8; ++j)
            features |= (uint64_t)fp[512 + j] << (8 * j);
        if ((features & (UINT64_C(1) << 9)) &&
            !nt_guest_xstate_pkru_matches(fp, slot.xstate_bytes))
            _Exit(174);
        uint32_t low = 0, high = 0;
        __asm__ __volatile__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
        const uint32_t mask = low &
            ((1u << 2) | (1u << 5) | (1u << 6) | (1u << 7));
        (void)high;
        if (mask)
            __asm__ __volatile__("xrstor64 (%0)" : :
                "r"(slot.xstate), "a"(mask), "d"(0u) : "memory");
    }
    slot.pending = 0;
    nt_restore_software_context(context);
}

static _Noreturn void dispatch_special_apc(void)
{
    if (!slot.pending || !slot.armed)
        _Exit(171);
    /* The Linux kernel has already returned from its signal handler.
     * This is ordinary runtime code with a private stack, not a signal
     * callback. Win64 APCs may call imports, allocate, queue further APCs
     * or enter the existing VEH/SEH machinery. */
    _Alignas(16) struct nt_context64 original = {0};
    const greg_t *g = slot.gregs;
    original.flags = UINT32_C(0x10000f);
    original.rax = (uint64_t)g[REG_RAX];
    original.rcx = (uint64_t)g[REG_RCX];
    original.rdx = (uint64_t)g[REG_RDX];
    original.rbx = (uint64_t)g[REG_RBX];
    original.rsp = (uint64_t)g[REG_RSP];
    original.rbp = (uint64_t)g[REG_RBP];
    original.rsi = (uint64_t)g[REG_RSI];
    original.rdi = (uint64_t)g[REG_RDI];
    original.r8 = (uint64_t)g[REG_R8];
    original.r9 = (uint64_t)g[REG_R9];
    original.r10 = (uint64_t)g[REG_R10];
    original.r11 = (uint64_t)g[REG_R11];
    original.r12 = (uint64_t)g[REG_R12];
    original.r13 = (uint64_t)g[REG_R13];
    original.r14 = (uint64_t)g[REG_R14];
    original.r15 = (uint64_t)g[REG_R15];
    original.rip = (uint64_t)g[REG_RIP];
    original.eflags = (uint32_t)g[REG_EFL];
    original.cs = (uint16_t)g[REG_CSGSFS];
    original.gs = (uint16_t)((uint64_t)g[REG_CSGSFS] >> 16);
    original.fs = (uint16_t)((uint64_t)g[REG_CSGSFS] >> 32);
    memcpy(original.floating, slot.xstate, 512);
    memcpy(&original.mxcsr, original.floating + 24, sizeof(original.mxcsr));

    nt_runtime_special_apc_checkpoint();
    /* Never return with a fabricated ELF frame on the original guest
     * stack. The final assembly restores GPR, EFLAGS, FXSAVE and RSP/RIP.
     * If SEH cannot recover a guest frame, its existing fail-closed
     * dispatcher terminates the guest instead of releasing host locks. */
    restore_guest_context(&original);
}

static void async_apc_signal_handler(int sig, siginfo_t *info, void *frame)
{
    if (sig != apc_signal_number || !frame || !info ||
        info->si_code != SI_TKILL || info->si_pid != getpid() ||
        !slot.armed || slot.pending)
        return;
    /* An APC queued while the target is inside a host import is already
     * visible to the existing import-return checkpoint; it may NEVER
     * hijack that ELF stack or a pending nonlocal SEH transfer. */
    if (!nt_guest_boundary_signal_guest_phase() ||
        !nt_runtime_special_apc_pending_signal())
        return;
    ucontext_t *uc = frame;
    const uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    if (!nt_guest_boundary_signal_guest_code(pc) ||
        (((unsigned long)uc->uc_mcontext.gregs[REG_EFL]) & 0x100u) != 0)
        return; /* unknown image, host transition or single-step */
    if (!capture_signal_context(uc))
        _Exit(172); /* unsupported XSTATE: no ambiguous resume */
    slot.pending = 1;
    const uintptr_t top = (slot.stack_low + slot.stack_bytes) & ~(uintptr_t)15;
    ((uintptr_t *)top)[-1] = 0;
    uc->uc_mcontext.gregs[REG_RSP] = (greg_t)(top - 8);
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)dispatch_special_apc;
}

static void install_once_fn(void)
{
    const int target = SIGRTMIN + 4;
    if (target > SIGRTMAX)
        return;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0 || page > 65536)
        return;
    struct sigaction old = {0};
    if (sigaction(target, NULL, &old) != 0 ||
        old.sa_handler != SIG_DFL)
        return; /* never steal a signal already owned by another host */
    struct sigaction act = {0};
    act.sa_sigaction = async_apc_signal_handler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&act.sa_mask);
    if (sigaction(target, &act, NULL) != 0)
        return;
    page_size = (size_t)page;
    apc_signal_number = target;
    installed = true;
}

bool nt_runtime_guest_async_apc_install(void)
{
    return pthread_once(&install_once, install_once_fn) == 0 && installed;
}

bool nt_runtime_guest_async_apc_thread_attach(void)
{
    if (!installed || slot.armed)
        return false;
    const size_t total = APC_STACK_BYTES + page_size;
    void *memory = mmap(NULL, total, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
        return false;
    if (mprotect(memory, page_size, PROT_NONE) != 0) {
        (void)munmap(memory, total);
        return false;
    }
    slot.mapping = memory;
    slot.mapping_bytes = total;
    slot.stack_low = (uintptr_t)memory + page_size;
    slot.stack_bytes = APC_STACK_BYTES;
    slot.pending = 0;
    slot.armed = 1;
    return true;
}

void nt_runtime_guest_async_apc_thread_detach(void)
{
    if (!slot.armed)
        return;
    slot.armed = 0;
    /* Never free the stack on which the live APC callback or its VEH
     * may still be running: doing so would corrupt unrelated host code. */
    if (slot.pending)
        _Exit(173);
    (void)munmap(slot.mapping, slot.mapping_bytes);
    slot.mapping = NULL;
    slot.stack_low = 0;
    slot.stack_bytes = 0;
}

bool nt_runtime_guest_async_apc_nudge(pthread_t target)
{
    return installed && pthread_kill(target, apc_signal_number) == 0;
}

bool nt_runtime_guest_async_apc_stack_for_current(uintptr_t *low, size_t *size)
{
    const uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
    if (!low || !size || !slot.pending ||
        frame < slot.stack_low ||
        frame >= slot.stack_low + slot.stack_bytes)
        return false;
    *low = slot.stack_low;
    *size = slot.stack_bytes;
    return true;
}
