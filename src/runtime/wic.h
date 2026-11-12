/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_WIC_H
#define NT_RUNTIME_WIC_H

#include "ntnative/runtime.h"
#include <stdint.h>

int32_t NT_MS_ABI nt_CoCreateInstance(const void *class_id, void *outer, uint32_t context,
                                      const void *iid, void **object);

#endif
