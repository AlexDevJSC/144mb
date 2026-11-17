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
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

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
    atomic_store(&ticks, 0);
    atomic_store(&entered, 0);
    atomic_store(&stop, 0);
    atomic_store(&host_phase, 0);
    atomic_store(&finish, 0);
    uint32_t tid = 0;
    const uintptr_t thread = nt_CreateThread(NULL, 0, test_worker,
                                              NULL, 0, &tid);
    if (!thread || !tid) return 10;
    for (int i = 0; i < 3000 && !atomic_load(&entered); ++i)
        msleep(1);
    if (!atomic_load(&entered)) return 11;
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
    if (nt_SuspendThread(thread) != 1) return 14;
    _Alignas(16) struct nt_context64 context = {0};
    context.flags = 0x100003u;
    if (!nt_GetThreadContext(thread, &context) ||
        context.rip < (uintptr_t)runtime_test_guest_begin ||
        context.rip >= (uintptr_t)runtime_test_guest_end ||
        context.rsp == 0)
        return 15;
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
    if (!nt_SetThreadContext(thread, &integer)) return 18;
    integer.flags = 0x100003u;
    if (nt_SetThreadContext(thread, &integer)) return 19;
    if (nt_ResumeThread(thread) != 2) return 20;
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
         "Get/SetThreadContext AMD64 integer readback, ResumeThread, "
         "HOST fail-close, authenticated direct-parent IPC, two sessions");
    return 0;
}
