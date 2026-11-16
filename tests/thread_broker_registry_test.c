/* SPDX-License-Identifier: MIT */
/* End-to-end parent-owned registry: two distinct Linux guest-child tasks,
 * per-message authenticated AF_UNIX request, opaque handles, right masks,
 * generation invalidation and concurrent accesses. Never alters host TIDs. */
#define _GNU_SOURCE
#include "thread_broker_registry.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void check(bool value, const char *description)
{
    if (!value) {
        fprintf(stderr, "thread registry: %s\n", description);
        ++failures;
    }
}

static int child_pidfd(pid_t pid)
{
#if defined(SYS_pidfd_open)
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    (void)pid;
    return -1;
#endif
}
static pid_t create_waiting_child(int *release)
{
    int pipefd[2];
    if (pipe(pipefd) != 0)
        return -1;
    const pid_t child = fork();
    if (child == 0) {
        close(pipefd[1]);
        alarm(15);
        char byte = 0;
        const int ok = read(pipefd[0], &byte, 1) == 1 && byte == 'q';
        close(pipefd[0]);
        _exit(ok ? 0 : 1);
    }
    close(pipefd[0]);
    if (child < 0) {
        close(pipefd[1]);
        return child;
    }
    *release = pipefd[1];
    return child;
}
static bool finish_child(pid_t pid, int release)
{
    const bool wrote = write(release, "q", 1) == 1;
    close(release);
    int code = 0;
    return wrote && waitpid(pid, &code, 0) == pid &&
           WIFEXITED(code) && WEXITSTATUS(code) == 0;
}
static bool send_identity_query(int fd, const uint8_t key[16])
{
    struct nt_thread_broker_request request = {
        .magic = NT_THREAD_BROKER_MAGIC,
        .version = NT_THREAD_BROKER_VERSION,
        .operation = NT_BROKER_QUERY_IDENTITY,
        .requested_access = NT_BROKER_QUERY_LIMITED_INFORMATION
    };
    memcpy(request.secret, key, 16);
    return send(fd, &request, sizeof(request), MSG_NOSIGNAL) ==
           (ssize_t)sizeof(request);
}

struct concurrent_case {
    pid_t target;
    atomic_int *failed;
};
static void *concurrent_open(void *argument)
{
    struct concurrent_case *test = argument;
    for (unsigned i = 0; i < 150; ++i) {
        uint64_t handle = 0;
        struct nt_broker_thread_info identity = {0};
        if (!nt_broker_open_child_thread(getpid(), test->target,
                 NT_BROKER_QUERY_LIMITED_INFORMATION, &handle) ||
            !nt_broker_query_child_thread(getpid(), handle,
                 NT_BROKER_QUERY_LIMITED_INFORMATION, &identity) ||
            identity.guest_process_id != (uint32_t)test->target ||
            identity.guest_thread_id != (uint32_t)test->target ||
            !nt_broker_close_child_thread(getpid(), handle))
            atomic_fetch_add(test->failed, 1);
    }
    return NULL;
}

int main(void)
{
    alarm(19);
    uint8_t target_secret[16], requester_secret[16], next_secret[16];
    check(nt_thread_broker_generate_secret(target_secret) &&
          nt_thread_broker_generate_secret(requester_secret) &&
          nt_thread_broker_generate_secret(next_secret),
          "CSPRNG token creation");
    int target_release = -1;
    pid_t target = create_waiting_child(&target_release);
    check(target > 0, "create separate trusted guest-like target");
    if (target <= 0)
        return EXIT_FAILURE;
    int target_fd = child_pidfd(target);
    check(target_fd >= 0, "target pidfd is mandatory");
    if (target_fd < 0)
        return EXIT_FAILURE;

    int pair[2], command[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0 ||
        pipe(command) != 0)
        return EXIT_FAILURE;
    pid_t requester = fork();
    if (requester == 0) {
        close(pair[0]);
        close(command[1]);
        alarm(15);
        char trigger = 0;
        const bool start = read(command[0], &trigger, 1) == 1 &&
                           trigger == 'g' &&
                           send_identity_query(pair[1], requester_secret);
        const bool stop = read(command[0], &trigger, 1) == 1 &&
                          trigger == 'q';
        close(pair[1]);
        close(command[0]);
        _exit(start && stop ? 0 : 1);
    }
    if (requester <= 0)
        return EXIT_FAILURE;
    close(command[0]);
    int requester_fd = child_pidfd(requester);
    check(requester_fd >= 0, "requester pidfd is mandatory");
    if (requester_fd < 0)
        return EXIT_FAILURE;

    uint64_t gen_target = 0, gen_requester = 0;
    check(nt_broker_register_child(target, target_fd, getuid(), getgid(),
                                   target_secret, &gen_target) &&
          nt_broker_register_child(requester, requester_fd, getuid(), getgid(),
                                   requester_secret, &gen_requester) &&
          gen_target > 0 && gen_requester > gen_target,
          "register two children with independent live pidfds/generations");

    uint64_t untouched_gen = 0x5151;
    check(!nt_broker_register_child(requester, target_fd, getuid(), getgid(),
                                    requester_secret, &untouched_gen) &&
          untouched_gen == 0x5151,
          "wrong PID and pidfd pair cannot register another identity");
    uint64_t out = 0x5151;
    check(!nt_broker_open_child_thread(requester, target,
                                      NT_BROKER_QUERY_LIMITED_INFORMATION, &out) &&
          out == 0x5151,
          "cross-child access denied without explicit trusted-host grant");
    check(!nt_broker_grant_child_query(requester, target, 0x20u),
          "reject THREAD_SET_INFORMATION cross-child grant");
    check(nt_broker_grant_child_query(requester, target,
                                     NT_BROKER_QUERY_LIMITED_INFORMATION),
          "host grants only permitted read-only access");

    uint64_t parent_handle = 0, sibling_handle = 0;
    struct nt_broker_thread_info info = {0};
    check(nt_broker_open_child_thread(getpid(), target,
                                     NT_BROKER_QUERY_LIMITED_INFORMATION,
                                     &parent_handle) &&
          nt_broker_query_child_thread(getpid(), parent_handle,
                                     NT_BROKER_QUERY_LIMITED_INFORMATION, &info) &&
          info.guest_process_id == (uint32_t)target &&
          info.guest_thread_id == (uint32_t)target &&
          info.generation == gen_target,
          "parent-owned capability resolves registered initial guest thread");
    check(nt_broker_open_child_thread(requester, target,
                                     NT_BROKER_QUERY_LIMITED_INFORMATION,
                                     &sibling_handle) &&
          sibling_handle != parent_handle,
          "registered sibling receives a distinct opaque handle");
    struct nt_broker_thread_info unchanged = {0x5555, 0x5555, 0x5555};
    check(!nt_broker_query_child_thread(getpid(), sibling_handle,
                                       NT_BROKER_QUERY_LIMITED_INFORMATION,
                                       &unchanged) &&
          unchanged.guest_thread_id == 0x5555,
          "another process may not borrow sibling capability");
    check(!nt_broker_query_child_thread(requester, sibling_handle,
                                       NT_BROKER_QUERY_INFORMATION, &unchanged) &&
          unchanged.guest_thread_id == 0x5555,
          "a limited-query handle may not gain full query rights");
    check(!nt_broker_close_child_thread(getpid(), sibling_handle),
          "cross-client capability close denied");

    struct nt_thread_broker_registered_peer peer = {
        .child_pid = requester,
        .uid = getuid(),
        .gid = getgid(),
        .child_pidfd = requester_fd,
        .allowed_access = NT_BROKER_QUERY_LIMITED_INFORMATION
    };
    memcpy(peer.secret, requester_secret, 16);
    uint64_t wire_handle = 0x5151;

    /* Attacker inherited endpoint after a separate fork and knows token.
     * Kernel SCM_CREDENTIALS still identify it, not the true requester. */
    const pid_t attacker = fork();
    if (attacker == 0) {
        close(pair[0]);
        _exit(send_identity_query(pair[1], requester_secret) ? 0 : 1);
    }
    int code = 0;
    check(attacker > 0 && waitpid(attacker, &code, 0) == attacker &&
          WIFEXITED(code) && WEXITSTATUS(code) == 0,
          "impostor can send token but not impersonate peer");
    check(!nt_broker_authenticated_open_child(pair[0], &peer, target,
                                               &wire_handle) &&
          wire_handle == 0x5151,
          "AF_UNIX refuses the kernel-credential spoof despite known secret");
    check(write(command[1], "g", 1) == 1, "start authenticated request");
    check(nt_broker_authenticated_open_child(pair[0], &peer, target,
                                             &wire_handle) &&
          wire_handle != 0x5151 &&
          nt_broker_query_child_thread(requester, wire_handle,
                       NT_BROKER_QUERY_LIMITED_INFORMATION, &info) &&
          info.guest_thread_id == (uint32_t)target,
          "real registered sibling opens and queries target through broker");

    atomic_int concurrent_failure = 0;
    struct concurrent_case test = {.target = target, .failed = &concurrent_failure};
    pthread_t workers[4];
    for (size_t i = 0; i < 4; ++i)
        check(pthread_create(&workers[i], NULL, concurrent_open, &test) == 0,
              "start concurrent capability access");
    for (size_t i = 0; i < 4; ++i)
        check(pthread_join(workers[i], NULL) == 0, "join concurrent worker");
    check(atomic_load(&concurrent_failure) == 0,
          "600 open/query/close transactions preserve identities and rights");

    nt_broker_unregister_child(target, gen_target + 1);
    check(nt_broker_query_child_thread(requester, wire_handle,
                       NT_BROKER_QUERY_LIMITED_INFORMATION, &info),
          "wrong generation cannot revoke a live child");
    nt_broker_unregister_child(target, gen_target);
    unchanged = (struct nt_broker_thread_info){0x5555, 0x5555, 0x5555};
    check(!nt_broker_query_child_thread(requester, wire_handle,
                       NT_BROKER_QUERY_LIMITED_INFORMATION, &unchanged) &&
          unchanged.guest_thread_id == 0x5555 &&
          !nt_broker_close_child_thread(requester, wire_handle),
          "revocation invalidates every outstanding handle immediately");
    uint64_t second_generation = 0;
    check(nt_broker_register_child(target, target_fd, getuid(), getgid(),
                                   next_secret, &second_generation) &&
          second_generation > gen_target,
          "re-registration of a live PID requires a new generation");
    out = 0x5151;
    check(!nt_broker_open_child_thread(requester, target,
                                      NT_BROKER_QUERY_LIMITED_INFORMATION, &out) &&
          out == 0x5151,
          "new generation does not inherit another session's ACL");
    check(nt_broker_grant_child_query(requester, target,
                                     NT_BROKER_QUERY_LIMITED_INFORMATION),
          "explicitly authorize fresh registry generation");
    check(nt_broker_open_child_thread(requester, target,
                                     NT_BROKER_QUERY_LIMITED_INFORMATION, &out) &&
          out != wire_handle &&
          nt_broker_query_child_thread(requester, out,
                                      NT_BROKER_QUERY_LIMITED_INFORMATION, &info) &&
          info.generation == second_generation,
          "new capability cannot collide with stale generation");
    check(nt_broker_close_child_thread(requester, out) &&
          !nt_broker_close_child_thread(requester, out),
          "close is requester-scoped and never reusable");

    check(write(command[1], "q", 1) == 1, "release requester");
    close(command[1]);
    close(pair[0]);
    close(pair[1]);
    check(waitpid(requester, &code, 0) == requester &&
          WIFEXITED(code) && WEXITSTATUS(code) == 0,
          "registered requester exits");
    nt_broker_unregister_child(requester, gen_requester);
    check(finish_child(target, target_release),
          "registered target exits");
    out = 0x5151;
    check(!nt_broker_open_child_thread(getpid(), target,
                                      NT_BROKER_QUERY_LIMITED_INFORMATION, &out) &&
          out == 0x5151,
          "a dead pidfd is never trusted even before explicit cleanup");
    nt_broker_unregister_child(target, second_generation);
    close(requester_fd);
    close(target_fd);
    if (!failures)
        puts("PASS registry: two live guest children, authenticated cross-process query, ACL, generations, concurrency and revocation");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
