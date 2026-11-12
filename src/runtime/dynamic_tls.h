/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_DYNAMIC_TLS_H
#define NTNATIVE_DYNAMIC_TLS_H

#include "compat.h"

uint32_t NT_MS_ABI nt_TlsAlloc(void);
int32_t NT_MS_ABI nt_TlsFree(uint32_t index);
void *NT_MS_ABI nt_TlsGetValue(uint32_t index);
int32_t NT_MS_ABI nt_TlsSetValue(uint32_t index, void *value);

#endif
