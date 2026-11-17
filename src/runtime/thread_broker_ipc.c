/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"
#include "ntnative/guest_boundary_runtime.h"
#include "../supervisor/nt_thread_supervisor.h"
#include "../supervisor/nt_supervisor_nt_bridge.h"

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

_Static_assert(sizeof(struct nt_broker_rpc) == 1280, "private RPC v4 layout");
_Static_assert(sizeof(struct nt_thread_broker_reply) == 1264, "private reply v4 layout");
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
        .code_end = peer->trusted_code_end
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

/* One owner-thread (the authenticated parent broker) serializes all
 * mutation of these controls. Tracer objects are never shared with the
 * child or retained in guest memory. State is canonical PER TID, not
 * per HANDLE, exactly as Windows suspend counts are. */
enum { MAX_SUPERVISED_THREADS = 256 };
struct controlled_guest {
    bool active;
    uint32_t tid;
    uint64_t generation;
    struct nt_thread_supervisor tracer;
    struct nt_supervisor_nt_thread nt;
};
static void release_control(struct controlled_guest *slot)
{
    if (!slot->active) return;
    nt_supervisor_release(&slot->tracer);
    *slot = (struct controlled_guest){0};
}
static struct controlled_guest *find_control(struct controlled_guest *controls,
                                            uint32_t tid, uint64_t generation)
{
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i)
        if (controls[i].active && controls[i].tid == tid &&
            controls[i].generation == generation)
            return &controls[i];
    return NULL;
}
static struct controlled_guest *new_control(struct controlled_guest *controls)
{
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i)
        if (!controls[i].active)
            return &controls[i];
    return NULL;
}
static nt_status create_control(struct controlled_guest *slot,
    const struct nt_thread_broker_registered_peer *peer,
    const struct nt_broker_thread_info *identity)
{
    if (!slot || !peer->guest_session || !peer->trusted_code_begin ||
        peer->trusted_code_end <= peer->trusted_code_begin)
        return NT_STATUS_NOT_SUPPORTED;
    const struct nt_guest_boundary_slot *boundary =
        nt_guest_boundary_find_thread(peer->guest_session,
                                      (pid_t)identity->guest_thread_id);
    if (!boundary) return NT_STATUS_NOT_SUPPORTED;
    const uintptr_t teb = atomic_load_explicit(&boundary->teb, memory_order_acquire);
    if (!teb) return NT_STATUS_NOT_SUPPORTED;
    nt_supervisor_init(&slot->tracer);
    if (nt_supervisor_seize_child(&slot->tracer, peer->child_pid,
                                  (pid_t)identity->guest_thread_id))
        return errno == EACCES || errno == EPERM ? NT_STATUS_ACCESS_DENIED :
               NT_STATUS_NOT_SUPPORTED;
    slot->tid = identity->guest_thread_id;
    slot->generation = identity->generation;
    slot->active = true;
    slot->nt = (struct nt_supervisor_nt_thread){
        .tracer = &slot->tracer,
        .scope = {
            .boundary = &boundary->boundary,
            .teb = teb,
            .code_begin = peer->trusted_code_begin,
            .code_end = peer->trusted_code_end
        },
        .granted_access = NT_SUP_THREAD_SUSPEND_RESUME |
                          NT_SUP_THREAD_GET_CONTEXT |
                          NT_SUP_THREAD_SET_CONTEXT
    };
    return NT_STATUS_SUCCESS;
}
static nt_status operate_control(struct controlled_guest *controls,
    const struct nt_thread_broker_registered_peer *peer,
    const struct nt_broker_rpc *request, struct nt_thread_broker_reply *reply,
    const struct nt_broker_thread_info *identity)
{
    struct controlled_guest *slot =
        find_control(controls, identity->guest_thread_id, identity->generation);
    switch (request->operation) {
    case NT_BROKER_RPC_SUP_SUSPEND: {
        if (!slot) {
            slot = new_control(controls);
            nt_status status = create_control(slot, peer, identity);
            if (status != NT_STATUS_SUCCESS) return status;
        }
        uint32_t previous = 0;
        nt_status status = nt_supervisor_nt_suspend_thread(&slot->nt, &previous);
        if (status != NT_STATUS_SUCCESS && slot->nt.suspend_count == 0)
            release_control(slot);
        if (status == NT_STATUS_SUCCESS)
            reply->capability = previous;
        return status;
    }
    case NT_BROKER_RPC_SUP_RESUME: {
        if (!slot || !slot->nt.suspend_count)
            return NT_STATUS_NOT_SUPPORTED;
        const uint32_t previous = slot->nt.suspend_count;
        if (previous == 1) {
            /* DETACH is a complete final resume; no stranded ptrace
             * attachment remains once the counted suspension reaches 0. */
            if (nt_supervisor_detach(&slot->tracer)) {
                release_control(slot);
                return NT_STATUS_UNSUCCESSFUL;
            }
            release_control(slot);
        } else {
            --slot->nt.suspend_count;
        }
        reply->capability = previous;
        return NT_STATUS_SUCCESS;
    }
    case NT_BROKER_RPC_SUP_GET_CONTEXT: {
        if (!slot || !slot->nt.suspend_count) return NT_STATUS_NOT_SUPPORTED;
        _Alignas(16) struct nt_context64 output = {0};
        output.flags = request->context.flags;
        nt_status status = nt_supervisor_nt_get_context(&slot->nt, &output);
        if (status == NT_STATUS_SUCCESS)
            reply->context = output;
        return status;
    }
    case NT_BROKER_RPC_SUP_SET_CONTEXT:
        return !slot || !slot->nt.suspend_count ? NT_STATUS_NOT_SUPPORTED :
               nt_supervisor_nt_set_context(&slot->nt, &request->context);
    default: return NT_STATUS_INVALID_PARAMETER;
    }
}
void nt_broker_server_loop(int fd,
    const struct nt_thread_broker_registered_peer *peer)
{
    if (peer == NULL || !fd_seqpacket(fd))
        return;
    struct controlled_guest controls[MAX_SUPERVISED_THREADS] = {0};
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
            /* No guest packet can grant this flag. The trusted loader
             * must explicitly authorize the backend, and every request
             * revalidates the capability's ownership and generation. */
            if ((peer->allowed_access & NT_BROKER_SERVER_ENABLE_ASYNC_CONTEXT) &&
                request.thread_id == 0 && request.access == rights &&
                nt_broker_supervisor_query_owned(requester, request.capability,
                                                  rights, &info)) {
                response.status = (uint32_t)operate_control(
                    controls, peer, &request, &response, &info);
            } else {
                response.status = (uint32_t)NT_STATUS_ACCESS_DENIED;
            }
            break;
        }
        default:
            break;
        }
        if (!send_packet(fd, &response, sizeof(response)))
            break;
    }
    /* Any EOF, invalid credentials, fatal protocol error or child death
     * releases every suspended thread BEFORE revoking the session.
     * Fail-safe ptrace cleanup never strands a stopped target. */
    for (unsigned i = 0; i < MAX_SUPERVISED_THREADS; ++i)
        release_control(&controls[i]);
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

static bool client_rpc_extended(uint32_t operation, uint32_t tid, uint32_t access,
                       uint64_t capability, const struct nt_context64 *input,
                       struct nt_thread_broker_reply *answer)
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
        if (input != NULL) request.context = *input;
        /* A Linux PTRACE_INTERRUPT can consume up to 1500 ms, plus
         * scheduler jitter and protocol handling. Never time out earlier
         * than the parent: otherwise a late success would strand a
         * suspended guest while the client wrongly falls back. */
        bool transport_valid =
            send_packet(client_socket, &request, sizeof(request)) &&
            read_ready_for(client_socket, POLLIN, 4000);
        struct nt_thread_broker_reply response = {0};
        if (transport_valid)
            transport_valid = recv(client_socket, &response, sizeof(response),
                                   MSG_DONTWAIT) == (ssize_t)sizeof(response) &&
                              response.magic == NT_THREAD_BROKER_MAGIC;
        if (!transport_valid) {
            /* Treat missing/late/malformed ACK as an ambiguous mutation.
             * Closing the only authenticated socket obliges the parent
             * to revoke capabilities and DETACH all held ptrace threads.
             * Never issue another request on a desynchronized stream. */
            (void)shutdown(client_socket, SHUT_RDWR);
            close(client_socket);
            client_socket = -1;
            memset(client_secret, 0, sizeof(client_secret));
        }
        if (transport_valid && answer != NULL)
            *answer = response;
        okay = transport_valid && response.status == 0;
    }
    pthread_mutex_unlock(&client_lock);
    return okay;
}

static bool client_rpc(uint32_t operation, uint32_t tid, uint32_t access,
                       uint64_t capability, struct nt_thread_broker_reply *answer)
{
    return client_rpc_extended(operation, tid, access, capability, NULL, answer);
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
bool nt_broker_client_supervisor_close(uint64_t capability)
{
    return capability != 0 &&
        client_rpc(NT_BROKER_RPC_SUP_CLOSE, 0, 0, capability, NULL);
}

/* Win32 front-end is intentionally opt-in. On any timeout/malformed reply,
 * leave caller outputs untouched and return NOT_SUPPORTED; never claim
 * suspension or context changes without a verified server ACK. */
static nt_status control_rpc(uint32_t operation, uint64_t capability,
                             uint32_t access, const struct nt_context64 *input,
                             struct nt_thread_broker_reply *response)
{
    if (capability == 0 || !response) return NT_STATUS_INVALID_PARAMETER;
    *response = (struct nt_thread_broker_reply){0};
    if (client_rpc_extended(operation, 0, access, capability, input, response))
        return NT_STATUS_SUCCESS;
    if (response->magic == NT_THREAD_BROKER_MAGIC &&
        response->status != 0 && response->status != 1)
        return (nt_status)response->status;
    return NT_STATUS_NOT_SUPPORTED;
}
nt_status nt_broker_client_supervisor_suspend(uint64_t capability, uint32_t *previous)
{
    struct nt_thread_broker_reply reply;
    nt_status status = control_rpc(NT_BROKER_RPC_SUP_SUSPEND, capability,
        NT_BROKER_SUPERVISOR_SUSPEND, NULL, &reply);
    if (status == NT_STATUS_SUCCESS && previous)
        *previous = (uint32_t)reply.capability;
    return status;
}
nt_status nt_broker_client_supervisor_resume(uint64_t capability, uint32_t *previous)
{
    struct nt_thread_broker_reply reply;
    nt_status status = control_rpc(NT_BROKER_RPC_SUP_RESUME, capability,
        NT_BROKER_SUPERVISOR_SUSPEND, NULL, &reply);
    if (status == NT_STATUS_SUCCESS && previous)
        *previous = (uint32_t)reply.capability;
    return status;
}
nt_status nt_broker_client_supervisor_get_context(uint64_t capability,
                                                    struct nt_context64 *context)
{
    if (!context) return NT_STATUS_ACCESS_VIOLATION;
    struct nt_thread_broker_reply reply;
    nt_status status = control_rpc(NT_BROKER_RPC_SUP_GET_CONTEXT, capability,
        NT_BROKER_SUPERVISOR_GET_CONTEXT, context, &reply);
    if (status == NT_STATUS_SUCCESS)
        *context = reply.context;
    return status;
}
nt_status nt_broker_client_supervisor_set_context(uint64_t capability,
                                                    const struct nt_context64 *context)
{
    if (!context) return NT_STATUS_ACCESS_VIOLATION;
    struct nt_thread_broker_reply reply;
    return control_rpc(NT_BROKER_RPC_SUP_SET_CONTEXT, capability,
        NT_BROKER_SUPERVISOR_SET_CONTEXT, context, &reply);
}
