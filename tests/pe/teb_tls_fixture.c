/* SPDX-License-Identifier: MIT */
/* Freestanding PE32+ regression: Windows x64 TEB dynamic TLS slots. */
#include <windows.h>
#include <stdint.h>

static uintptr_t teb_slot(DWORD index)
{
    uintptr_t contents;
    const uintptr_t offset = UINT64_C(0x1480) + (uintptr_t)index * sizeof(contents);
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(contents) : "r"(offset));
    return contents;
}

static DWORD WINAPI worker(void *parameter)
{
    const DWORD slot = *(const DWORD *)parameter;
    SetLastError(0x1234);
    if (TlsGetValue(slot) != NULL || GetLastError() != ERROR_SUCCESS ||
        teb_slot(slot) != 0)
        return 1;
    if (!TlsSetValue(slot, (void *)(uintptr_t)0x22334455) ||
        TlsGetValue(slot) != (void *)(uintptr_t)0x22334455 ||
        teb_slot(slot) != (uintptr_t)0x22334455)
        return 2;
    return 0x144;
}

int import_fixture_entry(void)
{
    DWORD slots[64];
    slots[0] = TlsAlloc();
    if (slots[0] != 0 || TlsGetValue(slots[0]) != NULL ||
        GetLastError() != ERROR_SUCCESS || teb_slot(slots[0]) != 0)
        return 10;
    if (!TlsSetValue(slots[0], (void *)(uintptr_t)0x11223344) ||
        TlsGetValue(slots[0]) != (void *)(uintptr_t)0x11223344 ||
        teb_slot(slots[0]) != (uintptr_t)0x11223344)
        return 11;

    HANDLE thread = CreateThread(NULL, 0, worker, &slots[0], 0, NULL);
    DWORD code = 0;
    if (thread == NULL || WaitForSingleObject(thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &code) || code != 0x144 ||
        !CloseHandle(thread))
        return 12;

    /* A different guest thread must not modify this thread's Win64 TEB. */
    if (TlsGetValue(slots[0]) != (void *)(uintptr_t)0x11223344 ||
        teb_slot(slots[0]) != (uintptr_t)0x11223344)
        return 13;

    /* Allocate exactly the supported inline slots, then exhaust them. */
    for (DWORD i = 1; i < 64; ++i) {
        slots[i] = TlsAlloc();
        if (slots[i] != i)
            return 14;
    }
    if (TlsAlloc() != TLS_OUT_OF_INDEXES)
        return 15;

    /* Free + reallocate the same index. The previous value must not leak. */
    if (!TlsFree(slots[0]) || TlsGetValue(slots[0]) != NULL ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 16;
    DWORD reused = TlsAlloc();
    if (reused != 0 || TlsGetValue(reused) != NULL ||
        GetLastError() != ERROR_SUCCESS || teb_slot(reused) != 0)
        return 17;
    if (!TlsSetValue(reused, NULL) || TlsGetValue(reused) != NULL ||
        GetLastError() != ERROR_SUCCESS)
        return 18;

    if (!TlsFree(reused) || TlsFree(reused) ||
        GetLastError() != ERROR_INVALID_PARAMETER)
        return 19;
    for (DWORD i = 1; i < 64; ++i)
        if (!TlsFree(slots[i]))
            return 20;
    return 0;
}
