/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_WINFILE_H
#define NTNATIVE_WINFILE_H

#include "compat.h"

/* Start-up file HANDLE inheritance between PE children. The source FDs
 * are private duplicates, pinned until posix_spawn has duplicated them.
 * The target uses a sealed manifest and the exact guest HANDLE values. */
enum { NT_CHILD_FILE_LIMIT = 32, NT_CHILD_FILE_FD_BASE = 32 };
struct nt_child_file_transfer {
    int manifest_fd;
    int source_fds[NT_CHILD_FILE_LIMIT];
    size_t count;
};
uint32_t nt_runtime_snapshot_inherited_files(bool enabled, struct nt_child_file_transfer *transfer,
                                             const uintptr_t standard_handles[3]);
void nt_runtime_release_inherited_files(struct nt_child_file_transfer *transfer);
bool nt_runtime_inherited_file_count(int manifest_fd, size_t *count);
bool nt_runtime_inherited_standard_handles(int manifest_fd, uintptr_t handles[3]);
bool nt_runtime_restore_inherited_files(int manifest_fd);

int32_t NT_MS_ABI nt_CreatePipe(uintptr_t *reader, uintptr_t *writer, const void *security, uint32_t size_hint);
int32_t NT_MS_ABI nt_GetHandleInformation(uintptr_t handle, uint32_t *flags);
int32_t NT_MS_ABI nt_SetHandleInformation(uintptr_t handle, uint32_t mask, uint32_t flags);
int32_t NT_MS_ABI nt_GetProcessHandleCount(uintptr_t process, uint32_t *count);
int32_t NT_MS_ABI nt_DuplicateHandle(uintptr_t source_process, uintptr_t source,
                                     uintptr_t target_process, uintptr_t *target,
                                     uint32_t access, int32_t inherit, uint32_t options);

struct nt_createfile2_params {
    uint32_t size, attributes, flags, security_qos;
    const void *security;
    uintptr_t template_file;
};
struct nt_file_attribute_data {
    uint32_t attributes;
    struct nt_file_time created, accessed, written;
    uint32_t size_high, size_low;
};
struct nt_find_data_w {
    struct nt_file_attribute_data info;
    uint32_t reserved[2];
    uint16_t name[260], alternate[14];
};
struct nt_find_data_a {
    struct nt_file_attribute_data info;
    uint32_t reserved[2];
    char name[260], alternate[14];
};
struct nt_handle_file_info {
    uint32_t attributes;
    struct nt_file_time created, accessed, written;
    uint32_t volume_serial, size_high, size_low, links, index_high, index_low;
};

uintptr_t NT_MS_ABI nt_CreateFileW(const uint16_t *path, uint32_t access, uint32_t share,
                                  const void *security, uint32_t disposition, uint32_t flags,
                                  uintptr_t template_file);
uintptr_t NT_MS_ABI nt_CreateFile2(const uint16_t *path, uint32_t access, uint32_t share,
                                  uint32_t disposition, const struct nt_createfile2_params *params);
int32_t NT_MS_ABI nt_CreateDirectoryW(const uint16_t *path, const void *security);
int32_t NT_MS_ABI nt_RemoveDirectoryW(const uint16_t *path);
int32_t NT_MS_ABI nt_DeleteFileW(const uint16_t *path);
int32_t NT_MS_ABI nt_MoveFileExW(const uint16_t *source, const uint16_t *destination, uint32_t flags);
int32_t NT_MS_ABI nt_MoveFileW(const uint16_t *source, const uint16_t *destination);
int32_t NT_MS_ABI nt_CopyFileW(const uint16_t *source, const uint16_t *destination, int32_t fail_if_exists);
uint32_t NT_MS_ABI nt_GetCurrentDirectoryW(uint32_t size, uint16_t *buffer);
int32_t NT_MS_ABI nt_SetCurrentDirectoryW(const uint16_t *path);
uint32_t NT_MS_ABI nt_GetFullPathNameW(const uint16_t *path, uint32_t size, uint16_t *buffer,
                                      uint16_t **leaf);
uint32_t NT_MS_ABI nt_GetFileAttributesW(const uint16_t *path);
int32_t NT_MS_ABI nt_SetFileAttributesW(const uint16_t *path, uint32_t attributes);
int32_t NT_MS_ABI nt_GetFileAttributesExW(const uint16_t *path, int32_t level,
                                         struct nt_file_attribute_data *info);
int32_t NT_MS_ABI nt_AreFileApisANSI(void);
uintptr_t NT_MS_ABI nt_FindFirstFileExW(const uint16_t *path, int32_t level, struct nt_find_data_w *data,
                                       int32_t operation, void *filter, uint32_t flags);
uintptr_t NT_MS_ABI nt_FindFirstFileW(const uint16_t *path, struct nt_find_data_w *data);
int32_t NT_MS_ABI nt_FindNextFileW(uintptr_t handle, struct nt_find_data_w *data);
uintptr_t NT_MS_ABI nt_FindFirstFileExA(const char *path, int32_t level, struct nt_find_data_a *data,
                                       int32_t operation, void *filter, uint32_t flags);
uintptr_t NT_MS_ABI nt_FindFirstFileA(const char *path, struct nt_find_data_a *data);
int32_t NT_MS_ABI nt_FindNextFileA(uintptr_t handle, struct nt_find_data_a *data);
int32_t NT_MS_ABI nt_FindClose(uintptr_t handle);
int32_t NT_MS_ABI nt_ReadFile(uintptr_t handle, void *buffer, uint32_t size, uint32_t *done, void *overlap);
int32_t NT_MS_ABI nt_WriteFile(uintptr_t handle, const void *buffer, uint32_t size, uint32_t *done, void *overlap);
int32_t NT_MS_ABI nt_FlushFileBuffers(uintptr_t handle);
int32_t NT_MS_ABI nt_GetFileSizeEx(uintptr_t handle, int64_t *size);
int32_t NT_MS_ABI nt_SetFilePointerEx(uintptr_t handle, int64_t offset, int64_t *position, uint32_t method);
int32_t NT_MS_ABI nt_SetEndOfFile(uintptr_t handle);
uint32_t NT_MS_ABI nt_GetFileType(uintptr_t handle);
int32_t NT_MS_ABI nt_GetFileInformationByHandle(uintptr_t handle, struct nt_handle_file_info *info);
uint32_t NT_MS_ABI nt_GetFileSize(uintptr_t handle, uint32_t *high);
int32_t NT_MS_ABI nt_GetFileTime(uintptr_t handle, struct nt_file_time *created,
                                struct nt_file_time *accessed, struct nt_file_time *written);
int32_t NT_MS_ABI nt_SetFileTime(uintptr_t handle, const struct nt_file_time *created,
                                const struct nt_file_time *accessed, const struct nt_file_time *written);
int32_t NT_MS_ABI nt_GetFileInformationByHandleEx(uintptr_t handle, int32_t kind, void *info, uint32_t size);
int32_t NT_MS_ABI nt_SetFileInformationByHandle(uintptr_t handle, int32_t kind, const void *info, uint32_t size);
uint32_t NT_MS_ABI nt_GetFinalPathNameByHandleW(uintptr_t handle, uint16_t *buffer, uint32_t size, uint32_t flags);
uintptr_t NT_MS_ABI nt_GetStdHandle(uint32_t which);
int32_t NT_MS_ABI nt_SetStdHandle(uint32_t which, uintptr_t handle);
int32_t NT_MS_ABI nt_GetConsoleMode(uintptr_t handle, uint32_t *mode);
int32_t NT_MS_ABI nt_ReadConsoleW(uintptr_t handle, uint16_t *buffer, uint32_t count,
                                  uint32_t *done, const void *control);
int32_t NT_MS_ABI nt_SetConsoleTitleA(const char *title);
int32_t NT_MS_ABI nt_WriteConsoleA(uintptr_t handle, const char *buffer, uint32_t count, uint32_t *done, void *reserved);
int32_t NT_MS_ABI nt_WriteConsoleW(uintptr_t handle, const uint16_t *buffer, uint32_t count, uint32_t *done, void *reserved);
uint32_t NT_MS_ABI nt_GetConsoleOutputCP(void);

#endif
