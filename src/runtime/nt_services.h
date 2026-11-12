/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_NT_SERVICES_H
#define NTNATIVE_NT_SERVICES_H

#include "ntnative/counter.h"
#include "ntnative/runtime.h"

#include <time.h>

#define NT_STATUS_TIMEOUT INT32_C(0x00000102)
#define NT_STATUS_INVALID_HANDLE ((nt_status)UINT32_C(0xc0000008))
#define NT_STATUS_INVALID_PARAMETER ((nt_status)UINT32_C(0xc000000d))
#define NT_STATUS_NO_MEMORY ((nt_status)UINT32_C(0xc0000017))
#define NT_STATUS_ACCESS_DENIED ((nt_status)UINT32_C(0xc0000022))
#define NT_STATUS_OBJECT_TYPE_MISMATCH ((nt_status)UINT32_C(0xc0000024))
#define NT_STATUS_NOT_SUPPORTED ((nt_status)UINT32_C(0xc00000bb))

struct nt_guest_unicode_string;
struct nt_object_attributes {
    uint32_t size;
    uintptr_t root;
    const struct nt_guest_unicode_string *name;
    uint32_t attributes;
    const void *security, *qos;
};

struct nt_wait_deadline {
    struct timespec time;
    int clock;
};

nt_status nt_runtime_wait_deadline(int64_t ticks, struct nt_wait_deadline *deadline);
nt_status NT_MS_ABI nt_NtQuerySystemTime(int64_t *ticks);
nt_status NT_MS_ABI nt_NtDelayExecution(uint8_t alertable, const int64_t *interval);
nt_status NT_MS_ABI nt_NtClose(uintptr_t handle);
nt_status NT_MS_ABI nt_NtCreateEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes,
                                   uint32_t type, uint8_t initial_state);
nt_status NT_MS_ABI nt_NtSetEvent(uintptr_t handle, int32_t *previous);
nt_status NT_MS_ABI nt_NtResetEvent(uintptr_t handle, int32_t *previous);
nt_status NT_MS_ABI nt_NtWaitForSingleObject(uintptr_t handle, uint8_t alertable, const int64_t *timeout);
nt_status NT_MS_ABI nt_NtWaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                              uint32_t type, uint8_t alertable, const int64_t *timeout);
uint32_t nt_runtime_nt_error(nt_status status);

#endif
