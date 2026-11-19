/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FS4_ACL_STORAGE_H
#define NTNATIVE_FS4_ACL_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Trusted-service-only, pinned inode xattr backing. NO guest-facing setter.
 * Caller MUST have authenticated the token, authorized WRITE_DAC, and
 * serialized through the namespace WAL before calling write(). Linking
 * this helper alone does not authorize filesystem operations. */
enum { NT_ACL_SD_MAX_BYTES = 32768 };
enum { NT_ACL_STORAGE_ABSENT = 0, NT_ACL_STORAGE_PRESENT = 1,
       NT_ACL_STORAGE_ERROR = -1, NT_ACL_STORAGE_SHORT_BUFFER = -2 };

int nt_acl_storage_read_fd(int fd, uint8_t *out, size_t capacity,
                           size_t *needed);
/* Full replace via one xattr, fsync inode and verify committed contents.
 * On fsync failure the new value MAY be visible; return false, fail closed. */
bool nt_acl_storage_write_trusted_fd(int fd, const uint8_t *descriptor,
                                     size_t bytes);
#endif
