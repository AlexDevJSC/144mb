/* SPDX-License-Identifier: MIT */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

__declspec(dllimport) NTSTATUS NTAPI NtCreateEvent(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, ULONG, BOOLEAN);
__declspec(dllimport) NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);
__declspec(dllimport) NTSTATUS NTAPI ZwResetEvent(HANDLE, LONG *);
__declspec(dllimport) NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG, const HANDLE *, ULONG, BOOLEAN,
                                                           const LARGE_INTEGER *);
__declspec(dllimport) NTSTATUS NTAPI NtDelayExecution(BOOLEAN, const LARGE_INTEGER *);
__declspec(dllimport) ULONG NTAPI RtlGetLastWin32Error(void);
__declspec(dllimport) void NTAPI RtlSetLastWin32Error(ULONG);

static int test_clocks_and_errors(void)
{
    LARGE_INTEGER before, after;
    const LARGE_INTEGER interval = {.QuadPart = -20000};
    FILETIME filetime;
    SetLastError(0x401u);
    if (NtQuerySystemTime(&before) != STATUS_SUCCESS || before.QuadPart <= 116444736000000000LL)
        return 10;
    GetSystemTimeAsFileTime(&filetime);
    if (NtQuerySystemTime(&after) != STATUS_SUCCESS)
        return 11;
    const ULONGLONG value = ((ULONGLONG)filetime.dwHighDateTime << 32) | filetime.dwLowDateTime;
    if (value < (ULONGLONG)before.QuadPart || value > (ULONGLONG)after.QuadPart ||
        NtQuerySystemTime(NULL) != STATUS_ACCESS_VIOLATION || RtlGetLastWin32Error() != 0x401u)
        return 12;
    if (NtDelayExecution(FALSE, &interval) != STATUS_SUCCESS || NtDelayExecution(FALSE, NULL) != STATUS_ACCESS_VIOLATION ||
        GetLastError() != 0x401u)
        return 13;

    RtlSetLastWin32Error(0x402u);
    ULONG error;
    __asm__ volatile("movl %%gs:0x68, %0" : "=r"(error));
    if (error != 0x402u || GetLastError() != error)
        return 14;
    __asm__ volatile("movl %0, %%gs:0x68" : : "r"(0x403u) : "memory");
    return RtlGetLastWin32Error() == 0x403u ? 0 : 15;
}

static DWORD WINAPI signal_worker(void *event)
{
    const LARGE_INTEGER interval = {.QuadPart = -30000};
    if (GetLastError() != 0 || NtDelayExecution(FALSE, &interval) != STATUS_SUCCESS)
        return 1;
    SetLastError(0x404u);
    return NtSetEvent(event, NULL) == STATUS_SUCCESS ? 0x144u : 2;
}

static int test_shared_objects(void)
{
    LARGE_INTEGER poll = {.QuadPart = 0};
    const LARGE_INTEGER one_second = {.QuadPart = -10000000};
    OBJECT_ATTRIBUTES attributes = {.Length = sizeof(attributes)};
    HANDLE manual = NULL;
    if (NtCreateEvent(&manual, SYNCHRONIZE | EVENT_MODIFY_STATE, &attributes, 0, FALSE) != STATUS_SUCCESS ||
        !manual)
        return 20;
    HANDLE automatic = CreateEventW(NULL, FALSE, TRUE, NULL);
    HANDLE both[] = {manual, automatic};
    if (!automatic || NtWaitForMultipleObjects(2, both, 0, FALSE, &poll) != STATUS_TIMEOUT ||
        NtWaitForMultipleObjects(2, both, 1, FALSE, &poll) != STATUS_WAIT_1 ||
        WaitForSingleObject(automatic, 0) != WAIT_TIMEOUT)
        return 21;
    LONG previous = -1;
    if (NtSetEvent(manual, &previous) != STATUS_SUCCESS || previous != 0 || !SetEvent(automatic) ||
        WaitForMultipleObjects(2, both, TRUE, 0) != WAIT_OBJECT_0 ||
        WaitForSingleObject(automatic, 0) != WAIT_TIMEOUT || NtWaitForSingleObject(manual, FALSE, &poll) != STATUS_SUCCESS ||
        ZwResetEvent(manual, &previous) != STATUS_SUCCESS || previous != 1)
        return 22;

    HANDLE restricted = NULL;
    if (NtCreateEvent(&restricted, SYNCHRONIZE, NULL, 0, TRUE) != STATUS_SUCCESS)
        return 23;
    SetLastError(0x405u);
    previous = -1;
    if (NtSetEvent(restricted, &previous) != STATUS_ACCESS_DENIED || previous != -1 || GetLastError() != 0x405u ||
        SetEvent(restricted) || GetLastError() != ERROR_ACCESS_DENIED ||
        NtWaitForSingleObject(restricted, FALSE, &poll) != STATUS_SUCCESS || !CloseHandle(restricted))
        return 24;

    SetLastError(0x406u);
    HANDLE thread = CreateThread(NULL, 0, signal_worker, manual, 0, NULL);
    HANDLE pending[] = {manual, thread};
    DWORD exit_code = 0;
    if (!thread || NtWaitForMultipleObjects(2, pending, 0, FALSE, &one_second) != STATUS_SUCCESS ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144u || GetLastError() != 0x406u)
        return 25;
    if (NtClose(thread) != STATUS_SUCCESS || NtClose(manual) != STATUS_SUCCESS || !CloseHandle(automatic) ||
        NtClose(manual) != STATUS_INVALID_HANDLE || NtWaitForSingleObject(manual, FALSE, &poll) != STATUS_INVALID_HANDLE)
        return 26;

    HANDLE invalid = (HANDLE)(ULONG_PTR)0x77u;
    attributes.Length = 0;
    return NtCreateEvent(&invalid, SYNCHRONIZE, &attributes, 0, FALSE) == STATUS_INVALID_PARAMETER &&
           invalid == (HANDLE)(ULONG_PTR)0x77u ? 0 : 27;
}

int import_fixture_entry(void)
{
    int status = test_clocks_and_errors();
    return status ? status : test_shared_objects();
}
