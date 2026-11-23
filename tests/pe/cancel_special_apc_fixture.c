/* SPDX-License-Identifier: MIT */
/* Genuine freestanding PE32+ exercising an NTNATIVE extension, not
 * an invented native Windows APC cancellation API. Each ticket belongs
 * to the canonical target thread, protected by THREAD_SET_CONTEXT. */
#include <windows.h>
#include <stdint.h>

#define STATUS_OK ((LONG)0)
#define STATUS_ACCESS_DENIED ((LONG)0xc0000022u)
#define STATUS_NOT_FOUND ((LONG)0xc0000225u)
#define STATUS_THREAD_IS_TERMINATING ((LONG)0xc000004bu)

__declspec(dllimport) LONG NTAPI NtNativeQueueSpecialApc(
    HANDLE, void (NTAPI *)(void *, void *, void *),
    void *, void *, void *, uint64_t *);
__declspec(dllimport) LONG NTAPI NtNativeCancelSpecialApc(HANDLE, uint64_t);
__declspec(dllimport) LONG NTAPI NtNativeQuerySpecialApc(HANDLE, uint64_t, DWORD *);

static volatile DWORD called, observed_thread;
static DWORD WINAPI controlled_worker(void *argument)
{
    HANDLE *events = argument;
    if (!SetEvent(events[0]))
        return 30;
    /* No alertable wait. While blocked in this host import, the special
     * dispatcher is forbidden from invoking a callback or host locks. */
    if (WaitForSingleObject(events[1], 5000) != WAIT_OBJECT_0)
        return 31;
    Sleep(0); /* trusted PE import return delivers surviving APC */
    return called == 1 ? 0x144u : 32u;
}
static void NTAPI delivered(void *context, void *first, void *second)
{
    if (context == (void *)(uintptr_t)0x144u &&
        first == (void *)(uintptr_t)0x145u &&
        second == (void *)(uintptr_t)0x146u) {
        observed_thread = GetCurrentThreadId();
        ++called;
    }
}

int import_fixture_entry(void)
{
    HANDLE events[2];
    events[0] = CreateEventW(NULL, TRUE, FALSE, NULL);
    events[1] = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!events[0] || !events[1]) return 10;
    DWORD tid = 0, exit_code = 0;
    /* CREATE_SUSPENDED is a deterministic pending-queue barrier:
     * CreateThread has already attached the guest TEB but the worker
     * has not executed a single PE instruction. The cancel result
     * cannot race a callback which starts during SetEvent's return. */
    HANDLE thread = CreateThread(NULL, 0, controlled_worker, events,
                                 CREATE_SUSPENDED, &tid);
    if (!thread || !tid) return 11;
    HANDLE query = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!query) return 13;
    uint64_t ticket = UINT64_C(0x1122334455667788);
    if (NtNativeQueueSpecialApc(query, delivered,
                                (void *)(uintptr_t)0x144u,
                                (void *)(uintptr_t)0x145u,
                                (void *)(uintptr_t)0x146u, &ticket) !=
        STATUS_ACCESS_DENIED || ticket != UINT64_C(0x1122334455667788))
        return 14;
    uint64_t revoked = 0, retained = 0;
    if (NtNativeQueueSpecialApc(thread, delivered,
                                (void *)(uintptr_t)0x144u,
                                (void *)(uintptr_t)0x145u,
                                (void *)(uintptr_t)0x146u, &revoked) !=
            STATUS_OK || revoked == 0)
        return 15;
    DWORD apc_state = 0;
    if (NtNativeQuerySpecialApc(thread, revoked, &apc_state) != STATUS_OK ||
        apc_state != 1u)
        return 25;
    if (NtNativeCancelSpecialApc(query, revoked) != STATUS_ACCESS_DENIED ||
        NtNativeCancelSpecialApc(thread, 0) != (LONG)STATUS_INVALID_PARAMETER)
        return 16;
    if (NtNativeCancelSpecialApc(thread, revoked) != STATUS_OK ||
        NtNativeCancelSpecialApc(thread, revoked) != STATUS_NOT_FOUND)
        return 17;
    if (NtNativeQuerySpecialApc(thread, revoked, &apc_state) != STATUS_OK ||
        apc_state != 4u)
        return 26;
    /* A stale ticket must NEVER refer to a later APC in the same queue. */
    if (NtNativeQueueSpecialApc(thread, delivered,
                                (void *)(uintptr_t)0x144u,
                                (void *)(uintptr_t)0x145u,
                                (void *)(uintptr_t)0x146u, &retained) !=
            STATUS_OK || retained == revoked || retained == 0)
        return 18;
    if (NtNativeCancelSpecialApc(thread, revoked) != STATUS_NOT_FOUND ||
        NtNativeQueueSpecialApc(thread, delivered, NULL, NULL, NULL,
                                NULL) != (LONG)STATUS_INVALID_PARAMETER)
        return 19;
    if (called != 0 || !CloseHandle(query) ||
        ResumeThread(thread) != 1u)
        return 20;
    if (WaitForSingleObject(events[0], 5000) != WAIT_OBJECT_0 ||
        !SetEvent(events[1]))
        return 12;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144u ||
        called != 1 || observed_thread != tid)
        return 21;
    if (NtNativeCancelSpecialApc(thread, retained) != STATUS_THREAD_IS_TERMINATING)
        return 22;
    if (NtNativeQuerySpecialApc(thread, retained, &apc_state) != STATUS_OK ||
        apc_state != 3u)
        return 27;
    if (!CloseHandle(thread) || !CloseHandle(events[0]) ||
        !CloseHandle(events[1]))
        return 23;
    if (NtNativeCancelSpecialApc(thread, retained) != (LONG)STATUS_INVALID_HANDLE)
        return 24;
    return 0;
}
