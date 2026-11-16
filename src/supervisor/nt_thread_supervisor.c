/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nt_thread_supervisor.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/elf.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if !defined(__linux__) || !defined(__x86_64__)
#error "nt_thread_supervisor requires Linux AMD64"
#endif

static int read_identity(pid_t process, pid_t tid, pid_t *ppid, pid_t *tgid)
{
    char path[96], line[160];
    if (process <= 1 || tid <= 1) { errno = EINVAL; return -1; }
    int size = snprintf(path, sizeof(path), "/proc/%ld/task/%ld/status", (long)process, (long)tid);
    if (size < 0 || (size_t)size >= sizeof(path)) { errno = EOVERFLOW; return -1; }
    FILE *file = fopen(path, "r");
    if (!file) return -1;
    int have_parent = 0, have_group = 0;
    while (fgets(line, sizeof(line), file)) {
        long value;
        if (sscanf(line, "PPid:%ld", &value) == 1 && value > 0 && value <= INT_MAX) {
            *ppid = (pid_t)value; have_parent = 1;
        }
        if (sscanf(line, "Tgid:%ld", &value) == 1 && value > 0 && value <= INT_MAX) {
            *tgid = (pid_t)value; have_group = 1;
        }
    }
    int close_error = fclose(file);
    if (close_error || !have_parent || !have_group) { errno = EPROTO; return -1; }
    return 0;
}

static int child_is_direct(pid_t child, pid_t tid)
{
    pid_t parent = 0, group = 0;
    if (read_identity(child, child, &parent, &group) || parent != getpid() || group != child) {
        errno = EPERM;
        return -1;
    }
    if (read_identity(child, tid, &parent, &group) || group != child) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

void nt_supervisor_init(struct nt_thread_supervisor *sup)
{
    if (sup) *sup = (struct nt_thread_supervisor){.pidfd = -1};
}

int nt_supervisor_seize_child(struct nt_thread_supervisor *sup, pid_t child, pid_t tid)
{
    if (!sup || sup->seized || sup->pidfd != -1) { errno = EINVAL; return -1; }
    if (child_is_direct(child, tid)) return -1;
#ifdef SYS_pidfd_open
    int pidfd = (int)syscall(SYS_pidfd_open, child, 0);
#else
    int pidfd = -1; errno = ENOTSUP;
#endif
    if (pidfd < 0) return -1;
    struct pollfd pfd = {.fd = pidfd, .events = POLLIN};
    if (poll(&pfd, 1, 0) != 0) { close(pidfd); errno = ESRCH; return -1; }
    if (child_is_direct(child, tid)) { int saved = errno; close(pidfd); errno = saved; return -1; }
    if (ptrace(PTRACE_SEIZE, tid, NULL, (void *)(uintptr_t)PTRACE_O_EXITKILL) == -1) {
        int saved = errno; close(pidfd); errno = saved; return -1;
    }
    *sup = (struct nt_thread_supervisor){.child = child, .tid = tid, .pidfd = pidfd,
                                         .seized = true, .stopped = false};
    return 0;
}

static uint64_t mono_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

int nt_supervisor_interrupt(struct nt_thread_supervisor *sup, unsigned timeout_ms)
{
    if (!sup || !sup->seized) { errno = EINVAL; return -1; }
    if (sup->stopped) return 0;
    if (timeout_ms == 0 || timeout_ms > 10000) { errno = EINVAL; return -1; }
    if (ptrace(PTRACE_INTERRUPT, sup->tid, NULL, NULL) == -1) return -1;
    uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        int status = 0;
        pid_t observed = waitpid(sup->tid, &status, __WALL | WNOHANG);
        if (observed == sup->tid) {
            if (WIFSTOPPED(status) && (unsigned)status >> 16 == PTRACE_EVENT_STOP) {
                sup->stopped = true;
                return 0;
            }
            /* An unexpected signal/event is not safe to silently consume. */
            errno = EPROTO; return -1;
        }
        if (observed < 0) return -1;
        if (mono_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        (void)nanosleep(&pause, NULL);
    }
}

int nt_supervisor_get_registers(struct nt_thread_supervisor *sup, struct user_regs_struct *out)
{
    if (!sup || !sup->seized || !sup->stopped || !out) { errno = EINVAL; return -1; }
    struct iovec vec = {.iov_base = out, .iov_len = sizeof(*out)};
    if (ptrace(PTRACE_GETREGSET, sup->tid, (void *)(uintptr_t)NT_PRSTATUS, &vec) == -1) return -1;
    if (vec.iov_len != sizeof(*out)) { errno = EPROTO; return -1; }
    return 0;
}

int nt_supervisor_set_rax(struct nt_thread_supervisor *sup, uint64_t value)
{
    struct user_regs_struct regs;
    if (nt_supervisor_get_registers(sup, &regs)) return -1;
    regs.rax = value;
    struct iovec vec = {.iov_base = &regs, .iov_len = sizeof(regs)};
    if (ptrace(PTRACE_SETREGSET, sup->tid, (void *)(uintptr_t)NT_PRSTATUS, &vec) == -1) return -1;
    if (vec.iov_len != sizeof(regs)) { errno = EPROTO; return -1; }
    return 0;
}

int nt_supervisor_continue(struct nt_thread_supervisor *sup)
{
    if (!sup || !sup->seized || !sup->stopped) { errno = EINVAL; return -1; }
    if (ptrace(PTRACE_CONT, sup->tid, NULL, NULL) == -1) return -1;
    sup->stopped = false;
    return 0;
}

int nt_supervisor_detach(struct nt_thread_supervisor *sup)
{
    if (!sup || !sup->seized || !sup->stopped) { errno = EINVAL; return -1; }
    if (ptrace(PTRACE_DETACH, sup->tid, NULL, NULL) == -1) return -1;
    sup->seized = false;
    sup->stopped = false;
    return 0;
}

void nt_supervisor_release(struct nt_thread_supervisor *sup)
{
    if (!sup) return;
    if (sup->pidfd >= 0) (void)close(sup->pidfd);
    nt_supervisor_init(sup);
}
