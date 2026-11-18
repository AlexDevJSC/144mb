/* SPDX-License-Identifier: MIT */
/* Real kernel CPU faults through the async-safe signal frame -> dedicated
 * host dispatch-stack transition. This intentionally stubs final Windows
 * SEH; the separate PE64 integration test verifies .pdata/.xdata. */
#define _GNU_SOURCE
#include "guest_hardware_fault.h"
#include "ntnative/guest_boundary_runtime.h"
#include "compat.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static _Thread_local volatile sig_atomic_t guest_phase;
static uintptr_t trusted_begin, trusted_end;
static int current_mode;

bool nt_guest_boundary_signal_guest_phase(void)
{
    return guest_phase != 0;
}
bool nt_guest_boundary_get_image_range(uintptr_t *start, uintptr_t *end)
{
    if (!start || !end) return false;
    *start = trusted_begin;
    *end = trusted_end;
    return trusted_begin != 0 && trusted_end > trusted_begin;
}

_Noreturn void nt_dispatch_hardware_exception(struct nt_context64 *context,
    uint32_t code, uint32_t count, const uintptr_t *args)
{
    const uint32_t expected = current_mode == 0 ? UINT32_C(0xc000001d) :
                              current_mode == 1 ? UINT32_C(0xc0000094) :
                                                  UINT32_C(0xc0000005);
    if (!context || context->rip < trusted_begin ||
        context->rip >= trusted_end || !context->rsp ||
        code != expected ||
        (current_mode == 2 &&
         (count != 2 || !args || args[0] != 0 || args[1] != 0)))
        _Exit(91);
    /* A REAL kernel signal reached a normal C dispatch function on a
     * separate stack, not a handler executing user/Windows callbacks. */
    _Exit(0);
}

__attribute__((noinline)) static void fault_ud2(void)
{
    __asm__ __volatile__("ud2" ::: "memory");
}
__attribute__((noinline)) static void fault_divide(void)
{
    __asm__ __volatile__(
        "movl $1, %%eax\n\t"
        "xorl %%edx, %%edx\n\t"
        "xorl %%ecx, %%ecx\n\t"
        "divl %%ecx\n\t" ::: "rax", "rcx", "rdx", "cc");
}
__attribute__((noinline)) static void fault_null(void)
{
    __asm__ __volatile__(
        "xorq %%rax, %%rax\n\t"
        "movq (%%rax), %%rax\n\t" ::: "rax", "memory");
}

static _Noreturn void child_case(int mode)
{
    current_mode = mode;
    void (*target)(void) = mode == 0 ? fault_ud2 :
                           mode == 1 ? fault_divide : fault_null;
    uintptr_t pc = 0;
    _Static_assert(sizeof(pc) == sizeof(target), "AMD64 function address");
    memcpy(&pc, &target, sizeof(pc));
    trusted_begin = pc > 16 ? pc - 16 : pc;
    trusted_end = pc + 512;
    if (!nt_runtime_guest_hardware_fault_install() ||
        !nt_runtime_guest_hardware_fault_thread_attach())
        _Exit(75);
    guest_phase = 1;
    if (mode == 3) {
        raise(SIGILL); /* SI_TKILL, NOT a CPU-origin SIGILL */
        _Exit(76);
    }
    if (mode == 4) {
        guest_phase = 0; /* even genuine CPU UD2 must never cross host */
        fault_ud2();
    }
    target();
    _Exit(77); /* CPU fault was erroneously ignored */
}

int main(void)
{
    for (int mode = 0; mode < 5; ++mode) {
        const pid_t child = fork();
        if (child < 0) return 1;
        if (child == 0)
            child_case(mode);
        int status = 0;
        if (waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) ||
            WEXITSTATUS(status) !=
                (mode < 3 ? 0 : 128 + SIGILL)) {
            fprintf(stderr, "FAIL kernel signal bridge mode=%d status=%d\n",
                    mode, status);
            return 1;
        }
    }
    puts("PASS real kernel hardware signals: UD2, divide/0, NULL access "
         "via altstack/ucontext/host dispatcher; spoof and HOST rejected");
    return 0;
}
