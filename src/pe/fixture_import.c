/* SPDX-License-Identifier: MIT */
#include "fixture_import.h"
#include "ntnative/runtime.h"
#include "pe_bytes.h"

#include <string.h>

enum {
    COFF_SIZE = 20,
    SECTION_SIZE = 40,
    DIRECTORY_OFFSET = 112,
    DIRECTORY_SIZE = 8,
    DIRECTORY_COUNT = 16,
    IMPORT_DIRECTORY = 1,
    IAT_DIRECTORY = 12,
    DESCRIPTOR_SIZE = 20,
    THUNK_TABLE_SIZE = 16,
    FILE_ALIGNMENT = 512,
    MAX_IMAGE_SIZE = 64 * 1024
};

struct section_header {
    struct pe_import_section view;
    uint32_t raw_offset;
    uint32_t raw_size;
    uint32_t permissions;
};

struct import_range {
    uint32_t rva;
    size_t size;
};

static bool read_section(const unsigned char *file, size_t length,
                         const unsigned char *header, const char name[8],
                         uint32_t headers_size, uint32_t image_size,
                         struct section_header *section)
{
    const uint32_t size = read_u32(header + 8);
    const uint32_t rva = read_u32(header + 12);
    const uint32_t raw_size = read_u32(header + 16);
    const uint32_t raw_offset = read_u32(header + 20);
    const uint32_t permissions = read_u32(header + 36) & UINT32_C(0xe0000000);

    if (memcmp(header, name, 8) != 0 ||
        rva < PE_IMPORT_PAGE_SIZE || rva % PE_IMPORT_PAGE_SIZE != 0 ||
        !contains(image_size, rva, PE_IMPORT_PAGE_SIZE) ||
        size == 0 || size > raw_size || raw_size > PE_IMPORT_PAGE_SIZE ||
        raw_size % FILE_ALIGNMENT != 0 || raw_offset % FILE_ALIGNMENT != 0 ||
        raw_offset < headers_size || !contains(length, raw_offset, raw_size))
        return false;

    *section = (struct section_header){{file + raw_offset, rva, size},
                                       raw_offset, raw_size, permissions};
    return true;
}

static bool sections_ordered(const struct section_header *first,
                             const struct section_header *next)
{
    return first->view.rva < next->view.rva &&
           first->raw_offset + (size_t)first->raw_size <= next->raw_offset;
}

static const unsigned char *section_bytes(const struct pe_import_section *section,
                                         uint32_t rva, size_t size)
{
    if (rva < section->rva || !contains(section->size, rva - section->rva, size))
        return NULL;
    return section->bytes + (rva - section->rva);
}

static bool ranges_disjoint(const struct import_range *ranges, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < i; ++j) {
            if (ranges[i].rva < ranges[j].rva + ranges[j].size &&
                ranges[j].rva < ranges[i].rva + ranges[i].size)
                return false;
        }
    }
    return true;
}

static bool valid_import(const unsigned char *directories,
                         const struct pe_import_section *idata, const char *module,
                         const char *symbol, uint32_t *iat_rva)
{
    const size_t module_size = strlen(module) + 1;
    const size_t symbol_size = strlen(symbol) + 1;
    const unsigned char *import = directories + IMPORT_DIRECTORY * DIRECTORY_SIZE;
    const unsigned char *iat = directories + IAT_DIRECTORY * DIRECTORY_SIZE;
    const uint32_t import_rva = read_u32(import);
    const uint32_t import_size = read_u32(import + 4);
    const uint32_t slot_rva = read_u32(iat);
    const unsigned char *descriptor = section_bytes(idata, import_rva, import_size);
    const unsigned char *slots = section_bytes(idata, slot_rva, THUNK_TABLE_SIZE);

    /* One descriptor and its all-zero terminator, followed by exactly one
     * unbound lookup and address entry per table.
     */
    if (descriptor == NULL || import_size < 2 * DESCRIPTOR_SIZE ||
        slots == NULL || read_u32(iat + 4) != THUNK_TABLE_SIZE ||
        read_u32(descriptor + 4) != 0 || read_u32(descriptor + 8) != 0 ||
        read_u32(descriptor + 16) != slot_rva ||
        memcmp(descriptor + DESCRIPTOR_SIZE,
               (const unsigned char[DESCRIPTOR_SIZE]){0}, DESCRIPTOR_SIZE) != 0)
        return false;

    const uint32_t lookup_rva = read_u32(descriptor);
    const uint32_t dll_rva = read_u32(descriptor + 12);
    const unsigned char *lookup = section_bytes(idata, lookup_rva, THUNK_TABLE_SIZE);
    const unsigned char *dll = section_bytes(idata, dll_rva, module_size);
    if (lookup == NULL || lookup_rva % 8 != 0 || slot_rva % 8 != 0 ||
        dll == NULL || dll[module_size - 1] != '\0' ||
        !nt_module_name_equal((const char *)dll, module))
        return false;

    const uint64_t name_entry = read_u64(lookup);
    /* A name is a 32-bit RVA. Ordinals and pre-bound addresses cannot fit. */
    if (name_entry > UINT32_MAX || read_u64(lookup + 8) != 0 ||
        read_u64(slots) != name_entry || read_u64(slots + 8) != 0)
        return false;

    const uint32_t name_rva = (uint32_t)name_entry;
    const unsigned char *name = section_bytes(idata, name_rva, 2 + symbol_size);
    if (name == NULL || memcmp(name + 2, symbol, symbol_size) != 0)
        return false;

    const struct import_range ranges[] = {
        {import_rva, 2 * DESCRIPTOR_SIZE},
        {lookup_rva, THUNK_TABLE_SIZE},
        {slot_rva, THUNK_TABLE_SIZE},
        {dll_rva, module_size},
        {name_rva, 2 + symbol_size}
    };
    if (!ranges_disjoint(ranges, sizeof(ranges) / sizeof(ranges[0])))
        return false;

    *iat_rva = slot_rva;
    return true;
}

bool pe_parse_import_fixture(const unsigned char *file, size_t length,
                             const char *module, const char *symbol,
                             struct pe_import_image *result)
{
    if (!contains(length, 0, 64) || memcmp(file, "MZ", 2) != 0)
        return false;

    const size_t pe_offset = read_u32(file + 0x3c);
    const size_t optional_size = DIRECTORY_OFFSET + DIRECTORY_COUNT * DIRECTORY_SIZE;
    const size_t pe_header_size = 4 + COFF_SIZE + optional_size;
    if (pe_offset < 64 || !contains(length, pe_offset, pe_header_size) ||
        memcmp(file + pe_offset, "PE\0\0", 4) != 0)
        return false;

    const unsigned char *coff = file + pe_offset + 4;
    const unsigned char *optional = coff + COFF_SIZE;
    const uint16_t section_count = read_u16(coff + 2);
    const size_t headers_length = pe_header_size + (size_t)section_count * SECTION_SIZE;
    const uint32_t image_size = read_u32(optional + 56);
    const uint32_t headers_size = read_u32(optional + 60);
    if (read_u16(coff) != 0x8664 || (section_count != 2 && section_count != 3) ||
        !contains(length, pe_offset, headers_length) ||
        read_u16(coff + 16) != optional_size ||
        (read_u16(coff + 18) & 0x2002) != 0x0002 ||
        read_u16(optional) != 0x020b || read_u16(optional + 68) != 3 ||
        read_u32(optional + 32) != PE_IMPORT_PAGE_SIZE ||
        read_u32(optional + 36) != FILE_ALIGNMENT ||
        read_u32(optional + 108) != DIRECTORY_COUNT ||
        image_size > MAX_IMAGE_SIZE || image_size % PE_IMPORT_PAGE_SIZE != 0 ||
        headers_size % FILE_ALIGNMENT != 0 || headers_size > length ||
        headers_size < pe_offset + headers_length)
        return false;

    const unsigned char *directories = optional + DIRECTORY_OFFSET;
    for (size_t i = 0; i < DIRECTORY_COUNT; ++i) {
        if (i != IMPORT_DIRECTORY && i != IAT_DIRECTORY &&
            read_u64(directories + i * DIRECTORY_SIZE) != 0)
            return false;
    }

    struct section_header text;
    struct section_header rdata = {0};
    struct section_header idata;
    const unsigned char *sections = optional + optional_size;
    const unsigned char *import_section = sections + (section_count - 1) * SECTION_SIZE;
    const uint32_t entry_rva = read_u32(optional + 16);
    uint32_t iat_rva;
    if (!read_section(file, length, sections, ".text\0\0\0", headers_size, image_size, &text) ||
        text.permissions != UINT32_C(0x60000000) ||
        section_bytes(&text.view, entry_rva, 1) == NULL ||
        !read_section(file, length, import_section, ".idata\0\0", headers_size, image_size, &idata) ||
        (idata.permissions & UINT32_C(0x60000000)) != UINT32_C(0x40000000) ||
        !valid_import(directories, &idata.view, module, symbol, &iat_rva))
        return false;

    /* The PE linker can emit read-only constants and constructor sentinels
     * even for a freestanding image with no CRT.
     */
    if (section_count == 3 &&
        (!read_section(file, length, sections + SECTION_SIZE, ".rdata\0\0",
                       headers_size, image_size, &rdata) ||
         rdata.permissions != UINT32_C(0x40000000) ||
         !sections_ordered(&text, &rdata)))
        return false;
    if (!sections_ordered(section_count == 3 ? &rdata : &text, &idata))
        return false;

    *result = (struct pe_import_image){text.view, rdata.view, idata.view, image_size,
                                       entry_rva, iat_rva};
    return true;
}
