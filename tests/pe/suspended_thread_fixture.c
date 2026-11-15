/* SPDX-License-Identifier: MIT */
/* Build-generated Win64 PE: startup suspension is counted and rights-checked. */
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtResumeThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtGetContextThread(HANDLE, PCONTEXT);
__declspec(dllimport) LONG NTAPI NtSetContextThread(HANDLE, const CONTEXT *);
__declspec(dllimport) LONG NTAPI NtCreateThreadEx(HANDLE *, ACCESS_MASK, void *,
    HANDLE, DWORD (WINAPI *)(void *), void *, ULONG, SIZE_T, SIZE_T, SIZE_T, void *);
__declspec(dllimport) LONG NTAPI NtTerminateThread(HANDLE, LONG);
__declspec(dllimport) LONG NTAPI NtSetInformationThread(HANDLE, ULONG, const void *, ULONG);

#define STATUS_ACCESS_DENIED ((LONG)0xc0000022u)
#define STATUS_THREAD_IS_TERMINATING ((LONG)0xc000004bu)
#define STATUS_SUSPEND_COUNT_EXCEEDED ((LONG)0xc000004au)
#define STATUS_NOT_SUPPORTED ((LONG)0xc00000bbu)

static volatile DWORD entered;
static volatile DWORD waiting;
static HANDLE release_running;
static HANDLE cooperative_ready, cooperative_never;
static volatile DWORD cooperative_delivered;
static void CALLBACK cooperative_apc(ULONG_PTR data)
{
    if (data == (ULONG_PTR)0x59u)
        cooperative_delivered = 1;
}
static DWORD WINAPI cooperative_worker(void *mode)
{
    if (!SetEvent(cooperative_ready))
        return 0x201u;
    const DWORD result = mode == NULL ? SleepEx(INFINITE, TRUE) :
        WaitForSingleObjectEx(cooperative_never, INFINITE, TRUE);
    return result == WAIT_IO_COMPLETION && cooperative_delivered == 1 ?
           0x202u : 0x203u;
}

static DWORD WINAPI worker(void *unused)
{
    (void)unused;
    entered = 1;
    return 0x144;
}

static DWORD WINAPI native_exit_worker(void *unused)
{
    (void)unused;
    NtTerminateThread(GetCurrentThread(), 0x7788u);
    return 0x1199u; /* must never run */
}

static DWORD WINAPI running_worker(void *unused)
{
    (void)unused;
    waiting = 1;
    return WaitForSingleObject(release_running, 5000) == WAIT_OBJECT_0 ? 0x155u : 0x166u;
}

int import_fixture_entry(void)
{
    DWORD thread_id = 0, exit_code = 0;
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, &thread_id);
    if (thread == NULL || thread_id == 0)
        return 10;
    if (entered != 0 || WaitForSingleObject(thread, 0) != WAIT_TIMEOUT)
        return 11;

    HANDLE query_only = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    ULONG previous = 999;
    if (query_only == NULL || NtSuspendThread(query_only, &previous) != STATUS_ACCESS_DENIED ||
        previous != 999 || !CloseHandle(query_only))
        return 12;
    HANDLE suspend_only = OpenThread(THREAD_SUSPEND_RESUME, FALSE, thread_id);
    exit_code = 0xabcdefu;
    SetLastError(0);
    if (suspend_only == NULL || GetExitCodeThread(suspend_only, &exit_code) ||
        GetLastError() != ERROR_ACCESS_DENIED || exit_code != 0xabcdefu ||
        !CloseHandle(suspend_only))
        return 30;
    if (NtSuspendThread(thread, &previous) != 0 || previous != 1)
        return 13;
    if (SuspendThread(thread) != 2)
        return 14;
    /* Windows has a 127-level maximum suspend count. We support that
     * bound only while the worker is still blocked at its startup gate. */
    for (DWORD count = 3; count < 127; ++count) {
        if (NtSuspendThread(thread, &previous) != 0 || previous != count)
            return 15;
    }
    previous = 999;
    if (NtSuspendThread(thread, &previous) != STATUS_SUSPEND_COUNT_EXCEEDED ||
        previous != 999 || entered != 0)
        return 16;
    for (DWORD remaining = 127; remaining != 0; --remaining) {
        if (remaining == 1 &&
            (entered != 0 || WaitForSingleObject(thread, 0) != WAIT_TIMEOUT))
            return 17;
        if (remaining & 1) {
            if (NtResumeThread(thread, &previous) != 0 || previous != remaining)
                return 18;
        } else if (ResumeThread(thread) != remaining) {
            return 19;
        }
    }
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 || entered != 1 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144)
        return 20;
    previous = 999;
    if (NtSuspendThread(thread, &previous) != STATUS_THREAD_IS_TERMINATING ||
        previous != 999 || !CloseHandle(thread))
        return 21;

    /* Asynchronous suspension of an executing Linux pthread is not
     * emulated. Both entry points must fail instead of claiming success. */
    release_running = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (release_running == NULL)
        return 22;
    thread = CreateThread(NULL, 0, running_worker, NULL, 0, NULL);
    if (thread == NULL)
        return 23;
    for (DWORD retry = 0; !waiting && retry < 1000; ++retry)
        Sleep(1);
    if (!waiting)
        return 24;
    SetLastError(0);
    if (SuspendThread(thread) != 0xffffffffu || GetLastError() != ERROR_NOT_SUPPORTED)
        return 25;
    previous = 999;
    if (NtSuspendThread(thread, &previous) != STATUS_NOT_SUPPORTED || previous != 999)
        return 26;
    if (!SetEvent(release_running) ||
        WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x155u ||
        !CloseHandle(thread) || !CloseHandle(release_running))
        return 27;

    SetLastError(0);
    if (ResumeThread(NULL) != 0xffffffffu || GetLastError() != ERROR_INVALID_HANDLE)
        return 28;
    if (CreateThread(NULL, 0, worker, NULL, 0x10000u, NULL) != NULL ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 29;

    /* NtCreateThreadEx shares the native TEB/handles/startup gate. */
    HANDLE native = NULL;
    SetLastError(0x778u);
    if (NtCreateThreadEx(&native, THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
                         NULL, GetCurrentProcess(), native_exit_worker, NULL,
                         1, 0, 0, 0, NULL) != 0 || native == NULL ||
        GetLastError() != 0x778u || WaitForSingleObject(native, 0) != WAIT_TIMEOUT)
        return 31;
    DWORD id = GetThreadId(native);
    HANDLE supervisor = OpenThread(THREAD_SUSPEND_RESUME, FALSE, id);
    SetLastError(0);
    if (supervisor == NULL || ResumeThread(native) != 0xffffffffu ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 32;
    if (ResumeThread(supervisor) != 1 || WaitForSingleObject(native, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(native, &exit_code) || exit_code != 0x7788u ||
        !CloseHandle(supervisor) || !CloseHandle(native))
        return 33;
    native = (HANDLE)(uintptr_t)0x10u;
    if (NtCreateThreadEx(&native, THREAD_ALL_ACCESS, NULL, GetCurrentProcess(),
                         native_exit_worker, NULL, 2, 0, 0, 0, NULL) != STATUS_NOT_SUPPORTED ||
        native != NULL)
        return 34;
    if (NtTerminateThread((HANDLE)(uintptr_t)0x123u, 0) != (LONG)0xc0000008u)
        return 35;

    /* Cancellation at CREATE_SUSPENDED is safe: no guest entry or TLS
     * attach callback may execute for the cancelled worker. */
    entered = 0;
    HANDLE aborting = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    if (aborting == NULL || entered != 0 ||
        !TerminateThread(aborting, 0x9911u) ||
        WaitForSingleObject(aborting, 5000) != WAIT_OBJECT_0 ||
        entered != 0 || !GetExitCodeThread(aborting, &exit_code) || exit_code != 0x9911u ||
        !CloseHandle(aborting))
        return 36;
    entered = 0;
    aborting = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    SetLastError(0x77bu);
    if (aborting == NULL || NtTerminateThread(aborting, 0x9922u) != 0 ||
        GetLastError() != 0x77bu ||
        WaitForSingleObject(aborting, 5000) != WAIT_OBJECT_0 ||
        entered != 0 || !GetExitCodeThread(aborting, &exit_code) || exit_code != 0x9922u ||
        !CloseHandle(aborting))
        return 37;

    /* Linux backs affinity with a real CPU set. The x64 processor-group
     * profile is deliberately bounded to masks representable in 64 bits. */
    DWORD_PTR process_mask = 0, system_mask = 0;
    SetLastError(0x756u);
    if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        if (process_mask == 0 || (process_mask & ~system_mask) != 0 ||
            GetLastError() != 0x756u ||
            SetThreadAffinityMask(GetCurrentThread(), process_mask) != process_mask)
            return 38;
        SetLastError(0x757u);
        if (NtSetInformationThread(GetCurrentThread(), 4, &process_mask,
                                   sizeof(process_mask)) != 0 ||
            GetLastError() != 0x757u)
            return 39;
    } else if (GetLastError() != ERROR_NOT_SUPPORTED) {
        return 40;
    }
    SetLastError(0x758u);
    if (NtSetInformationThread(GetCurrentThread(), 4, NULL,
                                sizeof(DWORD_PTR)) != (LONG)0xc0000005u ||
        NtSetInformationThread(GetCurrentThread(), 4, &process_mask, 1) !=
                                (LONG)0xc0000004u ||
        NtSetInformationThread(GetCurrentThread(), 41, &process_mask,
                                sizeof(DWORD_PTR)) != (LONG)0xc0000003u ||
        GetLastError() != 0x758u)
        return 41;
    SetLastError(0);
    if (GetProcessAffinityMask(GetCurrentProcess(), NULL, &system_mask) ||
        GetLastError() != ERROR_INVALID_PARAMETER ||
        SetThreadAffinityMask(GetCurrentThread(), 0) != 0 ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 42;
    /* Native-backed priority controls: changing nice=2 must be visible
     * in the test guest, not simply stored in a fake Win32 field. */
    FILETIME created = {0}, exited = {0}, kernel_time = {0}, user_time = {0};
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel_time, &user_time) ||
        (created.dwLowDateTime == 0 && created.dwHighDateTime == 0) ||
        exited.dwLowDateTime || exited.dwHighDateTime ||
        !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel_time, &user_time) ||
        exited.dwLowDateTime || exited.dwHighDateTime)
        return 47;

    HANDLE low = CreateThread(NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL);
    if (low == NULL || GetThreadPriority(low) != THREAD_PRIORITY_NORMAL ||
        !SetThreadPriority(low, THREAD_PRIORITY_BELOW_NORMAL) ||
        GetThreadPriority(low) != THREAD_PRIORITY_BELOW_NORMAL)
        return 43;
    if (!GetThreadTimes(low, &created, &exited, &kernel_time, &user_time) ||
        (created.dwLowDateTime == 0 && created.dwHighDateTime == 0) ||
        exited.dwLowDateTime || exited.dwHighDateTime)
        return 48;
    DWORD low_id = GetThreadId(low);
    HANDLE limited = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, low_id);
    SetLastError(0);
    if (limited == NULL || SetThreadPriority(limited, THREAD_PRIORITY_NORMAL) ||
        GetLastError() != ERROR_ACCESS_DENIED ||
        GetThreadPriority(limited) != THREAD_PRIORITY_BELOW_NORMAL ||
        !CloseHandle(limited))
        return 44;
    SetLastError(0);
    if (SetThreadPriority(low, 12) || GetLastError() != ERROR_INVALID_PARAMETER ||
        ResumeThread(low) != 1 || WaitForSingleObject(low, 5000) != WAIT_OBJECT_0)
        return 45;
    if (!GetThreadTimes(low, &created, &exited, &kernel_time, &user_time) ||
        (exited.dwLowDateTime == 0 && exited.dwHighDateTime == 0) ||
        !CloseHandle(low))
        return 49;
    SetLastError(0);
    if (GetThreadPriority(NULL) != THREAD_PRIORITY_ERROR_RETURN ||
        GetLastError() != ERROR_INVALID_HANDLE)
        return 46;
    /* Cross-process thread injection is deliberately unsupported, but
     * Windows programs can use the same API with a current-process HANDLE. */
    HANDLE self = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION,
                              FALSE, GetCurrentProcessId());
    HANDLE no_create = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                   FALSE, GetCurrentProcessId());
    if (self == NULL || no_create == NULL)
        return 50;
    entered = 0;
    DWORD remote_id = 0;
    HANDLE remote = CreateRemoteThread(self, NULL, 0, worker, NULL,
                                       CREATE_SUSPENDED, &remote_id);
    if (remote == NULL || remote_id == 0 || entered != 0 ||
        ResumeThread(remote) != 1 || WaitForSingleObject(remote, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(remote, &exit_code) || exit_code != 0x144u ||
        !CloseHandle(remote))
        return 51;
    SetLastError(0);
    if (CreateRemoteThread(no_create, NULL, 0, worker, NULL, 0, NULL) != NULL ||
        GetLastError() != ERROR_ACCESS_DENIED)
        return 52;
    remote = NULL;
    SetLastError(0x788u);
    if (NtCreateThreadEx(&remote, THREAD_ALL_ACCESS, NULL, self, worker, NULL,
                         1, 0, 0, 0, NULL) != 0 || remote == NULL ||
        GetLastError() != 0x788u ||
        ResumeThread(remote) != 1 ||
        WaitForSingleObject(remote, 5000) != WAIT_OBJECT_0 ||
        !CloseHandle(remote))
        return 53;
    if (NtCreateThreadEx(&remote, THREAD_ALL_ACCESS, NULL, no_create, worker, NULL,
                         1, 0, 0, 0, NULL) != STATUS_ACCESS_DENIED)
        return 54;
    typedef HANDLE (WINAPI *remote_ex_proc)(HANDLE, const void *, SIZE_T,
                                            LPTHREAD_START_ROUTINE, void *, DWORD,
                                            const void *, DWORD *);
    FARPROC raw_remote_ex = GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                           "CreateRemoteThreadEx");
    remote_ex_proc remote_ex = NULL;
    __builtin_memcpy(&remote_ex, &raw_remote_ex, sizeof(remote_ex));
    if (remote_ex == NULL)
        return 55;
    entered = 0;
    remote = remote_ex(self, NULL, 0, worker, NULL, CREATE_SUSPENDED, NULL, &remote_id);
    if (remote == NULL || entered != 0 ||
        ResumeThread(remote) != 1 || WaitForSingleObject(remote, 5000) != WAIT_OBJECT_0 ||
        !CloseHandle(remote))
        return 56;
    SetLastError(0);
    if (remote_ex(self, NULL, 0, worker, NULL, 0, (const void *)(uintptr_t)1, NULL) != NULL ||
        GetLastError() != ERROR_NOT_SUPPORTED ||
        !CloseHandle(self) || !CloseHandle(no_create))
        return 57;

    /* Running Win64 threads MAY be suspended after acknowledging a safe
     * alertable-wait checkpoint. This is not asynchronous instruction-level
     * suspension: an arbitrary busy loop or non-alertable wait is still
     * explicitly unsupported, as checked above. */
    cooperative_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    cooperative_never = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!cooperative_ready || !cooperative_never)
        return 60;
    for (unsigned int mode = 0; mode < 2; ++mode) {
        cooperative_delivered = 0;
        if (!ResetEvent(cooperative_ready))
            return 61;
        HANDLE parked = CreateThread(NULL, 0, cooperative_worker,
                                     (void *)(uintptr_t)mode, 0, NULL);
        if (parked == NULL ||
            WaitForSingleObject(cooperative_ready, 5000) != WAIT_OBJECT_0)
            return 62;
        ULONG before = 999;
        SetLastError(0x1234u);
        if (NtSuspendThread(parked, &before) != 0 || before != 0 ||
            GetLastError() != 0x1234u ||
            WaitForSingleObject(parked, 0) != WAIT_TIMEOUT)
            return 63;

        /* An acknowledged runtime checkpoint has a real CPU snapshot.
         * The returned RIP belongs to the guest thread while parked in
         * native wait code, not a fabricated PE instruction address. */
        CONTEXT captured;
        captured.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        SetLastError(0x1234u);
        if (!GetThreadContext(parked, &captured) ||
            GetLastError() != 0x1234u ||
            captured.Rip == 0 || captured.Rsp == 0 ||
            captured.ContextFlags != (CONTEXT_CONTROL | CONTEXT_INTEGER))
            return 71;
        CONTEXT native_ctx;
        native_ctx.ContextFlags = CONTEXT_CONTROL;
        if (NtGetContextThread(parked, &native_ctx) != 0 ||
            native_ctx.Rip != captured.Rip ||
            native_ctx.Rsp != captured.Rsp ||
            native_ctx.ContextFlags != CONTEXT_CONTROL)
            return 72;
        HANDLE no_context = OpenThread(THREAD_QUERY_LIMITED_INFORMATION,
                                      FALSE, GetThreadId(parked));
        if (!no_context)
            return 73;
        native_ctx.ContextFlags = CONTEXT_CONTROL;
        SetLastError(0x1235u);
        if (GetThreadContext(no_context, &native_ctx) ||
            GetLastError() != ERROR_ACCESS_DENIED ||
            !CloseHandle(no_context))
            return 74;
        SetLastError(0x1234u);
        if (NtSetContextThread(parked, &captured) != STATUS_NOT_SUPPORTED ||
            GetLastError() != 0x1234u)
            return 75;
        SetLastError(0);
        if (SetThreadContext(parked, &captured) ||
            GetLastError() != ERROR_NOT_SUPPORTED)
            return 76;

        if (SuspendThread(parked) != 1)
            return 64;
        if (!QueueUserAPC(cooperative_apc, parked, 0x59u))
            return 65;
        Sleep(20);
        if (cooperative_delivered != 0)
            return 66;
        if (ResumeThread(parked) != 2)
            return 67;
        Sleep(20);
        if (cooperative_delivered != 0 || WaitForSingleObject(parked, 0) != WAIT_TIMEOUT)
            return 68;
        if (ResumeThread(parked) != 1 ||
            WaitForSingleObject(parked, 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(parked, &exit_code) || exit_code != 0x202u ||
            !CloseHandle(parked))
            return 69;
    }
    /* Terminating a running thread is safe only after it has already
     * acknowledged a pause at a runtime checkpoint. The target must
     * unwind its NT wait references and exit on its own stack, without
     * pthread_cancel or an asynchronous host signal. */
    for (unsigned int mode = 0; mode < 2; ++mode) {
        if (!ResetEvent(cooperative_ready))
            return 77;
        cooperative_delivered = 0;
        HANDLE parked = CreateThread(NULL, 0, cooperative_worker,
                                     (void *)(uintptr_t)mode, 0, NULL);
        if (parked == NULL ||
            WaitForSingleObject(cooperative_ready, 5000) != WAIT_OBJECT_0)
            return 78;
        ULONG previous = 777;
        if (NtSuspendThread(parked, &previous) != 0 || previous != 0)
            return 79;
        if (mode == 0) {
            SetLastError(0x52au);
            if (NtTerminateThread(parked, 0x9966u) != 0 ||
                GetLastError() != 0x52au)
                return 80;
        } else if (!TerminateThread(parked, 0x9966u)) {
            return 81;
        }
        if (WaitForSingleObject(parked, 5000) != WAIT_OBJECT_0 ||
            !GetExitCodeThread(parked, &exit_code) || exit_code != 0x9966u ||
            cooperative_delivered != 0 || !CloseHandle(parked))
            return 82;
    }
    if (!CloseHandle(cooperative_ready) || !CloseHandle(cooperative_never))
        return 70;
    return 0;
}
