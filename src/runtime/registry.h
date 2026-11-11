/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_REGISTRY_H
#define NTNATIVE_REGISTRY_H
#include "compat.h"

int32_t NT_MS_ABI nt_RegOpenKeyExW(uintptr_t base, const uint16_t *subkey, uint32_t options,
                                   uint32_t access, uintptr_t *result);
int32_t NT_MS_ABI nt_RegQueryValueExW(uintptr_t key, const uint16_t *name, uint32_t *reserved,
                                      uint32_t *type, unsigned char *data, uint32_t *size);
int32_t NT_MS_ABI nt_RegCloseKey(uintptr_t key);
#endif
