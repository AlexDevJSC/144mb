/* SPDX-License-Identifier: MIT */
#ifndef NT_GUEST_XSTATE_POLICY_H
#define NT_GUEST_XSTATE_POLICY_H
/* The signal bridge may preserve an OS-enabled PKRU component ONLY
 * when the guest's saved PKRU is identical to the current host-thread
 * value. Neither a temporary ELF stack nor an arbitrary Win64 callback
 * is permitted to change access rights. We never XRSTOR the PKRU bit;
 * the final CONTEXT restore must use the same unchanged PKRU.
 *
 * This CPU-only routine is async-signal-safe (CPUID/RDPKRU and bounded
 * reads), with no system call, allocator, external library or lock. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cpuid.h>

static inline bool nt_guest_xstate_pkru_matches(
    const volatile unsigned char *fp, size_t bytes)
{
    if (!fp || bytes < 576)
        return false;
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(0xdu, 9u, eax, ebx, ecx, edx);
    if (eax < 4 || ebx < 576 || ebx > bytes - 4)
        return false; /* invalid CPUID layout or truncated XSAVE */
    const uint32_t stored =
        (uint32_t)fp[ebx] |
        ((uint32_t)fp[ebx + 1] << 8) |
        ((uint32_t)fp[ebx + 2] << 16) |
        ((uint32_t)fp[ebx + 3] << 24);
    uint32_t current = 0, ignored = 0;
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xee"
                         : "=a"(current), "=d"(ignored) : "c"(0));
    return stored == current;
}
#endif
