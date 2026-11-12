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
