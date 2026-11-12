/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_EXCEPTION_CALLBACKS_H
#define NTNATIVE_EXCEPTION_CALLBACKS_H

#include "compat.h"

struct nt_exception_pointers;
typedef int32_t (NT_MS_ABI *nt_vectored_exception_handler)(struct nt_exception_pointers *);

void *NT_MS_ABI nt_AddVectoredExceptionHandler(uint32_t first, nt_vectored_exception_handler callback);
uint32_t NT_MS_ABI nt_RemoveVectoredExceptionHandler(void *handle);

/* Called by the existing AMD64 software-exception dispatcher before SEH
 * frame handling; returns EXCEPTION_CONTINUE_EXECUTION (-1) or SEARCH (0). */
int32_t nt_runtime_dispatch_vectored_exception(struct nt_exception_pointers *pointers);

#endif
