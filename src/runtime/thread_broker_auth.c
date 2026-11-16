/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_broker_auth.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert(sizeof(struct nt_thread_broker_request) == 32,
               "internal broker protocol wire size");

int nt_thread_broker_generate_secret(uint8_t output[16])
{
    if (output == NULL)
        return 0;
    size_t done = 0;
    while (done < 16) {
        const ssize_t count = getrandom(output + done, 16 - done, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            memset(output, 0, 16);
            return 0; /* no predictable or time-based fallback */
        }
        done += (size_t)count;
    }
    return 1;
}

static int live_registered_child(const struct nt_thread_broker_registered_peer *peer)
{
    if (peer == NULL || peer->child_pid <= 0 ||
        peer->child_pid == getpid() || peer->child_pidfd < 0)
        return 0;
    struct pollfd child = {.fd = peer->child_pidfd, .events = POLLIN};
    const int result = poll(&child, 1, 0);
    return result == 0 && child.revents == 0;
}

/* Fixed-size seqpacket transport avoids framing ambiguities. SO_PASSCRED
 * records the actual sender's kernel credentials on EACH message, unlike
 * SO_PEERCRED on socketpair() created before fork(), which records the
 * creator's credentials and would therefore misidentify a forked child. */
enum nt_thread_broker_auth_result
nt_thread_broker_receive_query(int socket_fd,
    const struct nt_thread_broker_registered_peer *peer, uint32_t *granted)
{
    if (socket_fd < 0 || granted == NULL || !live_registered_child(peer))
        return NT_THREAD_BROKER_UNAVAILABLE;
    int type = 0;
    socklen_t type_len = sizeof(type);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0 ||
        type_len != sizeof(type) || type != SOCK_SEQPACKET)
        return NT_THREAD_BROKER_UNAVAILABLE;
    const int on = 1;
    if (setsockopt(socket_fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0)
        return NT_THREAD_BROKER_UNAVAILABLE;

    /* Every operation is bounded, even if a guest abandons its peer FD. */
    struct pollfd available = {.fd = socket_fd, .events = POLLIN};
    if (poll(&available, 1, 1000) != 1 ||
        !(available.revents & POLLIN))
        return NT_THREAD_BROKER_UNAVAILABLE;
    struct nt_thread_broker_request packet = {0};
    struct iovec iov = {.iov_base = &packet, .iov_len = sizeof(packet)};
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(struct ucred))];
    } ancillary = {0};
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ancillary.bytes,
        .msg_controllen = sizeof(ancillary.bytes)
    };
    const ssize_t bytes = recvmsg(socket_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (bytes != (ssize_t)sizeof(packet) ||
        (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
        return NT_THREAD_BROKER_BAD_PACKET;

    struct ucred credential = {0};
    unsigned credential_count = 0;
    for (struct cmsghdr *item = CMSG_FIRSTHDR(&msg);
         item != NULL; item = CMSG_NXTHDR(&msg, item)) {
        if (item->cmsg_level != SOL_SOCKET ||
            item->cmsg_type != SCM_CREDENTIALS ||
            item->cmsg_len != CMSG_LEN(sizeof(struct ucred)))
            return NT_THREAD_BROKER_DENIED;
        if (++credential_count != 1)
            return NT_THREAD_BROKER_DENIED;
        memcpy(&credential, CMSG_DATA(item), sizeof(credential));
    }
    if (credential_count != 1 ||
        credential.pid != peer->child_pid ||
        credential.uid != peer->uid ||
        credential.gid != peer->gid ||
        !live_registered_child(peer))
        return NT_THREAD_BROKER_DENIED;

    /* A process that inherited the FD but is not the registered child
     * remains forbidden, even if it knows the secret. */
    volatile uint8_t mismatch = 0;
    for (size_t i = 0; i < 16; ++i)
        mismatch |= (uint8_t)(packet.secret[i] ^ peer->secret[i]);
    if (mismatch != 0)
        return NT_THREAD_BROKER_DENIED;

    if (packet.magic != NT_BROKER_MAGIC ||
        packet.version != NT_BROKER_VERSION)
        return NT_THREAD_BROKER_BAD_PACKET;
    if (packet.operation != NT_BROKER_QUERY_IDENTITY ||
        packet.requested_access == 0 ||
        (packet.requested_access &
         ~(NT_BROKER_QUERY_INFORMATION | NT_BROKER_QUERY_LIMITED_INFORMATION)) != 0)
        return NT_THREAD_BROKER_DENIED;
    if ((packet.requested_access & ~peer->allowed_access) != 0 ||
        (peer->allowed_access &
         ~(NT_BROKER_QUERY_INFORMATION | NT_BROKER_QUERY_LIMITED_INFORMATION)) != 0)
        return NT_THREAD_BROKER_DENIED;

    /* This is intentionally only a transport identity check. No HANDLE,
     * Linux TID, signal or nice value is produced or mutated. A future
     * object registry must mint distinct capabilities for guest threads. */
    *granted = packet.requested_access;
    return NT_THREAD_BROKER_OK;
}
