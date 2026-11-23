/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_WINFILE_H
#define NTNATIVE_WINFILE_H

#include "compat.h"

/* Explicit opt-in NT-compatible inode: only the trusted lower-tree operator
 * may set this xattr. FUSE does not expose user.ntnative.* metadata. */
#define NT_FS_COMPAT_MARKER "user.ntnative.fs_compat"
enum { NT_FS_READ = 1, NT_FS_WRITE = 2, NT_FS_DELETE = 4,
       NT_FS_SHARE_ALL = NT_FS_READ | NT_FS_WRITE | NT_FS_DELETE };
/* Host-only calls, not PE exports. Marker: 1 marked, 0 plain POSIX,
 * -1 unusable metadata (fail closed). A lease is an independent OFD fd. */
int nt_runtime_open_guest_file_readonly(const char *path);
int nt_runtime_nt_file_marker(int fd);
int nt_runtime_acquire_linux_nt_lease(int fd, uint32_t access, uint32_t share);
void nt_runtime_release_linux_nt_lease(int *lease);

/* Host-only, UTF-8 DOS paths (e.g. C:\\dir\\file). These are not Win32
 * exports. The inotify consumer uses the second entrypoint AFTER external
 * renames; it is best-effort, not an atomic interception of host writes. */
/* Host-only pre-mutation WAL admission. Not an atomic transaction lock. */
bool nt_runtime_namespace_mutation_admissible(const char *path);
/* This returned descriptor keeps the per-grant WAL flock until end() and
 * makes a FUSE POSIX mutation atomic with respect to cooperative renames.
 * It does NOT journal an interrupted direct create/mkdir/truncate. */
int nt_runtime_begin_namespace_mutation(const char *path);
/* Trusted storage broker only: pin an existing Win32 file HANDLE to its
 * exact Linux inode and copy its normalized DOS path for WAL admission.
 * Never exported to a guest DLL. The caller owns the returned dup FD. */
int nt_runtime_fs4_pin_security_handle(uintptr_t handle,
                                       char *path, size_t capacity);
void nt_runtime_end_namespace_mutation(int guard);
bool nt_runtime_refresh_dos83_path(const char *path);
bool nt_runtime_reconcile_host_rename(const char *old_path, const char *new_path);
/* Host-only recovery. Replays only unambiguous, checksummed pending C:
 * rename transactions; refuses corrupt/ambiguous records (errno=EUCLEAN). */
bool nt_runtime_recover_dos83(void);
/* Startup preflight for C:, D:-Z:, UNC and redirected folders that have
 * been registered in this process. A pending WAL must recover or fail. */
bool nt_runtime_recover_all_dos83_grants(void);

/* Start-up file HANDLE inheritance between PE children. The source FDs
 * are private duplicates, pinned until posix_spawn has duplicated them.
 * The target uses a sealed manifest and the exact guest HANDLE values. */
enum { NT_CHILD_FILE_LIMIT = 32, NT_CHILD_FILE_FD_BASE = 32,
       NT_CHILD_LEASE_FD_BASE = NT_CHILD_FILE_FD_BASE + NT_CHILD_FILE_LIMIT };
struct nt_child_file_transfer {
    int manifest_fd;
    int source_fds[NT_CHILD_FILE_LIMIT];
    int lease_fds[NT_CHILD_FILE_LIMIT]; /* OFD lease, or /dev/null for pipe HANDLE */
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
struct nt_overlapped {
    uintptr_t internal, internal_high;
    uint32_t offset, offset_high;
    uintptr_t event;
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
int32_t NT_MS_ABI nt_CreateSymbolicLinkW(const uint16_t *link_name, const uint16_t *target_name,
                                          uint32_t flags);
int32_t NT_MS_ABI nt_CreateSymbolicLinkA(const char *link_name, const char *target_name,
                                          uint32_t flags);
int32_t NT_MS_ABI nt_DeleteFileW(const uint16_t *path);
int32_t NT_MS_ABI nt_CreateHardLinkW(const uint16_t *new_name, const uint16_t *existing_name,
                                     const void *security);
int32_t NT_MS_ABI nt_CreateHardLinkA(const char *new_name, const char *existing_name,
                                     const void *security);
int32_t NT_MS_ABI nt_MoveFileExW(const uint16_t *source, const uint16_t *destination, uint32_t flags);
int32_t NT_MS_ABI nt_MoveFileW(const uint16_t *source, const uint16_t *destination);
int32_t NT_MS_ABI nt_CopyFileW(const uint16_t *source, const uint16_t *destination, int32_t fail_if_exists);
/* LPPROGRESS_ROUTINE, Microsoft x64 ABI; stream_number=1 for a regular file.
 * CALLBACK_STREAM_SWITCH=1, CALLBACK_CHUNK_FINISHED=0.
 * PROGRESS_CONTINUE=0, CANCEL=1, STOP=2, QUIET=3. */
typedef uint32_t (NT_MS_ABI *nt_copy_progress_routine)(
    int64_t total, int64_t transferred, int64_t stream_size,
    int64_t stream_transferred, uint32_t stream_number, uint32_t reason,
    uintptr_t source_handle, uintptr_t destination_handle, void *context);
int32_t NT_MS_ABI nt_CopyFileExA(const char *source, const char *destination,
                                nt_copy_progress_routine progress, void *context,
                                const int32_t *cancel, uint32_t flags);
int32_t NT_MS_ABI nt_CopyFileExW(const uint16_t *source, const uint16_t *destination,
                                nt_copy_progress_routine progress, void *context,
                                const int32_t *cancel, uint32_t flags);
/* Partial, read-only ADVAPI32 GetFileSecurity bridge: owner/group/DACL from
 * Linux UID/GID/mode. SACL and extended NTFS ACEs are not represented. */
int32_t NT_MS_ABI nt_GetFileSecurityW(const uint16_t *name, uint32_t information,
                                      void *descriptor, uint32_t size, uint32_t *needed);
int32_t NT_MS_ABI nt_GetFileSecurityA(const char *name, uint32_t information,
                                      void *descriptor, uint32_t size, uint32_t *needed);
int32_t NT_MS_ABI nt_SetFileSecurityW(const uint16_t *name,uint32_t information,
                                      const void *descriptor);
int32_t NT_MS_ABI nt_SetFileSecurityA(const char *name,uint32_t information,
                                      const void *descriptor);
nt_status NT_MS_ABI nt_NtSetSecurityObject(uintptr_t handle,
                                            uint32_t information,
                                            const void *descriptor);
uint32_t NT_MS_ABI nt_GetLogicalDrives(void);
uint32_t NT_MS_ABI nt_GetDriveTypeW(const uint16_t *root);
uint32_t NT_MS_ABI nt_GetCurrentDirectoryW(uint32_t size, uint16_t *buffer);
int32_t NT_MS_ABI nt_SetCurrentDirectoryW(const uint16_t *path);
uint32_t NT_MS_ABI nt_GetFullPathNameW(const uint16_t *path, uint32_t size, uint16_t *buffer,
                                      uint16_t **leaf);
uint32_t NT_MS_ABI nt_GetLongPathNameW(const uint16_t *input, uint16_t *buffer, uint32_t size);
uint32_t NT_MS_ABI nt_GetLongPathNameA(const char *input, char *buffer, uint32_t size);
uint32_t NT_MS_ABI nt_GetShortPathNameW(const uint16_t *input, uint16_t *buffer, uint32_t size);
uint32_t NT_MS_ABI nt_GetShortPathNameA(const char *input, char *buffer, uint32_t size);
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
int32_t NT_MS_ABI nt_LockFile(uintptr_t handle, uint32_t offset_low, uint32_t offset_high,
                              uint32_t length_low, uint32_t length_high);
int32_t NT_MS_ABI nt_UnlockFile(uintptr_t handle, uint32_t offset_low, uint32_t offset_high,
                                uint32_t length_low, uint32_t length_high);
int32_t NT_MS_ABI nt_LockFileEx(uintptr_t handle, uint32_t flags, uint32_t reserved,
                                uint32_t length_low, uint32_t length_high, struct nt_overlapped *overlap);
int32_t NT_MS_ABI nt_UnlockFileEx(uintptr_t handle, uint32_t reserved, uint32_t length_low,
                                  uint32_t length_high, struct nt_overlapped *overlap);
int32_t NT_MS_ABI nt_GetOverlappedResult(uintptr_t handle, struct nt_overlapped *overlap,
                                         uint32_t *done, int32_t wait);
int32_t NT_MS_ABI nt_CancelIo(uintptr_t handle);
int32_t NT_MS_ABI nt_CancelIoEx(uintptr_t handle, struct nt_overlapped *overlap);
int32_t NT_MS_ABI nt_GetOverlappedResultEx(uintptr_t handle, struct nt_overlapped *overlap,
                                           uint32_t *done, uint32_t milliseconds, int32_t alertable);
uintptr_t NT_MS_ABI nt_CreateIoCompletionPort(uintptr_t file, uintptr_t existing,
                                               uintptr_t completion_key, uint32_t concurrency);
int32_t NT_MS_ABI nt_PostQueuedCompletionStatus(uintptr_t port, uint32_t bytes,
                                                uintptr_t completion_key, struct nt_overlapped *overlap);
int32_t NT_MS_ABI nt_GetQueuedCompletionStatus(uintptr_t port, uint32_t *bytes,
                                               uintptr_t *completion_key, struct nt_overlapped **overlap,
                                               uint32_t milliseconds);
/* Windows OVERLAPPED_ENTRY (x64): KEY, LPOVERLAPPED, Internal NTSTATUS, DWORD bytes.
 * The final 4 bytes are ABI tail padding (32 bytes total). */
struct nt_overlapped_entry {
    uintptr_t completion_key;
    struct nt_overlapped *overlap;
    uintptr_t internal;
    uint32_t bytes;
};
_Static_assert(sizeof(struct nt_overlapped_entry) == 32, "OVERLAPPED_ENTRY x64 layout");
int32_t NT_MS_ABI nt_GetQueuedCompletionStatusEx(uintptr_t port,
                                                 struct nt_overlapped_entry *entries,
                                                 uint32_t count, uint32_t *removed,
                                                 uint32_t milliseconds, int32_t alertable);
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
/* Narrow FSCTL_GET_REPARSE_POINT profile: explicit final symlink handle only,
 * relative Linux targets with no traversal; never exposes arbitrary host paths. */
int32_t NT_MS_ABI nt_DeviceIoControl(uintptr_t handle, uint32_t control,
                                    const void *input, uint32_t input_size,
                                    void *output, uint32_t output_size,
                                    uint32_t *returned, void *overlapped);
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
