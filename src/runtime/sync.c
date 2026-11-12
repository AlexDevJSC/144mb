/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "sync.h"

#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

struct critical_section {
    uintptr_t debug;
    _Atomic uint32_t locked;
    uint32_t recursion;
    _Atomic uintptr_t owner;
    uintptr_t semaphore, spin;
};
_Static_assert(sizeof(struct critical_section) == 40, "CRITICAL_SECTION layout");

static void wake(void *word, int count)
{
    (void)syscall(SYS_futex, word, FUTEX_WAKE_PRIVATE, count, NULL, NULL, 0);
}

static struct timespec deadline(uint32_t milliseconds)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    time.tv_sec += milliseconds / 1000;
    time.tv_nsec += (long)(milliseconds % 1000) * 1000000;
    if (time.tv_nsec >= 1000000000) {
        ++time.tv_sec;
        time.tv_nsec -= 1000000000;
    }
    return time;
}

static int wait_word(void *word, uint32_t value, const struct timespec *until)
{
    int result;
    do {
        result = (int)syscall(SYS_futex, word, FUTEX_WAIT_BITSET_PRIVATE, value, until,
                              NULL, FUTEX_BITSET_MATCH_ANY);
    } while (result < 0 && errno == EINTR);
    return result;
}

static bool try_lock(_Atomic uint32_t *word, uint32_t unlocked)
{
    return atomic_compare_exchange_strong_explicit(word, &unlocked, 1,
                                                    memory_order_acquire, memory_order_relaxed);
}

static void acquire(_Atomic uint32_t *word, uint32_t unlocked)
{
    while (!try_lock(word, unlocked))
        wait_word(word, 1, NULL);
}

static void release(_Atomic uint32_t *word, uint32_t unlocked)
{
    atomic_store_explicit(word, unlocked, memory_order_release);
    wake(word, 1);
}

int32_t NT_MS_ABI nt_InitializeCriticalSectionEx(void *address, uint32_t spin, uint32_t flags)
{
    if (address == NULL || (flags & ~UINT32_C(0x01000000)) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    struct critical_section *section = address;
    memset(section, 0, sizeof(*section));
    atomic_init(&section->locked, UINT32_MAX);
    atomic_init(&section->owner, 0);
    section->spin = spin;
    return 1;
}

void NT_MS_ABI nt_InitializeCriticalSection(void *section)
{
    (void)nt_InitializeCriticalSectionEx(section, 0, 0);
}

void NT_MS_ABI nt_EnterCriticalSection(void *address)
{
    struct critical_section *section = address;
    const uintptr_t tid = (uintptr_t)gettid();
    if (atomic_load_explicit(&section->owner, memory_order_relaxed) == tid) {
        ++section->recursion;
        return;
    }
    acquire(&section->locked, UINT32_MAX);
    atomic_store_explicit(&section->owner, tid, memory_order_relaxed);
    section->recursion = 1;
}

void NT_MS_ABI nt_LeaveCriticalSection(void *address)
{
    struct critical_section *section = address;
    if (atomic_load_explicit(&section->owner, memory_order_relaxed) != (uintptr_t)gettid()) {
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return;
    }
    if (--section->recursion == 0) {
        atomic_store_explicit(&section->owner, 0, memory_order_relaxed);
        release(&section->locked, UINT32_MAX);
    }
}

void NT_MS_ABI nt_DeleteCriticalSection(void *address)
{
    if (address != NULL)
        memset(address, 0, sizeof(struct critical_section));
}

/* A zeroed Win32 SRWLOCK and CONDITION_VARIABLE each supply one futex word. */
void NT_MS_ABI nt_AcquireSRWLockExclusive(void *lock) { acquire(lock, 0); }
uint8_t NT_MS_ABI nt_TryAcquireSRWLockExclusive(void *lock) { return try_lock(lock, 0); }
void NT_MS_ABI nt_ReleaseSRWLockExclusive(void *lock) { release(lock, 0); }

int32_t NT_MS_ABI nt_SleepConditionVariableSRW(void *condition, void *lock, uint32_t timeout, uint32_t flags)
{
    if (condition == NULL || lock == NULL || flags != 0) {
        nt_SetLastError(flags != 0 ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uint32_t expected = atomic_load_explicit((_Atomic uint32_t *)condition, memory_order_relaxed);
    const struct timespec until = deadline(timeout);
    release(lock, 0);
    const int status = wait_word(condition, expected, timeout == UINT32_MAX ? NULL : &until);
    const int error = errno;
    acquire(lock, 0);
    if (status == 0 || error == EAGAIN)
        return 1;
    nt_SetLastError(error == ETIMEDOUT ? NT_ERROR_TIMEOUT : NT_ERROR_GEN_FAILURE);
    return 0;
}

static void wake_condition(void *condition, int count)
{
    atomic_fetch_add_explicit((_Atomic uint32_t *)condition, 1, memory_order_release);
    wake(condition, count);
}

void NT_MS_ABI nt_WakeConditionVariable(void *condition) { wake_condition(condition, 1); }
void NT_MS_ABI nt_WakeAllConditionVariable(void *condition) { wake_condition(condition, INT_MAX); }

int32_t NT_MS_ABI nt_InitOnceBeginInitialize(void *address, uint32_t flags, int32_t *pending, void **context)
{
    enum { CHECK_ONLY = 1 };
    if (address == NULL || pending == NULL || (flags & ~CHECK_ONLY) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    _Atomic uintptr_t *once = address;
    for (;;) {
        uintptr_t value = atomic_load_explicit(once, memory_order_acquire);
        if ((value & 3) == 2) {
            *pending = 0;
            if (context != NULL)
                *context = (void *)(value & ~(uintptr_t)3);
            return 1;
        }
        if ((flags & CHECK_ONLY) != 0) {
            nt_SetLastError(NT_ERROR_GEN_FAILURE);
            return 0;
        }
        if (value == 0) {
            if (atomic_compare_exchange_strong_explicit(once, &value, 1,
                                                        memory_order_acquire, memory_order_relaxed)) {
                *pending = 1;
                return 1;
            }
        } else if (value == 1) {
            wait_word(address, 1, NULL);
        } else {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
}

int32_t NT_MS_ABI nt_InitOnceComplete(void *address, uint32_t flags, void *context)
{
    enum { INIT_FAILED = 4 };
    if (address == NULL || ((uintptr_t)context & 3) != 0 ||
        (flags & ~INIT_FAILED) != 0 || ((flags & INIT_FAILED) != 0 && context != NULL)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uintptr_t pending = 1;
    const uintptr_t result = (flags & INIT_FAILED) != 0 ? 0 : (uintptr_t)context | 2;
    if (!atomic_compare_exchange_strong_explicit((_Atomic uintptr_t *)address, &pending, result,
                                                 memory_order_release, memory_order_relaxed)) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    wake(address, INT_MAX);
    return 1;
}
