/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "fixture_import.h"
#include "ntnative/runtime.h"
#include "pe_bytes.h"
#ifdef TEST_DESKTOP
#include "desktop.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern const unsigned char nt_pe_fixture_bytes[];
extern const size_t nt_pe_fixture_size;

#ifdef TEST_DESKTOP
#define NAMED(module, name) {module ".dll", name, 0}
static const struct pe_fixture_symbol symbols[] = {
    {"comctl32.dll", NULL, 17},
    NAMED("user32", "BeginPaint"), NAMED("user32", "CreateWindowExA"),
    NAMED("user32", "DefWindowProcA"), NAMED("user32", "DispatchMessageA"),
    NAMED("user32", "EndPaint"), NAMED("user32", "GetMessageA"),
    NAMED("user32", "KillTimer"), NAMED("user32", "PostQuitMessage"),
    NAMED("user32", "RegisterClassA"), NAMED("user32", "SendMessageA"),
    NAMED("user32", "SetTimer"), NAMED("user32", "ShowWindow"),
    NAMED("user32", "CheckDlgButton"), NAMED("user32", "IsDlgButtonChecked"),
    NAMED("user32", "GetDlgItemTextA"), NAMED("user32", "SendDlgItemMessageA"),
    NAMED("user32", "LoadIconA"), NAMED("gdi32", "SetPixel"),
    NAMED("comdlg32", "GetOpenFileNameA"), NAMED("comdlg32", "CommDlgExtendedError")
};
#undef NAMED
enum { import_count = sizeof(symbols) / sizeof(symbols[0]) };
#elif defined(TEST_WIN32_COUNTER)
static const char fixture_dll[] = "kernel32.dll";
static const char fixture_symbol[] = "QueryPerformanceCounter";
enum { fixture_cases = 2 };
#else
static const char fixture_dll[] = "ntdll.dll";
static const char fixture_symbol[] = "NtQueryPerformanceCounter";
enum { fixture_cases = 3 };
#endif

#ifndef TEST_DESKTOP
static const struct pe_fixture_symbol symbols[] = {{fixture_dll, fixture_symbol, 0}};
enum { import_count = 1 };

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
        {"missing DLL terminator", dll + sizeof(fixture_dll) - 1, 'x'},
        {"unknown import name", symbol, 'X'},
        {"missing import name terminator", symbol + sizeof(fixture_symbol) - 1, 'x'},
        {"import by ordinal", lookup + 7, 0x80},
        {"lookup RVA outside the image", descriptor + 3, 0xff},
        {"IAT directory RVA outside the image", directory_offset + 12 * 8 + 3, 0xff},
        {"extra address table entry", iat + 8, 1},
        {"delay import directory", directory_offset + 13 * 8, 1},
        {"executable import section", last_section + 39, 0xe0}
    };

    struct pe_import_image rejected;
    if (pe_parse_import_fixture(file, idata_offset + image->idata.size - 1,
                                fixture_dll, fixture_symbol, &rejected)) {
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
        if (pe_parse_import_fixture(copy, nt_pe_fixture_size, fixture_dll,
                                    fixture_symbol, &rejected)) {
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
#else
static bool check_invalid_imports(const struct pe_import_image *image)
{
    unsigned char *copy = malloc(nt_pe_fixture_size);
    if (copy == NULL)
        return false;
    memcpy(copy, nt_pe_fixture_bytes, nt_pe_fixture_size);
    const size_t idata = (size_t)(image->idata.bytes - nt_pe_fixture_bytes);
    const size_t offset = idata + image->slots[0] - image->idata.rva;
    const size_t directories = read_u32(copy + 0x3c) + 4 + 20 + 112;
    size_t descriptor = idata + read_u32(copy + directories + 8) - image->idata.rva;
    while (read_u32(copy + descriptor + 16) != image->slots[0])
        descriptor += 20;
    const size_t lookup = idata + read_u32(copy + descriptor) - image->idata.rva;
    struct pe_import_image rejected;
    bool valid = false;
    const struct { size_t byte; unsigned char value; } cases[] = {
        {0, 18}, /* another ordinal */
        {2, 1}   /* nonzero reserved bits */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const size_t byte = cases[i].byte;
        copy[offset + byte] = copy[lookup + byte] = cases[i].value;
        valid |= pe_parse_import_fixture_many(copy, nt_pe_fixture_size, symbols, import_count, &rejected);
        copy[offset + byte] = nt_pe_fixture_bytes[offset + byte];
        copy[lookup + byte] = nt_pe_fixture_bytes[lookup + byte];
    }
    copy[offset] = 0; /* IAT and lookup disagree */
    valid |= pe_parse_import_fixture_many(copy, nt_pe_fixture_size, symbols, import_count, &rejected);
    free(copy);
    return !valid;
}
#endif

static bool run_fixture(const struct pe_import_image *image)
{
    nt_guest_proc targets[import_count];
    for (size_t i = 0; i < import_count; ++i) {
        targets[i] = symbols[i].name != NULL ? nt_runtime_resolve(symbols[i].module, symbols[i].name) :
                     nt_runtime_resolve_ordinal(symbols[i].module, symbols[i].ordinal);
        if (targets[i] == NULL) {
            if (symbols[i].name != NULL)
                fprintf(stderr, "Missing fixture import %s!%s\n", symbols[i].module, symbols[i].name);
            else
                fprintf(stderr, "Missing fixture import %s!#%u\n", symbols[i].module,
                        (unsigned int)symbols[i].ordinal);
            return false;
        }
    }

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
    _Static_assert(sizeof(targets[0]) == 8, "PE32+ IAT slot size");
    for (size_t i = 0; i < import_count; ++i)
        memcpy(memory + image->slots[i], &targets[i], sizeof(targets[i]));

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
#ifdef TEST_DESKTOP
    if (!nt_runtime_open_desktop()) {
        fputs("Could not open the Linux desktop for the PE fixture\n", stderr);
        munmap(memory, image->image_size);
        return false;
    }
#endif
    const int status = entry();
#ifdef TEST_DESKTOP
    nt_runtime_close_desktop();
#endif
    if (munmap(memory, image->image_size) != 0) {
        perror("munmap");
        return false;
    }

    if (status != 0) {
        fprintf(stderr, "PE import fixture case %d failed\n", status);
        return false;
    }
#ifdef TEST_DESKTOP
    printf("PASS PE32+ window, controls and file dialog ABI through %d imports including COMCTL32 #17\n",
           import_count);
#else
    printf("PASS PE32+ imports %s!%s through the IAT (%d cases)\n",
           fixture_dll, fixture_symbol, fixture_cases);
#endif
    return true;
}

int main(void)
{
    struct pe_import_image image;
    if (!pe_parse_import_fixture_many(nt_pe_fixture_bytes, nt_pe_fixture_size,
                                      symbols, import_count, &image)) {
        fputs("Embedded PE does not match the import fixture profile\n", stderr);
        return EXIT_FAILURE;
    }

    /* Only the image embedded by the build reaches the execution path. */
    return check_invalid_imports(&image) && run_fixture(&image) ? EXIT_SUCCESS : EXIT_FAILURE;
}
