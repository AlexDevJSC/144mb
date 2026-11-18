/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GUEST_BOUNDARY_RUNTIME_H
#define NTNATIVE_GUEST_BOUNDARY_RUNTIME_H

#include "ntnative/guest_boundary.h"
#include "ntnative/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/types.h>

/* Experimental cooperative provenance table for the external ptrace
 * supervisor. MAP_SHARED is created by the trusted direct parent BEFORE
 * fork; no file descriptor, address or token is provided by PE imports.
 * This is not an adversarial process isolation mechanism: untrusted native
 * code executing in the same Linux address space can modify its own memory.
 *
 * Suspensions are safe only when BOTH the shared phase and the kernel's
 * RIP/GS/TEB indicate guest code. A DLL or executable whose code range is
 * not independently known to the parent must fail closed. */
enum { NT_GUEST_BOUNDARY_SLOTS = 256 };
struct nt_guest_boundary_slot {
    struct nt_guest_boundary boundary;
    _Atomic uint32_t tid;
    _Atomic uintptr_t teb;
};
struct nt_guest_boundary_session {
    struct nt_guest_boundary_slot slots[NT_GUEST_BOUNDARY_SLOTS];
};

/* Parent/launcher chooses the opt-in BEFORE PE IAT resolution. */
void nt_guest_boundary_enable_import_gate(void);
bool nt_guest_boundary_import_gate_enabled(void);
/* Immutable to the guest: supplied by the trusted loader before fork.
 * Main executable code range only; unloaded/secondary DLLs fail closed. */
bool nt_guest_boundary_set_image_range(uintptr_t begin, uintptr_t end);
bool nt_guest_boundary_get_image_range(uintptr_t *begin, uintptr_t *end);

/* Immutable PARENT manifest for DLLs imported and pinned before fork.
 * Deliberately excludes LoadLibrary after process start, JIT and any
 * module that the guest could unload/remap. MAX=64 matches dll_graph. */
enum { NT_GUEST_ADDITIONAL_CODE_MAX = 64 };
struct nt_guest_code_range { uintptr_t begin, end; };
bool nt_guest_boundary_set_startup_code_ranges(
    const struct nt_guest_code_range *ranges, size_t count);
bool nt_guest_boundary_get_startup_code_ranges(
    struct nt_guest_code_range *out, size_t capacity, size_t *count);

/* All resolved native imports get a W^X trampoline, irrespective of their
 * C return types or parameter counts. On allocation failure, returns NULL,
 * causing the PE loader to fail rather than installing an unguarded import. */
nt_guest_proc nt_guest_boundary_wrap_import(nt_guest_proc target);

/* Called only in the forked child before any guest instruction. The parent
 * owns the MAP_SHARED lifetime and may inspect it when authorizing stops. */
void nt_guest_boundary_install_child_session(struct nt_guest_boundary_session *session);
struct nt_guest_boundary_session *nt_guest_boundary_current_session(void);
const struct nt_guest_boundary_slot *nt_guest_boundary_find_thread(
    const struct nt_guest_boundary_session *session, pid_t tid);

/* The thread's initial image and TEB are registered after GS installation;
 * all states remain HOST until an explicit validated guest entry. */
bool nt_guest_boundary_thread_attach(uintptr_t teb);
void nt_guest_boundary_thread_detach(void);
void nt_guest_boundary_guest_enter(void);
void nt_guest_boundary_guest_leave(void);
uint32_t nt_guest_boundary_current_phase(void);
uint32_t nt_guest_boundary_host_nesting(void);
/* Is the CURRENT thread at a reviewed PE-owned import-return safe point?
 * The ordinary PE runner may have no parent supervisor session: that is
 * not by itself an error. Never use UNREGISTERED as a fake GUEST phase
 * for ptrace; this query ONLY authorises synchronous same-thread calls
 * from the reviewed return gate. */
bool nt_guest_boundary_special_apc_allowed(void);
/* Any nonlocal exception/longjmp that may bypass an import epilogue
 * permanently disables asynchronous stops on that TID until teardown.
 * The runtime must NOT attempt to reconstruct abandoned host locks or
 * assume the old return stack has unwound. This is fail-closed. */
void nt_guest_boundary_mark_nonlocal_transfer(void);
bool nt_guest_boundary_nonlocal_poisoned(void);


/* ABI-independent ms_x64 import assembly calls these. Reentrant host
 * callbacks must remain HOST while the outer runtime stack frame exists.
 * The return stack is private to the current Linux thread. */
uintptr_t nt_guest_boundary_import_enter(uintptr_t target, uintptr_t return_pc);
uintptr_t nt_guest_boundary_import_leave(void);
void nt_guest_boundary_import_entry(void); /* written in x86-64 assembly */
void nt_guest_boundary_import_return(void); /* synthetic Win64 import return */

/* Only the naked RaiseException/RtlUnwind entrypoints can legitimately
 * bypass the reviewed import-return stub. Given the captured synthetic
 * return, recover the immutable PE caller address only for ONE
 * non-nested host import, verified in the pre-fork image/.text manifest.
 * Returns the unchanged RIP if verification fails. No host stack unlock
 * or general ELF unwind is implied. */
uintptr_t nt_guest_boundary_seh_caller_pc(uintptr_t recorded_pc);

#endif
