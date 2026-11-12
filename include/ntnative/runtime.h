/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_RUNTIME_H
#define NTNATIVE_RUNTIME_H

#include <stdbool.h>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define NT_MS_ABI
#elif defined(__x86_64__) && defined(__GNUC__)
#define NT_MS_ABI __attribute__((ms_abi))
#else
#error The runtime requires the Microsoft x64 ABI on x86-64
#endif

typedef void (NT_MS_ABI *nt_guest_proc)(void);

/* The caller converts the returned address to the declared guest signature
 * before calling it. A missing symbol returns NULL.
 */
nt_guest_proc nt_runtime_resolve(const char *module, const char *name);
nt_guest_proc nt_runtime_resolve_ordinal(const char *module, unsigned int ordinal);

static inline bool nt_module_name_equal(const char *left, const char *right)
{
    for (;; ++left, ++right) {
        unsigned char a = (unsigned char)*left;
        unsigned char b = (unsigned char)*right;
        if (a >= 'A' && a <= 'Z')
            a = (unsigned char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z')
            b = (unsigned char)(b + ('a' - 'A'));
        if (a != b)
            return false;
        if (a == '\0')
            return true;
    }
}

#endif
