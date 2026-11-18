/* SPDX-License-Identifier: MIT */
/* Host-only negative transport fixture. A fake but direct parent sends
 * malformed, oversized, denied and delayed SOCK_SEQPACKET replies.
 * The client MUST tear down ambiguous sessions and preserve outputs.
 * No guest PE instructions, ptrace, or root permissions are required. */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"
#include "nt_services.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t signal_seen;
static void harmless_signal(int signum)
{
    (void)signum;
    signal_seen = 1;
}
static int ready(int fd)
{
    struct pollfd p = {.fd=fd, .events=POLLIN};
    return poll(&p, 1, 7000) == 1 && (p.revents & POLLIN);
}
static void delay_ms(unsigned milliseconds)
{
    struct timespec pause = {.tv_sec=milliseconds / 1000,
        .tv_nsec=(long)(milliseconds % 1000) * 1000000};
    while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
}
static int fake_reply(int fd, uint32_t pid, int mode, unsigned step)
{
    struct nt_thread_broker_reply reply = {
        .magic = NT_THREAD_BROKER_MAGIC,
        .process_id = pid,
        .thread_id = pid,
        .generation = 1,
        .status = mode == 1 && step == 0 ? UINT32_C(0xc0000022) : 0
    };
    if (mode == 0 || (mode == 1 && step == 1)) {
        /* This is not exactly one reply frame. A stream-style partial
         * read might silently accept it and leave extra bytes buffered. */
        unsigned char oversized[sizeof(reply) + 8] = {0};
        memcpy(oversized, &reply, sizeof(reply));
        return send(fd, oversized, sizeof(oversized),
                    MSG_DONTWAIT | MSG_NOSIGNAL) == (ssize_t)sizeof(oversized);
    }
    if (mode == 2) reply.magic ^= 0xffu;
    if (mode == 3) {
        /* Client's request timeout is 4s; a late response must NOT be
         * parsed as the reply to another Win32 operation. */
        delay_ms(4300);
    }
    return send(fd, &reply, sizeof(reply), MSG_DONTWAIT | MSG_NOSIGNAL) ==
           (ssize_t)sizeof(reply);
}
/* This is a *real* two-thread client: a worker holds the serial RPC
 * lock while waiting for the parent, and the other thread must revoke
 * its live session without waiting for the 4-second RPC timeout. The
 * fake parent keeps the socket OPEN throughout the test. */
struct cancel_case {
    bool mutation;
    uint64_t capability;
    bool query_success;
    nt_status control_status;
    struct nt_broker_thread_info identity;
    uint32_t previous;
};
static void *blocking_client_request(void *argument)
{
    struct cancel_case *item = argument;
    if (item->mutation) {
        item->control_status = nt_broker_client_supervisor_suspend(
            item->capability, &item->previous);
    } else {
        item->query_success = nt_broker_client_supervisor_query(
            item->capability, NT_BROKER_SUPERVISOR_GET_CONTEXT, &item->identity);
    }
    return NULL;
}
static int cancel_live_rpc(int notify_fd, int mode)
{
    struct cancel_case item = {
        .mutation = mode == 7,
        .control_status = NT_STATUS_NOT_SUPPORTED,
        .capability = UINT64_C(0x4141414141414141),
        .identity = {.guest_thread_id = 0x51515151u},
        .previous = 0x5a5a5a5au
    };
    pthread_t worker;
    if (pthread_create(&worker, NULL, blocking_client_request, &item) != 0)
        return 21;
    /* The parent writes only AFTER recvmsg read the request. It does not
     * send a fake reply, EOF or a signal: only the explicit abort/detach
     * can wake the blocked socket before the 4s timeout. */
    char signal = 0;
    if (read(notify_fd, &signal, 1) != 1 || signal != 'R')
        return 22;
    if (mode == 6) {
        if (!nt_broker_client_abort_inflight())
            return 23;
    } else {
        /* A concurrent detach MUST wake the worker instead of waiting
         * for it to exhaust the old receive timeout. */
        nt_broker_client_detach();
    }
    if (pthread_join(worker, NULL) != 0)
        return 24;
    if (item.query_success ||
        item.control_status != NT_STATUS_NOT_SUPPORTED ||
        item.identity.guest_thread_id != 0x51515151u ||
        item.previous != 0x5a5a5a5au ||
        nt_broker_client_is_attached() ||
        nt_broker_client_abort_inflight())
        return 25;
    if (nt_broker_client_supervisor_query(item.capability,
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &item.identity) ||
        item.identity.guest_thread_id != 0x51515151u)
        return 26;
    nt_broker_client_detach();
    return 0;
}

static int child_case(int fd, int mode, int notify_fd)
{
    if (mode == 5) {
        struct sigaction action = {.sa_handler = harmless_signal};
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGUSR1, &action, NULL) != 0)
            return 19;
    }
    if (!nt_broker_client_attach(fd)) return 10;
    if (mode == 6 || mode == 7)
        return cancel_live_rpc(notify_fd, mode);
    struct nt_broker_thread_info id = {.guest_thread_id = 0x51515151u};
    const uint64_t fake_capability = UINT64_C(0x4141414141414141);
    if (mode == 5) {
        if (!nt_broker_client_supervisor_query(fake_capability,
                NT_BROKER_SUPERVISOR_GET_CONTEXT, &id) ||
            id.guest_process_id != (uint32_t)getpid() ||
            signal_seen != 1 || !nt_broker_client_is_attached())
            return 20;
        nt_broker_client_detach();
        return 0;
    }
    if (mode == 4) {
        _Alignas(16) struct nt_context64 context = {0};
        context.flags = UINT32_C(0x100003);
        context.rax = UINT64_C(0x5555666677778888);
        if (nt_broker_client_supervisor_get_context(fake_capability, &context) !=
                NT_STATUS_NOT_SUPPORTED ||
            context.rax != UINT64_C(0x5555666677778888))
            return 11; /* Short GET_CONTEXT must be rejected. */
    } else {
        if (nt_broker_client_supervisor_query(fake_capability,
                    NT_BROKER_SUPERVISOR_GET_CONTEXT, &id))
            return 12;
        if (id.guest_thread_id != 0x51515151u)
            return 13;
        if (mode == 1) {
            /* A well-formed access denial does NOT desynchronise the
             * transport; the subsequent malformed packet MUST do so. */
            if (!nt_broker_client_is_attached())
                return 14;
            if (nt_broker_client_supervisor_query(fake_capability,
                    NT_BROKER_SUPERVISOR_GET_CONTEXT, &id))
                return 15;
        }
    }
    if (nt_broker_client_is_attached())
        return 16;
    id.guest_thread_id = 0x51515151u;
    if (nt_broker_client_supervisor_query(fake_capability,
            NT_BROKER_SUPERVISOR_GET_CONTEXT, &id) ||
        id.guest_thread_id != 0x51515151u)
        return 17;
    nt_broker_client_detach();
    return 0;
}
static int run_case(int mode)
{
    int fd[2] = {-1,-1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fd) != 0)
        return 1;
    int notice[2] = {-1,-1};
    if ((mode == 6 || mode == 7) && pipe(notice) != 0)
        return 2;
    const pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        close(fd[0]);
        if (notice[1] >= 0) close(notice[1]);
        _exit(child_case(fd[1], mode, notice[0]));
    }
    close(fd[1]);
    if (notice[0] >= 0) close(notice[0]);
    struct nt_broker_hello hello = {
        .magic = NT_THREAD_BROKER_MAGIC, .version = NT_BROKER_RPC_VERSION
    };
    const bool greeted = send(fd[0], &hello, sizeof(hello), MSG_NOSIGNAL) ==
                         (ssize_t)sizeof(hello);
    if (!greeted) return 3;
    int packets = mode == 1 ? 2 : 1;
    for (int step = 0; step < packets; ++step) {
        struct nt_broker_context_request request = {0};
        if (!ready(fd[0]) ||
            recv(fd[0], &request, sizeof(request), 0) <= 0)
            return 4;
        /* A control GET must receive a *full* extended reply. The fake
         * parent deliberately sends only the 32-byte prefix. */
        if (mode == 4 &&
            request.request.operation != NT_BROKER_RPC_SUP_GET_CONTEXT)
            return 5;
        if (mode == 5) {
            /* SIGUSR1 interrupts the client's poll in a real process.
             * A valid authenticated reply within the original timeout
             * must still complete without losing the IPC session. */
            delay_ms(65);
            if (kill(child, SIGUSR1) != 0) return 8;
            delay_ms(135);
        }
        if (mode == 6 || mode == 7) {
            if (request.request.operation !=
                (mode == 7 ? NT_BROKER_RPC_SUP_SUSPEND :
                             NT_BROKER_RPC_SUP_QUERY))
                return 9;
            /* The fake parent stays alive and connected, without
             * replying. Notify child that the request was received. */
            if (write(notice[1], "R", 1) != 1) return 10;
            close(notice[1]);
            notice[1] = -1;
        } else if (!fake_reply(fd[0], (uint32_t)child, mode,
                               (unsigned)step) && mode != 3) {
            return 6;
        }
    }
    int status = 0;
    if (mode == 6 || mode == 7) {
        bool finished = false;
        for (unsigned elapsed = 0; elapsed < 1500; elapsed += 10) {
            const pid_t got = waitpid(child, &status, WNOHANG);
            if (got == child) { finished = true; break; }
            if (got < 0 && errno != EINTR) return 11;
            delay_ms(10);
        }
        if (!finished) {
            fprintf(stderr, "FAIL cancel RPC remained blocked mode=%d\\n", mode);
            (void)kill(child, SIGKILL);
            (void)waitpid(child, &status, 0);
            close(fd[0]);
            return 12;
        }
        close(fd[0]);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "FAIL in-flight cancel mode=%d exit=%d\\n", mode, status);
            return 13;
        }
        return 0;
    }
    close(fd[0]);
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL IPC corrupted reply mode=%d exit=%d\n", mode, status);
        return 7;
    }
    return 0;
}
int main(void)
{
    alarm(35);
    for (int mode=0; mode<8; ++mode)
        if (run_case(mode)) return 1;
    puts("PASS broker transport: oversized/denied/bad-magic/late/short-CONTEXT "
         "packets fail closed; EINTR preserves session; concurrent abort/detach "
         "interrupt in-flight RPC without a late ACK or mutated outputs");
    return 0;
}
