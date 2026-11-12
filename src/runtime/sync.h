/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_SYNC_H
#define NTNATIVE_SYNC_H

#include "compat.h"

typedef uint32_t (NT_MS_ABI *nt_thread_start)(void *);
typedef void (NT_MS_ABI *nt_fls_callback)(void *);

void NT_MS_ABI nt_InitializeCriticalSection(void *section);
int32_t NT_MS_ABI nt_InitializeCriticalSectionEx(void *section, uint32_t spin, uint32_t flags);
void NT_MS_ABI nt_EnterCriticalSection(void *section);
void NT_MS_ABI nt_LeaveCriticalSection(void *section);
void NT_MS_ABI nt_DeleteCriticalSection(void *section);
void NT_MS_ABI nt_AcquireSRWLockExclusive(void *lock);
uint8_t NT_MS_ABI nt_TryAcquireSRWLockExclusive(void *lock);
void NT_MS_ABI nt_ReleaseSRWLockExclusive(void *lock);
int32_t NT_MS_ABI nt_SleepConditionVariableSRW(void *condition, void *lock, uint32_t timeout, uint32_t flags);
void NT_MS_ABI nt_WakeConditionVariable(void *condition);
void NT_MS_ABI nt_WakeAllConditionVariable(void *condition);
int32_t NT_MS_ABI nt_InitOnceBeginInitialize(void *once, uint32_t flags, int32_t *pending, void **context);
int32_t NT_MS_ABI nt_InitOnceComplete(void *once, uint32_t flags, void *context);

uintptr_t NT_MS_ABI nt_CreateEventW(const void *security, int32_t manual, int32_t initial, const uint16_t *name);
int32_t NT_MS_ABI nt_SetEvent(uintptr_t event);
int32_t NT_MS_ABI nt_ResetEvent(uintptr_t event);
uint32_t NT_MS_ABI nt_WaitForSingleObject(uintptr_t handle, uint32_t milliseconds);
uint32_t NT_MS_ABI nt_WaitForSingleObjectEx(uintptr_t handle, uint32_t milliseconds, int32_t alertable);
uint32_t NT_MS_ABI nt_WaitForMultipleObjects(uint32_t count, const uintptr_t *handles,
                                          int32_t all, uint32_t milliseconds);
uint32_t NT_MS_ABI nt_WaitForMultipleObjectsEx(uint32_t count, const uintptr_t *handles,
                                            int32_t all, uint32_t milliseconds, int32_t alertable);
bool nt_runtime_close_waitable(uintptr_t handle);
bool nt_runtime_get_waitable_flags(uintptr_t handle, uint32_t *flags);
nt_status nt_runtime_duplicate_waitable(uintptr_t source, uint32_t access,
                                         uint32_t attributes, uint32_t options,
                                         uintptr_t *target);
uintptr_t NT_MS_ABI nt_GetCurrentThread(void);
uintptr_t NT_MS_ABI nt_CreateThread(const void *security, size_t stack, nt_thread_start start,
                                   void *parameter, uint32_t flags, uint32_t *thread_id);
void NT_MS_ABI nt_ExitThread(uint32_t code);
void NT_MS_ABI nt_FreeLibraryAndExitThread(uintptr_t module, uint32_t code);
int32_t NT_MS_ABI nt_GetExitCodeThread(uintptr_t thread, uint32_t *code);
int32_t NT_MS_ABI nt_SetThreadDescription(uintptr_t thread, const uint16_t *description);
uint32_t NT_MS_ABI nt_FlsAlloc(nt_fls_callback callback);
int32_t NT_MS_ABI nt_FlsFree(uint32_t index);
void *NT_MS_ABI nt_FlsGetValue(uint32_t index);
int32_t NT_MS_ABI nt_FlsSetValue(uint32_t index, void *value);
void nt_runtime_run_fls_cleanup(void);

#endif
