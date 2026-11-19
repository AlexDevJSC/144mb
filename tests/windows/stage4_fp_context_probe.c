/* SPDX-License-Identifier: MIT */
/* Native Windows NT kernel reference (MSVC), not a Wine or NT144 shim.
 * Records observable Get/SetThreadContext legacy FXSAVE behavior on an
 * actual suspended thread, without claiming PE/ELF or XSAVE equivalence. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static DWORD WINAPI short_worker(LPVOID argument)
{
    (void)argument;
    return 0;
}

static const char *yesno(BOOL ok) { return ok ? "PASS" : "FAIL"; }

int main(void)
{
#if !defined(_M_X64)
#error This reference is only for the AMD64 Windows CONTEXT layout
#endif
    HANDLE thread = CreateThread(NULL, 0, short_worker, NULL,
                                 CREATE_SUSPENDED, NULL);
    BOOL captured = FALSE;
    BOOL changed = FALSE;
    BOOL roundtrip = FALSE;
    BOOL restored = FALSE;
    BOOL joined = FALSE;
    DWORD get_error = 0, set_error = 0;
    if (thread != NULL) {
        __declspec(align(16)) CONTEXT before;
        __declspec(align(16)) CONTEXT after;
        __declspec(align(16)) CONTEXT final;
        ZeroMemory(&before, sizeof(before));
        ZeroMemory(&after, sizeof(after));
        ZeroMemory(&final, sizeof(final));
        before.ContextFlags = CONTEXT_FLOATING_POINT;
        captured = GetThreadContext(thread, &before);
        if (!captured)
            get_error = GetLastError();
        if (captured) {
            __declspec(align(16)) CONTEXT desired = before;
            desired.ContextFlags = CONTEXT_FLOATING_POINT;
            desired.MxCsr ^= 0x2000u; /* SSE rounding mode; legal mask */
            desired.FltSave.MxCsr = desired.MxCsr;
            changed = SetThreadContext(thread, &desired);
            if (!changed)
                set_error = GetLastError();
            if (changed) {
                after.ContextFlags = CONTEXT_FLOATING_POINT;
                roundtrip = GetThreadContext(thread, &after) &&
                            after.MxCsr == desired.MxCsr;
                /* Restore the kernel's original state BEFORE resuming. */
                restored = SetThreadContext(thread, &before);
                if (restored) {
                    final.ContextFlags = CONTEXT_FLOATING_POINT;
                    restored = GetThreadContext(thread, &final) &&
                               final.MxCsr == before.MxCsr;
                }
            }
        }
        if (ResumeThread(thread) != (DWORD)-1) {
            joined = WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0;
        }
        CloseHandle(thread);
    }
    puts("SCHEMA=ntnative.stage4.fp-context.v1");
    puts("PLATFORM=WINDOWS_NATIVE");
    puts("EXECUTION=HOSTED_OR_PHYSICAL_UNKNOWN");
    printf("FP_CAPTURE=%s\n", yesno(captured));
    printf("FP_WRITE=%s\n", yesno(changed));
    printf("MXCSR_ROUNDTRIP=%s\n", yesno(roundtrip));
    printf("FP_RESTORE=%s\n", yesno(restored));
    printf("THREAD_EXIT=%s\n", yesno(joined));
    printf("GET_ERROR=%lu\n", (unsigned long)get_error);
    printf("SET_ERROR=%lu\n", (unsigned long)set_error);
    puts("NT_CONTEXT_PARITY_VERIFIED=NO");
    /* An unsupported native contract is valuable evidence: do not make
     * the Windows reference collection itself fail and hide the error. */
    return 0;
}
