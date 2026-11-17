/* SPDX-License-Identifier: MIT */
/* Opt-in trusted integration fixture. Loads a real MinGW-built PE32+,
 * executes the guest entry on a thread with its actual GS/TEB, and
 * supervises only that direct child's guest thread. */
#define _GNU_SOURCE
#include "image.h"
#include "ntnative/guest_thread.h"
#include "nt_thread_supervisor.h"
#include "ntnative/guest_boundary_runtime.h"
#include "nt_supervisor_nt_bridge.h"
#include "supervisor_pe_shared.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct guest_worker {
    const struct pe_image *image;
    struct nt_supervisor_pe_shared *shared;
    int ready_fd;
};

static void pause_ms(long ms)
{
    struct timespec value = {.tv_sec = ms / 1000,
                             .tv_nsec = (ms % 1000) * 1000000L};
    while (nanosleep(&value, &value) && errno == EINTR) {}
}

static bool read_pe_fixture(const char *path, unsigned char **bytes, size_t *length)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat statbuf;
    if (fstat(fd, &statbuf) || !S_ISREG(statbuf.st_mode) ||
        statbuf.st_size <= 0 || statbuf.st_size > PE_IMAGE_MAX_BYTES) {
        close(fd);
        return false;
    }
    *length = (size_t)statbuf.st_size;
    *bytes = malloc(*length);
    if (!*bytes) { close(fd); return false; }
    size_t done = 0;
    while (done < *length) {
        ssize_t n = read(fd, *bytes + done, *length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    close(fd);
    if (done == *length) return true;
    free(*bytes);
    *bytes = NULL;
    return false;
}

/* The guest voluntarily enters this controlled Win64-ABI host import.
 * While the host lock is held, a pause request MUST fail, even though
 * the thread's GS still points at its guest TEB. */
static pthread_mutex_t callback_host_lock = PTHREAD_MUTEX_INITIALIZER;

static void __attribute__((ms_abi)) controlled_host_callback(struct nt_supervisor_pe_shared *shared)
{
    if (nt_guest_boundary_current_phase() != NT_GUEST_BOUNDARY_HOST ||
        nt_guest_boundary_host_nesting() == 0)
        _exit(66);
    if (pthread_mutex_lock(&callback_host_lock) != 0) _exit(61);
    atomic_store_explicit(&shared->host_call_entered, 1u, memory_order_release);
    while (!atomic_load_explicit(&shared->host_call_release, memory_order_acquire))
        pause_ms(1);
    if (pthread_mutex_unlock(&callback_host_lock) != 0) _exit(62);
}

static void *guest_worker_main(void *arg)
{
    struct guest_worker *worker = arg;
    struct nt_guest_thread state = {0};
    if (!nt_guest_thread_enter(&state, (uintptr_t)worker->image->base))
        _exit(53);
    if (!nt_guest_boundary_thread_attach((uintptr_t)nt_guest_thread_teb()))
        _exit(67);
    atomic_store_explicit(&worker->shared->guest_teb,
                          (uintptr_t)nt_guest_thread_teb(), memory_order_release);
    const uintptr_t target = (uintptr_t)worker->image->base + worker->image->entry_rva;
    int (__attribute__((ms_abi)) *guest_entry)(struct nt_supervisor_pe_shared *);
    memcpy(&guest_entry, &target, sizeof(guest_entry));
    const pid_t tid = (pid_t)syscall(SYS_gettid);
    if (write(worker->ready_fd, &tid, sizeof(tid)) != (ssize_t)sizeof(tid))
        _exit(54);
    close(worker->ready_fd);
    nt_guest_boundary_guest_enter();
    const int result = guest_entry(worker->shared);
    nt_guest_boundary_guest_leave();
    atomic_store_explicit(&worker->shared->guest_exit_code, (uint32_t)result,
                          memory_order_release);
    if (!nt_guest_thread_leave(&state))
        _exit(55);
    return NULL;
}

static _Noreturn void child_main(const char *path,
                                 struct nt_supervisor_pe_shared *shared,
                                 int ready_fd)
{
    nt_guest_boundary_install_child_session(&shared->session);
    nt_guest_boundary_enable_import_gate();
    unsigned char *file = NULL;
    size_t length = 0;
    if (!read_pe_fixture(path, &file, &length)) _exit(50);
    struct pe_image image = {0};
    char error[192] = {0};
    const bool loaded = pe_image_load(file, length, NULL, NULL,
                                      &image, error, sizeof(error));
    free(file);
    if (!loaded) {
        fprintf(stderr, "FAIL controlled PE load: %s\n", error);
        _exit(51);
    }
    if (image.code_size == 0 ||
        image.entry_rva < image.code_rva ||
        image.entry_rva - image.code_rva >= image.code_size ||
        (uintptr_t)image.base > UINTPTR_MAX - image.code_rva - image.code_size)
        _exit(52);
    atomic_store_explicit(&shared->code_begin,
                          (uintptr_t)image.base + image.code_rva, memory_order_release);
    atomic_store_explicit(&shared->code_end,
                          (uintptr_t)image.base + image.code_rva + image.code_size,
                          memory_order_release);
    /* Function pointers cross this boundary only inside a trusted test:
     * do not expose callbacks or raw Linux function addresses to PE input. */
    void (__attribute__((ms_abi)) *callback)(struct nt_supervisor_pe_shared *) =
        controlled_host_callback;
    uintptr_t callback_address = 0;
    _Static_assert(sizeof(callback) == sizeof(callback_address), "AMD64 callback pointer");
    nt_guest_proc native_callback = NULL;
    memcpy(&native_callback, &callback, sizeof(native_callback));
    nt_guest_proc gated_callback = nt_guest_boundary_wrap_import(native_callback);
    if (!gated_callback)
        _exit(68);
    memcpy(&callback_address, &gated_callback, sizeof(gated_callback));
    atomic_store_explicit(&shared->host_callback, callback_address, memory_order_release);
    /* Alternate guard-paged stack is owned solely by the controlled
     * guest APC trampoline; never substitute the interrupted host stack. */
    const size_t stack_bytes = 65536;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || page_size >= (long)stack_bytes)
        _exit(63);
    void *apc_stack = mmap(NULL, stack_bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (apc_stack == MAP_FAILED ||
        mprotect(apc_stack, (size_t)page_size, PROT_NONE) != 0)
        _exit(64);
    atomic_store_explicit(&shared->apc_stack_top,
                          (uintptr_t)apc_stack + stack_bytes, memory_order_release);
    struct guest_worker worker = {.image = &image, .shared = shared, .ready_fd = ready_fd};
    pthread_t thread;
    if (pthread_create(&thread, NULL, guest_worker_main, &worker))
        _exit(56);
    while (!atomic_load_explicit(&shared->stop_requested, memory_order_acquire))
        atomic_fetch_add_explicit(&shared->host_ticks, 1, memory_order_relaxed);
    if (pthread_join(thread, NULL)) _exit(57);
    const uint32_t result = atomic_load_explicit(&shared->guest_exit_code,
                                                  memory_order_acquire);
    if (!atomic_load_explicit(&shared->guest_finished, memory_order_acquire))
        _exit(58);
    if (munmap(apc_stack, stack_bytes) != 0)
        _exit(65);
    pe_image_unload(&image);
    _exit(result == 0 ? 0 : 59);
}

static bool read_child_tid(int fd, pid_t *tid)
{
    struct pollfd pollfd = {.fd = fd, .events = POLLIN};
    if (poll(&pollfd, 1, 2500) != 1 || !(pollfd.revents & POLLIN)) return false;
    return read(fd, tid, sizeof(*tid)) == (ssize_t)sizeof(*tid);
}

static bool wait_for_guest_ticks(struct nt_supervisor_pe_shared *state, uint64_t previous)
{
    for (int i = 0; i < 300; ++i) {
        if (atomic_load_explicit(&state->guest_ticks, memory_order_acquire) > previous)
            return true;
        pause_ms(5);
    }
    return false;
}

static bool wait_for_value(_Atomic uint32_t *value, uint32_t expected)
{
    for (int i = 0; i < 300; ++i) {
        if (atomic_load_explicit(value, memory_order_acquire) == expected)
            return true;
        pause_ms(5);
    }
    return false;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("Usage: supervisor-pe-thread-test BUILD_GENERATED_PE.exe\n", stderr);
        return 2;
    }
    struct nt_supervisor_pe_shared *shared =
        mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (shared == MAP_FAILED) { perror("mmap"); return 1; }
    memset(shared, 0, sizeof(*shared));
    int ready[2] = {-1, -1};
    if (pipe(ready)) { perror("pipe"); munmap(shared, sizeof(*shared)); return 1; }
    pid_t child = fork();
    if (child < 0) { perror("fork"); close(ready[0]); close(ready[1]); return 1; }
    if (child == 0) {
        close(ready[0]);
        child_main(argv[1], shared, ready[1]);
    }
    close(ready[1]);
    ready[1] = -1;
    int rc = 1, status = 0;
    struct nt_thread_supervisor sup;
    nt_supervisor_init(&sup);
    pid_t tid = 0;
    if (!read_child_tid(ready[0], &tid)) {
        fputs("FAIL PE guest thread did not initialize\n", stderr);
        goto cleanup;
    }
    close(ready[0]); ready[0] = -1;
    if (!wait_for_guest_ticks(shared, 1000)) {
        fputs("FAIL real PE code did not execute\n", stderr);
        goto cleanup;
    }
    const uintptr_t begin = atomic_load_explicit(&shared->code_begin, memory_order_acquire);
    const uintptr_t end = atomic_load_explicit(&shared->code_end, memory_order_acquire);
    const uintptr_t teb = atomic_load_explicit(&shared->guest_teb, memory_order_acquire);
    if (begin == 0 || begin >= end || teb == 0 || tid == child) {
        fputs("FAIL invalid guest image/TEB/TID identity\n", stderr);
        goto cleanup;
    }
    const struct nt_guest_boundary_slot *slot =
        nt_guest_boundary_find_thread(&shared->session, tid);
    if (!slot || atomic_load_explicit(&slot->teb, memory_order_acquire) != teb) {
        fputs("FAIL real runtime boundary did not register the guest TID and TEB\\n", stderr);
        goto cleanup;
    }
    const struct nt_supervisor_guest_scope scope = {
        .boundary = &slot->boundary,
        .code_begin = begin, .code_end = end, .teb = teb
    };
    if (nt_supervisor_seize_child(&sup, child, tid)) {
        if (errno == EPERM || errno == EACCES || errno == ENOTSUP || errno == ENOSYS) {
            fprintf(stderr, "INCOMPLETE PE ptrace denied by host policy: %s\n",
                    strerror(errno));
            rc = 77;
        } else perror("FAIL PE supervisor seize");
        goto cleanup;
    }
    /* Contract adapter for NT/Win32 suspension/context semantics:
     * rights, nested counts, authentic CORE+INTEGER context, and
     * verified integer register writes. This runs in the external
     * supervisor, not yet through the PE's production import table. */
    struct nt_supervisor_nt_thread nt = {
        .tracer = &sup, .scope = scope,
        .granted_access = NT_SUP_THREAD_SUSPEND_RESUME |
                          NT_SUP_THREAD_GET_CONTEXT | NT_SUP_THREAD_SET_CONTEXT
    };
    uint32_t previous = 999;
    nt.granted_access &= ~NT_SUP_THREAD_SUSPEND_RESUME;
    if (nt_supervisor_nt_suspend_thread(&nt, &previous) != NT_STATUS_ACCESS_DENIED) {
        fputs("FAIL NT thread-rights gate\\n", stderr);
        goto cleanup;
    }
    nt.granted_access |= NT_SUP_THREAD_SUSPEND_RESUME;
    if (nt_supervisor_nt_suspend_thread(&nt, &previous) != NT_STATUS_SUCCESS ||
        previous != 0 ||
        nt_supervisor_nt_suspend_thread(&nt, &previous) != NT_STATUS_SUCCESS ||
        previous != 1 || nt.suspend_count != 2) {
        fputs("FAIL NT nested suspension semantics\\n", stderr);
        goto cleanup;
    }
    _Alignas(16) struct nt_context64 context = {0};
    context.flags = NT_SUP_CONTEXT_AMD64 | NT_SUP_CONTEXT_CONTROL |
                    NT_SUP_CONTEXT_INTEGER;
    if (nt_supervisor_nt_get_context(&nt, &context) != NT_STATUS_SUCCESS ||
        context.rip < begin || context.rip >= end || context.rsp == 0) {
        fputs("FAIL NT AMD64 context capture\\n", stderr);
        goto cleanup;
    }
    _Alignas(16) struct nt_context64 integer = context;
    integer.flags = NT_SUP_CONTEXT_AMD64 | NT_SUP_CONTEXT_INTEGER;
    const uint64_t original_rax = integer.rax;
    integer.rax = UINT64_C(0xaabbccdd11223344);
    if (nt_supervisor_nt_set_context(&nt, &integer) != NT_STATUS_SUCCESS) {
        fputs("FAIL NT AMD64 integer context write\\n", stderr);
        goto cleanup;
    }
    _Alignas(16) struct nt_context64 readback = {0};
    readback.flags = NT_SUP_CONTEXT_AMD64 | NT_SUP_CONTEXT_INTEGER;
    if (nt_supervisor_nt_get_context(&nt, &readback) != NT_STATUS_SUCCESS ||
        readback.rax != integer.rax) {
        fputs("FAIL NT AMD64 context readback\\n", stderr);
        goto cleanup;
    }
    integer.rax = original_rax;
    if (nt_supervisor_nt_set_context(&nt, &integer) != NT_STATUS_SUCCESS) {
        fputs("FAIL NT AMD64 integer context restore\\n", stderr);
        goto cleanup;
    }
    const uint64_t frozen_ticks = atomic_load(&shared->guest_ticks);
    if (nt_supervisor_nt_resume_thread(&nt, &previous) != NT_STATUS_SUCCESS ||
        previous != 2 || nt.suspend_count != 1) {
        fputs("FAIL NT partial resume\\n", stderr);
        goto cleanup;
    }
    pause_ms(20);
    if (atomic_load(&shared->guest_ticks) != frozen_ticks ||
        nt_supervisor_nt_resume_thread(&nt, &previous) != NT_STATUS_SUCCESS ||
        previous != 1 || nt.suspend_count != 0 ||
        !wait_for_guest_ticks(shared, frozen_ticks)) {
        fputs("FAIL NT final resume\\n", stderr);
        goto cleanup;
    }
    for (int cycle = 0; cycle < 2; ++cycle) {
        struct user_regs_struct original, modified, restored;
        if (nt_supervisor_interrupt_guest(&sup, &scope, 1500, &original)) {
            perror("FAIL guarded PE interrupt"); goto cleanup;
        }
        if (nt_supervisor_get_guest_context(&sup, &scope, &original)) {
            perror("FAIL PE GETREGSET"); goto cleanup;
        }
        if (original.rip < begin || original.rip >= end || original.rsp == 0 ||
            original.gs_base != teb) {
            fprintf(stderr, "FAIL stopped outside PE guest or with incorrect GS: "
                    "rip=%llx code=[%llx,%llx) gs=%llx expected=%llx\n",
                    (unsigned long long)original.rip, (unsigned long long)begin,
                    (unsigned long long)end, (unsigned long long)original.gs_base,
                    (unsigned long long)teb);
            goto cleanup;
        }
        const uint64_t guest_before = atomic_load_explicit(&shared->guest_ticks,
                                                            memory_order_acquire);
        const uint64_t host_before = atomic_load_explicit(&shared->host_ticks,
                                                           memory_order_acquire);
        pause_ms(60);
        if (atomic_load_explicit(&shared->guest_ticks, memory_order_acquire) != guest_before ||
            atomic_load_explicit(&shared->host_ticks, memory_order_acquire) <= host_before) {
            fputs("FAIL PE thread not independently stopped\n", stderr);
            goto cleanup;
        }
        if (nt_supervisor_set_guest_rax(&sup, &scope, UINT64_C(0x1122334455667788)) ||
            nt_supervisor_get_registers(&sup, &modified) ||
            modified.rax != UINT64_C(0x1122334455667788) ||
            modified.rip != original.rip || modified.rsp != original.rsp ||
            modified.gs_base != original.gs_base ||
            nt_supervisor_set_guest_rax(&sup, &scope, original.rax) ||
            nt_supervisor_get_registers(&sup, &restored) ||
            restored.rax != original.rax) {
            fputs("FAIL PE context SETREGSET/restore\n", stderr);
            goto cleanup;
        }
        if (nt_supervisor_continue(&sup)) { perror("FAIL PE continue"); goto cleanup; }
        if (!wait_for_guest_ticks(shared, guest_before)) {
            fputs("FAIL PE did not resume execution\n", stderr);
            goto cleanup;
        }
    }
    /* The same PE transitions into a native callback that holds a Linux
     * mutex. A raw ptrace stop could strand it; the guest gate must
     * reject, automatically resume, and not report successful suspension.
     * The second attempt forges the phase to GUEST: actual RIP must still
     * independently reject the host callback. */
    atomic_store_explicit(&shared->request_host_call, 1u, memory_order_release);
    if (!wait_for_value(&shared->host_call_entered, 1u) ||
        !wait_for_value(&slot->boundary.phase, NT_GUEST_BOUNDARY_HOST)) {
        fputs("FAIL controlled PE did not enter host critical section\\n", stderr);
        goto cleanup;
    }
    struct user_regs_struct rejected = {0};
    errno = 0;
    if (nt_supervisor_interrupt_guest(&sup, &scope, 1500, &rejected) != -1 ||
        errno != EBUSY || sup.stopped) {
        fputs("FAIL host-locked thread was accepted as guest-safe\\n", stderr);
        goto cleanup;
    }
    atomic_store_explicit(&slot->boundary.phase, NT_GUEST_BOUNDARY_GUEST,
                          memory_order_release);
    errno = 0;
    if (nt_supervisor_interrupt_guest(&sup, &scope, 1500, &rejected) != -1 ||
        errno != EBUSY || sup.stopped) {
        fputs("FAIL forged phase bypassed host RIP check\\n", stderr);
        goto cleanup;
    }
    nt_guest_boundary_publish((struct nt_guest_boundary *)&slot->boundary, NT_GUEST_BOUNDARY_HOST);
    atomic_store_explicit(&shared->host_call_release, 1u, memory_order_release);
    if (!wait_for_value(&slot->boundary.phase, NT_GUEST_BOUNDARY_GUEST)) {
        fputs("FAIL host callback did not return to PE\\n", stderr);
        goto cleanup;
    }
    const uint64_t after_callback = atomic_load(&shared->guest_ticks);
    if (!wait_for_guest_ticks(shared, after_callback)) {
        fputs("FAIL PE did not progress after host critical boundary\\n", stderr);
        goto cleanup;
    }
    /* A genuine asynchronous APC-like transfer: no SleepEx or alertable
     * wait is involved. Only an import-free trusted PE trampoline is
     * allowed; it signals INT3, and the supervisor restores complete
     * XSTATE and original registers before returning to the interrupted
     * PE instruction. This is not general Win32 APC support. */
    const uintptr_t apc_entry =
        atomic_load_explicit(&shared->apc_entry, memory_order_acquire);
    const uintptr_t apc_stack_top =
        atomic_load_explicit(&shared->apc_stack_top, memory_order_acquire);
    if (apc_entry < scope.code_begin || apc_entry >= scope.code_end ||
        !apc_stack_top) {
        fputs("FAIL cannot prepare controlled guest APC\\n", stderr);
        goto cleanup;
    }
    const uint64_t guest_before_apc = atomic_load(&shared->guest_ticks);
    if (nt_supervisor_nt_queue_trusted_apc(&nt, apc_entry, apc_stack_top,
                                            (uintptr_t)shared,
                                            &shared->apc_delivered) != NT_STATUS_SUCCESS) {
        fputs("FAIL supervised NT special APC delivery\\n", stderr);
        goto cleanup;
    }
    if (atomic_load_explicit(&shared->apc_delivered, memory_order_acquire) != 1u ||
        !wait_for_guest_ticks(shared, guest_before_apc)) {
        fputs("FAIL guest did not resume after asynchronous APC\\n", stderr);
        goto cleanup;
    }
    if (nt_supervisor_interrupt_guest(&sup, &scope, 1500, &rejected) ||
        nt_supervisor_detach(&sup)) {
        perror("FAIL PE detach"); goto cleanup;
    }
    atomic_store_explicit(&shared->stop_requested, 1u, memory_order_release);
    for (int i = 0; i < 300; ++i) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
                !atomic_load_explicit(&shared->guest_finished, memory_order_acquire)) {
                fputs("FAIL PE guest did not return cleanly\n", stderr);
                goto finished;
            }
            puts("PASS supervisor PE32+: guest-only RIP/GS boundary, real async stop, "
                 "AMD64 GET/SETREGSET, host-mutex rejection, forged-phase rejection, "
                 "NT suspend/context adapter, asynchronous APC/XSTATE restore, detach and clean return");
            rc = 0;
            goto finished;
        }
        if (waited < 0) break;
        pause_ms(10);
    }
    fputs("FAIL PE process did not exit\n", stderr);
cleanup:
    atomic_store_explicit(&shared->stop_requested, 1u, memory_order_release);
    (void)kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
finished:
    if (ready[0] >= 0) (void)close(ready[0]);
    nt_supervisor_release(&sup);
    (void)munmap(shared, sizeof(*shared));
    return rc;
}
