/* SPDX-License-Identifier: MIT */
/* A freestanding AMD64 PE validates native ntdll + Win32 ABI and state. */
#include <windows.h>
#include <stdint.h>

#define NT_OK ((LONG)0)
#define NT_INVALID_HANDLE ((LONG)0xc0000008u)
#define NT_INVALID_INFO_CLASS ((LONG)0xc0000003u)
#define NT_INFO_LENGTH_MISMATCH ((LONG)0xc0000004u)
#define NT_INVALID_PARAMETER ((LONG)0xc000000du)
#define NT_CONFLICTING_ADDRESSES ((LONG)0xc0000018u)

typedef struct {
    LONG exit_status;
    void *peb;
    uintptr_t affinity;
    LONG base_priority;
    uintptr_t pid, parent;
} PROCESS_BASIC;

typedef struct {
    LONG exit_status;
    void *teb;
    struct { uintptr_t process, thread; } client_id;
    uintptr_t affinity;
    LONG priority, base_priority;
} THREAD_BASIC;

__declspec(dllimport) LONG NTAPI NtQueryInformationProcess(HANDLE, ULONG, void *, ULONG, ULONG *);
__declspec(dllimport) LONG NTAPI NtQueryInformationThread(HANDLE, ULONG, void *, ULONG, ULONG *);
__declspec(dllimport) void *NTAPI RtlGetCurrentPeb(void);
__declspec(dllimport) LONG NTAPI NtAllocateVirtualMemory(HANDLE, void **, uintptr_t,
                                                          SIZE_T *, ULONG, ULONG);
__declspec(dllimport) LONG NTAPI NtFreeVirtualMemory(HANDLE, void **, SIZE_T *, ULONG);
__declspec(dllimport) LONG NTAPI NtProtectVirtualMemory(HANDLE, void **, SIZE_T *, ULONG, ULONG *);
__declspec(dllimport) LONG NTAPI NtQueryVirtualMemory(HANDLE, const void *, ULONG,
                                                       void *, SIZE_T, SIZE_T *);

static uintptr_t gs_pointer(unsigned int offset)
{
    uintptr_t address;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(address) : "r"((uintptr_t)offset));
    return address;
}

int import_fixture_entry(void)
{
    const HANDLE process = GetCurrentProcess();
    const HANDLE thread = GetCurrentThread();
    PROCESS_BASIC basic = {0};
    THREAD_BASIC thread_basic = {0};
    ULONG returned = 0;
    SetLastError(0x5678);

    if (sizeof(basic) != 48 || sizeof(thread_basic) != 48 ||
        NtQueryInformationProcess(process, 0, &basic, sizeof(basic), &returned) != NT_OK ||
        returned != sizeof(basic) || basic.peb != (void *)gs_pointer(0x60) ||
        basic.peb != RtlGetCurrentPeb() ||
        basic.pid != GetCurrentProcessId() || basic.exit_status != 259 ||
        GetLastError() != 0x5678)
        return 10;

    if (NtQueryInformationThread(thread, 0, &thread_basic, sizeof(thread_basic), &returned) != NT_OK ||
        returned != sizeof(thread_basic) || thread_basic.teb != (void *)gs_pointer(0x30) ||
        thread_basic.client_id.process != GetCurrentProcessId() ||
        thread_basic.client_id.thread != GetCurrentThreadId() ||
        GetLastError() != 0x5678)
        return 11;

    if (NtQueryInformationThread((HANDLE)(uintptr_t)17, 0, &thread_basic,
                                 sizeof(thread_basic), NULL) != NT_INVALID_HANDLE ||
        NtQueryInformationProcess(process, 99, &basic, sizeof(basic), NULL) != NT_INVALID_INFO_CLASS ||
        NtQueryInformationProcess(process, 0, &basic, sizeof(basic) - 1, &returned) != NT_INFO_LENGTH_MISMATCH ||
        returned != sizeof(basic) || GetLastError() != 0x5678)
        return 12;

    uintptr_t wow64 = UINTPTR_MAX;
    if (NtQueryInformationProcess(process, 26, &wow64, sizeof(wow64), &returned) != NT_OK ||
        wow64 != 0 || returned != sizeof(wow64))
        return 13;

    void *base = NULL;
    SIZE_T size = 8192;
    if (NtAllocateVirtualMemory(process, &base, 0, &size, MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE) != NT_OK ||
        base == NULL || size != 8192 || GetLastError() != 0x5678)
        return 14;

    MEMORY_BASIC_INFORMATION info = {0};
    SIZE_T actual = 0;
    if (NtQueryVirtualMemory(process, base, 0, &info, sizeof(info), &actual) != NT_OK ||
        actual != sizeof(info) || info.AllocationBase != base ||
        info.RegionSize != size || info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE)
        return 15;

    void *portion = (unsigned char *)base + 3;
    SIZE_T portion_size = 1024;
    ULONG old = 0;
    if (NtProtectVirtualMemory(process, &portion, &portion_size,
                               PAGE_READONLY, &old) != NT_OK ||
        portion != base || portion_size != 4096 || old != PAGE_READWRITE ||
        GetLastError() != 0x5678)
        return 16;

    if (NtAllocateVirtualMemory(process, &portion, 1, &portion_size,
                                MEM_COMMIT, PAGE_READWRITE) != NT_INVALID_PARAMETER ||
        NtQueryVirtualMemory(process, base, 25, &info, sizeof(info), NULL) != NT_INVALID_INFO_CLASS ||
        GetLastError() != 0x5678)
        return 17;

    size = 0;
    if (NtFreeVirtualMemory(process, &base, &size, MEM_RELEASE) != NT_OK ||
        size != 0 || GetLastError() != 0x5678)
        return 18;
    if (NtQueryVirtualMemory(process, base, 0, &info, sizeof(info), NULL) != NT_CONFLICTING_ADDRESSES)
        return 19;
    return 0;
}
