/* SPDX-License-Identifier: MIT */
#include "imports.h"
#include "pe_bytes.h"

#include <string.h>

enum {
    SECTION_SIZE = 40,
    MAX_SECTIONS = 96,
    OPTIONAL_FIXED = 112,
    DIRECTORY_SIZE = 8,
    IMPORT_DIRECTORY = 1,
    DESCRIPTOR_SIZE = 20,
    MAX_IMPORTS = 65536,
    MAX_NAME = 512
};

struct pe_file {
    const unsigned char *file;
    const unsigned char *optional;
    const unsigned char *sections;
    uint16_t section_count;
};

struct bytes {
    const unsigned char *data;
    size_t length;
};

static bool read_headers(const unsigned char *file, size_t length, struct pe_file *image)
{
    if (!contains(length, 0, 64) || memcmp(file, "MZ", 2) != 0)
        return false;

    const size_t pe = read_u32(file + 0x3c);
    if (pe < 64 || !contains(length, pe, 24) || memcmp(file + pe, "PE\0\0", 4) != 0)
        return false;

    const unsigned char *coff = file + pe + 4;
    const size_t optional_offset = pe + 24;
    const size_t optional_size = read_u16(coff + 16);
    const uint16_t count = read_u16(coff + 2);
    if (read_u16(coff) != 0x8664 || count == 0 || count > MAX_SECTIONS ||
        optional_size < OPTIONAL_FIXED || !contains(length, optional_offset, optional_size))
        return false;

    const unsigned char *optional = file + optional_offset;
    const uint32_t headers_size = read_u32(optional + 60);
    const uint32_t image_size = read_u32(optional + 56);
    const uint32_t directories = read_u32(optional + 108);
    const size_t section_offset = optional_offset + optional_size;
    if (read_u16(optional) != 0x020b || image_size < headers_size || headers_size > length ||
        directories > (optional_size - OPTIONAL_FIXED) / DIRECTORY_SIZE ||
        !contains(headers_size, section_offset, (size_t)count * SECTION_SIZE))
        return false;

    const unsigned char *sections = file + section_offset;
    size_t virtual_end = headers_size;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *section = sections + i * SECTION_SIZE;
        const uint32_t virtual_size = read_u32(section + 8);
        const uint32_t rva = read_u32(section + 12);
        const uint32_t raw_size = read_u32(section + 16);
        const uint32_t raw_offset = read_u32(section + 20);
        const uint32_t extent = virtual_size > raw_size ? virtual_size : raw_size;
        if (rva < virtual_end || !contains(image_size, rva, extent) ||
            (raw_size != 0 &&
             (raw_offset < headers_size || !contains(length, raw_offset, raw_size))))
            return false;
        virtual_end = (size_t)rva + extent;
    }

    *image = (struct pe_file){file, optional, sections, count};
    return true;
}

static struct bytes rva_bytes(const struct pe_file *image, uint32_t rva)
{
    for (size_t i = 0; i < image->section_count; ++i) {
        const unsigned char *section = image->sections + i * SECTION_SIZE;
        const uint32_t first = read_u32(section + 12);
        if (rva < first)
            break;
        const uint32_t virtual_size = read_u32(section + 8);
        const uint32_t raw_size = read_u32(section + 16);
        const uint32_t backed = virtual_size != 0 && virtual_size < raw_size ?
                                virtual_size : raw_size;
        const uint32_t offset = rva - first;
        if (offset < backed) {
            const size_t raw_offset = read_u32(section + 20);
            return (struct bytes){image->file + raw_offset + offset, backed - offset};
        }
    }
    return (struct bytes){0};
}

static const char *read_name(struct bytes span, size_t prefix)
{
    if (span.length <= prefix)
        return NULL;
    const unsigned char *name = span.data + prefix;
    const size_t available = span.length - prefix;
    const size_t limit = available < MAX_NAME ? available : MAX_NAME;
    const unsigned char *end = memchr(name, '\0', limit);
    if (end == NULL || end == name)
        return NULL;
    for (const unsigned char *byte = name; byte < end; ++byte) {
        if (*byte < 0x20 || *byte > 0x7e)
            return NULL;
    }
    return (const char *)name;
}

static bool scan_thunks(const struct pe_file *image, const unsigned char *descriptor,
                        pe_import_visitor visit, void *context, size_t *total)
{
    const uint32_t iat_rva = read_u32(descriptor + 16);
    uint32_t lookup_rva = read_u32(descriptor);
    if (lookup_rva == 0)
        lookup_rva = iat_rva;
    const struct bytes lookup = rva_bytes(image, lookup_rva);
    const struct bytes iat = rva_bytes(image, iat_rva);
    const char *module = read_name(rva_bytes(image, read_u32(descriptor + 12)), 0);
    if (module == NULL || lookup_rva % 8 != 0 || iat_rva % 8 != 0)
        return false;

    const size_t common_length = lookup.length < iat.length ? lookup.length : iat.length;
    const uint64_t ordinal_flag = UINT64_C(1) << 63;
    for (size_t offset = 0; contains(common_length, offset, 8); offset += 8) {
        const uint64_t entry = read_u64(lookup.data + offset);
        if (entry == 0)
            return offset != 0 && read_u64(iat.data + offset) == 0;
        if (*total == MAX_IMPORTS)
            return false;

        struct pe_import import = {.module = module};
        if ((entry & ordinal_flag) != 0) {
            if ((entry & ~(ordinal_flag | UINT16_MAX)) != 0)
                return false;
            import.ordinal = (uint16_t)entry;
        } else {
            if (entry > INT32_MAX)
                return false;
            import.name = read_name(rva_bytes(image, (uint32_t)entry), 2);
            if (import.name == NULL)
                return false;
        }
        visit(&import, context);
        ++*total;
    }
    return false;
}

bool pe_scan_imports(const unsigned char *file, size_t length,
                     pe_import_visitor visit, void *context)
{
    struct pe_file image;
    if (!read_headers(file, length, &image))
        return false;
    if (read_u32(image.optional + 108) <= IMPORT_DIRECTORY)
        return true;

    const unsigned char *directory = image.optional + OPTIONAL_FIXED +
                                     IMPORT_DIRECTORY * DIRECTORY_SIZE;
    const uint32_t rva = read_u32(directory);
    const uint32_t size = read_u32(directory + 4);
    if (rva == 0 || size == 0)
        return rva == 0 && size == 0;

    const struct bytes descriptors = rva_bytes(&image, rva);
    if (size > descriptors.length)
        return false;

    const unsigned char terminator[DESCRIPTOR_SIZE] = {0};
    size_t total = 0;
    for (size_t offset = 0; contains(size, offset, DESCRIPTOR_SIZE); offset += DESCRIPTOR_SIZE) {
        const unsigned char *descriptor = descriptors.data + offset;
        if (memcmp(descriptor, terminator, sizeof(terminator)) == 0)
            return true;
        if (!scan_thunks(&image, descriptor, visit, context, &total))
            return false;
    }
    return false;
}
