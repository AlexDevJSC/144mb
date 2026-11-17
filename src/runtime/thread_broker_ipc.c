/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"
#include "ntnative/guest_boundary_runtime.h"
#include "../supervisor/nt_thread_supervisor.h"
#include "thread_supervisor_control_server.h"

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
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
static bool read_ready_for(int fd, short events, int timeout_ms)
{
    struct pollfd p = {.fd = fd, .events = events};
    const int n = poll(&p, 1, timeout_ms);
    return n == 1 && (p.revents & events) != 0;
}
static bool read_ready(int fd, short events)
{
    return read_ready_for(fd, events, 1000);
}
static bool send_packet(int fd, const void *data, size_t bytes)
{
    return fd_seqpacket(fd) && read_ready(fd, POLLOUT) &&
           send(fd, data, bytes, MSG_NOSIGNAL | MSG_DONTWAIT) == (ssize_t)bytes;
}

/* The protocol is SOCK_SEQPACKET, not a byte stream. Verify the *whole*
 * reply including MSG_TRUNC; a short/long or unrelated reply must never
 * be reinterpreted as the result of the next Windows API call. */
static bool receive_exact_reply(int fd, void *out, size_t size, int timeout_ms)
{
    if (!read_ready_for(fd, POLLIN, timeout_ms))
        return false;
    struct iovec body = {.iov_base = out, .iov_len = size};
    struct msghdr packet = {.msg_iov = &body, .msg_iovlen = 1};
    const ssize_t n = recvmsg(fd, &packet, MSG_DONTWAIT);
    return n == (ssize_t)size &&
           (packet.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) == 0;
}

/* Caller holds client_lock. An ambiguous mutation ACK is irrevocable:
 * detach the socket so the parent broker revokes capabilities and
 * releases any stopped tracees on EOF. Never accept a late packet. */
static void abandon_client_session_locked(void)
{
    if (client_socket >= 0) {
        (void)shutdown(client_socket, SHUT_RDWR);
        (void)close(client_socket);
        client_socket = -1;
    }
    memset(client_secret, 0, sizeof(client_secret));
}

struct server_context {
    int fd;
    struct nt_thread_broker_registered_peer peer;
};
static void *server_main(void *context)
{
    struct server_context *owner = context;
    nt_broker_server_loop(owner->fd, &owner->peer);
    if (owner->peer.guest_session != NULL)
        (void)munmap(owner->peer.guest_session,
                     sizeof(*owner->peer.guest_session));
    close(owner->fd);
    close(owner->peer.child_pidfd);
    memset(owner->peer.secret, 0, sizeof(owner->peer.secret));
    free(owner);
    return NULL;
}
bool nt_broker_server_start(int fd,
    const struct nt_thread_broker_registered_peer *peer)
{
    if (peer == NULL || !fd_seqpacket(fd) || !live_child(peer) ||
        peer->trusted_startup_dll_count > NT_GUEST_ADDITIONAL_CODE_MAX)
        return false;
    for (size_t i = 0; i < peer->trusted_startup_dll_count; ++i) {
        const struct nt_guest_code_range range = peer->trusted_startup_dlls[i];
        if (!range.begin || range.end <= range.begin ||
            !(range.end <= peer->trusted_code_begin ||
              range.begin >= peer->trusted_code_end))
            return false;
        for (size_t j = 0; j < i; ++j) {
            const struct nt_guest_code_range prior =
                peer->trusted_startup_dlls[j];
            if (!(range.end <= prior.begin || range.begin >= prior.end))
                return false;
        }
    }
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
    struct nt_broker_context_request *out, bool *has_context)
{
    if (!live_child(peer) || !fd_seqpacket(fd))
        return false;
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0 ||
        !read_ready(fd, POLLIN))
        return false;
    struct nt_broker_context_request packet = {0};
    struct iovec body = {.iov_base = &packet, .iov_len = sizeof(packet)};
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
    const bool extended = read_bytes == (ssize_t)sizeof(packet);
    const struct nt_broker_rpc request = packet.request;
    if ((read_bytes != (ssize_t)sizeof(request) && !extended) ||
        (extended && request.operation != NT_BROKER_RPC_SUP_SET_CONTEXT) ||
        (!extended && request.operation == NT_BROKER_RPC_SUP_SET_CONTEXT) ||
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
    *out = packet;
    *has_context = extended;
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

/* Always verify the kernel-owned PC/GS/TEB independently of the
 * shared provenance. Only the original direct parent may inspect the
 * scope. A rejected interruption is immediately resumed, then detached
 * with an additional bounded stop. Diagnostic only, not Win32 suspend. */
static bool supervisor_guest_probe(const struct nt_thread_broker_registered_peer *peer,
                                    const struct nt_broker_thread_info *identity,
                                    uint64_t *guest_rip)
{
    if (!peer || !identity || !guest_rip || !peer->guest_session ||
        peer->trusted_code_begin == 0 ||
        peer->trusted_code_end <= peer->trusted_code_begin ||
        identity->guest_process_id != (uint32_t)peer->child_pid)
        return false;
    const struct nt_guest_boundary_slot *slot =
        nt_guest_boundary_find_thread(peer->guest_session,
                                      (pid_t)identity->guest_thread_id);
    if (!slot) return false;
    const uintptr_t teb = atomic_load_explicit(&slot->teb, memory_order_acquire);
    if (teb == 0) return false;
    const struct nt_supervisor_guest_scope scope = {
        .boundary = &slot->boundary, .teb = teb,
        .code_begin = peer->trusted_code_begin,
        .code_end = peer->trusted_code_end,
        .additional_code = peer->trusted_startup_dlls,
        .additional_code_count = peer->trusted_startup_dll_count
    };
    struct nt_thread_supervisor sup;
    nt_supervisor_init(&sup);
    if (nt_supervisor_seize_child(&sup, peer->child_pid,
                                  (pid_t)identity->guest_thread_id))
        return false;
    struct user_regs_struct context = {0};
    const bool safe = nt_supervisor_interrupt_guest(&sup, &scope, 500,
                                                     &context) == 0;
    bool okay = false;
    if (safe && context.rip != 0)
        *guest_rip = context.rip;
    /* Even if the guest gate rejected a host critical section and already
     * resumed, a bounded raw INTERRUPT is required for PTRACE_DETACH.
     * Never persist a stopped tracee on a rejected diagnostic request. */
    if (!sup.stopped)
        (void)nt_supervisor_interrupt(&sup, 500);
    if (sup.stopped && nt_supervisor_detach(&sup) == 0)
        okay = safe && context.rip != 0;
    if (sup.seized) {
#if defined(SYS_pidfd_send_signal)
        (void)syscall(SYS_pidfd_send_signal, peer->child_pidfd,
                      SIGKILL, NULL, 0);
#endif
    }
    nt_supervisor_release(&sup);
    return okay;
}

void nt_broker_server_loop(int fd,
    const struct nt_thread_broker_registered_peer *peer)
{
    if (peer == NULL || !fd_seqpacket(fd))
        return;
    struct nt_supervisor_control_server *control =
        nt_supervisor_control_server_create();
    if (!control) return;
    for (;;) {
        if (!live_child(peer))
            break;
        struct pollfd active = {.fd = fd, .events = POLLIN};
        const int available = poll(&active, 1, 250);
        if (available == 0 || (available < 0 && errno == EINTR))
            continue; /* Idle channels stay usable across arbitrary waits. */
        if (available < 0 || !(active.revents & POLLIN))
            break;
        struct nt_broker_context_request authenticated = {0};
        bool has_context = false;
        if (!receive_authenticated_rpc(fd, peer, &authenticated, &has_context))
            break; /* Fail closed on disconnect, bad protocol or impersonation. */
        const struct nt_broker_rpc request = authenticated.request;
        struct nt_thread_broker_reply response = {
            .magic = NT_THREAD_BROKER_MAGIC,
            /* Control APIs must return a genuine NTSTATUS on a rejected
             * capability, never a positive value that Windows might
             * interpret as success or an information-level status. */
            .status = request.operation >= NT_BROKER_RPC_SUP_SUSPEND &&
                      request.operation <= NT_BROKER_RPC_SUP_SET_CONTEXT ?
                      (uint32_t)NT_STATUS_ACCESS_DENIED : 1u
        };
        /* GET_CONTEXT replies are extended even on authorization or
         * validation failure; the client must receive the true NTSTATUS
         * without mistaking a short error reply for broken transport. */
        _Alignas(16) struct nt_context64 context_response = {0};
        const pid_t requester = peer->child_pid;
        /* Authenticate response ownership even when no target capability
         * was resolved: rejected calls must still carry their precise
         * negative NTSTATUS without being confused with lost transport. */
        response.process_id = (uint32_t)requester;
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
                nt_supervisor_control_server_revoke_tid(control, request.thread_id);
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
        case NT_BROKER_RPC_SUP_PROBE_GUEST:
            if (request.thread_id == 0 && request.access ==
                    NT_BROKER_SUPERVISOR_GET_CONTEXT &&
                (peer->allowed_access & NT_BROKER_SERVER_ENABLE_PTRACE_PROBE) != 0 &&
                nt_broker_supervisor_query_owned(requester, request.capability,
                                                  request.access, &info)) {
                uint64_t instruction = 0;
                if (supervisor_guest_probe(peer, &info, &instruction)) {
                    response.status = 0;
                    response.process_id = info.guest_process_id;
                    response.thread_id = info.guest_thread_id;
                    response.generation = info.generation;
                    response.capability = instruction;
                }
            }
            break;
        case NT_BROKER_RPC_SUP_SUSPEND:
        case NT_BROKER_RPC_SUP_RESUME:
        case NT_BROKER_RPC_SUP_GET_CONTEXT:
        case NT_BROKER_RPC_SUP_SET_CONTEXT: {
            const uint32_t rights =
                request.operation == NT_BROKER_RPC_SUP_SUSPEND ||
                request.operation == NT_BROKER_RPC_SUP_RESUME ?
                    NT_BROKER_SUPERVISOR_SUSPEND :
                request.operation == NT_BROKER_RPC_SUP_GET_CONTEXT ?
                    NT_BROKER_SUPERVISOR_GET_CONTEXT :
                    NT_BROKER_SUPERVISOR_SET_CONTEXT;
            /* Any guest attempt to nominate a Linux TID, reuse a stale
             * capability or submit unexpected payload is rejected. */
            const bool shaped = request.thread_id == 0 &&
                (request.operation == NT_BROKER_RPC_SUP_GET_CONTEXT ?
                    request.access != 0 : request.access == 0) &&
                (request.operation == NT_BROKER_RPC_SUP_SET_CONTEXT ?
                    has_context : !has_context);
            if (!shaped ||
                !nt_broker_supervisor_query_owned(requester,
                             request.capability, rights, &info))
                break;
            _Alignas(16) struct nt_context64 output = {0};
            uint32_t previous = UINT32_MAX;
            const nt_status status = nt_supervisor_control_server_call(
                control, peer, &info, request.operation, request.access,
                has_context ? &authenticated.context : NULL,
                &output, &previous);
            response.status = (uint32_t)status;
            response.process_id = info.guest_process_id;
            response.thread_id = info.guest_thread_id;
            response.generation = info.generation;
            if (status == NT_STATUS_SUCCESS) {
                if (request.operation == NT_BROKER_RPC_SUP_GET_CONTEXT)
                    context_response = output;
                if (request.operation == NT_BROKER_RPC_SUP_SUSPEND ||
                    request.operation == NT_BROKER_RPC_SUP_RESUME)
                    response.capability = previous;
            }
            break;
        }
        default:
            break;
        }
        if (request.operation == NT_BROKER_RPC_SUP_GET_CONTEXT) {
            const struct nt_broker_context_reply extended = {
                .reply = response, .context = context_response
            };
            if (!send_packet(fd, &extended, sizeof(extended)))
                break;
        } else if (!send_packet(fd, &response, sizeof(response))) {
            break;
        }
    }
    nt_supervisor_control_server_destroy(control);
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
    bool okay = false;
    if (client_socket >= 0) {
        struct nt_broker_rpc request = {
            .magic = NT_THREAD_BROKER_MAGIC, .version = NT_BROKER_RPC_VERSION,
            .operation = operation, .access = access,
            .thread_id = tid, .capability = capability
        };
        memcpy(request.secret, client_secret, 16);
        struct nt_thread_broker_reply response = {0};
        const bool valid = send_packet(client_socket, &request, sizeof(request)) &&
            receive_exact_reply(client_socket, &response, sizeof(response), 4000) &&
            response.magic == NT_THREAD_BROKER_MAGIC &&
            response.process_id == (uint32_t)getpid();
        if (!valid) {
            abandon_client_session_locked();
        } else if (response.status == 0) {
            okay = true;
            if (answer != NULL)
                *answer = response;
        }
        /* A well-formed access denial leaves the channel reusable. */
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
bool nt_broker_client_supervisor_probe_guest(uint64_t capability, uint64_t *guest_rip)
{
    if (capability == 0 || !guest_rip) return false;
    struct nt_thread_broker_reply reply = {0};
    if (!client_rpc(NT_BROKER_RPC_SUP_PROBE_GUEST, 0,
                    NT_BROKER_SUPERVISOR_GET_CONTEXT, capability, &reply) ||
        reply.process_id != (uint32_t)getpid() || reply.thread_id == 0 ||
        reply.generation == 0 || reply.capability == 0)
        return false;
    *guest_rip = reply.capability;
    return true;
}
/* Serial client-side transport: user APC handlers cannot interleave
 * payload packets with another worker's RPC. Never expose host errors
 * as a fabricated Windows success status. */
static nt_status control_rpc(uint32_t op, uint64_t cap, uint32_t flags,
                             const struct nt_context64 *input,
                             struct nt_context64 *output, uint32_t *previous)
{
    if (cap == 0) return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&client_lock);
    nt_status result = NT_STATUS_NOT_SUPPORTED;
    if (client_socket >= 0) {
        struct nt_broker_context_request request = {0};
        request.request = (struct nt_broker_rpc){
            .magic = NT_THREAD_BROKER_MAGIC,
            .version = NT_BROKER_RPC_VERSION,
            .operation = op,
            .access = flags,
            .capability = cap
        };
        memcpy(request.request.secret, client_secret, sizeof(client_secret));
        if (input != NULL)
            request.context = *input;
        const bool sent = input != NULL ?
            send_packet(client_socket, &request, sizeof(request)) :
            send_packet(client_socket, &request.request, sizeof(request.request));
        /* Parent PTRACE_INTERRUPT can wait 1500ms plus scheduling.
         * A timeout shorter than its stop deadline could strand a
         * successfully suspended guest despite a missing client ACK. */
        struct nt_broker_context_reply reply = {0};
        const size_t expected = op == NT_BROKER_RPC_SUP_GET_CONTEXT ?
            sizeof(reply) : sizeof(reply.reply);
        const bool transport_valid =
            sent && receive_exact_reply(client_socket, &reply, expected, 4000) &&
            reply.reply.magic == NT_THREAD_BROKER_MAGIC &&
            reply.reply.process_id == (uint32_t)getpid();
        if (!transport_valid) {
            abandon_client_session_locked();
        } else {
            result = (nt_status)reply.reply.status;
            if (result == NT_STATUS_SUCCESS) {
                if (output != NULL)
                    *output = reply.context;
                if (previous != NULL)
                    *previous = (uint32_t)reply.reply.capability;
            }
        }
    }
    pthread_mutex_unlock(&client_lock);
    return result;
}

nt_status nt_broker_client_supervisor_suspend(uint64_t cap, uint32_t *previous)
{
    return control_rpc(NT_BROKER_RPC_SUP_SUSPEND, cap, 0, NULL, NULL, previous);
}
nt_status nt_broker_client_supervisor_resume(uint64_t cap, uint32_t *previous)
{
    return control_rpc(NT_BROKER_RPC_SUP_RESUME, cap, 0, NULL, NULL, previous);
}
nt_status nt_broker_client_supervisor_get_context(uint64_t cap,
                                                   struct nt_context64 *context)
{
    if (!context) return NT_STATUS_INVALID_PARAMETER;
    _Alignas(16) struct nt_context64 result = {0};
    const nt_status status = control_rpc(NT_BROKER_RPC_SUP_GET_CONTEXT,
        cap, context->flags, NULL, &result, NULL);
    if (status == NT_STATUS_SUCCESS)
        *context = result;
    return status;
}
nt_status nt_broker_client_supervisor_set_context(uint64_t cap,
                                                   const struct nt_context64 *context)
{
    if (!context) return NT_STATUS_INVALID_PARAMETER;
    return control_rpc(NT_BROKER_RPC_SUP_SET_CONTEXT, cap, 0, context, NULL, NULL);
}
bool nt_broker_client_supervisor_close(uint64_t capability)
{
    return capability != 0 &&
        client_rpc(NT_BROKER_RPC_SUP_CLOSE, 0, 0, capability, NULL);
}
