/* SPDX-License-Identifier: MIT */
/* P2.3: deny unmediated writes to the backing tree for arbitrary Linux
 * descendants, without needing user/mount namespaces or setuid helpers.
 *
 * Requires Landlock ABI >=3: REFER (rename/link, ABI2) and TRUNCATE (ABI3)
 * must be enforced, otherwise a subprocess could bypass DOS 8.3 mediation.
 * No best-effort/weak-profile fallback is allowed for --exec.
 *
 * This protects path-based name/data writes, NOT all metadata operations
 * (chmod/chown may be possible), and not programs started outside this
 * wrapper. FUSE must already be mounted with subtype=nt-dos83.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/landlock.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>

static uint64_t write_rights(void)
{
    uint64_t mask =
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG |
        LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO |
        LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM |
        LANDLOCK_ACCESS_FS_REFER |
        LANDLOCK_ACCESS_FS_TRUNCATE;
    /* Do NOT handle DEVICE_IOCTL here: blanket denial would break Vulkan,
     * terminal and other ordinary Linux clients. Raw block-device access
     * requires separate host DAC/SELinux confinement by the administrator. */
    return mask;
}

/* Confirm this is OUR FUSE filesystem, not merely some writable directory
 * or arbitrary mount supplied by a hostile caller. Mountinfo fields are
 * octal-escaped; compare a canonical absolute path with decoded mountpoint. */
static void unescape_mountpoint(char *path)
{
    char *read = path, *write = path;
    while (*read) {
        if (read[0] == '\\' && read[1] && read[2] && read[3] &&
            read[1] >= '0' && read[1] <= '7' &&
            read[2] >= '0' && read[2] <= '7' &&
            read[3] >= '0' && read[3] <= '7') {
            *write++ = (char)((read[1]-'0') * 64 +
                              (read[2]-'0') * 8 + read[3]-'0');
            read += 4;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static bool verified_mediation_mount(const char *view)
{
    struct statfs st;
    if (statfs(view, &st) != 0 ||
        (unsigned long)st.f_type != (unsigned long)FUSE_SUPER_MAGIC)
        return false;
    FILE *file = fopen("/proc/self/mountinfo", "r");
    if (!file)
        return false;
    char *line = NULL;
    size_t length = 0;
    bool match = false;
    while (getline(&line, &length, file) >= 0) {
        char path[PATH_MAX], type[128], source[128];
        const char *suffix = strstr(line, " - ");
        if (!suffix)
            continue;
        if (sscanf(line, "%*u %*u %*s %*s %4095s", path) != 1)
            continue;
        unescape_mountpoint(path);
        if (strcmp(path, view))
            continue;
        if (sscanf(suffix + 3, "%127s %127s", type, source) == 2 &&
            !strcmp(type, "fuse.nt-dos83") &&
            !strcmp(source, "nt-dos83")) {
            match = true;
            break;
        }
    }
    free(line);
    fclose(file);
    return match;
}

/* Dropping inherited write-capable descriptors is essential. Landlock
 * cannot revoke access through descriptors opened BEFORE restrict_self. */
static bool close_inherited_descriptors(void)
{
    /* Landlock cannot revoke already-open writes. The usual stdio FDs
     * are inherited deliberately, so fail closed if any is a host regular
     * file or directory (e.g. stdout pre-opened on the backing volume). */
    for (int fd = 0; fd < 3; ++fd) {
        struct stat st;
        if (fstat(fd, &st) == 0 &&
            (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode) ||
             S_ISBLK(st.st_mode))) {
            errno = EPERM;
            return false;
        }
    }
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0)
        return true;
    if (errno != ENOSYS && errno != EINVAL)
        return false;
#endif
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) != 0)
        return false;
    rlim_t max = lim.rlim_cur;
    if (max == RLIM_INFINITY || max > 1048576)
        max = 1048576;
    /* On older kernels, do not leave unexamined descriptors > 1M open:
     * fail closed rather than claim complete inherited-FD isolation. */
    if (lim.rlim_cur == RLIM_INFINITY || lim.rlim_cur > 1048576) {
        errno = ENOTSUP;
        return false;
    }
    for (rlim_t fd = 3; fd < max; ++fd)
        (void)close((int)fd);
    return true;
}

static int restrict_writes_to_view(const char *view)
{
    int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                           LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 3) {
        fprintf(stderr, "P2.3 requires Landlock ABI >= 3; available=%d errno=%d\n",
                abi, errno);
        return 3;
    }
    int view_fd = open(view, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (view_fd < 0) {
        perror("open mediated view");
        return 3;
    }
    struct landlock_ruleset_attr ruleset = {
        .handled_access_fs = write_rights()
    };
    int ruleset_fd = (int)syscall(SYS_landlock_create_ruleset,
                                  &ruleset, sizeof(ruleset), 0);
    if (ruleset_fd < 0) {
        perror("landlock_create_ruleset");
        close(view_fd);
        return 3;
    }
    struct landlock_path_beneath_attr rule = {
        .allowed_access = ruleset.handled_access_fs,
        .parent_fd = view_fd
    };
    if (syscall(SYS_landlock_add_rule, ruleset_fd,
                LANDLOCK_RULE_PATH_BENEATH, &rule, 0) < 0 ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        syscall(SYS_landlock_restrict_self, ruleset_fd, 0) < 0) {
        perror("Landlock could not apply mandatory view-only policy");
        close(view_fd);
        close(ruleset_fd);
        return 3;
    }
    close(view_fd);
    close(ruleset_fd);
    if (!close_inherited_descriptors()) {
        perror("inherited descriptor cleanup");
        return 3;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--probe") == 0) {
        int abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0,
                               LANDLOCK_CREATE_RULESET_VERSION);
        if (abi < 3) {
            fprintf(stderr, "SKIP Landlock v3 unavailable (abi=%d, errno=%d)\n",
                    abi, errno);
            return 3;
        }
        printf("PASS Landlock ABI %d supports REFER and TRUNCATE\n", abi);
        return 0;
    }
    if (argc < 5 || strcmp(argv[1], "--view") != 0 ||
        strcmp(argv[3], "--") != 0) {
        fprintf(stderr,
                "Usage: %s --view FUSE_MOUNTPOINT -- COMMAND [ARGS...]\n",
                argv[0]);
        return 2;
    }
    char view[PATH_MAX];
    if (!realpath(argv[2], view)) {
        perror("realpath view");
        return 3;
    }
    if (!verified_mediation_mount(view)) {
        fprintf(stderr,
                "Refusing an unverified or unmounted nt-dos83 FUSE view: %s\n",
                view);
        return 3;
    }
    int result = restrict_writes_to_view(view);
    if (result)
        return result;
    execvp(argv[4], argv + 4);
    perror("execvp");
    return 127;
}
