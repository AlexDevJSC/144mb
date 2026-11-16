/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_SECTION_MEMORY_H
#define NTNATIVE_SECTION_MEMORY_H

#include "virtual_memory.h"
#include "nt_services.h"

/* Initial pagefile-backed mapping profile. No disk-backed mapping, no
 * global cross-process object broker or arbitrary executable views yet. */
uintptr_t NT_MS_ABI nt_CreateFileMappingW(uintptr_t file, const void *security,
    uint32_t protect, uint32_t maximum_high, uint32_t maximum_low, const uint16_t *name);
uintptr_t NT_MS_ABI nt_OpenFileMappingW(uint32_t access, int32_t inherit, const uint16_t *name);
void *NT_MS_ABI nt_MapViewOfFile(uintptr_t mapping, uint32_t access,
    uint32_t offset_high, uint32_t offset_low, size_t length);
int32_t NT_MS_ABI nt_UnmapViewOfFile(const void *address);
int32_t NT_MS_ABI nt_FlushViewOfFile(const void *address, size_t length);

/* Native section subset: anonymous committed memory in the current process.
 * These use NTSTATUS and preserve Win32 LastError. */
nt_status NT_MS_ABI nt_NtCreateSection(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, const int64_t *maximum,
    uint32_t protect, uint32_t allocation, uintptr_t file);
nt_status NT_MS_ABI nt_NtMapViewOfSection(uintptr_t section, uintptr_t process,
    void **base, uintptr_t zero_bits, size_t commit_size, const int64_t *offset,
    size_t *view_size, uint32_t inherit, uint32_t allocation, uint32_t protect);
nt_status NT_MS_ABI nt_NtUnmapViewOfSection(uintptr_t process, const void *base);

/* Hooks into the existing Windows HANDLE and MEM_MAPPED bookkeeping. */
int nt_runtime_section_close(uintptr_t handle);
nt_status nt_runtime_section_duplicate(uintptr_t source, uint32_t access,
    uint32_t attributes, uint32_t options, uintptr_t *target);
bool nt_runtime_section_get_flags(uintptr_t handle, uint32_t *flags);
bool nt_runtime_section_set_flags(uintptr_t handle, uint32_t mask, uint32_t flags);
size_t nt_runtime_section_handle_count(void);
bool nt_runtime_section_query(const void *address, struct nt_memory_basic_information *info);
#endif
