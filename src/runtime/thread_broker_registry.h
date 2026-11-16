/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_THREAD_BROKER_REGISTRY_H
#define NTNATIVE_THREAD_BROKER_REGISTRY_H

/* HOST-ONLY, limited child initial-thread registry. Never expose these
 * capabilities as Win32 HANDLEs or accept Linux TIDs from guest code.
 * The parent registers only a child it spawned and holds a pidfd for.
 * A process can observe another registered guest's MAIN thread if the
 * parent explicitly grants query rights. No host scheduler mutations. */
#include "thread_broker_auth.h"
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct nt_broker_thread_info {
    uint32_t guest_process_id;
    uint32_t guest_thread_id;
    uint64_t generation;
};

/* All calls are thread-safe. Registration is host-only, from a trusted
 * launcher; pidfd MUST refer to that precise child. No guest-supplied
 * PID/TID can become a registered process. Registers initial TID == PID;
 * additional threads require a future authenticated child protocol. */
bool nt_broker_register_child(pid_t pid, int pidfd, uid_t uid, gid_t gid,
                              const uint8_t secret[16], uint64_t *generation);
void nt_broker_unregister_child(pid_t pid, uint64_t generation);
/* Host-only: rekey immediately after trusted posix_spawn, before the
 * broker sends its startup handshake and before any guest code runs. */
bool nt_broker_install_child_session(pid_t pid, const uint8_t secret[16]);

/* Secondary guest thread life events can only be forwarded by the authenticated
 * owner via the AF_UNIX broker. task identities are pinned by /proc task
 * directory inode and rechecked against TID reuse, never mutable host PIDs. */
bool nt_broker_register_guest_thread(pid_t owner, pid_t tid);
void nt_broker_unregister_guest_thread(pid_t owner, pid_t tid);

/* Resolve any registered live thread (initial or authenticated secondary),
 * with the target's owner PID and generation recorded in the handle.
 * Only query capabilities are ever allowed. */
bool nt_broker_open_registered_thread(pid_t requester, pid_t tid, uint32_t access,
                                      uint64_t *opaque_handle);

/* An explicitly registered child may query siblings from the same runtime
 * only after this parent's trusted launcher installs the grants. */
void nt_broker_grant_existing_sibling_queries(pid_t new_child);

/* Return an immutable snapshot of a live, trusted registered main thread.
 * Owner may be parent process or a peer explicitly granted query access.
 * All other target threads and raw foreign Linux PIDs fail closed. */
bool nt_broker_grant_child_query(pid_t requester, pid_t target, uint32_t access);
bool nt_broker_open_child_thread(pid_t requester, pid_t target, uint32_t access,
                                uint64_t *opaque_handle);
bool nt_broker_query_child_thread(pid_t requester, uint64_t opaque_handle,
                                  uint32_t required_access,
                                  struct nt_broker_thread_info *info);
bool nt_broker_close_child_thread(pid_t requester, uint64_t opaque_handle);

/* Actual AF_UNIX message -> kernel peer credentials/pidfd/token ->
 * registered requester -> explicitly-granted target -> handle scoped to
 * requester PID. Reads only the signed v1 query message, no raw TID.
 * Does not modify out_handle when validation fails. */
bool nt_broker_authenticated_open_child(int socket_fd,
    const struct nt_thread_broker_registered_peer *peer,
    pid_t target, uint64_t *out_handle);

#endif
