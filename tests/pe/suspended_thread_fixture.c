/* SPDX-License-Identifier: MIT */
/* Build-generated Win64 PE: startup suspension is counted and rights-checked. */
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtResumeThread(HANDLE, ULONG *);

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
    return 0;
}
