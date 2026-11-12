/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "image.h"
#include "pe_bytes.h"
#include "ntnative/runtime.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static void w16(unsigned char *p, uint16_t n) { p[0] = (unsigned char)n; p[1] = (unsigned char)(n >> 8); }
static void w32(unsigned char *p, uint32_t n) { w16(p, (uint16_t)n); w16(p + 2, (uint16_t)(n >> 16)); }
static void w64(unsigned char *p, uint64_t n) { w32(p, (uint32_t)n); w32(p + 4, (uint32_t)(n >> 32)); }

static int NT_MS_ABI answer(void) { return 42; }

static uintptr_t resolver(const char *module, const char *name,
                          uint16_t ordinal, void *ctx)
{
    (void)ctx;
    if (strcmp(module, "demo.dll") || name == NULL ||
        strcmp(name, "Return42") || ordinal != 0)
        return 0;
    nt_guest_proc f = (nt_guest_proc)answer;
    uintptr_t result = 0;
    memcpy(&result, &f, sizeof(result));
    return result;
}

static uintptr_t forward_resolver(const char *module, const char *name,
                                  uint16_t ordinal, void *context)
{
    (void)context;
    nt_guest_proc function = (nt_guest_proc)answer;
    uintptr_t address = 0;
    memcpy(&address, &function, sizeof(address));
    if (strcmp(module, "KERNEL32.dll") == 0 && name &&
        strcmp(name, "Sleep") == 0 && ordinal == 0)
        return address;
    if (strcmp(module, "NTDLL.dll") == 0 && name == NULL && ordinal == 51)
        return address;
    return 0;
}

static void section(unsigned char *sh, const char *name,
                    uint32_t rva, uint32_t raw, uint32_t flags)
{
    memcpy(sh, name, strlen(name));
    w32(sh + 8, 0x200);
    w32(sh + 12, rva);
    w32(sh + 16, 0x200);
    w32(sh + 20, raw);
    w32(sh + 36, flags);
}

static void make_pe(unsigned char *file)
{
    memset(file, 0, 0x800);
    memcpy(file, "MZ", 2);
    w32(file + 0x3c, 0x80);
    memcpy(file + 0x80, "PE\0\0", 4);
    unsigned char *coff = file + 0x84;
    w16(coff, 0x8664);
    w16(coff + 2, 3);
    w16(coff + 16, 240);
    w16(coff + 18, 0x0002);
    unsigned char *opt = file + 0x98;
    w16(opt, 0x20b);
    w32(opt + 16, 0x1000);
    w64(opt + 24, UINT64_C(0x140000000));
    w32(opt + 32, 0x1000);
    w32(opt + 36, 0x200);
    w32(opt + 56, 0x4000);
    w32(opt + 60, 0x200);
    w16(opt + 68, 3);
    w32(opt + 108, 16);
    w32(opt + 112 + 8, 0x2000);
    w32(opt + 112 + 12, 40);
    w32(opt + 112 + 5 * 8, 0x3000);
    w32(opt + 112 + 5 * 8 + 4, 12);
    unsigned char *sh = opt + 240;
    section(sh, ".text", 0x1000, 0x200, 0x60000020);
    section(sh + 40, ".idata", 0x2000, 0x400, 0xc0000040);
    section(sh + 80, ".reloc", 0x3000, 0x600, 0x40000040);

    /* sub rsp,40; call qword ptr [rip+0x1056]; add rsp,40; ret.
     * This is an MS x64 guest-to-host import with a real shadow space.
     */
    const unsigned char code[] = {0x48,0x83,0xec,0x28,0xff,0x15,0x56,0x10,0x00,0x00,
                                  0x48,0x83,0xc4,0x28,0xc3};
    memcpy(file + 0x200, code, sizeof(code));
    unsigned char *idata = file + 0x400;
    w32(idata, 0x2050);
    w32(idata + 12, 0x2080);
    w32(idata + 16, 0x2060);
    w64(idata + 0x50, 0x2090);
    w64(idata + 0x60, 0x2090);
    memcpy(idata + 0x80, "demo.dll", 9);
    w16(idata + 0x90, 0);
    memcpy(idata + 0x92, "Return42", 9);
    w64(idata + 0xc0, UINT64_C(0x140000000) + 123);
    unsigned char *reloc = file + 0x600;
    w32(reloc, 0x2000);
    w32(reloc + 4, 12);
    w16(reloc + 8, 0xa0c0); /* DIR64 */
}

static void make_tls_pe(unsigned char *bytes)
{
    make_pe(bytes);
    const uint64_t preferred = UINT64_C(0x140000000);
    unsigned char *opt = bytes + 0x98;
    unsigned char *idata = bytes + 0x400;
    unsigned char *reloc = bytes + 0x600;
    w32(opt + 112 + 9 * 8, 0x2100);  /* IMAGE_DIRECTORY_ENTRY_TLS */
    w32(opt + 112 + 9 * 8 + 4, 40);
    w32(opt + 112 + 5 * 8 + 4, 20);
    /* IMAGE_TLS_DIRECTORY64 with initialized bytes and zero fill. */
    w64(idata + 0x100, preferred + 0x2158);
    w64(idata + 0x108, preferred + 0x2160);
    w32(idata + 0x120, 8);
    w32(idata + 0x124, 0x00500000);
    memcpy(idata + 0x158, "template", 8);
    w64(idata + 0x110, preferred + 0x2150); /* AddressOfIndex */
    w64(idata + 0x118, preferred + 0x2140); /* AddressOfCallbacks */
    w64(idata + 0x140, preferred + 0x1100); /* one callback */
    bytes[0x200 + 0x100] = 0xc3; /* guest callback: ret */
    w32(reloc + 4, 20);
    w16(reloc + 8, 0xa0c0);
    w16(reloc + 10, 0xa100);
    w16(reloc + 12, 0xa108);
    w16(reloc + 14, 0xa110);
    w16(reloc + 16, 0xa118);
    w16(reloc + 18, 0xa140);
}

/* A synthetic DLL exports the same MS x64 code via ordinal #5 and
 * by name. None of these bytes come from an external executable. */
static void make_export_dll(unsigned char *bytes)
{
    make_pe(bytes);
    w16(bytes + 0x84 + 18, 0x2002); /* IMAGE_FILE_DLL | EXECUTABLE_IMAGE */
    w32(bytes + 0x98 + 16, 0);      /* Resource-only DLLs may omit DllMain. */
    w32(bytes + 0x98 + 112, 0x2100); /* IMAGE_DIRECTORY_ENTRY_EXPORT */
    w32(bytes + 0x98 + 116, 0x100);

    unsigned char *idata = bytes + 0x400;
    w32(idata + 0x100 + 12, 0x2160); /* Name */
    w32(idata + 0x100 + 16, 5);      /* Base ordinal */
    w32(idata + 0x100 + 20, 1);      /* NumberOfFunctions */
    w32(idata + 0x100 + 24, 1);      /* NumberOfNames */
    w32(idata + 0x100 + 28, 0x2130); /* AddressOfFunctions */
    w32(idata + 0x100 + 32, 0x2140); /* AddressOfNames */
    w32(idata + 0x100 + 36, 0x2150); /* AddressOfNameOrdinals */
    w32(idata + 0x130, 0x1000);      /* .text entry point RVA */
    w32(idata + 0x140, 0x2170);      /* "Return42" RVA */
    w16(idata + 0x150, 0);           /* Index into function RVAs */
    memcpy(idata + 0x160, "demo.dll", 9);
    memcpy(idata + 0x170, "Return42", 9);
}

static void rejected(unsigned char *bytes, const char *what)
{
    struct pe_image image = {0};
    char reason[128] = {0};
    assert(!pe_image_load(bytes, 0x800, resolver, NULL, &image, reason, sizeof(reason)));
    assert(image.base == NULL);
    printf("PASS rejects %s: %s\n", what, reason);
}

int main(void)
{
    unsigned char bytes[0x800];
    make_pe(bytes);
    const uint64_t preferred = UINT64_C(0x140000000);
    void *blocker = mmap((void *)(uintptr_t)preferred, 0x4000, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    struct pe_image image = {0};
    char reason[128];
    assert(pe_image_load(bytes, sizeof(bytes), resolver, NULL, &image, reason, sizeof(reason)));
    assert(image.base != NULL && image.size == 0x4000);
    assert(read_u64(image.base + 0x20c0) == (uintptr_t)image.base + 123);
    const void *entry_address = image.base + image.entry_rva;
    int (NT_MS_ABI *entry)(void);
    memcpy(&entry, &entry_address, sizeof(entry));
    assert(entry() == 42);
    puts("PASS PE32+ general mapping, DIR64 relocation, IAT and MS x64 execution");
    pe_image_unload(&image);
    if (blocker != MAP_FAILED)
        munmap(blocker, 0x4000);
    assert(image.base == NULL);

    unsigned char signed_image[0x4010] = {0};
    make_pe(signed_image);
    /* The certificate table is file data, outside the mapped image. */
    w32(signed_image + 0x98 + 112 + 4 * 8, 0x4000);
    w32(signed_image + 0x98 + 112 + 4 * 8 + 4, 16);
    assert(pe_image_load(signed_image, sizeof(signed_image), resolver, NULL,
                         &image, reason, sizeof(reason)));
    pe_image_unload(&image);
    assert(!pe_image_load(signed_image, sizeof(signed_image) - 1, resolver, NULL,
                          &image, reason, sizeof(reason)));
    puts("PASS certificate file bounds and truncated certificate rejection");

    make_tls_pe(bytes);
    assert(pe_image_load(bytes, sizeof(bytes), resolver, NULL, &image, reason, sizeof(reason)));
    assert(image.tls.callbacks_rva == 0x2140 && image.tls.callback_count == 1);
    assert(image.tls.data_rva == 0x2158 && image.tls.bytes == 8 && image.tls.zero_fill == 8);
    assert(memcmp(image.base + image.tls.data_rva, "template", 8) == 0);
    assert(read_u32(image.base + 0x2150) == 0);
    assert(read_u64(image.base + 0x2140) == (uintptr_t)image.base + 0x1100);
    const uint64_t tls_address = read_u64(image.base + 0x2140);
    void (NT_MS_ABI *tls_callback)(void *, uint32_t, void *);
    memcpy(&tls_callback, &tls_address, sizeof(tls_callback));
    tls_callback(image.base, 1, NULL);
    pe_image_unload(&image);
    puts("PASS static TLS template, callback mapping and MS x64 process-attach call");

    make_tls_pe(bytes);
    w32(bytes + 0x400 + 0x120, 4096);
    rejected(bytes, "oversized static TLS");
    make_tls_pe(bytes);
    w64(bytes + 0x400 + 0x108, UINT64_C(0x140002157));
    rejected(bytes, "reversed TLS range");

    make_pe(bytes);
    w32(bytes + 0x98 + 240 + 36, 0xe0000020);
    rejected(bytes, "executable+writable section");
    make_pe(bytes);
    w32(bytes + 0x98 + 240 + 40 + 12, 0x1000);
    rejected(bytes, "overlapping virtual sections");
    make_pe(bytes);
    w32(bytes + 0x98 + 112 + 9*8, 0x2000);
    w32(bytes + 0x98 + 112 + 9*8 + 4, 24);
    rejected(bytes, "TLS before executable entry");
    make_pe(bytes);
    bytes[0x400 + 0x92] = 'X';
    rejected(bytes, "missing imported function");
    make_pe(bytes);
    w32(bytes + 0x98 + 112 + 8, 0x5000);
    rejected(bytes, "out-of-range import directory");
    make_pe(bytes);
    w32(bytes + 0x98 + 112 + 3*8, 0x2001);
    w32(bytes + 0x98 + 112 + 3*8 + 4, 12);
    rejected(bytes, "unaligned exception directory");
    make_pe(bytes);
    w32(bytes + 0x600 + 4, 7);
    w64(bytes + 0x98 + 24, UINT64_C(0x1000)); /* force an ASLR relocation */
    rejected(bytes, "malformed base-relocation block");

    /* Delay imports are patched lazily by the guest CRT. The image loader
     * must neither resolve optional DLLs eagerly nor discard the delay IAT. */
    make_pe(bytes);
    w32(bytes + 0x98 + 112 + 13 * 8, 0x2100);
    w32(bytes + 0x98 + 112 + 13 * 8 + 4, 64);
    w32(bytes + 0x400 + 0x100, 1);   /* dlattrRva */
    w32(bytes + 0x400 + 0x104, 0x2150); /* szName */
    w32(bytes + 0x400 + 0x108, 0x2160); /* phmod */
    w32(bytes + 0x400 + 0x10c, 0x2170); /* pIAT */
    w32(bytes + 0x400 + 0x110, 0x2180); /* pINT */
    memcpy(bytes + 0x400 + 0x150, "optional.dll", 13);
    w64(bytes + 0x400 + 0x170, UINT64_C(0x1100));
    assert(pe_image_load(bytes, sizeof(bytes), resolver, NULL,
                         &image, reason, sizeof(reason)));
    assert(read_u32(image.base + 0x2100) == 1);
    assert(read_u64(image.base + 0x2170) == UINT64_C(0x1100));
    pe_image_unload(&image);
    make_pe(bytes);
    w32(bytes + 0x98 + 112 + 13 * 8, 0x3ff0);
    w32(bytes + 0x98 + 112 + 13 * 8 + 4, 64);
    rejected(bytes, "delay-import directory beyond the image");
    puts("PASS CRT-owned delay imports are preserved for first-use resolution");

    /* The EXE entry point and the DLL loader must not be interchangeable. */
    make_export_dll(bytes);
    rejected(bytes, "DLL passed to the EXE-only loader");
    make_pe(bytes);
    assert(!pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                  &image, reason, sizeof(reason)));
    puts("PASS DLL and EXE image types remain distinct");

    make_export_dll(bytes);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(image.entry_rva == 0 && image.export_rva == 0x2100);
    struct pe_image_export by_name = {0}, by_ordinal = {0};
    assert(pe_image_lookup_export(&image, "Return42", 0, &by_name));
    assert(pe_image_lookup_export(&image, NULL, 5, &by_ordinal));
    assert(by_name.address == by_ordinal.address &&
           by_name.address == (uintptr_t)(image.base + 0x1000) &&
           by_name.forwarder == NULL);
    int (NT_MS_ABI *dll_entry)(void);
    const uintptr_t code_address = by_name.address;
    memcpy(&dll_entry, &code_address, sizeof(dll_entry));
    assert(dll_entry() == 42);
    assert(!pe_image_lookup_export(&image, "not-exported", 0, &by_name));
    assert(by_name.address == 0 && by_name.forwarder == NULL);
    assert(!pe_image_lookup_export(&image, NULL, 4, &by_name));
    assert(!pe_image_lookup_export(&image, NULL, 6, &by_name));
    pe_image_unload(&image);
    puts("PASS mapped DLL exports by name and base-relative ordinal");

    /* Calling DllMain is a deliberate operation: DLL mapping must not
     * execute code on its own. A synthetic AMD64 DLL can opt into it
     * after its imports and guest thread context have been arranged. */
    make_export_dll(bytes);
    w32(bytes + 0x98 + 16, 0x1000); /* OptionalHeader.AddressOfEntryPoint */
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(pe_image_run_library_entry(&image, 1));
    assert(pe_image_run_library_entry(&image, 2));
    assert(pe_image_run_library_entry(&image, 3));
    assert(pe_image_run_library_entry(&image, 0));
    assert(!pe_image_run_library_entry(&image, 4));
    pe_image_unload(&image);
    make_export_dll(bytes);
    w32(bytes + 0x98 + 16, 0x2100); /* Export data is not executable. */
    assert(!pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                  &image, reason, sizeof(reason)));
    /* The loader refuses bad entry metadata before mapping. Also guard
     * the explicit activation API against invalid state supplied later. */
    make_export_dll(bytes);
    w32(bytes + 0x98 + 16, 0x1000);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    image.entry_rva = 0x2100;
    assert(!pe_image_run_library_entry(&image, 1));
    pe_image_unload(&image);
    puts("PASS explicit DLL_PROCESS_ATTACH/DETACH and guarded entry RVA");

    make_export_dll(bytes);
    w32(bytes + 0x400 + 0x130, 0x2180);
    memcpy(bytes + 0x400 + 0x180, "KERNEL32.Sleep", 15);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(pe_image_lookup_export(&image, "Return42", 0, &by_name));
    assert(by_name.address == 0 && by_name.forwarder != NULL &&
           strcmp(by_name.forwarder, "KERNEL32.Sleep") == 0);
    uintptr_t forwarded = 0;
    assert(pe_image_resolve_export(&image, "Return42", 0, forward_resolver, NULL, &forwarded));
    nt_guest_proc expected_function = (nt_guest_proc)answer;
    uintptr_t expected_address = 0;
    memcpy(&expected_address, &expected_function, sizeof(expected_address));
    assert(forwarded == expected_address);
    assert(!pe_image_resolve_export(&image, "Return42", 0, NULL, NULL, &forwarded));
    assert(forwarded == 0);
    pe_image_unload(&image);
    puts("PASS forwarded exports resolve via the shared host import resolver");

    make_export_dll(bytes);
    w32(bytes + 0x400 + 0x130, 0x2180);
    memcpy(bytes + 0x400 + 0x180, "NTDLL.#51", 10);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(pe_image_resolve_export(&image, NULL, 5, forward_resolver, NULL, &forwarded));
    assert(forwarded == expected_address);
    pe_image_unload(&image);

    make_export_dll(bytes);
    w32(bytes + 0x400 + 0x130, 0x2180);
    memcpy(bytes + 0x400 + 0x180, "NTDLL.#0", 9);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(!pe_image_resolve_export(&image, "Return42", 0, forward_resolver, NULL, &forwarded));
    pe_image_unload(&image);
    puts("PASS forwarded ordinals and malformed forwarders are handled safely");

    make_export_dll(bytes);
    w32(bytes + 0x400 + 0x100 + 28, 0x3ffe);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(!pe_image_lookup_export(&image, "Return42", 0, &by_name));
    pe_image_unload(&image);
    make_export_dll(bytes);
    w16(bytes + 0x400 + 0x150, 2);
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(!pe_image_lookup_export(&image, "Return42", 0, &by_name));
    pe_image_unload(&image);
    make_export_dll(bytes);
    w32(bytes + 0x400 + 0x130, 0x21ff);
    bytes[0x400 + 0x1ff] = 'A';
    assert(pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                 &image, reason, sizeof(reason)));
    assert(!pe_image_lookup_export(&image, "Return42", 0, &by_name));
    pe_image_unload(&image);
    puts("PASS malformed export bounds, ordinals and unterminated forwarders rejected");

    make_export_dll(bytes);
    w32(bytes + 0x98 + 112, 0x3ffe);
    assert(!pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                  &image, reason, sizeof(reason)));
    make_tls_pe(bytes);
    w16(bytes + 0x84 + 18, 0x2002);
    assert(!pe_image_load_library(bytes, sizeof(bytes), resolver, NULL,
                                  &image, reason, sizeof(reason)));
    puts("PASS unreadable export directories and unsafe DLL TLS rejected");
    return 0;
}
