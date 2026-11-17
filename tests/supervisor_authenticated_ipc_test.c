/* SPDX-License-Identifier: MIT */
/* End-to-end production FD8 IPC protocol: real forked PE-like children,
 * kernel credentials, pidfd, authenticated registration, owner-only
 * generation leases and a real external ptrace GETREGSET probe. */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"

#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct endpoint {
    int socket[2], commands[2], responses[2], pidfd;
    pid_t child;
    uint64_t generation;
    uint8_t secret[16];
    bool enable_probe;
};
struct worker_state {
    _Atomic int running, registered, failed;
    uint32_t tid;
};

static bool transmit(int fd, const void *value, size_t length)
{
    return write(fd, value, length) == (ssize_t)length;
}
static bool receive(int fd, void *value, size_t length)
{
    struct pollfd active = {.fd = fd, .events = POLLIN};
    if (poll(&active, 1, 3500) != 1 || !(active.revents & POLLIN))
        return false;
    size_t read_bytes = 0;
    while (read_bytes < length) {
        const ssize_t n = read(fd, (char *)value + read_bytes,
                               length - read_bytes);
        if (n <= 0) return false;
        read_bytes += (size_t)n;
    }
    return true;
}

static void *guest_worker(void *argument)
{
    struct worker_state *state = argument;
    const uint32_t tid = (uint32_t)syscall(SYS_gettid);
    state->tid = tid;
    if (!nt_broker_client_register_thread(tid)) {
        atomic_store(&state->failed, 1);
        return NULL;
    }
    atomic_store(&state->registered, 1);
    while (atomic_load_explicit(&state->running, memory_order_acquire))
        __asm__ volatile ("" ::: "memory");
    nt_broker_client_unregister_thread(tid);
    return NULL;
}

static _Noreturn void client_main(int fd, int commands, int responses,
                                 bool probe_expected)
{
    alarm(22);
    if (!nt_broker_client_attach(fd)) _exit(10);
    struct worker_state worker = {0};
    atomic_store(&worker.running, 1);
    pthread_t thread;
    if (pthread_create(&thread, NULL, guest_worker, &worker)) _exit(11);
    bool registered = false;
    for (int i = 0; i < 100000; ++i) {
        if (atomic_load(&worker.failed)) break;
        if (atomic_load(&worker.registered)) { registered = true; break; }
        sched_yield();
    }
    if (!registered || worker.tid == 0 || worker.tid == (uint32_t)getpid())
        _exit(12);
    if (!transmit(responses, &worker.tid, sizeof(worker.tid))) _exit(13);

    uint32_t foreign_tid = 0;
    if (!receive(commands, &foreign_tid, sizeof(foreign_tid))) _exit(14);
    struct nt_broker_thread_info identity = {0};
    uint64_t handle = UINT64_C(0x5151);
    if (nt_broker_client_supervisor_open(foreign_tid,
                NT_BROKER_SUPERVISOR_GET_CONTEXT, &identity, &handle) ||
        handle != UINT64_C(0x5151))
        _exit(15); /* valid secret never grants a sibling's TID */
    if (nt_broker_client_supervisor_open((uint32_t)getppid(),
                NT_BROKER_SUPERVISOR_GET_CONTEXT, &identity, &handle) ||
        handle != UINT64_C(0x5151))
        _exit(16); /* parent/system process is never a guest target */
    if (nt_broker_client_supervisor_open(worker.tid, 0x80000000u,
                                         &identity, &handle) ||
        handle != UINT64_C(0x5151))
        _exit(17); /* reject requested rights escalation */
    if (!nt_broker_client_supervisor_open(worker.tid,
               NT_BROKER_SUPERVISOR_GET_CONTEXT | NT_BROKER_SUPERVISOR_SUSPEND,
               &identity, &handle) ||
        handle == UINT64_C(0x5151) || handle == 0 ||
        identity.guest_thread_id != worker.tid ||
        identity.guest_process_id != (uint32_t)getpid() ||
        identity.generation == 0)
        _exit(18);
    struct nt_broker_thread_info query = {0};
    if (!nt_broker_client_supervisor_query(handle,
              NT_BROKER_SUPERVISOR_GET_CONTEXT, &query) ||
        query.generation != identity.generation ||
        query.guest_thread_id != worker.tid ||
        nt_broker_client_supervisor_query(handle,
              NT_BROKER_SUPERVISOR_SET_CONTEXT, &query))
        _exit(19);
    if (nt_broker_client_supervisor_query(handle ^ UINT64_C(0x100), 
              NT_BROKER_SUPERVISOR_GET_CONTEXT, &query))
        _exit(20); /* guessed capability must not resolve */
    /* An authenticated channel and valid GET/SUSPEND capability alone
     * must NOT activate async ptrace without the trusted parent policy.
     * Every denied operation preserves caller-provided outputs. */
    uint32_t denied_previous = UINT32_C(0x51515151);
    if (nt_broker_client_supervisor_suspend(handle, &denied_previous) !=
            NT_STATUS_ACCESS_DENIED ||
        denied_previous != UINT32_C(0x51515151) ||
        nt_broker_client_supervisor_resume(handle, &denied_previous) !=
            NT_STATUS_ACCESS_DENIED ||
        denied_previous != UINT32_C(0x51515151))
        _exit(30);
    _Alignas(16) struct nt_context64 denied_context = {0};
    denied_context.flags = UINT32_C(0x100001);
    denied_context.rax = UINT64_C(0x123456789abcdef0);
    if (nt_broker_client_supervisor_get_context(handle, &denied_context) !=
            NT_STATUS_ACCESS_DENIED ||
        denied_context.flags != UINT32_C(0x100001) ||
        denied_context.rax != UINT64_C(0x123456789abcdef0) ||
        nt_broker_client_supervisor_set_context(handle, &denied_context) !=
            NT_STATUS_ACCESS_DENIED)
        _exit(31);
    uint64_t rip = UINT64_C(0xcafebabe);
    const bool probed = nt_broker_client_supervisor_probe(handle, &rip);
    if (probe_expected ? (!probed || rip == 0 || rip == UINT64_C(0xcafebabe)) :
                         (probed || rip != UINT64_C(0xcafebabe)))
        _exit(21); /* only trusted host-side flag enables ptrace probe */
    uint64_t guarded_rip = UINT64_C(0x51515151);
    if (nt_broker_client_supervisor_probe_guest(handle, &guarded_rip) ||
        guarded_rip != UINT64_C(0x51515151))
        _exit(29); /* Linux-only child has no trusted PE code scope */

    /* Explicit capability close and return of the old value to the pool
     * must not re-enable it (monotone handle + immutable generation). */
    if (!nt_broker_client_supervisor_close(handle) ||
        nt_broker_client_supervisor_query(handle,
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &query))
        _exit(22);
    uint64_t second = 0;
    if (!nt_broker_client_supervisor_open(worker.tid,
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &query, &second) ||
        second == 0 || second == handle ||
        query.generation != identity.generation)
        _exit(23);

    /* Initial thread is also parent-registered via the pidfd; the
     * requester's own primary TID can be opened without a self-ptrace. */
    uint64_t main_cap = 0;
    if (!nt_broker_client_supervisor_open((uint32_t)getpid(),
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &query, &main_cap) ||
        query.guest_thread_id != (uint32_t)getpid() ||
        !nt_broker_client_supervisor_close(main_cap))
        _exit(24);
    atomic_store_explicit(&worker.running, 0, memory_order_release);
    if (pthread_join(thread, NULL)) _exit(25);
    if (nt_broker_client_supervisor_query(second,
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &query))
        _exit(26); /* UNREGISTER revoked the lease */
    if (!transmit(responses, "P", 1)) _exit(27);
    char end = 0;
    if (!receive(commands, &end, 1) || end != 'Q') _exit(28);
    nt_broker_client_detach();
    _exit(0);
}

static bool begin_child(struct endpoint *ep, bool enable_probe)
{
    ep->enable_probe = enable_probe;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, ep->socket) ||
        pipe(ep->commands) || pipe(ep->responses) ||
        !nt_thread_broker_generate_secret(ep->secret))
        return false;
    const pid_t child = fork();
    if (child < 0) return false;
    if (child == 0) {
        close(ep->socket[0]);
        close(ep->commands[1]);
        close(ep->responses[0]);
        client_main(ep->socket[1], ep->commands[0], ep->responses[1],
                    enable_probe);
    }
    ep->child = child;
    close(ep->socket[1]);
    close(ep->commands[0]);
    close(ep->responses[1]);
    ep->pidfd = (int)syscall(SYS_pidfd_open, child, 0);
    if (ep->pidfd < 0) return false;
    if (!nt_broker_register_child(child, ep->pidfd, getuid(), getgid(),
                                  ep->secret, &ep->generation))
        return false;
    struct nt_thread_broker_registered_peer peer = {
        .child_pid = child, .uid = getuid(), .gid = getgid(),
        .child_pidfd = ep->pidfd,
        .allowed_access = NT_BROKER_QUERY_LIMITED_INFORMATION |
                          (enable_probe ? NT_BROKER_SERVER_ENABLE_PTRACE_PROBE : 0u)
    };
    memcpy(peer.secret, ep->secret, 16);
    return nt_broker_server_start(ep->socket[0], &peer) &&
           nt_broker_server_handshake(ep->socket[0], ep->secret);
}

int main(void)
{
    alarm(30);
    struct endpoint first = {0}, second = {0};
    if (!begin_child(&first, true) || !begin_child(&second, false)) {
        fputs("FAIL supervisor IPC: start authenticated child servers\n", stderr);
        return 1;
    }
    uint32_t first_tid = 0, second_tid = 0;
    if (!receive(first.responses[0], &first_tid, sizeof(first_tid)) ||
        !receive(second.responses[0], &second_tid, sizeof(second_tid)) ||
        first_tid == 0 || second_tid == 0 || first_tid == second_tid ||
        !transmit(first.commands[1], &second_tid, sizeof(second_tid)) ||
        !transmit(second.commands[1], &first_tid, sizeof(first_tid))) {
        fputs("FAIL supervisor IPC: child worker registration/coordination\n", stderr);
        return 2;
    }
    /* Even the trusted host registry denies a wrong owner and leaves the
     * output parameter unchanged on failure. */
    uint64_t forbidden = UINT64_C(0x5151);
    struct nt_broker_thread_info info = {0};
    if (nt_broker_supervisor_open_owned(first.child, (pid_t)second_tid,
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &forbidden, &info) ||
        forbidden != UINT64_C(0x5151)) {
        fputs("FAIL supervisor IPC: cross-process owner authority\n", stderr);
        return 3;
    }
    char first_reply = 0, second_reply = 0;
    if (!receive(first.responses[0], &first_reply, 1) || first_reply != 'P' ||
        !receive(second.responses[0], &second_reply, 1) || second_reply != 'P') {
        fputs("FAIL supervisor IPC: authenticated/open/probe/lease sequence\n", stderr);
        return 4;
    }
    if (!transmit(first.commands[1], "Q", 1) ||
        !transmit(second.commands[1], "Q", 1))
        return 5;
    int code_a = 0, code_b = 0;
    if (waitpid(first.child, &code_a, 0) != first.child ||
        waitpid(second.child, &code_b, 0) != second.child ||
        !WIFEXITED(code_a) || WEXITSTATUS(code_a) != 0 ||
        !WIFEXITED(code_b) || WEXITSTATUS(code_b) != 0) {
        fprintf(stderr, "FAIL supervisor IPC: child exits: a=%d b=%d\n",
                code_a, code_b);
        return 6;
    }
    nt_broker_unregister_child(first.child, first.generation);
    nt_broker_unregister_child(second.child, second.generation);
    close(first.pidfd);
    close(second.pidfd);
    close(first.commands[1]);
    close(second.commands[1]);
    close(first.responses[0]);
    close(second.responses[0]);
    puts("PASS supervisor IPC: FD8 SCM_CREDENTIALS/session/pidfd, two child workers, "
         "owner-bound generation capabilities, real opt-in ptrace GETREGSET, "
         "cross-child/rights/guessed-handle rejection, stale-thread revocation");
    return 0;
}
