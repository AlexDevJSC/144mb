/* SPDX-License-Identifier: MIT */
/* Build-generated Win64 PE: startup suspension is counted and rights-checked. */
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtResumeThread(HANDLE, ULONG *);
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
    return 0;
}
