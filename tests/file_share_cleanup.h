/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_TEST_FILE_SHARE_CLEANUP_H
#define NTNATIVE_TEST_FILE_SHARE_CLEANUP_H

/* Test-only teardown. Production registries intentionally remain linked:
 * removing a live registry inode would split interprocess sharing state. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool nt_test_cleanup_share_registry(const char *root)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/:ntnative-file-leases", root) >=
        (int)sizeof(path))
        return false;
    DIR *dir = opendir(path);
    if (dir == NULL)
        return errno == ENOENT;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0)
                ok = false;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        if (unlinkat(dirfd(dir), entry->d_name, 0) != 0) {
            ok = false;
            break;
        }
    }
    if (closedir(dir) != 0)
        ok = false;
    return ok && rmdir(path) == 0;
}

#endif
