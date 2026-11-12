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
