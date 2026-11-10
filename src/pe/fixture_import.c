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
    THUNK_SIZE = 8,
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

static size_t expected_symbol(const struct pe_fixture_symbol *symbols, size_t count,
                              const char *module, const char *name)
{
    for (size_t i = 0; i < count; ++i) {
        if (nt_module_name_equal(module, symbols[i].module) && strcmp(name, symbols[i].name) == 0)
            return i;
    }
    return count;
}

static const char *section_string(const struct pe_import_section *section, uint32_t rva)
{
    const unsigned char *first = section_bytes(section, rva, 1);
    if (first == NULL || *first == '\0' ||
        memchr(first, '\0', section->size - (rva - section->rva)) == NULL)
        return NULL;
    return (const char *)first;
}

static bool valid_imports(const unsigned char *directories, struct pe_import_image *image,
                          const struct pe_fixture_symbol *symbols, size_t count)
{
    const struct pe_import_section *idata = &image->idata;
    const unsigned char *import = directories + IMPORT_DIRECTORY * DIRECTORY_SIZE;
    const unsigned char *iat = directories + IAT_DIRECTORY * DIRECTORY_SIZE;
    const uint32_t import_rva = read_u32(import);
    const uint32_t import_size = read_u32(import + 4);
    const uint32_t iat_rva = read_u32(iat), iat_size = read_u32(iat + 4);
    const unsigned char *descriptors = section_bytes(idata, import_rva, import_size);
    const unsigned char *address_table = section_bytes(idata, iat_rva, iat_size);
    if (descriptors == NULL || address_table == NULL || iat_rva % THUNK_SIZE != 0 ||
        import_size < 2 * DESCRIPTOR_SIZE || iat_size < 2 * THUNK_SIZE ||
        iat_size % THUNK_SIZE != 0)
        return false;

    struct import_range ranges[2 + 3 * PE_FIXTURE_IMPORT_LIMIT] = {
        {import_rva, 0}, {iat_rva, iat_size}};
    size_t range_count = 2, consumed = 0, found = 0;
    const unsigned char terminator[DESCRIPTOR_SIZE] = {0};
    for (size_t offset = 0; contains(import_size, offset, DESCRIPTOR_SIZE); offset += DESCRIPTOR_SIZE) {
        const unsigned char *descriptor = descriptors + offset;
        if (memcmp(descriptor, terminator, sizeof(terminator)) == 0) {
            ranges[0].size = offset + DESCRIPTOR_SIZE;
            image->iat_rva = iat_rva;
            return found == count && consumed == iat_size && ranges_disjoint(ranges, range_count);
        }
        if (found == count || read_u32(descriptor + 4) != 0 || read_u32(descriptor + 8) != 0 ||
            read_u32(descriptor + 16) != iat_rva + consumed)
            return false;
        const uint32_t lookup_rva = read_u32(descriptor), dll_rva = read_u32(descriptor + 12);
        const char *dll = section_string(idata, dll_rva);
        if (dll == NULL || lookup_rva % THUNK_SIZE != 0 ||
            section_bytes(idata, lookup_rva, THUNK_SIZE) == NULL)
            return false;
        ranges[range_count++] = (struct import_range){dll_rva, strlen(dll) + 1};
        const size_t before = consumed;
        for (;;) {
            const uint32_t entry_rva = lookup_rva + (uint32_t)(consumed - before);
            const unsigned char *lookup = section_bytes(idata, entry_rva, THUNK_SIZE);
            if (lookup == NULL || !contains(iat_size, consumed, THUNK_SIZE))
                return false;
            const uint64_t entry = read_u64(lookup);
            if (entry != read_u64(address_table + consumed))
                return false;
            if (entry == 0) {
                if (consumed == before)
                    return false;
                consumed += THUNK_SIZE;
                break;
            }
            /* Fixtures permit only named, unbound entries. */
            if (found == count || entry > UINT32_MAX - 2 ||
                section_bytes(idata, (uint32_t)entry, 2) == NULL)
                return false;
            const char *name = section_string(idata, (uint32_t)entry + 2);
            if (name == NULL)
                return false;
            const size_t index = expected_symbol(symbols, count, dll, name);
            if (index == count || image->slots[index] != 0)
                return false;
            image->slots[index] = iat_rva + (uint32_t)consumed;
            ranges[range_count++] = (struct import_range){(uint32_t)entry, strlen(name) + 3};
            consumed += THUNK_SIZE;
            ++found;
        }
        ranges[range_count++] = (struct import_range){lookup_rva, consumed - before};
    }
    return false;
}

bool pe_parse_import_fixture_many(const unsigned char *file, size_t length,
                                  const struct pe_fixture_symbol *symbols, size_t count,
                                  struct pe_import_image *result)
{
    if (count == 0 || count > PE_FIXTURE_IMPORT_LIMIT ||
        !contains(length, 0, 64) || memcmp(file, "MZ", 2) != 0)
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
    if (!read_section(file, length, sections, ".text\0\0\0", headers_size, image_size, &text) ||
        text.permissions != UINT32_C(0x60000000) ||
        section_bytes(&text.view, entry_rva, 1) == NULL ||
        !read_section(file, length, import_section, ".idata\0\0", headers_size, image_size, &idata) ||
        (idata.permissions & UINT32_C(0x60000000)) != UINT32_C(0x40000000))
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

    struct pe_import_image image = {.text = text.view, .rdata = rdata.view, .idata = idata.view,
                                    .image_size = image_size, .entry_rva = entry_rva};
    if (!valid_imports(directories, &image, symbols, count))
        return false;
    *result = image;
    return true;
}

bool pe_parse_import_fixture(const unsigned char *file, size_t length,
                             const char *module, const char *symbol,
                             struct pe_import_image *result)
{
    const struct pe_fixture_symbol expected = {module, symbol};
    return pe_parse_import_fixture_many(file, length, &expected, 1, result);
}
