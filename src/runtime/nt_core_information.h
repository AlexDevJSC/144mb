/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_NT_CORE_INFORMATION_H
#define NTNATIVE_NT_CORE_INFORMATION_H

#include "nt_services.h"
#include "virtual_memory.h"

/* The x64 layouts returned for ProcessBasicInformation and ThreadBasicInformation.
 * The public Windows ABI uses 64-bit pointers and ULONG_PTR in both records.
 */
struct nt_process_basic_information {
    nt_status exit_status;
    void *peb_base;
    uintptr_t affinity_mask;
    int32_t base_priority;
    uintptr_t unique_process_id;
    uintptr_t inherited_from_unique_process_id;
};

struct nt_client_id {
    uintptr_t unique_process;
    uintptr_t unique_thread;
};

struct nt_thread_basic_information {
    nt_status exit_status;
    void *teb_base;
    struct nt_client_id client_id;
    uintptr_t affinity_mask;
    int32_t priority, base_priority;
};

nt_status NT_MS_ABI nt_NtQueryInformationProcess(uintptr_t process, uint32_t info_class,
                                                  void *buffer, uint32_t length,
                                                  uint32_t *returned_length);
nt_status NT_MS_ABI nt_NtQueryInformationThread(uintptr_t thread, uint32_t info_class,
                                                 void *buffer, uint32_t length,
                                                 uint32_t *returned_length);
void *NT_MS_ABI nt_RtlGetCurrentPeb(void);

nt_status NT_MS_ABI nt_NtAllocateVirtualMemory(uintptr_t process, void **base, uintptr_t zero_bits,
                                                size_t *size, uint32_t type, uint32_t protection);
nt_status NT_MS_ABI nt_NtFreeVirtualMemory(uintptr_t process, void **base,
                                            size_t *size, uint32_t type);
nt_status NT_MS_ABI nt_NtProtectVirtualMemory(uintptr_t process, void **base, size_t *size,
                                               uint32_t protection, uint32_t *previous);
nt_status NT_MS_ABI nt_NtQueryVirtualMemory(uintptr_t process, const void *address,
                                             uint32_t info_class, void *buffer,
                                             size_t length, size_t *returned_length);
#endif
