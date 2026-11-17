/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "ntnative/guest_boundary_runtime.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

/* All CET decisions share one fail-closed policy. Test-only forced mode
 * lives solely in a separately compiled negative test binary; neither
 * the normal launcher nor an incoming PE RPC can switch this at runtime. */
static bool shadow_stack_unsafe(void)
{
#ifdef NT_TEST_FORCE_CET_SHSTK
    return true;
#elif defined(SYS_arch_prctl)
    unsigned long state = 0;
    if (syscall(SYS_arch_prctl, 0x5005u, &state) == 0)
        return state != 0;
    return errno != EINVAL && errno != ENOSYS;
#else
    return true;
#endif
}

/* No unreviewed PE import can enable the gate or supply a mapping. */
static _Atomic bool import_gate_enabled;
static uintptr_t trusted_code_begin, trusted_code_end;
/* Written by the freshly forked child before any pthread is created. */
static struct nt_guest_boundary_session *process_session;
static _Thread_local struct nt_guest_boundary_session *current_session;
static _Thread_local struct nt_guest_boundary_slot *current_slot;
static _Thread_local unsigned guest_entry_depth;
static _Thread_local unsigned host_import_depth;
static _Thread_local bool nonlocal_poisoned;
enum { MAX_IMPORT_NESTING = 128, MAX_GATED_TARGETS = 8192 };
static _Thread_local uintptr_t host_return_stack[MAX_IMPORT_NESTING];

struct gate_target { uintptr_t native, wrapper; };
static struct gate_target gates[MAX_GATED_TARGETS];
static size_t gate_count;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;

void nt_guest_boundary_enable_import_gate(void)
{
    atomic_store_explicit(&import_gate_enabled, true, memory_order_release);
}
bool nt_guest_boundary_set_image_range(uintptr_t begin, uintptr_t end)
{
    if (!begin || end <= begin) return false;
    trusted_code_begin = begin;
    trusted_code_end = end;
    return true;
}
bool nt_guest_boundary_get_image_range(uintptr_t *begin, uintptr_t *end)
{
    if (!begin || !end || !trusted_code_begin ||
        trusted_code_end <= trusted_code_begin)
        return false;
    *begin = trusted_code_begin;
    *end = trusted_code_end;
    return true;
}
bool nt_guest_boundary_import_gate_enabled(void)
{
    return atomic_load_explicit(&import_gate_enabled, memory_order_acquire);
}
void nt_guest_boundary_install_child_session(struct nt_guest_boundary_session *session)
{
    process_session = session;
    current_session = session;
    current_slot = NULL;
    guest_entry_depth = 0;
    host_import_depth = 0;
    nonlocal_poisoned = false;
    memset(host_return_stack, 0, sizeof(host_return_stack));
}
struct nt_guest_boundary_session *nt_guest_boundary_current_session(void)
{
    return current_session != NULL ? current_session : process_session;
}
const struct nt_guest_boundary_slot *nt_guest_boundary_find_thread(
    const struct nt_guest_boundary_session *session, pid_t tid)
{
    if (!session || tid <= 0) return NULL;
    for (unsigned i = 0; i < NT_GUEST_BOUNDARY_SLOTS; ++i)
        if (atomic_load_explicit(&session->slots[i].tid,
                                  memory_order_acquire) == (uint32_t)tid)
            return &session->slots[i];
    return NULL;
}
bool nt_guest_boundary_thread_attach(uintptr_t teb)
{
    if (current_session == NULL)
        current_session = process_session;
    if (teb == 0 || current_slot != NULL || !current_session)
        return false;
    const uint32_t tid = (uint32_t)syscall(SYS_gettid);
    for (unsigned i = 0; i < NT_GUEST_BOUNDARY_SLOTS; ++i) {
        struct nt_guest_boundary_slot *slot = &current_session->slots[i];
        uint32_t available = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &slot->tid, &available, tid, memory_order_acq_rel,
                memory_order_acquire))
            continue;
        /* Readers reject every phase except explicitly GUEST. */
        nt_guest_boundary_publish(&slot->boundary, NT_GUEST_BOUNDARY_HOST);
        atomic_store_explicit(&slot->teb, teb, memory_order_release);
        current_slot = slot;
        return true;
    }
    return false; /* fail closed: do not claim a guest-safe thread */
}
void nt_guest_boundary_thread_detach(void)
{
    if (!current_slot) return;
    nt_guest_boundary_publish(&current_slot->boundary, NT_GUEST_BOUNDARY_FINISHED);
    atomic_store_explicit(&current_slot->teb, 0, memory_order_release);
    atomic_store_explicit(&current_slot->tid, 0, memory_order_release);
    current_slot = NULL;
    guest_entry_depth = 0;
    host_import_depth = 0;
    nonlocal_poisoned = false;
}
static void publish_phase(enum nt_guest_boundary_phase phase)
{
    if (current_slot)
        nt_guest_boundary_publish(&current_slot->boundary, phase);
}
void nt_guest_boundary_guest_enter(void)
{
    /* Called only on reviewed host->PE entry points, and never while an
     * outer import frame is active. Such callbacks must stay HOST even
     * when RIP is briefly in PE code (the host may still own locks). */
    if (guest_entry_depth == UINT32_MAX)
        _Exit(138);
    ++guest_entry_depth;
    if (shadow_stack_unsafe()) {
        nonlocal_poisoned = true; /* never mark a CET guest safe to stop */
        publish_phase(NT_GUEST_BOUNDARY_HOST);
    } else if (host_import_depth == 0 && !nonlocal_poisoned) {
        publish_phase(NT_GUEST_BOUNDARY_GUEST);
    }
}
void nt_guest_boundary_guest_leave(void)
{
    if (guest_entry_depth == 0)
        _Exit(139);
    --guest_entry_depth;
    if (guest_entry_depth == 0 || host_import_depth != 0 || nonlocal_poisoned)
        publish_phase(NT_GUEST_BOUNDARY_HOST);
}
uint32_t nt_guest_boundary_current_phase(void)
{
    return current_slot ?
        atomic_load_explicit(&current_slot->boundary.phase,
                             memory_order_acquire) :
        NT_GUEST_BOUNDARY_UNREGISTERED;
}
uint32_t nt_guest_boundary_host_nesting(void)
{
    return host_import_depth;
}
/* The only safe decision after a known exception/longjmp can bypass
 * a previously installed trampoline return slot is to deny suspension
 * for the remainder of this particular thread. We never fabricate a
 * sequence of host-unlock events or a replacement call stack. */
void nt_guest_boundary_mark_nonlocal_transfer(void)
{
    nonlocal_poisoned = true;
    publish_phase(NT_GUEST_BOUNDARY_HOST);
}
bool nt_guest_boundary_nonlocal_poisoned(void)
{
    return nonlocal_poisoned;
}

/* The x86_64 assembly gate preserves the complete MS ABI argument and
 * return register set across these SysV calls. No mutex is acquired on
 * the *execution* path. A leaked host stack frame stays HOST and cannot
 * authorize a later asynchronous stop. */
uintptr_t nt_guest_boundary_import_enter(uintptr_t target, uintptr_t return_pc)
{
    /* SHSTK may have become enabled AFTER imports were resolved. Our
     * return-address-rewriting gate is not safe with a hardware shadow
     * stack. Fail closed before installing the ordinary-stack return. */
    if (shadow_stack_unsafe())
        _Exit(142);
    if (host_import_depth >= MAX_IMPORT_NESTING || target == 0 || return_pc == 0)
        _Exit(140);
    host_return_stack[host_import_depth++] = return_pc;
    publish_phase(NT_GUEST_BOUNDARY_HOST);
    return target;
}
uintptr_t nt_guest_boundary_import_leave(void)
{
    if (host_import_depth == 0)
        _Exit(141);
    const uintptr_t ret = host_return_stack[--host_import_depth];
    host_return_stack[host_import_depth] = 0;
    if (host_import_depth == 0 && guest_entry_depth != 0 && !nonlocal_poisoned)
        publish_phase(NT_GUEST_BOUNDARY_GUEST);
    else
        publish_phase(NT_GUEST_BOUNDARY_HOST);
    return ret;
}

nt_guest_proc nt_guest_boundary_wrap_import(nt_guest_proc target)
{
#if !defined(__x86_64__)
    (void)target;
    return NULL;
#else
    if (!target || !nt_guest_boundary_import_gate_enabled())
        return target;
    /* This return-address-based gate is incompatible with userspace CET
     * shadow stacks. Fail closed instead of corrupting the hardware
     * protected return stack. Linux arch_prctl(ARCH_SHSTK_STATUS)=0x5005. */
    if (shadow_stack_unsafe()) {
        errno = ENOTSUP;
        return NULL;
    }
    uintptr_t original = 0;
    _Static_assert(sizeof(target) == sizeof(original), "AMD64 function pointer");
    memcpy(&original, &target, sizeof(original));
    pthread_mutex_lock(&gate_lock);
    for (size_t i = 0; i < gate_count; ++i)
        if (gates[i].native == original) {
            uintptr_t address = gates[i].wrapper;
            pthread_mutex_unlock(&gate_lock);
            nt_guest_proc wrapper;
            memcpy(&wrapper, &address, sizeof(wrapper));
            return wrapper;
        }
    if (gate_count >= MAX_GATED_TARGETS) {
        pthread_mutex_unlock(&gate_lock);
        return NULL;
    }
    const long page = sysconf(_SC_PAGESIZE);
    if (page < 27 || page > 65536) {
        pthread_mutex_unlock(&gate_lock);
        return NULL;
    }
    unsigned char *memory = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        pthread_mutex_unlock(&gate_lock);
        return NULL;
    }
    uintptr_t entry = 0;
    void (*dispatcher)(void) = nt_guest_boundary_import_entry;
    memcpy(&entry, &dispatcher, sizeof(entry));
    /* ENDBR64 ; movabs target,%r11 ; movabs gateway,%r10 ; jmp *%r10.
     * R10 and R11 are volatile in the Microsoft x64 ABI. Never map W+X. */
    /* ENDBR64 for IBT, followed by 2x MOVABS + indirect JMP. */
    memory[0] = 0xf3; memory[1] = 0x0f; memory[2] = 0x1e; memory[3] = 0xfa;
    memory[4] = 0x49; memory[5] = 0xbb;
    memcpy(memory + 6, &original, sizeof(original));
    memory[14] = 0x49; memory[15] = 0xba;
    memcpy(memory + 16, &entry, sizeof(entry));
    memory[24] = 0x41; memory[25] = 0xff; memory[26] = 0xe2;
    if (mprotect(memory, (size_t)page, PROT_READ | PROT_EXEC) != 0) {
        munmap(memory, (size_t)page);
        pthread_mutex_unlock(&gate_lock);
        return NULL;
    }
    uintptr_t wrapper_address = (uintptr_t)memory;
    gates[gate_count++] = (struct gate_target){original, wrapper_address};
    pthread_mutex_unlock(&gate_lock);
    nt_guest_proc result;
    memcpy(&result, &wrapper_address, sizeof(result));
    return result;
#endif
}
