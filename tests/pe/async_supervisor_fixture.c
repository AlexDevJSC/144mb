/* SPDX-License-Identifier: MIT */
/* A real PE32+ busy loop: unlike cooperative waits this requires a kernel
 * ptrace stop of the actual executing worker, followed by GETREGSET,
 * SETREGSET and a counted resume through Win32/NTDLL exports. */
#include <windows.h>
#include <stdint.h>

__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtResumeThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtGetContextThread(HANDLE, PCONTEXT);
__declspec(dllimport) LONG NTAPI NtSetContextThread(HANDLE, const CONTEXT *);

static volatile LONG ticks;
static volatile LONG go;
static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    while (!go)
        __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
    return 0x144;
}
int import_fixture_entry(void)
{
    DWORD id = 0, exit_code = 0;
    HANDLE thread = CreateThread(NULL, 0, worker, NULL, 0, &id);
    if (!thread || !id) return 10;
    for (unsigned i = 0; !ticks && i < 1000000; ++i) Sleep(0);
    if (!ticks) return 11;
    ULONG previous = 919;
    if (NtSuspendThread(thread, &previous) != 0 || previous != 0)
        return 12;
    LONG before = ticks;
    Sleep(50);
    if (ticks != before) return 13;
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (NtGetContextThread(thread, &ctx) != 0 ||
        ctx.Rip == 0 || ctx.Rsp == 0 ||
        ctx.ContextFlags != (CONTEXT_CONTROL | CONTEXT_INTEGER))
        return 14;
    /* Verified integer update; original value restored while still
     * suspended, so the guest continues with original observable state. */
    CONTEXT integer = ctx;
    integer.ContextFlags = CONTEXT_INTEGER;
    const DWORD64 saved = integer.Rax;
    integer.Rax = UINT64_C(0x1122334455667788);
    if (NtSetContextThread(thread, &integer) != 0)
        return 15;
    CONTEXT readback = {0};
    readback.ContextFlags = CONTEXT_INTEGER;
    if (NtGetContextThread(thread, &readback) != 0 ||
        readback.Rax != integer.Rax)
        return 16;
    integer.Rax = saved;
    if (NtSetContextThread(thread, &integer) != 0)
        return 17;
    if (SuspendThread(thread) != 1) return 18;
    if (ResumeThread(thread) != 2) return 19;
    Sleep(30);
    if (ticks != before) return 20;
    if (NtResumeThread(thread, &previous) != 0 || previous != 1)
        return 21;
    Sleep(30);
    if (ticks <= before) return 22;
    go = 1;
    if (WaitForSingleObject(thread, 4000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144 ||
        !CloseHandle(thread))
        return 23;
    return 0;
}
