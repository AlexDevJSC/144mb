/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_GRAPHICS_H
#define NT_RUNTIME_GRAPHICS_H

#include "ntnative/runtime.h"
#include <stddef.h>
#include <stdint.h>

int32_t NT_MS_ABI nt_D3D12CreateDevice(void *adapter, uint32_t minimum_level,
                                      const void *iid, void **device);
int32_t NT_MS_ABI nt_D3D12SerializeRootSignature(const void *desc, uint32_t version,
                                                void **blob, void **errors);
int32_t NT_MS_ABI nt_D3D12SerializeVersionedRootSignature(const void *desc, void **blob, void **errors);
int32_t NT_MS_ABI nt_CreateDXGIFactory(const void *iid, void **factory);
int32_t NT_MS_ABI nt_CreateDXGIFactory1(const void *iid, void **factory);
int32_t NT_MS_ABI nt_CreateDXGIFactory2(uint32_t flags, const void *iid, void **factory);

#endif
