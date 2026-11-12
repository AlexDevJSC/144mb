/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_STREAMLINE_H
#define NT_RUNTIME_STREAMLINE_H

#include "ntnative/runtime.h"
#include <stdint.h>

int32_t NT_MS_ABI nt_slInit(const void *preferences, uint64_t sdk_version);
int32_t NT_MS_ABI nt_slShutdown(void);
int32_t NT_MS_ABI nt_slSetD3DDevice(void *device);
int32_t NT_MS_ABI nt_slGetNativeInterface(void *interface, void **native);
int32_t NT_MS_ABI nt_slGetNewFrameToken(void **token, const uint32_t *index);
int32_t NT_MS_ABI nt_slSetConstants(const void *constants, const void *frame, const void *viewport);
int32_t NT_MS_ABI nt_slSetTagForFrame(const void *frame, const void *viewport, const void *tags,
                                    uint32_t count, void *command_list);
int32_t NT_MS_ABI nt_slGetFeatureRequirements(uint32_t feature, void *requirements);
int32_t NT_MS_ABI nt_slIsFeatureSupported(uint32_t feature, const void *adapter);
int32_t NT_MS_ABI nt_slSetFeatureLoaded(uint32_t feature, uint8_t loaded);
int32_t NT_MS_ABI nt_slGetFeatureFunction(uint32_t feature, const char *name, void **function);
int32_t NT_MS_ABI nt_slFreeResources(uint32_t feature, const void *viewport);
int32_t NT_MS_ABI nt_slEvaluateFeature(uint32_t feature, const void *frame, const void *inputs,
                                      uint32_t count, void *command_list);

#endif
