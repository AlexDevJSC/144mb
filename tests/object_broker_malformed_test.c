/* SPDX-License-Identifier: MIT */
/* Real SOCK_SEQPACKET/SCM_RIGHTS regression: invalid broker replies must
 * not leak FDs already installed by recvmsg before payload validation. */
#define _GNU_SOURCE
#include "object_broker.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ROUNDS = 16 };

static int fd_count(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (!dir)
        return -1;
    int count = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL)
        if (strcmp(item->d_name, ".") && strcmp(item->d_name, ".."))
            ++count;
    closedir(dir);
    return count;
}

static int serve_bad_reply(int client, int scenario)
{
    struct nt_broker_request request = {0};
    if (recv(client, &request, sizeof(request), 0) !=
        (ssize_t)sizeof(request))
        return -1;
    if (scenario == 3) {
        /* Same UID, valid request, but a peer that never answers. The
         * client must time out and close its own socket, not hang. */
        sleep(4);
        return 0;
    }
    struct nt_broker_reply reply = {
        .magic = NT_BROKER_MAGIC, .version = NT_BROKER_VERSION,
        .result = NT_BROKER_CREATED,
        .kind = scenario == 0 ? NT_BROKER_EVENT : request.kind,
        .rights = request.rights, .size = 4096
    };
    int fds[3] = {-1, -1, -1};
    int count = scenario == 2 ? 3 : 2;
    for (int i = 0; i < count; ++i) {
        fds[i] = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fds[i] < 0)
            goto failure;
    }
    char ancillary[CMSG_SPACE(sizeof(fds))] = {0};
    struct iovec iov = {
        .iov_base = &reply,
        .iov_len = scenario == 1 ? sizeof(reply) - 1u : sizeof(reply)
    };
    struct msghdr message = {
        .msg_iov = &iov, .msg_iovlen = 1,
        .msg_control = ancillary,
        .msg_controllen = CMSG_SPACE((size_t)count * sizeof(int))
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&message);
    if (!cmsg)
        goto failure;
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN((size_t)count * sizeof(int));
    memcpy(CMSG_DATA(cmsg), fds, (size_t)count * sizeof(int));
    ssize_t sent = sendmsg(client, &message, MSG_NOSIGNAL);
    for (int i = 0; i < count; ++i)
        close(fds[i]);
    return sent == (ssize_t)iov.iov_len ? 0 : -1;
failure:
    for (int i = 0; i < count; ++i)
        if (fds[i] >= 0)
            close(fds[i]);
    return -1;
}

static void server(int listener)
{
    for (int i = 0; i < ROUNDS; ++i) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0)
            _exit(2);
        int status = serve_bad_reply(client, i == ROUNDS - 1 ? 3 : i % 3);
        close(client);
        if (status != 0)
            _exit(3);
    }
    close(listener);
    _exit(0);
}

int main(void)
{
    char directory[] = "/tmp/nt-broker-invalid-XXXXXX";
    if (!mkdtemp(directory))
        return 2;
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int length = snprintf(path, sizeof(path), "%s/broker.sock", directory);
    if (length <= 0 || (size_t)length >= sizeof(path))
        return 2;
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0)
        return 2;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    memcpy(addr.sun_path, path, (size_t)length + 1u);
    if (bind(listener, (struct sockaddr *)&addr,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + length + 1)) != 0 ||
        listen(listener, 4) != 0)
        return 2;
    pid_t child = fork();
    if (child < 0)
        return 2;
    if (!child)
        server(listener);
    close(listener);
    if (setenv("NT_NATIVE_BROKER_SOCKET", path, 1) != 0)
        return 2;
    alarm(12);
    const int before = fd_count();
    int fail = before < 0;
    for (int i = 0; i < ROUNDS && !fail; ++i) {
        struct nt_broker_request req = {
            .magic = NT_BROKER_MAGIC, .version = NT_BROKER_VERSION,
            .kind = NT_BROKER_SECTION, .action = NT_BROKER_OPEN,
            .rights = 1, .size = 4096
        };
        memcpy(req.name, "bad_reply", sizeof("bad_reply"));
        struct nt_broker_resource result = {0};
        struct timespec start = {0}, end = {0};
        if (i == ROUNDS - 1 && clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
            fail = 1;
            break;
        }
        int status = nt_broker_acquire(&req, &result);
        if (i == ROUNDS - 1 && clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
            fail = 1;
            break;
        }
        const int after = fd_count();
        const double elapsed =
            (double)(end.tv_sec - start.tv_sec) +
            (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
        if (status != NT_BROKER_TRANSPORT_ERROR ||
            result.fd != -1 || result.gate_fd != -1 ||
            result.lease_fd != -1 || after != before ||
            (i == ROUNDS - 1 && elapsed > 3.5)) {
            fprintf(stderr,
                    "FAIL broker reply case=%d status=%d fd before=%d after=%d time=%.3f\\n",
                    i == ROUNDS - 1 ? 3 : i % 3, status, before, after, elapsed);
            fail = 1;
        }
    }
    if (fail)
        kill(child, SIGKILL);
    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail = 1;
    unlink(path);
    rmdir(directory);
    if (fail)
        return 1;
    puts("PASS malformed and stalled broker replies (wrong kind, short payload, "
         "truncated SCM_RIGHTS, bounded receive): no descriptor leaks");
    return 0;
}
