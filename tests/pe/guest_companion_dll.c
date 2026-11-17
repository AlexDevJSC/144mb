/* SPDX-License-Identifier: MIT */
/* PE64 DLL with native entry, no CRT, no TLS and no private host syscalls. */
#include <windows.h>
static volatile LONG process_attached;
static volatile LONG thread_attaches, thread_detaches;
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (instance == NULL)
        return FALSE;
    if (reason == DLL_PROCESS_ATTACH)
        process_attached = 1;
    else if (reason == DLL_PROCESS_DETACH)
        process_attached = 0;
    else if (reason == DLL_THREAD_ATTACH)
        ++thread_attaches;
    else if (reason == DLL_THREAD_DETACH)
        ++thread_detaches;
    return TRUE;
}
__declspec(dllexport) int WINAPI GuestAnswer(int value)
{
    return process_attached == 1 ? 42 + value : -1;
}

__declspec(dllexport) int WINAPI GuestThreadEvents(void)
{
    return (thread_attaches << 16) | thread_detaches;
}

/* This actual guest PE32+ DLL runs a leaf hot loop with no host imports,
 * libc calls or loader-held mutex. The supervisor may stop it only when
 * its .text range was pinned and copied to the parent before fork. */
__declspec(dllexport) __attribute__((noinline))
int WINAPI GuestHotLoop(volatile LONG *counter, const volatile LONG *release)
{
    if (!counter || !release || !process_attached)
        return -1;
    while (__atomic_load_n(release, __ATOMIC_RELAXED) == 0)
        __atomic_add_fetch(counter, 1, __ATOMIC_RELAXED);
    return 0x177;
}
