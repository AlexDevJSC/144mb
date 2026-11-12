/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_CRYPTO_H
#define NTNATIVE_CRYPTO_H

#include "compat.h"

int32_t NT_MS_ABI nt_BCryptOpenAlgorithmProvider(uintptr_t *handle, const uint16_t *id,
                                                const uint16_t *implementation, uint32_t flags);
int32_t NT_MS_ABI nt_BCryptGetProperty(uintptr_t handle, const uint16_t *property,
                                      unsigned char *buffer, uint32_t size, uint32_t *needed, uint32_t flags);
int32_t NT_MS_ABI nt_BCryptCloseAlgorithmProvider(uintptr_t handle, uint32_t flags);
int32_t NT_MS_ABI nt_BCryptCreateHash(uintptr_t algorithm, uintptr_t *handle, unsigned char *object,
                                      uint32_t object_size, const unsigned char *key, uint32_t key_size,
                                      uint32_t flags);
int32_t NT_MS_ABI nt_BCryptHashData(uintptr_t handle, const unsigned char *data, uint32_t size, uint32_t flags);
int32_t NT_MS_ABI nt_BCryptFinishHash(uintptr_t handle, unsigned char *digest, uint32_t size, uint32_t flags);
int32_t NT_MS_ABI nt_BCryptDestroyHash(uintptr_t handle);

#endif
