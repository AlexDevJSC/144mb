/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "image.h"
#include "ntnative/guest_thread.h"
#include "pe_bytes.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    PE_OPTIONAL_MIN = 112, PE_SECTION_SIZE = 40, PE_EXPORT_DIR = 0, PE_IMPORT_DIR = 1,
    PE_SECURITY_DIR = 4, PE_EXCEPTION_DIR = 3, PE_RESOURCE_DIR = 2, PE_BASERELOC_DIR = 5,
    PE_TLS_DIR = 9, PE_DELAY_DIR = 13, PE_IAT_DIR = 12,
    MAX_IMPORTS = 65536, MAX_NAME = 512
};
#define SCN_EXECUTE UINT32_C(0x20000000)
#define SCN_READ    UINT32_C(0x40000000)
#define SCN_WRITE   UINT32_C(0x80000000)
struct section {
    uint32_t rva, virtual_size, raw_size, raw_offset, extent, characteristics;
};
struct directory { uint32_t rva, size; };

static bool fail(char *error, size_t capacity, const char *message)
{
    if (error != NULL && capacity != 0)
        snprintf(error, capacity, "%s", message);
    return false;
}

static bool span(size_t total, uint64_t offset, uint64_t size)
{
    return offset <= total && size <= total - offset;
}

static bool cstring(const unsigned char *base, size_t size,
                    uint32_t rva, size_t maximum, const char **out)
{
    if (!span(size, rva, 1))
        return false;
    size_t cap = size - rva;
    if (cap > maximum)
        cap = maximum;
    const unsigned char *end = memchr(base + rva, 0, cap);
    if (end == NULL || end == base + rva)
        return false;
    *out = (const char *)(base + rva);
    return true;
}

static bool dir_read(const unsigned char *opt, size_t opt_size,
                     uint32_t count, unsigned index, struct directory *out)
{
    *out = (struct directory){0};
    if (index >= count)
        return true;
    const size_t offset = PE_OPTIONAL_MIN + (size_t)index * 8;
    if (!span(opt_size, offset, 8))
        return false;
    out->rva = read_u32(opt + offset);
    out->size = read_u32(opt + offset + 4);
    return (out->rva == 0) == (out->size == 0);
}

static bool relocate(unsigned char *base, size_t size, int64_t delta,
                     struct directory reloc)
{
    if (reloc.size == 0)
        return delta == 0;
    if (!span(size, reloc.rva, reloc.size))
        return false;
    size_t offset = reloc.rva, end = offset + reloc.size;
    while (offset < end) {
        if (!span(end, offset, 8))
            return false;
        const uint32_t page = read_u32(base + offset);
        const uint32_t block_size = read_u32(base + offset + 4);
        if (block_size < 8 || (block_size & 1) || !span(end, offset, block_size))
            return false;
        for (size_t item = offset + 8; item < offset + block_size; item += 2) {
            const uint16_t entry = read_u16(base + item);
            const uint16_t type = (uint16_t)(entry >> 12);
            const uint64_t target = (uint64_t)page + (entry & 0xfffu);
            if (type == 0)
                continue;
            if (type != 10 || !span(size, target, 8))
                return false;
            /* unsigned arithmetic avoids signed overflow for the relocation delta */
            const uint64_t old = read_u64(base + (size_t)target);
            const uint64_t next = old + (uint64_t)delta;
            memcpy(base + (size_t)target, &next, 8);
        }
        offset += block_size;
    }
    return offset == end;
}

static bool imports(unsigned char *base, size_t size, struct directory directory,
                    pe_image_resolver resolver, void *context)
{
    if (directory.size == 0)
        return true;
    if (resolver == NULL || !span(size, directory.rva, directory.size))
        return false;
    size_t total = 0;
    const size_t start = directory.rva;
    const size_t end = start + directory.size;
    for (size_t pos = start; span(end, pos, 20); pos += 20) {
        const uint32_t lookup_rva = read_u32(base + pos);
        const uint32_t timestamp = read_u32(base + pos + 4);
        const uint32_t chain = read_u32(base + pos + 8);
        const uint32_t module_rva = read_u32(base + pos + 12);
        const uint32_t iat_rva = read_u32(base + pos + 16);
        if (lookup_rva == 0 && timestamp == 0 && chain == 0 &&
            module_rva == 0 && iat_rva == 0)
            return true;
        if (iat_rva == 0 || (iat_rva & 7))
            return false;
        const char *module;
        if (!cstring(base, size, module_rva, MAX_NAME, &module))
            return false;
        const uint32_t src = lookup_rva ? lookup_rva : iat_rva;
        if (src & 7)
            return false;
        bool terminated = false;
        for (size_t index = 0; index < MAX_IMPORTS - total; ++index) {
            const uint64_t from = (uint64_t)src + index * 8;
            const uint64_t to = (uint64_t)iat_rva + index * 8;
            if (!span(size, from, 8) || !span(size, to, 8))
                return false;
            const uint64_t value = read_u64(base + from);
            if (value == 0) {
                terminated = true;
                break;
            }
            const char *name = NULL;
            uint16_t ordinal = 0;
            if (value & (UINT64_C(1) << 63)) {
                if (value & ~(UINT64_C(1) << 63 | UINT16_MAX))
                    return false;
                ordinal = (uint16_t)value;
                if (ordinal == 0)
                    return false;
            } else {
                if (value > UINT32_MAX || !span(size, value, 3) ||
                    !cstring(base, size, (uint32_t)value + 2, MAX_NAME, &name))
                    return false;
            }
            const uintptr_t address = resolver(module, name, ordinal, context);
            if (address == 0)
                return false;
            const uint64_t target = (uint64_t)address;
            memcpy(base + to, &target, 8);
            ++total;
        }
        if (!terminated)
            return false;
    }
    return false;
}

static bool section_has(const struct section *sections, size_t count,
                        uint32_t rva, size_t length, uint32_t required)
{
    for (size_t i = 0; i < count; ++i) {
        const struct section *section = &sections[i];
        if (rva >= section->rva && span(section->extent, rva - section->rva, length) &&
            (section->characteristics & required) == required)
            return true;
    }
    return false;
}

/* The executable is the only module assigned a static TLS index. */
static bool prepare_tls(unsigned char *base, size_t size, struct directory tls,
                        const struct section *sections, size_t section_count,
                        struct pe_image_tls *out)
{
    *out = (struct pe_image_tls){0};
    if (tls.size == 0)
        return true;
    if (tls.size < 40 || !span(size, tls.rva, tls.size))
        return false;
    const unsigned char *data = base + tls.rva;
    const uint64_t start = read_u64(data);
    const uint64_t end = read_u64(data + 8);
    const uint64_t index_va = read_u64(data + 16);
    const uint64_t callbacks_va = read_u64(data + 24);
    const uint32_t fill = read_u32(data + 32);
    const uint32_t alignment_code = (read_u32(data + 36) >> 20) & 15;
    if (start > end || end - start > NT_GUEST_TLS_MAX ||
        fill > NT_GUEST_TLS_MAX - (end - start) ||
        (alignment_code != 0 && (1u << (alignment_code - 1)) > NT_GUEST_TLS_ALIGNMENT) ||
        index_va < (uintptr_t)base ||
        index_va - (uintptr_t)base > UINT32_MAX ||
        !section_has(sections, section_count,
                     (uint32_t)(index_va - (uintptr_t)base), 4, SCN_WRITE))
        return false;
    if (end != start) {
        if (start < (uintptr_t)base || !span(size, start - (uintptr_t)base, end - start) ||
            !section_has(sections, section_count, (uint32_t)(start - (uintptr_t)base),
                         (uint32_t)(end - start), SCN_READ))
            return false;
        out->data_rva = (uint32_t)(start - (uintptr_t)base);
        out->bytes = (uint32_t)(end - start);
    }
    out->zero_fill = fill;
    const uint32_t index_rva = (uint32_t)(index_va - (uintptr_t)base);
    const uint32_t zero = 0;
    memcpy(base + index_rva, &zero, sizeof(zero));
    if (callbacks_va == 0)
        return true;
    if (callbacks_va < (uintptr_t)base || callbacks_va - (uintptr_t)base > UINT32_MAX)
        return false;
    const uint32_t array_rva = (uint32_t)(callbacks_va - (uintptr_t)base);
    for (uint32_t i = 0; i <= 64; ++i) {
        const uint64_t offset = (uint64_t)array_rva + (uint64_t)i * 8;
        if (!span(size, offset, 8) ||
            !section_has(sections, section_count, (uint32_t)offset, 8, SCN_READ))
            return false;
        const uint64_t address = read_u64(base + offset);
        if (address == 0) {
            out->callbacks_rva = array_rva;
            out->callback_count = i;
            return true;
        }
        if (i == 64 || address < (uintptr_t)base ||
            address - (uintptr_t)base > UINT32_MAX ||
            !section_has(sections, section_count,
                         (uint32_t)(address - (uintptr_t)base), 1, SCN_EXECUTE))
            return false;
    }
    return false;
}

void pe_image_unload(struct pe_image *image)
{
    if (image == NULL)
        return;
    if (image->base != NULL && image->size != 0)
        munmap(image->base, image->size);
    memset(image, 0, sizeof(*image));
}

static bool load_image(const unsigned char *file, size_t length, pe_image_resolver resolver,
                       void *context, struct pe_image *output, char *error,
                       size_t error_size, bool as_library)
{
    if (file == NULL || output == NULL || length < 64 || length > PE_IMAGE_MAX_BYTES ||
        memcmp(file, "MZ", 2) != 0)
        return fail(error, error_size, "Not a bounded DOS/PE executable");
    const uint32_t pe = read_u32(file + 0x3c);
    if (pe < 64 || !span(length, pe, 24) || memcmp(file + pe, "PE\0\0", 4) != 0)
        return fail(error, error_size, "Missing PE header");
    const unsigned char *coff = file + pe + 4;
    const uint16_t machine = read_u16(coff);
    const uint16_t count = read_u16(coff + 2);
    const uint16_t optional_length = read_u16(coff + 16);
    const uint16_t flags = read_u16(coff + 18);
    const size_t opt_offset = (size_t)pe + 24;
    if (machine != 0x8664 || count == 0 || count > PE_IMAGE_MAX_SECTIONS ||
        !(flags & 0x0002) || ((flags & 0x2000) != 0) != as_library ||
        optional_length < PE_OPTIONAL_MIN || !span(length, opt_offset, optional_length))
        return fail(error, error_size, "Not a PE32+ AMD64 image of the requested type");
    const unsigned char *opt = file + opt_offset;
    if (read_u16(opt) != 0x20b)
        return fail(error, error_size, "Only PE32+ is supported");
    const uint32_t image_size = read_u32(opt + 56);
    const uint32_t header_size = read_u32(opt + 60);
    const uint32_t entry_rva = read_u32(opt + 16);
    const uint64_t preferred = read_u64(opt + 24);
    const uint32_t section_align = read_u32(opt + 32);
    const uint32_t file_align = read_u32(opt + 36);
    const uint32_t dir_count = read_u32(opt + 108);
    const uint16_t subsystem = read_u16(opt + 68);
    const size_t section_offset = opt_offset + optional_length;
    if (image_size < 4096 || image_size > PE_IMAGE_MAX_MEMORY || image_size % 4096 ||
        section_align < 4096 || (section_align & (section_align - 1)) ||
        file_align < 512 || file_align > 65536 || (file_align & (file_align - 1)) ||
        header_size == 0 || header_size > image_size || header_size > length ||
        header_size % file_align || !span(header_size, section_offset, (size_t)count * PE_SECTION_SIZE) ||
        (entry_rva >= image_size && (!as_library || entry_rva != 0)) || preferred == 0 ||
        dir_count > ((uint32_t)optional_length - PE_OPTIONAL_MIN) / 8u ||
        (subsystem != 2 && subsystem != 3))
        return fail(error, error_size, "Invalid image dimensions or optional header");
    struct directory directory[16];
    for (unsigned i = 0; i < 16; ++i) {
        if (!dir_read(opt, optional_length, dir_count, i, &directory[i]))
            return fail(error, error_size, "Malformed data directory");
    }
    /* Delay-load descriptors belong to the application's CRT helper
     * (__delayLoadHelper2), not to the operating-system import resolver.
     * Keep their mapped contents and mutable delay IAT intact. Calls made
     * later through LoadLibrary/GetProcAddress are resolved at first use.
     * This does not imply support for arbitrary external guest DLLs. */
    /* A DLL needs its own loader-assigned TLS index. Reusing the EXE's
     * index zero would corrupt TLS and is deliberately prohibited. */
    if (as_library && directory[PE_TLS_DIR].size)
        return fail(error, error_size, "DLL static TLS needs a module loader");

    struct section sections[PE_IMAGE_MAX_SECTIONS] = {{0}};
    uint64_t previous_end = (uint64_t)(header_size + 4095u) & ~UINT64_C(4095);
    uint64_t previous_raw_end = header_size;
    const struct section *code = NULL;
    for (unsigned i = 0; i < count; ++i) {
        const unsigned char *sh = file + section_offset + (size_t)i * PE_SECTION_SIZE;
        struct section *s = &sections[i];
        s->virtual_size = read_u32(sh + 8);
        s->rva = read_u32(sh + 12);
        s->raw_size = read_u32(sh + 16);
        s->raw_offset = read_u32(sh + 20);
        s->characteristics = read_u32(sh + 36);
        s->extent = s->virtual_size > s->raw_size ? s->virtual_size : s->raw_size;
        if (s->extent == 0 || s->rva % section_align ||
            s->raw_size % file_align ||
            s->rva < previous_end || !span(image_size, s->rva, s->extent) ||
            (s->raw_size && (s->raw_offset % file_align ||
              s->raw_offset < header_size || !span(length, s->raw_offset, s->raw_size))) ||
            (s->characteristics & (SCN_EXECUTE | SCN_WRITE)) == (SCN_EXECUTE | SCN_WRITE))
            return fail(error, error_size, "Overlapping, unaligned or W+X section");
        if (s->raw_size && s->raw_offset < previous_raw_end)
            return fail(error, error_size, "Overlapping raw sections");
        if (s->raw_size)
            previous_raw_end = (uint64_t)s->raw_offset + s->raw_size;
        previous_end = ((uint64_t)s->rva + s->extent + 4095) & ~UINT64_C(4095);
        if ((s->characteristics & SCN_EXECUTE) &&
            (entry_rva ? (entry_rva >= s->rva && entry_rva - s->rva < s->extent)
                       : as_library && code == NULL))
            code = s;
    }
    if (!code && (!as_library || entry_rva != 0))
        return fail(error, error_size, "Entry point is not executable");
    for (unsigned i = 0; i < 16; ++i)
        if (directory[i].size && !span(i == PE_SECURITY_DIR ? length : image_size,
                                       directory[i].rva, directory[i].size))
            return fail(error, error_size, "Directory outside image");
    if (directory[PE_EXCEPTION_DIR].size &&
        ((directory[PE_EXCEPTION_DIR].rva & 3u) != 0 ||
         !section_has(sections, count, directory[PE_EXCEPTION_DIR].rva,
                      directory[PE_EXCEPTION_DIR].size, SCN_READ)))
        return fail(error, error_size, "Exception table is not aligned or readable");
    if (directory[PE_EXPORT_DIR].size &&
        (directory[PE_EXPORT_DIR].size < 40 ||
         !section_has(sections, count, directory[PE_EXPORT_DIR].rva,
                      directory[PE_EXPORT_DIR].size, SCN_READ)))
        return fail(error, error_size, "Export directory is not readable");

    /* Mapping preferred address only if the kernel says that region is unused. */
    void *mapped = MAP_FAILED;
#ifdef MAP_FIXED_NOREPLACE
    if (preferred <= UINTPTR_MAX && preferred % 4096 == 0)
        mapped = mmap((void *)(uintptr_t)preferred, image_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
#endif
    if (mapped == MAP_FAILED)
        mapped = mmap(NULL, image_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED)
        return fail(error, error_size, "Linux mmap failed");
    unsigned char *base = mapped;
    bool success = false;
    struct pe_image_tls tls;
    do {
        memcpy(base, file, header_size);
        for (unsigned i = 0; i < count; ++i)
            if (sections[i].raw_size)
                memcpy(base + sections[i].rva, file + sections[i].raw_offset,
                       sections[i].raw_size);
        const uint64_t offset = (uintptr_t)base - preferred;
        int64_t delta;
        memcpy(&delta, &offset, sizeof(delta));
        if (!relocate(base, image_size, delta, directory[PE_BASERELOC_DIR])) {
            fail(error, error_size, "Missing or invalid DIR64 base relocations");
            break;
        }
        if (!prepare_tls(base, image_size, directory[PE_TLS_DIR],
                         sections, count, &tls)) {
            fail(error, error_size, "Invalid or unsupported TLS directory");
            break;
        }
        if (!imports(base, image_size, directory[PE_IMPORT_DIR], resolver, context)) {
            fail(error, error_size, "Unresolved or malformed PE import table");
            break;
        }
        /* Remove access to gaps before adding page permissions. */
        if (mprotect(base, image_size, PROT_NONE) != 0) {
            fail(error, error_size, "Cannot protect image pages");
            break;
        }
        const size_t header_pages = (header_size + 4095u) & ~(size_t)4095;
        if (mprotect(base, header_pages, PROT_READ) != 0) {
            fail(error, error_size, "Cannot protect PE headers");
            break;
        }
        bool permissions_ok = true;
        for (unsigned i = 0; i < count; ++i) {
            const struct section *s = &sections[i];
            const size_t pages = ((size_t)s->extent + 4095u) & ~(size_t)4095;
            int protection = 0;
            if (s->characteristics & SCN_READ) protection |= PROT_READ;
            if (s->characteristics & SCN_WRITE) protection |= PROT_READ | PROT_WRITE;
            if (s->characteristics & SCN_EXECUTE) protection |= PROT_READ | PROT_EXEC;
            if (mprotect(base + s->rva, pages, protection) != 0) {
                permissions_ok = false;
                break;
            }
        }
        if (!permissions_ok) {
            fail(error, error_size, "Cannot protect section pages");
            break;
        }
        *output = (struct pe_image){
            .base = base, .size = image_size, .entry_rva = entry_rva,
            .code_rva = code ? code->rva : 0, .code_size = code ? code->extent : 0,
            .export_rva = directory[PE_EXPORT_DIR].rva,
            .export_size = directory[PE_EXPORT_DIR].size,
            .resource_rva = directory[PE_RESOURCE_DIR].rva,
            .resource_size = directory[PE_RESOURCE_DIR].size,
            .exception_rva = directory[PE_EXCEPTION_DIR].rva,
            .exception_size = directory[PE_EXCEPTION_DIR].size,
            .tls = tls,
            .subsystem = subsystem
        };
        output->readable[output->readable_count++] = (struct pe_image_range){0, header_size};
        for (size_t i = 0; i < count; ++i) {
            const struct section *s = &sections[i];
            if (s->characteristics & (SCN_READ | SCN_WRITE | SCN_EXECUTE))
                output->readable[output->readable_count++] = (struct pe_image_range){s->rva, s->extent};
        }
        success = true;
    } while (0);
    if (!success)
        munmap(mapped, image_size);
    return success;
}

bool pe_image_load(const unsigned char *file, size_t length, pe_image_resolver resolve,
                   void *context, struct pe_image *output, char *error, size_t error_size)
{
    return load_image(file, length, resolve, context, output, error, error_size, false);
}

bool pe_image_load_library(const unsigned char *file, size_t length, pe_image_resolver resolve,
                           void *context, struct pe_image *output, char *error, size_t error_size)
{
    return load_image(file, length, resolve, context, output, error, error_size, true);
}

/* Every byte must lie in readable image pages before scanning a NUL
 * terminator. A valid RVA alone does not make a PE section readable. */
static bool export_string(const struct pe_image *image, uint32_t rva, size_t maximum,
                          const char **out)
{
    if (maximum > MAX_NAME)
        maximum = MAX_NAME;
    for (size_t i = 0; i < maximum; ++i) {
        if (!span(image->size, (uint64_t)rva + i, 1) ||
            !pe_image_readable(rva + (uint32_t)i, 1, image))
            return false;
        if (image->base[rva + i] == 0) {
            if (i == 0)
                return false;
            *out = (const char *)(image->base + rva);
            return true;
        }
    }
    return false;
}

bool pe_image_lookup_export(const struct pe_image *image, const char *name, uint16_t ordinal,
                            struct pe_image_export *output)
{
    if (!output)
        return false;
    *output = (struct pe_image_export){0};
    if (!image || !image->base || !image->export_size || (!name && !ordinal) ||
        (name && ordinal) || (name && !*name) ||
        !pe_image_readable(image->export_rva, 40, image))
        return false;

    const unsigned char *dir = image->base + image->export_rva;
    const uint32_t base_ordinal = read_u32(dir + 16);
    const uint32_t functions = read_u32(dir + 20);
    const uint32_t names = read_u32(dir + 24);
    const uint32_t functions_rva = read_u32(dir + 28);
    const uint32_t names_rva = read_u32(dir + 32);
    const uint32_t ordinals_rva = read_u32(dir + 36);
    if (!functions || functions > MAX_IMPORTS || names > functions ||
        !pe_image_readable(functions_rva, (size_t)functions * 4, image) ||
        (names && (!pe_image_readable(names_rva, (size_t)names * 4, image) ||
                   !pe_image_readable(ordinals_rva, (size_t)names * 2, image))))
        return false;

    uint32_t index = UINT32_MAX;
    if (name) {
        for (uint32_t i = 0; i < names; ++i) {
            uint32_t target_name = read_u32(image->base + names_rva + (size_t)i * 4);
            const char *candidate = NULL;
            if (!export_string(image, target_name, MAX_NAME, &candidate))
                return false;
            if (strcmp(name, candidate) == 0) {
                index = read_u16(image->base + ordinals_rva + (size_t)i * 2);
                break;
            }
        }
    } else if ((uint32_t)ordinal >= base_ordinal &&
               (uint32_t)ordinal - base_ordinal < functions) {
        index = (uint32_t)ordinal - base_ordinal;
    }
    if (index >= functions)
        return false;
    const uint32_t target = read_u32(image->base + functions_rva + (size_t)index * 4);
    if (!target)
        return false;
    const uint64_t export_end = (uint64_t)image->export_rva + image->export_size;
    if (target >= image->export_rva && target < export_end) {
        /* A forwarder points to "DLL.Function" in the export directory.
         * It is not executable code and must be resolved by a later loader. */
        const char *forwarder = NULL;
        if (!export_string(image, target, (size_t)(export_end - target), &forwarder) ||
            !strchr(forwarder, '.'))
            return false;
        output->forwarder = forwarder;
        return true;
    }
    if (!pe_image_readable(target, 1, image))
        return false;
    output->address = (uintptr_t)(image->base + target);
    return true;
}

/* Forwarded exports are data strings inside the export directory, not
 * executable RVAs. Normalize bare module names to the builtin DLL registry.
 * A caller may delegate further mapped-DLL forwarding through resolve(). */
bool pe_image_resolve_export(const struct pe_image *image, const char *name, uint16_t ordinal,
                             pe_image_resolver resolve, void *context, uintptr_t *address)
{
    if (address == NULL)
        return false;
    *address = 0;
    struct pe_image_export result;
    if (!pe_image_lookup_export(image, name, ordinal, &result))
        return false;
    if (result.forwarder == NULL) {
        *address = result.address;
        return result.address != 0;
    }
    if (resolve == NULL)
        return false;
    const char *dot = strrchr(result.forwarder, '.');
    if (dot == NULL || dot == result.forwarder || dot[1] == 0)
        return false;
    const size_t module_length = (size_t)(dot - result.forwarder);
    const bool has_extension = module_length >= 4 &&
                               strncasecmp(dot - 4, ".dll", 4) == 0;
    if (module_length >= 128 - (has_extension ? 0u : 4u))
        return false;
    char module[128];
    memcpy(module, result.forwarder, module_length);
    if (has_extension)
        module[module_length] = 0;
    else
        memcpy(module + module_length, ".dll", sizeof(".dll"));

    const char *target = dot + 1;
    uint16_t target_ordinal = 0;
    if (*target == '#') {
        ++target;
        unsigned int decimal = 0;
        if (!*target)
            return false;
        for (; *target; ++target) {
            if (*target < '0' || *target > '9')
                return false;
            decimal = decimal * 10u + (unsigned int)(*target - '0');
            if (decimal > UINT16_MAX)
                return false;
        }
        if (decimal == 0)
            return false;
        target_ordinal = (uint16_t)decimal;
        target = NULL;
    }
    const uintptr_t found = resolve(module, target, target_ordinal, context);
    if (found == 0)
        return false;
    *address = found;
    return true;
}

bool pe_image_readable(uint32_t rva, size_t length, const void *opaque)
{
    const struct pe_image *image = opaque;
    for (size_t i = 0; i < image->readable_count; ++i) {
        const struct pe_image_range *range = &image->readable[i];
        if (rva >= range->rva && span(range->size, rva - range->rva, length))
            return true;
    }
    return false;
}
