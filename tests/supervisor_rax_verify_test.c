/* SPDX-License-Identifier: MIT */
/* Real Linux AMD64 ptrace regression for the direct RAX fixture helper.
 * Two separately built binaries inject a post-write readback mismatch;
 * the fatal variant also refuses rollback. Test macros are never built
 * into production binaries and cannot be set by a guest process. */
#define _GNU_SOURCE
#include "nt_thread_supervisor.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef NT_SUP_TEST_FORCE_RAX_VERIFY_MISMATCH
#error "Test requires a forced RAX readback mismatch"
#endif

_Atomic uint32_t nt_sup_test_rax_verify_corrupted;

struct shared_state {
    _Atomic unsigned ready;
    _Atomic unsigned stop;
    _Atomic uint64_t ticks;
};

static void pause_ms(unsigned ms)
{
    struct timespec delay = {.tv_sec = ms / 1000,
                             .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static _Noreturn void child_main(struct shared_state *shared)
{
    atomic_store_explicit(&shared->ready, 1, memory_order_release);
    while (!atomic_load_explicit(&shared->stop, memory_order_acquire))
        atomic_fetch_add_explicit(&shared->ticks, 1, memory_order_relaxed);
    _exit(0);
}

static int wait_for_child(pid_t child, int *status)
{
    for (unsigned i = 0; i < 4000; ++i) {
        pid_t observed = waitpid(child, status, WNOHANG);
        if (observed == child) return 1;
        if (observed < 0 && errno != EINTR) return 0;
        pause_ms(1);
    }
    return 0;
}

int main(void)
{
    alarm(15);
    struct shared_state *shared = mmap(NULL, sizeof(*shared),
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) { perror("mmap"); return 1; }
    memset(shared, 0, sizeof(*shared));
    pid_t child = fork();
    if (child < 0) { perror("fork"); munmap(shared, sizeof(*shared)); return 1; }
    if (child == 0) child_main(shared);

    struct nt_thread_supervisor sup;
    nt_supervisor_init(&sup);
    int rc = 1, status = 0;
    for (unsigned i = 0; i < 4000 &&
         (!atomic_load_explicit(&shared->ready, memory_order_acquire) ||
          atomic_load_explicit(&shared->ticks, memory_order_relaxed) < 1000); ++i)
        pause_ms(1);
    if (atomic_load_explicit(&shared->ticks, memory_order_relaxed) < 1000) {
        fputs("FAIL guest did not run\n", stderr);
        goto cleanup;
    }
    if (nt_supervisor_seize_child(&sup, child, child) != 0) {
        if (errno == EPERM || errno == EACCES || errno == ENOTSUP ||
            errno == ENOSYS) {
            rc = 77;
            fputs("INCOMPLETE: ptrace denied by host\n", stderr);
        } else perror("FAIL ptrace seize");
        goto cleanup;
    }
    if (nt_supervisor_interrupt(&sup, 1500) != 0) {
        perror("FAIL ptrace stop");
        goto cleanup;
    }

    struct user_regs_struct original = {0};
    if (nt_supervisor_get_registers(&sup, &original) != 0) {
        perror("FAIL initial registers");
        goto cleanup;
    }
    const uint64_t modified_rax = original.rax ^ UINT64_C(0x1234556677889abc);
    errno = 0;
    const int write_rc = nt_supervisor_set_rax(&sup, modified_rax);
    const int write_error = errno;
    if (write_rc != -1 ||
        atomic_load_explicit(&nt_sup_test_rax_verify_corrupted,
                             memory_order_relaxed) != 1) {
        fputs("FAIL direct RAX write claimed success after bad readback\n", stderr);
        goto cleanup;
    }

#ifdef NT_SUP_TEST_FORCE_RAX_ROLLBACK_FAILURE
    if (write_error != EIO || !sup.compromised ||
        nt_supervisor_continue(&sup) != -1 || errno != EPERM ||
        nt_supervisor_detach(&sup) != -1 || errno != EPERM ||
        nt_supervisor_set_rax(&sup, original.rax) != -1 || errno != EPERM) {
        fputs("FAIL compromised RAX state permitted mutation/detach\n", stderr);
        goto cleanup;
    }
    /* SIGKILL is delivered by pidfd; never release the uncertain GPRs. */
    nt_supervisor_release(&sup);
    if (!wait_for_child(child, &status) ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        fputs("FAIL quarantine did not kill direct child\n", stderr);
        goto cleanup;
    }
    nt_supervisor_release(&sup); /* pidfd signalled, safe to relinquish */
    puts("PASS direct RAX: failed rollback quarantined and pidfd killed child");
#else
    struct user_regs_struct after = {0};
    if (write_error != EPROTO || sup.compromised ||
        nt_supervisor_get_registers(&sup, &after) != 0 ||
        memcmp(&original, &after, sizeof(original)) != 0) {
        fputs("FAIL direct RAX rollback did not restore exact GPR state\n", stderr);
        goto cleanup;
    }
    if (nt_supervisor_detach(&sup) != 0) {
        perror("FAIL detach after valid rollback");
        goto cleanup;
    }
    nt_supervisor_release(&sup);
    atomic_store_explicit(&shared->stop, 1, memory_order_release);
    if (!wait_for_child(child, &status) ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fputs("FAIL child did not resume after valid rollback\n", stderr);
        goto cleanup;
    }
    puts("PASS direct RAX: failed verification restored exact original GPRs");
#endif
    rc = 0;
cleanup:
    if (rc != 0) {
        if (sup.seized)
            (void)nt_supervisor_terminate_compromised_child(&sup);
        nt_supervisor_release(&sup);
        (void)kill(child, SIGKILL); /* Direct child created by this fixture. */
        (void)waitpid(child, &status, 0);
    }
    (void)munmap(shared, sizeof(*shared));
    return rc;
}
