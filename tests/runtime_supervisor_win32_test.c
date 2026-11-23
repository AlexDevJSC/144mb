/* SPDX-License-Identifier: MIT */
/* Exercise actual Win32 API exports in the real nt_runtime_run_fixture fork.
 * Host opt-in and trusted code range make this a controlled ABI fixture.
 * There is no claim that arbitrary PE programs may enable ptrace. */
#define _GNU_SOURCE
#include "compat.h"
#include "sync.h"
#include "ntnative/guest_boundary_runtime.h"
#include "nt_services.h"

#include <asm/prctl.h>
#include <sched.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

extern const char runtime_test_guest_begin[], runtime_test_guest_end[];
static _Atomic uint64_t ticks;
static _Atomic uint32_t entered, stop, host_phase, finish;
static _Thread_local _Alignas(64) unsigned char teb[4096];

static void msleep(unsigned ms)
{
    struct timespec delay = {.tv_sec = ms/1000,
        .tv_nsec = (ms%1000)*1000000L};
    nanosleep(&delay, NULL);
}

static uint32_t NT_MS_ABI test_worker(void *ignored)
{
    (void)ignored;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb) != 0 ||
        !nt_guest_boundary_thread_attach((uintptr_t)teb))
        return 51;
    nt_guest_boundary_guest_enter();
    atomic_store_explicit(&entered, 1, memory_order_release);
    __asm__ __volatile__(
        ".globl runtime_test_guest_begin\n"
        "runtime_test_guest_begin:\n"
        "movl %1, %%eax\n"
        "testl %%eax, %%eax\n"
        "jne 1f\n"
        "lock incq %0\n"
        "jmp runtime_test_guest_begin\n"
        "1:\n"
        ".globl runtime_test_guest_end\n"
        "runtime_test_guest_end:\n"
        : "+m"(ticks) : "m"(stop) : "eax", "memory");
    nt_guest_boundary_guest_leave();
    atomic_store_explicit(&host_phase, 1, memory_order_release);
    while (!atomic_load_explicit(&finish, memory_order_acquire))
        sched_yield();
    nt_guest_boundary_thread_detach();
    return 0;
}

static void NT_MS_ABI never_called_user_apc(uintptr_t data)
{
    (void)data;
    _Exit(69);
}
static void NT_MS_ABI never_called_native_apc(void *a, void *b, void *c)
{
    (void)a; (void)b; (void)c;
    _Exit(70);
}
static int NT_MS_ABI test_entry(void)
{
    /* The controlled supervisor child MUST make Linux reject even a
     * direct guest arch_prctl(SHSTK_ENABLE/DISABLE/LOCK/UNLOCK), without
     * relying on CET hardware or a cooperative runtime API wrapper. */
    for (unsigned cet = 0x5001u; cet <= 0x5004u; ++cet) {
        errno = 0;
        if (syscall(SYS_arch_prctl, cet, 0UL) != -1 || errno != EPERM)
            return 38;
    }
    errno = 0;
    if (syscall(SYS_arch_prctl, 0x5100u, 0UL) != -1 || errno != EPERM)
        return 39; /* unknown future CET extension is fail closed */
    unsigned long query = 0;
    errno = 0;
    if (syscall(SYS_arch_prctl, 0x5005u, &query) == -1 &&
        errno == EPERM)
        return 40; /* read-only CET status remains permitted */
    atomic_store(&ticks, 0);
    atomic_store(&entered, 0);
    atomic_store(&stop, 0);
    atomic_store(&host_phase, 0);
    atomic_store(&finish, 0);
    uint32_t tid = 0;
    uintptr_t thread = nt_CreateThread(NULL, 0, test_worker,
                                              NULL, 0, &tid);
    if (!thread || !tid) return 10;
    for (int i = 0; i < 3000 && !atomic_load(&entered); ++i)
        msleep(1);
    if (!atomic_load(&entered)) return 11;
    /* The public HANDLE rights gate must reject a limited-access alias
     * before asking the trusted parent to interrupt the target. */
    uintptr_t limited = nt_OpenThread(0x0008u, 0, tid); /* GET_CONTEXT only */
    if (!limited || nt_SuspendThread(limited) != UINT32_MAX ||
        !nt_CloseHandle(limited))
        return 28;
    /* Special APC delivery is not yet safe for arbitrary functions,
     * and must NOT be reported as a successful normal APC queue. */
    if (nt_QueueUserAPC2(never_called_user_apc, thread, 0, 1) != 0 ||
        nt_NtQueueApcThreadEx(thread, 1, never_called_native_apc,
                              NULL, NULL, NULL) != NT_STATUS_NOT_SUPPORTED)
        return 27;
    /* Actual PE import API surface: no direct control-plane call here. */
    if (nt_SuspendThread(thread) != 0) return 12;
    const uint64_t frozen = atomic_load(&ticks);
    msleep(20);
    if (atomic_load(&ticks) != frozen) return 13;
    uint32_t native_previous = UINT32_MAX;
    if (nt_NtSuspendThread(thread, &native_previous) != NT_STATUS_SUCCESS ||
        native_previous != 1)
        return 14;
    /* Regression: TerminateThread is ONLY for CREATE_SUSPENDED startup
     * gates. On a real kernel stop it must not reset the local count or
     * strand the traced worker. Both public API forms must reject it. */
    if (nt_TerminateThread(thread, 99) ||
        nt_NtTerminateThread(thread, (nt_status)99) != NT_STATUS_NOT_SUPPORTED)
        return 36;
    /* Close the original HANDLE with a real tracer holding two nested
     * stops. The duplicate must retain the canonical Windows object and
     * allow a counted final resume; closing must not free its TID state. */
    uintptr_t alias = 0;
    if (nt_runtime_duplicate_waitable(thread, 0, 0, 2u, &alias) !=
            NT_STATUS_SUCCESS || alias == 0 || !nt_CloseHandle(thread))
        return 37;
    thread = alias;
    _Alignas(16) struct nt_context64 context = {0};
    context.flags = 0x100003u;
    if (!nt_GetThreadContext(thread, &context) ||
        context.rip < (uintptr_t)runtime_test_guest_begin ||
        context.rip >= (uintptr_t)runtime_test_guest_end ||
        context.rsp == 0)
        return 15;
    _Alignas(16) struct nt_context64 native_context = {0};
    native_context.flags = 0x100003u;
    if (nt_NtGetContextThread(thread, &native_context) != NT_STATUS_SUCCESS ||
        native_context.rip != context.rip || native_context.rsp != context.rsp)
        return 31;
    _Alignas(16) struct nt_context64 integer = context;
    integer.flags = 0x100002u;
    const uint64_t rax = integer.rax;
    integer.rax = UINT64_C(0x8877665511223344);
    if (!nt_SetThreadContext(thread, &integer)) return 16;
    _Alignas(16) struct nt_context64 readback = {0};
    readback.flags = 0x100002u;
    if (!nt_GetThreadContext(thread, &readback) ||
        readback.rax != integer.rax) return 17;
    integer.rax = rax;
    if (nt_NtSetContextThread(thread, &integer) != NT_STATUS_SUCCESS)
        return 32;
    if (!nt_SetThreadContext(thread, &integer)) return 18;
    integer.flags = 0x100003u;
    if (nt_SetThreadContext(thread, &integer)) return 19;
    /* The NTSTATUS forms must also refuse CONTROL, FP/XSTATE and
     * changes to the instruction pointer; they must never claim a
     * successful register write for unsupported groups. */
    if (nt_NtSetContextThread(thread, &integer) != NT_STATUS_NOT_SUPPORTED)
        return 29;
    /* Verify an actual FXSAVE image from Linux NT_PRFPREG, not a fabricated
     * success or a copy of general registers. Windows CONTEXT.FltSave
     * contains the 512-byte x87/XMM0-15 image and MXCSR at byte 24. */
    _Alignas(16) struct nt_context64 fp = {0};
    fp.flags = 0x100008u;
    if (nt_NtGetContextThread(thread, &fp) != NT_STATUS_SUCCESS ||
        fp.mxcsr == 0)
        return 30;
    uint32_t saved_mxcsr = 0;
    memcpy(&saved_mxcsr, fp.floating + 24, sizeof(saved_mxcsr));
    if (saved_mxcsr != fp.mxcsr)
        return 33;
    /* The guest is ptrace-stopped with a parent-approved grant. Modify
     * ONLY the MXCSR rounding-mode bits in the kernel FP regset, read
     * them back from the same TID, then restore before resuming. */
    _Alignas(16) struct nt_context64 changed_fp = fp;
    changed_fp.flags = 0x100008u;
    changed_fp.mxcsr ^= UINT32_C(0x2000);
    memcpy(changed_fp.floating + 24, &changed_fp.mxcsr, sizeof(changed_fp.mxcsr));
    if (nt_NtSetContextThread(thread, &changed_fp) != NT_STATUS_SUCCESS)
        return 34;
    _Alignas(16) struct nt_context64 verified_fp = {0};
    verified_fp.flags = 0x100008u;
    if (nt_NtGetContextThread(thread, &verified_fp) != NT_STATUS_SUCCESS ||
        verified_fp.mxcsr != changed_fp.mxcsr)
        return 41;
    fp.flags = 0x100008u;
    if (nt_NtSetContextThread(thread, &fp) != NT_STATUS_SUCCESS)
        return 42;
    verified_fp.flags = 0x100008u;
    if (nt_NtGetContextThread(thread, &verified_fp) != NT_STATUS_SUCCESS ||
        verified_fp.mxcsr != fp.mxcsr)
        return 43;
    changed_fp.mxcsr ^= UINT32_C(0x2000); /* image now inconsistent */
    if (nt_NtSetContextThread(thread, &changed_fp) != NT_STATUS_INVALID_PARAMETER)
        return 44;
    /* Combined INTEGER + legacy FXSAVE transaction: both kernel
     * regsets must change together and the parent must read both back
     * before acknowledging success. Restore the original RAX/MXCSR
     * while the exact guest child remains ptrace-stopped. */
    _Alignas(16) struct nt_context64 combined = fp;
    combined.flags = 0x10000au;
    combined.rax = rax ^ UINT64_C(0x1223344556677889);
    combined.mxcsr ^= UINT32_C(0x2000);
    memcpy(combined.floating + 24, &combined.mxcsr, sizeof(combined.mxcsr));
    if (nt_NtSetContextThread(thread, &combined) != NT_STATUS_SUCCESS)
        return 51;
    _Alignas(16) struct nt_context64 observed_combined = {0};
    observed_combined.flags = 0x10000au;
    if (nt_NtGetContextThread(thread, &observed_combined) != NT_STATUS_SUCCESS ||
        observed_combined.rax != combined.rax ||
        observed_combined.mxcsr != combined.mxcsr ||
        memcmp(observed_combined.floating, combined.floating, 512) != 0)
        return 52;
    combined.rax = rax;
    combined.mxcsr = fp.mxcsr;
    memcpy(combined.floating, fp.floating, 512);
    if (nt_NtSetContextThread(thread, &combined) != NT_STATUS_SUCCESS)
        return 53;
    observed_combined.flags = 0x10000au;
    if (nt_NtGetContextThread(thread, &observed_combined) != NT_STATUS_SUCCESS ||
        observed_combined.rax != rax ||
        observed_combined.mxcsr != fp.mxcsr ||
        memcmp(observed_combined.floating, fp.floating, 512) != 0)
        return 54;
    /* Never silently interpret CONTROL, SEGMENTS or DEBUG as a mixed
     * two-regset transaction. */
    combined.flags = 0x10000bu; /* CONTROL|INTEGER|FP */
    if (nt_NtSetContextThread(thread, &combined) != NT_STATUS_NOT_SUPPORTED)
        return 55;
    combined.flags = 0x10001au; /* DEBUG|INTEGER|FP */
    if (nt_NtSetContextThread(thread, &combined) != NT_STATUS_NOT_SUPPORTED)
        return 56;
    /* Real CONTROL subset: reversible CPU carry flag without changing
     * RIP/RSP/RBP, selectors, IF or TF of this suspended owned guest.
     * Read the actual kernel PRSTATUS after each authenticated RPC. */
    _Alignas(16) struct nt_context64 control = {0};
    control.flags = 0x100001u;
    if (nt_NtGetContextThread(thread, &control) != NT_STATUS_SUCCESS)
        return 57;
    _Alignas(16) struct nt_context64 changed_control = control;
    changed_control.eflags ^= 1u;
    if (nt_NtSetContextThread(thread, &changed_control) != NT_STATUS_SUCCESS)
        return 58;
    _Alignas(16) struct nt_context64 checked_control = {0};
    checked_control.flags = 0x100001u;
    if (nt_NtGetContextThread(thread, &checked_control) != NT_STATUS_SUCCESS ||
        checked_control.eflags != changed_control.eflags ||
        checked_control.rip != control.rip ||
        checked_control.rsp != control.rsp)
        return 59;
    if (nt_NtSetContextThread(thread, &control) != NT_STATUS_SUCCESS)
        return 60;
    checked_control.flags = 0x100001u;
    if (nt_NtGetContextThread(thread, &checked_control) != NT_STATUS_SUCCESS ||
        checked_control.eflags != control.eflags)
        return 61;
    changed_control = control;
    changed_control.rip += 1u; /* arbitrary PC rewrite is NOT trusted */
    if (nt_NtSetContextThread(thread, &changed_control) != NT_STATUS_NOT_SUPPORTED)
        return 62;
    changed_control = control;
    changed_control.rsp -= 16u; /* no authenticated stack replacement */
    if (nt_NtSetContextThread(thread, &changed_control) != NT_STATUS_NOT_SUPPORTED)
        return 63;
    changed_control = control;
    changed_control.eflags ^= 0x200u; /* IF is not guest-controlled */
    if (nt_NtSetContextThread(thread, &changed_control) != NT_STATUS_NOT_SUPPORTED)
        return 64;

    _Alignas(16) struct nt_context64 invalid = {0};
    invalid.flags = 0x100010u; /* debug registers not yet supported */
    if (nt_NtGetContextThread(thread, &invalid) != NT_STATUS_INVALID_PARAMETER)
        return 35;
    native_previous = UINT32_MAX;
    if (nt_NtResumeThread(thread, &native_previous) != NT_STATUS_SUCCESS ||
        native_previous != 2)
        return 20;
    msleep(20);
    if (atomic_load(&ticks) != frozen) return 21;
    if (nt_ResumeThread(thread) != 1) return 22;
    for (int i=0; i<3000 && atomic_load(&ticks) == frozen; ++i)
        msleep(1);
    if (atomic_load(&ticks) == frozen) return 23;
    atomic_store_explicit(&stop, 1, memory_order_release);
    for (int i=0; i<3000 && !atomic_load(&host_phase); ++i)
        msleep(1);
    if (!atomic_load(&host_phase)) return 24;
    if (nt_SuspendThread(thread) != UINT32_MAX) return 25;
    atomic_store_explicit(&finish, 1, memory_order_release);
    if (nt_WaitForSingleObject(thread, 3000) != 0 ||
        !nt_CloseHandle(thread))
        return 26;
    return 0;
}

int main(void)
{
    if (!nt_guest_boundary_set_image_range((uintptr_t)runtime_test_guest_begin,
                                            (uintptr_t)runtime_test_guest_end))
        return 1;
    nt_runtime_enable_supervisor_thread_control();
    for (int i = 0; i < 2; ++i) {
        uint32_t exit_code = UINT32_MAX;
        if (!nt_runtime_run_fixture(test_entry, &exit_code) ||
            exit_code != 0) {
            fprintf(stderr, "FAIL actual Win32 supervisor APIs: session=%d code=%u\n",
                    i, exit_code);
            return 2;
        }
    }
    puts("PASS real runtime Win32 entrypoints: SuspendThread nested counts, "
         "Get/SetThreadContext AMD64 integer and legacy FP kernel readback, "
         "ResumeThread, HANDLE rights, NTSTATUS API parity, HOST fail-close, "
         "authenticated direct-parent IPC, two sessions");
    return 0;
}
