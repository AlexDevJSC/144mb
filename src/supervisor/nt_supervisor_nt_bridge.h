/* SPDX-License-Identifier: MIT */
#ifndef NT_SUPERVISOR_NT_BRIDGE_H
#define NT_SUPERVISOR_NT_BRIDGE_H

#include "nt_thread_supervisor.h"
#include "../runtime/compat.h"
#include "../runtime/nt_services.h"

/* Trusted supervisor-side NT contract adapter. This is NOT installed as
 * a Win32 export: the real runtime needs authenticated IPC for HANDLE
 * ownership, rights and generations before it may call this backend.
 * The caller owns one registered, same-namespace PE child TID. */
struct nt_supervisor_nt_thread {
    struct nt_thread_supervisor *tracer;
    struct nt_supervisor_guest_scope scope;
    uint32_t granted_access;
    uint32_t suspend_count;
};

enum {
    NT_SUP_THREAD_SUSPEND_RESUME = 0x2,
    NT_SUP_THREAD_GET_CONTEXT = 0x8,
    NT_SUP_THREAD_SET_CONTEXT = 0x10,
    NT_SUP_CONTEXT_AMD64 = 0x100000,
    NT_SUP_CONTEXT_CONTROL = 0x1,
    NT_SUP_CONTEXT_INTEGER = 0x2,
    NT_SUP_CONTEXT_SEGMENTS = 0x4,
    NT_SUP_CONTEXT_FLOATING_POINT = 0x8
};

nt_status nt_supervisor_nt_suspend_thread(struct nt_supervisor_nt_thread *state,
                                          uint32_t *previous);
nt_status nt_supervisor_nt_resume_thread(struct nt_supervisor_nt_thread *state,
                                         uint32_t *previous);
nt_status nt_supervisor_nt_get_context(struct nt_supervisor_nt_thread *state,
                                       struct nt_context64 *context);
nt_status nt_supervisor_nt_set_context(struct nt_supervisor_nt_thread *state,
                                       const struct nt_context64 *context);
/* Experimental special APC to import-free fixture only. Returns success
 * only after INT3 interception and full GPR/XSTATE restoration. */
nt_status nt_supervisor_nt_queue_trusted_apc(struct nt_supervisor_nt_thread *state,
                                             uintptr_t entry, uintptr_t stack_top,
                                             uintptr_t argument,
                                             const _Atomic uint32_t *completion);
#endif
