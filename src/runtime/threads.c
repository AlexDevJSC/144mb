/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sync.h"
#include "child_process.h"
#include "thread_broker_registry.h"
#include "thread_broker_ipc.h"
#include "ntnative/guest_boundary_runtime.h"
#include "nt_services.h"
#include "nt_core_information.h"
#include "object_broker.h"
#include "winbase.h"
#include "ntnative/guest_thread.h"

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <stdio.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { WAITABLE_COUNT = 256, MAXIMUM_WAIT_OBJECTS = 64, FLS_COUNT = 128,
       STILL_ACTIVE = 259, MAX_PENDING_APC = 64, MAXIMUM_SUSPEND_COUNT = 127 };
enum { EVENT_QUERY_STATE = 1, EVENT_MODIFY_STATE = 2, READ_CONTROL = 0x20000,
       SYNCHRONIZE = 0x100000, EVENT_ALL_ACCESS = 0x1f0003,
       PROCESS_QUERY_INFORMATION = 0x400, PROCESS_QUERY_LIMITED_INFORMATION = 0x1000,
       PROCESS_ALL_ACCESS = 0x1fffff,
       THREAD_TERMINATE = 1, THREAD_SUSPEND_RESUME = 2, THREAD_ALERT = 4,
       THREAD_GET_CONTEXT = 8, THREAD_SET_CONTEXT = 0x10,
       THREAD_SET_INFORMATION = 0x20, THREAD_QUERY_INFORMATION = 0x40,
       THREAD_QUERY_LIMITED_INFORMATION = 0x800, THREAD_ALL_ACCESS = 0x1fffff };
enum waitable_kind { EVENT, THREAD, PROCESS, SEMAPHORE };
typedef void (NT_MS_ABI *guest_user_apc)(uintptr_t);
typedef void (NT_MS_ABI *guest_native_apc)(void *, void *, void *);
struct guest_apc {
    struct guest_apc *next;
    guest_user_apc user;
    guest_native_apc native;
    uintptr_t first, second, third;
    /* Nonzero only for explicitly cancellable NTNative special APCs. */
    uint64_t cancellation_ticket;
    bool cancellation_requested;
};
enum { APC_STATE_PENDING = 1, APC_STATE_RUNNING = 2,
       APC_STATE_COMPLETED = 3, APC_STATE_CANCELLED = 4,
       APC_STATE_COMPLETED_AFTER_REQUEST = 5, APC_STATE_ABORTED = 6,
       APC_COMPLETION_HISTORY = 64 };
struct guest_apc_receipt { uint64_t ticket; uint32_t state; };
struct guest_apc_queue {
    struct guest_apc *head, *tail;
    uint32_t count;
    /* Only special queues consult this lock-free indicator from the PE
     * import-return path. Mutation remains serialized by wait_lock. */
    _Atomic bool available;
    /* A popped callback remains owned by its canonical target thread,
     * even though it no longer appears among the cancelable queued nodes. */
    struct guest_apc *executing;
    struct guest_apc_receipt receipts[APC_COMPLETION_HISTORY];
    uint32_t receipt_index;
};
/* The bounded Linux backend can boost a guest by one Linux nice step
 * and automatically restore its original base after a monotonic deadline.
 * This is NOT Windows NT's full dynamic priority decay formula. */
struct guest_dynamic_boost {
    bool disabled, active;
    int base_nice;
    struct timespec expires;
};
struct waitable {
    uintptr_t handle;
    unsigned int refs;
    /* Independent handles can refer to one event/thread/process state.
     * The canonical slot cannot be recycled while an alias references it. */
    struct waitable *shared;
    unsigned int links;
    uint32_t handle_flags;
    enum waitable_kind kind;
    bool manual, signaled, started, alerted;
    /* Published by the HANDLE owner; only consumed by its own PE thread
     * at a host-import-return checkpoint with no host mutex held. */
    _Atomic bool termination_requested;
    bool termination_acknowledged;
    bool child_guest, remote_guest, terminate_at_start, self_process;
    uint64_t remote_broker_capability, remote_broker_generation;
    int32_t semaphore_count, semaphore_maximum;
    bool brokered;
    struct nt_broker_resource broker;
    uintptr_t teb_base;
    struct guest_apc_queue apcs, special_apcs;
    uint32_t suspend_count;
    /* Running guest threads can only be paused at an explicit runtime
     * alertable checkpoint, never through an asynchronous Linux signal. */
    bool supervisor_async_suspended; /* parent-owned ptrace stop, never cooperatively resumed */
    bool cooperative_pause_requested, cooperative_pause_acknowledged;
    /* Full real AMD64 register snapshot from the target's own safe-point
     * execution, not a fabricated PC/stack or a remote ptrace operation. */
    bool cooperative_context_valid;
    struct nt_context64 cooperative_context;
    uint32_t access;
    pthread_t host;
    uint32_t thread_id, exit_code, owner_pid;
    uint32_t requested_exit_code;
    int pidfd, start_gate_fd;
    nt_thread_start start;
    void *parameter;
    /* Canonical named event/semaphore entries own a name. Aliases share
     * the same state, and the object vanishes after the last close. */
    char object_name[128];
    uint16_t *thread_description;
    uint64_t create_filetime, exit_filetime, user_cpu_100ns, kernel_cpu_100ns;
    uintptr_t final_group0_affinity;
    /* Primary Windows group survives a multigroup host cpuset. */
    uint16_t primary_group;
    bool primary_group_initialized;
    bool explicit_group_outside_process;
    int final_nice;
    bool final_nice_valid;
    struct guest_dynamic_boost dynamic_boost;
};

static struct waitable waitables[WAITABLE_COUNT];
/* Win32 HANDLE values reserve the low bits for API-specific flags
 * (notably OVERLAPPED.hEvent bit zero). Never allocate an object handle
 * which aliases a tagged handle when that bit is set. */
static uintptr_t next_handle = 0x76000004;
static struct waitable *current_process_state;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
/* Scoped process-local sequence: a ticket is an identifier, NEVER
 * authority. Cancellation must still pass THREAD_SET_CONTEXT through
 * the canonical HANDLE table. All reads and increments take wait_lock. */
static uint64_t next_special_apc_ticket = 1;
/* Serialize all asynchronous parent RPCs affecting the same NT object
 * without blocking unrelated guest waitables on wait_lock. */
static pthread_mutex_t supervisor_gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local struct waitable *running_thread;
static _Thread_local uint32_t thread_result;
static _Thread_local sigjmp_buf thread_exit;
/* A guest's primary thread is not represented by a waitable. Its own
 * pseudo-handle has a strictly thread-local queue, never a host TID. */
static _Thread_local struct guest_apc_queue primary_apcs, primary_special_apcs;
/* Recursion guard: an APC may import Win32, which itself returns via this
 * boundary. Dispatch pending APCs only after the current callback returns
 * or when a later unrelated safe-point is reached. */
static _Thread_local bool special_apc_dispatching;
static _Thread_local struct guest_apc *current_ticketed_special_apc;
/* Weak in narrow standalone thread tests which link no PE signal gateway. */
extern bool nt_runtime_guest_async_apc_nudge(pthread_t target)
    __attribute__((weak));
static _Thread_local bool primary_alerted;
static _Thread_local uint16_t *primary_thread_description;
static _Thread_local uint16_t primary_thread_group;
static _Thread_local bool primary_thread_group_initialized;
/* Only the main thread has a pseudo-handle that the decay worker can
 * identify cross-thread. All other guest threads use canonical waitables. */
static struct guest_dynamic_boost leader_dynamic_boost;
static bool process_dynamic_boost_disabled;
static bool boost_rollback_failed;
static int boost_monitor_state; /* 0: unstarted, 1: running, -1: unavailable */
static pid_t boost_monitor_tid;
static bool scheduler_can_restore_nice(int requested_nice);
static void scheduler_guest_wait_woke_locked(void);
static bool scheduler_has_active_boosts_locked(void);
/* Needed by CreateThread before the group-affinity API definitions below. */
static nt_status thread_primary_affinity_locked(uintptr_t handle,
                                                 const cpu_set_t *cpus,
                                                 struct nt_group_affinity *result);
/* At runtime startup, capture the Linux cpuset visible to this sandbox.
 * Thread-specific pinning must not silently change the system-wide
 * processor-group topology reported to Win32 callers. Only CPUs the
 * sandbox is permitted to use are advertised. CPU hotplug and cpuset
 * reconfiguration after first use are outside this bounded profile. */
static pthread_once_t processor_groups_once = PTHREAD_ONCE_INIT;
/* Guest CPU indices are dense in the startup-allowed Linux cpuset.
 * Linux CPU IDs (e.g. 24, 64, 97) are not Windows group/bit positions.
 * Mapping remains immutable after startup. -1 means not advertised. */
static uint32_t processor_group_counts[CPU_SETSIZE / 64];
static uintptr_t processor_group_masks[CPU_SETSIZE / 64];
static int linux_cpu_by_guest_slot[CPU_SETSIZE];
static int guest_slot_by_linux_cpu[CPU_SETSIZE];
/* Process policy is distinct from the affinity of an individual host
 * pthread. An explicit SetThreadAffinityMask may narrow one thread
 * without changing the process-wide Win32 policy. Protected by wait_lock. */
static uintptr_t process_primary_affinity;
static bool process_primary_affinity_initialized;
static bool process_affinity_explicit;
static bool process_leader_group_outside;
static uint16_t process_primary_group;
static uint16_t processor_group_count;
static uint32_t processor_group_total;
static void capture_processor_groups(void)
{
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        guest_slot_by_linux_cpu[cpu] = -1;
    cpu_set_t initial;
    if (sched_getaffinity(0, sizeof(initial), &initial) != 0)
        return;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &initial))
            continue;
        const int slot = (int)processor_group_total++;
        guest_slot_by_linux_cpu[cpu] = slot;
        linux_cpu_by_guest_slot[slot] = cpu;
        const uint16_t group = (uint16_t)(slot / 64);
        ++processor_group_counts[group];
        processor_group_masks[group] |= (uintptr_t)UINT64_C(1) << (slot % 64);
        processor_group_count = group + 1;
    }
    /* Windows 11 primary process group is the first advertised group.
     * Processor numbering is a virtual guest policy, not Linux CPU IDs. */
    process_primary_group = 0;
}
static void ensure_processor_groups(void)
{
    (void)pthread_once(&processor_groups_once, capture_processor_groups);
}

/* Windows x64 WORD group identifiers, ALL_PROCESSOR_GROUPS=0xffff.
 * Values reflect the startup sandbox CPU entitlement, not Linux
 * system-wide online CPUs or a fabricated Windows kernel topology. */
uint16_t NT_MS_ABI nt_GetActiveProcessorGroupCount(void)
{
    ensure_processor_groups();
    return processor_group_count;
}
uint32_t NT_MS_ABI nt_GetActiveProcessorCount(uint16_t group)
{
    ensure_processor_groups();
    return group == UINT16_MAX ? processor_group_total :
           group < processor_group_count ? processor_group_counts[group] : 0;
}

/* The real Linux executing CPU is sampled by the kernel, not inferred
 * from a thread's allowed cpuset. Windows reports the number local to
 * the processor group; PROCESSOR_NUMBER also includes the group ID. */
_Static_assert(sizeof(struct nt_processor_number) == 4,
               "Windows x64 PROCESSOR_NUMBER ABI layout");

uint32_t NT_MS_ABI nt_GetCurrentProcessorNumber(void)
{
    ensure_processor_groups();
    const int cpu = sched_getcpu();
    return cpu >= 0 && cpu < CPU_SETSIZE &&
           guest_slot_by_linux_cpu[cpu] >= 0 ?
           (uint32_t)(guest_slot_by_linux_cpu[cpu] % 64) : 0;
}

void NT_MS_ABI nt_GetCurrentProcessorNumberEx(struct nt_processor_number *result)
{
    if (result == NULL)
        return;
    ensure_processor_groups();
    const int cpu = sched_getcpu();
    if (cpu < 0 || cpu >= CPU_SETSIZE ||
        guest_slot_by_linux_cpu[cpu] < 0)
        return;
    const int slot = guest_slot_by_linux_cpu[cpu];
    *result = (struct nt_processor_number){
        .group = (uint16_t)(slot / 64),
        .number = (uint8_t)(slot % 64),
        .reserved = 0
    };
}

/* Scheduler-class state is process-local and protected by wait_lock.
 * It is never a permission to change Linux threads in another process. */
static uint32_t process_priority_class = 0x20u; /* NORMAL_PRIORITY_CLASS */
static int process_priority_nice_base;
static int32_t process_priority_nt_base = 8;
/* Windows FILETIME counts 100 ns intervals since 1601-01-01 UTC. */
static uint64_t windows_ticks(struct timespec clock)
{
    return ((uint64_t)clock.tv_sec + UINT64_C(11644473600)) * UINT64_C(10000000) +
           (uint64_t)clock.tv_nsec / 100;
}

static uint64_t filetime_now(void)
{
    struct timespec realtime;
    return clock_gettime(CLOCK_REALTIME, &realtime) == 0 ? windows_ticks(realtime) : 0;
}

static uint64_t usage_ticks(struct timeval time)
{
    return (uint64_t)time.tv_sec * UINT64_C(10000000) +
           (uint64_t)time.tv_usec * UINT64_C(10);
}

/* Linux stat's comm field may contain spaces or ')' characters; parse
 * fields only after the final ')' and validate the expected numeric
 * fields, never using a host PID taken from a Windows HANDLE. */
static bool proc_task_times(const char *path, uint64_t *user, uint64_t *kernel,
                            uint64_t *start_filetime)
{
    char buffer[4096];
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;
    const bool read = fgets(buffer, sizeof(buffer), file) != NULL;
    fclose(file);
    if (!read)
        return false;
    char *tail = strrchr(buffer, ')');
    if (tail == NULL || tail[1] != ' ')
        return false;
    uint64_t utime = 0, stime = 0, startup = 0;
    char *state = NULL, *save = NULL;
    for (int field = 3; field <= 22; ++field) {
        char *token = field == 3 ? strtok_r(tail + 2, " \n", &save) :
                                   strtok_r(NULL, " \n", &save);
        if (token == NULL)
            return false;
        if (field == 3) {
            state = token;
            continue;
        }
        if (field != 14 && field != 15 && field != 22)
            continue;
        errno = 0;
        char *end = NULL;
        unsigned long long value = strtoull(token, &end, 10);
        if (errno || end == token || *end != '\0')
            return false;
        if (field == 14) utime = (uint64_t)value;
        if (field == 15) stime = (uint64_t)value;
        if (field == 22) startup = (uint64_t)value;
    }
    (void)state;
    const long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0)
        return false;
    if (user != NULL) *user = utime * UINT64_C(10000000) / (uint64_t)hz;
    if (kernel != NULL) *kernel = stime * UINT64_C(10000000) / (uint64_t)hz;
    if (start_filetime != NULL) {
        struct timespec now;
        if (clock_gettime(CLOCK_BOOTTIME, &now) != 0)
            return false;
        const uint64_t elapsed = (uint64_t)now.tv_sec * UINT64_C(10000000) +
                                  (uint64_t)now.tv_nsec / 100 -
                                  startup * UINT64_C(10000000) / (uint64_t)hz;
        const uint64_t current = filetime_now();
        if (current == 0 || current < elapsed)
            return false;
        *start_filetime = current - elapsed;
    }
    return true;
}

static void write_filetime(struct nt_file_time *output, uint64_t value)
{
    output->low = (uint32_t)value;
    output->high = (uint32_t)(value >> 32);
}

/* THREAD_BASIC_INFORMATION exposes a group-local KAFFINITY. */
static uintptr_t linux_group0_affinity(pthread_t host)
{
    ensure_processor_groups();
    cpu_set_t allowed;
    if (pthread_getaffinity_np(host, sizeof(allowed), &allowed) != 0)
        return 0;
    uintptr_t mask = 0;
    for (int slot = 0; slot < 64 && slot < (int)processor_group_total; ++slot)
        if (CPU_ISSET(linux_cpu_by_guest_slot[slot], &allowed))
            mask |= (uintptr_t)UINT64_C(1) << slot;
    return mask;
}

static struct waitable *find_waitable(uintptr_t handle);
static struct waitable *wait_state(struct waitable *handle);

static struct guest_apc_queue *current_apcs(void)
{
    return running_thread != NULL ? &running_thread->apcs : &primary_apcs;
}
static struct guest_apc_queue *current_special_apcs(void)
{
    return running_thread != NULL ?
           &running_thread->special_apcs : &primary_special_apcs;
}

/* The privileged PE loader has already pinned executable .text ranges.
 * Never let an APC invoke a raw host ELF pointer or JIT memory supplied
 * as an opaque Win32 callback. Dynamic LoadLibrary and unpinned modules
 * remain explicitly unsupported, rather than silently trusted. */
static bool trusted_special_apc_entry(uintptr_t pc)
{
    uintptr_t start = 0, end = 0;
    if (pc == 0 || !nt_guest_boundary_import_gate_enabled() ||
        !nt_guest_boundary_get_image_range(&start, &end))
        return false;
    if (pc >= start && pc < end)
        return true;
    struct nt_guest_code_range dlls[NT_GUEST_ADDITIONAL_CODE_MAX];
    size_t count = 0;
    if (!nt_guest_boundary_get_startup_code_ranges(
            dlls, NT_GUEST_ADDITIONAL_CODE_MAX, &count))
        return false;
    for (size_t i = 0; i < count; ++i)
        if (pc >= dlls[i].begin && pc < dlls[i].end)
            return true;
    return false;
}


/* The caller holds wait_lock. APC callbacks execute only on their target
 * thread, after dropping this lock, and never from a Linux signal handler. */
/* The NT alerted bit is per thread and survives non-alertable waits.
 * It is consumed by precisely one alertable wait or NtTestAlert.
 * Called only under wait_lock so an alert cannot be lost before sleep. */
static bool consume_alert_locked(void)
{
    bool *flag = running_thread != NULL ? &running_thread->alerted : &primary_alerted;
    if (!*flag)
        return false;
    *flag = false;
    return true;
}

static struct guest_apc *pop_apc_queue_locked(struct guest_apc_queue *queue)
{
    struct guest_apc *apc = queue->head;
    if (apc != NULL) {
        queue->head = apc->next;
        if (queue->head == NULL)
            queue->tail = NULL;
        --queue->count;
        if (queue->count == 0)
            atomic_store_explicit(&queue->available, false,
                                  memory_order_release);
        apc->next = NULL;
    }
    return apc;
}

static struct guest_apc *pop_apc_locked(void)
{
    return pop_apc_queue_locked(current_apcs());
}

/* Called under wait_lock in the target thread at an alertable waiting
 * boundary. The condition wait releases the mutex, so other guest threads
 * can use handles, resume the target, and queue APCs without deadlocking.
 * APCs are dispatched only after the final resume, by the original thread.
 * This is NOT an asynchronous suspension of arbitrary guest instructions. */
static void cooperative_suspend_checkpoint_locked(void)
{
    struct waitable *thread = running_thread;
    if (thread == NULL || !thread->cooperative_pause_requested)
        return;
    _Alignas(16) struct nt_context64 context = {0};
    nt_RtlCaptureContext(&context);
    thread->cooperative_context = context;
    thread->cooperative_context_valid = true;
    thread->cooperative_pause_acknowledged = true;
    pthread_cond_broadcast(&wait_changed);
    while (thread->suspend_count != 0 && !thread->signaled)
        pthread_cond_wait(&wait_changed, &wait_lock);
    thread->cooperative_pause_acknowledged = false;
    thread->cooperative_context_valid = false;
    pthread_cond_broadcast(&wait_changed);
}

static void record_apc_receipt_locked(struct guest_apc_queue *queue,
                                      uint64_t ticket, uint32_t state)
{
    if (!ticket) return;
    queue->receipts[queue->receipt_index++ % APC_COMPLETION_HISTORY] =
        (struct guest_apc_receipt){.ticket=ticket, .state=state};
}

static void invoke_apc(struct guest_apc *apc)
{
    if (apc == NULL)
        return;
    if (apc->native != NULL)
        apc->native((void *)apc->first, (void *)apc->second, (void *)apc->third);
    else
        apc->user(apc->first);
    free(apc);
}

/* Async-signal-safe read of a TLS atomic flag. The signal handler NEVER
 * consumes the APC or touches wait_lock. It merely redirects control to
 * the trusted runtime's preallocated dispatch stack. */
bool nt_runtime_special_apc_pending_signal(void)
{
    return atomic_load_explicit(&current_special_apcs()->available,
                                memory_order_acquire);
}

/* Called ONLY by the protected PE import-return boundary, after it
 * verified the final host import has returned, no host mutex is owned
 * by this gate, and GS still belongs to the guest. This is a guest-safe
 * cooperative APC delivery point, NOT arbitrary PC interruption.
 * A callback which raises a software Windows exception follows the
 * existing RaiseException -> VEH/SEH machinery on its own target thread.
 * Any nonlocal unwinding poisons the guest/host gate fail-closed. */
void nt_runtime_special_apc_checkpoint(void)
{
    if (special_apc_dispatching)
        return;
    /* A checked TerminateThread request can commit only when the target
     * is itself returning from a Windows import, after all host locks
     * have been released by that call. No signal, pthread_cancel or raw
     * guest-supplied TID may kill an active Linux host stack. */
    if (running_thread != NULL &&
        atomic_load_explicit(&running_thread->termination_requested,
                             memory_order_acquire) &&
        nt_guest_boundary_special_apc_allowed() &&
        nt_guest_thread_teb() != NULL) {
        bool exiting = false;
        uint32_t code = 0;
        pthread_mutex_lock(&wait_lock);
        if (running_thread->termination_requested &&
            !running_thread->signaled &&
            !running_thread->supervisor_async_suspended &&
            running_thread->suspend_count == 0) {
            code = running_thread->requested_exit_code;
            running_thread->termination_acknowledged = true;
            atomic_store_explicit(&running_thread->termination_requested,
                                  false, memory_order_release);
            pthread_cond_broadcast(&wait_changed);
            exiting = true;
        }
        pthread_mutex_unlock(&wait_lock);
        if (exiting)
            nt_ExitThread(code); /* only this thread's existing jump gate */
    }
    /* Most imported Win32 calls have no pending APC. Avoid taking
     * wait_lock (and a second CET probe) on every PE import. */
    if (!atomic_load_explicit(&current_special_apcs()->available,
                              memory_order_acquire))
        return;
    if (!nt_guest_boundary_special_apc_allowed() ||
        nt_guest_thread_teb() == NULL)
        return;
    special_apc_dispatching = true;
    /* Bound each crossing: callbacks can re-queue themselves. */
    for (unsigned delivered = 0; delivered < 8; ++delivered) {
        pthread_mutex_lock(&wait_lock);
        struct guest_apc_queue *queue = current_special_apcs();
        struct guest_apc *apc = pop_apc_queue_locked(queue);
        if (apc != NULL)
            queue->executing = apc;
        pthread_mutex_unlock(&wait_lock);
        if (!apc)
            break;
        current_ticketed_special_apc = apc->cancellation_ticket ? apc : NULL;
        /* Must execute outside all host locks, on the actual target PE
         * thread and its existing private APC stack. */
        if (apc->native)
            apc->native((void *)apc->first,
                        (void *)apc->second, (void *)apc->third);
        else
            apc->user(apc->first);
        current_ticketed_special_apc = NULL;
        pthread_mutex_lock(&wait_lock);
        if (queue->executing == apc) {
            record_apc_receipt_locked(queue, apc->cancellation_ticket,
                apc->cancellation_requested ?
                APC_STATE_COMPLETED_AFTER_REQUEST : APC_STATE_COMPLETED);
            queue->executing = NULL;
        }
        pthread_cond_broadcast(&wait_changed);
        pthread_mutex_unlock(&wait_lock);
        free(apc);
        if (nt_guest_boundary_nonlocal_poisoned())
            break;
    }
    special_apc_dispatching = false;
}

/* On termination the target cannot execute additional APCs. The queue
 * belongs to its canonical THREAD slot even after every HANDLE is closed. */
static void clear_apcs_locked(struct guest_apc_queue *queue)
{
    if (queue->executing != NULL) {
        record_apc_receipt_locked(queue,
            queue->executing->cancellation_ticket, APC_STATE_ABORTED);
        free(queue->executing); /* ExitThread may unwind out of callback */
        queue->executing = NULL;
        current_ticketed_special_apc = NULL;
    }
    struct guest_apc *apc = queue->head;
    while (apc != NULL) {
        struct guest_apc *next = apc->next;
        free(apc);
        apc = next;
    }
    /* Accesses are serialized by wait_lock; publish empty before the
     * underlying thread slot can be recycled. */
    queue->head = queue->tail = NULL;
    queue->count = 0;
    atomic_store_explicit(&queue->available, false, memory_order_release);
    /* Keep bounded terminal receipts until the last HANDLE is closed.
     * A recycled canonical slot starts with zeroed receipt history. */
}

/* Actual guest HANDLE -> parent-owned supervisor lease. The parent
 * validates SCM_CREDENTIALS, pidfd, live TID generation, TEB and the
 * loader's trusted immutable executable image inventory. The parent
 * authorizes local guest delivery, but NEVER receives a guest stack
 * pointer or arbitrary ptrace injection request from the PE. */
bool nt_runtime_supervisor_open_thread_handle(uintptr_t handle, uint32_t rights,
    struct nt_broker_thread_info *identity, uint64_t *capability);

static nt_status parent_authorize_special_apc(uintptr_t handle,
                                               uintptr_t callback)
{
    if (!nt_broker_client_is_attached() || handle == UINTPTR_MAX - 1)
        return NT_STATUS_SUCCESS; /* self pseudo-handle uses local TEB */
    struct nt_broker_thread_info identity = {0};
    uint64_t capability = 0;
    if (!nt_runtime_supervisor_open_thread_handle(handle,
            THREAD_SET_CONTEXT, &identity, &capability))
        return NT_STATUS_NOT_SUPPORTED;
    nt_status result = nt_broker_client_supervisor_authorize_apc(
        capability, identity.generation, callback);
    if (!nt_broker_client_supervisor_close(capability) &&
        result == NT_STATUS_SUCCESS)
        result = NT_STATUS_NOT_SUPPORTED;
    return result;
}

static nt_status queue_apc_common(uintptr_t handle,
                                guest_user_apc user, guest_native_apc native,
                                uintptr_t first, uintptr_t second, uintptr_t third,
                                bool special, uint64_t *ticket_out)
{
    if ((user == NULL) == (native == NULL) || (ticket_out && !special))
        return NT_STATUS_INVALID_PARAMETER;
    uintptr_t callback = 0;
    _Static_assert(sizeof(callback) == sizeof(native) &&
                   sizeof(callback) == sizeof(user), "PE32+ AMD64 pointers");
    if (native != NULL)
        memcpy(&callback, &native, sizeof(callback));
    else
        memcpy(&callback, &user, sizeof(callback));
    /* Lock order mirrors CloseHandle: gate -> wait_lock. Never hold the
     * waitable mutex across the authenticated parent IPC round trip. */
    const bool supervised = special && handle != UINTPTR_MAX - 1 &&
                            nt_broker_client_is_attached();
    if (supervised) {
        pthread_mutex_lock(&supervisor_gate_lock);
        const nt_status auth = trusted_special_apc_entry(callback) ?
            parent_authorize_special_apc(handle, callback) : NT_STATUS_NOT_SUPPORTED;
        if (auth != NT_STATUS_SUCCESS) {
            pthread_mutex_unlock(&supervisor_gate_lock);
            return auth;
        }
    }
    pthread_mutex_lock(&wait_lock);
    struct guest_apc_queue *queue = NULL;
    nt_status status = NT_STATUS_SUCCESS;
    if (handle == UINTPTR_MAX - 1) {
        if (special && nt_guest_thread_teb() == NULL)
            status = NT_STATUS_NOT_SUPPORTED;
        else
            queue = special ? current_special_apcs() : current_apcs();
    } else {
        struct waitable *object = find_waitable(handle);
        if (object == NULL)
            status = NT_STATUS_INVALID_HANDLE;
        else if (object->kind != THREAD)
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if (wait_state(object)->owner_pid != (uint32_t)getpid())
            status = NT_STATUS_NOT_SUPPORTED;
        else if (!(object->access & 0x10u)) /* THREAD_SET_CONTEXT */
            status = NT_STATUS_ACCESS_DENIED;
        else if (wait_state(object)->signaled ||
                 wait_state(object)->terminate_at_start)
            status = NT_STATUS_THREAD_IS_TERMINATING;
        else if (special && wait_state(object)->teb_base == 0)
            status = NT_STATUS_NOT_SUPPORTED;
        else
            queue = special ? &wait_state(object)->special_apcs :
                              &wait_state(object)->apcs;
    }
    if (queue != NULL) {
        if (special && !trusted_special_apc_entry(callback)) {
            status = NT_STATUS_NOT_SUPPORTED;
        } else if (queue->count >= MAX_PENDING_APC) {
            status = NT_STATUS_NO_MEMORY;
        } else {
            struct guest_apc *apc = calloc(1, sizeof(*apc));
            if (apc == NULL) {
                status = NT_STATUS_NO_MEMORY;
            } else if (ticket_out && next_special_apc_ticket == 0) {
                /* The ticket sequence must never wrap and alias a past
                 * cancel target even if an old HANDLE was recycled. */
                free(apc);
                status = NT_STATUS_NO_MEMORY;
            } else {
                *apc = (struct guest_apc){
                    .user = user, .native = native,
                    .first = first, .second = second, .third = third
                };
                if (ticket_out) {
                    apc->cancellation_ticket = next_special_apc_ticket++;
                    *ticket_out = apc->cancellation_ticket;
                }
                if (queue->tail != NULL)
                    queue->tail->next = apc;
                else
                    queue->head = apc;
                queue->tail = apc;
                ++queue->count;
                if (special) {
                    atomic_store_explicit(&queue->available, true,
                                          memory_order_release);
                    /* Signal only the canonical owned guest pthread, not
                     * a Win32 HANDLE, Linux TID supplied by PE or a
                     * cross-process broker identity. Keep wait_lock while
                     * taking the pthread reference: the target cannot
                     * retire its canonical THREAD slot before nudge.
                     * The handler never locks this mutex. A signal in
                     * HOST phase is harmless: the already queued APC
                     * remains for the next safe import-return point. */
                    if (nt_runtime_guest_async_apc_nudge != NULL) {
                        if (handle == UINTPTR_MAX - 1)
                            (void)nt_runtime_guest_async_apc_nudge(pthread_self());
                        else {
                            struct waitable *target = find_waitable(handle);
                            if (target != NULL && target->kind == THREAD &&
                                wait_state(target)->started &&
                                wait_state(target)->teb_base != 0 &&
                                !wait_state(target)->signaled)
                                (void)nt_runtime_guest_async_apc_nudge(
                                    wait_state(target)->host);
                        }
                    }
                }
                pthread_cond_broadcast(&wait_changed);
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (supervised)
        pthread_mutex_unlock(&supervisor_gate_lock);
    return status;
}

nt_status nt_runtime_queue_apc(uintptr_t handle, guest_user_apc user,
                                guest_native_apc native,
                                uintptr_t first, uintptr_t second, uintptr_t third)
{
    return queue_apc_common(handle, user, native, first, second, third, false, NULL);
}

uint32_t NT_MS_ABI nt_QueueUserAPC(guest_user_apc callback, uintptr_t handle, uintptr_t data)
{
    const nt_status status = nt_runtime_queue_apc(handle, callback, NULL, data, 0, 0);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtQueueApcThread(uintptr_t thread, guest_native_apc routine,
                                       void *context, void *first, void *second)
{
    return nt_runtime_queue_apc(thread, NULL, routine, (uintptr_t)context,
                                (uintptr_t)first, (uintptr_t)second);
}

/* Reserve==0 preserves the normal alertable-APC contract. Reserve==1
 * queues a distinct special APC, but ONLY for a pinned executable PE
 * callback and a target with a live TEB: it runs on that thread's next
 * safe Win64 import return, not at an arbitrary interrupted instruction.
 * Other nonzero reservations fail rather than silently downgrade. */
nt_status NT_MS_ABI nt_NtQueueApcThreadEx(uintptr_t thread, uintptr_t reserve,
                                         guest_native_apc routine,
                                         void *context, void *first, void *second)
{
    if (reserve == 1)
        return queue_apc_common(thread, NULL, routine, (uintptr_t)context,
                                (uintptr_t)first, (uintptr_t)second, true, NULL);
    if (reserve != 0)
        return NT_STATUS_NOT_SUPPORTED;
    return nt_NtQueueApcThread(thread, routine, context, first, second);
}

uint32_t NT_MS_ABI nt_QueueUserAPC2(guest_user_apc callback, uintptr_t handle,
                                   uintptr_t data, uint32_t flags)
{
    if (flags == 1) {
        const nt_status status =
            queue_apc_common(handle, callback, NULL, data, 0, 0, true, NULL);
        if (status != NT_STATUS_SUCCESS)
            nt_SetLastError(nt_runtime_nt_error(status));
        return status == NT_STATUS_SUCCESS;
    }
    if (flags != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return nt_QueueUserAPC(callback, handle, data);
}

/* Non-Windows NTNATIVE extension. Unlike Windows QueueUserAPC2, this
 * scoped API returns an opaque per-process ticket for an APC still in
 * the special queue. It does NOT offer cancellation of a callback that
 * has been dequeued or is currently executing on the guest stack. The
 * ticket is not a Linux TID, callback pointer, ptrace capability or
 * substitute for THREAD_SET_CONTEXT rights. */
nt_status NT_MS_ABI nt_NtNativeQueueSpecialApc(
    uintptr_t thread, guest_native_apc callback, void *context,
    void *first, void *second, uint64_t *ticket)
{
    if (ticket == NULL)
        return NT_STATUS_INVALID_PARAMETER;
    return queue_apc_common(thread, NULL, callback, (uintptr_t)context,
                            (uintptr_t)first, (uintptr_t)second, true, ticket);
}

nt_status NT_MS_ABI nt_NtNativeCancelSpecialApc(uintptr_t thread,
                                                uint64_t ticket)
{
    if (ticket == 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct guest_apc_queue *queue = NULL;
    nt_status status = NT_STATUS_SUCCESS;
    if (thread == UINTPTR_MAX - 1) {
        if (nt_guest_thread_teb() == NULL)
            status = NT_STATUS_NOT_SUPPORTED;
        else
            queue = current_special_apcs();
    } else {
        struct waitable *object = find_waitable(thread);
        if (object == NULL)
            status = NT_STATUS_INVALID_HANDLE;
        else if (object->kind != THREAD)
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if (wait_state(object)->owner_pid != (uint32_t)getpid())
            status = NT_STATUS_NOT_SUPPORTED;
        else if (!(object->access & THREAD_SET_CONTEXT))
            status = NT_STATUS_ACCESS_DENIED;
        else if (wait_state(object)->signaled || wait_state(object)->terminate_at_start)
            status = NT_STATUS_THREAD_IS_TERMINATING;
        else
            queue = &wait_state(object)->special_apcs;
    }
    if (queue != NULL) {
        struct guest_apc *previous = NULL;
        struct guest_apc *current = queue->head;
        while (current && current->cancellation_ticket != ticket) {
            previous = current;
            current = current->next;
        }
        if (current == NULL) {
            if (queue->executing != NULL &&
                queue->executing->cancellation_ticket == ticket) {
                /* This requests cooperative cancellation; it does NOT
                 * preempt the guest CPU or revert existing side effects. */
                queue->executing->cancellation_requested = true;
                status = NT_STATUS_PENDING;
            } else {
                status = NT_STATUS_NOT_FOUND;
            }
        } else {
            if (previous)
                previous->next = current->next;
            else
                queue->head = current->next;
            if (queue->tail == current)
                queue->tail = previous;
            --queue->count;
            if (queue->count == 0)
                atomic_store_explicit(&queue->available, false,
                                      memory_order_release);
            record_apc_receipt_locked(queue, current->cancellation_ticket,
                                      APC_STATE_CANCELLED);
            free(current);
            /* The signal dispatcher may have already captured a guest
             * context. It must check the queue again under wait_lock,
             * and will restore the snapshot without calling a canceled
             * callback. A matching dequeued callback is NOT cancelable. */
            pthread_cond_broadcast(&wait_changed);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

/* Explicitly different states, never a guessed success. This
 * extension queries the canonical object's bounded receipt history:
 * a historical ticket may expire from the ring and then returns
 * STATUS_NOT_FOUND, never a fabricated COMPLETED state. */
nt_status NT_MS_ABI nt_NtNativeQuerySpecialApc(
    uintptr_t thread, uint64_t ticket, uint32_t *state)
{
    if (!state || ticket == 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct guest_apc_queue *queue = NULL;
    nt_status result = NT_STATUS_SUCCESS;
    if (thread == UINTPTR_MAX - 1) {
        queue = nt_guest_thread_teb() ? current_special_apcs() : NULL;
        if (!queue) result = NT_STATUS_NOT_SUPPORTED;
    } else {
        struct waitable *object = find_waitable(thread);
        if (!object)
            result = NT_STATUS_INVALID_HANDLE;
        else if (object->kind != THREAD)
            result = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if (!(object->access & THREAD_SET_CONTEXT))
            result = NT_STATUS_ACCESS_DENIED;
        else if (wait_state(object)->owner_pid != (uint32_t)getpid())
            result = NT_STATUS_NOT_SUPPORTED;
        else
            queue = &wait_state(object)->special_apcs;
    }
    if (queue) {
        result = NT_STATUS_NOT_FOUND;
        for (struct guest_apc *node = queue->head; node; node = node->next)
            if (node->cancellation_ticket == ticket) {
                *state = APC_STATE_PENDING;
                result = NT_STATUS_SUCCESS;
                break;
            }
        if (result != NT_STATUS_SUCCESS && queue->executing &&
            queue->executing->cancellation_ticket == ticket) {
            *state = APC_STATE_RUNNING;
            result = NT_STATUS_SUCCESS;
        }
        if (result != NT_STATUS_SUCCESS)
            for (unsigned i = 0; i < APC_COMPLETION_HISTORY; ++i)
                if (queue->receipts[i].ticket == ticket) {
                    *state = queue->receipts[i].state;
                    result = NT_STATUS_SUCCESS;
                    break;
                }
    }
    pthread_mutex_unlock(&wait_lock);
    return result;
}

/* Invoked by a callback itself at a safe voluntary checkpoint, outside
 * Linux signal handlers. A canceling thread can request cancellation
 * after execution began; only the callback knows how to release locks,
 * unwind stack frames and return safely. */
uint32_t NT_MS_ABI nt_NtNativeSpecialApcCancelRequested(void)
{
    pthread_mutex_lock(&wait_lock);
    const bool requested = current_ticketed_special_apc != NULL &&
        current_ticketed_special_apc->cancellation_requested;
    pthread_mutex_unlock(&wait_lock);
    return requested ? 1u : 0u;
}

nt_status NT_MS_ABI nt_NtAlertThread(uintptr_t handle)
{
    pthread_mutex_lock(&wait_lock);
    nt_status status = NT_STATUS_SUCCESS;
    if (handle == nt_GetCurrentThread()) {
        if (running_thread != NULL)
            running_thread->alerted = true;
        else
            primary_alerted = true;
    } else {
        struct waitable *object = find_waitable(handle);
        if (!object)
            status = NT_STATUS_INVALID_HANDLE;
        else if (object->kind != THREAD)
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if ((object->access & THREAD_ALERT) == 0)
            status = NT_STATUS_ACCESS_DENIED;
        else {
            struct waitable *state = wait_state(object);
            if (state->owner_pid != (uint32_t)getpid())
                status = NT_STATUS_NOT_SUPPORTED;
            else if (state->signaled || state->terminate_at_start)
                status = NT_STATUS_THREAD_IS_TERMINATING;
            else
                state->alerted = true;
        }
    }
    if (status == NT_STATUS_SUCCESS)
        pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtTestAlert(void)
{
    pthread_mutex_lock(&wait_lock);
    struct guest_apc *apc = pop_apc_locked();
    const bool alerted = apc == NULL && consume_alert_locked();
    pthread_mutex_unlock(&wait_lock);
    if (apc != NULL) {
        invoke_apc(apc);
        return NT_STATUS_USER_APC;
    }
    return alerted ? NT_STATUS_ALERTED : NT_STATUS_SUCCESS;
}

/* Same condition variable as waits and queueing: no lost wake-ups between
 * checking for work and sleeping, even when APCs are queued by another
 * Linux host thread. NULL deadline denotes an infinite alertable wait. */
nt_status nt_runtime_alertable_delay(const struct nt_wait_deadline *deadline)
{
    pthread_mutex_lock(&wait_lock);
    struct guest_apc *apc = NULL;
    bool alerted = false;
    int error = 0;
    for (;;) {
        cooperative_suspend_checkpoint_locked();
        apc = pop_apc_locked();
        if (apc != NULL)
            break;
        if (consume_alert_locked()) {
            alerted = true;
            break;
        }
        if (deadline != NULL && deadline->time.tv_sec < 0) {
            error = ETIMEDOUT;
            break;
        }
        error = deadline != NULL ?
            pthread_cond_clockwait(&wait_changed, &wait_lock, deadline->clock, &deadline->time) :
            pthread_cond_wait(&wait_changed, &wait_lock);
        if (error != 0)
            break;
    }
    pthread_mutex_unlock(&wait_lock);
    if (apc != NULL) {
        invoke_apc(apc);
        return NT_STATUS_USER_APC;
    }
    if (alerted)
        return NT_STATUS_ALERTED;
    return error == 0 || error == ETIMEDOUT ? NT_STATUS_SUCCESS : NT_STATUS_UNSUCCESSFUL;
}

uint32_t NT_MS_ABI nt_SleepEx(uint32_t milliseconds, int32_t alertable)
{
    if (!alertable) {
        nt_Sleep(milliseconds);
        return 0;
    }
    const int64_t relative = -(int64_t)milliseconds * INT64_C(10000);
    struct nt_wait_deadline deadline;
    if (milliseconds == UINT32_MAX)
        return nt_runtime_alertable_delay(NULL) == NT_STATUS_USER_APC ? 0xc0u : 0;
    if (nt_runtime_wait_deadline(relative, &deadline) != NT_STATUS_SUCCESS)
        return 0;
    return nt_runtime_alertable_delay(&deadline) == NT_STATUS_USER_APC ? 0xc0u : 0;
}

static struct waitable *find_waitable(uintptr_t handle)
{
    if (handle != 0) {
        for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
            if (waitables[i].handle == handle)
                return &waitables[i];
        }
    }
    return NULL;
}

static struct waitable *new_waitable(enum waitable_kind kind)
{
    if (next_handle > UINTPTR_MAX - 4)
        return NULL;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        if (waitables[i].refs == 0 && waitables[i].links == 0) {
            waitables[i] = (struct waitable){.handle = next_handle, .refs = 1, .kind = kind,
                                             .access = SYNCHRONIZE, .pidfd = -1, .start_gate_fd = -1};
            next_handle += 4;
            return &waitables[i];
        }
    }
    return NULL;
}

static struct waitable *wait_state(struct waitable *handle)
{
    return handle->shared ? handle->shared : handle;
}

/* Releasing an alias after the final in-flight wait removes its reference
 * from the canonical slot. All mutations occur under wait_lock. */
static void release_waitable(struct waitable *object)
{
    if (--object->refs != 0)
        return;
    if (object->shared != NULL) {
        struct waitable *canonical = object->shared;
        --canonical->links;
        if (canonical->refs == 0 && canonical->links == 0) {
            if (canonical->brokered) {
                nt_broker_release(&canonical->broker);
                canonical->brokered = false;
            }
            free(canonical->thread_description);
            canonical->thread_description = NULL;
        }
    } else if (object->links == 0) {
        if (object->brokered) {
            nt_broker_release(&object->broker);
            object->brokered = false;
        }
        free(object->thread_description);
        object->thread_description = NULL;
    }
}

/* HANDLE_FLAG_PROTECT_FROM_CLOSE is metadata on the HANDLE, never on
 * the shared kernel-object state. It must survive DuplicateHandle aliasing
 * without preventing another independent alias from being released. */
nt_status nt_runtime_close_waitable_status(uintptr_t handle)
{
    /* Closing a HANDLE during a pending async resume previously allowed
     * the parent count to drop while the child could no longer commit
     * the matching local count. Use the same gate order as Suspend/
     * Resume/Get/Set: supervisor_gate_lock -> wait_lock. */
    pthread_mutex_lock(&supervisor_gate_lock);
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->handle_flags & 2u)
        status = NT_STATUS_ACCESS_DENIED;
    uint64_t release_capability = 0;
    if (status == NT_STATUS_SUCCESS) {
        struct waitable *canonical = wait_state(object);
        object->handle = 0;
        release_waitable(object);
        if (canonical->remote_guest && canonical->refs == 0 &&
            canonical->links == 0 && canonical->remote_broker_capability != 0) {
            release_capability = canonical->remote_broker_capability;
            canonical->remote_broker_capability = 0;
        }
        pthread_cond_broadcast(&wait_changed);
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_mutex_unlock(&supervisor_gate_lock);
    /* Never block wait_lock waiting for an IPC round trip; host cleanup is
     * bounded by the broker's deadline and may fail on disconnect. */
    if (release_capability != 0)
        (void)nt_broker_client_close(release_capability);
    return status;
}

bool nt_runtime_close_waitable(uintptr_t handle)
{
    return nt_runtime_close_waitable_status(handle) == NT_STATUS_SUCCESS;
}

bool nt_runtime_get_waitable_flags(uintptr_t handle, uint32_t *flags)
{
    pthread_mutex_lock(&wait_lock);
    const struct waitable *object = find_waitable(handle);
    if (object && flags)
        *flags = object->handle_flags;
    pthread_mutex_unlock(&wait_lock);
    return object != NULL;
}

bool nt_runtime_set_waitable_flags(uintptr_t handle, uint32_t mask, uint32_t flags)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    if (object != NULL)
        object->handle_flags = (object->handle_flags & ~mask) | (flags & mask);
    pthread_mutex_unlock(&wait_lock);
    return object != NULL;
}

bool nt_runtime_has_inheritable_waitables(void)
{
    pthread_mutex_lock(&wait_lock);
    bool exists = false;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i)
        if (waitables[i].handle != 0 && waitables[i].refs != 0 &&
            (waitables[i].handle_flags & 1u) != 0) {
            exists = true;
            break;
        }
    pthread_mutex_unlock(&wait_lock);
    return exists;
}

uint32_t nt_runtime_count_waitable_handles(void)
{
    pthread_mutex_lock(&wait_lock);
    uint32_t count = 0;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i)
        if (waitables[i].handle != 0 && waitables[i].refs != 0)
            ++count;
    pthread_mutex_unlock(&wait_lock);
    return count;
}

_Static_assert(sizeof(struct nt_object_attributes) == 48 &&
               offsetof(struct nt_object_attributes, name) == 16, "AMD64 OBJECT_ATTRIBUTES layout");

/* Bounded ASCII-only names under the local BaseNamedObjects namespace.
 * Do not claim global/session, nested directories or Unicode case folding
 * until a cross-process NT object namespace broker exists. */
static nt_status named_object_name(const struct nt_guest_unicode_string *unicode, char output[128])
{
    output[0] = 0;
    if (unicode == NULL)
        return NT_STATUS_SUCCESS;
    if (unicode->buffer == NULL || unicode->length == 0 ||
        unicode->length % 2 != 0 ||
        unicode->length > unicode->maximum_length ||
        unicode->length >= 128 * sizeof(uint16_t))
        return NT_STATUS_INVALID_PARAMETER;
    const size_t length = unicode->length / sizeof(uint16_t);
    for (size_t i = 0; i < length; ++i) {
        const uint16_t code = unicode->buffer[i];
        if (code < 0x20 || code > 0x7e)
            return NT_STATUS_NOT_SUPPORTED;
        output[i] = (char)code;
    }
    output[length] = 0;
    const char *normalized = output;
    if (strncasecmp(output, "Local\\", 6) == 0)
        normalized += 6;
    else if (strncasecmp(output, "\\BaseNamedObjects\\", 18) == 0)
        normalized += 18;
    else if (strncasecmp(output, "Global\\", 7) == 0 || strchr(output, '\\') != NULL)
        return NT_STATUS_NOT_SUPPORTED;
    if (*normalized == 0 || strchr(normalized, '\\') != NULL)
        return NT_STATUS_INVALID_PARAMETER;
    if (normalized != output)
        memmove(output, normalized, strlen(normalized) + 1);
    return NT_STATUS_SUCCESS;
}

static bool event_name_equal(const char *left, const char *right, bool insensitive)
{
    if (!insensitive)
        return strcmp(left, right) == 0;
    while (*left && *right) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right))
            return false;
        ++left;
        ++right;
    }
    return *left == 0 && *right == 0;
}

static struct waitable *find_named_object(const char *name, bool insensitive)
{
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        struct waitable *entry = &waitables[i];
        if ((entry->kind == EVENT || entry->kind == SEMAPHORE) &&
            entry->shared == NULL && entry->object_name[0] &&
            (entry->refs != 0 || entry->links != 0) &&
            event_name_equal(entry->object_name, name, insensitive))
            return entry;
    }
    return NULL;
}

/* Allocate an independent handle referring to a named object's state. */
static struct waitable *open_named_alias(struct waitable *state, uint32_t access, bool inherit)
{
    struct waitable *alias = new_waitable(state->kind);
    if (alias == NULL)
        return NULL;
    ++state->links;
    alias->shared = state;
    alias->access = access;
    alias->handle_flags = inherit ? 1u : 0u;
    return alias;
}

static uint32_t event_access(uint32_t access)
{
    uint32_t rights = access & ~UINT32_C(0xf2000000);
    if (access & UINT32_C(0x80000000)) /* GENERIC_READ */
        rights |= READ_CONTROL | EVENT_QUERY_STATE;
    if (access & UINT32_C(0x40000000)) /* GENERIC_WRITE */
        rights |= READ_CONTROL | EVENT_MODIFY_STATE;
    if (access & UINT32_C(0x20000000)) /* GENERIC_EXECUTE */
        rights |= READ_CONTROL | SYNCHRONIZE;
    if (access & UINT32_C(0x12000000)) /* GENERIC_ALL or MAXIMUM_ALLOWED */
        rights |= EVENT_ALL_ACCESS;
    return rights;
}

nt_status nt_runtime_duplicate_waitable(uintptr_t source, uint32_t access,
                                         uint32_t attributes, uint32_t options,
                                         uintptr_t *target)
{
    if (!target)
        return NT_STATUS_ACCESS_VIOLATION;
    *target = 0;
    if ((options & ~7u) != 0 || (attributes & ~2u) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *original = find_waitable(source);
    nt_status status = NT_STATUS_SUCCESS;
    if (!original) {
        status = NT_STATUS_INVALID_HANDLE;
    } else {
        uint32_t rights = (options & 2u) ? original->access :
                          (original->kind == EVENT || original->kind == SEMAPHORE) ?
                          event_access(access) : access;
        if (rights & ~original->access) {
            status = NT_STATUS_ACCESS_DENIED;
        } else {
            struct waitable *alias = new_waitable(original->kind);
            if (!alias) {
                status = NT_STATUS_NO_MEMORY;
            } else {
                struct waitable *state = wait_state(original);
                ++state->links;
                alias->shared = state;
                alias->access = rights;
                alias->handle_flags = (options & 4u) ? original->handle_flags :
                                      (attributes & 2u) ? 1u : 0u;
                *target = alias->handle;
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

/* Shared named objects use one broker lease per opened HANDLE. The
 * canonical in-process entry still owns refcounting and DuplicateHandle,
 * but signaling state and atomic consumption live in a shared memfd. */
static nt_status broker_named_waitable(uintptr_t *handle, const char *name,
    enum waitable_kind kind, uint32_t rights, bool inherit, bool insensitive,
    bool create, int32_t initial, int32_t maximum, bool manual)
{
    struct nt_broker_request request = {
        .magic = NT_BROKER_MAGIC, .version = NT_BROKER_VERSION,
        .action = create ? NT_BROKER_CREATE : NT_BROKER_OPEN,
        .kind = kind == EVENT ? NT_BROKER_EVENT : NT_BROKER_SEMAPHORE,
        .rights = rights, .flags = (insensitive ? 2u : 0u) |
                                 (create && manual ? 1u : 0u),
        .initial = create ? (uint32_t)initial : 0,
        .maximum = create ? (uint32_t)maximum : 0
    };
    memcpy(request.name, name, strlen(name) + 1);
    struct nt_broker_resource broker;
    const int result = nt_broker_acquire(&request, &broker);
    switch (result) {
    case NT_BROKER_CREATED: case NT_BROKER_EXISTS: break;
    case NT_BROKER_MISSING: return NT_STATUS_OBJECT_NAME_NOT_FOUND;
    case NT_BROKER_TYPE_MISMATCH: return NT_STATUS_OBJECT_TYPE_MISMATCH;
    case NT_BROKER_ACCESS_DENIED: return NT_STATUS_ACCESS_DENIED;
    case NT_BROKER_RESOURCE_ERROR: return NT_STATUS_NO_MEMORY;
    case NT_BROKER_INVALID: return NT_STATUS_INVALID_PARAMETER;
    default: return NT_STATUS_UNSUCCESSFUL;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = new_waitable(kind);
    if (object) {
        object->access = rights;
        object->handle_flags = inherit ? 1u : 0u;
        object->brokered = true;
        object->broker = broker;
        memcpy(object->object_name, name, strlen(name) + 1);
        *handle = object->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    if (!object) {
        nt_broker_release(&broker);
        return NT_STATUS_NO_MEMORY;
    }
    return result == NT_BROKER_EXISTS ?
           NT_STATUS_OBJECT_NAME_EXISTS : NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtCreateEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes,
                                   uint32_t type, uint8_t initial_state)
{
    if (!handle)
        return NT_STATUS_ACCESS_VIOLATION;
    if (type > 1 || (attributes && attributes->size != sizeof(*attributes)))
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes && (attributes->root || attributes->security || attributes->qos ||
                       (attributes->attributes & ~(0x40u | 2u))))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status name_status = named_object_name(attributes ? attributes->name : NULL, normalized);
    if (name_status != NT_STATUS_SUCCESS)
        return name_status;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    if (normalized[0] && nt_runtime_broker_enabled())
        return broker_named_waitable(handle, normalized, EVENT, rights,
                                    attributes && (attributes->attributes & 2u),
                                    attributes && (attributes->attributes & 0x40u),
                                    true, initial_state != 0, 1, type == 0);
    pthread_mutex_lock(&wait_lock);
    nt_status status = NT_STATUS_SUCCESS;
    struct waitable *event = normalized[0] ?
        find_named_object(normalized, attributes && (attributes->attributes & 0x40u)) : NULL;
    if (event != NULL) {
        if (event->kind != EVENT) {
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        } else if (rights & ~event->access) {
            status = NT_STATUS_ACCESS_DENIED;
        } else {
            event = open_named_alias(event, rights, attributes && (attributes->attributes & 2u));
            status = event ? NT_STATUS_OBJECT_NAME_EXISTS : NT_STATUS_NO_MEMORY;
        }
    } else {
        event = new_waitable(EVENT);
        if (event != NULL) {
            event->manual = type == 0; /* NotificationEvent */
            event->signaled = initial_state != 0;
            event->access = rights;
            event->handle_flags = attributes && (attributes->attributes & 2u) ? 1u : 0u;
            if (normalized[0])
                memcpy(event->object_name, normalized, strlen(normalized) + 1);
        } else {
            status = NT_STATUS_NO_MEMORY;
        }
    }
    if (status == NT_STATUS_SUCCESS || status == NT_STATUS_OBJECT_NAME_EXISTS)
        *handle = event->handle;
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtOpenEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (attributes == NULL || attributes->size != sizeof(*attributes) ||
        attributes->name == NULL)
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes->root || attributes->security || attributes->qos ||
        (attributes->attributes & ~(0x40u | 2u)))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status name_status = named_object_name(attributes->name, normalized);
    if (name_status != NT_STATUS_SUCCESS)
        return name_status;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    if (nt_runtime_broker_enabled())
        return broker_named_waitable(handle, normalized, EVENT, rights,
                                    (attributes->attributes & 2u) != 0,
                                    (attributes->attributes & 0x40u) != 0,
                                    false, 0, 0, false);
    pthread_mutex_lock(&wait_lock);
    struct waitable *state = find_named_object(normalized, (attributes->attributes & 0x40u) != 0);
    nt_status status = NT_STATUS_SUCCESS;
    if (state == NULL) {
        status = NT_STATUS_OBJECT_NAME_NOT_FOUND;
    } else if (state->kind != EVENT) {
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    } else if (rights & ~state->access) {
        status = NT_STATUS_ACCESS_DENIED;
    } else {
        struct waitable *alias = open_named_alias(state, rights, (attributes->attributes & 2u) != 0);
        if (alias == NULL)
            status = NT_STATUS_NO_MEMORY;
        else
            *handle = alias->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

static struct nt_object_attributes win_event_attributes(const struct nt_guest_unicode_string *name,
                                                         bool inherit)
{
    struct nt_object_attributes attributes = {0};
    attributes.size = sizeof(attributes);
    attributes.name = name;
    attributes.attributes = 0x40u | (inherit ? 2u : 0u);
    return attributes;
}

static struct nt_guest_unicode_string win_event_string(const uint16_t *name)
{
    size_t length = 0;
    if (name != NULL)
        while (length < 127 && name[length])
            ++length;
    const uint16_t bytes = (uint16_t)(length * sizeof(uint16_t));
    return (struct nt_guest_unicode_string){bytes, bytes, (uint16_t *)name};
}

/* Native Win64 Semaphore objects share the same waitable table and the
 * process-local object namespace as named events. Cross-process objects and
 * security descriptors still require a dedicated broker. */
nt_status NT_MS_ABI nt_NtCreateSemaphore(uintptr_t *handle, uint32_t access,
                                         const struct nt_object_attributes *attributes,
                                         int32_t initial, int32_t maximum)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (initial < 0 || maximum <= 0 || initial > maximum ||
        (attributes && attributes->size != sizeof(*attributes)))
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes && (attributes->root || attributes->security || attributes->qos ||
                       (attributes->attributes & ~(0x40u | 2u))))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status name_status = named_object_name(attributes ? attributes->name : NULL, normalized);
    if (name_status != NT_STATUS_SUCCESS)
        return name_status;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    if (normalized[0] && nt_runtime_broker_enabled())
        return broker_named_waitable(handle, normalized, SEMAPHORE, rights,
                                    attributes && (attributes->attributes & 2u),
                                    attributes && (attributes->attributes & 0x40u),
                                    true, initial, maximum, false);
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = normalized[0] ?
        find_named_object(normalized, attributes && (attributes->attributes & 0x40u)) : NULL;
    nt_status status = NT_STATUS_SUCCESS;
    if (object != NULL) {
        if (object->kind != SEMAPHORE)
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if (rights & ~object->access)
            status = NT_STATUS_ACCESS_DENIED;
        else {
            object = open_named_alias(object, rights, attributes && (attributes->attributes & 2u));
            status = object ? NT_STATUS_OBJECT_NAME_EXISTS : NT_STATUS_NO_MEMORY;
        }
    } else {
        object = new_waitable(SEMAPHORE);
        if (object == NULL) {
            status = NT_STATUS_NO_MEMORY;
        } else {
            object->access = rights;
            object->semaphore_count = initial;
            object->semaphore_maximum = maximum;
            object->signaled = initial > 0;
            object->handle_flags = attributes && (attributes->attributes & 2u) ? 1u : 0u;
            if (normalized[0])
                memcpy(object->object_name, normalized, strlen(normalized) + 1);
        }
    }
    if (status == NT_STATUS_SUCCESS || status == NT_STATUS_OBJECT_NAME_EXISTS)
        *handle = object->handle;
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtOpenSemaphore(uintptr_t *handle, uint32_t access,
                                       const struct nt_object_attributes *attributes)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (attributes == NULL || attributes->size != sizeof(*attributes) || attributes->name == NULL)
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes->root || attributes->security || attributes->qos ||
        (attributes->attributes & ~(0x40u | 2u)))
        return NT_STATUS_NOT_SUPPORTED;
    char normalized[128];
    const nt_status parsed = named_object_name(attributes->name, normalized);
    if (parsed != NT_STATUS_SUCCESS)
        return parsed;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    if (nt_runtime_broker_enabled())
        return broker_named_waitable(handle, normalized, SEMAPHORE, rights,
                                    (attributes->attributes & 2u) != 0,
                                    (attributes->attributes & 0x40u) != 0,
                                    false, 0, 0, false);
    pthread_mutex_lock(&wait_lock);
    struct waitable *state = find_named_object(normalized, (attributes->attributes & 0x40u) != 0);
    nt_status status = NT_STATUS_SUCCESS;
    if (state == NULL)
        status = NT_STATUS_OBJECT_NAME_NOT_FOUND;
    else if (state->kind != SEMAPHORE)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (rights & ~state->access)
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *alias = open_named_alias(state, rights, (attributes->attributes & 2u) != 0);
        if (alias == NULL)
            status = NT_STATUS_NO_MEMORY;
        else
            *handle = alias->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

struct nt_semaphore_basic_information {
    int32_t current_count, maximum_count;
};
_Static_assert(sizeof(struct nt_semaphore_basic_information) == 8, "SEMAPHORE_BASIC_INFORMATION ABI");

nt_status NT_MS_ABI nt_NtQuerySemaphore(uintptr_t handle, uint32_t information_class,
                                        void *buffer, uint32_t length, uint32_t *returned_length)
{
    if (information_class != 0)
        return NT_STATUS_INVALID_INFO_CLASS;
    if (returned_length != NULL)
        *returned_length = sizeof(struct nt_semaphore_basic_information);
    if (buffer == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length < sizeof(struct nt_semaphore_basic_information))
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (entry == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != SEMAPHORE)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(entry->access & EVENT_QUERY_STATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        const struct waitable *state = wait_state(entry);
        if (state->brokered && !nt_broker_lock(&state->broker)) {
            status = NT_STATUS_UNSUCCESSFUL;
        } else {
            const struct nt_broker_sync_state *shared =
                state->brokered ? state->broker.state : NULL;
            const struct nt_semaphore_basic_information info = {
                shared ? shared->count : state->semaphore_count,
                shared ? shared->maximum : state->semaphore_maximum
            };
            memcpy(buffer, &info, sizeof(info));
            if (state->brokered)
                nt_broker_unlock(&state->broker);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtReleaseSemaphore(uintptr_t handle, int32_t count, int32_t *previous)
{
    if (count <= 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (entry == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != SEMAPHORE)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(entry->access & EVENT_MODIFY_STATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(entry);
        if (state->brokered && !nt_broker_lock(&state->broker)) {
            status = NT_STATUS_UNSUCCESSFUL;
        } else {
            struct nt_broker_sync_state *shared =
                state->brokered ? state->broker.state : NULL;
            int32_t current = shared ? shared->count : state->semaphore_count;
            int32_t limit = shared ? shared->maximum : state->semaphore_maximum;
            if (count > limit - current)
                status = NT_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
            else {
                if (previous)
                    *previous = current;
                if (shared)
                    shared->count += count;
                else {
                    state->semaphore_count += count;
                    state->signaled = true;
                }
                pthread_cond_broadcast(&wait_changed);
            }
            if (state->brokered)
                nt_broker_unlock(&state->broker);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

uintptr_t NT_MS_ABI nt_CreateSemaphoreW(const void *security, int32_t initial,
                                         int32_t maximum, const uint16_t *name)
{
    if (security != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (name != NULL && *name && unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, false);
    uintptr_t handle = 0;
    const nt_status status = nt_NtCreateSemaphore(&handle, EVENT_ALL_ACCESS,
                                                  name && *name ? &attrs : NULL,
                                                  initial, maximum);
    nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

uintptr_t NT_MS_ABI nt_OpenSemaphoreW(uint32_t access, int32_t inherit, const uint16_t *name)
{
    if (name == NULL || !*name) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, inherit != 0);
    uintptr_t handle = 0;
    const nt_status status = nt_NtOpenSemaphore(&handle, access, &attrs);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

int32_t NT_MS_ABI nt_ReleaseSemaphore(uintptr_t handle, int32_t count, int32_t *previous)
{
    const nt_status status = nt_NtReleaseSemaphore(handle, count, previous);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

/* SECURITY_ATTRIBUTES is a Win64 ABI structure, not OBJECT_ATTRIBUTES.
 * The only supported security profile is a NULL descriptor; inheritance is
 * a property of this HANDLE entry, not the shared event's signaled state.
 * Actual serialization of an event into a child process is unsupported and
 * CreateProcessW must reject it before posix_spawn. */
struct nt_event_security_attributes {
    uint32_t length;
    const void *descriptor;
    int32_t inherit;
};
_Static_assert(sizeof(struct nt_event_security_attributes) == 24 &&
               offsetof(struct nt_event_security_attributes, descriptor) == 8 &&
               offsetof(struct nt_event_security_attributes, inherit) == 16,
               "Win64 SECURITY_ATTRIBUTES ABI");

uintptr_t NT_MS_ABI nt_CreateEventW(const void *security, int32_t manual, int32_t initial,
                                    const uint16_t *name)
{
    bool inherit = false;
    if (security != NULL) {
        const struct nt_event_security_attributes *attributes = security;
        if (attributes->length != sizeof(*attributes)) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (attributes->descriptor != NULL) {
            nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
            return 0;
        }
        inherit = attributes->inherit != 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (name != NULL && name[0] != 0 && unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Unnamed events MUST still forward OBJECT_ATTRIBUTES: the inherited
     * HANDLE flag is independent from whether an object has a name.
     * Supplying NULL to NtCreateEvent silently dropped bInheritHandle. */
    struct nt_object_attributes attrs =
        win_event_attributes(name && *name ? &unicode : NULL, inherit);
    uintptr_t handle = 0;
    const nt_status status = nt_NtCreateEvent(&handle, EVENT_ALL_ACCESS,
                                               &attrs, manual ? 0 : 1, initial != 0);
    nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

uintptr_t NT_MS_ABI nt_OpenEventW(uint32_t access, int32_t inherit, const uint16_t *name)
{
    if (name == NULL || !*name) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, inherit != 0);
    uintptr_t handle = 0;
    const nt_status status = nt_NtOpenEvent(&handle, access, &attrs);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
}

struct nt_event_basic_information {
    uint32_t type;
    int32_t state;
};

nt_status NT_MS_ABI nt_NtQueryEvent(uintptr_t handle, uint32_t information_class,
                                    void *buffer, uint32_t length, uint32_t *returned_length)
{
    if (information_class != 0)
        return NT_STATUS_INVALID_INFO_CLASS;
    if (returned_length)
        *returned_length = sizeof(struct nt_event_basic_information);
    if (buffer == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length < sizeof(struct nt_event_basic_information))
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (entry == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != EVENT)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(entry->access & EVENT_QUERY_STATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(entry);
        if (state->brokered && !nt_broker_lock(&state->broker)) {
            status = NT_STATUS_UNSUCCESSFUL;
        } else {
            const struct nt_broker_sync_state *shared =
                state->brokered ? state->broker.state : NULL;
            const struct nt_event_basic_information information = {
                shared ? (shared->manual ? 0u : 1u) : (state->manual ? 0u : 1u),
                shared ? (shared->count != 0) : (state->signaled ? 1 : 0)
            };
            memcpy(buffer, &information, sizeof(information));
            if (state->brokered)
                nt_broker_unlock(&state->broker);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

static nt_status change_event(uintptr_t handle, bool signaled, int32_t *previous)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *event = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (!event) {
        status = NT_STATUS_INVALID_HANDLE;
    } else if (event->kind != EVENT) {
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    } else if (!(event->access & EVENT_MODIFY_STATE)) {
        status = NT_STATUS_ACCESS_DENIED;
    } else {
        struct waitable *state = wait_state(event);
        if (state->brokered && !nt_broker_lock(&state->broker))
            status = NT_STATUS_UNSUCCESSFUL;
        else {
            struct nt_broker_sync_state *shared =
                state->brokered ? state->broker.state : NULL;
            if (previous)
                *previous = shared ? (shared->count != 0) : (state->signaled ? 1 : 0);
            if (shared)
                shared->count = signaled ? 1 : 0;
            else
                state->signaled = signaled;
            if (state->brokered)
                nt_broker_unlock(&state->broker);
            if (signaled)
                pthread_cond_broadcast(&wait_changed);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status NT_MS_ABI nt_NtSetEvent(uintptr_t handle, int32_t *previous) { return change_event(handle, true, previous); }
nt_status NT_MS_ABI nt_NtResetEvent(uintptr_t handle, int32_t *previous) { return change_event(handle, false, previous); }

static int32_t win_change_event(uintptr_t handle, bool signaled)
{
    const nt_status status = change_event(handle, signaled, NULL);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

int32_t NT_MS_ABI nt_SetEvent(uintptr_t handle) { return win_change_event(handle, true); }
int32_t NT_MS_ABI nt_ResetEvent(uintptr_t handle) { return win_change_event(handle, false); }

/* Called with wait_lock held. References keep closed slots from being reused. */
static nt_status acquire_wait_objects(struct waitable **objects, uint32_t count, const uintptr_t *handles)
{
    for (uint32_t i = 0; i < count; ++i) {
        struct waitable *object = find_waitable(handles[i]);
        if (!object)
            return NT_STATUS_INVALID_HANDLE;
        if (!(object->access & SYNCHRONIZE))
            return NT_STATUS_ACCESS_DENIED;
        for (uint32_t j = 0; j < i; ++j) {
            const struct waitable *a = wait_state(objects[j]);
            const struct waitable *b = wait_state(object);
            if (a == b)
                return NT_STATUS_INVALID_PARAMETER;
            if (a->brokered && b->brokered) {
                struct stat sa, sb;
                if (fstat(a->broker.fd, &sa) != 0 ||
                    fstat(b->broker.fd, &sb) != 0)
                    return NT_STATUS_UNSUCCESSFUL;
                if (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino)
                    return NT_STATUS_INVALID_PARAMETER;
            }
        }
        objects[i] = object;
    }
    for (uint32_t i = 0; i < count; ++i)
        ++objects[i]->refs;
    return NT_STATUS_SUCCESS;
}

static nt_status check_wait_objects(struct waitable *const *objects, uint32_t count, bool all)
{
    /* The global broker gate is shared by ALL event/semaphore memfds from
     * one broker. Taking it while holding wait_lock also makes WaitAll
     * consume multiple named semaphores as a single transaction against
     * other clients of the same broker. */
    const struct nt_broker_resource *gate = NULL;
    for (uint32_t i = 0; i < count; ++i) {
        struct waitable *state = wait_state(objects[i]);
        if (state->brokered) {
            gate = &state->broker;
            break;
        }
    }
    if (gate && !nt_broker_lock(gate))
        return NT_STATUS_UNSUCCESSFUL;
    uint32_t first = count, signaled = 0;
    nt_status result = NT_STATUS_TIMEOUT;
    for (uint32_t i = 0; i < count; ++i) {
        if (!objects[i]->handle) {
            result = NT_STATUS_INVALID_HANDLE;
            goto finish;
        }
        struct waitable *state = wait_state(objects[i]);
        bool ready = state->brokered ?
            ((struct nt_broker_sync_state *)state->broker.state)->count > 0 :
            state->signaled;
        if (ready) {
            if (!signaled)
                first = i;
            ++signaled;
        }
    }
    if (all ? signaled != count : signaled == 0)
        goto finish;
    const uint32_t start = all ? 0 : first;
    const uint32_t end = all ? count : first + 1;
    for (uint32_t i = start; i < end; ++i) {
        struct waitable *state = wait_state(objects[i]);
        if (state->brokered) {
            struct nt_broker_sync_state *shared = state->broker.state;
            if (objects[i]->kind == SEMAPHORE ||
                (objects[i]->kind == EVENT && !shared->manual))
                --shared->count;
        } else {
            if (objects[i]->kind == EVENT && !state->manual)
                state->signaled = false;
            if (objects[i]->kind == SEMAPHORE) {
                --state->semaphore_count;
                state->signaled = state->semaphore_count > 0;
            }
        }
    }
    result = all ? NT_STATUS_SUCCESS : (nt_status)first;
finish:
    if (gate)
        nt_broker_unlock(gate);
    return result;
}

nt_status NT_MS_ABI nt_NtWaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                              uint32_t type, uint8_t alertable, const int64_t *timeout)
{
    if (!count || count > MAXIMUM_WAIT_OBJECTS || type > 1)
        return NT_STATUS_INVALID_PARAMETER;
    if (!handles)
        return NT_STATUS_ACCESS_VIOLATION;
    struct nt_wait_deadline deadline;
    if (timeout) {
        const nt_status status = nt_runtime_wait_deadline(*timeout, &deadline);
        if (status != NT_STATUS_SUCCESS)
            return status;
    }
    struct waitable *objects[MAXIMUM_WAIT_OBJECTS];
    pthread_mutex_lock(&wait_lock);
    nt_status result = acquire_wait_objects(objects, count, handles);
    struct guest_apc *apc = NULL;
    bool really_waited = false;
    if (result == NT_STATUS_SUCCESS) {
        bool has_broker = false;
        for (uint32_t i = 0; i < count; ++i)
            if (wait_state(objects[i])->brokered)
                has_broker = true;
        int error = timeout && deadline.time.tv_sec < 0 ? ETIMEDOUT : 0;
        for (;;) {
            if (alertable != 0) {
                cooperative_suspend_checkpoint_locked();
                apc = pop_apc_locked();
                if (apc != NULL) {
                    result = NT_STATUS_USER_APC;
                    break;
                }
                if (consume_alert_locked()) {
                    result = NT_STATUS_ALERTED;
                    break;
                }
            }
            result = check_wait_objects(objects, count, type == 0);
            if (result != NT_STATUS_TIMEOUT || error)
                break;
            really_waited = true;
            if (!has_broker) {
                error = timeout ?
                    pthread_cond_clockwait(&wait_changed, &wait_lock, deadline.clock, &deadline.time) :
                    pthread_cond_wait(&wait_changed, &wait_lock);
            } else {
                /* Linux clients cannot signal another process's local
                 * pthread condition. Poll the shared memory after bounded
                 * 10ms slices while retaining full alertable-wait support.
                 * This is real cross-process synchronization; the tradeoff
                 * is wakeup latency, not a fake success result. */
                clockid_t clock = timeout ? deadline.clock : CLOCK_MONOTONIC;
                struct timespec slice;
                if (clock_gettime(clock, &slice) != 0) {
                    error = EINVAL;
                    break;
                }
                slice.tv_nsec += 10000000;
                if (slice.tv_nsec >= 1000000000L) {
                    slice.tv_sec += 1;
                    slice.tv_nsec -= 1000000000L;
                }
                if (timeout &&
                    (deadline.time.tv_sec < slice.tv_sec ||
                     (deadline.time.tv_sec == slice.tv_sec &&
                      deadline.time.tv_nsec < slice.tv_nsec)))
                    slice = deadline.time;
                error = pthread_cond_clockwait(&wait_changed, &wait_lock,
                                                clock, &slice);
                if (error == ETIMEDOUT) {
                    if (!timeout) {
                        error = 0;
                    } else {
                        struct timespec now;
                        if (clock_gettime(clock, &now) == 0 &&
                            (now.tv_sec < deadline.time.tv_sec ||
                             (now.tv_sec == deadline.time.tv_sec &&
                              now.tv_nsec < deadline.time.tv_nsec)))
                            error = 0;
                    }
                }
            }
        }
        if (result == NT_STATUS_TIMEOUT && error != ETIMEDOUT)
            result = NT_STATUS_UNSUCCESSFUL;
        for (uint32_t i = 0; i < count; ++i)
            release_waitable(objects[i]);
    }
    /* A signal must satisfy a REAL blocked guest wait, not timeout/poll.
     * The monitor restores nice on a monotonic deadline independently
     * of subsequent guest calls, even if the guest becomes CPU-bound. */
    if (really_waited && result >= NT_STATUS_SUCCESS &&
        result < (nt_status)MAXIMUM_WAIT_OBJECTS)
        scheduler_guest_wait_woke_locked();
    pthread_mutex_unlock(&wait_lock);
    if (apc != NULL)
        invoke_apc(apc);
    return result;
}

nt_status NT_MS_ABI nt_NtWaitForSingleObject(uintptr_t handle, uint8_t alertable, const int64_t *timeout)
{
    return nt_NtWaitForMultipleObjects(1, &handle, 1, alertable, timeout);
}

uint32_t NT_MS_ABI nt_WaitForMultipleObjectsEx(uint32_t count, const uintptr_t *handles,
                                            int32_t all, uint32_t milliseconds, int32_t alertable)
{
    const int64_t ticks = -(int64_t)milliseconds * 10000;
    const nt_status status = nt_NtWaitForMultipleObjects(count, handles, all ? 0 : 1, alertable != 0,
                                                        milliseconds == UINT32_MAX ? NULL : &ticks);
    if (status >= 0)
        return (uint32_t)status;
    nt_SetLastError(nt_runtime_nt_error(status));
    return UINT32_MAX;
}

uint32_t NT_MS_ABI nt_WaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                          int32_t all, uint32_t milliseconds)
{
    return nt_WaitForMultipleObjectsEx(count, handles, all, milliseconds, 0);
}

uint32_t NT_MS_ABI nt_WaitForSingleObjectEx(uintptr_t handle, uint32_t milliseconds, int32_t alertable)
{
    return nt_WaitForMultipleObjectsEx(1, &handle, 0, milliseconds, alertable);
}

uint32_t NT_MS_ABI nt_WaitForSingleObject(uintptr_t handle, uint32_t milliseconds)
{
    return nt_WaitForSingleObjectEx(handle, milliseconds, 0);
}

struct child_watch {
    pid_t pid;
    int status_fd;
    struct waitable *process, *thread;
    uint64_t broker_generation;
};

static void *reap_child(void *context)
{
    struct child_watch *watch = context;
    int status = 0;
    struct rusage usage = {0};
    pid_t result;
    do {
        result = wait4(watch->pid, &status, 0, &usage);
    } while (result < 0 && errno == EINTR);
    uint32_t code = NT_ERROR_GEN_FAILURE;
    if (result == watch->pid && WIFEXITED(status)) {
        ssize_t bytes;
        do {
            bytes = read(watch->status_fd, &code, sizeof(code));
        } while (bytes < 0 && errno == EINTR);
        if (bytes != (ssize_t)sizeof(code))
            code = (uint32_t)WEXITSTATUS(status);
    } else if (result == watch->pid && WIFSIGNALED(status)) {
        code = (uint32_t)(128 + WTERMSIG(status));
    }
    close(watch->status_fd);
    pthread_mutex_lock(&wait_lock);
    if (watch->process->termination_requested)
        code = watch->process->requested_exit_code;
    if (watch->thread->start_gate_fd >= 0) {
        close(watch->thread->start_gate_fd);
        watch->thread->start_gate_fd = -1;
    }
    if (watch->broker_generation != 0)
        nt_broker_unregister_child(watch->pid, watch->broker_generation);
    if (watch->process->pidfd >= 0) {
        close(watch->process->pidfd);
        watch->process->pidfd = -1;
    }
    watch->process->exit_code = watch->thread->exit_code = code;
    watch->process->exit_filetime = watch->thread->exit_filetime = filetime_now();
    if (result == watch->pid) {
        watch->process->user_cpu_100ns = usage_ticks(usage.ru_utime);
        watch->process->kernel_cpu_100ns = usage_ticks(usage.ru_stime);
    }
    watch->process->signaled = watch->thread->signaled = true;
    release_waitable(watch->process);
    release_waitable(watch->thread);
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
    free(watch);
    return NULL;
}

bool nt_runtime_watch_child(pid_t pid, int status_fd, int start_fd,
                            struct nt_process_information *info)
{
    struct child_watch *watch = malloc(sizeof(*watch));
    pthread_attr_t attr;
    if (!watch)
        return false;
    if (pthread_attr_init(&attr) != 0) {
        free(watch);
        return false;
    }
    int error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&wait_lock);
    struct waitable *process = error == 0 ? new_waitable(PROCESS) : NULL;
    struct waitable *thread = process ? new_waitable(THREAD) : NULL;
    if (thread) {
        ++process->refs;
        ++thread->refs;
        thread->thread_id = (uint32_t)pid;
        thread->owner_pid = (uint32_t)pid;
        thread->access = THREAD_ALL_ACCESS;
        thread->create_filetime = filetime_now();
        process->create_filetime = thread->create_filetime;
        thread->child_guest = true;
        thread->start_gate_fd = start_fd;
        thread->suspend_count = start_fd >= 0 ? 1u : 0u;
        process->access = PROCESS_ALL_ACCESS;
        process->owner_pid = (uint32_t)pid;
        process->pidfd = -1;
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
        /* pidfd pins the child identity against PID reuse after waitpid. */
        process->pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
#endif
        uint64_t broker_generation = 0;
        uint8_t broker_secret[16];
        if (process->pidfd >= 0 &&
            nt_thread_broker_generate_secret(broker_secret)) {
            (void)nt_broker_register_child(pid, process->pidfd, getuid(), getgid(),
                                           broker_secret, &broker_generation);
            memset(broker_secret, 0, sizeof(broker_secret));
        }
        *watch = (struct child_watch){pid, status_fd, process, thread,
                                      broker_generation};
        pthread_t worker;
        error = pthread_create(&worker, &attr, reap_child, watch);
        if (!error)
            *info = (struct nt_process_information){process->handle, thread->handle,
                                                   (uint32_t)pid, (uint32_t)pid};
    } else {
        error = ENOMEM;
    }
    if (error) {
        if (thread && watch->broker_generation != 0)
            nt_broker_unregister_child(pid, watch->broker_generation);
        if (process) {
            if (process->pidfd >= 0)
                close(process->pidfd);
            *process = (struct waitable){0};
        }
        if (thread)
            *thread = (struct waitable){0};
        free(watch);
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_attr_destroy(&attr);
    return error == 0;
}

/* Only the trusted CreateProcess launcher can activate cross-PE IPC.
 * The existing watch path preserves child lifetime and read-only primary
 * thread aliases. The server retains its own pidfd and socket until EOF. */
bool nt_runtime_watch_child_broker(pid_t pid, int status_fd, int start_fd,
                                   int broker_fd, const uint8_t secret[16],
                                   struct nt_process_information *info)
{
    if (!nt_runtime_watch_child(pid, status_fd, start_fd, info)) {
        if (broker_fd >= 0)
            close(broker_fd);
        return false;
    }
    if (broker_fd < 0)
        return true; /* trusted native tests / legacy launchers */
    if (secret == NULL || !nt_broker_install_child_session(pid, secret)) {
        close(broker_fd);
        return false;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *process = find_waitable(info->process);
    int pinned = -1;
    if (process != NULL && process->kind == PROCESS) {
        const struct waitable *canonical = wait_state(process);
        if (canonical->owner_pid == (uint32_t)pid &&
            canonical->pidfd >= 0 && !canonical->signaled)
            pinned = fcntl(canonical->pidfd, F_DUPFD_CLOEXEC, 3);
    }
    pthread_mutex_unlock(&wait_lock);
    if (pinned < 0) {
        close(broker_fd);
        return false;
    }
    struct nt_thread_broker_registered_peer peer = {
        .child_pid = pid, .uid = getuid(), .gid = getgid(),
        .child_pidfd = pinned,
        .allowed_access = NT_BROKER_QUERY_LIMITED_INFORMATION
    };
    memcpy(peer.secret, secret, sizeof(peer.secret));
    nt_broker_grant_existing_sibling_queries(pid);
    const bool started = nt_broker_server_start(broker_fd, &peer);
    close(pinned); /* detached server owns an independent pidfd dup */
    if (!started) {
        close(broker_fd);
        return false;
    }
    /* Once the detached service exists, it owns broker_fd even on failure.
     * A failure to authenticate forbids CreateProcess success; the caller
     * kills the child, while the reaper revokes every registered capability. */
    if (!nt_broker_server_handshake(broker_fd, secret)) {
        (void)shutdown(broker_fd, SHUT_RDWR);
        return false;
    }
    return true;
}

/* Only processes launched by our trusted host have a pidfd and an
 * observed child reaper. Never signal an arbitrary user-supplied PID.
 * The reaper retains the requested 32-bit Windows exit status. */
nt_status nt_runtime_terminate_child_process(uintptr_t handle, uint32_t code)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->kind != PROCESS)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(object->access & 1u)) /* PROCESS_TERMINATE */
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(object);
        if (state->signaled)
            status = NT_STATUS_ACCESS_DENIED;
        else if (state->pidfd < 0)
            status = NT_STATUS_NOT_SUPPORTED;
#if defined(SYS_pidfd_send_signal)
        else if (syscall(SYS_pidfd_send_signal, state->pidfd, SIGKILL, NULL, 0) == 0) {
            state->termination_requested = true;
            state->requested_exit_code = code;
        } else {
            status = errno == ESRCH ? NT_STATUS_ACCESS_DENIED : NT_STATUS_UNSUCCESSFUL;
        }
#endif
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

int32_t NT_MS_ABI nt_GetExitCodeProcess(uintptr_t handle, uint32_t *code)
{
    if (!code) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (handle == UINTPTR_MAX) { /* GetCurrentProcess */
        *code = STILL_ACTIVE;
        return 1;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    uint32_t error = NT_ERROR_INVALID_HANDLE;
    if (object != NULL && object->kind == PROCESS) {
        if (!(object->access & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION)))
            error = NT_ERROR_ACCESS_DENIED;
        else {
            const struct waitable *state = wait_state(object);
            *code = state->signaled ? state->exit_code : STILL_ACTIVE;
            error = 0;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (error != 0)
        nt_SetLastError(error);
    return error == 0;
}

/* Reopen an owned child by PID without confusing the numeric PID with
 * the guest HANDLE. The runtime does not expose arbitrary host processes. */
static nt_status open_local_process(uintptr_t *result, uint32_t access,
                                    bool inherit, uint32_t pid)
{
    if (result == NULL || pid == 0 || (access & ~PROCESS_ALL_ACCESS) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *original = NULL;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        struct waitable *item = &waitables[i];
        if (item->kind == PROCESS && item->shared == NULL &&
            (item->refs != 0 || item->links != 0) && item->owner_pid == pid) {
            original = item;
            break;
        }
    }
    /* Model the current process as a persistent canonical state. Only
     * aliases are exposed to the guest; the internal canonical HANDLE
     * itself remains zero and cannot be closed by the application. */
    if (original == NULL && pid == (uint32_t)getpid()) {
        if (current_process_state == NULL) {
            current_process_state = new_waitable(PROCESS);
            if (current_process_state != NULL) {
                current_process_state->handle = 0;
                current_process_state->owner_pid = (uint32_t)getpid();
                current_process_state->access = PROCESS_ALL_ACCESS;
                current_process_state->self_process = true;
            }
        }
        original = current_process_state;
    }
    nt_status status = original == NULL && pid == (uint32_t)getpid() ?
                       NT_STATUS_NO_MEMORY : NT_STATUS_INVALID_CID;
    if (original != NULL) {
        if (access & ~original->access)
            status = NT_STATUS_ACCESS_DENIED;
        else {
            struct waitable *alias = new_waitable(PROCESS);
            if (alias == NULL)
                status = NT_STATUS_NO_MEMORY;
            else {
                ++original->links;
                alias->shared = original;
                alias->access = access;
                alias->handle_flags = inherit ? 1u : 0u;
                *result = alias->handle;
                status = NT_STATUS_SUCCESS;
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

nt_status nt_runtime_check_self_process(uintptr_t handle, uint32_t any_access)
{
    if (handle == UINTPTR_MAX)
        return NT_STATUS_SUCCESS;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->kind != PROCESS)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!wait_state(object)->self_process)
        status = NT_STATUS_NOT_SUPPORTED;
    else if (any_access != 0 && !(object->access & any_access))
        status = NT_STATUS_ACCESS_DENIED;
    pthread_mutex_unlock(&wait_lock);
    return status;
}

uintptr_t NT_MS_ABI nt_OpenProcess(uint32_t access, int32_t inherit, uint32_t pid)
{
    uintptr_t opened = 0;
    const nt_status status = open_local_process(&opened, access, inherit != 0, pid);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(status == NT_STATUS_INVALID_CID ?
                        NT_ERROR_INVALID_PARAMETER : nt_runtime_nt_error(status));
    return opened;
}

nt_status NT_MS_ABI nt_NtOpenProcess(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes,
                                   const struct nt_client_id *client_id)
{
    if (handle == NULL || client_id == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (attributes != NULL) {
        if (attributes->size != sizeof(*attributes))
            return NT_STATUS_INVALID_PARAMETER;
        if (attributes->name != NULL || attributes->root != 0 ||
            attributes->security != NULL || attributes->qos != NULL ||
            (attributes->attributes & ~2u) != 0)
            return NT_STATUS_NOT_SUPPORTED;
    }
    if (client_id->unique_thread != 0)
        return NT_STATUS_INVALID_PARAMETER;
    if (client_id->unique_process > UINT32_MAX)
        return NT_STATUS_INVALID_CID;
    return open_local_process(handle, access,
                              attributes != NULL && (attributes->attributes & 2u) != 0,
                              (uint32_t)client_id->unique_process);
}

uintptr_t NT_MS_ABI nt_GetCurrentThread(void) { return UINTPTR_MAX - 1; }

/* Identity is taken from the canonical object, not from its numerical
 * HANDLE. In particular a child-process handle is never a host PID. */
uint32_t NT_MS_ABI nt_GetProcessId(uintptr_t handle)
{
    if (handle == UINTPTR_MAX)
        return (uint32_t)getpid();
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    uint32_t pid = 0;
    uint32_t error = NT_ERROR_INVALID_HANDLE;
    if (object != NULL && object->kind == PROCESS) {
        if (!(object->access & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION)))
            error = NT_ERROR_ACCESS_DENIED;
        else {
            pid = wait_state(object)->owner_pid;
            error = 0;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (error != 0)
        nt_SetLastError(error);
    return pid;
}

uint32_t NT_MS_ABI nt_GetProcessIdOfThread(uintptr_t handle)
{
    if (handle == UINTPTR_MAX - 1)
        return (uint32_t)getpid();
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    uint32_t pid = 0;
    uint32_t error = NT_ERROR_INVALID_HANDLE;
    if (object != NULL && object->kind == THREAD) {
        if (!(object->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)))
            error = NT_ERROR_ACCESS_DENIED;
        else {
            pid = wait_state(object)->owner_pid;
            error = 0;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (error != 0)
        nt_SetLastError(error);
    return pid;
}
/* Thread metadata is kept in the same canonical table as wait handles, so
 * OpenThread and native queries observe the same guest TEB and thread ID.
 * No host pthread_t is returned as a Win32 HANDLE. */
nt_status nt_runtime_query_thread(uintptr_t handle, uint32_t *thread_id,
                                   uintptr_t *teb, uint32_t *exit_code)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (wait_state(object)->remote_guest &&
             (teb != NULL || exit_code != NULL))
        status = NT_STATUS_NOT_SUPPORTED;
    else if (wait_state(object)->owner_pid != (uint32_t)getpid() &&
             (!wait_state(object)->child_guest || teb != NULL))
        status = NT_STATUS_NOT_SUPPORTED;
    else if (!(object->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        const struct waitable *state = wait_state(object);
        if (thread_id != NULL)
            *thread_id = state->thread_id;
        if (teb != NULL)
            *teb = state->teb_base;
        if (exit_code != NULL)
            *exit_code = state->signaled ? state->exit_code : STILL_ACTIVE;
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

uint32_t NT_MS_ABI nt_GetThreadId(uintptr_t handle)
{
    if (handle == nt_GetCurrentThread())
        return (uint32_t)gettid();
    uint32_t id = 0;
    const nt_status status = nt_runtime_query_thread(handle, &id, NULL, NULL);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS ? id : 0;
}

/* Read-only OpenThread for the initial thread of a child actually launched
 * by this runtime. The broker validates an owned live pidfd, unique
 * generation and query authority. Never use a raw guest PID as a syscall
 * target. Existing local threads retain their previous permissions. */
static bool registered_child_query_locked(const struct waitable *state,
                                          uint32_t access)
{
    const uint32_t query = access &
        (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION);
    if (!state->child_guest || state->signaled ||
        state->owner_pid != state->thread_id || query == 0 ||
        (access & ~(THREAD_QUERY_INFORMATION |
                    THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE)) != 0)
        return false;
    uint64_t capability = 0;
    if (!nt_broker_open_child_thread(getpid(), (pid_t)state->owner_pid,
                                     query, &capability))
        return false;
    struct nt_broker_thread_info observed = {0};
    const bool validated = nt_broker_query_child_thread(getpid(), capability,
                                     query, &observed) &&
                           observed.guest_process_id == state->owner_pid &&
                           observed.guest_thread_id == state->thread_id;
    (void)nt_broker_close_child_thread(getpid(), capability);
    return validated;
}
static nt_status open_thread_local(uintptr_t *result, uint32_t access,
                                   bool inherit, uint32_t id)
{
    if (result == NULL || id == 0 || (access & ~THREAD_ALL_ACCESS) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *original = NULL;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        struct waitable *item = &waitables[i];
        if (item->kind == THREAD && item->shared == NULL &&
            (item->refs != 0 || item->links != 0) &&
            item->thread_id == id &&
            (item->owner_pid == (uint32_t)getpid() ||
             (item->child_guest && item->owner_pid == id))) {
            original = item;
            break;
        }
    }
    nt_status status = NT_STATUS_INVALID_CID;
    if (original != NULL) {
        if (access & ~original->access) {
            status = NT_STATUS_ACCESS_DENIED;
        } else if (original->child_guest &&
                   !registered_child_query_locked(original, access)) {
            status = NT_STATUS_NOT_SUPPORTED;
        } else {
            struct waitable *alias = new_waitable(THREAD);
            if (alias == NULL)
                status = NT_STATUS_NO_MEMORY;
            else {
                ++original->links;
                alias->shared = original;
                alias->access = access;
                alias->handle_flags = inherit ? 1u : 0u;
                *result = alias->handle;
                status = NT_STATUS_SUCCESS;
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

/* A foreign guest ID is never passed to setpriority, sched_setaffinity
 * or Linux signals. The only supported cross-PE capability here is a
 * broker-proven QUERY_LIMITED_INFORMATION alias with a stable NT handle. */
static nt_status open_thread_remote(uintptr_t *result, uint32_t access,
                                   bool inherit, uint32_t id,
                                   uint32_t expected_owner)
{
    if (result == NULL || access != THREAD_QUERY_LIMITED_INFORMATION || id == 0)
        return NT_STATUS_NOT_SUPPORTED;
    struct nt_broker_thread_info identity = {0};
    uint64_t capability = 0;
    if (!nt_broker_client_open(id, access, &identity, &capability))
        return NT_STATUS_NOT_SUPPORTED;
    if (identity.guest_thread_id != id ||
        identity.guest_process_id == (uint32_t)getpid() ||
        (expected_owner != 0 && identity.guest_process_id != expected_owner)) {
        (void)nt_broker_client_close(capability);
        return NT_STATUS_NOT_SUPPORTED;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *alias = new_waitable(THREAD);
    if (alias != NULL) {
        alias->owner_pid = identity.guest_process_id;
        alias->thread_id = identity.guest_thread_id;
        alias->access = access;
        alias->remote_guest = true;
        alias->remote_broker_capability = capability;
        alias->remote_broker_generation = identity.generation;
        alias->handle_flags = inherit ? 1u : 0u;
        *result = alias->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    if (alias == NULL) {
        (void)nt_broker_client_close(capability);
        return NT_STATUS_NO_MEMORY;
    }
    return NT_STATUS_SUCCESS;
}

uintptr_t NT_MS_ABI nt_OpenThread(uint32_t access, int32_t inherit, uint32_t id)
{
    uintptr_t opened = 0;
    nt_status status = open_thread_local(&opened, access, inherit != 0, id);
    if (status == NT_STATUS_INVALID_CID && nt_broker_client_is_attached())
        status = open_thread_remote(&opened, access, inherit != 0, id, 0);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(status == NT_STATUS_INVALID_CID ?
                        NT_ERROR_INVALID_PARAMETER : nt_runtime_nt_error(status));
    return opened;
}

/* No directory names or security descriptors on this bounded, same-process
 * NtOpenThread profile. Native calls never touch the guest's LastError. */
nt_status NT_MS_ABI nt_NtOpenThread(uintptr_t *handle, uint32_t access,
                                  const struct nt_object_attributes *attributes,
                                  const struct nt_client_id *client_id)
{
    if (handle == NULL || client_id == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (attributes != NULL) {
        if (attributes->size != sizeof(*attributes))
            return NT_STATUS_INVALID_PARAMETER;
        if (attributes->root != 0 || attributes->name != NULL ||
            attributes->security != NULL || attributes->qos != NULL ||
            (attributes->attributes & ~2u) != 0)
            return NT_STATUS_NOT_SUPPORTED;
    }
    if (client_id->unique_thread > UINT32_MAX)
        return NT_STATUS_INVALID_CID;
    const bool remote = client_id->unique_process != 0 &&
                        client_id->unique_process != (uintptr_t)getpid();
    if (remote && client_id->unique_process > UINT32_MAX)
        return NT_STATUS_INVALID_CID;
    uintptr_t opened = 0;
    nt_status result = open_thread_local(&opened, access,
                             attributes != NULL && (attributes->attributes & 2u) != 0,
                             (uint32_t)client_id->unique_thread);
    if (result == NT_STATUS_SUCCESS && client_id->unique_process != 0) {
        pthread_mutex_lock(&wait_lock);
        struct waitable *opened_handle = find_waitable(opened);
        const bool matches = opened_handle != NULL &&
            wait_state(opened_handle)->owner_pid ==
                (uint32_t)client_id->unique_process;
        pthread_mutex_unlock(&wait_lock);
        if (!matches) {
            (void)nt_runtime_close_waitable_status(opened);
            return NT_STATUS_NOT_SUPPORTED;
        }
    }
    if (result == NT_STATUS_INVALID_CID && nt_broker_client_is_attached())
        result = open_thread_remote(&opened, access,
                                   attributes != NULL && (attributes->attributes & 2u) != 0,
                                   (uint32_t)client_id->unique_thread,
                                   remote ? (uint32_t)client_id->unique_process : 0);
    if (remote && result == NT_STATUS_INVALID_CID)
        return NT_STATUS_NOT_SUPPORTED;
    if (result == NT_STATUS_SUCCESS)
        *handle = opened; /* preserve native output exactly on failure */
    return result;
}



/* The missing join between a Windows HANDLE and the authenticated
 * per-process supervisor lease. Keep wait_lock OUT of the synchronous
 * IPC call: the parent may need to inspect the child's /proc TIDs.
 * The HANDLE values are monotone and we revalidate them after RPC;
 * a close/race cannot turn a stale HANDLE into another target. */
bool nt_runtime_supervisor_open_thread_handle(uintptr_t handle, uint32_t rights,
    struct nt_broker_thread_info *identity, uint64_t *capability)
{
    if (!identity || !capability || rights == 0 ||
        (rights & ~NT_BROKER_SUPERVISOR_ACCESS_MASK) != 0)
        return false;
    uint32_t thread_id = 0;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    if (entry != NULL && entry->kind == THREAD &&
        (entry->access & rights) == rights) {
        const struct waitable *state = wait_state(entry);
        if (!entry->remote_guest && !state->child_guest &&
            !state->signaled && state->owner_pid == (uint32_t)getpid())
            thread_id = state->thread_id;
    }
    pthread_mutex_unlock(&wait_lock);
    if (thread_id == 0)
        return false;
    struct nt_broker_thread_info claimed = {0};
    uint64_t lease = 0;
    if (!nt_broker_client_supervisor_open(thread_id, rights, &claimed, &lease))
        return false;
    pthread_mutex_lock(&wait_lock);
    entry = find_waitable(handle);
    bool current = entry != NULL && entry->kind == THREAD &&
        (entry->access & rights) == rights && !entry->remote_guest;
    if (current) {
        const struct waitable *state = wait_state(entry);
        current = !state->child_guest && !state->signaled &&
            state->owner_pid == (uint32_t)getpid() &&
            state->thread_id == thread_id;
    }
    pthread_mutex_unlock(&wait_lock);
    if (!current || claimed.guest_process_id != (uint32_t)getpid() ||
        claimed.guest_thread_id != thread_id || claimed.generation == 0) {
        (void)nt_broker_client_supervisor_close(lease);
        return false;
    }
    *identity = claimed;
    *capability = lease;
    return true;
}

static void finish_thread(struct nt_guest_thread *guest, bool attached)
{
    nt_runtime_run_fls_cleanup();
    nt_broker_client_unregister_thread((uint32_t)gettid());
    if (guest->entered) {
        /* A thread killed while CREATE_SUSPENDED has not yet executed
         * TLS callbacks or DLL_THREAD_ATTACH. It must not send DETACH. */
        if (attached)
            nt_runtime_notify_tls(3); /* DLL_THREAD_DETACH */
        nt_guest_thread_leave(guest);
    }
    pthread_mutex_lock(&wait_lock);
    struct rusage usage;
    if (getrusage(RUSAGE_THREAD, &usage) == 0) {
        running_thread->user_cpu_100ns = usage_ticks(usage.ru_utime);
        running_thread->kernel_cpu_100ns = usage_ticks(usage.ru_stime);
    }
    running_thread->exit_filetime = filetime_now();
    running_thread->final_group0_affinity = linux_group0_affinity(pthread_self());
    errno = 0;
    running_thread->final_nice = getpriority(PRIO_PROCESS, (id_t)gettid());
    running_thread->final_nice_valid = errno == 0;
    if (running_thread->dynamic_boost.active) {
        running_thread->final_nice = running_thread->dynamic_boost.base_nice;
        running_thread->dynamic_boost.active = false;
    }
    running_thread->exit_code = thread_result;
    running_thread->teb_base = 0; /* stack-allocated TEB is no longer live */
    clear_apcs_locked(&running_thread->apcs);
    clear_apcs_locked(&running_thread->special_apcs);
    running_thread->signaled = true;
    release_waitable(running_thread);
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
}

static void *thread_main(void *context)
{
    running_thread = context;
    const uint32_t host_tid = (uint32_t)gettid();
    /* This is a host runtime notification only. The parent revalidates
     * membership against its own /proc/<registered PID>/task/<TID> view. */
    (void)nt_broker_client_register_thread(host_tid);
    struct nt_guest_thread guest = {0};
    const uintptr_t image = nt_runtime_guest_module();
    const bool ready = image == 0 || nt_runtime_enter_guest_thread(&guest, image, nt_runtime_guest_tls());
    pthread_mutex_lock(&wait_lock);
    running_thread->thread_id = (uint32_t)gettid();
    running_thread->owner_pid = (uint32_t)getpid();
    running_thread->teb_base = guest.entered ? (uintptr_t)guest.teb : 0;
    running_thread->started = true;
    pthread_cond_broadcast(&wait_changed);
    /* CREATE_SUSPENDED must prevent guest instructions, TLS callbacks and
     * the user's thread entry from running until ResumeThread. */
    while (running_thread->suspend_count != 0 && !running_thread->terminate_at_start)
        pthread_cond_wait(&wait_changed, &wait_lock);
    const bool cancelled = running_thread->terminate_at_start;
    const uint32_t cancel_code = running_thread->requested_exit_code;
    pthread_mutex_unlock(&wait_lock);
    volatile bool attached = false; /* survives ExitThread siglongjmp */
    if (cancelled) {
        thread_result = cancel_code;
    } else if (sigsetjmp(thread_exit, 1) == 0) {
        if (guest.entered) {
            attached = true;
            nt_runtime_notify_tls(2); /* DLL_THREAD_ATTACH */
        }
        if (ready && guest.entered)
            nt_guest_boundary_guest_enter();
        thread_result = ready ? running_thread->start(running_thread->parameter) : NT_ERROR_GEN_FAILURE;
        if (ready && guest.entered)
            nt_guest_boundary_guest_leave();
    }
    finish_thread(&guest, attached);
    return NULL;
}

uintptr_t NT_MS_ABI nt_CreateThread(const void *security, size_t stack, nt_thread_start start,
                                   void *parameter, uint32_t flags, uint32_t *thread_id)
{
    ensure_processor_groups();
    if (start == NULL || security != NULL || (flags & ~4u) != 0) {
        nt_SetLastError(start == NULL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    int error = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (error == 0 && stack != 0) {
        const size_t minimum = (size_t)PTHREAD_STACK_MIN;
        error = pthread_attr_setstacksize(&attr, stack < minimum ? minimum : stack);
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *thread = error == 0 ? new_waitable(THREAD) : NULL;
    uintptr_t handle = 0;
    if (thread != NULL) {
        /* CREATE_THREAD inherits its creator's primary group if this
         * group is allowed in the caller's actual host cpuset. */
        cpu_set_t creator_affinity;
        if (pthread_getaffinity_np(pthread_self(), sizeof(creator_affinity),
                                   &creator_affinity) == 0) {
            struct nt_group_affinity inherited = {0};
            if (thread_primary_affinity_locked(nt_GetCurrentThread(),
                                               &creator_affinity, &inherited) ==
                NT_STATUS_SUCCESS) {
                thread->primary_group = inherited.group;
                thread->primary_group_initialized = true;
                thread->explicit_group_outside_process =
                    inherited.group != process_primary_group;
            }
        }
        thread->start = start;
        thread->create_filetime = filetime_now();
        thread->parameter = parameter;
        thread->access = THREAD_ALL_ACCESS;
        thread->suspend_count = (flags & 4u) ? 1u : 0u;
        ++thread->refs;
        error = pthread_create(&thread->host, &attr, thread_main, thread);
        if (error == 0) {
            while (!thread->started)
                pthread_cond_wait(&wait_changed, &wait_lock);
            handle = thread->handle;
            if (thread_id != NULL)
                *thread_id = thread->thread_id;
        } else {
            thread->handle = 0;
            thread->refs = 0;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_attr_destroy(&attr);
    if (handle == 0)
        nt_SetLastError(error == EINVAL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_NOT_ENOUGH_MEMORY);
    return handle;
}

/* CreateRemoteThread/Ex are supported only when the target HANDLE
 * denotes this guest's current process with PROCESS_CREATE_THREAD rights.
 * An arbitrary Windows HANDLE is never interpreted as a host Linux PID;
 * no cross-process memory injection or token impersonation is attempted. */
uintptr_t NT_MS_ABI nt_CreateRemoteThread(uintptr_t process, const void *security,
                                         size_t stack, nt_thread_start start,
                                         void *parameter, uint32_t flags, uint32_t *thread_id)
{
    const nt_status status = nt_runtime_check_self_process(process, 0x2u);
    if (status != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    return nt_CreateThread(security, stack, start, parameter, flags, thread_id);
}

uintptr_t NT_MS_ABI nt_CreateRemoteThreadEx(uintptr_t process, const void *security,
                                           size_t stack, nt_thread_start start,
                                           void *parameter, uint32_t flags,
                                           const void *attributes, uint32_t *thread_id)
{
    if (attributes != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return nt_CreateRemoteThread(process, security, stack, start,
                                 parameter, flags, thread_id);
}

/* Native thread creation is a bounded adapter over the same owned
 * pthread/TEB path as CreateThread. Unsupported extended attributes must
 * fail before the thread is started. Native entrypoints preserve LastError. */
nt_status NT_MS_ABI nt_NtCreateThreadEx(uintptr_t *handle, uint32_t desired_access,
                                       const struct nt_object_attributes *attributes,
                                       uintptr_t process, nt_thread_start start,
                                       void *argument, uint32_t flags,
                                       size_t zero_bits, size_t stack_commit,
                                       size_t stack_reserve, void *attribute_list)
{
    if (handle == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    *handle = 0;
    /* NtCreateThreadEx accepts real handles for our own process.
     * Unlike pseudo-handles these must grant PROCESS_CREATE_THREAD. */
    const nt_status process_status = nt_runtime_check_self_process(process, 0x2u);
    if (process_status != NT_STATUS_SUCCESS)
        return process_status;
    if (start == NULL || desired_access & ~THREAD_ALL_ACCESS)
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes != NULL) {
        if (attributes->size != sizeof(*attributes))
            return NT_STATUS_INVALID_PARAMETER;
        if (attributes->name != NULL || attributes->root != 0 ||
            attributes->security != NULL || attributes->qos != NULL ||
            attributes->attributes != 0)
            return NT_STATUS_NOT_SUPPORTED;
    }
    if ((flags & ~1u) != 0 || zero_bits != 0 || stack_reserve != 0 ||
        attribute_list != NULL)
        return NT_STATUS_NOT_SUPPORTED;
    const uint32_t old_error = nt_GetLastError();
    uintptr_t created = nt_CreateThread(NULL, stack_commit, start, argument,
                                        (flags & 1u) ? 4u : 0u, NULL);
    const uint32_t error = nt_GetLastError();
    nt_SetLastError(old_error);
    if (created == 0)
        return error == NT_ERROR_NOT_ENOUGH_MEMORY ? NT_STATUS_NO_MEMORY :
               error == NT_ERROR_NOT_SUPPORTED ? NT_STATUS_NOT_SUPPORTED :
               NT_STATUS_INVALID_PARAMETER;
    if (desired_access != THREAD_ALL_ACCESS) {
        uintptr_t reduced = 0;
        nt_status status = nt_runtime_duplicate_waitable(created, desired_access, 0, 0, &reduced);
        nt_runtime_close_waitable(created);
        if (status != NT_STATUS_SUCCESS)
            return status;
        created = reduced;
    }
    *handle = created;
    return NT_STATUS_SUCCESS;
}

static nt_status terminate_suspended_thread(uintptr_t handle, uint32_t exit_code)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(object->access & THREAD_TERMINATE))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(object);
        if (state->owner_pid != (uint32_t)getpid())
            status = NT_STATUS_NOT_SUPPORTED;
        else if (state->signaled || state->terminate_at_start)
            status = NT_STATUS_THREAD_IS_TERMINATING;
        else if (state->supervisor_async_suspended)
            /* This is a REAL ptrace stop, not CREATE_SUSPENDED startup.
             * Clearing the local counter here previously lost the only
             * tracked ResumeThread route and stranded a kernel tracee. */
            status = NT_STATUS_NOT_SUPPORTED;
        else if (state->cooperative_pause_requested ||
                 state->termination_requested ||
                 state->termination_acknowledged)
            status = NT_STATUS_NOT_SUPPORTED;
        else if (state->suspend_count == 0) {
            /* Only a local, owned and TEB-attached PE worker has a
             * reviewed import-return gate. Wait for that worker's ACK;
             * a busy PE loop or blocked host import returns NOT_SUPPORTED
             * without leaving a latent delayed termination request. */
            if (state->remote_guest || state->child_guest || !state->started ||
                state->teb_base == 0 || pthread_equal(state->host, pthread_self()) ||
                !nt_guest_boundary_import_gate_enabled()) {
                status = NT_STATUS_NOT_SUPPORTED;
            } else {
                struct timespec limit;
                if (clock_gettime(CLOCK_MONOTONIC, &limit) != 0) {
                    status = NT_STATUS_UNSUCCESSFUL;
                } else {
                    limit.tv_nsec += 500000000L;
                    if (limit.tv_nsec >= 1000000000L) {
                        ++limit.tv_sec;
                        limit.tv_nsec -= 1000000000L;
                    }
                    /* Pin the canonical object while cond_wait releases
                     * the lock: another guest may close its last HANDLE. */
                    ++state->refs;
                    state->requested_exit_code = exit_code;
                    state->termination_acknowledged = false;
                    atomic_store_explicit(&state->termination_requested,
                                          true, memory_order_release);
                    int error = 0;
                    while (!state->termination_acknowledged &&
                           !state->signaled && error == 0)
                        error = pthread_cond_clockwait(&wait_changed, &wait_lock,
                                                       CLOCK_MONOTONIC, &limit);
                    if (!state->termination_acknowledged) {
                        atomic_store_explicit(&state->termination_requested,
                                              false, memory_order_release);
                        status = state->signaled ?
                            NT_STATUS_THREAD_IS_TERMINATING :
                            NT_STATUS_NOT_SUPPORTED;
                    }
                    release_waitable(state);
                }
            }
        } else {
            /* This is CREATE_SUSPENDED, never a ptrace stop. */
            state->terminate_at_start = true;
            state->requested_exit_code = exit_code;
            state->suspend_count = 0;
            pthread_cond_broadcast(&wait_changed);
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

int32_t NT_MS_ABI nt_TerminateThread(uintptr_t thread, uint32_t exit_code)
{
    /* Win32 does not support the pseudo-handle's self-termination path:
     * use ExitThread for safe self-exit. */
    const nt_status status = terminate_suspended_thread(thread, exit_code);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtTerminateThread(uintptr_t thread, nt_status exit_code)
{
    if (thread == nt_GetCurrentThread()) {
        nt_ExitThread((uint32_t)exit_code);
        return NT_STATUS_SUCCESS; /* not reached */
    }
    /* Remote Linux pthreads remain untouchable. A local PE worker may
     * instead ACK termination at its own safe import-return boundary. */
    return terminate_suspended_thread(thread, (uint32_t)exit_code);
}

/* Parent RPC is only authorized when the *host* explicitly enabled
 * controlled supervision. It fails closed on HOST phase, bad GS/TEB,
 * unsafe code, missing ptrace permissions and unsupported namespaces.
 * No guest-controlled flag can turn it on. */
static bool async_suspend_candidate(uintptr_t handle)
{
    bool eligible = false;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    if (object && object->kind == THREAD &&
        (object->access & THREAD_SUSPEND_RESUME) != 0) {
        const struct waitable *state = wait_state(object);
        eligible = !object->remote_guest && !state->child_guest &&
            state->owner_pid == (uint32_t)getpid() && !state->signaled &&
            !state->terminate_at_start && state->started &&
            state->suspend_count == 0 && !state->cooperative_pause_requested &&
            !pthread_equal(state->host, pthread_self());
    }
    pthread_mutex_unlock(&wait_lock);
    return eligible;
}

/* All asynchronous nested counts must be acknowledged by the parent
 * supervisor, not fabricated by a local fallback. The parent has the
 * authoritative NT suspension count and owns ptrace for that TID. */
static nt_status async_suspend_guest_thread(uintptr_t handle, uint32_t *previous,
                                            bool *attempted_remote_mutation)
{
    *attempted_remote_mutation = false;
    uint32_t local_count = 0;
    bool previously_suspended = false;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    if (object && object->kind == THREAD &&
        (object->access & THREAD_SUSPEND_RESUME) != 0) {
        const struct waitable *state = wait_state(object);
        previously_suspended = state->supervisor_async_suspended;
        local_count = state->suspend_count;
    }
    pthread_mutex_unlock(&wait_lock);
    if (!previously_suspended && !async_suspend_candidate(handle))
        return NT_STATUS_NOT_SUPPORTED;
    if (previously_suspended && (local_count == 0 ||
                               local_count >= MAXIMUM_SUSPEND_COUNT))
        return local_count >= MAXIMUM_SUSPEND_COUNT ?
            NT_STATUS_SUSPEND_COUNT_EXCEEDED : NT_STATUS_NOT_SUPPORTED;

    struct nt_broker_thread_info identity = {0};
    uint64_t capability = 0;
    if (!nt_runtime_supervisor_open_thread_handle(handle,
            THREAD_SUSPEND_RESUME, &identity, &capability))
        return NT_STATUS_NOT_SUPPORTED;
    uint32_t remote_previous = UINT32_MAX;
    /* Even if the client loses its ACK after the kernel stop, never
     * fall back within this SAME call to a second cooperative suspend.
     * The parent must first resolve the ambiguous ptrace operation. */
    *attempted_remote_mutation = true;
    nt_status result =
        nt_broker_client_supervisor_suspend(capability, &remote_previous);
    if (result == NT_STATUS_SUCCESS) {
        bool committed = false;
        pthread_mutex_lock(&wait_lock);
        object = find_waitable(handle);
        if (object && object->kind == THREAD &&
            (object->access & THREAD_SUSPEND_RESUME) != 0) {
            struct waitable *state = wait_state(object);
            if (!state->signaled && !state->cooperative_pause_requested &&
                state->owner_pid == identity.guest_process_id &&
                state->thread_id == identity.guest_thread_id &&
                state->supervisor_async_suspended == previously_suspended &&
                state->suspend_count == local_count &&
                remote_previous == local_count) {
                state->suspend_count = local_count + 1;
                state->supervisor_async_suspended = true;
                committed = true;
            }
        }
        pthread_mutex_unlock(&wait_lock);
        if (!committed) {
            /* Roll back the parent count if local HANDLE state changed.
             * Never fall back to an unverified cooperative success. */
            uint32_t ignored = UINT32_MAX;
            (void)nt_broker_client_supervisor_resume(capability, &ignored);
            result = NT_STATUS_NOT_SUPPORTED;
        } else if (previous) {
            *previous = remote_previous;
        }
    }
    (void)nt_broker_client_supervisor_close(capability);
    return result;
}

/* Caller owns supervisor_gate_lock. Return handled=true only when a
 * ptrace suspension has actually been committed for this thread. */
/* Caller holds supervisor_gate_lock. No resume, including an intermediate
 * nested decrement, succeeds without an exact parent NT count ACK. */
static nt_status async_resume_guest_thread(uintptr_t handle, uint32_t *previous,
                                            bool alert, bool *handled)
{
    *handled = false;
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    struct waitable *state = entry && entry->kind == THREAD ?
                             wait_state(entry) : NULL;
    if (!state || !state->supervisor_async_suspended) {
        pthread_mutex_unlock(&wait_lock);
        return NT_STATUS_NOT_SUPPORTED;
    }
    *handled = true;
    nt_status status = NT_STATUS_SUCCESS;
    if ((entry->access & THREAD_SUSPEND_RESUME) == 0)
        status = NT_STATUS_ACCESS_DENIED;
    else if (state->signaled)
        status = NT_STATUS_THREAD_IS_TERMINATING;
    else if (state->suspend_count == 0 || alert)
        status = NT_STATUS_NOT_SUPPORTED;
    const uint32_t local_count = state->suspend_count;
    const uint32_t tid = state->thread_id;
    const uint32_t pid = state->owner_pid;
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS)
        return status;

    struct nt_broker_thread_info identity = {0};
    uint64_t capability = 0;
    if (!nt_runtime_supervisor_open_thread_handle(handle,
            THREAD_SUSPEND_RESUME, &identity, &capability))
        return NT_STATUS_NOT_SUPPORTED;
    uint32_t remote_previous = UINT32_MAX;
    status = nt_broker_client_supervisor_resume(capability, &remote_previous);
    (void)nt_broker_client_supervisor_close(capability);
    if (status != NT_STATUS_SUCCESS)
        return status;
    if (remote_previous != local_count ||
        identity.guest_thread_id != tid ||
        identity.guest_process_id != pid)
        return NT_STATUS_NOT_SUPPORTED;
    bool committed = false;
    pthread_mutex_lock(&wait_lock);
    entry = find_waitable(handle);
    if (entry && entry->kind == THREAD) {
        state = wait_state(entry);
        if (state->owner_pid == pid && state->thread_id == tid &&
            state->supervisor_async_suspended &&
            state->suspend_count == local_count) {
            state->suspend_count = local_count - 1;
            if (local_count == 1)
                state->supervisor_async_suspended = false;
            pthread_cond_broadcast(&wait_changed);
            committed = true;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (!committed)
        return NT_STATUS_NOT_SUPPORTED;
    if (previous)
        *previous = remote_previous;
    return NT_STATUS_SUCCESS;
}

static bool async_context_active(uintptr_t handle)
{
    bool active = false;
    pthread_mutex_lock(&wait_lock);
    const struct waitable *entry = find_waitable(handle);
    if (entry && entry->kind == THREAD)
        active = wait_state((struct waitable *)entry)->supervisor_async_suspended;
    pthread_mutex_unlock(&wait_lock);
    return active;
}
/* Suspension is deliberately restricted to the startup gate. It can
 * increase the count of a thread already created suspended, but it never
 * pauses guest instructions or calls pthread signals on a running thread.
 * The gate and its count are protected by wait_lock. */
static nt_status suspend_guest_thread(uintptr_t handle, uint32_t *previous)
{
    pthread_mutex_lock(&supervisor_gate_lock);
    bool remote_mutation_attempted = false;
    const nt_status asynchronous =
        async_suspend_guest_thread(handle, previous, &remote_mutation_attempted);
    if (asynchronous == NT_STATUS_SUCCESS) {
        pthread_mutex_unlock(&supervisor_gate_lock);
        return NT_STATUS_SUCCESS;
    }
    /* A previously ptrace-stopped target may NEVER be counted locally
     * after any failed, late or missing parent ACK. */
    if (remote_mutation_attempted || async_context_active(handle)) {
        pthread_mutex_unlock(&supervisor_gate_lock);
        return asynchronous;
    }
    /* Only threads never held by ptrace may use the legacy cooperative
     * fallback, where an alertable checkpoint can acknowledge the pause. */
    pthread_mutex_lock(&wait_lock);
    struct waitable *thread = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (thread == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (thread->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(thread->access & THREAD_SUSPEND_RESUME))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(thread);
        if (state->owner_pid != (uint32_t)getpid() && !state->child_guest)
            status = NT_STATUS_NOT_SUPPORTED;
        else if (state->signaled)
            status = NT_STATUS_THREAD_IS_TERMINATING;
        else if (state->suspend_count == MAXIMUM_SUSPEND_COUNT)
            status = NT_STATUS_SUSPEND_COUNT_EXCEEDED;
        else if (state->suspend_count != 0) {
            if (previous != NULL)
                *previous = state->suspend_count;
            ++state->suspend_count;
        } else if (state->child_guest ||
                   pthread_equal(state->host, pthread_self())) {
            /* No cross-sandbox broker and no self-suspension. */
            status = NT_STATUS_NOT_SUPPORTED;
        } else {
            /* A running thread may acknowledge a pause ONLY when it enters
             * SleepEx/WaitFor*Ex with alertable=TRUE. If it stays in
             * uninstrumented PE code or a non-alertable wait, rollback the
             * request and return NOT_SUPPORTED without claiming success. */
            struct timespec limit;
            if (clock_gettime(CLOCK_MONOTONIC, &limit) != 0) {
                status = NT_STATUS_UNSUCCESSFUL;
            } else {
                limit.tv_nsec += 500000000L;
                if (limit.tv_nsec >= 1000000000L) {
                    ++limit.tv_sec;
                    limit.tv_nsec -= 1000000000L;
                }
                state->cooperative_pause_requested = true;
                state->suspend_count = 1;
                pthread_cond_broadcast(&wait_changed);
                int error = 0;
                while (!state->cooperative_pause_acknowledged &&
                       !state->signaled && error == 0)
                    error = pthread_cond_clockwait(&wait_changed, &wait_lock,
                                                   CLOCK_MONOTONIC, &limit);
                if (state->cooperative_pause_acknowledged) {
                    if (previous != NULL)
                        *previous = 0;
                } else {
                    state->cooperative_pause_requested = false;
                    state->suspend_count = 0;
                    pthread_cond_broadcast(&wait_changed);
                    status = state->signaled ? NT_STATUS_THREAD_IS_TERMINATING :
                             NT_STATUS_NOT_SUPPORTED;
                }
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_mutex_unlock(&supervisor_gate_lock);
    return status;
}

uint32_t NT_MS_ABI nt_SuspendThread(uintptr_t handle)
{
    uint32_t previous = UINT32_MAX;
    const nt_status status = suspend_guest_thread(handle, &previous);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS ? previous : UINT32_MAX;
}

nt_status NT_MS_ABI nt_NtSuspendThread(uintptr_t handle, uint32_t *previous)
{
    return suspend_guest_thread(handle, previous);
}

static nt_status resume_guest_thread(uintptr_t handle, uint32_t *previous, bool alert)
{
    pthread_mutex_lock(&supervisor_gate_lock);
    bool handled = false;
    const nt_status asynchronous =
        async_resume_guest_thread(handle, previous, alert, &handled);
    if (handled) {
        pthread_mutex_unlock(&supervisor_gate_lock);
        return asynchronous;
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *thread = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (thread == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (thread->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (!(thread->access & THREAD_SUSPEND_RESUME))
        status = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(thread);
        if (state->owner_pid != (uint32_t)getpid() && !state->child_guest)
            status = NT_STATUS_NOT_SUPPORTED;
        else if (state->signaled)
            status = NT_STATUS_THREAD_IS_TERMINATING;
        else if (alert && state->child_guest)
            status = NT_STATUS_NOT_SUPPORTED; /* No cross-process alert broker. */
        else if (state->cooperative_pause_requested &&
                 !state->cooperative_pause_acknowledged)
            status = NT_STATUS_NOT_SUPPORTED; /* Suspension not confirmed yet. */
        else {
            if (previous != NULL)
                *previous = state->suspend_count;
            if (alert)
                state->alerted = true; /* Set before lifting the startup gate. */
            if (state->suspend_count != 0) {
                /* For a child created suspended, release the private fd5
                 * execution gate only on the final ResumeThread. Never
                 * reuse a PID or send SIGCONT to an unrelated process. */
                if (state->suspend_count == 1 && state->child_guest) {
                    const unsigned char token = 'G';
                    ssize_t written;
                    do {
                        written = write(state->start_gate_fd, &token, 1);
                    } while (written < 0 && errno == EINTR);
                    close(state->start_gate_fd);
                    state->start_gate_fd = -1;
                    if (written != 1)
                        status = NT_STATUS_THREAD_IS_TERMINATING;
                }
                if (status == NT_STATUS_SUCCESS) {
                    --state->suspend_count;
                    if (state->suspend_count == 0) {
                        state->cooperative_pause_requested = false;
                        state->cooperative_context_valid = false;
                    }
                    pthread_cond_broadcast(&wait_changed);
                }
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_mutex_unlock(&supervisor_gate_lock);
    return status;
}

uint32_t NT_MS_ABI nt_ResumeThread(uintptr_t handle)
{
    uint32_t previous = UINT32_MAX;
    const nt_status status = resume_guest_thread(handle, &previous, false);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS ? previous : UINT32_MAX;
}

nt_status NT_MS_ABI nt_NtResumeThread(uintptr_t handle, uint32_t *previous)
{
    return resume_guest_thread(handle, previous, false);
}

/* Serialize the alert flag and count decrement in one wait_lock critical
 * section, including the final release of a CREATE_SUSPENDED startup gate.
 * A child process has a different wait namespace and must be rejected. */
nt_status NT_MS_ABI nt_NtAlertResumeThread(uintptr_t handle, uint32_t *previous)
{
    return resume_guest_thread(handle, previous, true);
}

/* Read a real context captured by the target's own CPU at a negotiated
 * safe alertable boundary. Do NOT fabricate a guest RIP for startup-gated
 * threads: the guest entry hasn't run. Control register mutation and the
 * ability to stop arbitrary PE instructions still require a separate safe
 * context-restoration mechanism. Win32 and native NTSTATUS share rights
 * checks, but only Win32 modifies LastError. */
enum { CONTEXT_AMD64 = 0x100000, CONTEXT_CORE_GROUPS = 0x1f,
       CONTEXT_DEBUG_GROUP = 0x10 };

static nt_status get_guest_thread_context(uintptr_t handle, struct nt_context64 *output)
{
    if (output == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (((uintptr_t)output & 15u) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    const uint32_t requested = output->flags;
    if ((requested & CONTEXT_AMD64) == 0 ||
        (requested & CONTEXT_CORE_GROUPS) == 0 ||
        (requested & ~(CONTEXT_AMD64 | CONTEXT_CORE_GROUPS)) != 0)
        return NT_STATUS_INVALID_PARAMETER;

    pthread_mutex_lock(&supervisor_gate_lock);
    if (async_context_active(handle)) {
        struct nt_broker_thread_info identity = {0};
        uint64_t capability = 0;
        nt_status status = NT_STATUS_NOT_SUPPORTED;
        if (nt_runtime_supervisor_open_thread_handle(handle,
                THREAD_GET_CONTEXT, &identity, &capability)) {
            _Alignas(16) struct nt_context64 snapshot = {0};
            snapshot.flags = requested;
            status = nt_broker_client_supervisor_get_context(capability, &snapshot);
            (void)nt_broker_client_supervisor_close(capability);
            if (status == NT_STATUS_SUCCESS) *output = snapshot;
        }
        pthread_mutex_unlock(&supervisor_gate_lock);
        return status;
    }
    pthread_mutex_unlock(&supervisor_gate_lock);
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status result = NT_STATUS_SUCCESS;
    if (entry == NULL)
        result = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != THREAD)
        result = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if ((entry->access & THREAD_GET_CONTEXT) == 0)
        result = NT_STATUS_ACCESS_DENIED;
    else {
        struct waitable *state = wait_state(entry);
        if (state->owner_pid != (uint32_t)getpid() || state->child_guest)
            result = NT_STATUS_NOT_SUPPORTED;
        else if (state->signaled)
            result = NT_STATUS_THREAD_IS_TERMINATING;
        else if (!state->cooperative_pause_acknowledged ||
                 !state->cooperative_context_valid || state->suspend_count == 0)
            result = NT_STATUS_NOT_SUPPORTED;
        else if (requested & CONTEXT_DEBUG_GROUP)
            result = NT_STATUS_NOT_SUPPORTED; /* no kernel DRs in software capture */
        else {
            /* Groups requested by the caller are the only supported x64
             * groups (control, integer, segments, legacy floating point).
             * Debug registers and XSTATE require additional kernel work. */
            *output = state->cooperative_context;
            output->flags = requested;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return result;
}

nt_status NT_MS_ABI nt_NtGetContextThread(uintptr_t handle, struct nt_context64 *context)
{
    return get_guest_thread_context(handle, context);
}

int32_t NT_MS_ABI nt_GetThreadContext(uintptr_t handle, struct nt_context64 *context)
{
    const nt_status status = get_guest_thread_context(handle, context);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

/* A context change is deliberately unsupported until a verified scheme
 * can apply it to the target CPU on resume without skipping host cleanup
 * or compromising Linux signal/stack safety. Returning success without
 * a real register write would be a dangerous compatibility false positive. */
nt_status NT_MS_ABI nt_NtSetContextThread(uintptr_t handle, const struct nt_context64 *context)
{
    if (context == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (((uintptr_t)context & 15u) != 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&supervisor_gate_lock);
    if (async_context_active(handle)) {
        struct nt_broker_thread_info identity = {0};
        uint64_t capability = 0;
        nt_status status = NT_STATUS_NOT_SUPPORTED;
        if (nt_runtime_supervisor_open_thread_handle(handle,
                THREAD_SET_CONTEXT, &identity, &capability)) {
            status = nt_broker_client_supervisor_set_context(capability, context);
            (void)nt_broker_client_supervisor_close(capability);
        }
        pthread_mutex_unlock(&supervisor_gate_lock);
        return status;
    }
    pthread_mutex_unlock(&supervisor_gate_lock);
    pthread_mutex_lock(&wait_lock);
    struct waitable *entry = find_waitable(handle);
    nt_status result = NT_STATUS_NOT_SUPPORTED;
    if (entry == NULL)
        result = NT_STATUS_INVALID_HANDLE;
    else if (entry->kind != THREAD)
        result = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if ((entry->access & THREAD_SET_CONTEXT) == 0)
        result = NT_STATUS_ACCESS_DENIED;
    else if (wait_state(entry)->signaled)
        result = NT_STATUS_THREAD_IS_TERMINATING;
    pthread_mutex_unlock(&wait_lock);
    return result;
}

int32_t NT_MS_ABI nt_SetThreadContext(uintptr_t handle, const struct nt_context64 *context)
{
    const nt_status status = nt_NtSetContextThread(handle, context);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

/* Problem 6: a guest's primary Windows group is an explicit identity,
 * distinct from the thread's full Linux cpuset. Windows 11 can schedule
 * a thread across multiple processor groups, while the group-local
 * GROUP_AFFINITY/legacy APIs report and update its primary group. This
 * bounded backend maps a startup-allowed Linux CPU ordinal into a compact
 * guest slot. This is not a claim of exact Windows NUMA/CPU Sets topology. */
_Static_assert(sizeof(struct nt_group_affinity) == 16,
               "Windows AMD64 GROUP_AFFINITY layout");
_Static_assert(offsetof(struct nt_group_affinity, group) == 8,
               "GROUP_AFFINITY Group field offset");

static uintptr_t affinity_mask_in_group(const cpu_set_t *cpus, uint16_t group)
{
    if (group >= processor_group_count)
        return 0;
    uintptr_t mask = 0;
    for (int bit = 0; bit < 64; ++bit) {
        const int slot = (int)group * 64 + bit;
        if (slot < (int)processor_group_total &&
            CPU_ISSET(linux_cpu_by_guest_slot[slot], cpus))
            mask |= (uintptr_t)UINT64_C(1) << bit;
    }
    return mask;
}

static bool cpuset_advertised(const cpu_set_t *cpus)
{
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
        if (CPU_ISSET(cpu, cpus) && guest_slot_by_linux_cpu[cpu] < 0)
            return false;
    return true;
}

/* Convert a Windows group-local mask back to exact Linux CPU IDs. */
static void cpu_set_from_guest_mask(cpu_set_t *cpus, uint16_t group,
                                    uintptr_t mask)
{
    CPU_ZERO(cpus);
    for (int bit = 0; bit < 64; ++bit) {
        const int slot = (int)group * 64 + bit;
        if (slot < (int)processor_group_total &&
            (mask & ((uintptr_t)UINT64_C(1) << bit)))
            CPU_SET(linux_cpu_by_guest_slot[slot], cpus);
    }
}

/* Caller owns wait_lock, and the HANDLE was validated for the desired
 * rights. Out-of-band affinity changes may invalidate the primary group;
 * never silently invent a replacement group in that situation. */
static nt_status thread_primary_affinity_locked(uintptr_t handle,
                                                 const cpu_set_t *cpus,
                                                 struct nt_group_affinity *result)
{
    struct waitable *state = handle == nt_GetCurrentThread() ?
                             running_thread : wait_state(find_waitable(handle));
    uint16_t *group = state != NULL ? &state->primary_group : &primary_thread_group;
    bool *valid = state != NULL ? &state->primary_group_initialized :
                                  &primary_thread_group_initialized;
    if (!*valid) {
        uint16_t first = UINT16_MAX;
        for (int slot = 0; slot < (int)processor_group_total; ++slot)
            if (CPU_ISSET(linux_cpu_by_guest_slot[slot], cpus)) {
                first = (uint16_t)(slot / 64);
                break;
            }
        if (first == UINT16_MAX)
            return NT_STATUS_NOT_SUPPORTED;
        *group = first;
        *valid = true;
    }
    if (!cpuset_advertised(cpus))
        return NT_STATUS_NOT_SUPPORTED;
    const uintptr_t mask = affinity_mask_in_group(cpus, *group);
    if (mask == 0)
        return NT_STATUS_NOT_SUPPORTED;
    *result = (struct nt_group_affinity){.mask = mask, .group = *group};
    return NT_STATUS_SUCCESS;
}

static void update_thread_primary_group_locked(uintptr_t handle, uint16_t group)
{
    struct waitable *state = handle == nt_GetCurrentThread() ?
                             running_thread : wait_state(find_waitable(handle));
    if (state != NULL) {
        state->primary_group = group;
        state->primary_group_initialized = true;
        state->explicit_group_outside_process = group != process_primary_group;
    } else {
        primary_thread_group = group;
        primary_thread_group_initialized = true;
        if (gettid() == getpid())
            process_leader_group_outside = group != process_primary_group;
    }
}

static nt_status group_affinity_handle(uintptr_t handle, uint32_t access,
                                        pthread_t *host)
{
    /* Caller holds wait_lock. Handles never become arbitrary Linux PIDs. */
    if (handle == nt_GetCurrentThread()) {
        *host = pthread_self();
        return NT_STATUS_SUCCESS;
    }
    struct waitable *object = find_waitable(handle);
    if (object == NULL)
        return NT_STATUS_INVALID_HANDLE;
    if (object->kind != THREAD)
        return NT_STATUS_OBJECT_TYPE_MISMATCH;
    if (!(object->access & access))
        return NT_STATUS_ACCESS_DENIED;
    struct waitable *state = wait_state(object);
    if (state->owner_pid != (uint32_t)getpid() || state->child_guest)
        return NT_STATUS_NOT_SUPPORTED;
    if (state->signaled || state->terminate_at_start)
        return NT_STATUS_THREAD_IS_TERMINATING;
    *host = state->host;
    return NT_STATUS_SUCCESS;
}

int32_t NT_MS_ABI nt_GetThreadGroupAffinity(uintptr_t handle,
                                            struct nt_group_affinity *result)
{
    ensure_processor_groups();
    if (result == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    pthread_t host;
    nt_status status = group_affinity_handle(handle,
        THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, &host);
    struct nt_group_affinity observed = {0};
    if (status == NT_STATUS_SUCCESS) {
        cpu_set_t current;
        const int error = pthread_getaffinity_np(host, sizeof(current), &current);
        if (error != 0)
            status = error == ESRCH ? NT_STATUS_THREAD_IS_TERMINATING :
                     error == EINVAL ? NT_STATUS_NOT_SUPPORTED : NT_STATUS_UNSUCCESSFUL;
        else
            status = thread_primary_affinity_locked(handle, &current, &observed);
    }
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    *result = observed;
    return 1;
}

int32_t NT_MS_ABI nt_SetThreadGroupAffinity(uintptr_t handle,
                                            const struct nt_group_affinity *desired,
                                            struct nt_group_affinity *previous)
{
    ensure_processor_groups();
    if (desired == NULL || desired->mask == 0 ||
        desired->reserved[0] != 0 || desired->reserved[1] != 0 ||
        desired->reserved[2] != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((size_t)desired->group >= CPU_SETSIZE / 64) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    /* Do not issue a syscall when part of the requested group mask lies
     * outside the startup sandbox CPU set. Linux may otherwise silently
     * intersect it and force a best-effort rollback. */
    if (desired->group >= processor_group_count ||
        (desired->mask & ~processor_group_masks[desired->group]) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    cpu_set_t target;
    cpu_set_from_guest_mask(&target, desired->group, desired->mask);

    pthread_mutex_lock(&wait_lock);
    pthread_t host;
    nt_status status = group_affinity_handle(handle, THREAD_SET_INFORMATION, &host);
    struct nt_group_affinity old_group = {0};
    if (status == NT_STATUS_SUCCESS) {
        cpu_set_t before, after;
        int error = pthread_getaffinity_np(host, sizeof(before), &before);
        if (error != 0) {
            status = error == ESRCH ? NT_STATUS_THREAD_IS_TERMINATING :
                     error == EINVAL ? NT_STATUS_NOT_SUPPORTED : NT_STATUS_UNSUCCESSFUL;
        } else if (previous != NULL &&
                   (status = thread_primary_affinity_locked(handle, &before, &old_group)) !=
                       NT_STATUS_SUCCESS) {
            /* An externally modified cpuset may not overlap the stored
             * primary group. Never return an invented previous affinity. */
        } else {
            error = pthread_setaffinity_np(host, sizeof(target), &target);
            if (error == EPERM || error == EACCES)
                status = NT_STATUS_ACCESS_DENIED;
            else if (error == ESRCH)
                status = NT_STATUS_THREAD_IS_TERMINATING;
            else if (error == EINVAL)
                status = NT_STATUS_INVALID_PARAMETER;
            else if (error != 0)
                status = NT_STATUS_UNSUCCESSFUL;
            else if (pthread_getaffinity_np(host, sizeof(after), &after) != 0 ||
                     !CPU_EQUAL(&target, &after)) {
                /* Linux cpusets can silently intersect a requested mask.
                 * Never report success on partial application. */
                cpu_set_t restored;
                if (pthread_setaffinity_np(host, sizeof(before), &before) != 0 ||
                    pthread_getaffinity_np(host, sizeof(restored), &restored) != 0 ||
                    !CPU_EQUAL(&before, &restored))
                    status = NT_STATUS_UNSUCCESSFUL;
                else
                    status = NT_STATUS_INVALID_PARAMETER;
            }
        }
    }
    if (status == NT_STATUS_SUCCESS)
        update_thread_primary_group_locked(handle, desired->group);
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    if (previous != NULL)
        *previous = old_group;
    return 1;
}

/* SetThreadAffinityMask is relative to the explicit primary group.
 * With a multigroup host cpuset, it returns only the previous primary
 * group's mask, then confines the Linux thread to that group's bits. */
static nt_status set_guest_thread_affinity(uintptr_t handle, uintptr_t requested,
                                            uintptr_t *previous)
{
    ensure_processor_groups();
    if (requested == 0)
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = handle == nt_GetCurrentThread() ?
                              running_thread : find_waitable(handle);
    nt_status result = NT_STATUS_SUCCESS;
    if (handle != nt_GetCurrentThread() && object == NULL)
        result = NT_STATUS_INVALID_HANDLE;
    else if (object != NULL && object->kind != THREAD)
        result = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (object != NULL && handle != nt_GetCurrentThread() &&
             !(object->access & THREAD_SET_INFORMATION))
        result = NT_STATUS_ACCESS_DENIED;
    else if (object != NULL && wait_state(object)->owner_pid != (uint32_t)getpid())
        result = NT_STATUS_NOT_SUPPORTED;
    else if (object != NULL && wait_state(object)->signaled)
        result = NT_STATUS_THREAD_IS_TERMINATING;
    else {
        const pthread_t host = object != NULL ? wait_state(object)->host : pthread_self();
        cpu_set_t before, after, wanted;
        int err = pthread_getaffinity_np(host, sizeof(before), &before);
        if (err != 0)
            result = err == ESRCH ? NT_STATUS_THREAD_IS_TERMINATING : NT_STATUS_UNSUCCESSFUL;
        else {
            struct nt_group_affinity old_group = {0};
            result = thread_primary_affinity_locked(handle, &before, &old_group);
            if (result == NT_STATUS_SUCCESS) {
                const uint16_t group = old_group.group;
                /* Validate against the sandbox's startup CPU entitlement.
                 * This avoids silently accepting an intersected Linux mask,
                 * including on non-contiguous host CPU sets. */
                if (group >= processor_group_count ||
                    (requested & ~processor_group_masks[group]) != 0)
                    result = NT_STATUS_INVALID_PARAMETER;
                else {
                    cpu_set_from_guest_mask(&wanted, group, requested);
                    err = pthread_setaffinity_np(host, sizeof(wanted), &wanted);
                    if (err == EPERM || err == EACCES)
                        result = NT_STATUS_ACCESS_DENIED;
                    else if (err == ESRCH)
                        result = NT_STATUS_THREAD_IS_TERMINATING;
                    else if (err != 0)
                        result = NT_STATUS_INVALID_PARAMETER;
                    else if (pthread_getaffinity_np(host, sizeof(after), &after) != 0 ||
                             !CPU_EQUAL(&after, &wanted)) {
                        /* Host cpuset can change between snapshot and
                         * commit. Roll back rather than report a partial
                         * or different affinity as successful. */
                        cpu_set_t restored;
                        if (pthread_setaffinity_np(host, sizeof(before), &before) != 0 ||
                            pthread_getaffinity_np(host, sizeof(restored), &restored) != 0 ||
                            !CPU_EQUAL(&before, &restored))
                            result = NT_STATUS_UNSUCCESSFUL;
                        else
                            result = NT_STATUS_INVALID_PARAMETER;
                    } else if (previous != NULL)
                        *previous = old_group.mask;
                }
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return result;
}

uintptr_t NT_MS_ABI nt_SetThreadAffinityMask(uintptr_t handle, uintptr_t mask)
{
    uintptr_t previous = 0;
    const nt_status status = set_guest_thread_affinity(handle, mask, &previous);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS ? previous : 0;
}

/* ThreadAffinityMask (class 4): only the native x64 64-bit mask profile.
 * Native NTSTATUS calls do not alter Win32 LastError. */
nt_status NT_MS_ABI nt_NtSetInformationThread(uintptr_t thread, uint32_t info_class,
                                              const void *data, uint32_t length)
{
    if (info_class != 4u)
        return NT_STATUS_INVALID_INFO_CLASS;
    if (data == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length != sizeof(uintptr_t))
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    uintptr_t mask = 0;
    memcpy(&mask, data, sizeof(mask));
    return set_guest_thread_affinity(thread, mask, NULL);
}

/* GetProcessGroupAffinity reports the groups in which runtime-owned
 * guest threads (including the process leader) can actually execute.
 * This is not the global Linux CPU topology and does not grant access
 * to an arbitrary host PID. Canonical threads and their scheduling
 * identity are protected by wait_lock. */
int32_t NT_MS_ABI nt_GetProcessGroupAffinity(uintptr_t process,
                                              uint16_t *count,
                                              uint16_t *groups)
{
    if (count == NULL || groups == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const nt_status access = nt_runtime_check_self_process(process, 0x1400u);
    if (access != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(access));
        return 0;
    }
    ensure_processor_groups();
    bool present[CPU_SETSIZE / 64] = {0};
    pthread_mutex_lock(&wait_lock);
    bool valid = true;
    cpu_set_t mask;
    if (sched_getaffinity((pid_t)getpid(), sizeof(mask), &mask) != 0)
        valid = false;
    else {
        if (!cpuset_advertised(&mask))
            valid = false;
        for (int cpu = 0; cpu < CPU_SETSIZE && valid; ++cpu)
            if (CPU_ISSET(cpu, &mask))
                present[guest_slot_by_linux_cpu[cpu] / 64] = true;
    }
    for (size_t i = 0; i < WAITABLE_COUNT && valid; ++i) {
        const struct waitable *item = &waitables[i];
        if (item->kind != THREAD || item->shared != NULL ||
            (item->refs == 0 && item->links == 0) ||
            item->owner_pid != (uint32_t)getpid() || item->signaled ||
            !item->started)
            continue;
        if (sched_getaffinity((pid_t)item->thread_id, sizeof(mask), &mask) != 0) {
            valid = false;
            break;
        }
        if (!cpuset_advertised(&mask)) {
            valid = false;
            break;
        }
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &mask))
                present[guest_slot_by_linux_cpu[cpu] / 64] = true;
    }
    pthread_mutex_unlock(&wait_lock);
    if (!valid) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    uint16_t needed = 0;
    for (size_t i = 0; i < CPU_SETSIZE / 64; ++i)
        if (present[i])
            ++needed;
    if (needed == 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (*count < needed) {
        *count = needed;
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    uint16_t written = 0;
    for (uint16_t i = 0; i < CPU_SETSIZE / 64; ++i)
        if (present[i])
            groups[written++] = i;
    *count = needed;
    return 1;
}

int32_t NT_MS_ABI nt_GetProcessAffinityMask(uintptr_t process, uintptr_t *process_mask,
                                             uintptr_t *system_mask)
{
    if (process_mask == NULL || system_mask == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const nt_status access = nt_runtime_check_self_process(process, 0x1400u);
    if (access != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(access));
        return 0;
    }
    ensure_processor_groups();
    pthread_mutex_lock(&wait_lock);
    cpu_set_t current;
    struct nt_group_affinity calling_primary = {0};
    nt_status result = NT_STATUS_SUCCESS;
    if (pthread_getaffinity_np(pthread_self(), sizeof(current), &current) != 0)
        result = NT_STATUS_NOT_SUPPORTED;
    else
        result = thread_primary_affinity_locked(nt_GetCurrentThread(),
                                                &current, &calling_primary);
    const uint16_t group = calling_primary.group;
    uintptr_t policy = 0, system = 0;
    if (result == NT_STATUS_SUCCESS) {
        system = processor_group_masks[group];
        if (!process_primary_affinity_initialized) {
            process_primary_affinity = processor_group_masks[process_primary_group];
            process_primary_affinity_initialized = true;
        }
        if (group == process_primary_group)
            policy = process_primary_affinity;
        else if (!process_affinity_explicit)
            policy = processor_group_masks[group]; /* default Windows 11 multi-group */
        else
            result = NT_STATUS_NOT_SUPPORTED; /* no invented cross-group process policy */
        if (policy == 0 || (policy & ~system) != 0)
            result = NT_STATUS_NOT_SUPPORTED;
    }
    pthread_mutex_unlock(&wait_lock);
    if (result != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(result));
        return 0;
    }
    /* For the current process Windows 11 uses the caller's primary group.
     * Never truncate the full host cpuset or confuse thread pinning with
     * process policy. This is still a snapshot of startup entitlement. */
    *process_mask = policy;
    *system_mask = system;
    return 1;
}

/* Windows process priority classes are represented only when Linux
 * confirms an exact scheduler transition. Linux nice belongs to individual
 * TIDs, not to a Windows process object. Changing a class while multiple
 * threads execute cannot be made atomic without a cooperative scheduler;
 * fail before modifying any thread in that case.
 *
 * These are deliberately discrete, *static* classes. Dynamic priority
 * boosts, MMCSS and REALTIME_PRIORITY_CLASS cannot be represented by nice
 * and must not report success. */
enum { NT_PRIORITY_IDLE_CLASS = 0x40u, NT_PRIORITY_NORMAL_CLASS = 0x20u,
       NT_PRIORITY_HIGH_CLASS = 0x80u, NT_PRIORITY_REALTIME_CLASS = 0x100u,
       NT_PRIORITY_BELOW_NORMAL_CLASS = 0x4000u,
       NT_PRIORITY_ABOVE_NORMAL_CLASS = 0x8000u };

static bool host_nice_priority(int nice, int32_t *guest);

static bool priority_class_map(uint32_t klass, int *nice_base, int32_t *nt_base)
{
    switch (klass) {
    case NT_PRIORITY_IDLE_CLASS: *nice_base = 10; *nt_base = 4; return true;
    case NT_PRIORITY_BELOW_NORMAL_CLASS: *nice_base = 4; *nt_base = 6; return true;
    case NT_PRIORITY_NORMAL_CLASS: *nice_base = 0; *nt_base = 8; return true;
    case NT_PRIORITY_ABOVE_NORMAL_CLASS: *nice_base = -4; *nt_base = 10; return true;
    case NT_PRIORITY_HIGH_CLASS: *nice_base = -10; *nt_base = 13; return true;
    default: return false;
    }
}

/* A bounded snapshot of the native Linux task group. A Windows process
 * priority/affinity operation must never change a pthread belonging to
 * an untracked Linux library or an unrelated process. Every non-leader
 * TID must correspond to a live, canonical guest thread while wait_lock
 * is held; the leader is getpid(). No numbers come from guest HANDLEs.
 * Runtime-created thread start and completion are serialized by wait_lock.
 * Other host libraries can create pthreads independently; an independent
 * task causes the transaction to fail closed. */
struct scheduler_thread_set {
    pid_t ids[WAITABLE_COUNT + 1];
    size_t count;
};

static bool tracked_scheduler_tid_locked(pid_t tid)
{
    if (tid == getpid() || (boost_monitor_state == 1 &&
                             tid == boost_monitor_tid && tid > 0))
        return true;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        const struct waitable *item = &waitables[i];
        if (item->kind == THREAD && item->shared == NULL &&
            (item->refs != 0 || item->links != 0) && item->started &&
            !item->signaled && item->owner_pid == (uint32_t)getpid() &&
            item->thread_id == (uint32_t)tid)
            return true;
    }
    return false;
}

static int compare_scheduler_tid(const void *a, const void *b)
{
    const pid_t left = *(const pid_t *)a, right = *(const pid_t *)b;
    return (left > right) - (left < right);
}

static bool snapshot_scheduler_threads_locked(struct scheduler_thread_set *out)
{
    DIR *directory = opendir("/proc/self/task");
    if (directory == NULL)
        return false;
    out->count = 0;
    bool accepted = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;
        char *end = NULL;
        errno = 0;
        const long raw = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' ||
            raw <= 0 || raw > INT32_MAX ||
            out->count == WAITABLE_COUNT + 1 ||
            !tracked_scheduler_tid_locked((pid_t)raw)) {
            accepted = false;
            break;
        }
        out->ids[out->count++] = (pid_t)raw;
    }
    const int close_error = closedir(directory);
    if (!accepted || close_error != 0 || out->count == 0)
        return false;
    qsort(out->ids, out->count, sizeof(out->ids[0]), compare_scheduler_tid);
    for (size_t i = 1; i < out->count; ++i)
        if (out->ids[i] <= out->ids[i - 1])
            return false;
    return true;
}

static bool scheduler_threads_unchanged_locked(const struct scheduler_thread_set *before)
{
    struct scheduler_thread_set after;
    if (!snapshot_scheduler_threads_locked(&after) || after.count != before->count)
        return false;
    return memcmp(after.ids, before->ids,
                  before->count * sizeof(before->ids[0])) == 0;
}

/* Conservative Linux rollback proof. Without CAP_SYS_NICE or an
 * adequate RLIMIT_NICE, nice can be *increased* (lower priority) but
 * cannot later be restored to a more favorable number. Do not begin
 * a multi-TID downgrade without enough rights to roll it back.
 * Effective capability is obtained from our own /proc/self/status,
 * never from the guest. */
static bool scheduler_can_restore_nice(int old_nice)
{
    bool has_cap = false;
    FILE *file = fopen("/proc/self/status", "r");
    if (file != NULL) {
        char line[256];
        while (fgets(line, sizeof(line), file) != NULL) {
            unsigned long long caps = 0;
            if (sscanf(line, "CapEff: %llx", &caps) == 1) {
                has_cap = (caps & (UINT64_C(1) << 23)) != 0;
                break;
            }
        }
        fclose(file);
    }
    if (has_cap)
        return true;
    struct rlimit limit;
    if (getrlimit(RLIMIT_NICE, &limit) != 0)
        return false;
    if (limit.rlim_cur == RLIM_INFINITY || limit.rlim_cur > 40)
        return true;
    return limit.rlim_cur >= 1 &&
           old_nice >= 20 - (int)limit.rlim_cur;
}

/* Check the whole host process before a per-TID class transition.
 * Readdir uses the kernel's own /proc/self/task namespace; this test
 * never interprets guest HANDLEs as Linux TIDs. */
static bool priority_class_is_exclusive(void)
{
    DIR *dir = opendir("/proc/self/task");
    if (dir == NULL)
        return false;
    unsigned int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9' && ++count > 1)
            break;
    }
    const int close_error = closedir(dir);
    return count == 1 && close_error == 0;
}

/* A Windows process affinity policy is distinct from Linux per-TID
 * masks. A transaction can update all runtime-owned threads while
 * wait_lock excludes their creation/completion and competing runtime
 * affinity changes. Unknown host threads cause a fail-closed result.
 * Verification and rollback are mandatory: do not silently pin just
 * the caller or ignore cpuset intersection on another TID. */
static uint32_t set_process_affinity_locked(uintptr_t requested)
{
    if (processor_group_masks[process_primary_group] == 0 ||
        (requested & ~processor_group_masks[process_primary_group]) != 0)
        return NT_ERROR_INVALID_PARAMETER;
    /* Windows 11 allows narrowing the default multi-group policy as long
     * as no living guest thread was explicitly moved to another primary
     * group. This check uses owned objects, never host-supplied PIDs. */
    if (process_leader_group_outside)
        return NT_ERROR_INVALID_PARAMETER;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        const struct waitable *item = &waitables[i];
        if (item->kind == THREAD && item->shared == NULL &&
            (item->refs != 0 || item->links != 0) &&
            item->owner_pid == (uint32_t)getpid() && !item->signaled &&
            item->explicit_group_outside_process)
            return NT_ERROR_INVALID_PARAMETER;
    }

    struct scheduler_thread_set threads;
    if (!snapshot_scheduler_threads_locked(&threads))
        return NT_ERROR_NOT_SUPPORTED;

    struct scheduler_affinity_entry {
        pid_t tid;
        cpu_set_t before;
        bool changed;
    };
    struct scheduler_affinity_entry *items = calloc(threads.count, sizeof(*items));
    if (items == NULL)
        return NT_ERROR_NOT_ENOUGH_MEMORY;

    cpu_set_t wanted;
    cpu_set_from_guest_mask(&wanted, process_primary_group, requested);

    uint32_t error = NT_ERROR_SUCCESS;
    for (size_t i = 0; i < threads.count; ++i) {
        items[i].tid = threads.ids[i];
        if (sched_getaffinity(threads.ids[i], sizeof(items[i].before),
                              &items[i].before) != 0) {
            error = NT_ERROR_NOT_SUPPORTED;
            break;
        }
        if (!cpuset_advertised(&items[i].before))
            error = NT_ERROR_NOT_SUPPORTED; /* changed host entitlement */
        if (error != NT_ERROR_SUCCESS)
            break;
    }
    if (error == NT_ERROR_SUCCESS && !scheduler_threads_unchanged_locked(&threads))
        error = NT_ERROR_NOT_SUPPORTED;

    for (size_t i = 0; error == NT_ERROR_SUCCESS && i < threads.count; ++i) {
        if (CPU_EQUAL(&wanted, &items[i].before))
            continue;
        if (sched_setaffinity(items[i].tid, sizeof(wanted), &wanted) != 0) {
            const int failure = errno;
            error = failure == EPERM || failure == EACCES ? NT_ERROR_ACCESS_DENIED :
                    failure == EINVAL ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_GEN_FAILURE;
            break;
        }
        items[i].changed = true;
        cpu_set_t verified;
        if (sched_getaffinity(items[i].tid, sizeof(verified), &verified) != 0 ||
            !CPU_EQUAL(&wanted, &verified)) {
            error = NT_ERROR_GEN_FAILURE;
            break;
        }
    }
    if (error == NT_ERROR_SUCCESS &&
        !scheduler_threads_unchanged_locked(&threads))
        error = NT_ERROR_NOT_SUPPORTED;

    if (error != NT_ERROR_SUCCESS) {
        for (size_t i = threads.count; i > 0; --i) {
            struct scheduler_affinity_entry *item = &items[i - 1];
            if (!item->changed)
                continue;
            cpu_set_t verified;
            if (sched_setaffinity(item->tid, sizeof(item->before), &item->before) != 0 ||
                sched_getaffinity(item->tid, sizeof(verified), &verified) != 0 ||
                !CPU_EQUAL(&verified, &item->before))
                error = NT_ERROR_GEN_FAILURE;
        }
    } else {
        process_primary_affinity = requested;
        process_primary_affinity_initialized = true;
        process_affinity_explicit = true;
    }
    free(items);
    return error;
}

int32_t NT_MS_ABI nt_SetProcessAffinityMask(uintptr_t process, uintptr_t requested)
{
    const nt_status access = nt_runtime_check_self_process(process, 0x200u);
    if (access != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(access));
        return 0;
    }
    if (requested == 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    ensure_processor_groups();
    pthread_mutex_lock(&wait_lock);
    const uint32_t error = set_process_affinity_locked(requested);
    pthread_mutex_unlock(&wait_lock);
    if (error != NT_ERROR_SUCCESS) {
        nt_SetLastError(error);
        return 0;
    }
    return 1;
}

int32_t nt_runtime_current_process_base_priority(void)
{
    pthread_mutex_lock(&wait_lock);
    const int32_t result = process_priority_nt_base;
    pthread_mutex_unlock(&wait_lock);
    return result;
}

uint32_t NT_MS_ABI nt_GetPriorityClass(uintptr_t process)
{
    const nt_status access = nt_runtime_check_self_process(process, 0x1400u);
    if (access != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(access));
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    const uint32_t klass = process_priority_class;
    const int base = process_priority_nice_base;
    errno = 0;
    const int actual = getpriority(PRIO_PROCESS, (id_t)gettid());
    const int error = errno;
    pthread_mutex_unlock(&wait_lock);
    int32_t relative = 0;
    if (error != 0 || !host_nice_priority(actual - base, &relative)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return klass;
}

struct scheduler_priority_entry {
    pid_t tid;
    int before, after;
    bool changed;
};

/* This transaction is permitted only when all native TIDs belong to
 * tracked guest threads and the host can *both* apply and undo every
 * potential change. No Linux privilege is guessed from a Win32 handle.
 * The runtime's wait_lock gates its pthread creation/completion, but a
 * foreign library may start threads without this lock; such a change
 * is detected by the post-commit snapshot, rejected and rolled back.
 * Dynamic scheduling and an instantaneous CPU-level atomic switch are
 * not claimed. */
static uint32_t set_priority_class_many_locked(int new_base, int32_t new_nt_base,
                                                uint32_t klass)
{
    struct scheduler_thread_set threads;
    if (!snapshot_scheduler_threads_locked(&threads) || threads.count < 2)
        return NT_ERROR_NOT_SUPPORTED;
    struct scheduler_priority_entry items[WAITABLE_COUNT + 1] = {0};
    const int old_base = process_priority_nice_base;
    for (size_t i = 0; i < threads.count; ++i) {
        items[i].tid = threads.ids[i];
        errno = 0;
        const int nice = getpriority(PRIO_PROCESS, (id_t)items[i].tid);
        if (errno != 0)
            return NT_ERROR_NOT_SUPPORTED;
        int32_t relative = 0;
        if (!host_nice_priority(nice - old_base, &relative))
            return NT_ERROR_NOT_SUPPORTED;
        items[i].before = nice;
        items[i].after = nice - old_base + new_base;
        if (items[i].after < -20 || items[i].after > 19)
            return NT_ERROR_NOT_SUPPORTED;
        if (items[i].after < nice &&
            !scheduler_can_restore_nice(items[i].after))
            return NT_ERROR_NOT_SUPPORTED;
        if (items[i].after > nice &&
            !scheduler_can_restore_nice(nice))
            return NT_ERROR_NOT_SUPPORTED;
    }
    if (!scheduler_threads_unchanged_locked(&threads))
        return NT_ERROR_NOT_SUPPORTED;

    uint32_t error = NT_ERROR_SUCCESS;
    for (size_t i = 0; i < threads.count; ++i) {
        if (items[i].after == items[i].before)
            continue;
        if (setpriority(PRIO_PROCESS, (id_t)items[i].tid, items[i].after) != 0) {
            const int failure = errno;
            error = failure == EPERM || failure == EACCES ?
                    NT_ERROR_ACCESS_DENIED : NT_ERROR_GEN_FAILURE;
            break;
        }
        items[i].changed = true;
        errno = 0;
        const int observed = getpriority(PRIO_PROCESS, (id_t)items[i].tid);
        if (errno != 0 || observed != items[i].after) {
            error = NT_ERROR_GEN_FAILURE;
            break;
        }
    }
    if (error == NT_ERROR_SUCCESS &&
        !scheduler_threads_unchanged_locked(&threads))
        error = NT_ERROR_NOT_SUPPORTED;

    if (error != NT_ERROR_SUCCESS) {
        /* An unprivileged process would be unable to undo a downgraded
         * Linux nice value. Such transactions were rejected up-front. */
        for (size_t i = threads.count; i > 0; --i) {
            struct scheduler_priority_entry *item = &items[i - 1];
            if (!item->changed)
                continue;
            if (setpriority(PRIO_PROCESS, (id_t)item->tid, item->before) != 0)
                error = NT_ERROR_GEN_FAILURE;
            else {
                errno = 0;
                if (getpriority(PRIO_PROCESS, (id_t)item->tid) != item->before ||
                    errno != 0)
                    error = NT_ERROR_GEN_FAILURE;
            }
        }
        return error;
    }
    process_priority_class = klass;
    process_priority_nice_base = new_base;
    process_priority_nt_base = new_nt_base;
    return NT_ERROR_SUCCESS;
}

int32_t NT_MS_ABI nt_SetPriorityClass(uintptr_t process, uint32_t klass)
{
    const nt_status access = nt_runtime_check_self_process(process, 0x200u);
    if (access != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(access));
        return 0;
    }
    int new_base = 0;
    int32_t new_nt_base = 0;
    if (klass == NT_PRIORITY_REALTIME_CLASS || !priority_class_map(klass, &new_base, &new_nt_base)) {
        nt_SetLastError(klass == NT_PRIORITY_REALTIME_CLASS ? NT_ERROR_NOT_SUPPORTED :
                        NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    if (scheduler_has_active_boosts_locked()) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0; /* do not overwrite a base while decay still owns it */
    }
    const int old_base = process_priority_nice_base;
    if (klass == process_priority_class) {
        pthread_mutex_unlock(&wait_lock);
        return 1; /* No mutation, not a simulated scheduling change. */
    }
    if (!priority_class_is_exclusive()) {
        const uint32_t result = set_priority_class_many_locked(new_base, new_nt_base, klass);
        pthread_mutex_unlock(&wait_lock);
        if (result != NT_ERROR_SUCCESS)
            nt_SetLastError(result);
        return result == NT_ERROR_SUCCESS;
    }
    errno = 0;
    const int old = getpriority(PRIO_PROCESS, (id_t)gettid());
    const int get_error = errno;
    int32_t old_relative = 0;
    if (get_error != 0 || !host_nice_priority(old - old_base, &old_relative)) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const int requested = new_base + (old - old_base);
    if (requested < -20 || requested > 19) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (setpriority(PRIO_PROCESS, (id_t)gettid(), requested) != 0) {
        const int error = errno;
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(error == EPERM || error == EACCES ? NT_ERROR_ACCESS_DENIED :
                        error == EINVAL ? NT_ERROR_INVALID_PARAMETER :
                        NT_ERROR_GEN_FAILURE);
        return 0;
    }
    errno = 0;
    const int observed = getpriority(PRIO_PROCESS, (id_t)gettid());
    if (errno != 0 || observed != requested) {
        /* Refuse unverified success. A best-effort rollback can itself
         * fail when Linux denies raising a degraded nice value. */
        (void)setpriority(PRIO_PROCESS, (id_t)gettid(), old);
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    process_priority_class = klass;
    process_priority_nice_base = new_base;
    process_priority_nt_base = new_nt_base;
    pthread_mutex_unlock(&wait_lock);
    return 1;
}

/* Bounded native-backed Windows priority classes. Linux nice is per-TID
 * on current Fedora/Ubuntu kernels; a change to another thread requires
 * proper THREAD_SET_INFORMATION and host scheduler permission.
 *
 * Windows REALTIME_PRIORITY_CLASS and dynamic boosts have no equivalent
 * here: do not return fabricated success for those contracts. */
static bool guest_priority_nice(int32_t guest, int *nice)
{
    switch (guest) {
    case -15: *nice = 10; return true; /* IDLE */
    case -2:  *nice = 4; return true;  /* LOWEST */
    case -1:  *nice = 2; return true;  /* BELOW_NORMAL */
    case 0:   *nice = 0; return true;  /* NORMAL */
    case 1:   *nice = -2; return true; /* ABOVE_NORMAL */
    case 2:   *nice = -4; return true; /* HIGHEST */
    case 15:  *nice = -10; return true;/* TIME_CRITICAL */
    default: return false;
    }
}

static bool host_nice_priority(int nice, int32_t *guest)
{
    switch (nice) {
    case 10: *guest = -15; return true;
    case 4:  *guest = -2; return true;
    case 2:  *guest = -1; return true;
    case 0:  *guest = 0; return true;
    case -2: *guest = 1; return true;
    case -4: *guest = 2; return true;
    case -10: *guest = 15; return true;
    default: return false;
    }
}

/* Read process-local TIDs from the canonical thread object; a numerical
 * Windows HANDLE never becomes a Linux process identifier. */
/* Caller owns wait_lock throughout scheduler syscalls: the target
 * cannot finish/release its canonical object and let Linux recycle
 * its TID between access checking and setpriority/getpriority. */
static nt_status priority_thread_tid_locked(uintptr_t handle, uint32_t rights, pid_t *tid)
{
    if (handle == nt_GetCurrentThread()) {
        *tid = gettid();
        return NT_STATUS_SUCCESS;
    }
    struct waitable *thread = find_waitable(handle);
    if (thread == NULL)
        return NT_STATUS_INVALID_HANDLE;
    if (thread->kind != THREAD)
        return NT_STATUS_OBJECT_TYPE_MISMATCH;
    if (!(thread->access & rights))
        return NT_STATUS_ACCESS_DENIED;
    struct waitable *state = wait_state(thread);
    if (state->owner_pid != (uint32_t)getpid())
        return NT_STATUS_NOT_SUPPORTED;
    if (state->signaled || state->terminate_at_start)
        return NT_STATUS_THREAD_IS_TERMINATING;
    *tid = (pid_t)state->thread_id;
    return NT_STATUS_SUCCESS;
}

/* Bounded, opt-in dynamic boost. A private Linux pthread restores an owned
 * guest TID after a monotonic deadline. No asynchronous signals, ptrace,
 * realtime scheduling, raw guest PIDs or access to foreign Linux threads.
 * Every mutation is verified with getpriority; the monitor only uses guest
 * objects protected by wait_lock so TID reuse cannot race termination.
 *
 * One nice step for up to 20 ms is a measurable Linux approximation,
 * NOT an asserted equivalent of NT quantum or event-dependent boost curves.
 * This path is enabled only with NT_NATIVE_EXPERIMENTAL_BOOST=1 and enough
 * Linux privilege to both adjust and restore scheduling. */
enum { EXPERIMENTAL_BOOST_NS = 20000000L };
static bool boost_time_reached(const struct timespec *now,
                               const struct timespec *until)
{
    return now->tv_sec > until->tv_sec ||
           (now->tv_sec == until->tv_sec &&
            now->tv_nsec >= until->tv_nsec);
}

static bool restore_dynamic_boost_locked(pid_t tid,
                                         struct guest_dynamic_boost *boost)
{
    if (!boost->active)
        return true;
    errno = 0;
    const int observed = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno != 0 || observed != boost->base_nice - 1) {
        if (errno == 0 && observed == boost->base_nice) {
            boost->active = false;
            return true;
        }
        boost_rollback_failed = true;
        return false;
    }
    if (setpriority(PRIO_PROCESS, (id_t)tid, boost->base_nice) != 0) {
        boost_rollback_failed = true;
        return false;
    }
    errno = 0;
    if (getpriority(PRIO_PROCESS, (id_t)tid) != boost->base_nice ||
        errno != 0) {
        boost_rollback_failed = true;
        return false;
    }
    boost->active = false;
    return true;
}

static void *dynamic_boost_decay_worker(void *ignored)
{
    (void)ignored;
    pthread_mutex_lock(&wait_lock);
    boost_monitor_tid = gettid();
    pthread_cond_broadcast(&wait_changed);
    for (;;) {
        struct timespec now, earliest = {0};
        const bool clock_ok = clock_gettime(CLOCK_MONOTONIC, &now) == 0;
        bool pending = false;
        struct guest_dynamic_boost *leader = &leader_dynamic_boost;
        if (leader->active) {
            if (clock_ok && boost_time_reached(&now, &leader->expires))
                (void)restore_dynamic_boost_locked(getpid(), leader);
            if (leader->active) {
                if (clock_ok && boost_time_reached(&now, &leader->expires))
                    leader->expires = (struct timespec){.tv_sec = now.tv_sec,
                        .tv_nsec = now.tv_nsec + EXPERIMENTAL_BOOST_NS};
                if (leader->expires.tv_nsec >= 1000000000L) {
                    ++leader->expires.tv_sec;
                    leader->expires.tv_nsec -= 1000000000L;
                }
                earliest = leader->expires;
                pending = true;
            }
        }
        for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
            struct waitable *item = &waitables[i];
            if (item->kind != THREAD || item->shared != NULL ||
                item->owner_pid != (uint32_t)getpid() ||
                !item->started || item->signaled || !item->dynamic_boost.active)
                continue;
            struct guest_dynamic_boost *boost = &item->dynamic_boost;
            if (clock_ok && boost_time_reached(&now, &boost->expires))
                (void)restore_dynamic_boost_locked((pid_t)item->thread_id, boost);
            if (!boost->active)
                continue;
            if (clock_ok && boost_time_reached(&now, &boost->expires)) {
                boost->expires = now;
                boost->expires.tv_nsec += EXPERIMENTAL_BOOST_NS;
                if (boost->expires.tv_nsec >= 1000000000L) {
                    ++boost->expires.tv_sec;
                    boost->expires.tv_nsec -= 1000000000L;
                }
            }
            if (!pending || boost_time_reached(&earliest, &boost->expires))
                earliest = boost->expires;
            pending = true;
        }
        if (!pending || !clock_ok)
            (void)pthread_cond_wait(&wait_changed, &wait_lock);
        else
            (void)pthread_cond_clockwait(&wait_changed, &wait_lock,
                                        CLOCK_MONOTONIC, &earliest);
    }
    /* No detached worker shutdown is required: process teardown ends it. */
    return NULL;
}

static bool ensure_dynamic_boost_monitor_locked(void)
{
    if (boost_monitor_state != 0)
        return boost_monitor_state == 1 && !boost_rollback_failed;
    const char *opt_in = getenv("NT_NATIVE_EXPERIMENTAL_BOOST");
    if (opt_in == NULL || strcmp(opt_in, "1") != 0)
        return false;
    errno = 0;
    const int nice = getpriority(PRIO_PROCESS, (id_t)gettid());
    if (errno != 0 || nice <= -20 || !scheduler_can_restore_nice(nice - 1))
        return false;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
        return false;
    const int detach_error = pthread_attr_setdetachstate(&attr,
                                                         PTHREAD_CREATE_DETACHED);
    pthread_t monitor;
    const int error = detach_error == 0 ?
        pthread_create(&monitor, &attr, dynamic_boost_decay_worker, NULL) :
        detach_error;
    pthread_attr_destroy(&attr);
    if (error != 0) {
        boost_monitor_state = -1;
        return false;
    }
    boost_monitor_state = 1;
    return true;
}

static struct guest_dynamic_boost *dynamic_boost_for_thread_locked(
    uintptr_t handle)
{
    if (handle == nt_GetCurrentThread()) {
        if (running_thread != NULL)
            return &running_thread->dynamic_boost;
        return gettid() == getpid() ? &leader_dynamic_boost : NULL;
    }
    struct waitable *entry = find_waitable(handle);
    return entry != NULL && entry->kind == THREAD ?
           &wait_state(entry)->dynamic_boost : NULL;
}

static bool scheduler_has_active_boosts_locked(void)
{
    if (leader_dynamic_boost.active)
        return true;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i)
        if (waitables[i].kind == THREAD && waitables[i].shared == NULL &&
            waitables[i].dynamic_boost.active)
            return true;
    return false;
}

/* Invoked only after a guest NT object wait actually blocked and then
 * completed successfully; no boost for timeout, polling, or failed waits. */
static void scheduler_guest_wait_woke_locked(void)
{
    if (!ensure_dynamic_boost_monitor_locked() ||
        process_dynamic_boost_disabled || boost_rollback_failed)
        return;
    struct guest_dynamic_boost *boost =
        dynamic_boost_for_thread_locked(nt_GetCurrentThread());
    if (boost == NULL || boost->disabled || boost->active)
        return;
    const pid_t tid = gettid();
    errno = 0;
    const int base = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno != 0 || base <= -20 || !scheduler_can_restore_nice(base - 1))
        return;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return;
    if (setpriority(PRIO_PROCESS, (id_t)tid, base - 1) != 0)
        return;
    errno = 0;
    if (getpriority(PRIO_PROCESS, (id_t)tid) != base - 1 || errno != 0) {
        (void)setpriority(PRIO_PROCESS, (id_t)tid, base);
        boost_rollback_failed = true;
        return;
    }
    boost->base_nice = base;
    boost->expires = now;
    boost->expires.tv_nsec += EXPERIMENTAL_BOOST_NS;
    if (boost->expires.tv_nsec >= 1000000000L) {
        ++boost->expires.tv_sec;
        boost->expires.tv_nsec -= 1000000000L;
    }
    boost->active = true;
    pthread_cond_broadcast(&wait_changed);
}

/* Boost control APIs report success ONLY for a live, privileged and
 * explicitly opted-in dynamic scheduler backend. Normal unprivileged
 * launches continue to fail closed; never fake Windows policy state. */
int32_t NT_MS_ABI nt_GetThreadPriorityBoost(uintptr_t handle, int32_t *disabled)
{
    if (disabled == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    pid_t tid = 0;
    const nt_status status = priority_thread_tid_locked(handle,
        THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, &tid);
    struct guest_dynamic_boost *boost = status == NT_STATUS_SUCCESS ?
        dynamic_boost_for_thread_locked(handle) : NULL;
    const bool available = boost != NULL &&
        ensure_dynamic_boost_monitor_locked();
    if (available)
        *disabled = boost->disabled ? 1 : 0;
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS || !available) {
        nt_SetLastError(status == NT_STATUS_SUCCESS ? NT_ERROR_NOT_SUPPORTED :
                        nt_runtime_nt_error(status));
        return 0;
    }
    return 1;
}

int32_t NT_MS_ABI nt_SetThreadPriorityBoost(uintptr_t handle, int32_t disabled)
{
    pthread_mutex_lock(&wait_lock);
    pid_t tid = 0;
    const nt_status status = priority_thread_tid_locked(handle,
        THREAD_SET_INFORMATION, &tid);
    struct guest_dynamic_boost *boost = status == NT_STATUS_SUCCESS ?
        dynamic_boost_for_thread_locked(handle) : NULL;
    const bool available = boost != NULL &&
        ensure_dynamic_boost_monitor_locked();
    bool restored = true;
    if (available && disabled && boost->active)
        restored = restore_dynamic_boost_locked(tid, boost);
    if (available && restored)
        boost->disabled = disabled != 0;
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS || !available || !restored) {
        nt_SetLastError(status != NT_STATUS_SUCCESS ? nt_runtime_nt_error(status) :
                        !available ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return 1;
}

int32_t NT_MS_ABI nt_GetProcessPriorityBoost(uintptr_t handle, int32_t *disabled)
{
    if (disabled == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const nt_status status = nt_runtime_check_self_process(handle, 0x1400u);
    if (status != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    const bool available = ensure_dynamic_boost_monitor_locked();
    if (available)
        *disabled = process_dynamic_boost_disabled ? 1 : 0;
    pthread_mutex_unlock(&wait_lock);
    if (!available)
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return available ? 1 : 0;
}

int32_t NT_MS_ABI nt_SetProcessPriorityBoost(uintptr_t handle, int32_t disabled)
{
    const nt_status status = nt_runtime_check_self_process(handle, 0x200u);
    if (status != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    const bool available = ensure_dynamic_boost_monitor_locked();
    bool restored = true;
    if (available && disabled) {
        restored = restore_dynamic_boost_locked(getpid(), &leader_dynamic_boost);
        for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
            struct waitable *item = &waitables[i];
            if (item->kind == THREAD && item->shared == NULL &&
                !item->signaled && item->dynamic_boost.active &&
                item->owner_pid == (uint32_t)getpid() &&
                !restore_dynamic_boost_locked((pid_t)item->thread_id,
                                             &item->dynamic_boost))
                restored = false;
        }
    }
    if (available && restored)
        process_dynamic_boost_disabled = disabled != 0;
    pthread_mutex_unlock(&wait_lock);
    if (!available || !restored)
        nt_SetLastError(!available ? NT_ERROR_NOT_SUPPORTED :
                        NT_ERROR_GEN_FAILURE);
    return available && restored ? 1 : 0;
}

/* Native KPRIORITY values for the subset of Windows relative priorities
 * actually mapped onto the Linux nice scheduler. Dynamic boosts and
 * realtime scheduling are not implemented. */
nt_status nt_runtime_query_thread_scheduling(uintptr_t handle, uintptr_t *affinity,
                                             int32_t *priority)
{
    if (affinity == NULL || priority == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = handle == nt_GetCurrentThread() ?
                              running_thread : find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (handle != nt_GetCurrentThread() && object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object != NULL && object->kind != THREAD)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (object != NULL && handle != nt_GetCurrentThread() &&
             !(object->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)))
        status = NT_STATUS_ACCESS_DENIED;
    else if (object != NULL && wait_state(object)->owner_pid != (uint32_t)getpid())
        status = NT_STATUS_NOT_SUPPORTED;
    else {
        struct waitable *state = object != NULL ? wait_state(object) : NULL;
        uintptr_t mask = 0;
        int nice = 0;
        bool valid = true;
        if (state != NULL && state->signaled) {
            mask = state->final_group0_affinity;
            nice = state->final_nice;
            valid = state->final_nice_valid;
        } else {
            const pthread_t host = state != NULL ? state->host : pthread_self();
            const pid_t tid = state != NULL ? (pid_t)state->thread_id : gettid();
            mask = linux_group0_affinity(host);
            errno = 0;
            nice = getpriority(PRIO_PROCESS, (id_t)tid);
            valid = errno == 0;
            if (valid && state != NULL && state->dynamic_boost.active) {
                if (nice != state->dynamic_boost.base_nice - 1)
                    valid = false;
                else
                    nice = state->dynamic_boost.base_nice;
            } else if (valid && state == NULL && gettid() == getpid() &&
                       leader_dynamic_boost.active) {
                if (nice != leader_dynamic_boost.base_nice - 1)
                    valid = false;
                else
                    nice = leader_dynamic_boost.base_nice;
            }
        }
        int32_t relative = 0;
        if (!valid || mask == 0 || !host_nice_priority(nice - process_priority_nice_base, &relative))
            status = NT_STATUS_NOT_SUPPORTED;
        else {
            *affinity = mask;
            *priority = relative == -15 ? 1 : relative == 15 ? 15 : process_priority_nt_base + relative;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
}

int32_t NT_MS_ABI nt_SetThreadPriority(uintptr_t handle, int32_t priority)
{
    int relative = 0;
    if (!guest_priority_nice(priority, &relative)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&wait_lock);
    const int requested = relative + process_priority_nice_base;
    if (requested < -20 || requested > 19) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pid_t tid = 0;
    const nt_status status = priority_thread_tid_locked(handle, THREAD_SET_INFORMATION, &tid);
    if (status != NT_STATUS_SUCCESS) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    struct guest_dynamic_boost *boost = dynamic_boost_for_thread_locked(handle);
    if (boost != NULL && boost->active) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    errno = 0;
    const int previous = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno != 0) {
        const int failure = errno;
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(failure == ESRCH ? NT_ERROR_INVALID_HANDLE : NT_ERROR_GEN_FAILURE);
        return 0;
    }
    if (previous == requested) {
        pthread_mutex_unlock(&wait_lock);
        return 1;
    }
    if (setpriority(PRIO_PROCESS, (id_t)tid, requested) != 0) {
        const int failure = errno;
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(failure == EACCES || failure == EPERM ? NT_ERROR_ACCESS_DENIED :
                        failure == ESRCH ? NT_ERROR_INVALID_HANDLE :
                        failure == EINVAL ? NT_ERROR_INVALID_PARAMETER :
                        NT_ERROR_GEN_FAILURE);
        return 0;
    }
    errno = 0;
    const int observed = getpriority(PRIO_PROCESS, (id_t)tid);
    if (errno != 0 || observed != requested) {
        /* The Linux kernel might have intersected a scheduling request
         * with policy. Do not claim the guest-visible priority was set. */
        (void)setpriority(PRIO_PROCESS, (id_t)tid, previous);
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    pthread_mutex_unlock(&wait_lock);
    return 1;
}

int32_t NT_MS_ABI nt_GetThreadPriority(uintptr_t handle)
{
    pthread_mutex_lock(&wait_lock);
    pid_t tid = 0;
    const nt_status status = priority_thread_tid_locked(handle,
        THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, &tid);
    if (status != NT_STATUS_SUCCESS) {
        pthread_mutex_unlock(&wait_lock);
        nt_SetLastError(nt_runtime_nt_error(status));
        return INT32_MAX;
    }
    errno = 0;
    const int actual = getpriority(PRIO_PROCESS, (id_t)tid);
    const int failure = errno;
    const int nice_base = process_priority_nice_base;
    const struct guest_dynamic_boost *boost = dynamic_boost_for_thread_locked(handle);
    int stable = actual;
    bool boost_consistent = true;
    if (boost != NULL && boost->active) {
        boost_consistent = failure == 0 && actual == boost->base_nice - 1;
        if (boost_consistent)
            stable = boost->base_nice;
    }
    int32_t relative = 0;
    const bool representable = failure == 0 && boost_consistent &&
                              host_nice_priority(stable - nice_base, &relative);
    pthread_mutex_unlock(&wait_lock);
    if (failure != 0) {
        nt_SetLastError(failure == ESRCH ? NT_ERROR_INVALID_HANDLE : NT_ERROR_GEN_FAILURE);
        return INT32_MAX;
    }
    if (!representable) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return INT32_MAX;
    }
    return relative;
}

/* Windows GetThreadTimes / GetProcessTimes, bounded to threads and
 * child processes that this runtime owns. Closed or unauthorized handles
 * cannot be repurposed as arbitrary /proc PIDs. */
int32_t NT_MS_ABI nt_GetThreadTimes(uintptr_t handle,
                                   struct nt_file_time *created, struct nt_file_time *exited,
                                   struct nt_file_time *kernel, struct nt_file_time *user)
{
    if (created == NULL || exited == NULL || kernel == NULL || user == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint64_t beginning = 0, ending = 0, kernel_time = 0, user_time = 0;
    nt_status status = NT_STATUS_SUCCESS;
    pthread_mutex_lock(&wait_lock);
    if (handle == nt_GetCurrentThread() && running_thread == NULL) {
        char path[80];
        (void)snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", (long)gettid());
        struct rusage usage;
        if (!proc_task_times(path, NULL, NULL, &beginning) ||
            getrusage(RUSAGE_THREAD, &usage) != 0)
            status = NT_STATUS_NOT_SUPPORTED;
        else {
            kernel_time = usage_ticks(usage.ru_stime);
            user_time = usage_ticks(usage.ru_utime);
        }
    } else {
        struct waitable *object = handle == nt_GetCurrentThread() ?
                                   running_thread : find_waitable(handle);
        if (object == NULL)
            status = NT_STATUS_INVALID_HANDLE;
        else if (object->kind != THREAD)
            status = NT_STATUS_OBJECT_TYPE_MISMATCH;
        else if (handle != nt_GetCurrentThread() &&
                 !(object->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)))
            status = NT_STATUS_ACCESS_DENIED;
        else {
            struct waitable *thread = wait_state(object);
            if (thread->owner_pid != (uint32_t)getpid())
                status = NT_STATUS_NOT_SUPPORTED;
            else {
                beginning = thread->create_filetime;
                ending = thread->exit_filetime;
                if (thread->signaled) {
                    kernel_time = thread->kernel_cpu_100ns;
                    user_time = thread->user_cpu_100ns;
                } else {
                    char path[80];
                    (void)snprintf(path, sizeof(path), "/proc/self/task/%u/stat",
                                   thread->thread_id);
                    if (!proc_task_times(path, &user_time, &kernel_time, NULL))
                        status = NT_STATUS_NOT_SUPPORTED;
                }
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS || beginning == 0) {
        nt_SetLastError(status == NT_STATUS_SUCCESS ? NT_ERROR_NOT_SUPPORTED :
                        nt_runtime_nt_error(status));
        return 0;
    }
    write_filetime(created, beginning);
    write_filetime(exited, ending);
    write_filetime(kernel, kernel_time);
    write_filetime(user, user_time);
    return 1;
}

int32_t NT_MS_ABI nt_GetProcessTimes(uintptr_t handle,
                                    struct nt_file_time *created, struct nt_file_time *exited,
                                    struct nt_file_time *kernel, struct nt_file_time *user)
{
    if (created == NULL || exited == NULL || kernel == NULL || user == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint64_t beginning = 0, ending = 0, kernel_time = 0, user_time = 0;
    nt_status status = NT_STATUS_SUCCESS;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = handle == UINTPTR_MAX ? current_process_state :
                               find_waitable(handle);
    if (handle != UINTPTR_MAX && object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object != NULL && object->kind != PROCESS)
        status = NT_STATUS_OBJECT_TYPE_MISMATCH;
    else if (handle != UINTPTR_MAX &&
             !(object->access & (PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION)))
        status = NT_STATUS_ACCESS_DENIED;
    else if (handle == UINTPTR_MAX || object == NULL || wait_state(object)->self_process) {
        struct rusage usage;
        if (!proc_task_times("/proc/self/stat", NULL, NULL, &beginning) ||
            getrusage(RUSAGE_SELF, &usage) != 0)
            status = NT_STATUS_NOT_SUPPORTED;
        else {
            kernel_time = usage_ticks(usage.ru_stime);
            user_time = usage_ticks(usage.ru_utime);
        }
    } else {
        struct waitable *process = wait_state(object);
        beginning = process->create_filetime;
        ending = process->exit_filetime;
        if (process->signaled) {
            kernel_time = process->kernel_cpu_100ns;
            user_time = process->user_cpu_100ns;
        } else {
            char path[80];
            (void)snprintf(path, sizeof(path), "/proc/%u/stat", process->owner_pid);
            if (!proc_task_times(path, &user_time, &kernel_time, NULL))
                status = NT_STATUS_NOT_SUPPORTED;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    if (status != NT_STATUS_SUCCESS || beginning == 0) {
        nt_SetLastError(status == NT_STATUS_SUCCESS ? NT_ERROR_NOT_SUPPORTED :
                        nt_runtime_nt_error(status));
        return 0;
    }
    write_filetime(created, beginning);
    write_filetime(exited, ending);
    write_filetime(kernel, kernel_time);
    write_filetime(user, user_time);
    return 1;
}

void NT_MS_ABI nt_ExitThread(uint32_t code)
{
    if (running_thread != NULL) {
        thread_result = code;
        /* This skips the import gate's ordinary return epilogue and may
         * skip native frames. Never claim this thread suspendible again. */
        nt_guest_boundary_mark_nonlocal_transfer();
        siglongjmp(thread_exit, 1);
    }
    nt_runtime_run_fls_cleanup();
    nt_ExitProcess(code);
}

void NT_MS_ABI nt_FreeLibraryAndExitThread(uintptr_t module, uint32_t code)
{
    nt_FreeLibrary(module);
    nt_ExitThread(code);
}

int32_t NT_MS_ABI nt_GetExitCodeThread(uintptr_t handle, uint32_t *code)
{
    if (code == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (handle == nt_GetCurrentThread()) {
        *code = STILL_ACTIVE;
        return 1;
    }
    pthread_mutex_lock(&wait_lock);
    const struct waitable *thread = find_waitable(handle);
    uint32_t error = 0;
    if (thread == NULL || thread->kind != THREAD) {
        error = NT_ERROR_INVALID_HANDLE;
    } else if (!(thread->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION))) {
        error = NT_ERROR_ACCESS_DENIED;
    } else {
        const struct waitable *state = wait_state((struct waitable *)thread);
        *code = state->signaled ? state->exit_code : STILL_ACTIVE;
    }
    pthread_mutex_unlock(&wait_lock);
    if (error != 0)
        nt_SetLastError(error);
    return error == 0;
}

/* The complete UTF-16 name belongs to the NT thread object. The Linux
 * pthread name is only a best-effort 15-byte diagnostic abbreviation. */
int32_t NT_MS_ABI nt_SetThreadDescription(uintptr_t handle, const uint16_t *description)
{
    if (description == NULL)
        return (int32_t)UINT32_C(0x80070057);
    const size_t length = nt_runtime_wide_length(description, 32767);
    if (length == 32767 || length > (SIZE_MAX / sizeof(uint16_t)) - 1)
        return (int32_t)UINT32_C(0x80070057);
    uint16_t *copy = malloc((length + 1) * sizeof(*copy));
    if (copy == NULL)
        return (int32_t)UINT32_C(0x8007000e);
    memcpy(copy, description, (length + 1) * sizeof(*copy));
    char *name = nt_runtime_utf16_to_utf8(description, length);
    if (name == NULL) {
        free(copy);
        return (int32_t)UINT32_C(0x80070057);
    }
    size_t bytes = strlen(name);
    if (bytes > 15) {
        bytes = 15;
        while (((unsigned char)name[bytes] & 0xc0) == 0x80)
            --bytes;
        name[bytes] = '\0';
    }
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = handle == nt_GetCurrentThread() ? running_thread : find_waitable(handle);
    int error = 0;
    if (handle != nt_GetCurrentThread() && (object == NULL || object->kind != THREAD))
        error = (int)UINT32_C(0x80070006);
    else if (object != NULL && handle != nt_GetCurrentThread() &&
             !(object->access & (0x20u | 0x400u))) /* THREAD_SET_INFORMATION/_LIMITED */
        error = (int)UINT32_C(0x80070005);
    else if (object != NULL && wait_state(object)->owner_pid != (uint32_t)getpid())
        error = (int)UINT32_C(0x80070032); /* ERROR_NOT_SUPPORTED */
    else if (object != NULL && wait_state(object)->signaled)
        error = (int)UINT32_C(0x80070006);
    else {
        pthread_t host = object != NULL ? wait_state(object)->host : pthread_self();
        if (pthread_setname_np(host, name) != 0) {
            error = (int)UINT32_C(0x80070006);
        } else {
            uint16_t **slot = object != NULL ? &wait_state(object)->thread_description :
                                              &primary_thread_description;
            free(*slot);
            *slot = copy;
            copy = NULL;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    free(copy);
    free(name);
    return error;
}

int32_t NT_MS_ABI nt_GetThreadDescription(uintptr_t handle, uint16_t **description)
{
    if (description == NULL)
        return (int32_t)UINT32_C(0x80070057);
    *description = NULL;
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = handle == nt_GetCurrentThread() ? running_thread : find_waitable(handle);
    int32_t result = 0;
    if (handle != nt_GetCurrentThread() && (object == NULL || object->kind != THREAD))
        result = (int32_t)UINT32_C(0x80070006);
    else if (object != NULL && handle != nt_GetCurrentThread() &&
             !(object->access & (THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION)))
        result = (int32_t)UINT32_C(0x80070005);
    else if (object != NULL && wait_state(object)->owner_pid != (uint32_t)getpid())
        result = (int32_t)UINT32_C(0x80070032);
    else {
        const uint16_t *source = object != NULL ? wait_state(object)->thread_description :
                                                 primary_thread_description;
        size_t length = 0;
        if (source != NULL)
            while (source[length] != 0)
                ++length;
        uint16_t *allocated = (uint16_t *)nt_LocalAlloc(0, (length + 1) * sizeof(uint16_t));
        if (allocated == NULL)
            result = (int32_t)UINT32_C(0x8007000e);
        else {
            if (source != NULL)
                memcpy(allocated, source, length * sizeof(uint16_t));
            allocated[length] = 0;
            *description = allocated;
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return result;
}

struct fls_slot { nt_fls_callback callback; bool used; };
struct fls_thread { struct fls_thread *next; void *values[FLS_COUNT]; };
static struct fls_slot slots[FLS_COUNT];
static struct fls_thread *fls_threads;
static pthread_once_t fls_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t fls_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t fls_key;
static int fls_error;

static void release_fls_thread(void *context)
{
    struct fls_thread *thread = context;
    nt_fls_callback callbacks[FLS_COUNT];
    pthread_mutex_lock(&fls_lock);
    struct fls_thread **link = &fls_threads;
    while (*link != thread)
        link = &(*link)->next;
    *link = thread->next;
    for (size_t i = 0; i < FLS_COUNT; ++i)
        callbacks[i] = slots[i].callback;
    pthread_mutex_unlock(&fls_lock);
    for (size_t i = 0; i < FLS_COUNT; ++i) {
        if (thread->values[i] != NULL && callbacks[i] != NULL)
            callbacks[i](thread->values[i]);
    }
    free(thread);
}

static void create_fls_key(void) { fls_error = pthread_key_create(&fls_key, release_fls_thread); }

static bool fls_ready(void)
{
    return pthread_once(&fls_once, create_fls_key) == 0 && fls_error == 0;
}

void nt_runtime_run_fls_cleanup(void)
{
    if (!fls_ready())
        return;
    struct fls_thread *thread = pthread_getspecific(fls_key);
    for (int attempt = 0; thread != NULL && attempt < 4; ++attempt) {
        pthread_setspecific(fls_key, NULL);
        release_fls_thread(thread);
        thread = pthread_getspecific(fls_key);
    }
}

uint32_t NT_MS_ABI nt_FlsAlloc(nt_fls_callback callback)
{
    if (!fls_ready()) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINT32_MAX;
    }
    pthread_mutex_lock(&fls_lock);
    uint32_t index = 0;
    while (index < FLS_COUNT && slots[index].used)
        ++index;
    if (index < FLS_COUNT)
        slots[index] = (struct fls_slot){callback, true};
    pthread_mutex_unlock(&fls_lock);
    if (index == FLS_COUNT) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINT32_MAX;
    }
    return index;
}

int32_t NT_MS_ABI nt_FlsFree(uint32_t index)
{
    pthread_mutex_lock(&fls_lock);
    if (index >= FLS_COUNT || !slots[index].used) {
        pthread_mutex_unlock(&fls_lock);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const nt_fls_callback callback = slots[index].callback;
    size_t count = 0;
    if (callback != NULL) {
        for (const struct fls_thread *t = fls_threads; t != NULL; t = t->next)
            count += t->values[index] != NULL;
    }
    void **values = count != 0 ? malloc(count * sizeof(*values)) : NULL;
    if (count != 0 && values == NULL) {
        pthread_mutex_unlock(&fls_lock);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    size_t at = 0;
    for (struct fls_thread *t = fls_threads; t != NULL; t = t->next) {
        if (callback != NULL && t->values[index] != NULL)
            values[at++] = t->values[index];
        t->values[index] = NULL;
    }
    slots[index] = (struct fls_slot){0};
    pthread_mutex_unlock(&fls_lock);
    for (size_t i = 0; i < count; ++i)
        callback(values[i]);
    free(values);
    return 1;
}

void *NT_MS_ABI nt_FlsGetValue(uint32_t index)
{
    pthread_mutex_lock(&fls_lock);
    const bool valid = index < FLS_COUNT && slots[index].used && fls_ready();
    const struct fls_thread *thread = valid ? pthread_getspecific(fls_key) : NULL;
    void *value = thread != NULL ? thread->values[index] : NULL;
    pthread_mutex_unlock(&fls_lock);
    nt_SetLastError(valid ? NT_ERROR_SUCCESS : NT_ERROR_INVALID_PARAMETER);
    return value;
}

int32_t NT_MS_ABI nt_FlsSetValue(uint32_t index, void *value)
{
    pthread_mutex_lock(&fls_lock);
    uint32_t error = NT_ERROR_SUCCESS;
    if (index >= FLS_COUNT || !slots[index].used || !fls_ready()) {
        error = NT_ERROR_INVALID_PARAMETER;
    } else {
        struct fls_thread *thread = pthread_getspecific(fls_key);
        if (thread == NULL && value != NULL) {
            thread = calloc(1, sizeof(*thread));
            if (thread == NULL || pthread_setspecific(fls_key, thread) != 0) {
                free(thread);
                thread = NULL;
                error = NT_ERROR_NOT_ENOUGH_MEMORY;
            } else {
                thread->next = fls_threads;
                fls_threads = thread;
            }
        }
        if (thread != NULL)
            thread->values[index] = value;
    }
    pthread_mutex_unlock(&fls_lock);
    if (error != NT_ERROR_SUCCESS)
        nt_SetLastError(error);
    return error == NT_ERROR_SUCCESS;
}
