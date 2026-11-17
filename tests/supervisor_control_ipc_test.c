/* SPDX-License-Identifier: MIT */
/* Real forked Linux control-plane fixture: kernel creds and pidfd, ptrace
 * stopped registered thread, genuine CONTEXT GPR and signal survival.
 * This is NOT a claim of universal Windows PE code compatibility. */
#define _GNU_SOURCE
#include "thread_broker_ipc.h"
#include "nt_services.h"
#include "ntnative/guest_boundary_runtime.h"
#include <asm/prctl.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern const char control_loop_begin[], control_loop_end[];
struct shared {
    struct nt_guest_boundary_session session;
    _Atomic uint32_t tid, ready, stop, host_phase, finish, seen_signal;
    uint32_t test_mode;
    _Atomic uint64_t ticks;
};
static struct shared *data;
static _Thread_local _Alignas(64) unsigned char test_teb[4096];

static void user_signal(int sig)
{
    (void)sig;
    atomic_fetch_add_explicit(&data->seen_signal, 1, memory_order_relaxed);
}
static void sleep_ms(unsigned ms)
{
    struct timespec delay = {.tv_sec = ms/1000, .tv_nsec = (ms%1000)*1000000L};
    while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
}
static void *guest_worker(void *argument)
{
    struct shared *sh = argument;
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, test_teb) != 0 ||
        !nt_guest_boundary_thread_attach((uintptr_t)test_teb))
        _exit(51);
    const uint32_t tid = (uint32_t)syscall(SYS_gettid);
    if (!nt_broker_client_register_thread(tid))
        _exit(52);
    atomic_store_explicit(&sh->tid, tid, memory_order_release);
    /* One negative fixture actually executes RW→RX JIT code, which
     * the parent must NEVER treat as a trusted startup PE section. */
    void *jit = NULL;
    if (sh->test_mode == 4) {
        static const unsigned char loop[] = {
            0xf3,0x0f,0x1e,0xfa, /* ENDBR64 for IBT */
            0x8b,0x07,0x85,0xc0,0x75,0x06,0xf0,0x48,
            0xff,0x06,0xeb,0xf4,0xc3
        };
        jit = mmap(NULL, 4096, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if (jit == MAP_FAILED) _exit(53);
        memcpy(jit, loop, sizeof(loop));
        if (mprotect(jit, 4096, PROT_READ|PROT_EXEC) != 0)
            _exit(54);
    }
    nt_guest_boundary_guest_enter();
    atomic_store_explicit(&sh->ready, 1, memory_order_release);
    if (jit != NULL) {
        void (*dynamic_code)(const _Atomic uint32_t *, _Atomic uint64_t *) = NULL;
        memcpy(&dynamic_code, &jit, sizeof(dynamic_code));
        dynamic_code(&sh->stop, &sh->ticks);
    } else {
    /* Parent uses these independently-known symbols as a *trusted*
     * guest code range. All instructions inside are leaf, lock-free,
     * with no calls into host or libc. */
    __asm__ __volatile__(
        ".globl control_loop_begin\n"
        "control_loop_begin:\n"
        "movl %1, %%eax\n"
        "testl %%eax, %%eax\n"
        "jne 1f\n"
        "lock incq %0\n"
        "jmp control_loop_begin\n"
        "1:\n"
        ".globl control_loop_end\n"
        "control_loop_end:\n"
        : "+m"(sh->ticks)
        : "m"(sh->stop)
        : "eax", "memory");
    }
    nt_guest_boundary_guest_leave();
    if (jit != NULL)
        (void)munmap(jit, 4096);
    atomic_store_explicit(&sh->host_phase, 1, memory_order_release);
    while (!atomic_load_explicit(&sh->finish, memory_order_acquire))
        sched_yield();
    nt_broker_client_unregister_thread(tid);
    nt_guest_boundary_thread_detach();
    return NULL;
}

static _Noreturn void child_client(int fd, unsigned mode)
{
    alarm(20);
    struct sigaction action = {0};
    action.sa_handler = user_signal;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0) _exit(20);
    nt_guest_boundary_install_child_session(&data->session);
    if (!nt_broker_client_attach(fd)) _exit(21);
    pthread_t worker;
    if (pthread_create(&worker, NULL, guest_worker, data) != 0) _exit(22);
    for (int j = 0; j < 4000 && !atomic_load(&data->ready); ++j)
        sleep_ms(1);
    const uint32_t tid = atomic_load(&data->tid);
    if (!tid || !atomic_load(&data->ready)) _exit(23);
    if (mode == 3 || mode == 4) {
        /* Do not pass a JIT/DLL negative test merely because the target
         * has not yet entered its untrusted executable region. Prove it
         * has made substantial progress in that exact hot loop first. */
        for (int j = 0; j < 4000 &&
             atomic_load_explicit(&data->ticks, memory_order_acquire) < 1000; ++j)
            sleep_ms(1);
        if (atomic_load_explicit(&data->ticks, memory_order_acquire) < 1000)
            _exit(55);
    }
    struct nt_broker_thread_info identity = {0};
    uint64_t capability = 0;
    if (!nt_broker_client_supervisor_open(tid,
         NT_BROKER_SUPERVISOR_SUSPEND | NT_BROKER_SUPERVISOR_GET_CONTEXT |
         NT_BROKER_SUPERVISOR_SET_CONTEXT, &identity, &capability) ||
        identity.guest_thread_id != tid || capability == 0)
        _exit(24);
    /* Forged capability must produce a precise NTSTATUS and preserve
     * every caller output byte, including on the extended GET reply. */
    _Alignas(16) struct nt_context64 forged = {0};
    forged.flags = 0x100003u;
    forged.rax = UINT64_C(0xdeadbeef11223344);
    uint32_t untouched_count = UINT32_MAX;
    if (nt_broker_client_supervisor_get_context(capability + 1, &forged) !=
            NT_STATUS_ACCESS_DENIED ||
        forged.rax != UINT64_C(0xdeadbeef11223344) ||
        nt_broker_client_supervisor_suspend(capability + 1, &untouched_count) !=
            NT_STATUS_ACCESS_DENIED ||
        untouched_count != UINT32_MAX)
        _exit(56);
    _Alignas(16) struct nt_context64 before = {0};
    before.flags = 0x100003u; /* AMD64 CONTROL|INTEGER */
    if (nt_broker_client_supervisor_get_context(capability, &before) !=
        NT_STATUS_NOT_SUPPORTED)
        _exit(25); /* GET without a confirmed suspend must be rejected. */
    uint32_t previous = UINT32_MAX;
    if (mode == 0 || mode == 3 || mode == 4) {
        if (nt_broker_client_supervisor_suspend(capability, &previous) !=
            NT_STATUS_NOT_SUPPORTED || previous != UINT32_MAX)
            _exit(26);
    } else {
        nt_status suspended = NT_STATUS_NOT_SUPPORTED;
        for (int attempt = 0; attempt < 6; ++attempt) {
            suspended = nt_broker_client_supervisor_suspend(capability, &previous);
            if (suspended == NT_STATUS_SUCCESS) break;
            sleep_ms(3);
        }
        if (suspended != NT_STATUS_SUCCESS || previous != 0)
            _exit(27);
        const uint64_t frozen = atomic_load(&data->ticks);
        sleep_ms(20);
        if (atomic_load(&data->ticks) != frozen)
            _exit(28);
        if (mode == 2) {
            /* Abrupt authenticated EOF while a real tracee is stopped:
             * parent must revoke and detach without leaving a frozen TID. */
            nt_broker_client_detach();
            for (int j = 0; j < 4000 &&
                 atomic_load(&data->ticks) == frozen; ++j)
                sleep_ms(1);
            if (atomic_load(&data->ticks) == frozen)
                _exit(45);
            atomic_store_explicit(&data->stop, 1, memory_order_release);
            atomic_store_explicit(&data->finish, 1, memory_order_release);
            if (pthread_join(worker, NULL) != 0)
                _exit(46);
            _exit(0);
        }
        if (nt_broker_client_supervisor_suspend(capability, &previous) !=
            NT_STATUS_SUCCESS || previous != 1)
            _exit(29);
        if (nt_broker_client_supervisor_get_context(capability, &before) !=
            NT_STATUS_SUCCESS ||
            before.rip < (uintptr_t)control_loop_begin ||
            before.rip >= (uintptr_t)control_loop_end || before.rsp == 0)
            _exit(30);
        _Alignas(16) struct nt_context64 integer = before;
        integer.flags = 0x100002u;
        const uint64_t original_rax = integer.rax;
        integer.rax = UINT64_C(0x123456789abc);
        if (nt_broker_client_supervisor_set_context(capability, &integer) !=
            NT_STATUS_SUCCESS)
            _exit(31);
        _Alignas(16) struct nt_context64 roundtrip = {0};
        roundtrip.flags = 0x100002u;
        if (nt_broker_client_supervisor_get_context(capability, &roundtrip) !=
            NT_STATUS_SUCCESS || roundtrip.rax != integer.rax)
            _exit(32);
        integer.rax = original_rax;
        if (nt_broker_client_supervisor_set_context(capability, &integer) !=
            NT_STATUS_SUCCESS)
            _exit(33);
        integer.flags = 0x100003u;
        if (nt_broker_client_supervisor_set_context(capability, &integer) !=
            NT_STATUS_NOT_SUPPORTED)
            _exit(34);
        if (nt_broker_client_supervisor_resume(capability, &previous) !=
            NT_STATUS_SUCCESS || previous != 2)
            _exit(35);
        sleep_ms(15);
        if (atomic_load(&data->ticks) != frozen)
            _exit(36);
        /* Queue a genuine kernel signal specifically to the stopped TID.
         * Detach must preserve delivery instead of discarding SIGUSR1. */
        if (syscall(SYS_tgkill, getpid(), tid, SIGUSR1) != 0)
            _exit(37);
        if (nt_broker_client_supervisor_resume(capability, &previous) !=
            NT_STATUS_SUCCESS || previous != 1)
            _exit(38);
        for (int j = 0; j < 4000 &&
             (atomic_load(&data->ticks) == frozen ||
              atomic_load(&data->seen_signal) == 0); ++j)
            sleep_ms(1);
        if (atomic_load(&data->ticks) == frozen ||
            atomic_load(&data->seen_signal) == 0)
            _exit(39);
        if (nt_broker_client_supervisor_resume(capability, &previous) !=
            NT_STATUS_NOT_SUPPORTED)
            _exit(40);
        /* Repeated seize/interrupt/GS/RIP validation/DETACH is not allowed
         * to retain stale signal state or accumulate a hidden tracee. */
        for (unsigned round = 0; round < 32; ++round) {
            if (nt_broker_client_supervisor_suspend(capability, &previous) !=
                NT_STATUS_SUCCESS || previous != 0)
                _exit(47);
            _Alignas(16) struct nt_context64 repeated = {0};
            repeated.flags = 0x100003u;
            if (nt_broker_client_supervisor_get_context(capability, &repeated) !=
                NT_STATUS_SUCCESS ||
                repeated.rip < (uintptr_t)control_loop_begin ||
                repeated.rip >= (uintptr_t)control_loop_end)
                _exit(48);
            if (nt_broker_client_supervisor_resume(capability, &previous) !=
                NT_STATUS_SUCCESS || previous != 1)
                _exit(49);
        }
    }
    atomic_store_explicit(&data->stop, 1, memory_order_release);
    for (int j = 0; j < 4000 && !atomic_load(&data->host_phase); ++j)
        sleep_ms(1);
    if (!atomic_load(&data->host_phase))
        _exit(41);
    if (nt_broker_client_supervisor_suspend(capability, &previous) !=
        NT_STATUS_NOT_SUPPORTED)
        _exit(42); /* HOST phase cannot be ptrace-stopped. */
    atomic_store_explicit(&data->finish, 1, memory_order_release);
    if (pthread_join(worker, NULL) != 0)
        _exit(43);
    struct nt_broker_thread_info stale;
    if (nt_broker_client_supervisor_query(capability,
           NT_BROKER_SUPERVISOR_GET_CONTEXT, &stale))
        _exit(44);
    nt_broker_client_detach();
    _exit(0);
}
static int run_one(unsigned mode)
{
    data = mmap(NULL, sizeof(*data), PROT_READ|PROT_WRITE,
                MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) return 1;
    memset(data, 0, sizeof(*data));
    data->test_mode = mode;
    int sockets[2];
    uint8_t secret[16];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_CLOEXEC, 0, sockets) ||
        !nt_thread_broker_generate_secret(secret)) return 2;
    pid_t child = fork();
    if (child < 0) return 3;
    if (child == 0) {
        close(sockets[0]);
        child_client(sockets[1], mode);
    }
    close(sockets[1]);
    int pidfd = (int)syscall(SYS_pidfd_open, child, 0);
    uint64_t generation = 0;
    if (pidfd < 0 ||
        !nt_broker_register_child(child, pidfd, getuid(), getgid(),
                                  secret, &generation))
        return 4;
    struct nt_thread_broker_registered_peer peer = {
        .child_pid = child, .uid = getuid(), .gid = getgid(),
        .child_pidfd = pidfd,
        .guest_session = &data->session,
        .trusted_code_begin = mode == 3 ?
            (uintptr_t)control_loop_end + 32 : (uintptr_t)control_loop_begin,
        .trusted_code_end = mode == 3 ?
            (uintptr_t)control_loop_end + 64 : (uintptr_t)control_loop_end,
        .allowed_access = NT_BROKER_QUERY_LIMITED_INFORMATION |
            (mode != 0 ? NT_BROKER_SERVER_ENABLE_THREAD_CONTROL : 0u)
    };
    memcpy(peer.secret, secret, sizeof(secret));
    /* server_main owns MAP_SHARED after start */
    if (!nt_broker_server_start(sockets[0], &peer) ||
        !nt_broker_server_handshake(sockets[0], secret))
        return 5;
    int status = 0;
    if (waitpid(child, &status, 0) != child ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL control ipc child exit: enabled=%d status=%d\n",
                mode, status);
        return 6;
    }
    nt_broker_unregister_child(child, generation);
    close(pidfd);
    /* Deliberately do NOT munmap: detached server may still be observing
     * EOF and clearing its own mapping. Process teardown reclaims it. */
    return 0;
}
int main(void)
{
    alarm(35);
    if (run_one(0) || run_one(1) || run_one(2) ||
        run_one(3) || run_one(4)) return 1;
    puts("PASS authenticated control IPC: opt-in only, persistent nested "
         "suspension, real AMD64 CONTEXT GET/SET/restore, unsupported groups, "
         "queued Linux SIGUSR1 preserved, HOST-phase denial, no stale leases, "
         "EOF cleanup, 32 attach cycles, DLL-like out-of-range and real RW→RX JIT denial");
    return 0;
}
