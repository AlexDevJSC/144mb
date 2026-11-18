/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FS4_REMOTE_H
#define NTNATIVE_FS4_REMOTE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "compat.h"
struct nt_find_data_w;
struct nt_file_attribute_data;
struct nt_handle_file_info;

/* Win32 runtime-side, opt-in client of the dedicated-UID HANDLE broker.
 * No Linux backing FDs or paths are ever transferred to guest code. */
nt_status nt_fs4_remote_native_query(uintptr_t handle,void *buffer,
                                    uint32_t capacity,uint32_t kind,
                                    uint32_t *received);
nt_status nt_fs4_remote_native_set(uintptr_t handle,const void *buffer,
                                  uint32_t size,uint32_t kind);
nt_status nt_fs4_remote_native_flush(uintptr_t handle);
bool nt_fs4_remote_mode(void);
bool nt_fs4_remote_handle(uintptr_t handle);
bool nt_fs4_remote_search(uintptr_t handle);
bool nt_fs4_remote_valid(uintptr_t handle);
bool nt_fs4_remote_overlapped(uintptr_t handle);
int32_t nt_fs4_remote_get_flags(uintptr_t handle,uint32_t *flags);
int32_t nt_fs4_remote_set_flags(uintptr_t handle,uint32_t mask,uint32_t flags);
int32_t nt_fs4_remote_associate_iocp(uintptr_t handle,uintptr_t port,uintptr_t key);
int32_t nt_fs4_remote_completion(uintptr_t handle,uintptr_t *port,uintptr_t *key);
size_t nt_fs4_remote_count(void);
uintptr_t nt_fs4_remote_open(const char *path, uint32_t access,
                            uint32_t share, uint32_t disposition,
                            uint32_t flags);
int32_t nt_fs4_remote_io(uintptr_t handle, void *data, uint32_t length,
                         uint32_t *done, bool write);
int32_t nt_fs4_remote_close(uintptr_t handle);
uintptr_t nt_fs4_remote_duplicate(uintptr_t handle);
int32_t nt_fs4_remote_io_at(uintptr_t handle, void *data, uint32_t length,
                            uint32_t *done, bool write, uint64_t offset);
int32_t nt_fs4_remote_get_attributes(const char *path, struct nt_file_attribute_data *info);
int32_t nt_fs4_remote_set_attributes(const char *path, uint32_t flags);
uintptr_t nt_fs4_remote_find_first(const char *path, int32_t level,
                                   int32_t operation, uint32_t flags,
                                   struct nt_find_data_w *info);
int32_t nt_fs4_remote_find_next(uintptr_t handle, struct nt_find_data_w *info);
int32_t nt_fs4_remote_find_close(uintptr_t handle);
int32_t nt_fs4_remote_handle_info(uintptr_t handle, struct nt_handle_file_info *info);
int32_t nt_fs4_remote_delete(const char *path);
int32_t nt_fs4_remote_rename(const char *old_name, const char *new_name,
                             uint32_t flags);
int32_t nt_fs4_remote_seek(uintptr_t handle, int64_t offset, uint32_t method,
                           int64_t *position);
int32_t nt_fs4_remote_size(uintptr_t handle, int64_t *length);
int32_t nt_fs4_remote_truncate(uintptr_t handle);
int32_t nt_fs4_remote_lock(uintptr_t handle, uint64_t offset,
                           uint64_t length, bool unlock);
#endif
