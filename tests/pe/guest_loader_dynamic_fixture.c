/* SPDX-License-Identifier: MIT */
/* Guest DLLs must load through the ordinary Win32 LoadLibraryA resolver. */
#include <windows.h>
#include <stdint.h>

static uintptr_t peblist(void)
{
    uintptr_t peb = 0;
    __asm__ volatile("movq %%gs:0x60, %0" : "=r"(peb));
    return peb;
}
static int in_loader_list(uintptr_t module)
{
    uintptr_t peb = peblist();
    if (!peb)
        return 0;
    uintptr_t ldr = *(const uintptr_t *)(peb + 0x18);
    if (!ldr)
        return 0;
    uintptr_t head = ldr + 0x10;
    uintptr_t next = *(const uintptr_t *)head;
    for (unsigned int i = 0; i < 64 && next != head; ++i) {
        if (!next)
            return 0;
        if (*(const uintptr_t *)(next + 0x30) == module)
            return 1;
        next = *(const uintptr_t *)next;
    }
    return 0;
}
int import_fixture_entry(void)
{
    SetLastError(0);
    HMODULE module = LoadLibraryA("guest_companion.dll");
    if (!module || (uintptr_t)module == 0 || GetLastError() == ERROR_MOD_NOT_FOUND)
        return 10;
    if (GetModuleHandleA("guest_companion.dll") != module || !in_loader_list((uintptr_t)module))
        return 11;
    FARPROC proc = GetProcAddress(module, "GuestAnswer");
    if (proc == NULL)
        return 12;
    int (WINAPI *answer)(int) = (int (WINAPI *)(int))proc;
    if (answer(1) != 43)
        return 13;
    char path[200] = {0};
    if (GetModuleFileNameA(module, path, sizeof(path)) == 0 ||
        path[0] != 'C' || path[1] != ':' || path[2] != '\\')
        return 14;
    HMODULE same = LoadLibraryA("guest_companion.dll");
    if (same != module || !FreeLibrary(same) ||
        GetModuleHandleA("guest_companion.dll") != module)
        return 15;
    if (!FreeLibrary(module) || GetModuleHandleA("guest_companion.dll") != NULL ||
        in_loader_list((uintptr_t)module))
        return 16;
    if (LoadLibraryA("../escape.dll") != NULL || GetLastError() != ERROR_MOD_NOT_FOUND)
        return 17;
    if (LoadLibraryA("C:\\Windows\\System32\\shady.dll") != NULL ||
        GetLastError() != ERROR_MOD_NOT_FOUND)
        return 18;
    return 0;
}
