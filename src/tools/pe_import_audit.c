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

/* Return 0 if all direct imports resolve, 1 for unresolved imports and 2
 * for unreadable or invalid PE files. Verify the whole set before execution;
 * a valid DLL export name alone never proves functional compatibility. */
static int audit_file(const char *path, bool list, bool batch)
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        perror(path);
        return 2;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size <= 0 || info.st_size > MAX_FILE_SIZE) {
        fprintf(stderr, "%s: expected nonempty regular file of at most 128 MiB\n", path);
        close(fd);
        return 2;
    }
    const size_t length = (size_t)info.st_size;
    FILE *stream = fdopen(fd, "rb");
    if (!stream) {
        perror("fdopen");
        close(fd);
        return 2;
    }
    unsigned char *file = malloc(length);
    if (!file || fread(file, 1, length, stream) != length) {
        fprintf(stderr, "%s: could not read PE file\n", path);
        fclose(stream);
        free(file);
        return 2;
    }
    fclose(stream);
    struct audit_counts counts = {0};
    const bool valid = pe_scan_imports(file, length, count_import, &counts);
    if (batch)
        printf("%s: ", path);
    if (valid && list)
        pe_scan_imports(file, length, print_unresolved, NULL);
    free(file);
    if (!valid) {
        fprintf(stderr, "%s: invalid or unsupported AMD64 PE import table\n", path);
        return 2;
    }
    printf("%zu direct imports, %zu with native equivalents, %zu unresolved (%zu by ordinal)\n",
           counts.total, counts.native, counts.total - counts.native, counts.ordinals);
    return counts.native == counts.total ? 0 : 1;
}

int main(int argc, char **argv)
{
    const bool verify = argc >= 3 && strcmp(argv[1], "--verify") == 0;
    const bool list = argc == 3 && strcmp(argv[1], "--unresolved") == 0;
    if (!verify && argc != 2 && !list) {
        fputs("Usage: pe-import-audit [--unresolved] FILE.exe\n"
              "       pe-import-audit --verify FILE.exe [FILE2.exe ...]\n", stderr);
        return 2;
    }
    if (!verify)
        return audit_file(argv[list ? 2 : 1], list, false);
    int status = 0;
    for (int i = 2; i < argc; ++i) {
        const int current = audit_file(argv[i], true, true);
        if (current > status)
            status = current;
    }
    return status;
}
