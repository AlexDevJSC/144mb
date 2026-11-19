/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FS4_ACL_STORAGE_H
#define NTNATIVE_FS4_ACL_STORAGE_H

/* Trusted, internal DACL xattr persistence. NOT a Windows permission API.
 * The caller MUST hold the physical C: namespace WAL grant, authenticate
 * its token and check WRITE_DAC before calling the writer. Exporting this
 * directly to an untrusted PE/FUSE client is a security violation. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FS4_ACL_ABSENT=0, FS4_ACL_PRESENT=1 };
/* Reads a valid DACL xattr. Returns ABSENT, PRESENT, or -1 + errno.
 * On corrupted/unrepresentable metadata, fail closed with EUCLEAN. */
int fs4_acl_storage_read(int fd, void *out, size_t capacity, size_t *needed);

/* Persists one complete, validated self-relative descriptor to the pinned
 * inode with an explicit fsync before success. No WAL undo for mutations. */
bool fs4_acl_storage_write_trusted(int fd, const void *descriptor, size_t size);

/* Inherit a present parent DACL into a new, still-private child inode.
 * A caller must invoke this *before publication* while holding the WAL
 * namespace grant. If parent has no DACL, inherited=false and no write. */
bool fs4_acl_storage_inherit_trusted(int parent_fd, int child_fd,
                                    uint32_t owner_uid, uint32_t group_gid,
                                    bool child_directory, bool *inherited);

#endif
