/* SPDX-License-Identifier: MIT */
/* A freestanding Win64 executable verifies the NTDLL/Win32 file I/O bridge. */
#include <windows.h>
#include <stdint.h>

#define NT_SUCCESS ((LONG)0)
#define NT_END_OF_FILE ((LONG)0xc0000011u)
#define NT_ACCESS_VIOLATION ((LONG)0xc0000005u)
#define NT_INVALID_HANDLE ((LONG)0xc0000008u)
#define NT_INVALID_PARAMETER ((LONG)0xc000000du)
#define NT_INFO_LENGTH_MISMATCH ((LONG)0xc0000004u)
#define NT_INVALID_INFO_CLASS ((LONG)0xc0000003u)
#define NT_ACCESS_DENIED ((LONG)0xc0000022u)
#define NT_NOT_SUPPORTED ((LONG)0xc00000bbu)

struct native_io_status {
    LONG status;
    uintptr_t information;
};
struct native_file_standard {
    int64_t allocation_size, end_of_file;
    uint32_t links;
    uint8_t pending_delete, directory;
    uint8_t reserved[2];
};
struct native_file_position { int64_t byte_offset; };

__declspec(dllimport) LONG NTAPI NtReadFile(
    HANDLE, HANDLE, void *, void *, struct native_io_status *, void *, ULONG,
    const LARGE_INTEGER *, const ULONG *);
__declspec(dllimport) LONG NTAPI NtWriteFile(
    HANDLE, HANDLE, void *, void *, struct native_io_status *, const void *, ULONG,
    const LARGE_INTEGER *, const ULONG *);
__declspec(dllimport) LONG NTAPI NtQueryInformationFile(
    HANDLE, struct native_io_status *, void *, ULONG, ULONG);
__declspec(dllimport) LONG NTAPI NtSetInformationFile(
    HANDLE, struct native_io_status *, const void *, ULONG, ULONG);
__declspec(dllimport) LONG NTAPI NtFlushBuffersFile(
    HANDLE, struct native_io_status *);

static int test_file_io(void)
{
    static const WCHAR path[] = L"C:\\nt-io-test.bin";
    HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 10;

    struct native_io_status io = {0};
    SetLastError(0x1442);
    const char initial[] = {'A', 'B', 'C', 'D', 'E'};
    if (NtWriteFile(file, NULL, NULL, NULL, &io, initial, sizeof(initial), NULL, NULL) != NT_SUCCESS ||
        io.status != NT_SUCCESS || io.information != sizeof(initial) ||
        GetLastError() != 0x1442)
        return 11;

    struct native_file_position pos = {-1};
    if (NtQueryInformationFile(file, &io, &pos, sizeof(pos), 14) != NT_SUCCESS ||
        io.information != sizeof(pos) || pos.byte_offset != 5)
        return 12;

    const LARGE_INTEGER second = {.QuadPart = 2};
    char sample[6] = {0};
    if (NtReadFile(file, NULL, NULL, NULL, &io, sample, 2, &second, NULL) != NT_SUCCESS ||
        io.information != 2 || sample[0] != 'C' || sample[1] != 'D' ||
        NtQueryInformationFile(file, &io, &pos, sizeof(pos), 14) != NT_SUCCESS ||
        pos.byte_offset != 5)
        return 13;

    if (NtReadFile(file, NULL, NULL, NULL, &io, sample, 1, NULL, NULL) != NT_END_OF_FILE ||
        io.status != NT_END_OF_FILE || io.information != 0 || GetLastError() != 0x1442)
        return 14;

    const struct native_file_position at_one = {1};
    if (NtSetInformationFile(file, &io, &at_one, sizeof(at_one), 14) != NT_SUCCESS ||
        NtReadFile(file, NULL, NULL, NULL, &io, sample, 3, NULL, NULL) != NT_SUCCESS ||
        io.information != 3 || sample[0] != 'B' || sample[1] != 'C' || sample[2] != 'D')
        return 15;

    if (NtWriteFile(file, NULL, NULL, NULL, &io, "x", 1, &second, NULL) != NT_SUCCESS ||
        io.information != 1 || NtQueryInformationFile(file, &io, &pos, sizeof(pos), 14) != NT_SUCCESS ||
        pos.byte_offset != 4)
        return 16;
    if (NtWriteFile(file, NULL, NULL, NULL, &io, "!", 1, NULL, NULL) != NT_SUCCESS ||
        io.information != 1)
        return 17;

    struct native_file_standard standard = {0};
    if (sizeof(standard) != 24 ||
        NtQueryInformationFile(file, &io, &standard, sizeof(standard), 5) != NT_SUCCESS ||
        io.information != sizeof(standard) || standard.end_of_file != 5 ||
        standard.links == 0 || standard.directory != 0)
        return 18;

    LARGE_INTEGER beginning = {.QuadPart = 0};
    DWORD done = 0;
    if (!SetFilePointerEx(file, beginning, NULL, FILE_BEGIN) ||
        !ReadFile(file, sample, 5, &done, NULL) || done != 5 ||
        sample[0] != 'A' || sample[1] != 'B' || sample[2] != 'x' ||
        sample[3] != 'D' || sample[4] != '!')
        return 19;

    const struct native_file_position new_eof = {3};
    LARGE_INTEGER win_size = {0};
    if (NtSetInformationFile(file, &io, &new_eof, sizeof(new_eof), 20) != NT_SUCCESS ||
        NtFlushBuffersFile(file, &io) != NT_SUCCESS || io.information != 0 ||
        !GetFileSizeEx(file, &win_size) || win_size.QuadPart != 3)
        return 20;

    /* Check status/size validation and that NTDLL never changes LastError. */
    if (NtQueryInformationFile(file, &io, &standard, 4, 5) != NT_INFO_LENGTH_MISMATCH ||
        io.status != NT_INFO_LENGTH_MISMATCH ||
        NtQueryInformationFile(file, &io, &standard, sizeof(standard), 123) != NT_INVALID_INFO_CLASS ||
        NtSetInformationFile(file, &io, &new_eof, sizeof(new_eof), 123) != NT_INVALID_INFO_CLASS ||
        GetLastError() != 0x1442)
        return 21;

    struct native_file_position negative = {-1};
    if (NtSetInformationFile(file, &io, &negative, sizeof(negative), 14) != NT_INVALID_PARAMETER ||
        NtReadFile(file, NULL, NULL, NULL, &io, NULL, 1, NULL, NULL) != NT_ACCESS_VIOLATION ||
        NtReadFile(file, (HANDLE)(uintptr_t)3, NULL, NULL, &io, sample, 1, NULL, NULL) != NT_NOT_SUPPORTED ||
        GetLastError() != 0x1442)
        return 22;
    if (NtReadFile(NULL, NULL, NULL, NULL, &io, sample, 1, NULL, NULL) != NT_INVALID_HANDLE ||
        io.status != NT_INVALID_HANDLE ||
        NtReadFile(file, NULL, NULL, NULL, NULL, sample, 1, NULL, NULL) != NT_ACCESS_VIOLATION ||
        GetLastError() != 0x1442)
        return 23;

    HANDLE alias = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &alias,
                         0, FALSE, DUPLICATE_SAME_ACCESS) || alias == NULL || !CloseHandle(file))
        return 24;
    if (!SetFilePointerEx(alias, beginning, NULL, FILE_BEGIN) ||
        NtReadFile(alias, NULL, NULL, NULL, &io, sample, 3, NULL, NULL) != NT_SUCCESS ||
        io.information != 3 || sample[0] != 'A' || sample[1] != 'B' || sample[2] != 'x' ||
        !CloseHandle(alias))
        return 25;

    HANDLE read_only = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (read_only == INVALID_HANDLE_VALUE)
        return 26;
    SetLastError(0x1443);
    if (NtWriteFile(read_only, NULL, NULL, NULL, &io, "Z", 1, NULL, NULL) != NT_ACCESS_DENIED ||
        io.status != NT_ACCESS_DENIED ||
        NtSetInformationFile(read_only, &io, &new_eof, sizeof(new_eof), 20) != NT_ACCESS_DENIED ||
        GetLastError() != 0x1443)
        return 27;
    if (!CloseHandle(read_only) || !DeleteFileW(path))
        return 28;
    return 0;
}

int import_fixture_entry(void)
{
    return test_file_io();
}
