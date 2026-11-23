/* SPDX-License-Identifier: MIT */
/* Genuine PE32+: cooperative cancellation after START is a request,
 * not an unsafe asynchronous cross-stack unwind. The callback runs on
 * its own target Win64 thread and voluntarily exits after checking it. */
#include <windows.h>
#include <stdint.h>
#define NT_STATUS_SUCCESS ((LONG)0)
#define NT_STATUS_PENDING_VALUE ((LONG)0x103)
#define NT_STATUS_ACCESS_DENIED_VALUE ((LONG)0xc0000022u)
__declspec(dllimport) LONG NTAPI NtNativeQueueSpecialApc(
    HANDLE, void (NTAPI *)(void *, void *, void *),
    void *, void *, void *, uint64_t *);
__declspec(dllimport) LONG NTAPI NtNativeCancelSpecialApc(HANDLE, uint64_t);
__declspec(dllimport) LONG NTAPI NtNativeQuerySpecialApc(HANDLE, uint64_t, DWORD *);
__declspec(dllimport) DWORD NTAPI NtNativeSpecialApcCancelRequested(void);
struct state {
    volatile LONG entered, observed, returned;
    DWORD callback_tid;
};
static void NTAPI cancellable(void *context, void *unused1, void *unused2)
{
    struct state *state = (struct state *)context;
    (void)unused1;
    (void)unused2;
    state->callback_tid = GetCurrentThreadId();
    InterlockedExchange(&state->entered, 1);
    /* A callback can own locks or operate on PE resources. Do not
     * force-kill it: it must decide when those resources are safe. */
    for (unsigned i = 0; i < 10000u; ++i) {
        if (NtNativeSpecialApcCancelRequested()) {
            InterlockedExchange(&state->observed, 1);
            break;
        }
        Sleep(1); /* real import-return; no nested special dispatch */
    }
    InterlockedExchange(&state->returned, 1);
}
static DWORD WINAPI worker(void *context)
{
    struct state *state = context;
    /* The test starts suspended with the APC already queued. It cannot
     * run the callback on the wrong thread or in the Linux signal handler. */
    Sleep(0);
    if (!state->returned || state->callback_tid != GetCurrentThreadId())
        return 10;
    return 0x144u;
}
int import_fixture_entry(void)
{
    struct state state = {0};
    DWORD tid = 0, code = 0;
    HANDLE target = CreateThread(NULL, 0, worker, &state, CREATE_SUSPENDED, &tid);
    if (!target || tid == 0)
        return 1;
    uint64_t ticket = 0;
    if (NtNativeQueueSpecialApc(target, cancellable, &state, NULL, NULL,
                                &ticket) != NT_STATUS_SUCCESS || ticket == 0)
        return 2;
    DWORD observed = 0;
    if (NtNativeQuerySpecialApc(target, ticket, &observed) != NT_STATUS_SUCCESS ||
        observed != 1u)
        return 3;
    HANDLE limited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!limited) return 4;
    if (NtNativeCancelSpecialApc(limited, ticket) !=
        NT_STATUS_ACCESS_DENIED_VALUE ||
        NtNativeQuerySpecialApc(limited, ticket, &observed) !=
        NT_STATUS_ACCESS_DENIED_VALUE)
        return 5;
    if (ResumeThread(target) != 1u)
        return 6;
    int started = 0;
    for (unsigned i = 0; i < 5000; ++i) {
        if (InterlockedCompareExchange(&state.entered, 0, 0)) {
            started = 1; break;
        }
        Sleep(1);
    }
    if (!started) return 7;
    if (NtNativeQuerySpecialApc(target, ticket, &observed) != NT_STATUS_SUCCESS ||
        observed != 2u)
        return 8;
    if (NtNativeCancelSpecialApc(target, ticket) != NT_STATUS_PENDING_VALUE)
        return 9;
    if (WaitForSingleObject(target, 10000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(target, &code) || code != 0x144u)
        return 11;
    if (!state.observed || !state.returned || state.callback_tid != tid)
        return 12;
    if (NtNativeQuerySpecialApc(target, ticket, &observed) != NT_STATUS_SUCCESS ||
        observed != 5u)
        return 13;
    if (!CloseHandle(limited) || !CloseHandle(target))
        return 14;
    return 0;
}
