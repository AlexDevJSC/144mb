/* SPDX-License-Identifier: MIT */
/* Advanced APC positive profile: first-run delivery on the actual guest
 * thread outside all alertable waits, from CREATE_SUSPENDED only.
 * Never executes an APC in a signal handler or while runtime mutex is held. */
#define _GNU_SOURCE
#include "sync.h"
#include "nt_services.h"
#include "winbase.h"

#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { THREAD_QUERY_LIMITED_INFORMATION = 0x800 };
static atomic_int order;
static atomic_int errors;
static atomic_int running;
static atomic_int release_running;
static uint32_t expected_tid;

static void NT_MS_ABI special_user(uintptr_t marker)
{
    if (marker != 0x1441u ||
        (uint32_t)syscall(SYS_gettid) != expected_tid ||
        atomic_fetch_add(&order, 1) != 0)
        atomic_fetch_add(&errors, 1);
}
static void NT_MS_ABI special_native(void *context, void *first, void *second)
{
    if ((uintptr_t)context != 0x1442u ||
        (uintptr_t)first != 0x1443u ||
        (uintptr_t)second != 0x1444u ||
        (uint32_t)syscall(SYS_gettid) != expected_tid ||
        atomic_fetch_add(&order, 1) != 1)
        atomic_fetch_add(&errors, 1);
}
static uint32_t NT_MS_ABI initial_worker(void *unused)
{
    (void)unused;
    /* No alertable sleep/wait. Both special APCs must already have
     * executed in FIFO order on THIS exact guest thread. */
    return atomic_load(&order) == 2 && atomic_load(&errors) == 0 ?
           0x144u : 0xdead;
}
static uint32_t NT_MS_ABI running_worker(void *unused)
{
    (void)unused;
    atomic_store(&running, 1);
    while (atomic_load(&release_running) == 0)
        sched_yield();
    return 0x144u;
}
static int check(int condition, const char *reason)
{
    if (!condition)
        fprintf(stderr, "special-APC checkpoint: %s\n", reason);
    return !condition;
}

int main(void)
{
    int failed = 0;
    uintptr_t handle = nt_CreateThread(NULL, 0, initial_worker, NULL, 4u,
                                       &expected_tid);
    failed += check(handle != 0 && expected_tid != 0,
                    "create a genuinely suspended guest thread");
    if (handle == 0)
        return 1;

    nt_SetLastError(0x7171u);
    failed += check(nt_QueueUserAPC2(special_user, handle, 0x1441u, 1u) == 1 &&
                    nt_GetLastError() == 0x7171u,
                    "QueueUserAPC2 special accepted on startup checkpoint");
    failed += check(nt_NtQueueApcThreadEx(handle, 1u, special_native,
                    (void *)0x1442u, (void *)0x1443u, (void *)0x1444u) == 0,
                    "NtQueueApcThreadEx reserve=1 queues a distinct native APC");
    failed += check(atomic_load(&order) == 0,
                    "neither callback may execute while CREATE_SUSPENDED");

    uintptr_t limited = 0;
    failed += check(nt_runtime_duplicate_waitable(handle,
                    THREAD_QUERY_LIMITED_INFORMATION, 0, 0, &limited) == 0 &&
                    limited != 0,
                    "obtain a query-only handle");
    nt_SetLastError(0);
    failed += check(nt_QueueUserAPC2(special_user, limited, 0x1441u, 1u) == 0 &&
                    nt_GetLastError() == NT_ERROR_ACCESS_DENIED,
                    "THREAD_SET_CONTEXT required to enqueue special APCs");
    failed += check(nt_NtQueueApcThreadEx(limited, 1u, special_native,
                    NULL, NULL, NULL) == NT_STATUS_ACCESS_DENIED,
                    "native special APC checks independent handle rights");
    failed += check(nt_runtime_close_waitable(limited),
                    "release restricted alias");
    nt_SetLastError(0);
    failed += check(nt_QueueUserAPC2(special_user, handle, 0, 2u) == 0 &&
                    nt_GetLastError() == NT_ERROR_INVALID_PARAMETER,
                    "unknown APC2 flags rejected without enqueueing");
    failed += check(nt_NtQueueApcThreadEx(handle, 2u, special_native,
                    NULL, NULL, NULL) == NT_STATUS_NOT_SUPPORTED,
                    "unknown reserve values fail closed");

    failed += check(nt_ResumeThread(handle) == 1,
                    "release the initial suspended-thread gate");
    failed += check(nt_WaitForSingleObject(handle, 5000) == 0,
                    "special APCs and guest worker exit without alertable wait");
    uint32_t exit_code = 0;
    failed += check(nt_GetExitCodeThread(handle, &exit_code) && exit_code == 0x144u &&
                    atomic_load(&order) == 2 && atomic_load(&errors) == 0,
                    "both special callbacks exactly once, FIFO, target TID");
    failed += check(nt_runtime_close_waitable(handle),
                    "release completed thread handle");

    /* A CPU-bound non-alertable thread has no asynchronous safe point:
     * do not claim success or inject code under Linux pthread locks. */
    uint32_t busy_tid = 0;
    uintptr_t busy = nt_CreateThread(NULL, 0, running_worker, NULL, 0,
                                     &busy_tid);
    failed += check(busy != 0, "create ordinary running guest");
    if (busy == 0)
        return 1;
    for (int i = 0; i < 2000 && atomic_load(&running) == 0; ++i)
        usleep(1000);
    failed += check(atomic_load(&running) == 1, "worker reached CPU-bound section");
    nt_SetLastError(0);
    failed += check(nt_QueueUserAPC2(special_user, busy, 0x1441u, 1u) == 0 &&
                    nt_GetLastError() == NT_ERROR_NOT_SUPPORTED,
                    "no false success for asynchronous CPU-bound user APC");
    failed += check(nt_NtQueueApcThreadEx(busy, 1u, special_native,
                    NULL, NULL, NULL) == NT_STATUS_NOT_SUPPORTED,
                    "no false native special APC preemption");
    atomic_store(&release_running, 1);
    failed += check(nt_WaitForSingleObject(busy, 5000) == 0,
                    "CPU-bound guest completes without spurious callback");
    failed += check(atomic_load(&order) == 2 &&
                    nt_runtime_close_waitable(busy),
                    "unsupported special APCs did not enter the queue");
    if (!failed)
        puts("PASS special APC: startup safe-point, FIFO/identity, separate native+user delivery, rights and fail-closed CPU-bound targets");
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
