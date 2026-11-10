/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GUEST_THREAD_H
#define NTNATIVE_GUEST_THREAD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimum AMD64 TEB/PEB representation. It does not claim a complete NT
 * process environment and must only be installed for the guest worker.
 */
struct nt_guest_thread {
    _Alignas(16) unsigned char teb[4096];
    _Alignas(16) unsigned char peb[512];
    _Alignas(16) unsigned char tls_block[4096];
    uintptr_t tls_vector[64];
    uintptr_t saved_gs;
    bool entered;
};

bool nt_guest_thread_enter(struct nt_guest_thread *state, uintptr_t image_base);
bool nt_guest_thread_leave(struct nt_guest_thread *state);
#endif
