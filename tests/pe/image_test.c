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
    return 0;
}
