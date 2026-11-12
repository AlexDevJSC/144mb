/* SPDX-License-Identifier: MIT */
/* Freestanding PE32+ regression: Windows x64 TEB dynamic TLS slots. */
#include <windows.h>
#include <stdint.h>

static uintptr_t expansion_pointer(void)
{
    uintptr_t pointer;
    __asm__ volatile("movq %%gs:0x1780, %0" : "=r"(pointer));
    return pointer;
}

static uintptr_t teb_slot(DWORD index)
{
    if (index >= 64) {
        const uintptr_t expansion = expansion_pointer();
        return expansion ? ((const uintptr_t *)expansion)[index - 64] : 0;
    }
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
    DWORD slots[1088];
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

    /* Exercise all 64 inline slots and 1024 native TEB expansion slots. */
    for (DWORD i = 1; i < 1088; ++i) {
        slots[i] = TlsAlloc();
        if (slots[i] != i)
            return 14;
    }
    if (TlsAlloc() != TLS_OUT_OF_INDEXES)
        return 15;
    if (expansion_pointer() != 0 || TlsGetValue(slots[64]) != NULL ||
        GetLastError() != ERROR_SUCCESS)
        return 26;
    if (!TlsSetValue(slots[64], (void *)(uintptr_t)0x55667788) ||
        expansion_pointer() == 0 ||
        teb_slot(slots[64]) != (uintptr_t)0x55667788)
        return 27;
    HANDLE expanded_thread = CreateThread(NULL, 0, worker, &slots[64], 0, NULL);
    DWORD expanded_code = 0;
    if (expanded_thread == NULL ||
        WaitForSingleObject(expanded_thread, 5000) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(expanded_thread, &expanded_code) ||
        expanded_code != 0x144 || !CloseHandle(expanded_thread))
        return 28;
    if (TlsGetValue(slots[64]) != (void *)(uintptr_t)0x55667788 ||
        teb_slot(slots[64]) != (uintptr_t)0x55667788)
        return 29;

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
    for (DWORD i = 1; i < 1088; ++i)
        if (!TlsFree(slots[i]))
            return 20;
    return 0;
}
