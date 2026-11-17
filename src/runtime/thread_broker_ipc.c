/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"
#include "../supervisor/nt_thread_supervisor.h"

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>

_Static_assert(sizeof(struct nt_broker_rpc) == 48, "private RPC layout");
_Static_assert(sizeof(struct nt_thread_broker_reply) == 32, "private reply layout");
_Static_assert(sizeof(struct nt_broker_hello) == 24, "private hello layout");

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
static int client_socket = -1;
static uint8_t client_secret[16];
static const uint32_t read_rights =
    NT_BROKER_QUERY_INFORMATION | NT_BROKER_QUERY_LIMITED_INFORMATION;

static bool live_child(const struct nt_thread_broker_registered_peer *peer)
{
    if (!peer || peer->child_pid <= 0 || peer->child_pidfd < 0)
        return false;
    struct pollfd p = {.fd = peer->child_pidfd, .events = POLLIN};
    return poll(&p, 1, 0) == 0 && p.revents == 0;
}
static bool fd_seqpacket(int fd)
{
    int type = 0;
    socklen_t length = sizeof(type);
    return fd >= 0 && getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) == 0 &&
           length == sizeof(type) && type == SOCK_SEQPACKET;
}
static bool read_ready(int fd, short events)
{
    struct pollfd p = {.fd = fd, .events = events};
    const int n = poll(&p, 1, 1000);
    return n == 1 && (p.revents & events) != 0;
}
static bool send_packet(int fd, const void *data, size_t bytes)
{
    return fd_seqpacket(fd) && read_ready(fd, POLLOUT) &&
           send(fd, data, bytes, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)bytes;
}

struct server_context {
    int fd;
    struct nt_thread_broker_registered_peer peer;
};
static void *server_main(void *context)
{
    struct server_context *owner = context;
    nt_broker_server_loop(owner->fd, &owner->peer);
    close(owner->fd);
    close(owner->peer.child_pidfd);
    memset(owner->peer.secret, 0, sizeof(owner->peer.secret));
    free(owner);
    return NULL;
}
bool nt_broker_server_start(int fd,
    const struct nt_thread_broker_registered_peer *peer)
{
    if (peer == NULL || !fd_seqpacket(fd) || !live_child(peer))
        return false;
    /* Must be installed BEFORE any client message can be queued, not
     * lazily by the detached recvmsg worker after a poll wakeup. */
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0)
        return false;
    struct server_context *owner = calloc(1, sizeof(*owner));
    if (owner == NULL)
        return false;
    owner->fd = fd;
    owner->peer = *peer;
    owner->peer.child_pidfd = fcntl(peer->child_pidfd, F_DUPFD_CLOEXEC, 3);
    if (owner->peer.child_pidfd < 0) {
        free(owner);
        return false;
    }
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        close(owner->peer.child_pidfd);
        free(owner);
        return false;
    }
    const int detached = pthread_attr_setdetachstate(&attributes,
                                                      PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int error = detached == 0 ? pthread_create(&thread, &attributes,
                                                     server_main, owner) : detached;
    pthread_attr_destroy(&attributes);
    if (error != 0) {
        close(owner->peer.child_pidfd);
        free(owner);
        return false;
    }
    return true;
}

bool nt_broker_server_handshake(int fd, const uint8_t secret[16])
{
    if (secret == NULL)
        return false;
    const struct nt_broker_hello hello = {
        .magic = NT_THREAD_BROKER_MAGIC, .version = NT_BROKER_RPC_VERSION,
        .secret = {0}
    };
    struct nt_broker_hello message = hello;
    memcpy(message.secret, secret, 16);
    return send_packet(fd, &message, sizeof(message));
}

static bool receive_authenticated_rpc(int fd,
    const struct nt_thread_broker_registered_peer *peer,
    struct nt_broker_rpc *out)
{
    if (!live_child(peer) || !fd_seqpacket(fd))
        return false;
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0 ||
        !read_ready(fd, POLLIN))
        return false;
    struct nt_broker_rpc request = {0};
    struct iovec body = {.iov_base = &request, .iov_len = sizeof(request)};
    union {
        struct cmsghdr align;
        char storage[CMSG_SPACE(sizeof(struct ucred))];
    } ancillary = {0};
    struct msghdr message = {
        .msg_iov = &body, .msg_iovlen = 1,
        .msg_control = ancillary.storage,
        .msg_controllen = sizeof(ancillary.storage)
    };
    const ssize_t read_bytes = recvmsg(fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    if (read_bytes != (ssize_t)sizeof(request) ||
        (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
        return false;
    unsigned credential_count = 0;
    struct ucred sender = {0};
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
         cmsg != NULL; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_CREDENTIALS ||
            cmsg->cmsg_len != CMSG_LEN(sizeof(struct ucred)))
            return false;
        if (++credential_count != 1)
            return false;
        memcpy(&sender, CMSG_DATA(cmsg), sizeof(sender));
    }
    uint8_t mismatch = 0;
    for (unsigned i = 0; i < sizeof(request.secret); ++i)
        mismatch |= (uint8_t)(request.secret[i] ^ peer->secret[i]);
    if (credential_count != 1 || sender.pid != peer->child_pid ||
        sender.uid != peer->uid || sender.gid != peer->gid ||
        mismatch != 0 || !live_child(peer) ||
        request.magic != NT_THREAD_BROKER_MAGIC ||
        request.version != NT_BROKER_RPC_VERSION || request.reserved != 0)
        return false;
    *out = request;
    return true;
}

/* Optional proof that the authenticated control-plane reaches a REAL
 * external Linux thread supervisor. The original tracee always resumes,
 * and this is disabled for production unless the trusted parent grants the
 * explicit server-side test flag; no PE code can set that grant by RPC.
 * Never use a TID until the child-owned generation-pinned registry has
 * resolved the capability. */
static bool supervisor_readonly_probe(const struct nt_thread_broker_registered_peer *peer,
                                      const struct nt_broker_thread_info *identity,
                                      uint64_t *guest_rip)
{
    if (!peer || !identity || !guest_rip ||
        identity->guest_process_id != (uint32_t)peer->child_pid ||
        identity->guest_thread_id == 0)
        return false;
    struct nt_thread_supervisor supervisor;
    nt_supervisor_init(&supervisor);
    bool okay = false;
    if (nt_supervisor_seize_child(&supervisor, peer->child_pid,
                                  (pid_t)identity->guest_thread_id))
        return false;
    struct user_regs_struct context;
    if (nt_supervisor_interrupt(&supervisor, 500) == 0 &&
        nt_supervisor_get_registers(&supervisor, &context) == 0 &&
        context.rip != 0) {
        *guest_rip = context.rip; /* caller returns it only on full success */
        okay = true;
    }
    /* A verified detach must be possible even on a failed read. If a
     * ptrace failure prevents it, kill ONLY the exact pidfd-pinned child
     * rather than abandon a tracee that can remain permanently stopped. */
    if (!supervisor.stopped && supervisor.seized)
        (void)nt_supervisor_interrupt(&supervisor, 500);
    if (supervisor.seized &&
        (!supervisor.stopped || nt_supervisor_detach(&supervisor) != 0)) {
#if defined(SYS_pidfd_send_signal)
        (void)syscall(SYS_pidfd_send_signal, peer->child_pidfd, SIGKILL, NULL, 0);
#endif
        okay = false;
    }
    nt_supervisor_release(&supervisor);
    return okay;
}

void nt_broker_server_loop(int fd,
    const struct nt_thread_broker_registered_peer *peer)
{
    if (peer == NULL || !fd_seqpacket(fd))
        return;
    for (;;) {
        if (!live_child(peer))
            break;
        struct pollfd active = {.fd = fd, .events = POLLIN};
        const int available = poll(&active, 1, 250);
        if (available == 0 || (available < 0 && errno == EINTR))
            continue; /* Idle channels stay usable across arbitrary waits. */
        if (available < 0 || !(active.revents & POLLIN))
            break;
        struct nt_broker_rpc request = {0};
        if (!receive_authenticated_rpc(fd, peer, &request))
            break; /* Fail closed on disconnect, bad protocol or impersonation. */
        struct nt_thread_broker_reply response = {
            .magic = NT_THREAD_BROKER_MAGIC, .status = 1
        };
        const pid_t requester = peer->child_pid;
        struct nt_broker_thread_info info = {0};
        uint64_t handle = 0;
        switch (request.operation) {
        case NT_BROKER_RPC_REGISTER:
            if (request.access == 0 && request.capability == 0 &&
                nt_broker_register_guest_thread(requester,
                                               (pid_t)request.thread_id))
                response.status = 0;
            break;
        case NT_BROKER_RPC_UNREGISTER:
            if (request.access == 0 && request.capability == 0) {
                nt_broker_unregister_guest_thread(requester,
                                                 (pid_t)request.thread_id);
                response.status = 0;
            }
            break;
        case NT_BROKER_RPC_OPEN:
            if ((request.access & ~read_rights) == 0 &&
                request.access != 0 && request.capability == 0 &&
                nt_broker_open_registered_thread(requester,
                         (pid_t)request.thread_id, request.access, &handle)) {
                if (nt_broker_query_child_thread(requester, handle, request.access,
                                                 &info)) {
                    response.status = 0;
                    response.capability = handle;
                    response.process_id = info.guest_process_id;
                    response.thread_id = info.guest_thread_id;
                    response.generation = info.generation;
                } else {
                    (void)nt_broker_close_child_thread(requester, handle);
                }
            }
            break;
        case NT_BROKER_RPC_QUERY:
            if (request.thread_id == 0 &&
                request.access != 0 &&
                (request.access & ~read_rights) == 0 &&
                nt_broker_query_child_thread(requester, request.capability,
                                             request.access, &info)) {
                response.status = 0;
                response.process_id = info.guest_process_id;
                response.thread_id = info.guest_thread_id;
                response.generation = info.generation;
            }
            break;
        case NT_BROKER_RPC_CLOSE:
            if (request.access == 0 && request.thread_id == 0 &&
                nt_broker_close_child_thread(requester, request.capability))
                response.status = 0;
            break;
        case NT_BROKER_RPC_SUP_OPEN:
            /* Owner-only: never grant privileges over sibling child or
             * arbitrary Linux TIDs, even when the request contains a
             * valid authenticated session secret. */
            if (request.thread_id != 0 && request.capability == 0 &&
                nt_broker_supervisor_open_owned(requester,
                    (pid_t)request.thread_id, request.access, &handle, &info)) {
                response.status = 0;
                response.capability = handle;
                response.process_id = info.guest_process_id;
                response.thread_id = info.guest_thread_id;
                response.generation = info.generation;
            }
            break;
        case NT_BROKER_RPC_SUP_QUERY:
            if (request.thread_id == 0 &&
                nt_broker_supervisor_query_owned(requester, request.capability,
                                                  request.access, &info)) {
                response.status = 0;
                response.process_id = info.guest_process_id;
                response.thread_id = info.guest_thread_id;
                response.generation = info.generation;
            }
            break;
        case NT_BROKER_RPC_SUP_CLOSE:
            if (request.thread_id == 0 && request.access == 0 &&
                nt_broker_supervisor_close_owned(requester, request.capability))
                response.status = 0;
            break;
        case NT_BROKER_RPC_SUP_PROBE:
            /* Optional test-only, never enabled by the guest's own RPC.
             * Check GET_CONTEXT capability + owner + generation BEFORE
             * making any ptrace call. No lasting suspension or writes. */
            if (request.thread_id == 0 && request.access ==
                    NT_BROKER_SUPERVISOR_GET_CONTEXT &&
                (peer->allowed_access & NT_BROKER_SERVER_ENABLE_PTRACE_PROBE) != 0 &&
                nt_broker_supervisor_query_owned(requester, request.capability,
                                                  request.access, &info)) {
                uint64_t instruction = 0;
                if (supervisor_readonly_probe(peer, &info, &instruction)) {
                    response.status = 0;
                    response.process_id = info.guest_process_id;
                    response.thread_id = info.guest_thread_id;
                    response.generation = info.generation;
                    response.capability = instruction;
                }
            }
            break;
        default:
            break;
        }
        if (!send_packet(fd, &response, sizeof(response)))
            break;
    }
    /* The channel itself is the authority, not just the integer PID.
     * Revocation is immediate on EOF/auth failure, before child reaping. */
    nt_broker_revoke_requester(peer->child_pid);
}

bool nt_broker_client_is_attached(void)
{
    pthread_mutex_lock(&client_lock);
    const bool attached = client_socket >= 0;
    pthread_mutex_unlock(&client_lock);
    return attached;
}

bool nt_broker_client_attach(int fd)
{
    if (!fd_seqpacket(fd) || !read_ready(fd, POLLIN))
        return false;
    struct ucred parent = {0};
    socklen_t length = sizeof(parent);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &parent, &length) != 0 ||
        length != sizeof(parent) || parent.pid != getppid() ||
        parent.uid != getuid())
        return false;
    struct nt_broker_hello hello = {0};
    if (recv(fd, &hello, sizeof(hello), MSG_DONTWAIT) !=
            (ssize_t)sizeof(hello) ||
        hello.magic != NT_THREAD_BROKER_MAGIC ||
        hello.version != NT_BROKER_RPC_VERSION)
        return false;
    pthread_mutex_lock(&client_lock);
    bool okay = client_socket < 0;
    if (okay) {
        client_socket = fd;
        memcpy(client_secret, hello.secret, 16);
    }
    pthread_mutex_unlock(&client_lock);
    return okay;
}

void nt_broker_client_detach(void)
{
    pthread_mutex_lock(&client_lock);
    if (client_socket >= 0) {
        close(client_socket);
        client_socket = -1;
    }
    memset(client_secret, 0, sizeof(client_secret));
    pthread_mutex_unlock(&client_lock);
}

static bool client_rpc(uint32_t operation, uint32_t tid, uint32_t access,
                       uint64_t capability, struct nt_thread_broker_reply *answer)
{
    pthread_mutex_lock(&client_lock);
    bool okay = client_socket >= 0;
    if (okay) {
        struct nt_broker_rpc request = {
            .magic = NT_THREAD_BROKER_MAGIC, .version = NT_BROKER_RPC_VERSION,
            .operation = operation, .access = access,
            .thread_id = tid, .capability = capability
        };
        memcpy(request.secret, client_secret, 16);
        okay = send_packet(client_socket, &request, sizeof(request)) &&
               read_ready(client_socket, POLLIN);
        if (okay) {
            struct nt_thread_broker_reply response = {0};
            okay = recv(client_socket, &response, sizeof(response), MSG_DONTWAIT) ==
                       (ssize_t)sizeof(response) &&
                   response.magic == NT_THREAD_BROKER_MAGIC && response.status == 0;
            if (okay && answer != NULL)
                *answer = response;
        }
    }
    pthread_mutex_unlock(&client_lock);
    return okay;
}

bool nt_broker_client_register_thread(uint32_t tid)
{
    return tid != 0 && client_rpc(NT_BROKER_RPC_REGISTER, tid, 0, 0, NULL);
}
void nt_broker_client_unregister_thread(uint32_t tid)
{
    if (tid != 0)
        (void)client_rpc(NT_BROKER_RPC_UNREGISTER, tid, 0, 0, NULL);
}
bool nt_broker_client_open(uint32_t tid, uint32_t access,
                           struct nt_broker_thread_info *info, uint64_t *capability)
{
    if (tid == 0 || access == 0 || (access & ~read_rights) != 0 ||
        info == NULL || capability == NULL)
        return false;
    struct nt_thread_broker_reply response = {0};
    if (!client_rpc(NT_BROKER_RPC_OPEN, tid, access, 0, &response) ||
        response.thread_id != tid || response.capability == 0)
        return false;
    *info = (struct nt_broker_thread_info){
        .guest_process_id = response.process_id,
        .guest_thread_id = response.thread_id,
        .generation = response.generation
    };
    *capability = response.capability;
    return true;
}
bool nt_broker_client_query(uint64_t capability, uint32_t access,
                            struct nt_broker_thread_info *info)
{
    if (capability == 0 || info == NULL)
        return false;
    struct nt_thread_broker_reply response = {0};
    if (!client_rpc(NT_BROKER_RPC_QUERY, 0, access, capability, &response))
        return false;
    *info = (struct nt_broker_thread_info){
        .guest_process_id = response.process_id,
        .guest_thread_id = response.thread_id,
        .generation = response.generation
    };
    return true;
}
bool nt_broker_client_close(uint64_t capability)
{
    return capability != 0 &&
           client_rpc(NT_BROKER_RPC_CLOSE, 0, 0, capability, NULL);
}

bool nt_broker_client_supervisor_open(uint32_t thread_id, uint32_t access,
    struct nt_broker_thread_info *identity, uint64_t *capability)
{
    if (thread_id == 0 || access == 0 ||
        (access & ~NT_BROKER_SUPERVISOR_ACCESS_MASK) != 0 ||
        !identity || !capability)
        return false;
    struct nt_thread_broker_reply reply = {0};
    if (!client_rpc(NT_BROKER_RPC_SUP_OPEN, thread_id, access, 0, &reply) ||
        reply.capability == 0 || reply.process_id != (uint32_t)getpid() ||
        reply.thread_id != thread_id || reply.generation == 0)
        return false;
    *identity = (struct nt_broker_thread_info){
        .guest_process_id = reply.process_id,
        .guest_thread_id = reply.thread_id,
        .generation = reply.generation
    };
    *capability = reply.capability;
    return true;
}
bool nt_broker_client_supervisor_query(uint64_t capability, uint32_t access,
    struct nt_broker_thread_info *identity)
{
    if (capability == 0 || access == 0 ||
        (access & ~NT_BROKER_SUPERVISOR_ACCESS_MASK) != 0 || !identity)
        return false;
    struct nt_thread_broker_reply reply = {0};
    if (!client_rpc(NT_BROKER_RPC_SUP_QUERY, 0, access, capability, &reply) ||
        reply.process_id != (uint32_t)getpid() || reply.thread_id == 0 ||
        reply.generation == 0)
        return false;
    *identity = (struct nt_broker_thread_info){
        .guest_process_id = reply.process_id,
        .guest_thread_id = reply.thread_id,
        .generation = reply.generation
    };
    return true;
}
bool nt_broker_client_supervisor_probe(uint64_t capability, uint64_t *guest_rip)
{
    if (capability == 0 || !guest_rip)
        return false;
    struct nt_thread_broker_reply reply = {0};
    if (!client_rpc(NT_BROKER_RPC_SUP_PROBE, 0,
                    NT_BROKER_SUPERVISOR_GET_CONTEXT, capability, &reply) ||
        reply.process_id != (uint32_t)getpid() || reply.thread_id == 0 ||
        reply.generation == 0 || reply.capability == 0)
        return false;
    *guest_rip = reply.capability;
    return true;
}
bool nt_broker_client_supervisor_close(uint64_t capability)
{
    return capability != 0 &&
        client_rpc(NT_BROKER_RPC_SUP_CLOSE, 0, 0, capability, NULL);
}
