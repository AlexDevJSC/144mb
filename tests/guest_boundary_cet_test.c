/* SPDX-License-Identifier: MIT */
/* Independently built with NT_TEST_FORCE_CET_SHSTK: prove that an AMD64
 * shadow-stack-enabled guest cannot receive an import trampoline, even
 * when the host's test runner lacks CET hardware. */
#include "ntnative/guest_boundary_runtime.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void __attribute__((ms_abi)) callback(void) {}
int main(void)
{
    nt_guest_boundary_enable_import_gate();
    nt_guest_proc original = callback;
    errno = 0;
    if (nt_guest_boundary_wrap_import(original) != NULL || errno != ENOTSUP)
        return 1;
    /* Simulate SHSTK becoming active *during* an import call. Even on
     * runners with no CET hardware, the forced-policy test must reject
     * the return trampoline before it publishes a guest-safe phase. */
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        (void)nt_guest_boundary_import_leave();
        _Exit(0);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 142)
        return 3;
    puts("PASS CET SHSTK: deny import entry and a late enabled shadow-stack on return");
    return 0;
}
