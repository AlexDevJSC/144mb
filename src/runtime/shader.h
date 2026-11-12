/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_SHADER_H
#define NT_RUNTIME_SHADER_H

#include "ntnative/runtime.h"

#include <stddef.h>
#include <stdint.h>

bool nt_runtime_d3dcompiler_available(void);
int32_t NT_MS_ABI nt_D3DCompile(const void *source, size_t length, const char *filename,
                              const void *defines, void *include, const char *entry,
                              const char *profile, uint32_t flags, uint32_t effect_flags,
                              void **shader, void **errors);
bool nt_runtime_dxcompiler_available(void);
int32_t NT_MS_ABI nt_DxcCreateInstance(const void *class_id, const void *interface_id, void **object);

#endif
