/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_VIRTUAL_MEMORY_H
#define NTNATIVE_VIRTUAL_MEMORY_H

#include "compat.h"

/* The x64 Windows MEMORY_BASIC_INFORMATION ABI (48 bytes). */
struct nt_memory_basic_information {
    void *base_address;
    void *allocation_base;
    uint32_t allocation_protect;
    uint16_t partition_id;
    uint16_t reserved;
    size_t region_size;
    uint32_t state;
    uint32_t protect;
    uint32_t type;
};

void *NT_MS_ABI nt_VirtualAlloc(void *address, size_t size, uint32_t allocation_type,
                                uint32_t protection);
int32_t NT_MS_ABI nt_VirtualFree(void *address, size_t size, uint32_t free_type);
size_t NT_MS_ABI nt_VirtualQuery(const void *address, struct nt_memory_basic_information *info,
                                 size_t length);

/* Tri-state hook for VirtualProtect: -1 = not one of our allocations,
 * 0 = invalid access/protection, 1 = handled and changed successfully.
 * A -1 result lets the existing PE image protection logic run. */
int nt_virtual_protect_managed(void *address, size_t size, uint32_t protection, uint32_t *previous);

#endif
