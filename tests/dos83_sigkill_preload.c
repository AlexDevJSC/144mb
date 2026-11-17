/* SPDX-License-Identifier: MIT */
/* Test-only syscall boundary killer, injected into nt-dos83-mediator.
 * No production API or test-only hooks are installed in the runtime.
 * The child dies with SIGKILL immediately at selected transaction phases.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <stdio.h>

static void kill_at(const char *phase)
{
    const char *target = getenv("NT_FS4_TEST_KILL_AT");
    if (target && strcmp(target, phase) == 0) {
        kill(getpid(), SIGKILL);
        _exit(128 + SIGKILL);
    }
}

int fsync(int fd)
{
    int result = (int)syscall(SYS_fsync, fd);
    if (result == 0) {
        char path[64], name[512];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, name, sizeof(name) - 1);
        if (n > 0) {
            name[n] = '\0';
            if (strstr(name, "/dos83-rename-v1.wal") != NULL)
                kill_at("after_wal_fsync");
            if (strstr(name, "/longOriginalNameForCrashTest.txt") != NULL)
                kill_at("after_staged_inode_fsync");
            const size_t length = strlen(name);
            if (length >= 5 && strcmp(name + length - 5, "/left") == 0)
                kill_at("after_parent_fsync");
        }
    }
    return result;
}

static int is_alias(const char *name)
{
    return name && strncmp(name, "user.ntnative.dos83.", 20) == 0;
}

int fsetxattr(int fd, const char *name, const void *value,
             size_t size, int flags)
{
    int result = (int)syscall(SYS_fsetxattr, fd, name, value, size, flags);
    if (result == 0 && is_alias(name))
        kill_at("after_staging_xattr");
    return result;
}

int setxattr(const char *path, const char *name, const void *value,
             size_t size, int flags)
{
    int result = (int)syscall(SYS_setxattr, path, name, value, size, flags);
    if (result == 0 && is_alias(name))
        kill_at("after_staging_xattr");
    return result;
}

/* The runtime invokes SYS_renameat2 directly, bypassing libc renameat2.
 * Removing the previous interception avoids a false coverage assertion.
 * Source-alias removal happens ONLY after successful renameat2 in the
 * mediated path; directory fsync happens after alias cleanup. */
int fremovexattr(int fd, const char *name)
{
    int result = (int)syscall(SYS_fremovexattr, fd, name);
    if (result == 0 && is_alias(name))
        kill_at("after_source_alias_cleanup");
    return result;
}

int removexattr(const char *path, const char *name)
{
    int result = (int)syscall(SYS_removexattr, path, name);
    if (result == 0 && is_alias(name))
        kill_at("after_source_alias_cleanup");
    return result;
}
