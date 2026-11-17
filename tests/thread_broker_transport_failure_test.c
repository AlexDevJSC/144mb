/* SPDX-License-Identifier: MIT */
/* Host-only negative transport fixture. A fake but direct parent sends
 * malformed, oversized, denied and delayed SOCK_SEQPACKET replies.
 * The client MUST tear down ambiguous sessions and preserve outputs.
 * No guest PE instructions, ptrace, or root permissions are required. */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
static int child_case(int fd, int mode)
{
    if (!nt_broker_client_attach(fd)) return 10;
    struct nt_broker_thread_info id = {.guest_thread_id = 0x51515151u};
    const uint64_t fake_capability = UINT64_C(0x4141414141414141);
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
    const pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        close(fd[0]);
        _exit(child_case(fd[1], mode));
    }
    close(fd[1]);
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
        if (!fake_reply(fd[0], (uint32_t)child, mode, (unsigned)step) &&
            mode != 3)
            return 6;
    }
    close(fd[0]);
    int status = 0;
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
    for (int mode=0; mode<5; ++mode)
        if (run_case(mode)) return 1;
    puts("PASS broker transport: oversized/denied/bad-magic/late/short-CONTEXT "
         "packets fail closed and leave outputs untouched");
    return 0;
}
