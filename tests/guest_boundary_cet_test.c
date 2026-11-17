/* SPDX-License-Identifier: MIT */
/* Independently built with NT_TEST_FORCE_CET_SHSTK: prove that an AMD64
 * shadow-stack-enabled guest cannot receive an import trampoline, even
 * when the host's test runner lacks CET hardware. */
#include "ntnative/guest_boundary_runtime.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void __attribute__((ms_abi)) callback(void) {}
int main(void)
{
    nt_guest_boundary_enable_import_gate();
    nt_guest_proc original = callback;
    errno = 0;
    if (nt_guest_boundary_wrap_import(original) != NULL || errno != ENOTSUP)
        return 1;
    puts("PASS CET SHSTK: deny native-import gate when shadow-stack state is active");
    return 0;
}
