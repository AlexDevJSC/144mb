/* SPDX-License-Identifier: MIT */
/* Real forked guest processes and a real pthread worker exercise the same
 * private IPC lifecycle as posix_spawn PE children. No arbitrary Linux PID
 * receives a signal, affinity or priority syscall from this test. */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"

#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct endpoint {
    int socket[2], commands[2], replies[2], pidfd;
    pid_t pid;
    uint8_t secret[16];
    uint64_t generation;
};
static int write_all(int fd, const void *data, size_t length)
{
    return write(fd, data, length) == (ssize_t)length;
}
static int read_all(int fd, void *data, size_t length)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 3000) != 1 || !(p.revents & POLLIN))
        return 0;
    return read(fd, data, length) == (ssize_t)length;
}
static int start_endpoint(struct endpoint *client, void (*body)(int,int,int))
{
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, client->socket) != 0 ||
        pipe(client->commands) != 0 || pipe(client->replies) != 0 ||
        !nt_thread_broker_generate_secret(client->secret))
        return 0;
    const pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        close(client->socket[0]);
        close(client->commands[1]);
        close(client->replies[0]);
        alarm(15);
        body(client->socket[1], client->commands[0], client->replies[1]);
        _exit(1);
    }
    client->pid = pid;
    close(client->socket[1]);
    close(client->commands[0]);
    close(client->replies[1]);
    client->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    return client->pidfd >= 0;
}
/* Worker uses fds[0] for write notifications and fds[1] for release. */
static void *secondary_main(void *ctx)
{
    int *fds = ctx;
    const uint32_t tid = (uint32_t)syscall(SYS_gettid);
    if (!nt_broker_client_register_thread(tid)) {
        fprintf(stderr, "IPC target: authenticated worker REGISTER rejected (pid=%ld tid=%u)\n",
                (long)getpid(), tid);
        return (void *)1;
    }
    if (!write_all(fds[0], &tid, sizeof(tid)))
        return (void *)1;
    char release = 0;
    if (!read_all(fds[1], &release, 1) || release != 'F')
        return (void *)1;
    nt_broker_client_unregister_thread(tid);
    return NULL;
}
static void target_child(int fd, int commands, int replies)
{
    if (!nt_broker_client_attach(fd)) {
        fprintf(stderr, "IPC target: parent handshake failed\n");
        _exit(2);
    }
    int worker_release[2];
    if (pipe(worker_release) != 0)
        _exit(3);
    int args[2] = {replies, worker_release[0]};
    pthread_t worker;
    if (pthread_create(&worker, NULL, secondary_main, args) != 0)
        _exit(4);
    char flag = 0;
    if (!read_all(commands, &flag, 1) || flag != 'F' ||
        !write_all(worker_release[1], "F", 1))
        _exit(5);
    void *exit_status = (void *)1;
    if (pthread_join(worker, &exit_status) != 0 || exit_status != NULL)
        _exit(6);
    if (!write_all(replies, "D", 1))
        _exit(7);
    if (!read_all(commands, &flag, 1) || flag != 'Q')
        _exit(8);
    nt_broker_client_detach();
    _exit(0);
}
static void querying_child(int fd, int commands, int replies)
{
    if (!nt_broker_client_attach(fd)) {
        fprintf(stderr, "IPC requester: parent handshake failed\n");
        _exit(11);
    }
    uint32_t tid = 0;
    if (!read_all(commands, &tid, sizeof(tid)))
        _exit(12);
    struct nt_broker_thread_info info = {0};
    uint64_t capability = 0;
    const bool opened = nt_broker_client_open(tid,
                       NT_BROKER_QUERY_LIMITED_INFORMATION, &info, &capability);
    if (!opened || info.guest_thread_id != tid || capability == 0 ||
        !nt_broker_client_query(capability,
                    NT_BROKER_QUERY_LIMITED_INFORMATION, &info) ||
        info.guest_thread_id != tid)
        { fprintf(stderr, "IPC requester: OPEN or QUERY failed tid=%u opened=%d capability=%llu actual=%u\n",
                  tid, (int)opened, (unsigned long long)capability, info.guest_thread_id); _exit(13); }
    uint64_t forbidden = 0x5151;
    if (nt_broker_client_open(tid, 0x20u, &info, &forbidden) ||
        forbidden != 0x5151)
        _exit(14);
    if (!write_all(replies, "O", 1))
        _exit(15);
    char token = 0;
    if (!read_all(commands, &token, 1) || token != 'R')
        _exit(16);
    info = (struct nt_broker_thread_info){.guest_thread_id = 0x5151};
    if (nt_broker_client_query(capability, NT_BROKER_QUERY_LIMITED_INFORMATION,
                               &info) || info.guest_thread_id != 0x5151)
        _exit(17);
    if (!write_all(replies, "D", 1) ||
        !read_all(commands, &token, 1) || token != 'Q')
        _exit(18);
    nt_broker_client_detach();
    _exit(0);
}
static int enable_server(struct endpoint *ep)
{
    if (!nt_broker_register_child(ep->pid, ep->pidfd, getuid(), getgid(),
                                  ep->secret, &ep->generation))
        return 0;
    struct nt_thread_broker_registered_peer peer = {
        .child_pid = ep->pid, .uid = getuid(), .gid = getgid(),
        .child_pidfd = ep->pidfd,
        .allowed_access = NT_BROKER_QUERY_LIMITED_INFORMATION
    };
    memcpy(peer.secret, ep->secret, 16);
    if (!nt_broker_server_start(ep->socket[0], &peer))
        return 0;
    return nt_broker_server_handshake(ep->socket[0], ep->secret);
}
int main(void)
{
    alarm(19);
    struct endpoint target = {0}, asker = {0};
    if (!start_endpoint(&target, target_child) ||
        !start_endpoint(&asker, querying_child))
        return 1;
    if (!enable_server(&target) || !enable_server(&asker) ||
        !nt_broker_grant_child_query(asker.pid, target.pid,
                                    NT_BROKER_QUERY_LIMITED_INFORMATION))
        return 2;
    uint32_t actual_tid = 0;
    char response = 0;
    if (!read_all(target.replies[0], &actual_tid, sizeof(actual_tid))) {
        fprintf(stderr, "IPC parent: target failed to publish secondary TID\n");
        return 3;
    }
    if (actual_tid == 0 || actual_tid == (uint32_t)target.pid ||
        !write_all(asker.commands[1], &actual_tid, sizeof(actual_tid)) ||
        !read_all(asker.replies[0], &response, 1) || response != 'O') {
        fprintf(stderr, "IPC parent: cross-process remote OPEN did not confirm TID=%u response=%d\n",
                actual_tid, (int)response);
        return 3;
    }
    uint64_t handle = 0;
    struct nt_broker_thread_info info = {0};
    if (!nt_broker_open_registered_thread(getpid(), (pid_t)actual_tid,
                    NT_BROKER_QUERY_LIMITED_INFORMATION, &handle) ||
        !nt_broker_query_child_thread(getpid(), handle,
                    NT_BROKER_QUERY_LIMITED_INFORMATION, &info) ||
        info.guest_process_id != (uint32_t)target.pid ||
        info.guest_thread_id != actual_tid ||
        !nt_broker_close_child_thread(getpid(), handle))
        return 4;
    if (!write_all(target.commands[1], "F", 1) ||
        !read_all(target.replies[0], &response, 1) || response != 'D' ||
        !write_all(asker.commands[1], "R", 1) ||
        !read_all(asker.replies[0], &response, 1) || response != 'D')
        return 5;
    if (!write_all(target.commands[1], "Q", 1) ||
        !write_all(asker.commands[1], "Q", 1))
        return 6;
    int target_status = 0, asker_status = 0;
    if (waitpid(target.pid, &target_status, 0) != target.pid ||
        waitpid(asker.pid, &asker_status, 0) != asker.pid ||
        !WIFEXITED(target_status) || WEXITSTATUS(target_status) != 0 ||
        !WIFEXITED(asker_status) || WEXITSTATUS(asker_status) != 0)
        return 7;
    nt_broker_unregister_child(target.pid, target.generation);
    nt_broker_unregister_child(asker.pid, asker.generation);
    close(target.pidfd);
    close(asker.pidfd);
    close(target.commands[1]);
    close(target.replies[0]);
    close(asker.commands[1]);
    close(asker.replies[0]);
    puts("PASS IPC: 2 registered guest processes, authenticated secondary thread, remote query and post-exit revocation");
    return 0;
}
