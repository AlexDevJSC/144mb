/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FS4_ACL_ENGINE_H
#define NTNATIVE_FS4_ACL_ENGINE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Isolated Windows SD/ACL evaluation core. Not a POSIX mode projection.
 * Caller supplies authenticated token SIDs; never trust guest-declared IDs.
 * Positive 1=allowed, zero=denied, negative=invalid/unsupported (fail closed).
 */
enum { NT_ACL_INVALID = -1, NT_ACL_UNSUPPORTED = -2,
       NT_ACL_DENIED = 0, NT_ACL_ALLOWED = 1 };
/* Use-for-deny-only groups may match DENY ACEs but never ALLOW ACEs.
 * Flags are trusted token metadata, not supplied by a PE guest. All other
 * bits are rejected instead of silently enabling an unsupported privilege. */
enum { NT_ACL_SID_DENY_ONLY = 1u };
struct nt_acl_token {
    const uint8_t *sid[32];
    size_t length[32], count;
    uint8_t sid_flags[32]; /* zero = enabled; DENY_ONLY = deny-only group */
};
int nt_acl_access_check(const uint8_t *sd, size_t bytes,
                        const struct nt_acl_token *token, uint32_t requested,
                        uint32_t *granted);
int nt_acl_inherit_dacl(const uint8_t *parent_sd, size_t bytes,
                        bool child_directory, uint8_t *out,
                        size_t capacity, size_t *needed);
#endif
