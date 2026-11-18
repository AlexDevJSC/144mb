/* SPDX-License-Identifier: MIT */
/* A real freestanding Win64 PE checks guest thread APC delivery. */
#include <windows.h>
#include <stdint.h>

#define STATUS_OK ((LONG)0)
#define STATUS_ACCESS_DENIED ((LONG)0xc0000022u)
#define STATUS_NOT_SUPPORTED ((LONG)0xc00000bbu)
#define STATUS_ALERTED ((LONG)0x00000101u)
#define STATUS_THREAD_IS_TERMINATING ((LONG)0xc000004bu)
#define STATUS_OBJECT_TYPE_MISMATCH ((LONG)0xc0000024u)

__declspec(dllimport) LONG NTAPI NtQueueApcThread(
    HANDLE, void (NTAPI *)(void *, void *, void *), void *, void *, void *);
__declspec(dllimport) LONG NTAPI NtQueueApcThreadEx(
    HANDLE, HANDLE, void (NTAPI *)(void *, void *, void *),
    void *, void *, void *);
__declspec(dllimport) LONG NTAPI NtAlertThread(HANDLE);
__declspec(dllimport) LONG NTAPI NtAlertResumeThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtWaitForSingleObject(
    HANDLE, BOOLEAN, const LARGE_INTEGER *);
__declspec(dllimport) LONG NTAPI NtTestAlert(void);
__declspec(dllimport) LONG NTAPI NtDelayExecution(BOOLEAN, const LARGE_INTEGER *);

static volatile DWORD apc_count, callback_thread, callback_ok, worker_id;
static volatile DWORD self_count;
static void CALLBACK single_apc(ULONG_PTR argument)
{
    ++apc_count;
    callback_thread = GetCurrentThreadId();
    if (argument == 0x143u)
        ++callback_ok;
}
static void NTAPI native_apc(void *context, void *first, void *second)
{
    ++apc_count;
    callback_thread = GetCurrentThreadId();
    if ((uintptr_t)context == 0x144u &&
        (uintptr_t)first == 0x145u && (uintptr_t)second == 0x146u)
        ++callback_ok;
}
static void CALLBACK self_apc(ULONG_PTR argument)
{
    if (argument == 0x177u)
        ++self_count;
}
static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    worker_id = GetCurrentThreadId();
    if (SleepEx(INFINITE, TRUE) != WAIT_IO_COMPLETION || apc_count != 1 ||
        callback_ok != 1 || callback_thread != worker_id)
        return 21;
    if (SleepEx(INFINITE, TRUE) != WAIT_IO_COMPLETION || apc_count != 2 ||
        callback_ok != 2 || callback_thread != worker_id)
        return 22;

    HANDLE event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event == NULL)
        return 23;
    if (!QueueUserAPC(self_apc, GetCurrentThread(), 0x177u))
        return 24;
    if (WaitForSingleObjectEx(event, 0, FALSE) != WAIT_TIMEOUT || self_count != 0)
        return 25;
    if (WaitForSingleObjectEx(event, 0, TRUE) != WAIT_IO_COMPLETION || self_count != 1)
        return 26;
    if (NtTestAlert() != STATUS_OK)
        return 27;

    if (!QueueUserAPC(self_apc, GetCurrentThread(), 0x177u))
        return 28;
    LARGE_INTEGER interval = {.QuadPart = -10000000};
    if (NtDelayExecution(TRUE, &interval) != STATUS_USER_APC || self_count != 2)
        return 29;

    if (!QueueUserAPC(self_apc, GetCurrentThread(), 0x177u) ||
        NtTestAlert() != STATUS_USER_APC || self_count != 3 ||
        NtTestAlert() != STATUS_OK)
        return 30;
    /* Alert state is not an APC. NtTestAlert returns STATUS_ALERTED once;
     * a non-alertable delay does not consume it. NTDLL preserves LastError. */
    SetLastError(0x778u);
    if (NtAlertThread(GetCurrentThread()) != STATUS_OK ||
        GetLastError() != 0x778u ||
        NtTestAlert() != STATUS_ALERTED || NtTestAlert() != STATUS_OK)
        return 32;

    LARGE_INTEGER short_delay = {.QuadPart = -10000};
    if (NtAlertThread(GetCurrentThread()) != STATUS_OK ||
        NtDelayExecution(FALSE, &short_delay) != STATUS_OK ||
        NtTestAlert() != STATUS_ALERTED)
        return 33;
    if (NtAlertThread(GetCurrentThread()) != STATUS_OK ||
        NtDelayExecution(TRUE, &interval) != STATUS_ALERTED ||
        NtTestAlert() != STATUS_OK)
        return 34;
    LARGE_INTEGER zero = {.QuadPart = 0};
    if (NtAlertThread(GetCurrentThread()) != STATUS_OK ||
        NtWaitForSingleObject(event, TRUE, &zero) != STATUS_ALERTED ||
        WaitForSingleObject(event, 0) != WAIT_TIMEOUT ||
        NtTestAlert() != STATUS_OK)
        return 35;

    /* Reserve HANDLE 1 is a special APC (unsafe without asynchronous
     * context transition). It must fail, not queue a fake normal APC. */
    if (NtQueueApcThreadEx(GetCurrentThread(), (HANDLE)(uintptr_t)1,
                           native_apc, NULL, NULL, NULL) != STATUS_NOT_SUPPORTED)
        return 36;

    typedef DWORD (WINAPI *queue_apc2)(PAPCFUNC, HANDLE, ULONG_PTR, DWORD);
    union { FARPROC address; queue_apc2 call; } resolved = {
        .address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueueUserAPC2")
    };
    queue_apc2 modern = resolved.call;
    if (modern == NULL || !modern(self_apc, GetCurrentThread(), 0x177u, 0) ||
        NtTestAlert() != STATUS_USER_APC || self_count != 4)
        return 37;
    SetLastError(0);
    if (modern(self_apc, GetCurrentThread(), 0x177u, 1) ||
        GetLastError() != ERROR_NOT_SUPPORTED || NtTestAlert() != STATUS_OK)
        return 38;
    if (!CloseHandle(event))
        return 31;
    return 0x144;
}

/* Four concurrent PE Win32 threads independently consume two queued APCs,
 * preserving target identity and FIFO without signal-handler callbacks.
 * This exercises the production ordinary APC queue, NOT special APC. */
struct multi_apc_target {
    HANDLE ready;
    DWORD thread_id;
    DWORD user_seen, native_seen, calls;
};
static void CALLBACK multi_user_apc(ULONG_PTR argument)
{
    struct multi_apc_target *target = (struct multi_apc_target *)argument;
    if (!target) return;
    target->user_seen = GetCurrentThreadId();
    ++target->calls;
}
static void NTAPI multi_native_apc(void *context, void *first, void *second)
{
    struct multi_apc_target *target = context;
    if (!target || first != (void *)(uintptr_t)0x515u ||
        second != (void *)(uintptr_t)0x616u) return;
    target->native_seen = GetCurrentThreadId();
    ++target->calls;
}
static DWORD WINAPI multi_apc_worker(void *argument)
{
    struct multi_apc_target *target = argument;
    target->thread_id = GetCurrentThreadId();
    if (!SetEvent(target->ready))
        return 71;
    if (SleepEx(INFINITE, TRUE) != WAIT_IO_COMPLETION)
        return 72;
    if (SleepEx(INFINITE, TRUE) != WAIT_IO_COMPLETION)
        return 73;
    if (target->calls != 2u || target->user_seen != target->thread_id ||
        target->native_seen != target->thread_id)
        return 74;
    return 0x177u;
}
static int test_apc_concurrent_threads(void)
{
    enum { APC_THREADS = 4 };
    struct multi_apc_target targets[APC_THREADS] = {{0}};
    HANDLE handles[APC_THREADS] = {0};
    DWORD ids[APC_THREADS] = {0};
    for (int i = 0; i < APC_THREADS; ++i) {
        targets[i].ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!targets[i].ready) return 75;
        handles[i] = CreateThread(NULL, 0, multi_apc_worker, &targets[i], 0, &ids[i]);
        if (!handles[i] || ids[i] == 0 ||
            WaitForSingleObject(targets[i].ready, 5000) != WAIT_OBJECT_0)
            return 76;
    }
    for (int i = 0; i < APC_THREADS; ++i)
        if (!QueueUserAPC(multi_user_apc, handles[i],
                          (ULONG_PTR)&targets[i]))
            return 77;
    for (int i = 0; i < APC_THREADS; ++i)
        if (NtQueueApcThreadEx(handles[i], NULL, multi_native_apc,
                               &targets[i], (void *)(uintptr_t)0x515u,
                               (void *)(uintptr_t)0x616u) != STATUS_OK)
            return 78;
    for (int i = 0; i < APC_THREADS; ++i) {
        DWORD code = 0;
        if (WaitForSingleObject(handles[i], 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(handles[i], &code) || code != 0x177u ||
            targets[i].calls != 2u || targets[i].thread_id != ids[i] ||
            !CloseHandle(handles[i]) || !CloseHandle(targets[i].ready))
            return 79;
    }
    return 0;
}

/* A real cross-thread alert wakes the target's alertable NT wait, without
 * delivering any APC and without sending pthread signals or SIGSTOP. */
static DWORD WINAPI alert_worker(void *data)
{
    HANDLE ready = (HANDLE)data;
    if (!SetEvent(ready))
        return 51;
    LARGE_INTEGER five_seconds = {.QuadPart = -50000000};
    return NtDelayExecution(TRUE, &five_seconds) == STATUS_ALERTED &&
           NtTestAlert() == STATUS_OK ? 0x166u : 52u;
}
int import_fixture_entry(void)
{
    DWORD tid = 0;
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, &tid);
    if (thread == NULL || tid == 0)
        return 10;
    HANDLE restricted = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (restricted == NULL)
        return 11;
    SetLastError(0);
    if (QueueUserAPC(single_apc, restricted, 0x143u) != 0 ||
        GetLastError() != ERROR_ACCESS_DENIED ||
        NtQueueApcThread(restricted, native_apc, NULL, NULL, NULL) != STATUS_ACCESS_DENIED ||
        NtQueueApcThreadEx(restricted, NULL, native_apc, NULL, NULL, NULL) != STATUS_ACCESS_DENIED ||
        NtAlertThread(restricted) != STATUS_ACCESS_DENIED ||
        NtAlertResumeThread(restricted, NULL) != STATUS_ACCESS_DENIED)
        return 12;
    if (!CloseHandle(restricted))
        return 13;

    if (!QueueUserAPC(single_apc, thread, 0x143u) ||
        NtQueueApcThreadEx(thread, NULL, native_apc, (void *)(uintptr_t)0x144u,
                           (void *)(uintptr_t)0x145u, (void *)(uintptr_t)0x146u) != STATUS_OK)
        return 14;
    if ((DWORD)NtQueueApcThread(thread, NULL, NULL, NULL, NULL) != STATUS_INVALID_PARAMETER ||
        ResumeThread(thread) != 1)
        return 15;
    DWORD code = 0;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &code) || code != 0x144u ||
        apc_count != 2 || callback_ok != 2 || callback_thread != tid || worker_id != tid)
        return 16;

    if (QueueUserAPC(single_apc, thread, 0x143u) != 0 ||
        NtAlertThread(thread) != STATUS_THREAD_IS_TERMINATING)
        return 17;
    if (!CloseHandle(thread))
        return 18;

    HANDLE ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ready == NULL)
        return 41;
    DWORD alert_tid = 0;
    HANDLE interrupted = CreateThread(NULL, 0, alert_worker, ready, 0, &alert_tid);
    if (interrupted == NULL || alert_tid == 0)
        return 42;
    if (WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0 ||
        NtAlertThread(ready) != STATUS_OBJECT_TYPE_MISMATCH ||
        NtAlertThread(interrupted) != STATUS_OK)
        return 43;
    if (WaitForSingleObject(interrupted, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(interrupted, &code) || code != 0x166u)
        return 44;
    if (NtTestAlert() != STATUS_OK ||
        !CloseHandle(interrupted) || !CloseHandle(ready))
        return 45;

    /* Suspend+alert are one transition. A pending alert remains on the
     * canonical thread even when another HANDLE aliases the object. */
    ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (ready == NULL)
        return 46;
    HANDLE gated = CreateThread(NULL, 0, alert_worker, ready,
                                CREATE_SUSPENDED, NULL);
    if (gated == NULL)
        return 47;
    ULONG previous = 555;
    if (NtAlertResumeThread(gated, &previous) != STATUS_OK || previous != 1u ||
        WaitForSingleObject(ready, 5000) != WAIT_OBJECT_0 ||
        WaitForSingleObject(gated, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(gated, &code) || code != 0x166u)
        return 48;
    if (!CloseHandle(gated) || !CloseHandle(ready))
        return 49;
    return test_apc_concurrent_threads();
}
