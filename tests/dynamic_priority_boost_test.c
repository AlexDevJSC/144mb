/* SPDX-License-Identifier: MIT */
/* Real bounded Linux boost/deboost on a runtime-owned guest thread.
 * Requires NT_NATIVE_EXPERIMENTAL_BOOST=1 and reversible nice capability.
 * Never changes any foreign Linux process. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"
#include <stdatomic.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static atomic_int ready;
static atomic_int woke;
static uintptr_t event_handle;
static int baseline, observed_boost, restored_nice, base_priority;

static uint32_t NT_MS_ABI verify_waiter(void *ignored)
{
    (void)ignored;
    errno = 0;
    baseline = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno != 0)
        return 10;
    atomic_store_explicit(&ready, 1, memory_order_release);
    if (nt_WaitForSingleObject(event_handle, 5000) != 0)
        return 11;
    errno = 0;
    observed_boost = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno != 0)
        return 12;
    base_priority = nt_GetThreadPriority(nt_GetCurrentThread());
    atomic_store_explicit(&woke, 1, memory_order_release);
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 100000000L};
    (void)nanosleep(&delay, NULL);
    errno = 0;
    restored_nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno != 0)
        return 13;
    return 0x144u;
}

int main(void)
{
    int32_t disabled = -1;
    const bool available = nt_GetProcessPriorityBoost(UINTPTR_MAX, &disabled) != 0;
    const bool required = getenv("NT_REQUIRE_POSITIVE_BOOST") != NULL;
    if (!available) {
        if (required) {
            fputs("FAIL: no reversible Linux priority capability for required boost\n", stderr);
            return EXIT_FAILURE;
        }
        puts("SKIP positive boost: unprivileged or not opted in");
        return EXIT_SUCCESS;
    }
    if (disabled != 0) {
        fputs("FAIL: process boost must begin enabled\n", stderr);
        return EXIT_FAILURE;
    }
    event_handle = nt_CreateEventW(NULL, 0, 0, NULL);
    if (event_handle == 0)
        return 2;
    uintptr_t thread = nt_CreateThread(NULL, 0, verify_waiter, NULL, 0, NULL);
    if (thread == 0)
        return 3;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
    for (int i = 0; i < 1000 && !atomic_load_explicit(&ready, memory_order_acquire); ++i)
        (void)nanosleep(&pause, NULL);
    if (!atomic_load_explicit(&ready, memory_order_acquire))
        return 4;
    struct timespec block = {.tv_sec = 0, .tv_nsec = 30000000L};
    (void)nanosleep(&block, NULL); /* ensure waiter actually entered kernel wait */
    if (!nt_SetEvent(event_handle))
        return 5;
    if (nt_WaitForSingleObject(thread, 5000) != 0)
        return 6;
    uint32_t code = 0;
    if (!nt_GetExitCodeThread(thread, &code) || code != 0x144u)
        return 7;
    if (observed_boost != baseline - 1 || restored_nice != baseline ||
        base_priority != 0 ||
        !atomic_load_explicit(&woke, memory_order_acquire)) {
        fprintf(stderr, "FAIL boost/decay nice base=%d boosted=%d restored=%d Win32_base=%d\n",
                baseline, observed_boost, restored_nice, base_priority);
        return 8;
    }
    if (!nt_SetProcessPriorityBoost(UINTPTR_MAX, 1) ||
        !nt_GetProcessPriorityBoost(UINTPTR_MAX, &disabled) || disabled != 1 ||
        !nt_SetProcessPriorityBoost(UINTPTR_MAX, 0))
        return 9;
    if (!nt_runtime_close_waitable(thread) ||
        !nt_runtime_close_waitable(event_handle))
        return 14;
    puts("PASS dynamic priority: real Linux nice wake boost, Win32 base query and autonomous decay");
    return EXIT_SUCCESS;
}
