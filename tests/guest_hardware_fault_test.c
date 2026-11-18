/* SPDX-License-Identifier: MIT */
/* Pure kernel-signal classification regression. User-originated signals
 * MUST NOT spoof exceptions. The live mapped-PE hardware fault test is
 * separate (tests/pe/hardware_seh_fixture.S). */
#define _GNU_SOURCE
#include "guest_hardware_fault.h"
#include "ntnative/guest_boundary_runtime.h"
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include "compat.h"

/* This unit links the classifier standalone; deliberately no loader,
 * runtime mutex, PE map, ptrace or signal handler installation needed. */
bool nt_guest_boundary_signal_guest_phase(void) { return false; }
bool nt_guest_boundary_get_image_range(uintptr_t *begin, uintptr_t *end)
{ (void)begin; (void)end; return false; }
_Noreturn void nt_dispatch_hardware_exception(struct nt_context64 *ctx,
    uint32_t code, uint32_t count, const uintptr_t *arguments)
{
    (void)ctx; (void)code; (void)count; (void)arguments;
    abort();
}

static int verify(int signal, int linux_code, unsigned long err,
                  uint32_t expected, uint32_t parameters, uintptr_t access)
{
    uint32_t status = 0, count = 99;
    uintptr_t args[2] = {99, 99};
    if (!nt_guest_hardware_fault_translate(signal, linux_code, err,
            (uintptr_t)0x1234, &status, &count, args) ||
        status != expected || count != parameters ||
        (parameters == 2 && (args[0] != access || args[1] != 0x1234)))
        return 1;
    return 0;
}

int main(void)
{
    uint32_t status = 0, count = 0;
    uintptr_t args[2] = {0};
    if (verify(SIGSEGV, SEGV_MAPERR, 0, UINT32_C(0xc0000005), 2, 0) ||
        verify(SIGSEGV, SEGV_ACCERR, 2, UINT32_C(0xc0000005), 2, 1) ||
        verify(SIGSEGV, SEGV_ACCERR, 16, UINT32_C(0xc0000005), 2, 8) ||
        verify(SIGILL, ILL_ILLOPC, 0, UINT32_C(0xc000001d), 0, 0) ||
        verify(SIGILL, ILL_PRVOPC, 0, UINT32_C(0xc0000096), 0, 0) ||
        verify(SIGFPE, FPE_INTDIV, 0, UINT32_C(0xc0000094), 0, 0) ||
        verify(SIGFPE, FPE_INTOVF, 0, UINT32_C(0xc0000095), 0, 0) ||
        verify(SIGBUS, BUS_ADRALN, 0, UINT32_C(0x80000002), 0, 0) ||
        !nt_guest_hardware_fault_translate(SIGSEGV, SI_USER, 0, 0,
                                            &status, &count, args) == false ||
        nt_guest_hardware_fault_translate(SIGILL, SI_TKILL, 0, 0,
                                           &status, &count, args) ||
        nt_guest_hardware_fault_translate(SIGFPE, -1, 0, 0,
                                           &status, &count, args) ||
        nt_guest_hardware_fault_translate(SIGSEGV, 500, 0, 0,
                                           &status, &count, args)) {
        fputs("FAIL guest hardware signal classifier\n", stderr);
        return 1;
    }
    puts("PASS hardware fault translation: SIGSEGV read/write/execute, "
         "SIGILL, SIGFPE, SIGBUS, spoofed kill/tgkill denied");
    return 0;
}
