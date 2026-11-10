/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "desktop.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>

enum { WM_QUIT = 0x12, WM_TIMER = 0x113, MAX_TIMERS = 64, MIN_MS = 10, MAX_MS = INT32_MAX };

_Static_assert(sizeof(struct nt_message) == 48, "MSG layout");
_Static_assert(offsetof(struct nt_message, wparam) == 16, "MSG wParam offset");
_Static_assert(offsetof(struct nt_message, lparam) == 24, "MSG lParam offset");

struct timer {
    uintptr_t id, window;
    int fd;
    nt_timer_callback callback;
};

struct timer_queue {
    uintptr_t next_id;
    size_t next_poll;
    struct timer timers[MAX_TIMERS];
};

static pthread_once_t queue_once = PTHREAD_ONCE_INIT;
static pthread_key_t queue_key;
static int queue_error;
static _Thread_local bool quit_pending;
static _Thread_local int32_t quit_code;

static void release_queue(void *ptr)
{
    struct timer_queue *queue = ptr;
    for (size_t i = 0; i < MAX_TIMERS; ++i) {
        if (queue->timers[i].id != 0)
            close(queue->timers[i].fd);
    }
    free(queue);
}

static void initialize_queue_key(void)
{
    queue_error = pthread_key_create(&queue_key, release_queue);
}

static struct timer_queue *get_queue(bool create)
{
    if (pthread_once(&queue_once, initialize_queue_key) != 0 || queue_error != 0)
        return NULL;
    struct timer_queue *queue = pthread_getspecific(queue_key);
    if (queue == NULL && create) {
        queue = calloc(1, sizeof(*queue));
        if (queue != NULL && pthread_setspecific(queue_key, queue) != 0) {
            free(queue);
            return NULL;
        }
        if (queue != NULL)
            queue->next_id = 1;
    }
    return queue;
}

static struct timer *find_timer(struct timer_queue *queue, uintptr_t window, uintptr_t id)
{
    if (queue != NULL) {
        for (size_t i = 0; i < MAX_TIMERS; ++i) {
            if (queue->timers[i].id == id && (id == 0 || queue->timers[i].window == window))
                return &queue->timers[i];
        }
    }
    return NULL;
}

uintptr_t NT_MS_ABI nt_SetTimer(uintptr_t window, uintptr_t id, uint32_t milliseconds,
                               nt_timer_callback callback)
{
    if (window != 0 && (!nt_runtime_window_valid(window) || id == 0)) {
        nt_SetLastError(!nt_runtime_desktop_active() ? NT_ERROR_NOT_SUPPORTED :
                       (id == 0 ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_INVALID_HANDLE));
        return 0;
    }
    struct timer_queue *queue = get_queue(true);
    struct timer *timer = id != 0 ? find_timer(queue, window, id) : NULL;
    const bool create = timer == NULL;
    if (create)
        timer = find_timer(queue, 0, 0);
    if (timer == NULL || (create && window == 0 && queue->next_id == UINTPTR_MAX)) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    const int fd = create ? timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK) : timer->fd;
    if (fd < 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    if (milliseconds < MIN_MS)
        milliseconds = MIN_MS;
    else if (milliseconds > MAX_MS)
        milliseconds = MAX_MS;
    const struct timespec interval = {milliseconds / 1000, (milliseconds % 1000) * 1000000};
    const struct itimerspec due = {interval, interval};
    if (timerfd_settime(fd, 0, &due, NULL) != 0) {
        if (create)
            close(fd);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    const uintptr_t result = window != 0 ? id : (create ? queue->next_id++ : timer->id);
    *timer = (struct timer){result, window, fd, callback};
    return result;
}

int32_t NT_MS_ABI nt_KillTimer(uintptr_t window, uintptr_t id)
{
    if (id == 0 || (window != 0 && !nt_runtime_window_valid(window))) {
        nt_SetLastError(id == 0 ? NT_ERROR_INVALID_PARAMETER : NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    struct timer *timer = find_timer(get_queue(false), window, id);
    if (timer == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    close(timer->fd);
    timer->id = 0;
    return 1;
}

void nt_runtime_remove_window_timers(uintptr_t window)
{
    struct timer_queue *queue = get_queue(false);
    if (queue != NULL) {
        for (size_t i = 0; i < MAX_TIMERS; ++i) {
            struct timer *timer = &queue->timers[i];
            if (timer->id != 0 && timer->window == window) {
                close(timer->fd);
                timer->id = 0;
            }
        }
    }
}

void NT_MS_ABI nt_PostQuitMessage(int32_t code)
{
    quit_code = code;
    quit_pending = true;
}

static int wait_timer(struct timer_queue *queue, struct nt_message *message,
                      uintptr_t filter, int timeout)
{
    struct pollfd fds[MAX_TIMERS];
    const struct timer *timers[MAX_TIMERS];
    nfds_t count = 0;
    if (queue != NULL) {
        for (size_t i = 0; i < MAX_TIMERS; ++i) {
            const struct timer *timer = &queue->timers[(i + queue->next_poll) % MAX_TIMERS];
            if (timer->id != 0 && (filter == 0 || timer->window ==
                                                (filter == UINTPTR_MAX ? 0 : filter))) {
                fds[count] = (struct pollfd){.fd = timer->fd, .events = POLLIN};
                timers[count++] = timer;
            }
        }
    }
    if (count == 0)
        return 0;
    int status;
    do {
        status = poll(fds, count, timeout);
    } while (status < 0 && errno == EINTR);
    if (status == 0)
        return 0;
    if (status > 0) {
        for (nfds_t i = 0; i < count; ++i) {
            if ((fds[i].revents & POLLIN) == 0)
                continue;
            uint64_t expirations;
            ssize_t received;
            do {
                received = read(fds[i].fd, &expirations, sizeof(expirations));
            } while (received < 0 && errno == EINTR);
            if (received == sizeof(expirations)) {
                *message = (struct nt_message){.window = timers[i]->window, .message = WM_TIMER,
                    .wparam = timers[i]->id, .lparam = (intptr_t)timers[i]->callback,
                    .time = nt_GetTickCount()};
                queue->next_poll = ((size_t)(timers[i] - queue->timers) + 1) % MAX_TIMERS;
                return 1;
            }
        }
    }
    nt_SetLastError(NT_ERROR_GEN_FAILURE);
    return -1;
}

int32_t NT_MS_ABI nt_GetMessageA(struct nt_message *message, uintptr_t window,
                                 uint32_t first, uint32_t last)
{
    if (message == NULL || first > last || last > UINT16_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    if (window != 0 && window != UINTPTR_MAX && !nt_runtime_window_valid(window)) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return -1;
    }
    const bool desktop = nt_runtime_desktop_active();
    const bool timer_allowed = (first == 0 && last == 0) || (first <= WM_TIMER && last >= WM_TIMER);
    for (;;) {
        if (quit_pending) {
            *message = (struct nt_message){.message = WM_QUIT, .wparam = (uintptr_t)(intptr_t)quit_code,
                                           .time = nt_GetTickCount()};
            quit_pending = false;
            return 0;
        }
        if (desktop) {
            const int received = nt_runtime_take_window_message(message, window, first, last);
            if (received != 0)
                return received;
            if (quit_pending)
                continue;
        }
        if (timer_allowed) {
            const int received = wait_timer(get_queue(false), message, window, desktop ? 0 : -1);
            if (received != 0)
                return received;
        }
        if (!desktop) {
            nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
            return -1;
        }
        nt_runtime_desktop_wait(16);
    }
}

intptr_t NT_MS_ABI nt_DispatchMessageA(const struct nt_message *message)
{
    if (message == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (message->message == WM_TIMER && message->lparam != 0) {
        const nt_timer_callback callback = (nt_timer_callback)message->lparam;
        if (message->window == 0 || nt_runtime_window_valid(message->window))
            callback(message->window, WM_TIMER, message->wparam, nt_GetTickCount());
        return 0;
    }
    return message->window != 0 ?
           nt_SendMessageA(message->window, message->message, message->wparam, message->lparam) : 0;
}
