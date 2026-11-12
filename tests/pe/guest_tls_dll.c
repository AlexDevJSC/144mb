/* SPDX-License-Identifier: MIT */
/* Freestanding PE64 DLL with a real static TLS directory and no CRT.
 * MinGW GCC does not support __declspec(thread) under -Werror here, so
 * describe IMAGE_TLS_DIRECTORY64 explicitly and access its slot through
 * the documented Win64 TEB TLS vector at GS:0x58. */
#include <windows.h>
#include <stdint.h>

__attribute__((section(".tls"), aligned(16), used))
static uint32_t tls_template[2] = {17, 0};

static uint32_t tls_index;
static volatile LONG process_callbacks, thread_callbacks, thread_detaches;
static volatile LONG process_entries, thread_entries, thread_exits;
static volatile LONG callback_errors;

static volatile uint32_t *thread_tls(void)
{
    uintptr_t vector;
    __asm__ volatile("movq %%gs:0x58, %0" : "=r"(vector));
    if (vector == 0 || tls_index == 0 || tls_index >= 64)
        return NULL;
    return (volatile uint32_t *)((uintptr_t *)vector)[tls_index];
}

static VOID NTAPI guest_tls_callback(PVOID instance, DWORD reason, PVOID reserved)
{
    (void)reserved;
    if (instance == NULL)
        return;
    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH) {
        volatile uint32_t *block = thread_tls();
        if (block == NULL || block[0] != 17 || block[1] != 0)
            ++callback_errors;
    }
    if (reason == DLL_PROCESS_ATTACH)
        ++process_callbacks;
    else if (reason == DLL_THREAD_ATTACH)
        ++thread_callbacks;
    else if (reason == DLL_THREAD_DETACH)
        ++thread_detaches;
}

/* The PE linker recognizes _tls_used as the image's TLS data directory.
 * The two-pointer callback array is explicitly NUL-terminated. All three
 * addresses are base-relocated by the PE linker when needed. */
static PIMAGE_TLS_CALLBACK tls_callbacks[] = {guest_tls_callback, NULL};
const IMAGE_TLS_DIRECTORY64 _tls_used = {
    (ULONGLONG)(uintptr_t)tls_template,
    (ULONGLONG)(uintptr_t)(tls_template + 2),
    (ULONGLONG)(uintptr_t)&tls_index,
    (ULONGLONG)(uintptr_t)tls_callbacks,
    8, 0
};

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    volatile uint32_t *tls = thread_tls();
    if (instance == NULL || tls == NULL)
        return FALSE;
    if (reason == DLL_PROCESS_ATTACH) {
        if (process_callbacks != 1 || callback_errors != 0 || tls[0] != 17)
            return FALSE;
        ++process_entries;
    } else if (reason == DLL_THREAD_ATTACH) {
        if (thread_callbacks == 0 || callback_errors != 0 || tls[0] != 17)
            return FALSE;
        ++thread_entries;
    } else if (reason == DLL_THREAD_DETACH) {
        ++thread_exits;
    }
    return TRUE;
}
__declspec(dllexport) uint32_t WINAPI GuestTlsRead(void)
{
    volatile uint32_t *tls = thread_tls();
    return tls != NULL && process_entries == 1 && callback_errors == 0 ?
           tls[0] : UINT32_MAX;
}
__declspec(dllexport) uint32_t WINAPI GuestTlsWrite(uint32_t value)
{
    volatile uint32_t *tls = thread_tls();
    if (tls == NULL)
        return UINT32_MAX;
    tls[0] = value;
    return tls[0];
}
__declspec(dllexport) uint32_t WINAPI GuestTlsEvents(void)
{
    return ((uint32_t)thread_callbacks << 24) |
           ((uint32_t)thread_detaches << 16) |
           ((uint32_t)thread_entries << 8) | (uint32_t)thread_exits;
}
