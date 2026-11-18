/* SPDX-License-Identifier: MIT */
#ifndef NT_GUEST_ASYNC_APC_H
#define NT_GUEST_ASYNC_APC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Private NT runtime APIs. The trusted PE host enables this after it
 * establishes immutable executable code grants. Never export to Win32. */
bool nt_runtime_guest_async_apc_install(void);
bool nt_runtime_guest_async_apc_thread_attach(void);
void nt_runtime_guest_async_apc_thread_detach(void);
bool nt_runtime_guest_async_apc_nudge(pthread_t target);

/* Is the current SysV frame executing on the private APC dispatch stack?
 * Used by frame-based Windows SEH only, not as proof of ptrace safety. */
bool nt_runtime_guest_async_apc_stack_for_current(uintptr_t *low, size_t *size);
#endif
