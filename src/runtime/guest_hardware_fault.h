/* SPDX-License-Identifier: MIT */
#ifndef NT_GUEST_HARDWARE_FAULT_H
#define NT_GUEST_HARDWARE_FAULT_H

#include <stdbool.h>
#include <stdint.h>

/* Host-only, opt-in signal bridge. The privileged loader enables it only
 * after validating a PE64 exception table and executable code scope.
 * Never callable by imports from arbitrary PE code. */
bool nt_runtime_guest_hardware_fault_install(void);
bool nt_runtime_guest_hardware_fault_thread_attach(void);
void nt_runtime_guest_hardware_fault_thread_detach(void);

/* Pure classifier shared by the async signal gate and its negative tests.
 * CPU-generated si_code MUST be positive, so kill/tgkill cannot forge SEH.
 * err holds x86-64 PF error bits only when the kernel signal is SIGSEGV. */
bool nt_guest_hardware_fault_translate(int signal, int code, unsigned long err,
                                       uintptr_t address, uint32_t *nt_code,
                                       uint32_t *argument_count,
                                       uintptr_t arguments[2]);
#endif
