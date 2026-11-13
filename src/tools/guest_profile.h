/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GUEST_PROFILE_H
#define NTNATIVE_GUEST_PROFILE_H

#include <stddef.h>

/* Open the ordinary Linux directory named "C:" beneath the user's state
 * directory. On first use, migrate an existing private "drive-c" directory
 * by rename (preserving original files, not copying them). Existing C: and
 * drive-c at once is an error; a new directory is never overwritten.
 * An absolute state_home takes precedence over home/.local/state.
 * The caller owns the returned close-on-exec directory descriptor.
 */
int nt_open_guest_profile(const char *state_home, const char *home);

/* Open an explicitly selected existing Linux folder for sharing. */
int nt_open_shared_folder(const char *directory, const char *home);

/*
 * Persistent, explicitly authorized host-folder bindings for Linux.
 * Names must be an allowlist (Desktop, Documents, ...). Paths are original
 * Linux directories, not copied data. Values returned by load are allocated
 * and must be released with nt_guest_share_free.
 * No configuration is treated as an empty mapping set.
 */
int nt_guest_share_load(const char *config_home, const char *home,
                        const char *const *names, size_t count, char **paths);
int nt_guest_share_update(const char *config_home, const char *home,
                          const char *const *names, size_t count,
                          const char *folder, const char *directory);
void nt_guest_share_free(char **paths, size_t count);

#endif
