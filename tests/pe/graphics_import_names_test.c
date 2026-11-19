/* SPDX-License-Identifier: MIT */
/* Inspect the raw, NUL-terminated IMAGE_IMPORT_BY_NAME records generated
 * by MinGW-w64, rather than trusting a loader error's formatted message.
 * Independent of Vulkan, DXC, runtime export availability and dlopen. */
#define _POSIX_C_SOURCE 200809L
#include "imports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

struct expected_import {
    const char *module, *name;
    unsigned found;
    bool malformed;
};
static void inspect(const struct pe_import *entry, void *opaque)
{
    struct expected_import *expect = opaque;
    if (!entry->name || !entry->module ||
        strcasecmp(entry->module, expect->module) != 0)
        return;
    const size_t length = strlen(expect->name);
    /* A wrongly appended numeric zero must not be treated as an export. */
    if (strncmp(entry->name, expect->name, length) == 0 &&
        entry->name[length] == '0')
        expect->malformed = true;
    if (strcmp(entry->name, expect->name) == 0)
        ++expect->found;
}
static bool inspect_file(const char *path, struct expected_import *expect)
{
    FILE *input = fopen(path, "rb");
    if (!input) return false;
    struct stat info;
    if (fstat(fileno(input), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > 32 * 1024 * 1024) {
        fclose(input);
        return false;
    }
    const size_t count = (size_t)info.st_size;
    unsigned char *bytes = malloc(count);
    if (!bytes) {
        fclose(input);
        return false;
    }
    const bool read_ok = fread(bytes, 1, count, input) == count;
    fclose(input);
    const bool parsed = read_ok && pe_scan_imports(bytes, count, inspect, expect);
    free(bytes);
    return parsed && !expect->malformed && expect->found == 1;
}
int main(int argc, char **argv)
{
    if (argc != 3) {
        fputs("Usage: graphics-import-names GRAPHICS.exe SHADER.exe\n", stderr);
        return 2;
    }
    struct expected_import dxgi = {"sl.interposer.dll", "CreateDXGIFactory1", 0, false};
    struct expected_import shader = {"d3dcompiler_47.dll", "D3DCompile", 0, false};
    if (!inspect_file(argv[1], &dxgi) || !inspect_file(argv[2], &shader)) {
        fprintf(stderr, "FAIL raw PE import strings: dxgi=%u malformed=%d shader=%u malformed=%d\n",
                dxgi.found, dxgi.malformed, shader.found, shader.malformed);
        return 1;
    }
    puts("PASS raw IMAGE_IMPORT_BY_NAME: sl.interposer.dll!CreateDXGIFactory1, "
         "d3dcompiler_47.dll!D3DCompile, exact NUL-terminated names");
    return 0;
}
