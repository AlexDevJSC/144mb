/* SPDX-License-Identifier: MIT */
/* Kernel realtime-signal -> preallocated stack -> callback -> exact GPR
 * and FXSAVE restoration smoke test. Uses the production AMD64 context
 * restore assembly, not setjmp(), longjmp(), or a synthetic completed APC.
 * The PE32+ loop fixture independently proves the Win64 callback ABI. */
#define _GNU_SOURCE
#include "guest_async_apc.h"
#include "compat.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static _Thread_local bool inside_guest;
static _Atomic bool ready, pending_apc;
static _Atomic uint32_t calls;
static pthread_t target;
static uintptr_t code_low, code_high;
static _Atomic int failure;

bool nt_guest_boundary_signal_guest_phase(void) { return inside_guest; }
bool nt_guest_boundary_signal_guest_code(uintptr_t pc)
{ return pc >= code_low && pc < code_high; }
bool nt_runtime_special_apc_pending_signal(void)
{ return atomic_load_explicit(&pending_apc, memory_order_acquire); }

void nt_runtime_special_apc_checkpoint(void)
{
    uintptr_t low = 0;
    size_t bytes = 0;
    if (!pthread_equal(pthread_self(), target) ||
        !nt_runtime_guest_async_apc_stack_for_current(&low, &bytes) ||
        bytes < 65536u || low == 0)
        atomic_store(&failure, 1);
    atomic_fetch_add_explicit(&calls, 1, memory_order_release);
    atomic_store_explicit(&pending_apc, false, memory_order_release);
}

/* The test invokes only nt_restore_software_context from context.S;
 * other raw Win64 SEH entry stubs deliberately cannot run here. */
_Noreturn void nt_dispatch_software_exception(struct nt_context64 *context,
    uint32_t status, uint32_t flags, uint32_t count, const uintptr_t *args)
{
    (void)context; (void)status; (void)flags; (void)count; (void)args;
    _Exit(91);
}
_Noreturn void nt_unwind_software_exception(struct nt_context64 *context,
    uint64_t frame, uint64_t target_pc, struct nt_exception_record *record,
    uint64_t value, struct nt_context64 *output)
{
    (void)context; (void)frame; (void)target_pc; (void)record;
    (void)value; (void)output;
    _Exit(92);
}

__attribute__((noinline)) static void *pure_guest_loop(void *ignored)
{
    (void)ignored;
    if (!nt_runtime_guest_async_apc_thread_attach()) {
        atomic_store(&failure, 2);
        return NULL;
    }
    inside_guest = true;
    atomic_store_explicit(&ready, true, memory_order_release);
    /* No Win32 import, host mutex, blocking syscall or alertable wait. */
    for (uint64_t i = 0; i < UINT64_C(800000000); ++i) {
        if (atomic_load_explicit(&calls, memory_order_acquire))
            break;
        __asm__ __volatile__("pause" ::: "memory");
    }
    inside_guest = false;
    nt_runtime_guest_async_apc_thread_detach();
    if (atomic_load(&calls) != 1)
        atomic_store(&failure, 3);
    return NULL;
}
int main(void)
{
    /* Reserve the preferred signal for a simulated pre-existing host
     * component. The runtime must neither steal SIG_IGN nor give up
     * asynchronous PE dispatch when a later realtime signal is free. */
    const int preferred = SIGRTMIN + 4;
    if (preferred > SIGRTMAX)
        return 2;
    struct sigaction previous = {0};
    if (sigaction(preferred, NULL, &previous) != 0)
        return 2;
    const bool test_reserved = previous.sa_handler == SIG_DFL;
    if (test_reserved) {
        struct sigaction occupied = {0};
        occupied.sa_handler = SIG_IGN;
        sigemptyset(&occupied.sa_mask);
        if (sigaction(preferred, &occupied, NULL) != 0)
            return 2;
    }
    if (!nt_runtime_guest_async_apc_install())
        return 2;
    struct sigaction after = {0};
    if (sigaction(preferred, NULL, &after) != 0 ||
        (test_reserved && after.sa_handler != SIG_IGN))
        return 2;
    /* The synthetic immutable code grant is under TEST ownership only.
     * Production gets its range exclusively from validated PE loader. */
    const uintptr_t entry = (uintptr_t)pure_guest_loop;
    code_low = entry - 64u;
    code_high = entry + 1024u;
    if (pthread_create(&target, NULL, pure_guest_loop, NULL) != 0)
        return 3;
    for (int attempt = 0; attempt < 10000 && !atomic_load(&ready); ++attempt)
        usleep(100);
    if (!atomic_load(&ready))
        return 4;
    atomic_store_explicit(&pending_apc, true, memory_order_release);
    if (!nt_runtime_guest_async_apc_nudge(target) ||
        pthread_join(target, NULL) != 0)
        return 5;
    if (atomic_load(&failure) != 0 || atomic_load(&calls) != 1) {
        fprintf(stderr, "FAIL asynchronous APC restore: failure=%d calls=%u\n",
                atomic_load(&failure), atomic_load(&calls));
        return 6;
    }
    puts("PASS host-owned preferred signal preserved; asynchronous APC "
         "used another realtime signal, private stack and AMD64 restore");
    return 0;
}
