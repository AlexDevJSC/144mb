/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sync.h"
#include "child_process.h"
#include "nt_services.h"
#include "nt_core_information.h"
#include "winbase.h"
#include "ntnative/guest_thread.h"

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/resource.h>
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
};
struct guest_apc_queue {
    struct guest_apc *head, *tail;
    uint32_t count;
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
    bool termination_requested, child_guest, terminate_at_start, self_process;
    int32_t semaphore_count, semaphore_maximum;
    uintptr_t teb_base;
    struct guest_apc_queue apcs;
    uint32_t suspend_count;
    /* Running guest threads can only be paused at an explicit runtime
     * alertable checkpoint, never through an asynchronous Linux signal. */
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
    int final_nice;
    bool final_nice_valid;
};

static struct waitable waitables[WAITABLE_COUNT];
/* Win32 HANDLE values reserve the low bits for API-specific flags
 * (notably OVERLAPPED.hEvent bit zero). Never allocate an object handle
 * which aliases a tagged handle when that bit is set. */
static uintptr_t next_handle = 0x76000004;
static struct waitable *current_process_state;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local struct waitable *running_thread;
static _Thread_local uint32_t thread_result;
static _Thread_local sigjmp_buf thread_exit;
/* A guest's primary thread is not represented by a waitable. Its own
 * pseudo-handle has a strictly thread-local queue, never a host TID. */
static _Thread_local struct guest_apc_queue primary_apcs;
static _Thread_local bool primary_alerted;
static _Thread_local uint16_t *primary_thread_description;
static _Thread_local uint16_t primary_thread_group;
static _Thread_local bool primary_thread_group_initialized;
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
static uint32_t processor_group_counts[CPU_SETSIZE / 64];
static uintptr_t processor_group_masks[CPU_SETSIZE / 64];
/* Process policy is distinct from the affinity of an individual host
 * pthread. An explicit SetThreadAffinityMask may narrow one thread
 * without changing the process-wide Win32 policy. Protected by wait_lock. */
static uintptr_t process_group0_affinity;
static bool process_group0_affinity_initialized;
static uint16_t processor_group_count;
static uint32_t processor_group_total;
static void capture_processor_groups(void)
{
    cpu_set_t initial;
    if (sched_getaffinity(0, sizeof(initial), &initial) != 0)
        return;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (!CPU_ISSET(cpu, &initial))
            continue;
        const uint16_t group = (uint16_t)(cpu / 64);
        ++processor_group_counts[group];
        processor_group_masks[group] |= (uintptr_t)UINT64_C(1) << (cpu % 64);
        ++processor_group_total;
        if (processor_group_count <= group)
            processor_group_count = group + 1;
    }
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

/* Windows THREAD_BASIC_INFORMATION has one KAFFINITY group mask. Only
 * group zero (Linux CPUs 0..63) is representable in this runtime. */
static uintptr_t linux_group0_affinity(pthread_t host)
{
    cpu_set_t allowed;
    if (pthread_getaffinity_np(host, sizeof(allowed), &allowed) != 0)
        return 0;
    uintptr_t mask = 0;
    for (int cpu = 0; cpu < 64; ++cpu)
        if (CPU_ISSET(cpu, &allowed))
            mask |= (uintptr_t)UINT64_C(1) << cpu;
    return mask;
}

static struct waitable *find_waitable(uintptr_t handle);
static struct waitable *wait_state(struct waitable *handle);

static struct guest_apc_queue *current_apcs(void)
{
    return running_thread != NULL ? &running_thread->apcs : &primary_apcs;
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

static struct guest_apc *pop_apc_locked(void)
{
    struct guest_apc_queue *queue = current_apcs();
    struct guest_apc *apc = queue->head;
    if (apc != NULL) {
        queue->head = apc->next;
        if (queue->head == NULL)
            queue->tail = NULL;
        --queue->count;
        apc->next = NULL;
    }
    return apc;
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

/* On termination the target cannot execute additional APCs. The queue
 * belongs to its canonical THREAD slot even after every HANDLE is closed. */
static void clear_apcs_locked(struct guest_apc_queue *queue)
{
    struct guest_apc *apc = queue->head;
    while (apc != NULL) {
        struct guest_apc *next = apc->next;
        free(apc);
        apc = next;
    }
    *queue = (struct guest_apc_queue){0};
}

nt_status nt_runtime_queue_apc(uintptr_t handle, guest_user_apc user,
                                guest_native_apc native,
                                uintptr_t first, uintptr_t second, uintptr_t third)
{
    if ((user == NULL) == (native == NULL))
        return NT_STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&wait_lock);
    struct guest_apc_queue *queue = NULL;
    nt_status status = NT_STATUS_SUCCESS;
    if (handle == UINTPTR_MAX - 1) {
        queue = current_apcs();
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
        else if (wait_state(object)->signaled)
            status = NT_STATUS_THREAD_IS_TERMINATING;
        else
            queue = &wait_state(object)->apcs;
    }
    if (queue != NULL) {
        if (queue->count >= MAX_PENDING_APC) {
            status = NT_STATUS_NO_MEMORY;
        } else {
            struct guest_apc *apc = calloc(1, sizeof(*apc));
            if (apc == NULL) {
                status = NT_STATUS_NO_MEMORY;
            } else {
                *apc = (struct guest_apc){
                    .user = user, .native = native,
                    .first = first, .second = second, .third = third
                };
                if (queue->tail != NULL)
                    queue->tail->next = apc;
                else
                    queue->head = apc;
                queue->tail = apc;
                ++queue->count;
                pthread_cond_broadcast(&wait_changed);
            }
        }
    }
    pthread_mutex_unlock(&wait_lock);
    return status;
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

/* A NULL reserve behaves exactly as NtQueueApcThread. A special APC
 * (reserve=1) requires interrupting a thread even outside alertable waits;
 * no safe mechanism exists here, so refuse it instead of faking delivery. */
nt_status NT_MS_ABI nt_NtQueueApcThreadEx(uintptr_t thread, uintptr_t reserve,
                                         guest_native_apc routine,
                                         void *context, void *first, void *second)
{
    if (reserve != 0)
        return NT_STATUS_NOT_SUPPORTED;
    return nt_NtQueueApcThread(thread, routine, context, first, second);
}

uint32_t NT_MS_ABI nt_QueueUserAPC2(guest_user_apc callback, uintptr_t handle,
                                   uintptr_t data, uint32_t flags)
{
    /* QUEUE_USER_APC_FLAGS_SPECIAL_USER_APC cannot run safely while the
     * target holds internal pthread locks. Do not silently queue a normal
     * APC in its place. */
    if (flags != 0) {
        nt_SetLastError(flags == 1 ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return nt_QueueUserAPC(callback, handle, data);
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
            free(canonical->thread_description);
            canonical->thread_description = NULL;
        }
    } else if (object->links == 0) {
        free(object->thread_description);
        object->thread_description = NULL;
    }
}

/* HANDLE_FLAG_PROTECT_FROM_CLOSE is metadata on the HANDLE, never on
 * the shared kernel-object state. It must survive DuplicateHandle aliasing
 * without preventing another independent alias from being released. */
nt_status nt_runtime_close_waitable_status(uintptr_t handle)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    nt_status status = NT_STATUS_SUCCESS;
    if (object == NULL)
        status = NT_STATUS_INVALID_HANDLE;
    else if (object->handle_flags & 2u)
        status = NT_STATUS_ACCESS_DENIED;
    else {
        object->handle = 0;
        release_waitable(object);
        pthread_cond_broadcast(&wait_changed);
    }
    pthread_mutex_unlock(&wait_lock);
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
        const struct nt_semaphore_basic_information info = {
            state->semaphore_count, state->semaphore_maximum
        };
        memcpy(buffer, &info, sizeof(info));
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
        if (count > state->semaphore_maximum - state->semaphore_count) {
            status = NT_STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        } else {
            if (previous != NULL)
                *previous = state->semaphore_count;
            state->semaphore_count += count;
            state->signaled = true;
            pthread_cond_broadcast(&wait_changed);
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

uintptr_t NT_MS_ABI nt_CreateEventW(const void *security, int32_t manual, int32_t initial,
                                    const uint16_t *name)
{
    if (security != NULL) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    struct nt_guest_unicode_string unicode = win_event_string(name);
    if (name != NULL && name[0] != 0 && unicode.length >= 127 * sizeof(uint16_t)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct nt_object_attributes attrs = win_event_attributes(&unicode, false);
    uintptr_t handle = 0;
    const nt_status status = nt_NtCreateEvent(&handle, EVENT_ALL_ACCESS,
                                               name && *name ? &attrs : NULL,
                                               manual ? 0 : 1, initial != 0);
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
        const struct nt_event_basic_information information =
            {(uint32_t)(state->manual ? 0 : 1), state->signaled ? 1 : 0};
        memcpy(buffer, &information, sizeof(information));
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
        if (previous)
            *previous = state->signaled;
        state->signaled = signaled;
        if (signaled)
            pthread_cond_broadcast(&wait_changed);
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
            if (wait_state(objects[j]) == wait_state(object))
                return NT_STATUS_INVALID_PARAMETER;
        }
        objects[i] = object;
    }
    for (uint32_t i = 0; i < count; ++i)
        ++objects[i]->refs;
    return NT_STATUS_SUCCESS;
}

static nt_status check_wait_objects(struct waitable *const *objects, uint32_t count, bool all)
{
    uint32_t first = count, signaled = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!objects[i]->handle)
            return NT_STATUS_INVALID_HANDLE;
        if (wait_state(objects[i])->signaled) {
            if (!signaled)
                first = i;
            ++signaled;
        }
    }
    if (all ? signaled != count : signaled == 0)
        return NT_STATUS_TIMEOUT;
    const uint32_t start = all ? 0 : first;
    const uint32_t end = all ? count : first + 1;
    for (uint32_t i = start; i < end; ++i) {
        struct waitable *state = wait_state(objects[i]);
        if (objects[i]->kind == EVENT && !state->manual)
            state->signaled = false;
        if (objects[i]->kind == SEMAPHORE) {
            --state->semaphore_count;
            state->signaled = state->semaphore_count > 0;
        }
    }
    return all ? NT_STATUS_SUCCESS : (nt_status)first;
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
    if (result == NT_STATUS_SUCCESS) {
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
            error = timeout ? pthread_cond_clockwait(&wait_changed, &wait_lock, deadline.clock, &deadline.time) :
                              pthread_cond_wait(&wait_changed, &wait_lock);
        }
        if (result == NT_STATUS_TIMEOUT && error != ETIMEDOUT)
            result = NT_STATUS_UNSUCCESSFUL;
        for (uint32_t i = 0; i < count; ++i)
            release_waitable(objects[i]);
    }
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
        *watch = (struct child_watch){pid, status_fd, process, thread};
        pthread_t worker;
        error = pthread_create(&worker, &attr, reap_child, watch);
        if (!error)
            *info = (struct nt_process_information){process->handle, thread->handle,
                                                   (uint32_t)pid, (uint32_t)pid};
    } else {
        error = ENOMEM;
    }
    if (error) {
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
    else if (wait_state(object)->owner_pid != (uint32_t)getpid())
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
            item->owner_pid == (uint32_t)getpid() && item->thread_id == id) {
            original = item;
            break;
        }
    }
    nt_status status = NT_STATUS_INVALID_CID;
    if (original != NULL) {
        if (access & ~original->access) {
            status = NT_STATUS_ACCESS_DENIED;
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

uintptr_t NT_MS_ABI nt_OpenThread(uint32_t access, int32_t inherit, uint32_t id)
{
    uintptr_t opened = 0;
    const nt_status status = open_thread_local(&opened, access, inherit != 0, id);
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
    if (client_id->unique_process != 0 &&
        client_id->unique_process != (uintptr_t)getpid())
        return NT_STATUS_NOT_SUPPORTED;
    if (client_id->unique_thread > UINT32_MAX)
        return NT_STATUS_INVALID_CID;
    return open_thread_local(handle, access,
                             attributes != NULL && (attributes->attributes & 2u) != 0,
                             (uint32_t)client_id->unique_thread);
}


static void finish_thread(struct nt_guest_thread *guest, bool attached)
{
    nt_runtime_run_fls_cleanup();
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
    running_thread->exit_code = thread_result;
    running_thread->teb_base = 0; /* stack-allocated TEB is no longer live */
    clear_apcs_locked(&running_thread->apcs);
    running_thread->signaled = true;
    release_waitable(running_thread);
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
}

static void *thread_main(void *context)
{
    running_thread = context;
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
        thread_result = ready ? running_thread->start(running_thread->parameter) : NT_ERROR_GEN_FAILURE;
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
        else if (state->suspend_count == 0 || state->cooperative_pause_requested)
            status = NT_STATUS_NOT_SUPPORTED;
        else {
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
    /* Remote running pthreads cannot be cancelled without violating
     * their lock ownership. The startup gate is the only safe target. */
    return terminate_suspended_thread(thread, (uint32_t)exit_code);
}

/* Suspension is deliberately restricted to the startup gate. It can
 * increase the count of a thread already created suspended, but it never
 * pauses guest instructions or calls pthread signals on a running thread.
 * The gate and its count are protected by wait_lock. */
static nt_status suspend_guest_thread(uintptr_t handle, uint32_t *previous)
{
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
enum { CONTEXT_AMD64 = 0x100000, CONTEXT_CORE_GROUPS = 0x0f };

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
 * bounded backend still maps Linux CPU IDs to group cpu / 64; it does
 * not claim an arbitrary Windows NUMA/processor group topology. */
_Static_assert(sizeof(struct nt_group_affinity) == 16,
               "Windows AMD64 GROUP_AFFINITY layout");
_Static_assert(offsetof(struct nt_group_affinity, group) == 8,
               "GROUP_AFFINITY Group field offset");

static uintptr_t affinity_mask_in_group(const cpu_set_t *cpus, uint16_t group)
{
    if ((size_t)group >= CPU_SETSIZE / 64)
        return 0;
    uintptr_t mask = 0;
    for (int bit = 0; bit < 64; ++bit)
        if (CPU_ISSET((int)group * 64 + bit, cpus))
            mask |= (uintptr_t)UINT64_C(1) << bit;
    return mask;
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
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, cpus)) {
                first = (uint16_t)(cpu / 64);
                break;
            }
        if (first == UINT16_MAX)
            return NT_STATUS_NOT_SUPPORTED;
        *group = first;
        *valid = true;
    }
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
    } else {
        primary_thread_group = group;
        primary_thread_group_initialized = true;
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
    CPU_ZERO(&target);
    for (unsigned int bit = 0; bit < 64; ++bit)
        if (desired->mask & ((uintptr_t)UINT64_C(1) << bit))
            CPU_SET((int)desired->group * 64 + (int)bit, &target);

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
                    CPU_ZERO(&wanted);
                    for (int bit = 0; bit < 64; ++bit)
                        if (requested & ((uintptr_t)UINT64_C(1) << bit))
                            CPU_SET((int)group * 64 + bit, &wanted);
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
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &mask))
                present[cpu / 64] = true;
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
        for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &mask))
                present[cpu / 64] = true;
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
    const nt_status status = nt_runtime_check_self_process(process, 0x1400u);
    if (status != NT_STATUS_SUCCESS) {
        nt_SetLastError(nt_runtime_nt_error(status));
        return 0;
    }
    ensure_processor_groups();
    pthread_mutex_lock(&wait_lock);
    const bool supported = processor_group_count == 1 &&
                           processor_group_masks[0] != 0;
    if (supported && !process_group0_affinity_initialized) {
        process_group0_affinity = processor_group_masks[0];
        process_group0_affinity_initialized = true;
    }
    const uintptr_t policy = process_group0_affinity;
    const uintptr_t system = processor_group_masks[0];
    pthread_mutex_unlock(&wait_lock);
    if (!supported || policy == 0 || (policy & ~system) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    /* Process affinity is the runtime-owned policy, not the current
     * pthread's potentially narrower per-thread affinity. This is a
     * snapshot of sandbox entitlement at startup; direct Linux/cgroup
     * changes behind our back require explicit revalidation. */
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
    if (tid == getpid())
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
    if (processor_group_count != 1 || processor_group_masks[0] == 0 ||
        (requested & ~processor_group_masks[0]) != 0)
        return NT_ERROR_NOT_SUPPORTED;

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
    CPU_ZERO(&wanted);
    for (int cpu = 0; cpu < 64; ++cpu)
        if ((requested & ((uintptr_t)UINT64_C(1) << cpu)) != 0)
            CPU_SET(cpu, &wanted);

    uint32_t error = NT_ERROR_SUCCESS;
    for (size_t i = 0; i < threads.count; ++i) {
        items[i].tid = threads.ids[i];
        if (sched_getaffinity(threads.ids[i], sizeof(items[i].before),
                              &items[i].before) != 0) {
            error = NT_ERROR_NOT_SUPPORTED;
            break;
        }
        for (int cpu = 64; cpu < CPU_SETSIZE; ++cpu)
            if (CPU_ISSET(cpu, &items[i].before)) {
                error = NT_ERROR_NOT_SUPPORTED;
                break;
            }
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
        process_group0_affinity = requested;
        process_group0_affinity_initialized = true;
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
    int32_t relative = 0;
    const bool representable = failure == 0 &&
                              host_nice_priority(actual - nice_base, &relative);
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
