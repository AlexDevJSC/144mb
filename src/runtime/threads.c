/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sync.h"
#include "child_process.h"
#include "nt_services.h"
#include "winbase.h"
#include "ntnative/guest_thread.h"

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

enum { WAITABLE_COUNT = 256, MAXIMUM_WAIT_OBJECTS = 64, FLS_COUNT = 128, STILL_ACTIVE = 259 };
enum { EVENT_QUERY_STATE = 1, EVENT_MODIFY_STATE = 2, READ_CONTROL = 0x20000,
       SYNCHRONIZE = 0x100000, EVENT_ALL_ACCESS = 0x1f0003 };
enum waitable_kind { EVENT, THREAD, PROCESS };
struct waitable {
    uintptr_t handle;
    unsigned int refs;
    enum waitable_kind kind;
    bool manual, signaled, started;
    uint32_t access;
    pthread_t host;
    uint32_t thread_id, exit_code;
    nt_thread_start start;
    void *parameter;
};

static struct waitable waitables[WAITABLE_COUNT];
static uintptr_t next_handle = 0x76000001;
static pthread_mutex_t wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_changed = PTHREAD_COND_INITIALIZER;
static _Thread_local struct waitable *running_thread;
static _Thread_local uint32_t thread_result;
static _Thread_local sigjmp_buf thread_exit;

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
    if (next_handle == UINTPTR_MAX)
        return NULL;
    for (size_t i = 0; i < WAITABLE_COUNT; ++i) {
        if (waitables[i].refs == 0) {
            waitables[i] = (struct waitable){.handle = next_handle++, .refs = 1, .kind = kind,
                                             .access = SYNCHRONIZE};
            return &waitables[i];
        }
    }
    return NULL;
}

bool nt_runtime_close_waitable(uintptr_t handle)
{
    pthread_mutex_lock(&wait_lock);
    struct waitable *object = find_waitable(handle);
    if (object != NULL) {
        object->handle = 0;
        --object->refs;
        pthread_cond_broadcast(&wait_changed);
    }
    pthread_mutex_unlock(&wait_lock);
    return object != NULL;
}

_Static_assert(sizeof(struct nt_object_attributes) == 48 &&
               offsetof(struct nt_object_attributes, name) == 16, "AMD64 OBJECT_ATTRIBUTES layout");

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

nt_status NT_MS_ABI nt_NtCreateEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes,
                                   uint32_t type, uint8_t initial_state)
{
    if (!handle)
        return NT_STATUS_ACCESS_VIOLATION;
    if (type > 1 || (attributes && attributes->size != sizeof(*attributes)))
        return NT_STATUS_INVALID_PARAMETER;
    if (attributes && (attributes->name || attributes->root || attributes->security ||
                       attributes->qos || (attributes->attributes & ~0x40u)))
        return NT_STATUS_NOT_SUPPORTED;
    const uint32_t rights = event_access(access);
    if (rights & ~EVENT_ALL_ACCESS)
        return NT_STATUS_ACCESS_DENIED;
    pthread_mutex_lock(&wait_lock);
    struct waitable *event = new_waitable(EVENT);
    if (event != NULL) {
        event->manual = type == 0; /* NotificationEvent */
        event->signaled = initial_state != 0;
        event->access = rights;
        *handle = event->handle;
    }
    pthread_mutex_unlock(&wait_lock);
    return event ? NT_STATUS_SUCCESS : NT_STATUS_NO_MEMORY;
}

uintptr_t NT_MS_ABI nt_CreateEventW(const void *security, int32_t manual, int32_t initial, const uint16_t *name)
{
    if (security || (name && *name)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    uintptr_t handle = 0;
    const nt_status status = nt_NtCreateEvent(&handle, EVENT_ALL_ACCESS, NULL, manual ? 0 : 1, initial != 0);
    nt_SetLastError(nt_runtime_nt_error(status));
    return handle;
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
        if (previous)
            *previous = event->signaled;
        event->signaled = signaled;
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
            if (objects[j] == object)
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
        if (objects[i]->signaled) {
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
        if (objects[i]->kind == EVENT && !objects[i]->manual)
            objects[i]->signaled = false;
    }
    return all ? NT_STATUS_SUCCESS : (nt_status)first;
}

nt_status NT_MS_ABI nt_NtWaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                              uint32_t type, uint8_t alertable, const int64_t *timeout)
{
    (void)alertable; /* No guest APC producer is registered. */
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
    if (result == NT_STATUS_SUCCESS) {
        int error = timeout && deadline.time.tv_sec < 0 ? ETIMEDOUT : 0;
        for (;;) {
            result = check_wait_objects(objects, count, type == 0);
            if (result != NT_STATUS_TIMEOUT || error)
                break;
            error = timeout ? pthread_cond_clockwait(&wait_changed, &wait_lock, deadline.clock, &deadline.time) :
                              pthread_cond_wait(&wait_changed, &wait_lock);
        }
        if (result == NT_STATUS_TIMEOUT && error != ETIMEDOUT)
            result = NT_STATUS_UNSUCCESSFUL;
        for (uint32_t i = 0; i < count; ++i)
            --objects[i]->refs;
    }
    pthread_mutex_unlock(&wait_lock);
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
    pid_t result;
    do {
        result = waitpid(watch->pid, &status, 0);
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
    watch->process->exit_code = watch->thread->exit_code = code;
    watch->process->signaled = watch->thread->signaled = true;
    --watch->process->refs;
    --watch->thread->refs;
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
    free(watch);
    return NULL;
}

bool nt_runtime_watch_child(pid_t pid, int status_fd, struct nt_process_information *info)
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
        if (process)
            *process = (struct waitable){0};
        if (thread)
            *thread = (struct waitable){0};
        free(watch);
    }
    pthread_mutex_unlock(&wait_lock);
    pthread_attr_destroy(&attr);
    return error == 0;
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
    bool valid = object && object->kind == PROCESS;
    if (valid)
        *code = object->signaled ? object->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&wait_lock);
    if (!valid)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return valid;
}

uintptr_t NT_MS_ABI nt_GetCurrentThread(void) { return UINTPTR_MAX - 1; }

static void finish_thread(void *state)
{
    struct nt_guest_thread *guest = state;
    nt_runtime_run_fls_cleanup();
    if (guest->entered) {
        nt_runtime_notify_tls(3); /* DLL_THREAD_DETACH */
        nt_guest_thread_leave(guest);
    }
    pthread_mutex_lock(&wait_lock);
    running_thread->exit_code = thread_result;
    running_thread->signaled = true;
    --running_thread->refs;
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
    running_thread->started = true;
    pthread_cond_broadcast(&wait_changed);
    pthread_mutex_unlock(&wait_lock);
    if (sigsetjmp(thread_exit, 1) == 0) {
        if (guest.entered)
            nt_runtime_notify_tls(2); /* DLL_THREAD_ATTACH */
        thread_result = ready ? running_thread->start(running_thread->parameter) : NT_ERROR_GEN_FAILURE;
    }
    finish_thread(&guest);
    return NULL;
}

uintptr_t NT_MS_ABI nt_CreateThread(const void *security, size_t stack, nt_thread_start start,
                                   void *parameter, uint32_t flags, uint32_t *thread_id)
{
    if (start == NULL || security != NULL || flags != 0) {
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
        thread->start = start;
        thread->parameter = parameter;
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
    const bool valid = thread != NULL && thread->kind == THREAD;
    if (valid)
        *code = thread->signaled ? thread->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&wait_lock);
    if (!valid)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return valid;
}

int32_t NT_MS_ABI nt_SetThreadDescription(uintptr_t handle, const uint16_t *description)
{
    if (description == NULL)
        return (int32_t)UINT32_C(0x80070057);
    const size_t length = nt_runtime_wide_length(description, 32767);
    if (length == 32767)
        return (int32_t)UINT32_C(0x80070057);
    char *name = nt_runtime_utf16_to_utf8(description, length);
    if (name == NULL)
        return (int32_t)UINT32_C(0x8007000e);
    size_t bytes = strlen(name);
    if (bytes > 15) {
        bytes = 15;
        while (((unsigned char)name[bytes] & 0xc0) == 0x80)
            --bytes;
        name[bytes] = '\0';
    }
    pthread_mutex_lock(&wait_lock);
    const struct waitable *thread = find_waitable(handle);
    int error = ESRCH;
    if (handle == nt_GetCurrentThread())
        error = pthread_setname_np(pthread_self(), name);
    else if (thread != NULL && thread->kind == THREAD && !thread->signaled)
        error = pthread_setname_np(thread->host, name);
    pthread_mutex_unlock(&wait_lock);
    free(name);
    return error == 0 ? 0 : (int32_t)UINT32_C(0x80070006);
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
