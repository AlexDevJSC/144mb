/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 700
#include "guest_profile.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static bool prepare_user_directories(int drive)
{
    const char *const path[] = {"Users", "guest", "AppData"};
    int parent = dup(drive);
    if (parent < 0)
        return false;
    for (size_t i = 0; i < sizeof(path) / sizeof(path[0]); ++i) {
        const int next = open_directory(parent, path[i], true);
        const int error = errno;
        close(parent);
        if (next < 0) {
            errno = error;
            return false;
        }
        parent = next;
    }
    const int roaming = open_directory(parent, "Roaming", true);
    const int local = roaming >= 0 ? open_directory(parent, "Local", true) : -1;
    const int error = errno;
    if (roaming >= 0)
        close(roaming);
    if (local >= 0)
        close(local);
    close(parent);
    if (local < 0) {
        errno = error;
        return false;
    }
    const int temp = open_directory(drive, "Temp", true);
    if (temp < 0)
        return false;
    close(temp);
    return true;
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
    const int drive = open_directory(runtime, "drive-c", true);
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
static int load_share_config(int dir, const char *const *names, size_t count, char **paths)
{
    const int fd = openat(dir, NT_SHARE_FILE, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
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

int nt_guest_share_load(const char *config_home, const char *home,
                        const char *const *names, size_t count, char **paths)
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
    const int result = load_share_config(dir, names, count, paths);
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
int nt_guest_share_update(const char *config_home, const char *home,
                          const char *const *names, size_t count,
                          const char *folder, const char *directory)
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
    if (directory != NULL) {
        const int shared = nt_open_shared_folder(directory, home);
        if (shared < 0)
            return -1;
        close(shared);
        if (strchr(directory, '\n') || strchr(directory, '\r') ||
            strlen(directory) >= PATH_MAX) {
            errno = EINVAL;
            return -1;
        }
    }
    const int dir = share_config_directory(config_home, home, true);
    if (dir < 0)
        return -1;
    const int lock = openat(dir, NT_SHARE_LOCK, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
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
    int result = load_share_config(dir, names, count, paths);
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
            snprintf(temp, sizeof(temp), ".shares.tmp.%ld.%u", (long)getpid(), attempt);
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
        if (result == 0 && renameat(dir, temp, dir, NT_SHARE_FILE) != 0)
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
