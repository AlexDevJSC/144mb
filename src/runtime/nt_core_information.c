/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "nt_core_information.h"
#include "ntnative/guest_thread.h"
#include "winbase.h"
#include "sync.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

enum { NT_BASIC_INFORMATION_CLASS = 0, NT_PROCESS_WOW64_INFORMATION_CLASS = 26,
       NT_PAGE_SIZE = 4096, NT_VM_RELEASE = 0x8000,
       NT_VM_RESERVE = 0x2000, NT_VM_COMMIT = 0x1000 };

_Static_assert(sizeof(struct nt_process_basic_information) == 48,
               "PROCESS_BASIC_INFORMATION AMD64 layout");
_Static_assert(offsetof(struct nt_process_basic_information, peb_base) == 8,
               "PROCESS_BASIC_INFORMATION PEB offset");
_Static_assert(sizeof(struct nt_thread_basic_information) == 48,
               "THREAD_BASIC_INFORMATION AMD64 layout");
_Static_assert(offsetof(struct nt_thread_basic_information, client_id) == 16,
               "THREAD_BASIC_INFORMATION ClientId offset");

static nt_status local_process(uintptr_t process, uint32_t required_access)
{
    /* Real handles from OpenProcess(self) obey the class-specific rights. */
    return nt_runtime_check_self_process(process, required_access);
}

static bool local_thread(uintptr_t thread)
{
    return thread == UINTPTR_MAX - 1; /* NtCurrentThread pseudo-handle */
}

void *NT_MS_ABI nt_RtlGetCurrentPeb(void)
{
    const unsigned char *teb = nt_guest_thread_teb();
    uintptr_t result = 0;
    if (teb != NULL)
        memcpy(&result, teb + 0x60, sizeof(result));
    return (void *)result;
}

static nt_status require_buffer(void *buffer, uint32_t length, uint32_t needed,
                                uint32_t *returned_length)
{
    if (returned_length != NULL)
        *returned_length = needed;
    if (buffer == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length < needed)
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtQueryInformationProcess(uintptr_t process, uint32_t info_class,
                                                  void *buffer, uint32_t length,
                                                  uint32_t *returned_length)
{
    const nt_status access = local_process(process, 0x1400u);
    if (access != NT_STATUS_SUCCESS)
        return access;
    if (info_class == NT_PROCESS_WOW64_INFORMATION_CLASS) {
        const nt_status status = require_buffer(buffer, length, sizeof(uintptr_t), returned_length);
        if (status == NT_STATUS_SUCCESS) {
            const uintptr_t absent = 0; /* AMD64 is not running a WOW64 guest. */
            memcpy(buffer, &absent, sizeof(absent));
        }
        return status;
    }
    if (info_class != NT_BASIC_INFORMATION_CLASS)
        return NT_STATUS_INVALID_INFO_CLASS;
    void *peb = nt_RtlGetCurrentPeb();
    if (peb == NULL)
        return NT_STATUS_NOT_SUPPORTED;
    const nt_status status = require_buffer(buffer, length,
                                            sizeof(struct nt_process_basic_information), returned_length);
    if (status != NT_STATUS_SUCCESS)
        return status;
    const struct nt_process_basic_information result = {
        .exit_status = 259, /* STILL_ACTIVE */
        .peb_base = peb,
        .affinity_mask = 1,
        .base_priority = 8,
        .unique_process_id = (uintptr_t)getpid(),
        .inherited_from_unique_process_id = (uintptr_t)getppid()
    };
    memcpy(buffer, &result, sizeof(result));
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtQueryInformationThread(uintptr_t thread, uint32_t info_class,
                                                 void *buffer, uint32_t length,
                                                 uint32_t *returned_length)
{
    if (info_class != NT_BASIC_INFORMATION_CLASS)
        return NT_STATUS_INVALID_INFO_CLASS;
    uintptr_t teb = 0;
    uint32_t tid = 0, exit_status = 259;
    if (local_thread(thread)) {
        teb = (uintptr_t)nt_guest_thread_teb();
        if (teb == 0)
            return NT_STATUS_NOT_SUPPORTED;
        tid = (uint32_t)syscall(SYS_gettid);
    } else {
        const nt_status status = nt_runtime_query_thread(thread, &tid, &teb, &exit_status);
        if (status != NT_STATUS_SUCCESS)
            return status;
    }
    const nt_status status = require_buffer(buffer, length,
                                             sizeof(struct nt_thread_basic_information), returned_length);
    if (status != NT_STATUS_SUCCESS)
        return status;
    uintptr_t mask = 0;
    int32_t base_priority = 0;
    const nt_status sched_status = nt_runtime_query_thread_scheduling(thread, &mask, &base_priority);
    if (sched_status != NT_STATUS_SUCCESS)
        return sched_status;
    const struct nt_thread_basic_information result = {
        .exit_status = (nt_status)exit_status,
        .teb_base = (void *)teb,
        .client_id = {(uintptr_t)getpid(), (uintptr_t)tid},
        .affinity_mask = mask,
        .priority = base_priority,
        .base_priority = base_priority
    };
    memcpy(buffer, &result, sizeof(result));
    return NT_STATUS_SUCCESS;
}

/* Nt* functions do not alter Win32 LastError. Their backend is the same
 * tracked guest memory manager as the Virtual* API, not arbitrary host mmap.
 * The wrappers reject unsupported semantics rather than silently emulating
 * unrecognized process handles or ZeroBits. */
static nt_status memory_result(uint32_t error)
{
    switch (error) {
    case NT_ERROR_INVALID_ADDRESS: return NT_STATUS_CONFLICTING_ADDRESSES;
    case NT_ERROR_INVALID_PARAMETER: return NT_STATUS_INVALID_PARAMETER;
    case NT_ERROR_NOT_ENOUGH_MEMORY: return NT_STATUS_NO_MEMORY;
    case NT_ERROR_ACCESS_DENIED: return NT_STATUS_ACCESS_DENIED;
    case NT_ERROR_NOT_SUPPORTED: return NT_STATUS_NOT_SUPPORTED;
    default: return NT_STATUS_UNSUCCESSFUL;
    }
}

static bool page_round(size_t size, size_t *rounded)
{
    if (size == 0 || size > SIZE_MAX - (NT_PAGE_SIZE - 1))
        return false;
    *rounded = (size + NT_PAGE_SIZE - 1) & ~(size_t)(NT_PAGE_SIZE - 1);
    return true;
}

nt_status NT_MS_ABI nt_NtAllocateVirtualMemory(uintptr_t process, void **base, uintptr_t zero_bits,
                                                size_t *size, uint32_t type, uint32_t protection)
{
    const nt_status access = local_process(process, 0x8u);
    if (access != NT_STATUS_SUCCESS)
        return access;
    if (base == NULL || size == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    size_t rounded = 0;
    if (zero_bits != 0 || !page_round(*size, &rounded) ||
        (type & ~(NT_VM_RESERVE | NT_VM_COMMIT)) != 0 ||
        (type & (NT_VM_RESERVE | NT_VM_COMMIT)) == 0)
        return NT_STATUS_INVALID_PARAMETER;
    const uint32_t preserved = nt_GetLastError();
    void *allocated = nt_VirtualAlloc(*base, *size, type, protection);
    const uint32_t error = allocated == NULL ? nt_GetLastError() : NT_ERROR_SUCCESS;
    nt_SetLastError(preserved);
    if (allocated == NULL)
        return memory_result(error);
    *base = allocated;
    *size = rounded;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtFreeVirtualMemory(uintptr_t process, void **base,
                                            size_t *size, uint32_t type)
{
    const nt_status access = local_process(process, 0x8u);
    if (access != NT_STATUS_SUCCESS)
        return access;
    if (base == NULL || size == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (type != NT_VM_RELEASE && type != 0x4000)
        return NT_STATUS_INVALID_PARAMETER;
    size_t rounded = 0;
    if ((type == NT_VM_RELEASE && *size != 0) ||
        (type != NT_VM_RELEASE && !page_round(*size, &rounded)))
        return NT_STATUS_INVALID_PARAMETER;
    const uint32_t preserved = nt_GetLastError();
    const bool freed = nt_VirtualFree(*base, *size, type) != 0;
    const uint32_t error = freed ? NT_ERROR_SUCCESS : nt_GetLastError();
    nt_SetLastError(preserved);
    if (!freed)
        return memory_result(error);
    if (type == NT_VM_RELEASE)
        *size = 0;
    else
        *size = rounded;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtProtectVirtualMemory(uintptr_t process, void **base, size_t *size,
                                               uint32_t protection, uint32_t *previous)
{
    const nt_status access = local_process(process, 0x8u);
    if (access != NT_STATUS_SUCCESS)
        return access;
    if (base == NULL || size == NULL || previous == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    const uintptr_t raw = (uintptr_t)*base;
    const uintptr_t aligned = raw & ~(uintptr_t)(NT_PAGE_SIZE - 1);
    const size_t delta = raw - aligned;
    size_t rounded = 0;
    if (*base == NULL || *size > SIZE_MAX - delta ||
        !page_round(*size + delta, &rounded))
        return NT_STATUS_INVALID_PARAMETER;
    const uint32_t preserved = nt_GetLastError();
    const bool protected = nt_VirtualProtect(*base, *size, protection, previous) != 0;
    const uint32_t error = protected ? NT_ERROR_SUCCESS : nt_GetLastError();
    nt_SetLastError(preserved);
    if (!protected)
        return memory_result(error);
    *base = (void *)aligned;
    *size = rounded;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtQueryVirtualMemory(uintptr_t process, const void *address,
                                             uint32_t info_class, void *buffer,
                                             size_t length, size_t *returned_length)
{
    const nt_status access = local_process(process, 0x410u);
    if (access != NT_STATUS_SUCCESS)
        return access;
    if (info_class != NT_BASIC_INFORMATION_CLASS)
        return NT_STATUS_INVALID_INFO_CLASS;
    if (returned_length != NULL)
        *returned_length = sizeof(struct nt_memory_basic_information);
    if (buffer == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (length < sizeof(struct nt_memory_basic_information))
        return NT_STATUS_INFO_LENGTH_MISMATCH;
    const uint32_t preserved = nt_GetLastError();
    const size_t copied = nt_VirtualQuery(address, buffer, length);
    const uint32_t error = copied ? NT_ERROR_SUCCESS : nt_GetLastError();
    nt_SetLastError(preserved);
    return copied ? NT_STATUS_SUCCESS : memory_result(error);
}
