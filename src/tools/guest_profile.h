/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GUEST_PROFILE_H
#define NTNATIVE_GUEST_PROFILE_H

/* Open a private persistent C: drive beneath the user's state directory.
 * An absolute state_home takes precedence over home/.local/state.
 * The caller owns the returned close-on-exec directory descriptor.
 */
int nt_open_guest_profile(const char *state_home, const char *home);

#endif
