/* SPDX-License-Identifier: MIT */
/* FileSystem4 differential NT144-side probe. Run on a throwaway --root only.
 * Calls the actual Linux runtime's Win32 W entrypoints, not host POSIX
 * filename lookup. Its JSON is compared with a real Windows NTFS report.
 * This process is not itself a PE/Bubblewrap isolation test. */
#define _GNU_SOURCE
#include "compat.h"
#include "winfile.h"
#include "winbase.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static bool probe_has_tilde(const uint16_t *text, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (text[i] == (uint16_t)'~')
            return true;
    return false;
}

static bool probe_bytes_match(const uint16_t *path,
                              const char *contents, size_t length)
{
    uintptr_t fd = nt_CreateFileW(path, UINT32_C(0x80000000), 7,
                                  NULL, 3, 0x80, 0);
    if (fd == UINTPTR_MAX)
        return false;
    char readback[80] = {0};
    uint32_t got = 0;
    bool result = length <= sizeof(readback) &&
                  nt_ReadFile(fd, readback, (uint32_t)length, &got, NULL) &&
                  got == length && memcmp(readback, contents, length) == 0;
    if (!nt_CloseHandle(fd))
        result = false;
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--root") != 0) {
        fputs("Usage: fs4-ntfs-probe --root DISPOSABLE_GUEST_C_DIRECTORY\n", stderr);
        return 2;
    }
    if (!nt_runtime_set_file_root(argv[2]) ||
        !nt_runtime_recover_dos83()) {
        fputs("Cannot prepare verified C: root/WAL\n", stderr);
        return 3;
    }
    const uint16_t *dir = u"C:\\fs4-ntfs-probe";
    const uint16_t *original =
        u"C:\\fs4-ntfs-probe\\longFileSystem4ReferenceFilename.txt";
    const uint16_t *upper =
        u"C:\\FS4-NTFS-PROBE\\LONGFILESYSTEM4REFERENCEFILENAME.TXT";
    const uint16_t *renamed =
        u"C:\\fs4-ntfs-probe\\renamedFileSystem4Reference.txt";
    if (!nt_CreateDirectoryW(dir, NULL)) {
        fputs("Probe directory exists or cannot be created; use disposable root\n",
              stderr);
        return 3;
    }
    const char bytes[] = "NT144 Windows reference";
    uintptr_t fd = nt_CreateFileW(original, UINT32_C(0x40000000), 7,
                                  NULL, 1, 0x80, 0);
    if (fd == UINTPTR_MAX) {
        (void)nt_RemoveDirectoryW(dir);
        return 3;
    }
    uint32_t written = 0;
    const bool write_ok = nt_WriteFile(fd, bytes, (uint32_t)(sizeof(bytes)-1),
                                       &written, NULL) &&
                          written == sizeof(bytes)-1;
    (void)nt_CloseHandle(fd);
    if (!write_ok) {
        (void)nt_DeleteFileW(original);
        (void)nt_RemoveDirectoryW(dir);
        return 3;
    }
    uint32_t acl_needed = 0;
    uint8_t acl_descriptor[256] = {0};
    (void)nt_GetFileSecurityW(original, 7u, NULL, 0, &acl_needed);
    const bool acl_descriptor_readable = acl_needed >= 20u &&
        acl_needed <= sizeof(acl_descriptor) &&
        nt_GetFileSecurityW(original, 7u, acl_descriptor,
                            (uint32_t)sizeof(acl_descriptor), &acl_needed) &&
        acl_descriptor[0] == 1u && (acl_descriptor[3] & 0x80u) != 0;
    const bool case_insensitive = probe_bytes_match(upper, bytes,
                                                     sizeof(bytes)-1);
    uint16_t shortpath[512] = {0};
    const uint32_t shortlen = nt_GetShortPathNameW(original, shortpath, 512);
    const bool short_generated = shortlen > 0 && shortlen < 512 &&
                                 probe_has_tilde(shortpath, shortlen);
    const bool moved = nt_MoveFileExW(original, renamed, 0) != 0;
    const bool rename_preserves_content = moved &&
        probe_bytes_match(renamed, bytes, sizeof(bytes)-1);
    const uint16_t *existing = moved ? renamed : original;
    const bool set_readonly = nt_SetFileAttributesW(existing, 1) != 0;
    const uint32_t attributes = nt_GetFileAttributesW(existing);
    const bool readonly = set_readonly &&
                          attributes != UINT32_MAX &&
                          (attributes & 1u) != 0;
    if (set_readonly)
        (void)nt_SetFileAttributesW(existing, 0x80);
    const bool cleaned = nt_DeleteFileW(existing) &&
                         nt_RemoveDirectoryW(dir);
    printf("{\"schema\":\"filesystem4-nt144-reference-v1\","
           "\"case_insensitive_exact_name\":%s,"
           "\"rename_preserves_content\":%s,"
           "\"readonly_attribute_visible\":%s,"
           "\"short_path_generated\":%s,"
           "\"hardlink_created\":null,"
           "\"hardlink_readable\":null,"
           "\"acl_descriptor_readable\":%s,"
           "\"acl_explicit_deny_enforced\":null,"
           "\"junction_reparse_visible\":null,"
           "\"symlink_reparse_visible\":null,"
           "\"short_path\":null,"
           "\"probe_cleaned_up\":%s}\n",
           case_insensitive ? "true" : "false",
           rename_preserves_content ? "true" : "false",
           readonly ? "true" : "false",
           short_generated ? "true" : "false",
           acl_descriptor_readable ? "true" : "false",
           cleaned ? "true" : "false");
    return cleaned ? 0 : 3;
}
