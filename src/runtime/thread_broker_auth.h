/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_THREAD_BROKER_AUTH_H
#define NTNATIVE_THREAD_BROKER_AUTH_H

/* Internal Linux-only transport. Not a guest-facing NT HANDLE interface.
 * A registered child has an immutable kernel pidfd and an authenticated
 * AF_UNIX SOCK_SEQPACKET channel. Never accept Linux PIDs/TIDs as guest
 * object identifiers or authorize a host scheduling mutation here. */
#include <stdint.h>
#include <sys/types.h>
#include "ntnative/guest_boundary_runtime.h"

enum {
    NT_BROKER_QUERY_INFORMATION = 0x0040u,
    NT_BROKER_QUERY_LIMITED_INFORMATION = 0x0800u,
    NT_THREAD_BROKER_MAGIC = 0x3142544eu, /* "NTB1" little endian */
    NT_THREAD_BROKER_VERSION = 1,
    NT_BROKER_QUERY_IDENTITY = 1
};

struct nt_thread_broker_request {
    uint32_t magic;
    uint32_t version;
    uint32_t operation;
    uint32_t requested_access;
    uint8_t secret[16];
};

struct nt_guest_boundary_session;
struct nt_thread_broker_registered_peer {
    pid_t child_pid;     /* from trusted fork/spawn, NOT guest input */
    uid_t uid;
    gid_t gid;
    int child_pidfd;     /* stable kernel identity, must remain live */
    uint8_t secret[16];  /* from getrandom, passed over trusted startup */
    uint32_t allowed_access; /* query-only in this stage */
    /* Optional trusted direct-parent guest boundary; not supplied by the
     * PE RPC. Server takes ownership of the MAP_SHARED mapping on start.
     * NULL for legacy read-only child broker channels. */
    struct nt_guest_boundary_session *guest_session;
    uintptr_t trusted_code_begin, trusted_code_end;
    /* Copied from the trusted parent's PE loader before fork. This list
     * contains ONLY pinned startup DLL executable code and never PE RPC
     * input or a pointer into mutable guest storage. */
    struct nt_guest_code_range trusted_startup_dlls[NT_GUEST_ADDITIONAL_CODE_MAX];
    size_t trusted_startup_dll_count;
};

enum nt_thread_broker_auth_result {
    NT_THREAD_BROKER_OK = 0,
    NT_THREAD_BROKER_DENIED,
    NT_THREAD_BROKER_BAD_PACKET,
    NT_THREAD_BROKER_UNAVAILABLE
};

int nt_thread_broker_generate_secret(uint8_t output[16]);

/* Bounded receive. A socket peer claiming a PID inside a packet does not
 * authenticate anything; kernel SCM_CREDENTIALS and pidfd must agree.
 * Returns only validated query rights; does not query/change host TIDs.
 * 'granted' is NOT written on failure. */
enum nt_thread_broker_auth_result
nt_thread_broker_receive_query(int socket_fd,
    const struct nt_thread_broker_registered_peer *peer, uint32_t *granted);

#endif
