/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_PE_IMPORTS_H
#define NTNATIVE_PE_IMPORTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct pe_import {
    const char *module;
    const char *name; /* NULL for an import by ordinal. */
    uint16_t ordinal;
};

typedef void (*pe_import_visitor)(const struct pe_import *, void *);

/* Walk direct PE32+ AMD64 imports without mapping any guest code for
 * execution. The strings borrow the input. Discard partial results if
 * the scan reports a malformed file.
 */
bool pe_scan_imports(const unsigned char *file, size_t length,
                     pe_import_visitor visit, void *context);

#endif
