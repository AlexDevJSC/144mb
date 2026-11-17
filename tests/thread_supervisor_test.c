/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nt_thread_supervisor.h"
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct shared_state {
    _Atomic uint64_t worker_ticks;
    _Atomic uint64_t leader_ticks;
    _Atomic bool exit_requested;
};
struct fixture { struct shared_state *shared; int ready_fd; };

static void pause_ms(long ms)
{
    struct timespec t = {.tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000};
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

static void *work_thread(void *arg)
{
    struct fixture *fixture = arg;
    pid_t tid = (pid_t)syscall(SYS_gettid);
    if (write(fixture->ready_fd, &tid, sizeof(tid)) != sizeof(tid)) _exit(41);
    (void)close(fixture->ready_fd);
    while (!atomic_load_explicit(&fixture->shared->exit_requested, memory_order_relaxed))
        atomic_fetch_add_explicit(&fixture->shared->worker_ticks, 1, memory_order_relaxed);
    return NULL;
}

static _Noreturn void child_main(struct shared_state *shared, int ready_fd)
{
    struct fixture fixture = {.shared = shared, .ready_fd = ready_fd};
    pthread_t thread;
    if (pthread_create(&thread, NULL, work_thread, &fixture)) _exit(40);
    while (!atomic_load_explicit(&shared->exit_requested, memory_order_relaxed))
        atomic_fetch_add_explicit(&shared->leader_ticks, 1, memory_order_relaxed);
    if (pthread_join(thread, NULL)) _exit(42);
    _exit(0);
}

static int read_tid(int fd, pid_t *tid)
{
    struct pollfd entry = {.fd = fd, .events = POLLIN};
    if (poll(&entry, 1, 2500) != 1 || !(entry.revents & POLLIN)) return -1;
    return read(fd, tid, sizeof(*tid)) == sizeof(*tid) ? 0 : -1;
}

int main(void)
{
    struct shared_state *shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (shared == MAP_FAILED) { perror("mmap"); return 1; }
    memset(shared, 0, sizeof(*shared));
    int ready[2];
    if (pipe(ready)) { perror("pipe"); return 1; }
    pid_t child = fork();
    if (child == -1) { perror("fork"); return 1; }
    if (child == 0) {
        (void)close(ready[0]);
        child_main(shared, ready[1]);
    }
    (void)close(ready[1]);
    pid_t tid = 0;
    int rc = 1, wait_status = 0;
    struct nt_thread_supervisor sup;
    nt_supervisor_init(&sup);
    if (read_tid(ready[0], &tid)) { fputs("FAIL worker did not start\n", stderr); goto cleanup; }
    (void)close(ready[0]); ready[0] = -1;

    /* Must not allow the supervisor to attach to its own unrelated thread. */
    if (nt_supervisor_seize_child(&sup, child, getpid()) == 0) {
        fputs("FAIL supervisor accepted an unrelated TID\n", stderr); goto cleanup;
    }
    if (nt_supervisor_seize_child(&sup, child, tid)) {
        if (errno == EPERM || errno == EACCES || errno == ENOTSUP || errno == ENOSYS) {
            fprintf(stderr, "INCOMPLETE ptrace denied by host policy: %s\n", strerror(errno));
            rc = 77; goto cleanup;
        }
        perror("FAIL seize"); goto cleanup;
    }
    pause_ms(20);
    uint64_t before = atomic_load(&shared->worker_ticks);
    if (before == 0) { fputs("FAIL worker did not run\n", stderr); goto cleanup; }
    for (int cycle = 0; cycle < 2; ++cycle) {
        if (nt_supervisor_interrupt(&sup, 1500)) { perror("FAIL interrupt"); goto cleanup; }
        struct user_regs_struct original, changed;
        if (nt_supervisor_get_registers(&sup, &original) || original.rip == 0 || original.rsp == 0) {
            fputs("FAIL registers invalid\n", stderr); goto cleanup;
        }
        uint64_t worker = atomic_load(&shared->worker_ticks);
        uint64_t leader = atomic_load(&shared->leader_ticks);
        pause_ms(25);
        if (atomic_load(&shared->worker_ticks) != worker ||
            atomic_load(&shared->leader_ticks) <= leader) {
            fputs("FAIL worker did not stop independently\n", stderr); goto cleanup;
        }
        if (nt_supervisor_set_rax(&sup, UINT64_C(0x1122334455667788)) ||
            nt_supervisor_get_registers(&sup, &changed) ||
            changed.rax != UINT64_C(0x1122334455667788) ||
            changed.rip != original.rip || changed.rsp != original.rsp ||
            nt_supervisor_set_rax(&sup, original.rax)) {
            fputs("FAIL RAX roundtrip\n", stderr); goto cleanup;
        }
        if (nt_supervisor_continue(&sup)) { perror("FAIL continue"); goto cleanup; }
        pause_ms(25);
        if (atomic_load(&shared->worker_ticks) <= worker) {
            fputs("FAIL worker did not resume\n", stderr); goto cleanup;
        }
    }
    if (nt_supervisor_interrupt(&sup, 1500) || nt_supervisor_detach(&sup)) {
        perror("FAIL detach"); goto cleanup;
    }
    /* Re-seize the same still-live child. Release from a STOPPED state
     * MUST detach it, not merely close pidfd and strand it forever. */
    if (nt_supervisor_seize_child(&sup, child, tid) ||
        nt_supervisor_interrupt(&sup, 1500)) {
        perror("FAIL re-seize before forced release");
        goto cleanup;
    }
    const uint64_t progress_before_release = atomic_load(&shared->worker_ticks);
    nt_supervisor_release(&sup);
    pause_ms(30);
    if (sup.seized || sup.pidfd != -1 ||
        atomic_load(&shared->worker_ticks) <= progress_before_release) {
        fputs("FAIL release left a guest worker ptrace-stopped\\n", stderr);
        goto cleanup;
    }
    atomic_store(&shared->exit_requested, true);
    for (int i = 0; i < 300; ++i) {
        pid_t waited = waitpid(child, &wait_status, WNOHANG);
        if (waited == child) {
            if (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0) {
                fputs("FAIL child exit\n", stderr); goto cleanup_no_wait;
            }
            puts("PASS supervisor: child-only TID, async stop, independent leader, GETREGSET/SETREGSET, resume twice, detach, forced stopped release and clean exit");
            rc = 0; goto cleanup_no_wait;
        }
        if (waited < 0) break;
        pause_ms(10);
    }
    fputs("FAIL child did not exit\n", stderr);
cleanup:
    atomic_store(&shared->exit_requested, true);
    /* If attached, killing the child avoids stranded ptrace stops. */
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &wait_status, 0);
cleanup_no_wait:
    if (ready[0] >= 0) (void)close(ready[0]);
    nt_supervisor_release(&sup);
    (void)munmap(shared, sizeof(*shared));
    return rc;
}
