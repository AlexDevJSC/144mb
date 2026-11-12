/* SPDX-License-Identifier: MIT */
/* A real freestanding Win64 PE checks guest thread APC delivery. */
#include <windows.h>
#include <stdint.h>

#define STATUS_OK ((LONG)0)
#define STATUS_ACCESS_DENIED ((LONG)0xc0000022u)

__declspec(dllimport) LONG NTAPI NtQueueApcThread(
    HANDLE, void (NTAPI *)(void *, void *, void *), void *, void *, void *);
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
    if (!CloseHandle(event))
        return 31;
    return 0x144;
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
        NtQueueApcThread(restricted, native_apc, NULL, NULL, NULL) != STATUS_ACCESS_DENIED)
        return 12;
    if (!CloseHandle(restricted))
        return 13;

    if (!QueueUserAPC(single_apc, thread, 0x143u) ||
        NtQueueApcThread(thread, native_apc, (void *)(uintptr_t)0x144u,
                         (void *)(uintptr_t)0x145u, (void *)(uintptr_t)0x146u) != STATUS_OK)
        return 14;
    if (NtQueueApcThread(thread, NULL, NULL, NULL, NULL) != STATUS_INVALID_PARAMETER ||
        ResumeThread(thread) != 1)
        return 15;
    DWORD code = 0;
    if (WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &code) || code != 0x144u ||
        apc_count != 2 || callback_ok != 2 || callback_thread != tid || worker_id != tid)
        return 16;

    if (QueueUserAPC(single_apc, thread, 0x143u) != 0)
        return 17;
    if (!CloseHandle(thread))
        return 18;
    return 0;
}
