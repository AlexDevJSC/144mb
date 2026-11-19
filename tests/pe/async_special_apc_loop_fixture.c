/* SPDX-License-Identifier: MIT */
/* A REAL PE32+ thread spins forever without Windows imports or alertable
 * waits. A special APC MUST run on that exact thread while its RIP
 * remains inside this import-free loop, not at a synthetic safe point.
 * Failure cannot be misreported as a normal QueueUserAPC delivery. */
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

struct async_target {
    volatile uint32_t in_guest_spin;
    volatile uint32_t special_calls;
    volatile uint32_t ordinary_calls;
    volatile uint32_t worker_tid, callback_tid;
};
static void CALLBACK ordinary_apc(ULONG_PTR arg)
{
    struct async_target *state = (struct async_target *)arg;
    __atomic_add_fetch(&state->ordinary_calls, 1u, __ATOMIC_SEQ_CST);
}
#ifdef NT_TEST_ASYNC_SEH
/* Implemented by a real .pdata/.xdata scope in PE AMD64 assembly. */
extern void CALLBACK special_exception_apc(ULONG_PTR arg);
#define SPECIAL_CALLBACK special_exception_apc
#else
static void CALLBACK special_apc(ULONG_PTR arg)
{
    struct async_target *state = (struct async_target *)arg;
    __atomic_store_n(&state->callback_tid, GetCurrentThreadId(),
                     __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&state->special_calls, 1u, __ATOMIC_SEQ_CST);
    /* Intentionally perturb the SIMD control state on the private APC
     * stack. Only restoration of the interrupted kernel FXSAVE image
     * may allow the PE worker's saved MXCSR to survive this callback.
     * Toggling a valid rounding bit does not change PKRU or host policy. */
    uint32_t mxcsr = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr));
    mxcsr ^= UINT32_C(0x2000);
    __asm__ __volatile__("ldmxcsr %0" : : "m"(mxcsr) : "memory");
}
#define SPECIAL_CALLBACK special_apc
#endif
/* A single import-free PE instruction region spanning the real
 * asynchronous kernel-signal delivery. Set caller-saved AND Win64
 * nonvolatile GPRs and an XMM register, then check the exact same
 * values before returning to C. Linux must restore the original
 * ucontext and FXSAVE; mere callback completion is NOT enough. */
static const unsigned char xmm_sentinel[16] __attribute__((aligned(16))) = {
    0x14, 0x41, 0x42, 0x43, 0x80, 0x88, 0x90, 0xa0,
    0xb1, 0xc2, 0xd3, 0xe4, 0xf5, 0x16, 0x27, 0x38
};

__attribute__((noinline))
static uint32_t spin_and_verify_registers(volatile uint32_t *special_calls)
{
    uint32_t registers_intact = 0;
    __asm__ __volatile__(
        "movabsq $0x1122334455667788, %%rbx\n\t"
        "movabsq $0x2233445566778899, %%r12\n\t"
        "movabsq $0x33445566778899aa, %%r13\n\t"
        "movabsq $0x445566778899aabb, %%r14\n\t"
        "movabsq $0x5566778899aabbcc, %%r15\n\t"
        "movdqu %[sentinel], %%xmm6\n\t"
        "movq $900000000, %%rcx\n\t"
        "1:\n\t"
        "cmpl $0, (%[calls])\n\t"
        "jne 2f\n\t"
        "pause\n\t"
        "decq %%rcx\n\t"
        "jnz 1b\n\t"
        "2:\n\t"
        "movabsq $0x1122334455667788, %%rax\n\t"
        "cmpq %%rax, %%rbx\n\t"
        "jne 3f\n\t"
        "movabsq $0x2233445566778899, %%rax\n\t"
        "cmpq %%rax, %%r12\n\t"
        "jne 3f\n\t"
        "movabsq $0x33445566778899aa, %%rax\n\t"
        "cmpq %%rax, %%r13\n\t"
        "jne 3f\n\t"
        "movabsq $0x445566778899aabb, %%rax\n\t"
        "cmpq %%rax, %%r14\n\t"
        "jne 3f\n\t"
        "movabsq $0x5566778899aabbcc, %%rax\n\t"
        "cmpq %%rax, %%r15\n\t"
        "jne 3f\n\t"
        "movdqu %[sentinel], %%xmm7\n\t"
        "pcmpeqb %%xmm7, %%xmm6\n\t"
        "pmovmskb %%xmm6, %%edx\n\t"
        "cmpl $65535, %%edx\n\t"
        "jne 3f\n\t"
        "movl $1, %[intact]\n\t"
        "3:\n\t"
        : [intact] "=m"(registers_intact)
        : [calls] "r"(special_calls), [sentinel] "m"(xmm_sentinel)
        : "rax", "rcx", "rdx", "rbx", "r12", "r13", "r14", "r15",
          "xmm6", "xmm7", "cc", "memory"
    );
    return registers_intact;
}

static DWORD WINAPI pure_pe_loop(void *arg)
{
    struct async_target *state = arg;
    __atomic_store_n(&state->worker_tid, GetCurrentThreadId(),
                     __ATOMIC_SEQ_CST);
    __atomic_store_n(&state->in_guest_spin, 1u, __ATOMIC_SEQ_CST);
    /* The actual loop is one assembly block: no Windows imports,
     * host callbacks, syscalls or C compiler register reloads inside it. */
    uint32_t mxcsr_before = 0, mxcsr_after = 0;
    __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr_before));
    const uint32_t gpr_xmm_restored =
        spin_and_verify_registers(&state->special_calls);
    __asm__ __volatile__("stmxcsr %0" : "=m"(mxcsr_after));
    if (gpr_xmm_restored != 1u || mxcsr_before != mxcsr_after)
        return 106; /* interrupted PC resumed with corrupted AMD64 state */
    if (__atomic_load_n(&state->special_calls, __ATOMIC_SEQ_CST) != 1u ||
        __atomic_load_n(&state->ordinary_calls, __ATOMIC_SEQ_CST) != 0u ||
        __atomic_load_n(&state->callback_tid, __ATOMIC_SEQ_CST) !=
            __atomic_load_n(&state->worker_tid, __ATOMIC_SEQ_CST))
        return 103;
    return 0x1acu;
}
int import_fixture_entry(void)
{
    typedef DWORD (WINAPI *queue2)(PAPCFUNC, HANDLE, ULONG_PTR, DWORD);
    union { FARPROC raw; queue2 api; } resolved = {
        .raw = GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                              "QueueUserAPC2")
    };
    if (!resolved.api)
        return 100;
    struct async_target state = {0};
    DWORD tid = 0, exit_code = 0;
    HANDLE thread = CreateThread(NULL, 0, pure_pe_loop, &state, 0, &tid);
    if (!thread || tid == 0)
        return 101;
    bool entered = false;
    for (uint64_t spin = 0; spin < UINT64_C(900000000); ++spin) {
        if (__atomic_load_n(&state.in_guest_spin, __ATOMIC_SEQ_CST)) {
            entered = true;
            break;
        }
        __asm__ __volatile__("pause" ::: "memory");
    }
    if (!entered)
        return 102;
    /* A query-only HANDLE must not confer THREAD_SET_CONTEXT.
     * A pointer into this PE's writable data is not a loader-pinned
     * executable callback. Reject both WITHOUT scheduling anything. */
    HANDLE query_only = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!query_only)
        return 107;
    SetLastError(0);
    if (resolved.api(SPECIAL_CALLBACK, query_only, (ULONG_PTR)&state, 1u) ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 108;
    if (!CloseHandle(query_only))
        return 109;
    union { uintptr_t raw; PAPCFUNC callback; } writable_callback = {
        .raw = (uintptr_t)&state
    };
    SetLastError(0);
    if (resolved.api(writable_callback.callback, thread,
                     (ULONG_PTR)&state, 1u) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 110;

    /* A normal APC must remain pending: no alertable wait ever occurs. */
    if (!QueueUserAPC(ordinary_apc, thread, (ULONG_PTR)&state) ||
        !resolved.api(SPECIAL_CALLBACK, thread, (ULONG_PTR)&state, 1u))
        return 104;
    if (WaitForSingleObject(thread, 12000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code))
        return 105;
    /* Preserve the exact observed CPU-context regression code. A caller
     * must not collapse a corrupted register state to generic timeout. */
    if (exit_code == 106u)
        return 106;
    if (exit_code != 0x1acu ||
        __atomic_load_n(&state.ordinary_calls, __ATOMIC_SEQ_CST) != 0u ||
        __atomic_load_n(&state.special_calls, __ATOMIC_SEQ_CST) != 1u ||
        __atomic_load_n(&state.callback_tid, __ATOMIC_SEQ_CST) != tid ||
        !CloseHandle(thread))
        return 105;
    return 0;
}
