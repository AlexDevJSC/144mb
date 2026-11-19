/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "object_broker.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

bool nt_runtime_broker_enabled(void)
{
    const char *path = getenv("NT_NATIVE_BROKER_SOCKET");
    return path && *path;
}

static bool valid_name(const char *name)
{
    size_t n = strnlen(name, NT_BROKER_NAME_BYTES);
    return n > 0 && n < NT_BROKER_NAME_BYTES;
}

void nt_broker_release(struct nt_broker_resource *resource)
{
    if (!resource)
        return;
    if (resource->state)
        munmap(resource->state, 4096);
    if (resource->gate)
        munmap(resource->gate, 4096);
    if (resource->fd >= 0)
        close(resource->fd);
    if (resource->gate_fd >= 0)
        close(resource->gate_fd);
    if (resource->lease_fd >= 0)
        close(resource->lease_fd);
    *resource = (struct nt_broker_resource){
        .fd = -1, .gate_fd = -1, .lease_fd = -1
    };
}

/* recvmsg installs SCM_RIGHTS FDs even when the payload is short,
 * malformed, or MSG_CTRUNC is set. All such FDs belong to the receiver.
 * Walk the kernel-returned cmsgs before rejecting the reply so a hostile
 * same-UID peer cannot exhaust this process's descriptor table. */
static void close_received_rights(struct msghdr *message)
{
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(message); cmsg;
         cmsg = CMSG_NXTHDR(message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(sizeof(int)))
            continue;
        const size_t count =
            (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const unsigned char *data = (const unsigned char *)CMSG_DATA(cmsg);
        for (size_t i = 0; i < count; ++i) {
            int fd;
            memcpy(&fd, data + i * sizeof(int), sizeof(fd));
            if (fd >= 0)
                (void)close(fd);
        }
    }
}

static bool verify_memfd(int fd, uint64_t expected)
{
    struct stat st;
    int seals = fcntl(fd, F_GET_SEALS);
    return seals >= 0 &&
           (seals & (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) ==
               (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) &&
           fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_size == (off_t)expected;
}

int nt_broker_acquire(const struct nt_broker_request *request,
                      struct nt_broker_resource *resource)
{
    if (!resource || !request)
        return NT_BROKER_INVALID;
    *resource = (struct nt_broker_resource){
        .fd = -1, .gate_fd = -1, .lease_fd = -1
    };
    if (!nt_runtime_broker_enabled())
        return NT_BROKER_DISABLED;
    const char *path = getenv("NT_NATIVE_BROKER_SOCKET");
    if (!valid_name(request->name) ||
        request->magic != NT_BROKER_MAGIC ||
        request->version != NT_BROKER_VERSION ||
        (request->action != NT_BROKER_CREATE && request->action != NT_BROKER_OPEN) ||
        (request->kind < NT_BROKER_SECTION ||
         request->kind > NT_BROKER_SEMAPHORE))
        return NT_BROKER_INVALID;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    const size_t length = strlen(path);
    if (!length || length >= sizeof(addr.sun_path) || path[0] != '/')
        return NT_BROKER_INVALID;
    memcpy(addr.sun_path, path, length + 1);
    int sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return NT_BROKER_TRANSPORT_ERROR;
    /* A configured broker with matching SO_PEERCRED can still stall after
     * accepting a request. Never let a PE's open/create wait indefinitely
     * for that peer: this controls send/recv, not total connect time. */
    const struct timeval deadline = {.tv_sec = 2, .tv_usec = 0};
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &deadline, sizeof(deadline)) != 0 ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                   &deadline, sizeof(deadline)) != 0)
        goto error;
    socklen_t size = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1);
    if (connect(sock, (const struct sockaddr *)&addr, size) != 0)
        goto error;
    struct ucred peer;
    socklen_t peer_length = sizeof(peer);
    if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &peer, &peer_length) != 0 ||
        peer_length != sizeof(peer) || peer.uid != geteuid())
        goto error;
    if (send(sock, request, sizeof(*request), MSG_NOSIGNAL) !=
        (ssize_t)sizeof(*request))
        goto error;
    struct nt_broker_reply reply = {0};
    char ancillary[CMSG_SPACE(2 * sizeof(int))] = {0};
    struct iovec iov = {.iov_base = &reply, .iov_len = sizeof(reply)};
    struct msghdr message = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary, .msg_controllen = sizeof(ancillary)
    };
    const ssize_t received = recvmsg(sock, &message, MSG_CMSG_CLOEXEC);
    if (received != (ssize_t)sizeof(reply) ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
        reply.magic != NT_BROKER_MAGIC ||
        reply.version != NT_BROKER_VERSION ||
        reply.kind != request->kind)
        goto received_error;
    int rights[2] = {-1, -1};
    int count = 0;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(sizeof(int)))
            goto received_error;
        int incoming = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        if (count + incoming > 2)
            goto received_error;
        memcpy(rights + count, CMSG_DATA(cmsg), (size_t)incoming * sizeof(int));
        count += incoming;
    }
    if (reply.result > NT_BROKER_INVALID ||
        (reply.result > NT_BROKER_EXISTS && count != 0))
        goto received_error;
    if (reply.result != NT_BROKER_CREATED && reply.result != NT_BROKER_EXISTS) {
        close(sock);
        return (int)reply.result;
    }
    int expected_count = request->kind == NT_BROKER_SECTION ? 1 : 2;
    uint64_t expected_size = request->kind == NT_BROKER_SECTION ? reply.size : 4096;
    if (count != expected_count || !verify_memfd(rights[0], expected_size) ||
        (count == 2 && !verify_memfd(rights[1], 4096)) ||
        (reply.rights & request->rights) != request->rights)
        goto received_error;
    resource->fd = rights[0];
    resource->gate_fd = count == 2 ? rights[1] : -1;
    resource->lease_fd = sock;
    resource->kind = reply.kind;
    resource->rights = reply.rights;
    resource->protect = reply.protect;
    resource->size = reply.size;
    if (count == 2) {
        resource->state = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_SHARED, resource->fd, 0);
        if (resource->state == MAP_FAILED) {
            resource->state = NULL;
            goto resource_error;
        }
        resource->gate = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_SHARED, resource->gate_fd, 0);
        if (resource->gate == MAP_FAILED) {
            resource->gate = NULL;
            goto resource_error;
        }
        const struct nt_broker_sync_state *state = resource->state;
        if (state->kind != request->kind || state->maximum <= 0 ||
            (request->kind == NT_BROKER_EVENT && state->maximum != 1))
            goto resource_error;
    }
    return (int)reply.result;
resource_error:
    nt_broker_release(resource);
    return NT_BROKER_TRANSPORT_ERROR;
received_error:
    close_received_rights(&message);
error:
    close(sock);
    return NT_BROKER_TRANSPORT_ERROR;
}

bool nt_broker_lock(const struct nt_broker_resource *resource)
{
    if (!resource || !resource->gate)
        return false;
    struct nt_broker_gate *gate = resource->gate;
    int result = pthread_mutex_lock(&gate->mutex);
    if (result == EOWNERDEAD) {
        if (pthread_mutex_consistent(&gate->mutex) == 0)
            return true;
        (void)pthread_mutex_unlock(&gate->mutex);
    }
    return result == 0;
}

void nt_broker_unlock(const struct nt_broker_resource *resource)
{
    if (resource && resource->gate) {
        struct nt_broker_gate *gate = resource->gate;
        (void)pthread_mutex_unlock(&gate->mutex);
    }
}
