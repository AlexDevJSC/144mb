/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_FIXTURE_IMPORT_H
#define NTNATIVE_FIXTURE_IMPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PE_IMPORT_PAGE_SIZE = 4096, PE_FIXTURE_IMPORT_LIMIT = 32 };

struct pe_fixture_symbol {
    const char *module;
    const char *name;
};

struct pe_import_section {
    const unsigned char *bytes;
    uint32_t rva;
    uint32_t size;
};

struct pe_import_image {
    struct pe_import_section text;
    struct pe_import_section rdata;
    struct pe_import_section idata;
    uint32_t image_size;
    uint32_t entry_rva;
    uint32_t iat_rva;
    uint32_t slots[PE_FIXTURE_IMPORT_LIMIT];
};

/* Validate the generated fixture profile. On success section views borrow
 * the input bytes. On failure the result is untouched.
 */
bool pe_parse_import_fixture(const unsigned char *file, size_t length,
                             const char *module, const char *symbol,
                             struct pe_import_image *result);
bool pe_parse_import_fixture_many(const unsigned char *file, size_t length,
                                  const struct pe_fixture_symbol *symbols, size_t count,
                                  struct pe_import_image *result);

#endif
