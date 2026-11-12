/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_WINBASE_H
#define NTNATIVE_WINBASE_H

#include "compat.h"
#include "ntnative/guest_thread.h"
#include "virtual_memory.h"

bool nt_runtime_enter_guest_thread(struct nt_guest_thread *thread, uintptr_t module,
                                   const struct nt_guest_tls *initial);

void NT_MS_ABI nt_GetSystemTime(struct nt_system_time *result);
uint64_t NT_MS_ABI nt_GetTickCount64(void);
void NT_MS_ABI nt_Sleep(uint32_t milliseconds);
void NT_MS_ABI nt_ExitProcess(uint32_t code);
char *NT_MS_ABI nt_GetCommandLineA(void);
uint16_t *NT_MS_ABI nt_GetCommandLineW(void);
uint32_t NT_MS_ABI nt_GetModuleFileNameW(uintptr_t module, uint16_t *buffer, uint32_t size);
void nt_runtime_set_guest_image_extent(size_t size);
void nt_runtime_set_guest_image_entrypoint(uint32_t rva);
void nt_runtime_set_guest_tls(const struct nt_guest_tls *initial, const unsigned char *callbacks, size_t count);
const struct nt_guest_tls *nt_runtime_guest_tls(void);
void nt_runtime_notify_tls(uint32_t reason);
uintptr_t nt_runtime_guest_image_at(uintptr_t address);
uintptr_t NT_MS_ABI nt_RtlPcToFileHeader(const void *address, uintptr_t *image);

int32_t NT_MS_ABI nt_VirtualProtect(void *address, size_t size, uint32_t protection, uint32_t *previous);
struct nt_os_version_info {
    uint32_t size, major, minor, build, platform;
    uint16_t service_pack[128], sp_major, sp_minor, suite;
    uint8_t product, reserved;
};
uint64_t NT_MS_ABI nt_VerSetConditionMask(uint64_t mask, uint32_t fields, uint8_t condition);
int32_t NT_MS_ABI nt_VerifyVersionInfoW(const struct nt_os_version_info *version,
                                       uint32_t fields, uint64_t mask);

uintptr_t NT_MS_ABI nt_LocalAlloc(uint32_t flags, size_t size);
uintptr_t NT_MS_ABI nt_GetProcessHeap(void);
void *NT_MS_ABI nt_HeapAlloc(uintptr_t heap, uint32_t flags, size_t size);
void *NT_MS_ABI nt_HeapReAlloc(uintptr_t heap, uint32_t flags, void *memory, size_t size);
size_t NT_MS_ABI nt_HeapSize(uintptr_t heap, uint32_t flags, const void *memory);
int32_t NT_MS_ABI nt_HeapFree(uintptr_t heap, uint32_t flags, void *memory);
uint32_t NT_MS_ABI nt_FormatMessageW(uint32_t flags, const void *source, uint32_t code,
                                    uint32_t language, uint16_t *buffer, uint32_t size, void *arguments);
uint16_t **NT_MS_ABI nt_CommandLineToArgvW(const uint16_t *command, int32_t *count);
int32_t NT_MS_ABI nt_CoInitializeEx(void *reserved, uint32_t mode);
void NT_MS_ABI nt_CoUninitialize(void);
bool nt_runtime_com_initialized(void);

void nt_runtime_set_win_environment(const char *const *source);
uint32_t NT_MS_ABI nt_GetEnvironmentVariableA(const char *name, char *buffer, uint32_t size);
uint32_t NT_MS_ABI nt_GetEnvironmentVariableW(const uint16_t *name, uint16_t *buffer, uint32_t size);
int32_t NT_MS_ABI nt_SetEnvironmentVariableW(const uint16_t *name, const uint16_t *value);
uint16_t *NT_MS_ABI nt_GetEnvironmentStringsW(void);
int32_t NT_MS_ABI nt_FreeEnvironmentStringsW(uint16_t *block);
uint32_t NT_MS_ABI nt_GetTempPathW(uint32_t capacity, uint16_t *buffer);
uint32_t NT_MS_ABI nt_GetSystemDirectoryW(uint16_t *buffer, uint32_t capacity);
int nt_runtime_open_guest_file_readonly(const char *path);
int nt_runtime_read_guest_file(const char *path, size_t limit, void **data, size_t *size);

#endif
