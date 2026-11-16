/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_OBJECT_BROKER_H
#define NTNATIVE_OBJECT_BROKER_H

/* Private host IPC protocol: it is not a Windows ABI. The Unix socket is
 * a per-user object namespace; SCM_RIGHTS carries pinned memfd objects.
 * A live socket is the object's lease, independent from the guest HANDLE. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    NT_BROKER_MAGIC = 0x314b5242u, NT_BROKER_VERSION = 1,
    NT_BROKER_NAME_BYTES = 256,
    NT_BROKER_SECTION = 1, NT_BROKER_EVENT = 2, NT_BROKER_SEMAPHORE = 3,
    NT_BROKER_CREATE = 1, NT_BROKER_OPEN = 2,
    NT_BROKER_CREATED = 0, NT_BROKER_EXISTS = 1,
    NT_BROKER_MISSING = 2, NT_BROKER_TYPE_MISMATCH = 3,
    NT_BROKER_ACCESS_DENIED = 4, NT_BROKER_RESOURCE_ERROR = 5,
    NT_BROKER_INVALID = 6,
    NT_BROKER_DISABLED = -1, NT_BROKER_TRANSPORT_ERROR = -2
};

struct nt_broker_request {
    uint32_t magic, version, action, kind;
    uint32_t rights, flags, protect, initial, maximum;
    uint64_t size;
    char name[NT_BROKER_NAME_BYTES]; /* bounded UTF-8, NUL terminated */
};

struct nt_broker_reply {
    uint32_t magic, version, result, kind;
    uint32_t rights, protect, manual, maximum;
    uint64_t size;
};

struct nt_broker_sync_state {
    int32_t count;
    int32_t maximum;
    uint32_t kind;
    uint32_t manual;
};

struct nt_broker_gate {
    pthread_mutex_t mutex;
};

struct nt_broker_resource {
    int fd, gate_fd, lease_fd;
    void *state, *gate;
    uint32_t kind, rights, protect;
    uint64_t size;
};

/* Returns DISABLED only if the broker was not explicitly requested; if the
 * configured socket is missing or hostile, operations FAIL CLOSED. */
bool nt_runtime_broker_enabled(void);
int nt_broker_acquire(const struct nt_broker_request *request,
                      struct nt_broker_resource *resource);
void nt_broker_release(struct nt_broker_resource *resource);
bool nt_broker_lock(const struct nt_broker_resource *resource);
void nt_broker_unlock(const struct nt_broker_resource *resource);

#endif
