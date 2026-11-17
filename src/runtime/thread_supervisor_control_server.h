/* SPDX-License-Identifier: MIT */
#ifndef NT_SUPERVISOR_CONTROL_SERVER_H
#define NT_SUPERVISOR_CONTROL_SERVER_H
#include "thread_broker_auth.h"
#include "thread_broker_ipc.h"
#include "nt_services.h"
#include <stdint.h>
#include <stdbool.h>

/* One owner thread per authenticated SOCK_SEQPACKET server. No supervisor
 * state is shared between independent children and no PE pointer is used
 * as a host address. All external TIDs are resolved via the pinned
 * registry before reaching this contract. */
struct nt_supervisor_control_server;
struct nt_supervisor_control_server *nt_supervisor_control_server_create(void);
void nt_supervisor_control_server_revoke_tid(
    struct nt_supervisor_control_server *, uint32_t tid);
void nt_supervisor_control_server_destroy(
    struct nt_supervisor_control_server *);
nt_status nt_supervisor_control_server_call(
    struct nt_supervisor_control_server *,
    const struct nt_thread_broker_registered_peer *,
    const struct nt_broker_thread_info *,
    uint32_t operation, uint32_t context_flags,
    const struct nt_context64 *input,
    struct nt_context64 *output, uint32_t *previous);
#endif
