/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_THREAD_BROKER_IPC_H
#define NTNATIVE_THREAD_BROKER_IPC_H
/* v2 private host protocol. It is NOT a Windows ABI or a guest-controlled
 * ability to issue Linux syscalls. Every request checks kernel credentials,
 * registered pidfd and the session token. Only query rights are supported. */
#include "thread_broker_registry.h"
#include <stdbool.h>
#include <stdint.h>

enum { NT_BROKER_RPC_VERSION = 2, NT_BROKER_RPC_REGISTER = 1,
       NT_BROKER_RPC_UNREGISTER = 2, NT_BROKER_RPC_OPEN = 3,
       NT_BROKER_RPC_QUERY = 4, NT_BROKER_RPC_CLOSE = 5 };
struct nt_broker_rpc {
    uint32_t magic, version, operation, access;
    uint32_t thread_id, reserved;
    uint64_t capability;
    uint8_t secret[16];
};
struct nt_broker_reply {
    uint32_t magic, status, process_id, thread_id;
    uint64_t generation, capability;
};
struct nt_broker_hello {
    uint32_t magic, version;
    uint8_t secret[16];
};

/* Parent side. Send an authenticated handshake before PE startup;
 * serve in a separate runtime-owned thread until EOF/revocation. */
bool nt_broker_server_handshake(int fd, const uint8_t secret[16]);
/* Takes ownership of fd only when true; detached server owns a pidfd dup. */
bool nt_broker_server_start(int fd,
    const struct nt_thread_broker_registered_peer *peer);
void nt_broker_server_loop(int fd,
    const struct nt_thread_broker_registered_peer *peer);

/* Child side: fd8 is a private SCM_CREDENTIALS-checked UNIX seqpacket
 * endpoint inherited from the trusted launcher's posix_spawn file actions.
 * No environment variables, paths or arbitrary Linux process controls.
 * Operations fail closed on timeout/disconnect. */
bool nt_broker_client_is_attached(void);
bool nt_broker_client_attach(int fd);
void nt_broker_client_detach(void);
bool nt_broker_client_register_thread(uint32_t thread_id);
void nt_broker_client_unregister_thread(uint32_t thread_id);
bool nt_broker_client_open(uint32_t thread_id, uint32_t access,
                           struct nt_broker_thread_info *info, uint64_t *capability);
bool nt_broker_client_query(uint64_t capability, uint32_t access,
                            struct nt_broker_thread_info *info);
bool nt_broker_client_close(uint64_t capability);

#endif
