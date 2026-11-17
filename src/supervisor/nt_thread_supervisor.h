/* SPDX-License-Identifier: MIT */
#ifndef NT_THREAD_SUPERVISOR_H
#define NT_THREAD_SUPERVISOR_H
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/user.h>

/* Experimental Linux AMD64 supervisor, not a Win32 API implementation.
 * Only a direct, live child of this caller can be supervised; `tid` must
 * belong to that child. All operations are synchronous and single-owner.
 * Suspend only guest-code windows: stopping a thread while it owns a host
 * mutex is NOT safe, regardless of ptrace capabilities. */
struct nt_thread_supervisor {
    pid_t child;
    pid_t tid;
    int pidfd;
    bool seized;
    bool stopped;
};

void nt_supervisor_init(struct nt_thread_supervisor *sup);
/* 0 success, -1 errno; ENOTSUP means host feature unavailable. */
int nt_supervisor_seize_child(struct nt_thread_supervisor *sup, pid_t child, pid_t tid);
int nt_supervisor_interrupt(struct nt_thread_supervisor *sup, unsigned timeout_ms);
int nt_supervisor_get_registers(struct nt_thread_supervisor *sup, struct user_regs_struct *out);
/* Limited proof of real SETREGSET. Does not change RIP/RSP or guest memory. */
int nt_supervisor_set_rax(struct nt_thread_supervisor *sup, uint64_t value);
int nt_supervisor_continue(struct nt_thread_supervisor *sup);
/* Detach while stopped; does not kill or wait for the child. */
int nt_supervisor_detach(struct nt_thread_supervisor *sup);
void nt_supervisor_release(struct nt_thread_supervisor *sup);
#endif
