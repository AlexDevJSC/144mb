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

/* Object-specific, explicitly authenticated GENERIC_* translation.
 * A null mapping does NOT give generic rights: the old entrypoint keeps
 * rejecting masks it cannot interpret. The mapped output contains only
 * concrete access bits, so the caller can compare it to its NT grants. */
struct nt_acl_generic_mapping {
    uint32_t read, write, execute, all;
};
int nt_acl_access_check_mapped(const uint8_t *sd, size_t bytes,
                               const struct nt_acl_token *token,
                               uint32_t requested,
                               const struct nt_acl_generic_mapping *mapping,
                               uint32_t *granted);

/* Validate a complete self-relative descriptor (not merely parse an ACL
 * header). No writes to filesystem; never accept a malformed SD to xattr. */
int nt_acl_validate_descriptor(const uint8_t *sd, size_t bytes);

/* Materialize a CHILD self-relative descriptor with authenticated owner
 * and group SIDs, carrying forward inherited ACEs (including arbitrary
 * binary SID values). CREATOR_OWNER/GROUP SIDs are substituted using the
 * supplied identities. Only ACL revision 2 allow/deny ACEs are supported.
 * Result 0 means insufficient capacity, with *needed set, no output. */
int nt_acl_inherit_descriptor(const uint8_t *parent_sd, size_t bytes,
                              bool child_directory,
                              const uint8_t *owner, size_t owner_bytes,
                              const uint8_t *group, size_t group_bytes,
                              uint8_t *out, size_t capacity, size_t *needed);

/* Narrow, explicit UNIX-backed SID identity. The caller is responsible for
 * sourcing uid/gid from SO_PEERCRED or fuse_get_context, never guest IPC.
 * Does NOT simulate domain SIDs, supplementary groups or impersonation.
 * Its self-owned bytes make SID pointers valid until the struct is moved. */
struct nt_acl_unix_token {
    struct nt_acl_token token;
    uint8_t sid_bytes[3][16];
};
bool nt_acl_unix_token_init(struct nt_acl_unix_token *out,
                            uint32_t authenticated_uid,
                            uint32_t authenticated_gid);
#endif
