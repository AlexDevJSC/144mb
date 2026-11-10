/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "imports.h"
#include "ntnative/runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_FILE_SIZE = 128 * 1024 * 1024 };

struct audit_counts {
    size_t total;
    size_t native;
    size_t ordinals;
};

static bool resolved(const struct pe_import *import)
{
    return import->name != NULL ? nt_runtime_resolve(import->module, import->name) != NULL :
           nt_runtime_resolve_ordinal(import->module, import->ordinal) != NULL;
}

static void count_import(const struct pe_import *import, void *context)
{
    struct audit_counts *counts = context;
    ++counts->total;
    if (resolved(import))
        ++counts->native;
    else if (import->name == NULL)
        ++counts->ordinals;
}

static void print_unresolved(const struct pe_import *import, void *context)
{
    (void)context;
    if (resolved(import))
        return;
    if (import->name != NULL)
        printf("unresolved\t%s\t%s\n", import->module, import->name);
    else
        printf("unresolved\t%s\t#%u\n", import->module, (unsigned int)import->ordinal);
}

int main(int argc, char **argv)
{
    const bool list = argc == 3 && strcmp(argv[1], "--unresolved") == 0;
    if (argc != 2 && !list) {
        fputs("Usage: pe-import-audit [--unresolved] FILE.exe\n", stderr);
        return 2;
    }

    const char *path = argv[list ? 2 : 1];
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror(path);
        return 2;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > MAX_FILE_SIZE) {
        fputs("Expected a nonempty regular file of at most 128 MiB\n", stderr);
        close(fd);
        return 2;
    }

    const size_t length = (size_t)info.st_size;
    FILE *stream = fdopen(fd, "rb");
    if (stream == NULL) {
        perror("fdopen");
        close(fd);
        return 2;
    }
    unsigned char *file = malloc(length);
    if (file == NULL || fread(file, 1, length, stream) != length) {
        fputs("Could not read the PE file\n", stderr);
        fclose(stream);
        free(file);
        return 2;
    }
    fclose(stream);

    struct audit_counts counts = {0};
    const bool valid = pe_scan_imports(file, length, count_import, &counts);
    if (valid && list)
        pe_scan_imports(file, length, print_unresolved, NULL);
    free(file);
    if (!valid) {
        fputs("Invalid or unsupported AMD64 PE import table\n", stderr);
        return 2;
    }

    printf("%zu direct imports, %zu with native equivalents, %zu unresolved (%zu by ordinal)\n",
           counts.total, counts.native, counts.total - counts.native, counts.ordinals);
    return counts.native == counts.total ? 0 : 1;
}
