/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "fixture_abi.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

extern const unsigned char nt_pe_fixture_bytes[];
extern const size_t nt_pe_fixture_size;

struct pe_code {
    const unsigned char *bytes;
    size_t length;
    size_t entry_offset;
};

static uint16_t read_u16(const unsigned char *field)
{
    return (uint16_t)(field[0] | (uint16_t)field[1] << 8);
}

static uint32_t read_u32(const unsigned char *field)
{
    return read_u16(field) | (uint32_t)read_u16(field + 2) << 16;
}

static bool contains(size_t total, size_t offset, size_t length)
{
    return offset <= total && length <= total - offset;
}

/* Recognize the subset emitted for our embedded, position-independent
 * fixture. This parser never receives a path or arbitrary executable bytes.
 */
static bool fixture_code(const unsigned char *image, size_t size, struct pe_code *code)
{
    enum {
        COFF_SIZE = 20, SECTION_SIZE = 40, OPTIONAL_FIXED = 112,
        DIRECTORY_SIZE = 8, DIRECTORY_COUNT = 16
    };
    if (!contains(size, 0, 64) || memcmp(image, "MZ", 2) != 0)
        return false;

    const size_t pe_offset = read_u32(image + 0x3c);
    if (pe_offset < 64 || !contains(size, pe_offset, 4 + COFF_SIZE) ||
        memcmp(image + pe_offset, "PE\0\0", 4) != 0)
        return false;

    const unsigned char *coff = image + pe_offset + 4;
    const uint16_t flags = read_u16(coff + 18);
    const size_t optional_size = read_u16(coff + 16);
    const size_t optional_offset = pe_offset + 4 + COFF_SIZE;
    if (read_u16(coff) != 0x8664 || (flags & 0x2002) != 0x0002 ||
        read_u16(coff + 2) != 1 ||
        optional_size != OPTIONAL_FIXED + DIRECTORY_COUNT * DIRECTORY_SIZE ||
        !contains(size, optional_offset, optional_size))
        return false;

    const unsigned char *optional = image + optional_offset;
    if (read_u16(optional) != 0x020b || read_u32(optional + 108) != DIRECTORY_COUNT)
        return false;

    for (size_t offset = OPTIONAL_FIXED; offset < optional_size; offset += 4) {
        if (read_u32(optional + offset) != 0)
            return false;
    }

    const uint32_t entry = read_u32(optional + 16);
    const size_t table_offset = optional_offset + optional_size;
    if (!contains(size, table_offset, SECTION_SIZE))
        return false;

    const unsigned char *section = image + table_offset;
    if (memcmp(section, ".text\0\0\0", 8) != 0)
        return false;

    const uint32_t virtual_size = read_u32(section + 8);
    const uint32_t rva = read_u32(section + 12);
    const uint32_t raw_size = read_u32(section + 16);
    const uint32_t raw_offset = read_u32(section + 20);
    const uint32_t section_flags = read_u32(section + 36);
    if ((section_flags & UINT32_C(0xe0000000)) != UINT32_C(0x60000000) ||
        raw_offset < table_offset + SECTION_SIZE || virtual_size > raw_size ||
        !contains(size, raw_offset, raw_size) ||
        entry < rva || entry - rva >= virtual_size)
        return false;

    *code = (struct pe_code){image + raw_offset, virtual_size, entry - rva};
    return true;
}

static nt_status NT_MS_ABI query_from_pe(int64_t *counter, int64_t *frequency)
{
    return nt_query_performance_counter(counter, frequency);
}

int main(void)
{
    struct pe_code code;
    if (!fixture_code(nt_pe_fixture_bytes, nt_pe_fixture_size, &code)) {
        fputs("Embedded PE does not match the fixture profile\n", stderr);
        return EXIT_FAILURE;
    }

    void *memory = mmap(NULL, code.length, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return EXIT_FAILURE;
    }

    memcpy(memory, code.bytes, code.length);
    if (mprotect(memory, code.length, PROT_READ | PROT_EXEC) != 0) {
        perror("mprotect");
        munmap(memory, code.length);
        return EXIT_FAILURE;
    }

    const void *address = (const unsigned char *)memory + code.entry_offset;
    nt_fixture_entry entry;
    _Static_assert(sizeof(entry) == sizeof(address), "x86-64 pointer size");
    memcpy(&entry, &address, sizeof(entry));
    const int result = entry(query_from_pe);
    if (munmap(memory, code.length) != 0) {
        perror("munmap");
        return EXIT_FAILURE;
    }

    if (result != 0) {
        fprintf(stderr, "PE fixture case %d failed\n", result);
        return EXIT_FAILURE;
    }

    puts("PASS PE32+ AMD64 code, MS x64 ABI, NtQueryPerformanceCounter (4 cases)");
    return EXIT_SUCCESS;
}
