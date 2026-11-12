/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GUEST_THREAD_H
#define NTNATIVE_GUEST_THREAD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Partial AMD64 TEB/PEB representation. The native Win64 TEB has 64
 * inline dynamic TLS slots starting at 0x1480. Keep the entire range mapped
 * for guest code accessing those offsets directly through GS. This is not
 * the complete NT process or thread environment. */
enum { NT_GUEST_TLS_MAX = 4096, NT_GUEST_TLS_ALIGNMENT = 16 };
enum { NT_TEB_LAST_ERROR = 0x68, NT_TEB_TLS_SLOTS = 0x1480,
       NT_GUEST_DYNAMIC_TLS_SLOTS = 64, NT_GUEST_TEB_BYTES = 8192 };
struct nt_guest_tls {
    const unsigned char *data;
    size_t bytes, zero_fill;
};

struct nt_guest_unicode_string {
    uint16_t length, maximum_length;
    uint16_t *buffer;
};

struct nt_guest_process_parameters {
    uint32_t maximum_length, length, flags, debug_flags;
    uintptr_t console;
    uint32_t console_flags;
    uintptr_t input, output, error;
    struct nt_guest_unicode_string current_directory;
    uintptr_t directory_handle;
    struct nt_guest_unicode_string dll_path, image_path, command_line;
    uint16_t *environment;
};

struct nt_guest_process {
    _Alignas(16) unsigned char peb[512];
    struct nt_guest_process_parameters parameters;
};

struct nt_guest_thread {
    _Alignas(16) unsigned char teb[NT_GUEST_TEB_BYTES];
    struct nt_guest_process standalone;
    _Alignas(NT_GUEST_TLS_ALIGNMENT) unsigned char tls_block[NT_GUEST_TLS_MAX];
    uintptr_t tls_vector[64];
    uintptr_t saved_gs;
    unsigned char *saved_teb;
    struct nt_guest_thread *saved_thread;
    /* Thread-specific generations prevent a freed and reallocated TLS index
     * from returning a stale value from this thread's TEB. */
    uint64_t dynamic_tls_epochs[NT_GUEST_DYNAMIC_TLS_SLOTS];
    bool entered;
};

void nt_guest_process_init(struct nt_guest_process *process, uintptr_t image, uintptr_t heap);
bool nt_guest_thread_enter(struct nt_guest_thread *state, uintptr_t image_base);
bool nt_guest_thread_enter_with_tls(struct nt_guest_thread *state, uintptr_t image_base,
                                    const struct nt_guest_tls *initial);
bool nt_guest_thread_enter_in_process(struct nt_guest_thread *state, uintptr_t image_base,
        const struct nt_guest_tls *initial, const struct nt_guest_process *process);
bool nt_guest_thread_leave(struct nt_guest_thread *state);
unsigned char *nt_guest_thread_teb(void);
struct nt_guest_thread *nt_guest_thread_current(void);
#endif
