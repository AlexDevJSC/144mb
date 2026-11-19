/* SPDX-License-Identifier: MIT */
/* Freestanding PE32+ AMD64 fixture: execute real CreateHardLinkW via the
 * kernel32 import table inside NT144, never via a host POSIX call. */
#include <windows.h>
#include <stdint.h>

int import_fixture_entry(void)
{
    static const WCHAR original[] = L"C:\\fs4-hardlink-pe-original.dat";
    static const WCHAR alias[] = L"C:\\fs4-hardlink-pe-second.dat";
    static const WCHAR moved[] = L"C:\\fs4-hardlink-pe-moved.dat";
    const DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE first = CreateFileW(original, GENERIC_READ | GENERIC_WRITE,
                               sharing, NULL, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, NULL);
    if (first == INVALID_HANDLE_VALUE)
        return 10;
    DWORD written = 0;
    const char bytes[] = "SAME-FILE";
    if (!WriteFile(first, bytes, 9, &written, NULL) || written != 9 ||
        !CloseHandle(first))
        return 11;
    if (!CreateHardLinkW(alias, original, NULL))
        return 12;
    if (CreateHardLinkW(alias, original, NULL) ||
        GetLastError() != ERROR_ALREADY_EXISTS)
        return 13;
    first = CreateFileW(original, GENERIC_READ, sharing, NULL,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE second = CreateFileW(alias, GENERIC_READ, sharing, NULL,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (first == INVALID_HANDLE_VALUE || second == INVALID_HANDLE_VALUE)
        return 14;
    BY_HANDLE_FILE_INFORMATION left = {0}, right = {0};
    if (!GetFileInformationByHandle(first, &left) ||
        !GetFileInformationByHandle(second, &right) ||
        left.dwVolumeSerialNumber != right.dwVolumeSerialNumber ||
        left.nFileIndexHigh != right.nFileIndexHigh ||
        left.nFileIndexLow != right.nFileIndexLow ||
        left.nNumberOfLinks < 2 || right.nNumberOfLinks < 2)
        return 15;
    if (!CloseHandle(first) || !CloseHandle(second))
        return 16;
    HANDLE blocker = CreateFileW(original, GENERIC_READ, 0, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (blocker == INVALID_HANDLE_VALUE)
        return 17;
    if (CreateHardLinkW(moved, original, NULL) ||
        GetLastError() != ERROR_SHARING_VIOLATION || !CloseHandle(blocker))
        return 18;
    if (!DeleteFileW(original))
        return 19;
    second = CreateFileW(alias, GENERIC_READ, sharing, NULL, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    if (second == INVALID_HANDLE_VALUE)
        return 20;
    char readback[9] = {0};
    DWORD received = 0;
    if (!ReadFile(second, readback, 9, &received, NULL) || received != 9)
        return 21;
    for (int i = 0; i < 9; ++i)
        if (readback[i] != bytes[i])
            return 22;
    if (!CloseHandle(second) || !MoveFileW(alias, moved) ||
        !DeleteFileW(moved))
        return 23;
    return 0;
}
