/* SPDX-License-Identifier: MIT */
/* Test authenticated child transport WITHOUT enabling OpenThread on arbitrary
 * host TIDs. Tests real fork, kernel SCM_CREDENTIALS, pidfd and query ACL. */
#define _GNU_SOURCE
#include "thread_broker_auth.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failed;

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "thread broker boundary: %s\n", message);
        ++failed;
    }
}
static int send_packet(int fd, const struct nt_thread_broker_request *packet,
                       size_t size)
{
    return send(fd, packet, size, MSG_NOSIGNAL) == (ssize_t)size;
}
static int send_child_sequence(int fd, const uint8_t token[16])
{
    struct nt_thread_broker_request packet = {
        .magic = NT_BROKER_MAGIC,
        .version = NT_BROKER_VERSION,
        .operation = NT_BROKER_QUERY_IDENTITY,
        .requested_access = NT_BROKER_QUERY_LIMITED_INFORMATION
    };
    memcpy(packet.secret, token, 16);
    if (!send_packet(fd, &packet, 8)) /* truncated header */
        return 0;
    packet.version = 300;
    if (!send_packet(fd, &packet, sizeof(packet)))
        return 0;
    packet.version = NT_BROKER_VERSION;
    packet.secret[0] ^= 0x5a;
    if (!send_packet(fd, &packet, sizeof(packet)))
        return 0;
    packet.secret[0] ^= 0x5a;
    packet.requested_access = 0x20; /* THREAD_SET_INFORMATION forbidden */
    if (!send_packet(fd, &packet, sizeof(packet)))
        return 0;
    packet.requested_access = NT_BROKER_QUERY_LIMITED_INFORMATION;
    return send_packet(fd, &packet, sizeof(packet));
}
static int exit_clean(pid_t child)
{
    int status = 0;
    return waitpid(child, &status, 0) == child &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void)
{
    uint8_t secret[16];
    check(nt_thread_broker_generate_secret(secret),
          "CSPRNG must create nonpredictable 128-bit peer secret");
    if (failed)
        return 1;
    int channel[2], trigger[2], release[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, channel) != 0 ||
        pipe(trigger) != 0 || pipe(release) != 0)
        return 2;
    const int passcred = 1;
    check(setsockopt(channel[0], SOL_SOCKET, SO_PASSCRED,
                     &passcred, sizeof(passcred)) == 0,
          "enable per-message kernel credentials BEFORE child inherits FD");

    const pid_t legitimate = fork();
    if (legitimate < 0)
        return 3;
    if (legitimate == 0) {
        close(channel[0]);
        close(trigger[1]);
        close(release[1]);
        char go = 0, quit = 0;
        const int success = read(trigger[0], &go, 1) == 1 &&
            go == 'g' && send_child_sequence(channel[1], secret) &&
            read(release[0], &quit, 1) == 1 && quit == 'q';
        _exit(success ? 0 : 1);
    }
    const int pidfd = (int)syscall(SYS_pidfd_open, legitimate, 0);
    check(pidfd >= 0, "pidfd identity required, no PID-only fallback");
    if (pidfd < 0)
        return 4;
    struct nt_thread_broker_registered_peer peer = {
        .child_pid = legitimate,
        .uid = getuid(),
        .gid = getgid(),
        .child_pidfd = pidfd,
        .allowed_access = NT_BROKER_QUERY_LIMITED_INFORMATION
    };
    memcpy(peer.secret, secret, 16);

    /* Adversarial sibling knows the FULL secret and has inherited the
     * client socket. Kernel per-message PID still prevents impersonation. */
    const pid_t attacker = fork();
    if (attacker < 0)
        return 5;
    if (attacker == 0) {
        close(channel[0]);
        struct nt_thread_broker_request packet = {
            .magic = NT_BROKER_MAGIC, .version = NT_BROKER_VERSION,
            .operation = NT_BROKER_QUERY_IDENTITY,
            .requested_access = NT_BROKER_QUERY_LIMITED_INFORMATION
        };
        memcpy(packet.secret, secret, 16);
        _exit(send_packet(channel[1], &packet, sizeof(packet)) ? 0 : 1);
    }
    check(exit_clean(attacker), "attacker sent its impersonation attempt");
    close(channel[1]);
    close(trigger[0]);
    close(release[0]);

    uint32_t granted = 0x5151;
    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_DENIED && granted == 0x5151,
          "forged sibling with valid token must fail kernel PID check");
    check(write(trigger[1], "g", 1) == 1, "start registered child");
    close(trigger[1]);

    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_BAD_PACKET && granted == 0x5151,
          "truncated message rejected without granting rights");
    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_BAD_PACKET && granted == 0x5151,
          "unsupported protocol version rejected");
    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_DENIED && granted == 0x5151,
          "bad session token rejected");
    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_DENIED && granted == 0x5151,
          "external THREAD_SET_INFORMATION forbidden");
    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_OK &&
              granted == NT_BROKER_QUERY_LIMITED_INFORMATION,
          "real registered child query gets exact authorized read-only scope");

    check(write(release[1], "q", 1) == 1, "release registered child");
    close(release[1]);
    check(exit_clean(legitimate), "registered child exits without host effects");
    granted = 0x5151;
    check(nt_thread_broker_receive_query(channel[0], &peer, &granted) ==
              NT_THREAD_BROKER_UNAVAILABLE && granted == 0x5151,
          "dead pidfd forbids reviving any session authority");
    close(pidfd);
    close(channel[0]);
    if (!failed)
        puts("PASS broker boundary: authenticated SCM_CREDENTIALS + pidfd, query-only, no foreign Linux mutation");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
