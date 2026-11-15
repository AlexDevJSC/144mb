/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "guest_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

static int open_directory(int parent, const char *name, bool private)
{
    if (mkdirat(parent, name, 0700) != 0 && errno != EEXIST)
        return -1;
    const int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (private ? O_NOFOLLOW : 0);
    const int fd = openat(parent, name, flags);
    if (fd < 0)
        return -1;
    struct stat info;
    if (private && (fstat(fd, &info) != 0 || info.st_uid != geteuid() ||
                    (info.st_mode & 0077) != 0)) {
        close(fd);
        errno = EACCES;
        return -1;
    }
    return fd;
}

static int open_state_directory(char *path)
{
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    char *cursor = NULL;
    for (char *name = strtok_r(path, "/", &cursor); name != NULL;
         name = strtok_r(NULL, "/", &cursor)) {
        const int next = open_directory(fd, name, false);
        const int error = errno;
        close(fd);
        if (next < 0) {
            errno = error;
            return -1;
        }
        fd = next;
    }
    return fd;
}

/*
 * The host directory named "C:" is an ordinary Linux directory, not an
 * NTFS volume. All guest DOS paths are mapped to entries beneath its fd.
 * Keep the directory layout only; do not ship fake Windows DLLs or data.
 */
static bool prepare_tree_entry(int root, const char *relative)
{
    char path[128];
    if (snprintf(path, sizeof(path), "%s", relative) >= (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return false;
    }
    int parent = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (parent < 0)
        return false;
    char *cursor = NULL;
    for (char *part = strtok_r(path, "/", &cursor); part != NULL;
         part = strtok_r(NULL, "/", &cursor)) {
        const int next = open_directory(parent, part, true);
        const int error = errno;
        close(parent);
        if (next < 0) {
            errno = error;
            return false;
        }
        parent = next;
    }
    return close(parent) == 0;
}

static bool prepare_user_directories(int drive)
{
    static const char *const tree[] = {
        "Windows", "Windows/System32", "Windows/SysWOW64",
        "Windows/Fonts", "Windows/Temp",
        "Program Files", "Program Files (x86)", "ProgramData",
        "Users/guest", "Users/guest/AppData",
        "Users/guest/AppData/Roaming", "Users/guest/AppData/Local",
        "Users/guest/Desktop", "Users/guest/Documents", "Users/guest/Downloads",
        "Users/guest/Music", "Users/guest/Pictures", "Users/guest/Videos",
        "Users/Public", "Temp"
    };
    for (size_t i = 0; i < sizeof(tree) / sizeof(tree[0]); ++i)
        if (!prepare_tree_entry(drive, tree[i]))
            return false;
    return true;
}

/*
 * Migrate the former "drive-c" directory in place, preserving all original
 * files and inode identities. Never merge two independent profiles silently.
 * A runtime-directory lock makes concurrent launches of this implementation
 * choose the same root; RENAME_NOREPLACE protects against other writers.
 */
static int open_transcribed_c_directory(int runtime)
{
    if (flock(runtime, LOCK_EX) != 0)
        return -1;
    struct stat modern, legacy;
    const bool has_modern = fstatat(runtime, "C:", &modern, AT_SYMLINK_NOFOLLOW) == 0;
    int error = errno;
    if (!has_modern && error != ENOENT)
        goto fail;
    const bool has_legacy = fstatat(runtime, "drive-c", &legacy, AT_SYMLINK_NOFOLLOW) == 0;
    error = errno;
    if (!has_legacy && error != ENOENT)
        goto fail;
    if (has_modern && has_legacy) {
        error = EEXIST; /* Explicit manual choice: do not discard either tree. */
        goto fail;
    }
    if (has_legacy) {
        if (!S_ISDIR(legacy.st_mode) || legacy.st_uid != geteuid() ||
            (legacy.st_mode & 0077) != 0) {
            error = EACCES;
            goto fail;
        }
        if (renameat2(runtime, "drive-c", runtime, "C:", RENAME_NOREPLACE) != 0) {
            error = errno;
            goto fail;
        }
    }
    const int drive = open_directory(runtime, "C:", true);
    error = errno;
    (void)flock(runtime, LOCK_UN);
    errno = error;
    return drive;

fail:
    (void)flock(runtime, LOCK_UN);
    errno = error;
    return -1;
}

int nt_open_guest_profile(const char *state_home, const char *home)
{
    char state[PATH_MAX];
    int length;
    if (state_home != NULL && state_home[0] == '/') {
        length = snprintf(state, sizeof(state), "%s", state_home);
    } else if (home != NULL && home[0] == '/') {
        length = snprintf(state, sizeof(state), "%s/.local/state", home);
    } else {
        errno = EINVAL;
        return -1;
    }
    if (length < 0 || (size_t)length >= sizeof(state)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    const int base = open_state_directory(state);
    if (base < 0)
        return -1;
    const int runtime = open_directory(base, "nt-native-linux", true);
    int error = errno;
    close(base);
    if (runtime < 0) {
        errno = error;
        return -1;
    }
    const int drive = open_transcribed_c_directory(runtime);
    error = errno;
    close(runtime);
    if (drive < 0) {
        errno = error;
        return -1;
    }
    if (!prepare_user_directories(drive)) {
        error = errno;
        close(drive);
        errno = error;
        return -1;
    }
    return drive;
}

int nt_open_shared_folder(const char *directory, const char *home)
{
    if (directory == NULL || directory[0] != '/' || home == NULL || home[0] != '/') {
        errno = EINVAL;
        return -1;
    }
    char target[PATH_MAX], personal[PATH_MAX];
    if (realpath(directory, target) == NULL || realpath(home, personal) == NULL)
        return -1;
    const size_t length = strlen(target);
    /* XDG may return the home directory for a disabled user folder. */
    if (length == 1 || (strncmp(personal, target, length) == 0 &&
                        (personal[length] == '/' || personal[length] == '\0'))) {
        errno = EACCES;
        return -1;
    }
    return open(target, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}


/*
 * Persistent native Linux directory mappings. These are explicit grants:
 * the launcher mounts only the chosen host directories and the guest reads
 * the original inodes directly. No copies, overlays or automatic HOME
 * exposure are introduced by this configuration.
 */
#define NT_SHARE_FILE "shares.conf"
#define NT_SHARE_LOCK ".shares.lock"
#define NT_DRIVE_FILE "drives.conf"
#define NT_DRIVE_LOCK ".drives.lock"
#define NT_SHARE_MAX_BYTES 32768

static int share_config_directory(const char *config_home, const char *home, bool create)
{
    char path[PATH_MAX];
    int length;
    const bool explicit_home = config_home != NULL && config_home[0] == '/';
    if (explicit_home) {
        length = snprintf(path, sizeof(path), "%s", config_home);
    } else if (home != NULL && home[0] == '/') {
        length = snprintf(path, sizeof(path), "%s/.config", home);
    } else {
        errno = EINVAL;
        return -1;
    }
    if (length < 0 || (size_t)length >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (create && !explicit_home) {
        /* Only create the conventional .config in the selected HOME. */
        const int home_fd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (home_fd < 0)
            return -1;
        if (mkdirat(home_fd, ".config", 0700) != 0 && errno != EEXIST) {
            const int error = errno;
            close(home_fd);
            errno = error;
            return -1;
        }
        close(home_fd);
    }
    const int base = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (base < 0)
        return -1;
    int fd;
    if (create) {
        fd = open_directory(base, "nt-native-linux", true);
    } else {
        fd = openat(base, "nt-native-linux", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) {
            struct stat info;
            if (fstat(fd, &info) != 0 || info.st_uid != geteuid() ||
                (info.st_mode & 0077) != 0) {
                close(fd);
                fd = -1;
                errno = EACCES;
            }
        }
    }
    const int error = errno;
    close(base);
    errno = error;
    return fd;
}

void nt_guest_share_free(char **paths, size_t count)
{
    if (paths == NULL)
        return;
    for (size_t i = 0; i < count; ++i) {
        free(paths[i]);
        paths[i] = NULL;
    }
}

static int share_folder_index(const char *name, const char *const *names, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        if (names[i] != NULL && strcmp(name, names[i]) == 0)
            return (int)i;
    return -1;
}

/* Read one complete configuration snapshot, or no mappings when absent. */
static int load_share_config(int dir, const char *file, const char *const *names, size_t count, char **paths)
{
    const int fd = openat(dir, file, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat info;
    if (fstat(fd, &info) != 0) {
        const int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0077) != 0 || info.st_size < 0 ||
        info.st_size > NT_SHARE_MAX_BYTES) {
        close(fd);
        errno = info.st_size > NT_SHARE_MAX_BYTES ? EFBIG : EACCES;
        return -1;
    }
    char data[NT_SHARE_MAX_BYTES + 1];
    size_t used = 0;
    for (;;) {
        const ssize_t bytes = read(fd, data + used, NT_SHARE_MAX_BYTES + 1 - used);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes < 0) {
            const int error = errno;
            close(fd);
            errno = error;
            return -1;
        }
        if (bytes == 0)
            break;
        used += (size_t)bytes;
        if (used > NT_SHARE_MAX_BYTES) {
            close(fd);
            errno = EFBIG;
            return -1;
        }
    }
    close(fd);
    if (memchr(data, '\0', used) != NULL) {
        errno = EINVAL;
        return -1;
    }
    data[used] = '\0';
    char *line = data;
    while (line < data + used) {
        char *end = strchr(line, '\n');
        if (end == NULL)
            end = data + used;
        else
            *end = '\0';
        if (*line != '\0') {
            char *separator = strchr(line, '=');
            if (separator == NULL) {
                errno = EINVAL;
                goto invalid;
            }
            *separator++ = '\0';
            const int index = share_folder_index(line, names, count);
            if (index < 0 || paths[index] != NULL ||
                separator[0] != '/' || strlen(separator) >= PATH_MAX) {
                errno = EINVAL;
                goto invalid;
            }
            for (const unsigned char *p = (const unsigned char *)separator; *p; ++p) {
                if (*p < 32 || *p == 127) {
                    errno = EINVAL;
                    goto invalid;
                }
            }
            paths[index] = strdup(separator);
            if (paths[index] == NULL)
                goto invalid;
        }
        line = end == data + used ? end : end + 1;
    }
    return 0;

invalid:
    {
        const int error = errno;
        nt_guest_share_free(paths, count);
        errno = error;
        return -1;
    }
}

static int grant_load(const char *config_home, const char *home,
                      const char *const *names, size_t count, char **paths,
                      const char *filename)
{
    if (names == NULL || paths == NULL || count == 0 || count > 64) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < count; ++i)
        paths[i] = NULL;
    const int dir = share_config_directory(config_home, home, false);
    if (dir < 0)
        return errno == ENOENT ? 0 : -1;
    const int result = load_share_config(dir, filename, names, count, paths);
    const int error = errno;
    close(dir);
    errno = error;
    return result;
}

static bool share_write_all(int fd, const char *data, size_t length)
{
    while (length != 0) {
        const ssize_t written = write(fd, data, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            if (written == 0)
                errno = EIO;
            return false;
        }
        data += written;
        length -= (size_t)written;
    }
    return true;
}

/* Atomic replace, serialized against other writers via the private lock. */
static int grant_update(const char *config_home, const char *home,
                        const char *const *names, size_t count,
                        const char *folder, const char *directory,
                        const char *filename, const char *lockname,
                        const char *temp_prefix)
{
    if (names == NULL || folder == NULL || count == 0 || count > 64) {
        errno = EINVAL;
        return -1;
    }
    const int index = share_folder_index(folder, names, count);
    if (index < 0) {
        errno = EINVAL;
        return -1;
    }
    char canonical[PATH_MAX];
    if (directory != NULL) {
        const int shared = nt_open_shared_folder(directory, home);
        if (shared < 0)
            return -1;
        /* Freeze symbolic aliases at grant time: a later change to a
         * symlink named in --remember-share must not silently redirect
         * an already approved Documents/Downloads mapping. */
        if (realpath(directory, canonical) == NULL) {
            const int error = errno;
            close(shared);
            errno = error;
            return -1;
        }
        const int verified = nt_open_shared_folder(canonical, home);
        struct stat original, now;
        const bool unchanged = verified >= 0 &&
                               fstat(shared, &original) == 0 &&
                               fstat(verified, &now) == 0 &&
                               original.st_dev == now.st_dev &&
                               original.st_ino == now.st_ino;
        const int error = verified < 0 ? errno : EAGAIN;
        close(shared);
        if (verified >= 0)
            close(verified);
        if (!unchanged) {
            errno = error;
            return -1;
        }
        if (strchr(canonical, '\n') || strchr(canonical, '\r')) {
            errno = EINVAL;
            return -1;
        }
        directory = canonical;
    }
    const int dir = share_config_directory(config_home, home, true);
    if (dir < 0)
        return -1;
    const int lock = openat(dir, lockname, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    if (lock < 0 || fstat(lock, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
        flock(lock, LOCK_EX) != 0) {
        const int error = lock < 0 ? errno : EACCES;
        if (lock >= 0)
            close(lock);
        close(dir);
        errno = error;
        return -1;
    }
    char *paths[64] = {0};
    int result = load_share_config(dir, filename, names, count, paths);
    if (result == 0) {
        free(paths[index]);
        paths[index] = directory != NULL ? strdup(directory) : NULL;
        if (directory != NULL && paths[index] == NULL)
            result = -1;
    }
    char temp[64] = {0};
    int fd = -1;
    if (result == 0) {
        for (unsigned int attempt = 0; attempt < 64; ++attempt) {
            snprintf(temp, sizeof(temp), "%s%ld.%u", temp_prefix, (long)getpid(), attempt);
            fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0 || errno != EEXIST)
                break;
        }
        if (fd < 0)
            result = -1;
    }
    if (result == 0) {
        for (size_t i = 0; i < count; ++i) {
            if (paths[i] == NULL)
                continue;
            if (!share_write_all(fd, names[i], strlen(names[i])) ||
                !share_write_all(fd, "=", 1) ||
                !share_write_all(fd, paths[i], strlen(paths[i])) ||
                !share_write_all(fd, "\n", 1)) {
                result = -1;
                break;
            }
        }
    }
    if (fd >= 0) {
        if (result == 0 && fsync(fd) != 0)
            result = -1;
        if (close(fd) != 0 && result == 0)
            result = -1;
        if (result == 0 && renameat(dir, temp, dir, filename) != 0)
            result = -1;
        if (result == 0 && fsync(dir) != 0)
            result = -1;
        if (result != 0)
            (void)unlinkat(dir, temp, 0);
    }
    const int error = errno;
    nt_guest_share_free(paths, count);
    (void)flock(lock, LOCK_UN);
    close(lock);
    close(dir);
    errno = error;
    return result;
}

/* Keep user-folder grants separate from optional DOS-drive mappings.
 * A previously installed launcher can continue reading shares.conf. */
int nt_guest_share_load(const char *config_home, const char *home,
                        const char *const *names, size_t count, char **paths)
{
    return grant_load(config_home, home, names, count, paths, NT_SHARE_FILE);
}

int nt_guest_share_update(const char *config_home, const char *home,
                          const char *const *names, size_t count,
                          const char *folder, const char *directory)
{
    return grant_update(config_home, home, names, count, folder, directory,
                        NT_SHARE_FILE, NT_SHARE_LOCK, ".shares.tmp.");
}

static void extra_drive_names(char labels[NT_GUEST_EXTRA_DRIVES][3],
                              const char *names[NT_GUEST_EXTRA_DRIVES])
{
    for (size_t i = 0; i < NT_GUEST_EXTRA_DRIVES; ++i) {
        labels[i][0] = (char)('D' + i);
        labels[i][1] = ':';
        labels[i][2] = '\0';
        names[i] = labels[i];
    }
}

int nt_guest_drive_load(const char *config_home, const char *home,
                        char **paths)
{
    char labels[NT_GUEST_EXTRA_DRIVES][3];
    const char *names[NT_GUEST_EXTRA_DRIVES];
    extra_drive_names(labels, names);
    return grant_load(config_home, home, names, NT_GUEST_EXTRA_DRIVES,
                      paths, NT_DRIVE_FILE);
}

int nt_guest_drive_update(const char *config_home, const char *home,
                          char letter, const char *directory)
{
    if (letter >= 'a' && letter <= 'z')
        letter = (char)(letter - 'a' + 'A');
    if (letter < 'D' || letter > 'Z') {
        errno = EINVAL;
        return -1;
    }
    char labels[NT_GUEST_EXTRA_DRIVES][3];
    const char *names[NT_GUEST_EXTRA_DRIVES];
    extra_drive_names(labels, names);
    return grant_update(config_home, home, names, NT_GUEST_EXTRA_DRIVES,
                        names[letter - 'D'], directory,
                        NT_DRIVE_FILE, NT_DRIVE_LOCK, ".drives.tmp.");
}


/* Each UNC grant is an independently authorized ordinary Linux directory.
 * This configuration is not an SMB discovery protocol and never grants
 * access beyond the actual user/mount/SELinux permissions. */
#define NT_UNC_FILE "unc.conf"
#define NT_UNC_LOCK ".unc.lock"

static bool unc_label_valid(const char *name)
{
    if (!name || !*name || name[0] == '.' || strlen(name) >= 64)
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-' || *p == '.'))
            return false;
    return true;
}

void nt_guest_unc_free(struct nt_guest_unc_grant grants[NT_GUEST_UNC_LIMIT], size_t count)
{
    if (!grants)
        return;
    if (count > NT_GUEST_UNC_LIMIT)
        count = NT_GUEST_UNC_LIMIT;
    for (size_t i = 0; i < count; ++i) {
        free(grants[i].directory);
        grants[i].directory = NULL;
    }
}

/* Parse exactly one validated snapshot. Duplicate server/share keys are
 * rejected rather than choosing an arbitrary mapping. */
static int load_unc_config(int dir,
                           struct nt_guest_unc_grant grants[NT_GUEST_UNC_LIMIT],
                           size_t *count)
{
    *count = 0;
    const int fd = openat(dir, NT_UNC_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
        info.st_size < 0 || info.st_size > NT_SHARE_MAX_BYTES) {
        const int error = info.st_size > NT_SHARE_MAX_BYTES ? EFBIG : EACCES;
        close(fd);
        errno = error;
        return -1;
    }
    char data[NT_SHARE_MAX_BYTES + 1];
    size_t used = 0;
    for (;;) {
        const ssize_t got = read(fd, data + used, sizeof(data) - used);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0 || used + (size_t)got > NT_SHARE_MAX_BYTES) {
            const int error = got < 0 ? errno : EFBIG;
            close(fd);
            errno = error;
            return -1;
        }
        if (got == 0)
            break;
        used += (size_t)got;
    }
    close(fd);
    if (memchr(data, '\0', used) != NULL) {
        errno = EINVAL;
        return -1;
    }
    data[used] = '\0';
    char *cursor = data;
    while (*cursor) {
        char *newline = strchr(cursor, '\n');
        if (newline)
            *newline = '\0';
        if (*cursor) {
            char *equals = strchr(cursor, '=');
            char *slash = strchr(cursor, '/');
            const size_t server_length = slash ? (size_t)(slash - cursor) : 0;
            const size_t share_length = slash && equals && slash < equals ?
                                         (size_t)(equals - slash - 1) : 0;
            if (!equals || !slash || slash > equals ||
                server_length == 0 || server_length >= 64 ||
                share_length == 0 || share_length >= 64 ||
                * (equals + 1) != '/' || strlen(equals + 1) >= PATH_MAX ||
                *count == NT_GUEST_UNC_LIMIT) {
                errno = EINVAL;
                goto invalid;
            }
            struct nt_guest_unc_grant *grant = &grants[*count];
            memcpy(grant->server, cursor, server_length);
            grant->server[server_length] = '\0';
            memcpy(grant->share, slash + 1, share_length);
            grant->share[share_length] = '\0';
            if (!unc_label_valid(grant->server) || !unc_label_valid(grant->share)) {
                errno = EINVAL;
                goto invalid;
            }
            for (const unsigned char *p = (const unsigned char *)(equals + 1); *p; ++p)
                if (*p < 32 || *p == 127) {
                    errno = EINVAL;
                    goto invalid;
                }
            for (size_t i = 0; i < *count; ++i)
                if (strcasecmp(grant->server, grants[i].server) == 0 &&
                    strcasecmp(grant->share, grants[i].share) == 0) {
                    errno = EINVAL;
                    goto invalid;
                }
            grant->directory = strdup(equals + 1);
            if (!grant->directory)
                goto invalid;
            ++*count;
        }
        if (!newline)
            break;
        cursor = newline + 1;
    }
    return 0;

invalid:
    {
        const int error = errno;
        nt_guest_unc_free(grants, *count + (*count < NT_GUEST_UNC_LIMIT));
        *count = 0;
        errno = error;
        return -1;
    }
}

int nt_guest_unc_load(const char *config_home, const char *home,
                      struct nt_guest_unc_grant grants[NT_GUEST_UNC_LIMIT],
                      size_t *count)
{
    if (!grants || !count) {
        errno = EINVAL;
        return -1;
    }
    memset(grants, 0, sizeof(*grants) * NT_GUEST_UNC_LIMIT);
    *count = 0;
    const int dir = share_config_directory(config_home, home, false);
    if (dir < 0)
        return errno == ENOENT ? 0 : -1;
    const int result = load_unc_config(dir, grants, count);
    const int error = errno;
    close(dir);
    errno = error;
    return result;
}

int nt_guest_unc_update(const char *config_home, const char *home,
                        const char *server, const char *share, const char *directory)
{
    if (!unc_label_valid(server) || !unc_label_valid(share)) {
        errno = EINVAL;
        return -1;
    }
    char canonical[PATH_MAX];
    if (directory != NULL) {
        int old = nt_open_shared_folder(directory, home);
        if (old < 0)
            return -1;
        if (realpath(directory, canonical) == NULL) {
            const int error = errno;
            close(old);
            errno = error;
            return -1;
        }
        int fresh = nt_open_shared_folder(canonical, home);
        struct stat before, after;
        const bool same = fresh >= 0 && fstat(old, &before) == 0 &&
                          fstat(fresh, &after) == 0 &&
                          before.st_dev == after.st_dev && before.st_ino == after.st_ino;
        close(old);
        if (fresh >= 0)
            close(fresh);
        if (!same) {
            errno = fresh < 0 ? EACCES : EAGAIN;
            return -1;
        }
        if (strchr(canonical, '\n') || strchr(canonical, '\r')) {
            errno = EINVAL;
            return -1;
        }
        directory = canonical;
    }
    const int dir = share_config_directory(config_home, home, directory != NULL);
    if (dir < 0)
        return directory == NULL && errno == ENOENT ? 0 : -1;
    const int lock = openat(dir, NT_UNC_LOCK,
                            O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    struct stat info;
    if (lock < 0 || fstat(lock, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != geteuid() || (info.st_mode & 0077) != 0 ||
        flock(lock, LOCK_EX) != 0) {
        const int error = lock < 0 ? errno : EACCES;
        if (lock >= 0)
            close(lock);
        close(dir);
        errno = error;
        return -1;
    }
    struct nt_guest_unc_grant grants[NT_GUEST_UNC_LIMIT] = {0};
    size_t count = 0;
    int status = load_unc_config(dir, grants, &count);
    size_t index = count;
    if (status == 0) {
        for (size_t i = 0; i < count; ++i)
            if (strcasecmp(grants[i].server, server) == 0 &&
                strcasecmp(grants[i].share, share) == 0) {
                index = i;
                break;
            }
        if (directory != NULL && index == count && count == NT_GUEST_UNC_LIMIT) {
            errno = ENOSPC;
            status = -1;
        } else if (directory != NULL) {
            if (index == count) {
                ++count;
                snprintf(grants[index].server, sizeof(grants[index].server), "%s", server);
                snprintf(grants[index].share, sizeof(grants[index].share), "%s", share);
            }
            char *copy = strdup(directory);
            if (!copy) {
                status = -1;
            } else {
                free(grants[index].directory);
                grants[index].directory = copy;
            }
        } else if (index < count) {
            free(grants[index].directory);
            memmove(&grants[index], &grants[index + 1],
                    (count - index - 1) * sizeof(*grants));
            --count;
            memset(&grants[count], 0, sizeof(*grants));
        }
    }
    char temp[64] = {0};
    int fd = -1;
    if (status == 0) {
        for (unsigned int attempt = 0; attempt < 64; ++attempt) {
            snprintf(temp, sizeof(temp), ".unc.tmp.%ld.%u", (long)getpid(), attempt);
            fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0 || errno != EEXIST)
                break;
        }
        if (fd < 0)
            status = -1;
    }
    for (size_t i = 0; status == 0 && i < count; ++i) {
        if (!share_write_all(fd, grants[i].server, strlen(grants[i].server)) ||
            !share_write_all(fd, "/", 1) ||
            !share_write_all(fd, grants[i].share, strlen(grants[i].share)) ||
            !share_write_all(fd, "=", 1) ||
            !share_write_all(fd, grants[i].directory, strlen(grants[i].directory)) ||
            !share_write_all(fd, "\n", 1))
            status = -1;
    }
    if (fd >= 0) {
        if (status == 0 && fsync(fd) != 0)
            status = -1;
        if (close(fd) != 0 && status == 0)
            status = -1;
        if (status == 0 && renameat(dir, temp, dir, NT_UNC_FILE) != 0)
            status = -1;
        if (status == 0 && fsync(dir) != 0)
            status = -1;
        if (status != 0)
            (void)unlinkat(dir, temp, 0);
    }
    const int error = errno;
    nt_guest_unc_free(grants, count);
    (void)flock(lock, LOCK_UN);
    close(lock);
    close(dir);
    errno = error;
    return status;
}
