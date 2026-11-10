/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "imports.h"
#include "ntnative/runtime.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MAX_FILE_SIZE = 128 * 1024 * 1024 };

struct audit_counts {
    size_t total;
    size_t native;
    size_t ordinals;
};

static void count_import(const struct pe_import *import, void *context)
{
    struct audit_counts *counts = context;
    ++counts->total;
    if (import->name == NULL)
        ++counts->ordinals;
    if (nt_runtime_resolve(import->module, import->name) != NULL)
        ++counts->native;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fputs("Usage: pe-import-audit FILE.exe\n", stderr);
        return 2;
    }

    const int fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror(argv[1]);
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
    free(file);
    if (!valid) {
        fputs("Invalid or unsupported AMD64 PE import table\n", stderr);
        return 2;
    }

    printf("%zu direct imports, %zu with native equivalents, %zu unresolved (%zu by ordinal)\n",
           counts.total, counts.native, counts.total - counts.native, counts.ordinals);
    return counts.native == counts.total ? 0 : 1;
}
