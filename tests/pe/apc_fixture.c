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
static volatile DWORD self_count, special_count, special_seh_count;
static void NTAPI special_native_apc(void *context, void *first, void *second)
{
    if ((uintptr_t)context == 0x777u && (uintptr_t)first == 0x778u &&
        (uintptr_t)second == 0x779u)
        ++special_count;
}
static void CALLBACK special_user_apc(ULONG_PTR argument)
{
    if (argument == 0x77au)
        ++special_count;
}
/* Reuse the runtime's existing PE RaiseException -> VEH -> CONTEXT
 * restoration, rather than a Linux SIGSEGV handler. This tests a
 * continuable Windows software exception *inside* an APC callback. */
static LONG CALLBACK special_veh(PEXCEPTION_POINTERS pointers)
{
    if (pointers && pointers->ExceptionRecord &&
        pointers->ExceptionRecord->ExceptionCode == 0xe14400a5u) {
        ++special_seh_count;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void CALLBACK special_seh_apc(ULONG_PTR argument)
{
    if (argument == 0x77bu) {
        RaiseException(0xe14400a5u, 0, 0, NULL);
        ++special_seh_count;
    }
}
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

    /* Guest-only, bounded delivery at the safe return from this NT
     * import, without an alertable wait or hijacking a host mutex. */
    const LONG special_status =
        NtQueueApcThreadEx(GetCurrentThread(), (HANDLE)(uintptr_t)1,
                           special_native_apc, (void *)(uintptr_t)0x777u,
                           (void *)(uintptr_t)0x778u,
                           (void *)(uintptr_t)0x779u);
    if (special_status != STATUS_OK) return 36;
    if (special_count != 1u) return 92;
    if (NtTestAlert() != STATUS_OK) return 93;

    typedef DWORD (WINAPI *queue_apc2)(PAPCFUNC, HANDLE, ULONG_PTR, DWORD);
    union { FARPROC address; queue_apc2 call; } resolved = {
        .address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueueUserAPC2")
    };
    queue_apc2 modern = resolved.call;
    if (modern == NULL || !modern(self_apc, GetCurrentThread(), 0x177u, 0) ||
        NtTestAlert() != STATUS_USER_APC || self_count != 4)
        return 37;
    if (!modern(special_user_apc, GetCurrentThread(), 0x77au, 1) ||
        special_count != 2u || NtTestAlert() != STATUS_OK)
        return 38;
    /* A malformed flag and a callback outside the parent's pinned PE
     * executable range must never be scheduled or executed. */
    SetLastError(0);
    if (modern(special_user_apc, GetCurrentThread(), 0x77au, 2) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 86;
    union { uintptr_t address; PAPCFUNC callback; } invalid = {
        .address = (uintptr_t)0x12345u
    };
    SetLastError(0);
    if (modern(invalid.callback, GetCurrentThread(), 0x77au, 1) ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 87;

    /* A continuable software exception in the APC uses existing
     * guest VEH/SEH dispatch. Run it LAST: a non-local restore must
     * poison the host-import boundary against future async ptrace. */
    void *veh = AddVectoredExceptionHandler(1, special_veh);
    if (!veh)
        return 39;
    if (!modern(special_seh_apc, GetCurrentThread(), 0x77bu, 1) ||
        special_seh_count != 2u)
        return 40;
    if (!RemoveVectoredExceptionHandler(veh) || !CloseHandle(event))
        return 31;
    return 0x144;
}

/* Special APCs run in a real secondary PE thread's import-return
 * checkpoint while its application only uses NON-ALERTABLE Sleep().
 * Its normal APC queue must not consume these callbacks. */
struct special_target {
    HANDLE ready;
    DWORD tid, observed, count, fifo;
};
static void CALLBACK special_secondary_user(ULONG_PTR argument)
{
    struct special_target *state = (struct special_target *)argument;
    state->observed = GetCurrentThreadId();
    if (state->count == 0 && state->fifo == 0)
        state->fifo = 1;
    ++state->count;
}
static void NTAPI special_secondary_native(void *context, void *first, void *second)
{
    struct special_target *state = context;
    if (first != (void *)(uintptr_t)0x711u ||
        second != (void *)(uintptr_t)0x722u)
        return;
    if (state->count == 1 && state->fifo == 1)
        state->fifo = 2;
    state->observed = GetCurrentThreadId();
    ++state->count;
}
static DWORD WINAPI special_nonalertable_worker(void *argument)
{
    struct special_target *state = argument;
    state->tid = GetCurrentThreadId();
    if (!SetEvent(state->ready))
        return 80;
    for (unsigned i = 0; i < 400 && state->count < 2; ++i)
        Sleep(5); /* never SleepEx(..., TRUE) */
    if (state->count != 2 || state->fifo != 2 ||
        state->observed != state->tid)
        return 81;
    return 0x188u;
}
static int test_special_nonalertable(void)
{
    struct special_target state = {0};
    state.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!state.ready) return 82;
    DWORD tid = 0, exit_code = 0;
    HANDLE thread = CreateThread(NULL, 0, special_nonalertable_worker,
                                 &state, 0, &tid);
    if (!thread || !tid || WaitForSingleObject(state.ready, 5000) != WAIT_OBJECT_0)
        return 83;
    typedef DWORD (WINAPI *queue_apc2)(PAPCFUNC, HANDLE, ULONG_PTR, DWORD);
    union { FARPROC address; queue_apc2 call; } resolved = {
        .address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "QueueUserAPC2")
    };
    if (!resolved.call ||
        !resolved.call(special_secondary_user, thread, (ULONG_PTR)&state, 1) ||
        NtQueueApcThreadEx(thread, (HANDLE)(uintptr_t)1,
                           special_secondary_native, &state,
                           (void *)(uintptr_t)0x711u,
                           (void *)(uintptr_t)0x722u) != STATUS_OK)
        return 84;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x188u ||
        state.count != 2 || state.fifo != 2 || state.observed != tid ||
        !CloseHandle(thread) || !CloseHandle(state.ready))
        return 85;
    return 0;
}

/* Four concurrent PE Win32 threads independently consume two queued APCs,
 * preserving target identity and FIFO without signal-handler callbacks.
 * This exercises the production ordinary APC queue, NOT special APC. */
struct multi_apc_target {
    HANDLE ready;
    DWORD thread_id;
    DWORD user_seen, native_seen, calls, ordered;
};
static void CALLBACK multi_user_apc(ULONG_PTR argument)
{
    struct multi_apc_target *target = (struct multi_apc_target *)argument;
    if (!target) return;
    target->user_seen = GetCurrentThreadId();
    if (target->calls == 0u)
        target->ordered = 1u;
    ++target->calls;
}
static void NTAPI multi_native_apc(void *context, void *first, void *second)
{
    struct multi_apc_target *target = context;
    if (!target || first != (void *)(uintptr_t)0x515u ||
        second != (void *)(uintptr_t)0x616u) return;
    target->native_seen = GetCurrentThreadId();
    if (target->calls == 1u && target->ordered == 1u)
        target->ordered = 2u;
    ++target->calls;
}
static DWORD WINAPI multi_apc_worker(void *argument)
{
    struct multi_apc_target *target = argument;
    target->thread_id = GetCurrentThreadId();
    if (!SetEvent(target->ready))
        return 71;
    /* Windows may dispatch several queued user APCs in one alertable
     * wait; this runtime currently consumes one callback per wake. Do
     * not require exactly two wake-ups to prove FIFO and target identity. */
    for (unsigned wake = 0; wake < 2 && target->calls < 2u; ++wake)
        if (SleepEx(5000, TRUE) != WAIT_IO_COMPLETION)
            return 72;
    if (target->calls != 2u || target->ordered != 2u ||
        target->user_seen != target->thread_id ||
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
        NtQueueApcThreadEx(restricted, (HANDLE)(uintptr_t)1,
                           special_native_apc, NULL, NULL, NULL) != STATUS_ACCESS_DENIED ||
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
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0)
        return 16;
    if (!GetExitCodeThread(thread, &code))
        return 86;
    if (code != 0x144u)
        return code > 0 && code < 86u ? (int)code : 87;
    if (apc_count != 2) return 88;
    if (callback_ok != 2) return 89;
    if (callback_thread != tid) return 90;
    if (worker_id != tid) return 91;

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
    const int special = test_special_nonalertable();
    return special ? special : test_apc_concurrent_threads();
}
