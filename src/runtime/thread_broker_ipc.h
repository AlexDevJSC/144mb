/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_THREAD_BROKER_IPC_H
#define NTNATIVE_THREAD_BROKER_IPC_H
/* v4 private host protocol. It is NOT a Windows ABI or a guest-controlled
 * ability to issue Linux syscalls. Every request checks kernel credentials,
 * registered pidfd and the session token. Thread mutations require a
 * separate explicit HOST-only control grant and verified guest scope. */
#include "thread_broker_registry.h"
#include "apc_parent_ledger.h"
#include <stdbool.h>
#include <stdint.h>

enum { NT_BROKER_RPC_VERSION = 4, NT_BROKER_RPC_REGISTER = 1,
       NT_BROKER_RPC_UNREGISTER = 2, NT_BROKER_RPC_OPEN = 3,
       NT_BROKER_RPC_QUERY = 4, NT_BROKER_RPC_CLOSE = 5,
       NT_BROKER_RPC_SUP_OPEN = 6, NT_BROKER_RPC_SUP_QUERY = 7,
       NT_BROKER_RPC_SUP_CLOSE = 8, NT_BROKER_RPC_SUP_PROBE = 9,
       NT_BROKER_RPC_SUP_PROBE_GUEST = 10,
       NT_BROKER_RPC_SUP_SUSPEND = 11, NT_BROKER_RPC_SUP_RESUME = 12,
       NT_BROKER_RPC_SUP_GET_CONTEXT = 13, NT_BROKER_RPC_SUP_SET_CONTEXT = 14,
       NT_BROKER_RPC_SUP_APC_AUTHORIZE = 15,
       NT_BROKER_SERVER_ENABLE_PTRACE_PROBE = 0x10000000u,
       NT_BROKER_SERVER_ENABLE_THREAD_CONTROL = 0x20000000u };
struct nt_broker_rpc {
    uint32_t magic, version, operation, access;
    uint32_t thread_id, reserved;
    uint64_t capability;
    uint8_t secret[16];
};
struct nt_thread_broker_reply {
    uint32_t magic, status, process_id, thread_id;
    uint64_t generation, capability;
};
/* Fixed 16-byte aligned CONTEXT payloads used only by SET and GET.
 * Other RPCs preserve their 48/32-byte frames. No guest-pointer
 * dereference is performed in the supervisor. NTSTATUS travels in the
 * reply.status field as an unsigned 32-bit bit pattern. */
#include "compat.h"
#include "ntnative/counter.h"
struct nt_broker_context_request {
    struct nt_broker_rpc request;
    struct nt_context64 context;
};
struct nt_broker_context_reply {
    struct nt_thread_broker_reply reply;
    struct nt_context64 context;
};
/* Private parent-owned APC authorization: callback and generation must
 * come from a live, SET_CONTEXT-scoped supervisor lease. There are no
 * raw host TIDs or guest-supplied stack pointers in this operation. */
struct nt_broker_apc_request {
    struct nt_broker_rpc request;
    uint64_t generation;
    uint64_t callback;
    uint64_t ticket;
    uint32_t phase; /* 0 authorize; 1 START; 2 DONE; 3 ABORT */
    uint32_t reserved;
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
/* Trusted-runtime emergency cancellation of a request already executing
 * against the authenticated parent broker. Thread-safe even while another
 * thread holds the serial RPC mutex. Shutdowns the session, never a raw
 * process/thread or guest-provided FD. False if no RPC is in flight.
 * Cancellation is irrevocable: all leases become stale; no partial
 * mutation result or late ACK may be exposed as success. */
bool nt_broker_client_abort_inflight(void);
bool nt_broker_client_register_thread(uint32_t thread_id);
void nt_broker_client_unregister_thread(uint32_t thread_id);
bool nt_broker_client_open(uint32_t thread_id, uint32_t access,
                           struct nt_broker_thread_info *info, uint64_t *capability);
bool nt_broker_client_query(uint64_t capability, uint32_t access,
                            struct nt_broker_thread_info *info);
bool nt_broker_client_close(uint64_t capability);

/* PRIVATE runtime→parent-supervisor control plane, using the existing
 * per-message authenticated FD8. OPEN binds only to the owner's own
 * kernel-registered thread and mints an unforgeable-in-practice opaque
 * lease with generation. QUERY and CLOSE do not interpret raw TIDs.
 *
 * PROBE is a deliberately opt-in, one-shot parent ptrace GETREGSET test.
 * The trusted parent must grant ENABLE_PTRACE_PROBE. It is not a Win32
 * SuspendThread implementation, never persists a stopped TID, and cannot
 * be called for another child's registered thread. */
bool nt_broker_client_supervisor_open(uint32_t thread_id, uint32_t access,
    struct nt_broker_thread_info *identity, uint64_t *capability);
bool nt_broker_client_supervisor_query(uint64_t capability, uint32_t access,
    struct nt_broker_thread_info *identity);
bool nt_broker_client_supervisor_probe(uint64_t capability, uint64_t *guest_rip);
/* Gated probe: same authenticated lease, additionally requires per-TID HOST/GUEST
 * phase, parent-trusted PE executable range and real GS/TEB verification. */
bool nt_broker_client_supervisor_probe_guest(uint64_t capability, uint64_t *guest_rip);
/* Host-gated control operations. Full NTSTATUS is returned on failure,
 * without writing output arguments. CONTROL/INTEGER/SEGMENTS/FXSAVE
 * reads and INTEGER-only writes are implemented; unsupported groups fail. */
nt_status nt_broker_client_supervisor_suspend(uint64_t capability, uint32_t *previous);
nt_status nt_broker_client_supervisor_resume(uint64_t capability, uint32_t *previous);
nt_status nt_broker_client_supervisor_get_context(uint64_t capability,
                                                   struct nt_context64 *context);
nt_status nt_broker_client_supervisor_set_context(uint64_t capability,
                                                   const struct nt_context64 *context);
/* Attests loader-pinned code + generation-pinned TEB in the parent.
 * This is an authorization receipt, not a claim of ptrace delivery. */
nt_status nt_broker_client_supervisor_authorize_apc(
    uint64_t capability, uint64_t generation, uintptr_t callback);
/* Parent-owned lifecycle acknowledgement. This requires an existing
 * SET_CONTEXT lease for the exact generation, and does not grant ptrace
 * execution or arbitrary callback stack ownership. */
nt_status nt_broker_client_supervisor_apc_transition(
    uint64_t capability, uint64_t generation, uintptr_t callback,
    uint64_t ticket, uint32_t phase);
bool nt_broker_client_supervisor_close(uint64_t capability);


#endif
