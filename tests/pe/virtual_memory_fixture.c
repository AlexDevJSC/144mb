/* SPDX-License-Identifier: MIT */
/* Build-generated Win64 PE: exercise the public kernel32 ABI, not host C. */
#include <windows.h>
#include <stdint.h>

int import_fixture_entry(void)
{
    enum { PAGE_BYTES = 4096 };
    BYTE *const reserved = VirtualAlloc(NULL, PAGE_BYTES * 3, MEM_RESERVE, PAGE_READWRITE);
    if (reserved == NULL || ((uintptr_t)reserved & (65536 - 1)) != 0)
        return 10;

    MEMORY_BASIC_INFORMATION info = {0};
    if (VirtualQuery(reserved + PAGE_BYTES, &info, sizeof(info)) != sizeof(info) ||
        info.AllocationBase != reserved || info.State != MEM_RESERVE ||
        info.RegionSize != PAGE_BYTES * 3 || info.Protect != 0)
        return 11;

    BYTE *committed = VirtualAlloc(reserved + PAGE_BYTES, PAGE_BYTES,
                                   MEM_COMMIT, PAGE_READWRITE);
    if (committed != reserved + PAGE_BYTES)
        return 12;
    committed[1] = 0x65;

    if (VirtualQuery(committed, &info, sizeof(info)) != sizeof(info) ||
        info.BaseAddress != committed || info.State != MEM_COMMIT ||
        info.Protect != PAGE_READWRITE || info.Type != MEM_PRIVATE ||
        info.RegionSize != PAGE_BYTES)
        return 13;

    DWORD previous = 0;
    if (!VirtualProtect(committed, PAGE_BYTES, PAGE_READONLY, &previous) ||
        previous != PAGE_READWRITE)
        return 14;
    if (VirtualQuery(committed, &info, sizeof(info)) != sizeof(info) ||
        info.Protect != PAGE_READONLY || committed[1] != 0x65)
        return 15;

    if (VirtualProtect(reserved, PAGE_BYTES, PAGE_READONLY, &previous) ||
        GetLastError() != ERROR_INVALID_ADDRESS)
        return 16;

    if (!VirtualFree(committed, PAGE_BYTES, MEM_DECOMMIT))
        return 17;
    if (VirtualQuery(committed, &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_RESERVE || info.Protect != 0)
        return 18;
    committed = VirtualAlloc(reserved + PAGE_BYTES, PAGE_BYTES, MEM_COMMIT, PAGE_READWRITE);
    if (committed != reserved + PAGE_BYTES || committed[1] != 0)
        return 19;

    if (VirtualAlloc(reserved + PAGE_BYTES, PAGE_BYTES, MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE) != NULL ||
        GetLastError() != ERROR_NOT_SUPPORTED)
        return 20;

    if (VirtualFree(reserved + PAGE_BYTES, 0, MEM_RELEASE) ||
        GetLastError() != ERROR_INVALID_ADDRESS)
        return 21;
    if (!VirtualFree(reserved, 0, MEM_RELEASE))
        return 22;
    if (VirtualFree(reserved, 0, MEM_RELEASE) ||
        GetLastError() != ERROR_INVALID_ADDRESS)
        return 23;

    BYTE *automatic = VirtualAlloc(NULL, PAGE_BYTES, MEM_COMMIT, PAGE_READWRITE);
    if (automatic == NULL || (uintptr_t)automatic % 65536 != 0)
        return 24;
    automatic[0] = 7;
    if (automatic[0] != 7 || !VirtualFree(automatic, 0, MEM_RELEASE))
        return 25;
    return 0;
}
