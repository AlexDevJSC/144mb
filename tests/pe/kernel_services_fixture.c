/* SPDX-License-Identifier: MIT */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>

__declspec(dllimport) NTSTATUS NTAPI NtCreateEvent(HANDLE *, ACCESS_MASK, OBJECT_ATTRIBUTES *, ULONG, BOOLEAN);
__declspec(dllimport) NTSTATUS NTAPI NtDuplicateObject(HANDLE, HANDLE, HANDLE, HANDLE *, ACCESS_MASK,
                                                        ULONG, ULONG);
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

    HANDLE manual_alias = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), manual, GetCurrentProcess(), &manual_alias,
                         0, FALSE, DUPLICATE_SAME_ACCESS) || !manual_alias || manual_alias == manual ||
        !SetEvent(manual_alias) || WaitForSingleObject(manual, 0) != WAIT_OBJECT_0 ||
        !ResetEvent(manual))
        return 28;
    if (!CloseHandle(manual_alias))
        return 29;

    HANDLE restricted = NULL;
    if (NtCreateEvent(&restricted, SYNCHRONIZE, NULL, 0, TRUE) != STATUS_SUCCESS)
        return 23;
    SetLastError(0x405u);
    previous = -1;
    if (NtSetEvent(restricted, &previous) != STATUS_ACCESS_DENIED || previous != -1 || GetLastError() != 0x405u ||
        SetEvent(restricted) || GetLastError() != ERROR_ACCESS_DENIED ||
        NtWaitForSingleObject(restricted, FALSE, &poll) != STATUS_SUCCESS)
        return 24;
    HANDLE restricted_alias = NULL;
    if (DuplicateHandle(GetCurrentProcess(), restricted, GetCurrentProcess(), &restricted_alias,
                        EVENT_MODIFY_STATE, FALSE, 0) || GetLastError() != ERROR_ACCESS_DENIED ||
        restricted_alias != NULL)
        return 30;
    if (NtDuplicateObject(GetCurrentProcess(), restricted, GetCurrentProcess(), &restricted_alias,
                          0, 2, DUPLICATE_SAME_ACCESS) != STATUS_SUCCESS || !restricted_alias)
        return 31;
    DWORD alias_flags = 0;
    if (!GetHandleInformation(restricted_alias, &alias_flags) ||
        (alias_flags & HANDLE_FLAG_INHERIT) == 0 || !CloseHandle(restricted) ||
        NtWaitForSingleObject(restricted_alias, FALSE, &poll) != STATUS_SUCCESS ||
        NtSetEvent(restricted_alias, NULL) != STATUS_ACCESS_DENIED ||
        !CloseHandle(restricted_alias))
        return 32;

    SetLastError(0x406u);
    HANDLE thread = CreateThread(NULL, 0, signal_worker, manual, 0, NULL);
    HANDLE thread_alias = NULL;
    if (!thread || !DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(),
                                     &thread_alias, 0, FALSE, DUPLICATE_SAME_ACCESS))
        return 33;
    HANDLE pending[] = {manual, thread};
    DWORD exit_code = 0;
    if (NtWaitForMultipleObjects(2, pending, 0, FALSE, &one_second) != STATUS_SUCCESS ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144u || GetLastError() != 0x406u)
        return 25;
    if (NtClose(thread) != STATUS_SUCCESS ||
        !GetExitCodeThread(thread_alias, &exit_code) || exit_code != 0x144u ||
        WaitForSingleObject(thread_alias, 0) != WAIT_OBJECT_0)
        return 34;
    if (NtClose(thread_alias) != STATUS_SUCCESS ||
        NtClose(manual) != STATUS_SUCCESS || !CloseHandle(automatic) ||
        NtClose(manual) != STATUS_INVALID_HANDLE ||
        NtWaitForSingleObject(manual, FALSE, &poll) != STATUS_INVALID_HANDLE)
        return 26;

    HANDLE invalid = (HANDLE)(ULONG_PTR)0x77u;
    attributes.Length = 0;
    return NtCreateEvent(&invalid, SYNCHRONIZE, &attributes, 0, FALSE) == STATUS_INVALID_PARAMETER &&
           invalid == (HANDLE)(ULONG_PTR)0x77u ? 0 : 27;
}

static int test_duplicated_file(void)
{
    /* dup(2) must preserve the shared file pointer when the original
     * guest HANDLE closes. Neither handle escapes the sandbox. */
    HANDLE file = CreateFileW(L"C:\\nt-duplicate-test.bin", GENERIC_READ | GENERIC_WRITE,
                              0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 40;
    HANDLE copy = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &copy,
                         0, FALSE, DUPLICATE_SAME_ACCESS) || !copy || copy == file)
        return 41;
    if (!CloseHandle(file))
        return 42;
    const char value[] = "OK";
    char result[sizeof(value)] = {0};
    DWORD count = 0;
    LARGE_INTEGER beginning = {.QuadPart = 0};
    if (!WriteFile(copy, value, sizeof(value), &count, NULL) || count != sizeof(value) ||
        !SetFilePointerEx(copy, beginning, NULL, FILE_BEGIN) ||
        !ReadFile(copy, result, sizeof(result), &count, NULL) ||
        count != sizeof(result) || result[0] != 'O' || result[1] != 'K' || result[2] != 0)
        return 43;
    if (!CloseHandle(copy) || !DeleteFileW(L"C:\\nt-duplicate-test.bin"))
        return 44;
    return 0;
}

int import_fixture_entry(void)
{
    int status = test_clocks_and_errors();
    if (!status)
        status = test_shared_objects();
    return status ? status : test_duplicated_file();
}
