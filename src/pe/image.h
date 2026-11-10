/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_PE_IMAGE_H
#define NTNATIVE_PE_IMAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { PE_IMAGE_MAX_BYTES = 128 * 1024 * 1024, PE_IMAGE_MAX_MEMORY = 256 * 1024 * 1024, PE_IMAGE_MAX_SECTIONS = 96 };
typedef uintptr_t (*pe_image_resolver)(const char *module, const char *name, uint16_t ordinal, void *context);
struct pe_image {
    unsigned char *base;
    size_t size;
    uint32_t entry_rva;
    uint32_t resource_rva, resource_size;
    uint32_t exception_rva, exception_size;
    uint32_t tls_callbacks_rva, tls_callback_count;
    uint16_t subsystem;
};
/* No guest instruction is executed. Caller must sandbox before calling entry. */
bool pe_image_load(const unsigned char *file, size_t length, pe_image_resolver resolve,
                   void *context, struct pe_image *output, char *error, size_t error_size);
void pe_image_unload(struct pe_image *image);
#endif
