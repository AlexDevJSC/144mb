/* SPDX-License-Identifier: MIT */
/* A real PE32+ busy loop: unlike cooperative waits this requires a kernel
 * ptrace stop of the actual executing worker, followed by GETREGSET,
 * SETREGSET and a counted resume through Win32/NTDLL exports. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

/* Freestanding fixture with -nostdlib. GCC may lower CONTEXT structure
 * copies/zero-initialization to these operations even without CRT startup.
 * Use volatile byte writes so the optimizer cannot rediscover a recursive
 * memset/memcpy call. Not used as runtime Windows implementations. */
void *memset(void *out, int value, size_t size)
{
    volatile unsigned char *write = (volatile unsigned char *)out;
    for (size_t i = 0; i < size; ++i)
        write[i] = (unsigned char)value;
    return out;
}
void *memcpy(void *out, const void *input, size_t size)
{
    volatile unsigned char *write = (volatile unsigned char *)out;
    const volatile unsigned char *read =
        (const volatile unsigned char *)input;
    for (size_t i = 0; i < size; ++i)
        write[i] = read[i];
    return out;
}

__declspec(dllimport) LONG NTAPI NtSuspendThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtResumeThread(HANDLE, ULONG *);
__declspec(dllimport) LONG NTAPI NtGetContextThread(HANDLE, PCONTEXT);
__declspec(dllimport) LONG NTAPI NtSetContextThread(HANDLE, const CONTEXT *);

/* Actual PE32+ code from a companion DLL statically imported at startup.
 * This must use the parent-owned, pinned DLL manifest instead of the
 * executable's primary .text range. No runtime-native function pointers. */
__declspec(dllimport) int WINAPI GuestHotLoop(volatile LONG *, const volatile LONG *);
static volatile LONG dll_ticks, dll_release;
static DWORD WINAPI dll_worker(void *unused)
{
    (void)unused;
    return (DWORD)GuestHotLoop(&dll_ticks, &dll_release);
}


static volatile LONG ticks;
static volatile LONG go;
static volatile LONG safe_import_ticks;
static DWORD WINAPI safe_import_worker(void *arg)
{
    (void)arg;
    for (;;) {
        __atomic_add_fetch(&safe_import_ticks, 1, __ATOMIC_RELAXED);
        Sleep(0); /* trusted Win32 import-return checkpoint */
    }
    return 0; /* required by the DWORD WINAPI signature; never reached */
}
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
    /* The real PE import now exercises the separately authorised legacy
     * FXSAVE write, kernel readback and restoration on a STOPPED worker.
     * It cannot write RIP/RSP/GS or any extended XSTATE group. */
    CONTEXT floating = {0};
    floating.ContextFlags = CONTEXT_FLOATING_POINT;
    if (NtGetContextThread(thread, &floating) != 0 ||
        floating.MxCsr == 0 ||
        floating.ContextFlags != CONTEXT_FLOATING_POINT)
        return 28;
    const DWORD old_mxcsr = floating.MxCsr;
    floating.MxCsr ^= 0x2000u;
    floating.FltSave.MxCsr = floating.MxCsr;
    if (NtSetContextThread(thread, &floating) != 0)
        return 29;
    CONTEXT actual_fp = {0};
    actual_fp.ContextFlags = CONTEXT_FLOATING_POINT;
    if (NtGetContextThread(thread, &actual_fp) != 0 ||
        actual_fp.MxCsr != floating.MxCsr)
        return 35;
    floating.MxCsr = old_mxcsr;
    floating.FltSave.MxCsr = old_mxcsr;
    if (NtSetContextThread(thread, &floating) != 0)
        return 36;
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
    /* Exhaust the NT maximum of 127 nested suspensions without ever
     * allowing guest instructions to advance. Each previous count must
     * reflect the live Linux ptrace state, not a fabricated counter. */
    for (ULONG n = 1; n < 127; ++n) {
        ULONG old = 919;
        if (NtSuspendThread(thread, &old) != 0 || old != n)
            return 24;
    }
    previous = 919;
    if (NtSuspendThread(thread, &previous) != (LONG)0xc000004au ||
        previous != 919 || ticks != before)
        return 25;
    for (ULONG n = 127; n > 1; --n) {
        ULONG old = 919;
        if (NtResumeThread(thread, &old) != 0 || old != n)
            return 26;
    }
    if (ticks != before)
        return 27;
    if (SuspendThread(thread) != 1) return 18;
    if (ResumeThread(thread) != 2) return 19;
    Sleep(30);
    if (ticks != before) return 20;
    if (NtResumeThread(thread, &previous) != 0 || previous != 1)
        return 21;
    Sleep(30);
    if (ticks <= before) return 22;
    /* No import boundary in the target PE busy loop. TerminateThread
     * must time out WITHOUT cancelling its Linux pthread or leaving a
     * delayed kill request that would run at a later unrelated import. */
    if (TerminateThread(thread, 0xdead)) return 46;
    if (ticks <= before) return 47;
    go = 1;
    if (WaitForSingleObject(thread, 4000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code != 0x144 ||
        !CloseHandle(thread))
        return 23;

    /* Distinct real DLL .text stop: the PE executable is not on the
     * whitelist for this instruction pointer. The parent must have
     * captured its pinned initial import before fork. */
    dll_ticks = dll_release = 0;
    HANDLE dll_thread = CreateThread(NULL, 0, dll_worker, NULL, 0, &id);
    if (!dll_thread || !id) return 38;
    for (unsigned spin = 0; !dll_ticks && spin < 1000000; ++spin) Sleep(0);
    if (!dll_ticks) return 39;
    if (SuspendThread(dll_thread) != 0) return 40;
    const LONG frozen_dll = dll_ticks;
    Sleep(35);
    if (dll_ticks != frozen_dll) return 41;
    CONTEXT from_dll = {0};
    from_dll.ContextFlags = CONTEXT_CONTROL;
    if (NtGetContextThread(dll_thread, &from_dll) != 0 || from_dll.Rip == 0)
        return 42;
    int (WINAPI *export_address)(volatile LONG *, const volatile LONG *) =
        GuestHotLoop;
    uintptr_t trusted_dll_entry = 0;
    memcpy(&trusted_dll_entry, &export_address, sizeof(trusted_dll_entry));
    if (from_dll.Rip < trusted_dll_entry ||
        from_dll.Rip >= trusted_dll_entry + 4096)
        return 43; /* prove RIP is in the actual guest DLL, not the EXE */
    if (ResumeThread(dll_thread) != 1) return 44;
    dll_release = 1;
    if (WaitForSingleObject(dll_thread, 4000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(dll_thread, &exit_code) || exit_code != 0x177 ||
        !CloseHandle(dll_thread))
        return 45;
    /* Positive: a local PE worker repeatedly crosses an import-return
     * boundary with all host mutexes already released. Only the worker
     * itself may ACK and ExitThread. */
    safe_import_ticks = 0;
    HANDLE safe_thread = CreateThread(NULL, 0, safe_import_worker,
                                      NULL, 0, &id);
    if (!safe_thread || !id) return 48;
    for (unsigned spin = 0; !safe_import_ticks && spin < 1000000; ++spin)
        Sleep(0);
    if (!safe_import_ticks) return 49;
    if (!TerminateThread(safe_thread, 0x178)) return 50;
    if (WaitForSingleObject(safe_thread, 4000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(safe_thread, &exit_code) ||
        exit_code != 0x178 || !CloseHandle(safe_thread))
        return 51;
    return 0;
}
