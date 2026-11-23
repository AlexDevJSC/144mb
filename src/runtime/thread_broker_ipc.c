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
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

_Static_assert(sizeof(struct nt_broker_rpc) == 48, "private RPC layout");
_Static_assert(sizeof(struct nt_thread_broker_reply) == 32, "private reply layout");
_Static_assert(sizeof(struct nt_broker_hello) == 24, "private hello layout");

static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;
/* Independent of client_lock: a trusted runtime thread must be able to
 * shut down a blocked RPC while its owner holds client_lock. The abort
 * mutex pins the socket's lifetime so a recycled integer FD can NEVER
 * refer to an unrelated guest or host resource during cancellation.
 * Lock order: client_lock -> client_abort_lock; never the reverse. */
static pthread_mutex_t client_abort_lock = PTHREAD_MUTEX_INITIALIZER;
static bool client_rpc_inflight;
static bool client_rpc_aborted;
static int client_socket = -1;
static uint8_t client_secret[16];

#ifdef NT_BROKER_TEST_DELAY_CONTEXT_ACK
/* Only linked into the dedicated test binary; a kernel-confirmed, truly
 * suspended guest must reach this gate before its client disconnects. */
static _Atomic uint32_t *test_context_ack_gate;
void nt_broker_test_set_context_ack_gate(_Atomic uint32_t *gate)
{
    test_context_ack_gate = gate;
}
#endif
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
static bool broker_peer_disconnected(int fd)
{
    /* Read-only check, no recv/consume: the parent already authenticated
     * exactly one request. POLLRDHUP is produced by an explicit
     * shutdown(SHUT_RDWR), not by a request for more data. */
    struct pollfd state = {.fd = fd, .events = POLLRDHUP};
    const int ready = poll(&state, 1, 0);
    return ready < 0 ? errno != EINTR :
           ready > 0 && (state.revents &
                         (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
}
static bool read_ready_for(int fd, short events, int timeout_ms)
{
    if (timeout_ms < 0)
        return false;
    struct timespec clock_now;
    if (clock_gettime(CLOCK_MONOTONIC, &clock_now) != 0)
        return false;
    const uint64_t start_ms = (uint64_t)clock_now.tv_sec * 1000u +
                              (uint64_t)clock_now.tv_nsec / 1000000u;
    const uint64_t deadline = start_ms + (unsigned)timeout_ms;
    int remaining = timeout_ms;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = events};
        const int n = poll(&p, 1, remaining);
        if (n == 1)
            return (p.revents & events) != 0 &&
                   (p.revents & POLLNVAL) == 0;
        if (n == 0 || (n < 0 && errno != EINTR))
            return false;
        /* Linux signals may interrupt poll even with SA_RESTART.
         * They are not a transport/protocol error. Retry only until
         * the ORIGINAL deadline; never extend RPC time on signals. */
        if (clock_gettime(CLOCK_MONOTONIC, &clock_now) != 0)
            return false;
        const uint64_t now_ms = (uint64_t)clock_now.tv_sec * 1000u +
                                (uint64_t)clock_now.tv_nsec / 1000000u;
        if (now_ms >= deadline)
            return false;
        remaining = (int)(deadline - now_ms);
    }
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

/* Only the RPC owner holds client_lock. A cancellation thread holds
 * only client_abort_lock, can interrupt poll/recv via shutdown, and never
 * closes the FD while the RPC owner might still use its integer value. */
static void begin_client_rpc_locked(void)
{
    pthread_mutex_lock(&client_abort_lock);
    client_rpc_inflight = true;
    client_rpc_aborted = false;
    pthread_mutex_unlock(&client_abort_lock);
}

static bool finish_client_rpc_locked(void)
{
    pthread_mutex_lock(&client_abort_lock);
    const bool aborted = client_rpc_aborted;
    client_rpc_inflight = false;
    client_rpc_aborted = false;
    pthread_mutex_unlock(&client_abort_lock);
    return aborted;
}

/* This operation can run on another trusted host thread while the owner
 * is inside sendmsg/poll/recvmsg (with client_lock HELD). It revokes the
 * entire ambiguous session: a late reply must not be consumed as an ACK.
 * Returns false when no RPC is in flight, never acts on an arbitrary FD. */
bool nt_broker_client_abort_inflight(void)
{
    pthread_mutex_lock(&client_abort_lock);
    const bool active = client_rpc_inflight && client_socket >= 0;
    if (active) {
        client_rpc_aborted = true;
        (void)shutdown(client_socket, SHUT_RDWR);
    }
    pthread_mutex_unlock(&client_abort_lock);
    return active;
}

/* Caller holds client_lock. An ambiguous mutation ACK is irrevocable:
 * detach the socket so the parent broker revokes capabilities and
 * releases any stopped tracees on EOF. Never accept a late packet. */
static void abandon_client_session_locked(void)
{
    pthread_mutex_lock(&client_abort_lock);
    if (client_socket >= 0) {
        (void)shutdown(client_socket, SHUT_RDWR);
        (void)close(client_socket);
        client_socket = -1;
    }
    client_rpc_inflight = false;
    client_rpc_aborted = false;
    pthread_mutex_unlock(&client_abort_lock);
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

/* A 1-second quiet period is NOT an RPC error. The broker session owns
 * long-lived NT thread capabilities, and a guest can execute for minutes
 * without issuing a request. Watch BOTH the socket and the exact pidfd
 * until a packet arrives, EOF is detected or the child actually exits.
 * Keep bounded 4-second deadlines only on client request/reply. */
static bool wait_for_authenticated_request(int fd,
    const struct nt_thread_broker_registered_peer *peer)
{
    for (;;) {
        struct pollfd watchers[2] = {
            {.fd = fd, .events = POLLIN},
            {.fd = peer->child_pidfd, .events = POLLIN}
        };
        const int ready = poll(watchers, 2, -1);
        if (ready == -1 && errno == EINTR)
            continue;
        if (ready <= 0)
            return false;
        if (watchers[1].revents &
            (POLLIN | POLLHUP | POLLERR | POLLNVAL))
            return false; /* registered child has exited */
        if (watchers[0].revents & POLLIN)
            return true; /* recvmsg still verifies credentials + length */
        if (watchers[0].revents & (POLLHUP | POLLERR | POLLNVAL))
            return false; /* real channel failure, not idle timeout */
    }
}

static bool receive_authenticated_rpc(int fd,
    const struct nt_thread_broker_registered_peer *peer,
    struct nt_broker_context_request *out, bool *has_context,
    bool *has_apc)
{
    if (!live_child(peer) || !fd_seqpacket(fd))
        return false;
    const int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on)) != 0 ||
        !wait_for_authenticated_request(fd, peer))
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
    const bool apc_frame =
        read_bytes == (ssize_t)sizeof(struct nt_broker_apc_request);
    const struct nt_broker_rpc request = packet.request;
    if ((read_bytes != (ssize_t)sizeof(request) && !extended && !apc_frame) ||
        (extended && request.operation != NT_BROKER_RPC_SUP_SET_CONTEXT) ||
        (apc_frame && request.operation != NT_BROKER_RPC_SUP_APC_AUTHORIZE) ||
        (!extended && request.operation == NT_BROKER_RPC_SUP_SET_CONTEXT) ||
        (!apc_frame && request.operation == NT_BROKER_RPC_SUP_APC_AUTHORIZE) ||
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
    *has_apc = apc_frame;
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
        bool has_context = false, has_apc = false;
        if (!receive_authenticated_rpc(fd, peer, &authenticated,
                                       &has_context, &has_apc))
            break; /* Fail closed on disconnect, bad protocol or impersonation. */
        const struct nt_broker_rpc request = authenticated.request;
        struct nt_thread_broker_reply response = {
            .magic = NT_THREAD_BROKER_MAGIC,
            /* Control APIs must return a genuine NTSTATUS on a rejected
             * capability, never a positive value that Windows might
             * interpret as success or an information-level status. */
            .status = request.operation >= NT_BROKER_RPC_SUP_SUSPEND &&
                      request.operation <= NT_BROKER_RPC_SUP_APC_AUTHORIZE ?
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
        case NT_BROKER_RPC_SUP_APC_AUTHORIZE: {
            const struct nt_broker_apc_request *apc =
                (const struct nt_broker_apc_request *)(const void *)&authenticated;
            if (!has_apc || request.thread_id != 0 || request.access != 0 ||
                apc->generation == 0 || apc->callback == 0 ||
                !nt_broker_supervisor_query_owned(requester, request.capability,
                    NT_BROKER_SUPERVISOR_SET_CONTEXT, &info))
                break;
            response.process_id = info.guest_process_id;
            response.thread_id = info.guest_thread_id;
            response.generation = info.generation;
            if (info.generation != apc->generation) {
                response.status = (uint32_t)NT_STATUS_ACCESS_DENIED;
                break;
            }
            response.status = (uint32_t)nt_supervisor_control_server_authorize_apc(
                peer, &info, (uintptr_t)apc->callback);
            break;
        }
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
        /* A SET_CONTEXT that the kernel accepted is not committed to the
         * caller until its ACK can be delivered. If the caller has left
         * the authenticated session, never detach with unacknowledged
         * mutated GPRs. Only this generation-owned pidfd tracee may be
         * terminated; the server thread owns its ptrace state. */
        const bool unacked_context =
            request.operation == NT_BROKER_RPC_SUP_SET_CONTEXT &&
            response.status == NT_STATUS_SUCCESS &&
            info.guest_thread_id != 0 && info.generation != 0;
#ifdef NT_BROKER_TEST_DELAY_CONTEXT_ACK
        if (unacked_context && test_context_ack_gate != NULL) {
            atomic_store_explicit(test_context_ack_gate, 1u, memory_order_release);
            /* The test client waits for this exact point, then closes
             * during the pause. Production builds omit this entire hook. */
            const struct timespec pause = {.tv_nsec = 400000000L};
            (void)nanosleep(&pause, NULL);
        }
#endif
        if (broker_peer_disconnected(fd)) {
            if (unacked_context)
                nt_supervisor_control_server_abort_unacked_context(control, &info);
            break;
        }
        bool reply_sent = false;
        if (request.operation == NT_BROKER_RPC_SUP_GET_CONTEXT) {
            const struct nt_broker_context_reply extended = {
                .reply = response, .context = context_response
            };
            reply_sent = send_packet(fd, &extended, sizeof(extended));
        } else {
            reply_sent = send_packet(fd, &response, sizeof(response));
        }
        if (!reply_sent) {
            if (unacked_context)
                nt_supervisor_control_server_abort_unacked_context(control, &info);
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
        pthread_mutex_lock(&client_abort_lock);
        client_socket = fd;
        client_rpc_inflight = false;
        client_rpc_aborted = false;
        pthread_mutex_unlock(&client_abort_lock);
        memcpy(client_secret, hello.secret, 16);
    }
    pthread_mutex_unlock(&client_lock);
    return okay;
}

void nt_broker_client_detach(void)
{
    /* Unlike a blocking detach, first interrupt the in-flight poll from
     * another thread. The RPC owner still closes the socket after seeing
     * that its request/reply was ambiguous. */
    (void)nt_broker_client_abort_inflight();
    pthread_mutex_lock(&client_lock);
    abandon_client_session_locked();
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
        begin_client_rpc_locked();
        const bool received = send_packet(client_socket, &request, sizeof(request)) &&
            receive_exact_reply(client_socket, &response, sizeof(response), 4000);
        const bool canceled = finish_client_rpc_locked();
        const bool valid = received && !canceled &&
            response.magic == NT_THREAD_BROKER_MAGIC &&
            response.process_id != 0;
        /* Remote QUERY/OPEN intentionally returns the *target* child's
         * PID, not the requester's. Session identity is verified with
         * peer credentials on attach and every parent-side request; we
         * must not reject valid cross-process read-only queries here.
         * The caller checks returned TID/capability and the registry
         * enforces permission on the parent side. */
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
        begin_client_rpc_locked();
        const bool sent = input != NULL ?
            send_packet(client_socket, &request, sizeof(request)) :
            send_packet(client_socket, &request.request, sizeof(request.request));
        /* Parent PTRACE_INTERRUPT can wait 1500ms plus scheduling.
         * A timeout shorter than its stop deadline could strand a
         * successfully suspended guest despite a missing client ACK. */
        struct nt_broker_context_reply reply = {0};
        const size_t expected = op == NT_BROKER_RPC_SUP_GET_CONTEXT ?
            sizeof(reply) : sizeof(reply.reply);
        const bool received =
            sent && receive_exact_reply(client_socket, &reply, expected, 4000);
        const bool canceled = finish_client_rpc_locked();
        const bool transport_valid = received && !canceled &&
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
nt_status nt_broker_client_supervisor_authorize_apc(
    uint64_t capability, uint64_t generation, uintptr_t callback)
{
    if (capability == 0 || generation == 0 || callback == 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&client_lock);
    nt_status result = NT_STATUS_NOT_SUPPORTED;
    if (client_socket >= 0) {
        struct nt_broker_apc_request request = {0};
        request.request = (struct nt_broker_rpc){
            .magic = NT_THREAD_BROKER_MAGIC,
            .version = NT_BROKER_RPC_VERSION,
            .operation = NT_BROKER_RPC_SUP_APC_AUTHORIZE,
            .capability = capability
        };
        memcpy(request.request.secret, client_secret, sizeof(client_secret));
        request.generation = generation;
        request.callback = (uint64_t)callback;
        begin_client_rpc_locked();
        struct nt_thread_broker_reply reply = {0};
        const bool received =
            send_packet(client_socket, &request, sizeof(request)) &&
            receive_exact_reply(client_socket, &reply, sizeof(reply), 4000);
        const bool canceled = finish_client_rpc_locked();
        if (!received || canceled || reply.magic != NT_THREAD_BROKER_MAGIC ||
            reply.process_id != (uint32_t)getpid()) {
            abandon_client_session_locked();
        } else if (reply.status == (uint32_t)NT_STATUS_SUCCESS &&
                   (reply.thread_id == 0 || reply.generation != generation)) {
            /* A claimed successful admission must match the exact
             * generation. A genuine ACCESS_DENIED on stale generation,
             * however, is NOT a broken transport session. */
            abandon_client_session_locked();
        } else {
            result = (nt_status)reply.status;
        }
    }
    pthread_mutex_unlock(&client_lock);
    return result;
}

bool nt_broker_client_supervisor_close(uint64_t capability)
{
    return capability != 0 &&
        client_rpc(NT_BROKER_RPC_SUP_CLOSE, 0, 0, capability, NULL);
}
