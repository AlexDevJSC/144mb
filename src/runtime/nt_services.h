/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_NT_SERVICES_H
#define NTNATIVE_NT_SERVICES_H

#include "ntnative/counter.h"
#include "ntnative/runtime.h"

#include <time.h>

#define NT_STATUS_USER_APC INT32_C(0x000000c0)
#define NT_STATUS_SUSPEND_COUNT_EXCEEDED ((nt_status)UINT32_C(0xc000004a))
#define NT_STATUS_THREAD_IS_TERMINATING ((nt_status)UINT32_C(0xc000004b))
#define NT_STATUS_TIMEOUT INT32_C(0x00000102)
#define NT_STATUS_INVALID_INFO_CLASS ((nt_status)UINT32_C(0xc0000003))
#define NT_STATUS_INFO_LENGTH_MISMATCH ((nt_status)UINT32_C(0xc0000004))
#define NT_STATUS_CONFLICTING_ADDRESSES ((nt_status)UINT32_C(0xc0000018))
#define NT_STATUS_OBJECT_NAME_EXISTS ((nt_status)UINT32_C(0x40000000))
#define NT_STATUS_OBJECT_NAME_NOT_FOUND ((nt_status)UINT32_C(0xc0000034))
#define NT_STATUS_INVALID_HANDLE ((nt_status)UINT32_C(0xc0000008))
#define NT_STATUS_INVALID_CID ((nt_status)UINT32_C(0xc000000b))
#define NT_STATUS_INVALID_PARAMETER ((nt_status)UINT32_C(0xc000000d))
#define NT_STATUS_NO_MEMORY ((nt_status)UINT32_C(0xc0000017))
#define NT_STATUS_ACCESS_DENIED ((nt_status)UINT32_C(0xc0000022))
#define NT_STATUS_OBJECT_TYPE_MISMATCH ((nt_status)UINT32_C(0xc0000024))
#define NT_STATUS_SEMAPHORE_LIMIT_EXCEEDED ((nt_status)UINT32_C(0xc0000047))
#define NT_STATUS_NOT_SUPPORTED ((nt_status)UINT32_C(0xc00000bb))
#define NT_STATUS_END_OF_FILE ((nt_status)UINT32_C(0xc0000011))

struct nt_guest_unicode_string;
struct nt_object_attributes {
    uint32_t size;
    uintptr_t root;
    const struct nt_guest_unicode_string *name;
    uint32_t attributes;
    const void *security, *qos;
};

struct nt_io_status_block {
    nt_status status;
    uintptr_t information;
};

nt_status NT_MS_ABI nt_NtCreateFile(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, struct nt_io_status_block *io,
    const int64_t *allocation, uint32_t file_attributes, uint32_t share,
    uint32_t disposition, uint32_t options, const void *ea, uint32_t ea_length);
nt_status NT_MS_ABI nt_NtOpenFile(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, struct nt_io_status_block *io,
    uint32_t share, uint32_t options);

/* Synchronous file I/O for handles held by the shared Win32 file table.
 * These are the native NT argument layouts, with unsupported asynchronous
 * event/APC requests rejected explicitly. */
nt_status NT_MS_ABI nt_NtReadFile(uintptr_t handle, uintptr_t event, void *apc,
    void *apc_context, struct nt_io_status_block *io, void *buffer, uint32_t length,
    const int64_t *offset, const uint32_t *key);
nt_status NT_MS_ABI nt_NtWriteFile(uintptr_t handle, uintptr_t event, void *apc,
    void *apc_context, struct nt_io_status_block *io, const void *buffer, uint32_t length,
    const int64_t *offset, const uint32_t *key);
nt_status NT_MS_ABI nt_NtQueryInformationFile(uintptr_t handle,
    struct nt_io_status_block *io, void *buffer, uint32_t length, uint32_t information_class);
nt_status NT_MS_ABI nt_NtSetInformationFile(uintptr_t handle,
    struct nt_io_status_block *io, const void *buffer, uint32_t length, uint32_t information_class);
nt_status NT_MS_ABI nt_NtFlushBuffersFile(uintptr_t handle, struct nt_io_status_block *io);

struct nt_wait_deadline {
    struct timespec time;
    int clock;
};

nt_status nt_runtime_wait_deadline(int64_t ticks, struct nt_wait_deadline *deadline);
nt_status NT_MS_ABI nt_NtQuerySystemTime(int64_t *ticks);
nt_status NT_MS_ABI nt_NtDelayExecution(uint8_t alertable, const int64_t *interval);
nt_status nt_runtime_alertable_delay(const struct nt_wait_deadline *deadline);
nt_status NT_MS_ABI nt_NtQueueApcThread(uintptr_t thread,
    void (NT_MS_ABI *routine)(void *, void *, void *),
    void *context, void *first, void *second);
nt_status NT_MS_ABI nt_NtTestAlert(void);
nt_status NT_MS_ABI nt_NtClose(uintptr_t handle);
nt_status NT_MS_ABI nt_NtDuplicateObject(uintptr_t source_process, uintptr_t source,
                                        uintptr_t target_process, uintptr_t *target,
                                        uint32_t access, uint32_t attributes, uint32_t options);
nt_status NT_MS_ABI nt_NtCreateEvent(uintptr_t *handle, uint32_t access,
                                   const struct nt_object_attributes *attributes,
                                   uint32_t type, uint8_t initial_state);
nt_status NT_MS_ABI nt_NtOpenEvent(uintptr_t *handle, uint32_t access,
                                  const struct nt_object_attributes *attributes);
nt_status NT_MS_ABI nt_NtQueryEvent(uintptr_t handle, uint32_t information_class,
                                   void *buffer, uint32_t length, uint32_t *returned_length);
nt_status NT_MS_ABI nt_NtCreateSemaphore(uintptr_t *handle, uint32_t access,
                                         const struct nt_object_attributes *attributes,
                                         int32_t initial, int32_t maximum);
nt_status NT_MS_ABI nt_NtOpenSemaphore(uintptr_t *handle, uint32_t access,
                                       const struct nt_object_attributes *attributes);
nt_status NT_MS_ABI nt_NtQuerySemaphore(uintptr_t handle, uint32_t information_class,
                                        void *buffer, uint32_t length, uint32_t *returned_length);
nt_status NT_MS_ABI nt_NtReleaseSemaphore(uintptr_t handle, int32_t count, int32_t *previous);
struct nt_client_id;
nt_status NT_MS_ABI nt_NtTerminateProcess(uintptr_t process, nt_status exit_code);
struct nt_client_id;
nt_status NT_MS_ABI nt_NtOpenProcess(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, const struct nt_client_id *client_id);
typedef uint32_t (NT_MS_ABI *nt_guest_start_routine)(void *);
nt_status NT_MS_ABI nt_NtCreateThreadEx(uintptr_t *handle, uint32_t desired_access,
    const struct nt_object_attributes *attributes, uintptr_t process,
    nt_guest_start_routine start, void *argument, uint32_t flags, size_t zero_bits,
    size_t stack_commit, size_t stack_reserve, void *attribute_list);
nt_status NT_MS_ABI nt_NtTerminateThread(uintptr_t thread, nt_status exit_code);
nt_status NT_MS_ABI nt_NtOpenThread(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, const struct nt_client_id *client_id);
nt_status NT_MS_ABI nt_NtSetInformationThread(uintptr_t thread, uint32_t info_class,
                                              const void *buffer, uint32_t length);
nt_status NT_MS_ABI nt_NtSuspendThread(uintptr_t handle, uint32_t *previous);
nt_status NT_MS_ABI nt_NtResumeThread(uintptr_t handle, uint32_t *previous);
nt_status NT_MS_ABI nt_NtSetEvent(uintptr_t handle, int32_t *previous);
nt_status NT_MS_ABI nt_NtResetEvent(uintptr_t handle, int32_t *previous);
nt_status NT_MS_ABI nt_NtWaitForSingleObject(uintptr_t handle, uint8_t alertable, const int64_t *timeout);
nt_status NT_MS_ABI nt_NtWaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                              uint32_t type, uint8_t alertable, const int64_t *timeout);
uint32_t nt_runtime_nt_error(nt_status status);

#endif
