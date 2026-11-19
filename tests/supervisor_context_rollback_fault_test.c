/* SPDX-License-Identifier: MIT */
/* Dedicated, real ptrace AMD64 test of post-write CONTEXT verification.
 * Each binary is separately compiled with a test-only fault macro.
 * Production builds NEVER define these macros; the guest cannot enable
 * this injection through environment variables or NT exports. */
#define _GNU_SOURCE
#include "nt_supervisor_nt_bridge.h"
#include "ntnative/guest_boundary_runtime.h"

#include <asm/prctl.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(NT_SUP_TEST_FORCE_GPR_VERIFY_MISMATCH) && \
    !defined(NT_SUP_TEST_FORCE_FP_VERIFY_MISMATCH)
#error "This test is only for forced verification failure"
#endif
#ifdef NT_SUP_TEST_FORCE_GPR_VERIFY_MISMATCH
_Atomic uint32_t nt_sup_test_verify_corrupted;
#endif
#ifdef NT_SUP_TEST_FORCE_FP_VERIFY_MISMATCH
_Atomic uint32_t nt_sup_test_fp_verify_corrupted;
#endif

struct fixture_shared {
    struct nt_guest_boundary_session boundary;
    _Atomic uint32_t ready, stop;
    _Atomic uint64_t ticks;
};
extern const char nt_sup_fault_loop_begin[], nt_sup_fault_loop_end[];
static _Thread_local _Alignas(64) unsigned char teb[4096];

static void pause_ms(unsigned milliseconds)
{
    struct timespec wait = {.tv_sec=milliseconds/1000,
        .tv_nsec=(long)(milliseconds%1000)*1000000};
    while (nanosleep(&wait, &wait) != 0 && errno == EINTR) {}
}

static _Noreturn void child_code(struct fixture_shared *data)
{
    nt_guest_boundary_install_child_session(&data->boundary);
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, teb) != 0 ||
        !nt_guest_boundary_thread_attach((uintptr_t)teb))
        _exit(51);
    nt_guest_boundary_guest_enter();
    atomic_store_explicit(&data->ready, 1u, memory_order_release);
    /* Real instruction window, not a fabricated register image. */
    __asm__ __volatile__(
        ".globl nt_sup_fault_loop_begin\n"
        "nt_sup_fault_loop_begin:\n"
        "movl %1, %%eax\n"
        "testl %%eax, %%eax\n"
        "jne 1f\n"
        "lock incq %0\n"
        "jmp nt_sup_fault_loop_begin\n"
        "1:\n"
        ".globl nt_sup_fault_loop_end\n"
        "nt_sup_fault_loop_end:\n"
        : "+m"(data->ticks) : "m"(data->stop)
        : "eax", "memory");
    nt_guest_boundary_guest_leave();
    nt_guest_boundary_thread_detach();
    _exit(0);
}

static int wait_for_exit(pid_t child, int *status)
{
    for (int i = 0; i < 4000; ++i) {
        const pid_t got = waitpid(child, status, WNOHANG);
        if (got == child) return 1;
        if (got < 0 && errno != EINTR) return 0;
        pause_ms(1);
    }
    return 0;
}

int main(void)
{
    alarm(15);
    struct fixture_shared *data = mmap(NULL, sizeof(*data),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) return 1;
    memset(data, 0, sizeof(*data));
    const pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) child_code(data);

    int result = 3, status = 0;
    struct nt_thread_supervisor supervisor;
    nt_supervisor_init(&supervisor);
    for (int i = 0; i < 4000 &&
        (!atomic_load_explicit(&data->ready, memory_order_acquire) ||
         atomic_load_explicit(&data->ticks, memory_order_acquire) < 1000); ++i)
        pause_ms(1);
    if (atomic_load_explicit(&data->ticks, memory_order_acquire) < 1000)
        goto cleanup;

    if (nt_supervisor_seize_child(&supervisor, child, child) != 0) {
        if (errno == EPERM || errno == EACCES || errno == ENOTSUP)
            result = 77; /* Host policy, not a successful acceptance. */
        goto cleanup;
    }
    const struct nt_supervisor_guest_scope scope = {
        .boundary = &data->boundary.slots[0].boundary,
        .teb = atomic_load_explicit(&data->boundary.slots[0].teb,
                                    memory_order_acquire),
        .code_begin = (uintptr_t)nt_sup_fault_loop_begin,
        .code_end = (uintptr_t)nt_sup_fault_loop_end
    };
    struct nt_supervisor_nt_thread thread = {
        .tracer = &supervisor, .scope = scope,
        .granted_access = NT_SUP_THREAD_SUSPEND_RESUME |
            NT_SUP_THREAD_GET_CONTEXT | NT_SUP_THREAD_SET_CONTEXT
    };
    uint32_t previous = UINT32_MAX;
    if (!scope.teb || nt_supervisor_nt_suspend_thread(&thread, &previous) !=
            NT_STATUS_SUCCESS || previous != 0)
        goto cleanup;

    _Alignas(16) struct nt_context64 original = {0};
#ifdef NT_SUP_TEST_FORCE_FP_VERIFY_MISMATCH
    original.flags = 0x100008u;
#else
    original.flags = 0x100003u;
#endif
    if (nt_supervisor_nt_get_context(&thread, &original) != NT_STATUS_SUCCESS)
        goto cleanup;
    _Alignas(16) struct nt_context64 edited = original;
#ifdef NT_SUP_TEST_FORCE_FP_VERIFY_MISMATCH
    edited.flags = 0x100008u;
    edited.mxcsr ^= UINT32_C(0x2000);
    memcpy(edited.floating + 24, &edited.mxcsr, sizeof(edited.mxcsr));
    if (nt_supervisor_nt_set_context(&thread, &edited) !=
            NT_STATUS_NOT_SUPPORTED ||
        atomic_load_explicit(&nt_sup_test_fp_verify_corrupted,
                              memory_order_relaxed) != 1u)
        goto cleanup;
#else
    edited.flags = 0x100002u;
    edited.rax ^= UINT64_C(0x5555666677778888);
    if (nt_supervisor_nt_set_context(&thread, &edited) !=
            NT_STATUS_NOT_SUPPORTED ||
        atomic_load_explicit(&nt_sup_test_verify_corrupted,
                              memory_order_relaxed) != 1u)
        goto cleanup;
#endif
#if defined(NT_SUP_TEST_FORCE_GPR_ROLLBACK_FAILURE) || \
    defined(NT_SUP_TEST_FORCE_FP_ROLLBACK_FAILURE)
    /* The requested mutation reached the kernel, but readback failed
     * and rollback was forced to fail. No DETACH of these regs is legal.
     * Only the pidfd-pinned child can be terminated. */
    if (!thread.unsafe_register_state || !supervisor.compromised)
        goto cleanup;
    errno = 0;
    if (nt_supervisor_detach(&supervisor) != -1 || errno != EPERM)
        goto cleanup;
    errno = 0;
    if (nt_supervisor_continue(&supervisor) != -1 || errno != EPERM)
        goto cleanup;
    if (nt_supervisor_nt_resume_thread(&thread, &previous) !=
        NT_STATUS_THREAD_IS_TERMINATING)
        goto cleanup;
    if (nt_supervisor_terminate_compromised_child(&supervisor) != 0)
        goto cleanup;
    /* release() is not allowed to detach while SIGKILL is pending:
     * PTRACE_O_EXITKILL remains armed until kernel confirms death. */
    nt_supervisor_release(&supervisor);
    if (!wait_for_exit(child, &status) ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        goto cleanup;
    nt_supervisor_release(&supervisor); /* death now confirmed by pidfd */
    puts("PASS fatal CONTEXT rollback: no CONT/DETACH, pidfd-SIGKILL confirmed");
#else
    if (thread.unsafe_register_state)
        goto cleanup;
    _Alignas(16) struct nt_context64 restored = {0};
    restored.flags = original.flags;
    if (nt_supervisor_nt_get_context(&thread, &restored) != NT_STATUS_SUCCESS)
        goto cleanup;
#ifdef NT_SUP_TEST_FORCE_FP_VERIFY_MISMATCH
    if (restored.mxcsr != original.mxcsr ||
        memcmp(restored.floating, original.floating, 512) != 0)
        goto cleanup;
#else
    if (restored.rax != original.rax || restored.rip != original.rip ||
        restored.rsp != original.rsp)
        goto cleanup;
#endif
    if (nt_supervisor_detach(&supervisor) != 0)
        goto cleanup;
    nt_supervisor_release(&supervisor);
    atomic_store_explicit(&data->stop, 1u, memory_order_release);
    if (!wait_for_exit(child, &status) ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        goto cleanup;
    puts("PASS supervisor CONTEXT rollback: kernel GPR restored without false success");
#endif
    result = 0;
cleanup:
    if (result != 0) {
        if (supervisor.seized)
            (void)nt_supervisor_terminate_compromised_child(&supervisor);
        nt_supervisor_release(&supervisor);
        (void)kill(child, SIGKILL); /* child created solely by THIS test */
        (void)waitpid(child, &status, 0);
    }
    (void)munmap(data, sizeof(*data));
    return result;
}
