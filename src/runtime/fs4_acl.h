/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FS4_ACL_H
#define NTNATIVE_FS4_ACL_H
/*
 * Strict bounded SECURITY_DESCRIPTOR_RELATIVE and ordered DACL evaluator.
 * This is a pure policy engine: it does NOT, by itself, authorize any
 * Win32/FUSE mutation, impersonate a Windows token or change Linux DAC.
 * An integrator must provide authenticated token SIDs and guard every
 * relevant filesystem operation. Unknown ACE types fail closed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    FS4_SD_LIMIT = 2048,
    FS4_MAX_ACES = 128,
    FS4_TOKEN_SIDS = 32,
    FS4_SID_LIMIT = 68,
    FS4_ACE_ALLOW = 0,
    FS4_ACE_DENY = 1,
    FS4_ACE_OBJECT_INHERIT = 0x01,
    FS4_ACE_CONTAINER_INHERIT = 0x02,
    FS4_ACE_NO_PROPAGATE = 0x04,
    FS4_ACE_INHERIT_ONLY = 0x08,
    FS4_ACE_INHERITED = 0x10
};

struct fs4_acl_token {
    const unsigned char *sid[FS4_TOKEN_SIDS];
    size_t sid_length[FS4_TOKEN_SIDS];
    size_t count;
};

/* Enforces: self-relative layout, aligned offsets, SID revision, every
 * ACE length/count and DACL coverage. Opaque/unknown ACEs are rejected,
 * not silently treated as an access grant or inherit operation. */
bool fs4_acl_validate(const void *descriptor, size_t length);

/* Ordered Win32 DENY/ALLOW evaluation with a previously authenticated
 * list of SIDs. TRUE only if every requested access bit was granted.
 * No implicit owner override, privilege bypass, SACL or NULL DACL. */
bool fs4_acl_check(const void *descriptor, size_t length,
                   const struct fs4_acl_token *token, uint32_t rights);

/* Inherit standard ACEs into a child descriptor with synthetic Unix
 * owner/group SIDs, substituting CREATOR_OWNER and CREATOR_GROUP. A parent
 * whose DACL cannot be evaluated is NOT inherited. The result is a fully
 * validated self-relative descriptor, never a pointer into parent memory.
 * Returns false and sets errno on insufficient buffer/unsupported ACE. */
bool fs4_acl_inherit(const void *parent, size_t parent_length,
                     uint32_t owner_uid, uint32_t group_gid, bool child_dir,
                     void *out, size_t capacity, size_t *written);

/* Build synthetic owner/group and well-known SIDs without writing Linux
 * ACL/xattr. Returns 16 bytes for S-1-22-{1,2}-<id>. */
void fs4_acl_unix_sid(unsigned char out[16], uint32_t type, uint32_t id);

#endif
