/* SPDX-License-Identifier: MIT */
#include "imports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    PE_OFFSET = 0x80,
    OPTIONAL = PE_OFFSET + 24,
    SECTIONS = OPTIONAL + 240,
    RAW = 0x200,
    SECTION_RVA = 0x1000,
    FILE_SIZE = 0x400
};

static void check(bool condition, const char *label)
{
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", label);
        exit(EXIT_FAILURE);
    }
}

static void write_u32(unsigned char *field, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        field[i] = (unsigned char)(value >> (8 * i));
}

static void build_image(unsigned char file[FILE_SIZE])
{
    memcpy(file, "MZ", 2);
    write_u32(file + 0x3c, PE_OFFSET);
    memcpy(file + PE_OFFSET, "PE\0\0", 4);
    write_u32(file + PE_OFFSET + 4, UINT32_C(0x00018664));
    file[PE_OFFSET + 20] = 240;
    file[OPTIONAL] = 0x0b;
    file[OPTIONAL + 1] = 2;
    write_u32(file + OPTIONAL + 56, 0x2000);
    write_u32(file + OPTIONAL + 60, RAW);
    write_u32(file + OPTIONAL + 108, 16);
    write_u32(file + OPTIONAL + 120, SECTION_RVA);
    write_u32(file + OPTIONAL + 124, 60);
    memcpy(file + SECTIONS, ".rdata", 6);
    write_u32(file + SECTIONS + 8, 0x200);
    write_u32(file + SECTIONS + 12, SECTION_RVA);
    write_u32(file + SECTIONS + 16, 0x200);
    write_u32(file + SECTIONS + 20, RAW);

    /* Two descriptors, then the zero terminator. Each descriptor has
     * a separate lookup table and address table in the same section.
     */
    unsigned char *data = file + RAW;
    write_u32(data, SECTION_RVA + 0x40);
    write_u32(data + 12, SECTION_RVA + 0x80);
    write_u32(data + 16, SECTION_RVA + 0x50);
    write_u32(data + 20, SECTION_RVA + 0x60);
    write_u32(data + 32, SECTION_RVA + 0xa0);
    write_u32(data + 36, SECTION_RVA + 0x70);
    write_u32(data + 0x40, SECTION_RVA + 0xc0);
    write_u32(data + 0x50, SECTION_RVA + 0xc0);
    data[0x60] = data[0x70] = 17;
    data[0x67] = data[0x77] = 0x80;
    memcpy(data + 0x80, "KERNEL32.dll", sizeof("KERNEL32.dll"));
    memcpy(data + 0xa0, "COMCTL32.dll", sizeof("COMCTL32.dll"));
    memcpy(data + 0xc2, "QueryPerformanceCounter", sizeof("QueryPerformanceCounter"));
}

struct imports_seen {
    size_t total;
    bool named;
    bool ordinal;
};

static void collect(const struct pe_import *import, void *context)
{
    struct imports_seen *seen = context;
    ++seen->total;
    if (import->name == NULL) {
        seen->ordinal = strcmp(import->module, "COMCTL32.dll") == 0 && import->ordinal == 17;
    } else {
        seen->named = strcmp(import->module, "KERNEL32.dll") == 0 &&
                      strcmp(import->name, "QueryPerformanceCounter") == 0;
    }
}

int main(void)
{
    unsigned char file[FILE_SIZE] = {0};
    build_image(file);
    struct imports_seen seen = {0};
    check(pe_scan_imports(file, sizeof(file), collect, &seen) &&
              seen.total == 2 && seen.named && seen.ordinal,
          "two DLLs in .rdata with a named import and ordinal 17");

    for (size_t length = 0; length < sizeof(file); ++length)
        check(!pe_scan_imports(file, length, collect, &seen), "truncated file");

    const struct {
        size_t offset;
        unsigned char value;
        const char *label;
    } bad[] = {
        {OPTIONAL + 123, 0xff, "import directory RVA out of range"},
        {RAW + 40, 1, "missing zero import descriptor"},
        {RAW + 16, 0x51, "unaligned IAT"},
        {RAW + 0x42, 1, "symbol RVA out of range"},
        {RAW + 0x62, 1, "ordinal with reserved bits"},
        {RAW + 0x58, 1, "missing IAT terminator"},
        {RAW + 0xc2, 0x1b, "invalid name byte"}
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        const unsigned char before = file[bad[i].offset];
        file[bad[i].offset] = bad[i].value;
        seen = (struct imports_seen){0};
        check(!pe_scan_imports(file, sizeof(file), collect, &seen), bad[i].label);
        file[bad[i].offset] = before;
    }
    puts("PASS AMD64 direct imports by name and ordinal and malformed import checks");
    return EXIT_SUCCESS;
}
