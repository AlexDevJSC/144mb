/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_PE_BYTES_H
#define NTNATIVE_PE_BYTES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint16_t read_u16(const unsigned char *field)
{
    return (uint16_t)(field[0] | (uint16_t)field[1] << 8);
}

static inline uint32_t read_u32(const unsigned char *field)
{
    return read_u16(field) | (uint32_t)read_u16(field + 2) << 16;
}

static inline uint64_t read_u64(const unsigned char *field)
{
    return read_u32(field) | (uint64_t)read_u32(field + 4) << 32;
}

static inline bool contains(size_t total, size_t offset, size_t length)
{
    return offset <= total && length <= total - offset;
}

#endif
