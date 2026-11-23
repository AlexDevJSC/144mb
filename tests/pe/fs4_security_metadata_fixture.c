/* SPDX-License-Identifier: MIT */
/* Controlled PE32+ AMD64 fixture for Win32 security/time/basic metadata.
 * No host APIs or arbitrary executable input: all imports are resolved by
 * NT144 from advapi32/kernel32 in the project test runner. */
#include <windows.h>
#include <stdint.h>

static uint64_t to_ticks(FILETIME time)
{
    return ((uint64_t)time.dwHighDateTime << 32) | time.dwLowDateTime;
}

static FILETIME from_ticks(uint64_t ticks)
{
    FILETIME result = {(DWORD)ticks, (DWORD)(ticks >> 32)};
    return result;
}

int import_fixture_entry(void)
{
    static const WCHAR path[] = L"C:\\fs4-security-metadata-fixture.dat";
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 10;
    const uint64_t created_ticks = UINT64_C(132223104000000000); /* 2020-01-01 */
    const uint64_t changed_ticks = UINT64_C(132539328000000000); /* 2021-01-01 */
    const FILETIME created = from_ticks(created_ticks);
    const FILETIME written = from_ticks(changed_ticks);
    if (!SetFileTime(h, &created, NULL, &written))
        return 11;
    FILETIME got_created = {0}, got_accessed = {0}, got_written = {0};
    if (!GetFileTime(h, &got_created, &got_accessed, &got_written) ||
        to_ticks(got_created) != created_ticks ||
        to_ticks(got_written) != changed_ticks)
        return 12;

    DWORD needed=0;
    const DWORD wanted = OWNER_SECURITY_INFORMATION |
                         GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    if (GetFileSecurityW(path,wanted,NULL,0,&needed) ||
        GetLastError()!=ERROR_INSUFFICIENT_BUFFER ||
        needed<20 || needed>256)
        return 13;
    unsigned char security[256] = {0};
    if (!GetFileSecurityW(path,wanted,security,sizeof(security),&needed) ||
        security[0] != SECURITY_DESCRIPTOR_REVISION ||
        ((((uint16_t)security[2]) | ((uint16_t)security[3] << 8)) &
         SE_SELF_RELATIVE) == 0)
        return 14;
    if (GetFileSecurityW(path,SACL_SECURITY_INFORMATION,
                         security,sizeof(security),&needed) ||
        GetLastError()!=ERROR_ACCESS_DENIED)
        return 15;

    /* The local, unbrokered runtime must never pretend it accepted an
     * NT DACL write. Both real advapi32 imports are checked by the PE
     * loader; only the authenticated service has a writable DACL API. */
    if (SetFileSecurityW(path,DACL_SECURITY_INFORMATION,security) ||
        GetLastError()!=ERROR_NOT_SUPPORTED)
        return 20;
    if (SetFileSecurityA("C:\\fs4-security-metadata-fixture.dat",
                         DACL_SECURITY_INFORMATION,security) ||
        GetLastError()!=ERROR_NOT_SUPPORTED)
        return 21;

    FILE_BASIC_INFO basic={0};
    if (!GetFileInformationByHandleEx(h,FileBasicInfo,&basic,sizeof(basic)) ||
        (uint64_t)basic.CreationTime.QuadPart != created_ticks ||
        (uint64_t)basic.LastWriteTime.QuadPart != changed_ticks)
        return 16;
    FILE_STANDARD_INFO standard={0};
    if (!GetFileInformationByHandleEx(h,FileStandardInfo,&standard,sizeof(standard)) ||
        standard.EndOfFile.QuadPart != 0 || standard.NumberOfLinks < 1)
        return 17;
    basic.CreationTime.QuadPart=0;
    basic.LastAccessTime.QuadPart=0;
    basic.ChangeTime.QuadPart=0;
    basic.FileAttributes=0;
    basic.LastWriteTime.QuadPart=(LONGLONG)created_ticks;
    if (!SetFileInformationByHandle(h,FileBasicInfo,&basic,sizeof(basic)) ||
        !GetFileTime(h,NULL,NULL,&got_written) ||
        to_ticks(got_written)!=created_ticks)
        return 18;

    if (!CloseHandle(h) || !DeleteFileW(path))
        return 19;
    return 0;
}
