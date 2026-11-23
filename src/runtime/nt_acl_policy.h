/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FS4_ACL_POLICY_H
#define NTNATIVE_FS4_ACL_POLICY_H
#include "nt_acl_engine.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A token constructed ONLY by trusted code from kernel-authenticated
 * credentials. The application/PE must not populate uid, gid or groups.
 * Primary Unix user, primary group, optional supplementary groups and
 * Everyone are represented as synthetic, bounded S-1-22/S-1-1 SIDs.
 * We do not claim this is a Windows domain token or grant any privileges. */
struct nt_acl_peer_token {
    struct nt_acl_token token;
    uint8_t sid_storage[32][16];
    uint32_t privileges; /* always zero for this mapping */
};
int nt_acl_peer_token_init(struct nt_acl_peer_token *out,
                           uid_t authenticated_uid, gid_t authenticated_gid,
                           const gid_t *supplementary, size_t group_count,
                           const uint8_t *deny_only);

/* Status is *explicit*: absence of managed SD is not an ACL allow.
 * Callers must use their existing legacy policy for ABSENT. A corrupt
 * record is ERROR, never ABSENT. */
enum { NT_ACL_POLICY_ERROR = -1, NT_ACL_POLICY_DENIED = 0,
       NT_ACL_POLICY_ALLOWED = 1, NT_ACL_POLICY_ABSENT = 2 };

/* GenericMapping is chosen by a trusted caller for that NT object class.
 * No raw guest pointer, no direct physical backing path. FD must refer to
 * the inode being checked; O_PATH is accepted for read-only checks.
 * The caller still MUST serialize ACL writes against all operations. */
int nt_acl_policy_check_fd(int fd, const struct nt_acl_peer_token *peer,
                           uint32_t requested,
                           const struct nt_acl_generic_mapping *mapping);

/* TRUSTED SERVICE ONLY. Install DACL_SECURITY_INFORMATION on a pinned
 * inode after the caller acquired namespace WAL and an appropriate share
 * lease; verify WRITE_DAC against the kernel-derived peer token. Existing
 * owner/group remain immutable; unmanaged inode is bootstrapped with the
 * trusted peer owner/group. Returns policy ALLOWED or DENIED/ERROR.
 * No guest memory pointers, and no owner/SACL privilege escalation. */
int nt_acl_policy_set_dacl_fd(int fd, const struct nt_acl_peer_token *peer,
                              const uint8_t *input, size_t input_bytes);

/* Resolve only beneath a pinned root with no symlink/mount escapes.
 * DOS names begin C:/, POSIX names begin /; root itself is ".".
 * If parent=true, check the object's immediate parent directory instead. */
int nt_acl_policy_check_path(int root_fd, const char *path, bool parent,
                             const struct nt_acl_peer_token *peer,
                             uint32_t requested,
                             const struct nt_acl_generic_mapping *mapping);

/* Supported concrete file/dir rights for the first service check gate.
 * These are bit positions of NT FILE_* / standard rights, not POSIX mode. */
enum {
    NT_ACL_DATA_READ = 0x00000001u,
    NT_ACL_DATA_WRITE = 0x00000002u,
    NT_ACL_DIR_ADD_SUBDIR = 0x00000004u,
    NT_ACL_DATA_APPEND = 0x00000004u,
    NT_ACL_READ_EA = 0x00000008u,
    NT_ACL_WRITE_EA = 0x00000010u,
    NT_ACL_EXECUTE = 0x00000020u,
    NT_ACL_DIR_DELETE_CHILD = 0x00000040u,
    NT_ACL_READ_ATTR = 0x00000080u,
    NT_ACL_WRITE_ATTR = 0x00000100u,
    NT_ACL_DELETE = 0x00010000u,
    NT_ACL_READ_CONTROL = 0x00020000u,
    NT_ACL_WRITE_DAC = 0x00040000u,
    NT_ACL_WRITE_OWNER = 0x00080000u
};

/* File object mapping, does not include security rights by default.
 * The caller supplies an explicit desired mask and must ensure its Win32
 * HANDLE has at most the granted access (including duplicate handles). */
extern const struct nt_acl_generic_mapping nt_acl_file_generic_mapping;
extern const struct nt_acl_generic_mapping nt_acl_directory_generic_mapping;
#endif
