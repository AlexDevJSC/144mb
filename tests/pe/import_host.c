/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "fixture_abi.h"
#include "fixture_import.h"
#include "pe_bytes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern const unsigned char nt_pe_fixture_bytes[];
extern const size_t nt_pe_fixture_size;

static bool check_invalid_imports(const struct pe_import_image *image)
{
    const unsigned char *file = nt_pe_fixture_bytes;
    const size_t pe_offset = read_u32(file + 0x3c);
    const size_t directory_offset = pe_offset + 4 + 20 + 112;
    const size_t import_offset = directory_offset + 8;
    const uint32_t import_rva = read_u32(file + import_offset);
    const size_t idata_offset = (size_t)(image->idata.bytes - file);
    const size_t descriptor = idata_offset + (import_rva - image->idata.rva);
    const size_t lookup = idata_offset + (read_u32(file + descriptor) - image->idata.rva);
    const size_t dll = idata_offset + (read_u32(file + descriptor + 12) - image->idata.rva);
    const size_t symbol = idata_offset + (read_u32(file + lookup) - image->idata.rva) + 2;
    const size_t iat = idata_offset + (image->iat_rva - image->idata.rva);
    const size_t last_section = directory_offset + 16 * 8 +
                                (read_u16(file + pe_offset + 6) - 1) * 40;
    const struct {
        const char *label;
        size_t offset;
        unsigned char byte;
    } cases[] = {
        {"import RVA outside the image", import_offset + 3, 0xff},
        {"extra import descriptor", descriptor + 20, 1},
        {"unknown DLL", dll, 'x'},
        {"missing DLL terminator", dll + sizeof("ntdll.dll") - 1, 'x'},
        {"unknown import name", symbol, 'X'},
        {"missing import name terminator", symbol + sizeof("NtQueryPerformanceCounter") - 1, 'x'},
        {"import by ordinal", lookup + 7, 0x80},
        {"lookup RVA outside the image", descriptor + 3, 0xff},
        {"IAT directory RVA outside the image", directory_offset + 12 * 8 + 3, 0xff},
        {"extra address table entry", iat + 8, 1},
        {"delay import directory", directory_offset + 13 * 8, 1},
        {"executable import section", last_section + 39, 0xe0}
    };

    struct pe_import_image rejected;
    if (pe_parse_import_fixture(file, idata_offset + image->idata.size - 1, &rejected)) {
        fputs("FAIL accepts a truncated import section\n", stderr);
        return false;
    }
    puts("PASS rejects a truncated import section");

    unsigned char *copy = malloc(nt_pe_fixture_size);
    if (copy == NULL) {
        perror("malloc");
        return false;
    }
    memcpy(copy, file, nt_pe_fixture_size);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        copy[cases[i].offset] = cases[i].byte;
        if (pe_parse_import_fixture(copy, nt_pe_fixture_size, &rejected)) {
            fprintf(stderr, "FAIL accepts %s\n", cases[i].label);
            free(copy);
            return false;
        }
        copy[cases[i].offset] = file[cases[i].offset];
        printf("PASS rejects %s\n", cases[i].label);
    }
    free(copy);
    return true;
}

static bool run_fixture(const struct pe_import_image *image)
{
    if (sysconf(_SC_PAGESIZE) != PE_IMPORT_PAGE_SIZE) {
        fputs("The import fixture needs 4096-byte host pages\n", stderr);
        return false;
    }

    unsigned char *memory = mmap(NULL, image->image_size, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return false;
    }

    memcpy(memory + image->text.rva, image->text.bytes, image->text.size);
    if (image->rdata.size != 0)
        memcpy(memory + image->rdata.rva, image->rdata.bytes, image->rdata.size);
    memcpy(memory + image->idata.rva, image->idata.bytes, image->idata.size);
    const nt_fixture_query query = query_from_pe;
    _Static_assert(sizeof(query) == 8, "PE32+ IAT slot size");
    memcpy(memory + image->iat_rva, &query, sizeof(query));

    if (mprotect(memory, image->image_size, PROT_NONE) != 0 ||
        mprotect(memory + image->text.rva, PE_IMPORT_PAGE_SIZE, PROT_READ | PROT_EXEC) != 0 ||
        (image->rdata.size != 0 &&
         mprotect(memory + image->rdata.rva, PE_IMPORT_PAGE_SIZE, PROT_READ) != 0) ||
        mprotect(memory + image->idata.rva, PE_IMPORT_PAGE_SIZE, PROT_READ) != 0) {
        perror("mprotect");
        munmap(memory, image->image_size);
        return false;
    }

    const void *address = memory + image->entry_rva;
    int (NT_MS_ABI *entry)(void);
    _Static_assert(sizeof(entry) == sizeof(address), "x86-64 pointer size");
    memcpy(&entry, &address, sizeof(entry));
    const int status = entry();
    if (munmap(memory, image->image_size) != 0) {
        perror("munmap");
        return false;
    }

    if (status != 0) {
        fprintf(stderr, "PE import fixture case %d failed\n", status);
        return false;
    }
    puts("PASS PE32+ imports ntdll.dll!NtQueryPerformanceCounter through the IAT (3 cases)");
    return true;
}

int main(void)
{
    struct pe_import_image image;
    if (!pe_parse_import_fixture(nt_pe_fixture_bytes, nt_pe_fixture_size, &image)) {
        fputs("Embedded PE does not match the import fixture profile\n", stderr);
        return EXIT_FAILURE;
    }

    /* Only the image embedded by the build reaches the execution path. */
    return check_invalid_imports(&image) && run_fixture(&image) ? EXIT_SUCCESS : EXIT_FAILURE;
}
