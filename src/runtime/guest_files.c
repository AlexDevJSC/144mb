/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "console.h"
#include "nt_services.h"
#include "winbase.h"
#include "winfile.h"
#include "sync.h"
#include "user_folders.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <termios.h>
#include <unistd.h>

static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;
static int drive_root = -1;
static char guest_cwd[PATH_MAX];
static struct { int fd; bool active; } folder_roots[NT_USER_FOLDER_COUNT];

bool nt_runtime_set_file_root(const char *directory)
{
    const int next = directory != NULL ? open(directory, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory != NULL && next < 0)
        return false;
    pthread_mutex_lock(&root_lock);
    const int previous = drive_root;
    drive_root = next;
    guest_cwd[0] = '\0';
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
        if (folder_roots[i].active) {
            close(folder_roots[i].fd);
            folder_roots[i].active = false;
        }
    }
    pthread_mutex_unlock(&root_lock);
    if (previous >= 0)
        close(previous);
    return true;
}

bool nt_runtime_set_user_folder_root(size_t folder, const char *directory)
{
    if (folder >= NT_USER_FOLDER_COUNT) {
        errno = EINVAL;
        return false;
    }
    const int fd = directory != NULL ? open(directory, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory != NULL && fd < 0)
        return false;
    pthread_mutex_lock(&root_lock);
    if (folder_roots[folder].active)
        close(folder_roots[folder].fd);
    folder_roots[folder].active = fd >= 0;
    folder_roots[folder].fd = fd;
    pthread_mutex_unlock(&root_lock);
    return true;
}

unsigned int nt_runtime_shared_user_folders(void)
{
    unsigned int folders = 0;
    pthread_mutex_lock(&root_lock);
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
        if (folder_roots[i].active)
            folders |= 1u << i;
    }
    pthread_mutex_unlock(&root_lock);
    return folders;
}

/* The caller holds root_lock while resolving paths and duplicating the root. */
static int path_root(const char **path)
{
    for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
        if (!folder_roots[i].active)
            continue;
        const char *prefix = nt_user_folders[i].relative;
        const size_t length = strlen(prefix);
        if (strncasecmp(*path, prefix, length) == 0 && ((*path)[length] == '/' || (*path)[length] == '\0')) {
            *path = (*path)[length] == '/' ? *path + length + 1 : ".";
            return folder_roots[i].fd;
        }
    }
    return drive_root;
}

static int duplicate_root(const char **path)
{
    pthread_mutex_lock(&root_lock);
    const int root = path_root(path);
    int result = -1;
    if (root >= 0)
        result = fcntl(root, F_DUPFD_CLOEXEC, 0);
    else
        errno = ENOENT;
    const int error = errno;
    pthread_mutex_unlock(&root_lock);
    errno = error;
    return result;
}

static bool separator(char byte)
{
    return byte == '\\' || byte == '/';
}

static bool device_name(const char *name, size_t size)
{
    char stem[5];
    size_t length = 0;
    while (length < size && name[length] != '.' && length < sizeof(stem) - 1) {
        const char byte = name[length];
        stem[length++] = byte >= 'a' && byte <= 'z' ? (char)(byte - ('a' - 'A')) : byte;
    }
    if (length < size && name[length] != '.')
        return false;
    stem[length] = '\0';
    return strcmp(stem, "CON") == 0 || strcmp(stem, "PRN") == 0 ||
           strcmp(stem, "AUX") == 0 || strcmp(stem, "NUL") == 0 ||
           (length == 4 && stem[3] >= '1' && stem[3] <= '9' &&
            (memcmp(stem, "COM", 3) == 0 || memcmp(stem, "LPT", 3) == 0));
}

static bool normalize_path(const char *input, char output[PATH_MAX], bool allow_root)
{
    if (input == NULL || *input == '\0') {
        errno = EINVAL;
        return false;
    }
    if (strnlen(input, PATH_MAX) == PATH_MAX) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (nt_MultiByteToWideChar(65001, 8, input, -1, NULL, 0) == 0) {
        errno = EINVAL;
        return false;
    }
    const bool absolute = input[1] == ':' || separator(input[0]);
    if (input[1] == ':') {
        if ((*input != 'C' && *input != 'c') || !separator(input[2])) {
            errno = EINVAL;
            return false;
        }
        input += 2;
    } else if (separator(input[0]) && separator(input[1])) {
        errno = EACCES;
        return false;
    }

    size_t used = 0;
    if (!absolute) {
        pthread_mutex_lock(&root_lock);
        used = strlen(guest_cwd);
        memcpy(output, guest_cwd, used);
        pthread_mutex_unlock(&root_lock);
    }
    while (*input != '\0') {
        while (separator(*input))
            ++input;
        const char *component = input;
        while (*input != '\0' && !separator(*input)) {
            const unsigned char byte = (unsigned char)*input;
            if (byte < 0x20 || byte == 0x7f || strchr("<>:\"|?*", byte) != NULL) {
                errno = EINVAL;
                return false;
            }
            ++input;
        }
        const size_t length = (size_t)(input - component);
        if (length == 0 || (length == 1 && component[0] == '.'))
            continue;
        if (length == 2 && memcmp(component, "..", 2) == 0) {
            /* A shared known-folder root is a separate security grant.
             * Do not turn Documents/../secret into an unshared C: path. */
            bool at_share_boundary = false;
            pthread_mutex_lock(&root_lock);
            for (size_t i = 0; i < NT_USER_FOLDER_COUNT; ++i) {
                const char *prefix = nt_user_folders[i].relative;
                if (folder_roots[i].active && strlen(prefix) == used &&
                    strncasecmp(output, prefix, used) == 0) {
                    at_share_boundary = true;
                    break;
                }
            }
            pthread_mutex_unlock(&root_lock);
            if (used == 0 || at_share_boundary) {
                errno = EACCES;
                return false;
            }
            while (used != 0 && output[used - 1] != '/')
                --used;
            if (used != 0)
                --used;
            continue;
        }
        if (component[length - 1] == '.' || component[length - 1] == ' ' ||
            device_name(component, length)) {
            errno = EINVAL;
            return false;
        }
        if (length + (used != 0) >= PATH_MAX - used) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (used != 0)
            output[used++] = '/';
        memcpy(output + used, component, length);
        used += length;
    }
    if (used == 0 && !allow_root) {
        errno = EINVAL;
        return false;
    }
    output[used] = '\0';
    return true;
}

static bool guest_path(const char *input, char output[PATH_MAX])
{
    return normalize_path(input, output, false);
}

/* Windows ASCII case folding on ordinary Linux files, resolved by FD.
 * Keep non-ASCII UTF-8 bytes exact until Unicode folding is implemented. */
static unsigned char lower_ascii(unsigned char byte)
{
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte + 'a' - 'A') : byte;
}

static bool same_ascii_name(const char *left, const char *right)
{
    while (*left && *right) {
        if (lower_ascii((unsigned char)*left++) != lower_ascii((unsigned char)*right++))
            return false;
    }
    return *left == '\0' && *right == '\0';
}

static int confined_open_raw(int root, const char *path, int flags, unsigned int mode)
{
    const struct open_how how = {
        .flags = (uint64_t)(flags | O_CLOEXEC),
        .mode = mode,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV
    };
    return (int)syscall(SYS_openat2, root, path, &how, sizeof(how));
}

/* Exact spelling wins, including a symlink (the actual open rejects it).
 * Two differently-cased alternatives without an exact match are ambiguous. */
static int resolve_name(int parent, const char *asked, char actual[NAME_MAX + 1])
{
    const size_t length = strlen(asked);
    if (!length || length > NAME_MAX) {
        errno = length ? ENAMETOOLONG : EINVAL;
        return -1;
    }
    struct stat stat_info;
    if (fstatat(parent, asked, &stat_info, AT_SYMLINK_NOFOLLOW) == 0) {
        memcpy(actual, asked, length + 1);
        return 1;
    }
    if (errno != ENOENT)
        return -1;
    const int scan_fd = confined_open_raw(parent, ".", O_RDONLY | O_DIRECTORY, 0);
    if (scan_fd < 0)
        return -1;
    DIR *dir = fdopendir(scan_fd);
    if (!dir) {
        int error = errno;
        close(scan_fd);
        errno = error;
        return -1;
    }
    int matches = 0, error = 0;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(dir);
        if (!entry) {
            error = errno;
            break;
        }
        if (same_ascii_name(asked, entry->d_name)) {
            if (++matches > 1) {
                error = EEXIST;
                break;
            }
            strcpy(actual, entry->d_name);
        }
    }
    if (closedir(dir) != 0 && !error)
        error = errno;
    if (error) {
        errno = error;
        return -1;
    }
    if (!matches)
        memcpy(actual, asked, length + 1);
    return matches;
}

/* Walk under verified directory FDs; intermediates must exist and cannot
 * escape the allowed root through symlinks, mounts or textual traversal. */
static int open_parent(int root, const char *path, char leaf[NAME_MAX + 1])
{
    if (!path || !*path || strcmp(path, ".") == 0) {
        errno = EINVAL;
        return -1;
    }
    int parent = fcntl(root, F_DUPFD_CLOEXEC, 0);
    if (parent < 0)
        return -1;
    const char *cursor = path;
    for (;;) {
        const char *slash = strchr(cursor, '/');
        const size_t length = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (!length || length > NAME_MAX) {
            const int error = length ? ENAMETOOLONG : EINVAL;
            close(parent);
            errno = error;
            return -1;
        }
        char asked[NAME_MAX + 1], actual[NAME_MAX + 1];
        memcpy(asked, cursor, length);
        asked[length] = '\0';
        const int found = resolve_name(parent, asked, actual);
        if (found < 0) {
            const int error = errno;
            close(parent);
            errno = error;
            return -1;
        }
        if (!slash) {
            strcpy(leaf, actual);
            return parent;
        }
        if (!found) {
            close(parent);
            errno = ENOENT;
            return -1;
        }
        const int child = confined_open_raw(parent, actual, O_PATH | O_DIRECTORY, 0);
        const int error = errno;
        close(parent);
        if (child < 0) {
            errno = error;
            return -1;
        }
        parent = child;
        cursor = slash + 1;
    }
}

static int confined_open(int root, const char *path, int flags, unsigned int mode)
{
    if (strcmp(path, ".") == 0)
        return confined_open_raw(root, ".", flags, mode);
    char leaf[NAME_MAX + 1];
    const int parent = open_parent(root, path, leaf);
    if (parent < 0)
        return -1;
    const int fd = confined_open_raw(parent, leaf, flags, mode);
    const int error = errno;
    close(parent);
    errno = error;
    return fd;
}

static int open_regular(int root, const char *path, int flags, bool *created)
{
    *created = false;
    if ((flags & O_EXCL) != 0) {
        const int fd = confined_open(root, path, flags, 0666);
        *created = fd >= 0;
        return fd;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int pinned = confined_open(root, path, O_PATH, 0);
        if (pinned >= 0) {
            struct stat info;
            int fd = -1;
            if (fstat(pinned, &info) == 0) {
                if (S_ISREG(info.st_mode)) {
                    /* Reopen the checked inode, even if its pathname changed. */
                    char fd_path[64];
                    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", pinned);
                    fd = open(fd_path, (flags & ~(O_CREAT | O_TRUNC)) | O_CLOEXEC);
                } else {
                    errno = EACCES;
                }
            }
            const int error = errno;
            close(pinned);
            errno = error;
            return fd;
        }
        if (errno != ENOENT || (flags & O_CREAT) == 0)
            return -1;

        const int fd = confined_open(root, path, flags | O_EXCL, 0666);
        *created = fd >= 0;
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

int nt_runtime_open_guest_file_readonly(const char *input)
{
    char path[PATH_MAX];
    if (!guest_path(input, path))
        return -1;
    const char *relative = path;
    const int root = duplicate_root(&relative);
    if (root < 0)
        return -1;
    bool created;
    const int fd = open_regular(root, relative, O_RDONLY, &created);
    const int error = errno;
    close(root);
    errno = error;
    return fd;
}

int nt_runtime_read_guest_file(const char *path, size_t limit, void **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    const int fd = nt_runtime_open_guest_file_readonly(path);
    if (fd < 0)
        return errno;
    struct stat info;
    int error = fstat(fd, &info) != 0 ? errno : 0;
    if (error == 0 && (info.st_size < 0 || (uintmax_t)info.st_size > limit))
        error = EFBIG;
    if (error) {
        close(fd);
        return error;
    }
    size_t length = (size_t)info.st_size;
    unsigned char *bytes = malloc(length ? length : 1);
    if (!bytes) {
        close(fd);
        return ENOMEM;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t got = read(fd, bytes + done, length - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            error = got == 0 ? EIO : errno;
            break;
        }
        done += (size_t)got;
    }
    close(fd);
    if (error) {
        free(bytes);
        return error;
    }
    *data = bytes;
    *size = length;
    return 0;
}

static bool copy_dos_path(const char *relative, char *output, size_t capacity)
{
    const size_t length = strlen(relative);
    if (capacity < 4 || length > capacity - 4) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(output, "C:\\", 3);
    for (size_t i = 0; i <= length; ++i)
        output[3 + i] = relative[i] == '/' ? '\\' : relative[i];
    return true;
}

int nt_runtime_open_guest_directory(const char *path, char *output, size_t capacity)
{
    char relative[PATH_MAX];
    if (!normalize_path(path != NULL ? path : "C:\\", relative, true))
        return -1;
    const char *resolved = relative[0] != '\0' ? relative : ".";
    const int root = duplicate_root(&resolved);
    if (root < 0)
        return -1;
    int directory = confined_open(root, resolved, O_RDONLY | O_DIRECTORY, 0);
    int error = errno;
    if (directory >= 0 && !copy_dos_path(relative, output, capacity)) {
        error = errno;
        close(directory);
        directory = -1;
    }
    close(root);
    errno = error;
    return directory;
}

bool nt_runtime_guest_directory_entry(int directory, const char *name, bool *is_directory)
{
    char relative[PATH_MAX], rooted[PATH_MAX];
    if (snprintf(rooted, sizeof(rooted), "/%s", name) >= (int)sizeof(rooted) ||
        !guest_path(rooted, relative) || strcmp(name, relative) != 0 || strchr(relative, '/') != NULL)
        return false;
    const int fd = confined_open(directory, relative, O_PATH, 0);
    if (fd < 0)
        return false;
    struct stat info;
    const bool valid = fstat(fd, &info) == 0 && (S_ISDIR(info.st_mode) || S_ISREG(info.st_mode));
    if (valid)
        *is_directory = S_ISDIR(info.st_mode);
    close(fd);
    return valid;
}

bool nt_runtime_check_guest_file(const char *path, char *output, size_t capacity, bool *exists)
{
    char relative[PATH_MAX];
    if (!guest_path(path, relative))
        return false;
    const char *resolved = relative;
    const int root = duplicate_root(&resolved);
    if (root < 0)
        return false;
    int fd = confined_open(root, resolved, O_PATH, 0);
    bool valid = false, found = false;
    if (fd >= 0) {
        struct stat info;
        valid = fstat(fd, &info) == 0 && S_ISREG(info.st_mode);
        found = valid;
        close(fd);
    } else if (errno == ENOENT) {
        char leaf[NAME_MAX + 1];
        fd = open_parent(root, resolved, leaf);
        if (fd >= 0) {
            valid = true;
            close(fd);
        }
    }
    close(root);
    if (!valid || !copy_dos_path(relative, output, capacity))
        return false;
    *exists = found;
    return true;
}

static uint32_t path_error(int error, bool parent)
{
    switch (error) {
    case ENOENT:
        return parent ? NT_ERROR_PATH_NOT_FOUND : NT_ERROR_FILE_NOT_FOUND;
    case ENOTDIR:
        return NT_ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case ELOOP: case EXDEV: case EROFS: case EISDIR:
        return NT_ERROR_ACCESS_DENIED;
    case EEXIST:
        return NT_ERROR_ALREADY_EXISTS;
    case ENOTEMPTY:
        return NT_ERROR_DIR_NOT_EMPTY;
    case EINVAL: case ENAMETOOLONG:
        return NT_ERROR_INVALID_PARAMETER;
    case EBADF:
        return NT_ERROR_INVALID_HANDLE;
    case EPIPE:
        return NT_ERROR_BROKEN_PIPE;
    case ENOMEM:
        return NT_ERROR_NOT_ENOUGH_MEMORY;
    case EBUSY:
        return NT_ERROR_SHARING_VIOLATION;
    default:
        return NT_ERROR_GEN_FAILURE;
    }
}

enum {
    FILE_READ = 1, FILE_WRITE = 2, FILE_TABLE_SIZE = 256,
    HANDLE_FLAG_INHERIT = 1,
    CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4,
    TRUNCATE_EXISTING = 5, FILE_ATTRIBUTE_NORMAL = 0x80
};

enum file_owner { FREE_FILE, WIN_HANDLE, CRT_DESCRIPTOR, CRT_STREAM };

struct guest_file {
    enum file_owner owner;
    uintptr_t handle;
    int fd;
    unsigned int access, share;
    uint32_t handle_flags;
    dev_t device;
    ino_t inode;
    bool text;
    char *path;
};

static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_file files[FILE_TABLE_SIZE];
static uintptr_t next_handle = 0x24000004;
static uintptr_t standard_handles[3], initial_handles[3];

static bool same_file(const struct guest_file *file, const struct stat *info)
{
    return file->owner != FREE_FILE && file->device == info->st_dev &&
           file->inode == info->st_ino;
}

static bool file_in_use(const struct stat *info)
{
    for (size_t i = 0; i < FILE_TABLE_SIZE; ++i) {
        if (same_file(&files[i], info))
            return true;
    }
    return false;
}

static struct guest_file *new_file(void)
{
    for (size_t i = 0; i < FILE_TABLE_SIZE; ++i) {
        if (files[i].owner == FREE_FILE)
            return &files[i];
    }
    errno = ENOMEM;
    return NULL;
}

static struct guest_file *find_handle(uintptr_t handle)
{
    for (size_t i = 0; i < FILE_TABLE_SIZE; ++i) {
        if (files[i].owner == WIN_HANDLE && files[i].handle == handle)
            return &files[i];
    }
    return NULL;
}

static int open_shared(struct guest_file *slot, int parent, const char *path,
                       int flags, unsigned int access, unsigned int share, bool *created)
{
    const int fd = open_regular(parent, path, flags & ~O_TRUNC, created);
    if (fd < 0)
        return -1;
    struct stat info;
    int error = fstat(fd, &info) != 0 ? errno : 0;
    for (size_t i = 0; error == 0 && i < FILE_TABLE_SIZE; ++i) {
        const struct guest_file *other = &files[i];
        if (same_file(other, &info) && ((access & ~other->share) != 0 ||
                                      (other->access & ~share) != 0))
            error = EBUSY;
    }
    if (error != 0) {
        close(fd);
        errno = error;
        return -1;
    }
    *slot = (struct guest_file){.fd = fd, .access = access, .share = share,
                               .device = info.st_dev, .inode = info.st_ino};
    return fd;
}

struct security_attributes {
    uint32_t size;
    const void *descriptor;
    int32_t inherit;
};

_Static_assert(sizeof(struct security_attributes) == 24, "SECURITY_ATTRIBUTES layout");

uintptr_t NT_MS_ABI nt_CreateFileA(const char *input, uint32_t desired, uint32_t share,
                                  const void *security, uint32_t disposition, uint32_t flags,
                                  uintptr_t template_file)
{
    const uint32_t generic_read = UINT32_C(0x80000000), generic_write = UINT32_C(0x40000000);
    const struct security_attributes *attributes = security;
    const bool read_access = (desired & generic_read) != 0, write_access = (desired & generic_write) != 0;
    if (disposition < CREATE_NEW || disposition > TRUNCATE_EXISTING ||
        (attributes != NULL && attributes->size != sizeof(*attributes))) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return UINTPTR_MAX;
    }
    if ((desired & ~(generic_read | generic_write)) != 0 ||
        (desired == 0 && disposition != OPEN_EXISTING) ||
        (share & ~(FILE_READ | FILE_WRITE)) != 0 || (flags & ~FILE_ATTRIBUTE_NORMAL) != 0 ||
        template_file != 0 || (attributes != NULL && attributes->descriptor != NULL)) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return UINTPTR_MAX;
    }
    const bool truncate = disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING;
    if (truncate && !write_access) {
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return UINTPTR_MAX;
    }
    char path[PATH_MAX];
    if (!guest_path(input, path)) {
        nt_SetLastError(path_error(errno, true));
        return UINTPTR_MAX;
    }
    const char *resolved = path;
    const int root = duplicate_root(&resolved);
    if (root < 0) {
        nt_SetLastError(path_error(errno, true));
        return UINTPTR_MAX;
    }
    char leaf[NAME_MAX + 1];
    const int parent = open_parent(root, resolved, leaf);
    const int parent_error = errno;
    close(root);
    if (parent < 0) {
        nt_SetLastError(path_error(parent_error, true));
        return UINTPTR_MAX;
    }

    char *saved_path = strdup(path);
    if (saved_path == NULL) {
        close(parent);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return UINTPTR_MAX;
    }
    int options = write_access ? (read_access ? O_RDWR : O_WRONLY) : O_RDONLY;
    if (disposition == CREATE_NEW || disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
        options |= O_CREAT;
    if (disposition == CREATE_NEW)
        options |= O_EXCL;
    const unsigned int access = (read_access ? FILE_READ : 0) | (write_access ? FILE_WRITE : 0);
    pthread_mutex_lock(&file_lock);
    struct guest_file *slot = new_file();
    bool created = false;
    uintptr_t result = UINTPTR_MAX;
    int error = ENOMEM;
    if (slot != NULL && next_handle <= UINTPTR_MAX - 16) {
        const int fd = open_shared(slot, parent, leaf, options, access, share, &created);
        error = errno;
        if (fd >= 0) {
            if (truncate && !created && ftruncate(fd, 0) != 0) {
                error = errno;
                close(fd);
            } else {
                result = next_handle;
                next_handle += 16;
                slot->owner = WIN_HANDLE;
                slot->handle = result;
                slot->handle_flags = attributes && attributes->inherit ? HANDLE_FLAG_INHERIT : 0;
                slot->path = saved_path;
                saved_path = NULL;
            }
        }
    }
    pthread_mutex_unlock(&file_lock);
    close(parent);
    free(saved_path);
    if (result == UINTPTR_MAX)
        nt_SetLastError(error == EEXIST ? NT_ERROR_FILE_EXISTS : path_error(error, false));
    else if (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
        nt_SetLastError(created ? NT_ERROR_SUCCESS : NT_ERROR_ALREADY_EXISTS);
    return result;
}

static int close_file(struct guest_file *file)
{
    const int result = close(file->fd);
    free(file->path);
    file->path = NULL;
    file->owner = FREE_FILE;
    return result;
}

/* Duplicated descriptors share a Linux open-file description (and therefore
 * the seek offset), as a duplicated Windows file HANDLE does. They remain
 * independent entries in the guest handle table. Rights-preserving copies
 * are supported; reducing access rights needs a separate access-mask model. */
static nt_status duplicate_file_handle(uintptr_t source, uint32_t attributes,
                                       uint32_t options, uintptr_t *target)
{
    pthread_mutex_lock(&file_lock);
    const struct guest_file *original = find_handle(source);
    nt_status status = NT_STATUS_SUCCESS;
    if (!original) {
        status = NT_STATUS_INVALID_HANDLE;
    } else if (!(options & 2u)) { /* DUPLICATE_SAME_ACCESS */
        status = NT_STATUS_NOT_SUPPORTED;
    } else {
        struct guest_file *copy = new_file();
        if (!copy || next_handle == UINTPTR_MAX) {
            status = NT_STATUS_NO_MEMORY;
        } else {
            const int fd = fcntl(original->fd, F_DUPFD_CLOEXEC, 0);
            char *path = original->path ? strdup(original->path) : NULL;
            if (fd < 0 || (original->path && !path)) {
                if (fd >= 0)
                    close(fd);
                free(path);
                status = NT_STATUS_NO_MEMORY;
            } else {
                *copy = *original;
                copy->fd = fd;
                copy->path = path;
                copy->handle = next_handle++;
                copy->handle_flags = (options & 4u) ? original->handle_flags :
                                     (attributes & 2u) ? HANDLE_FLAG_INHERIT : 0;
                *target = copy->handle;
            }
        }
    }
    pthread_mutex_unlock(&file_lock);
    return status;
}

int32_t NT_MS_ABI nt_GetHandleInformation(uintptr_t handle, uint32_t *flags)
{
    if (!flags) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&file_lock);
    const struct guest_file *file = find_handle(handle);
    if (file)
        *flags = file->handle_flags;
    pthread_mutex_unlock(&file_lock);
    if (!file && nt_runtime_get_waitable_flags(handle, flags))
        return 1;
    if (!file)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return file != NULL;
}

static int close_object(uintptr_t handle)
{
    pthread_mutex_lock(&file_lock);
    struct guest_file *file = find_handle(handle);
    const int error = !file ? EBADF : close_file(file) == 0 ? 0 : errno;
    pthread_mutex_unlock(&file_lock);
    if (file == NULL && nt_runtime_close_waitable(handle))
        return 0;
    return error;
}

nt_status NT_MS_ABI nt_NtDuplicateObject(uintptr_t source_process, uintptr_t source,
                                        uintptr_t target_process, uintptr_t *target,
                                        uint32_t access, uint32_t attributes, uint32_t options)
{
    /* A real cross-process object-manager requires a shared namespace and
     * broker. Do not pass host pid/FD values as foreign NT process handles. */
    if (!target)
        return NT_STATUS_ACCESS_VIOLATION;
    *target = 0;
    if (source_process != UINTPTR_MAX || target_process != UINTPTR_MAX)
        return NT_STATUS_NOT_SUPPORTED;
    if ((options & ~7u) || (attributes & ~2u))
        return NT_STATUS_INVALID_PARAMETER;
    nt_status status = nt_runtime_duplicate_waitable(source, access, attributes, options, target);
    if (status == NT_STATUS_INVALID_HANDLE)
        status = duplicate_file_handle(source, attributes, options, target);
    if (status == NT_STATUS_SUCCESS && (options & 1u)) {
        /* After successful duplication each handle has an independent life. */
        (void)nt_NtClose(source);
    }
    return status;
}

int32_t NT_MS_ABI nt_DuplicateHandle(uintptr_t source_process, uintptr_t source,
                                     uintptr_t target_process, uintptr_t *target,
                                     uint32_t access, int32_t inherit, uint32_t options)
{
    const nt_status status = nt_NtDuplicateObject(source_process, source, target_process,
                                                   target, access, inherit ? 2u : 0, options);
    if (status != NT_STATUS_SUCCESS)
        nt_SetLastError(nt_runtime_nt_error(status));
    return status == NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtClose(uintptr_t handle)
{
    const int error = close_object(handle);
    switch (error) {
    case 0: return NT_STATUS_SUCCESS;
    case EBADF: return NT_STATUS_INVALID_HANDLE;
    case EACCES: case EPERM: return NT_STATUS_ACCESS_DENIED;
    default: return NT_STATUS_UNSUCCESSFUL;
    }
}

int32_t NT_MS_ABI nt_CloseHandle(uintptr_t handle)
{
    const int error = close_object(handle);
    if (error)
        nt_SetLastError(path_error(error, false));
    return !error;
}

enum path_operation { MAKE_DIRECTORY, REMOVE_FILE, REMOVE_DIRECTORY };

static int32_t change_path(const char *input, enum path_operation operation)
{
    char path[PATH_MAX];
    if (!guest_path(input, path)) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const char *resolved = path;
    const int root = duplicate_root(&resolved);
    if (root < 0) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    char leaf[NAME_MAX + 1];
    const int parent = open_parent(root, resolved, leaf);
    const int parent_error = errno;
    close(root);
    if (parent < 0) {
        nt_SetLastError(path_error(parent_error, true));
        return 0;
    }

    pthread_mutex_lock(&file_lock);
    int status;
    if (operation == MAKE_DIRECTORY) {
        status = mkdirat(parent, leaf, 0777);
    } else {
        struct stat info;
        status = fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW);
        if (status == 0 && (S_ISLNK(info.st_mode) || file_in_use(&info))) {
            errno = S_ISLNK(info.st_mode) ? EACCES : EBUSY;
            status = -1;
        }
        if (status == 0)
            status = unlinkat(parent, leaf, operation == REMOVE_DIRECTORY ? AT_REMOVEDIR : 0);
    }
    const int error = errno;
    pthread_mutex_unlock(&file_lock);
    close(parent);
    if (status != 0)
        nt_SetLastError(path_error(error, operation == REMOVE_DIRECTORY));
    return status == 0;
}

int32_t NT_MS_ABI nt_CreateDirectoryA(const char *path, const void *security)
{
    if (security != NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return change_path(path, MAKE_DIRECTORY);
}

int32_t NT_MS_ABI nt_DeleteFileA(const char *path)
{
    return change_path(path, REMOVE_FILE);
}

int32_t NT_MS_ABI nt_RemoveDirectoryA(const char *path)
{
    return change_path(path, REMOVE_DIRECTORY);
}

int32_t NT_MS_ABI nt_MoveFileA(const char *from, const char *to)
{
    return nt_MoveFileExA(from, to, 0);
}

int32_t NT_MS_ABI nt_MoveFileExA(const char *from, const char *to, uint32_t flags)
{
    char source[PATH_MAX], destination[PATH_MAX];
    /* MOVEFILE_REPLACE_EXISTING and MOVEFILE_COPY_ALLOWED are the
     * supported flags. Explicitly reject delayed and hard-link moves. */
    if ((flags & ~3u) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (!guest_path(from, source) || !guest_path(to, destination)) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const char *resolved_source = source, *resolved_destination = destination;
    pthread_mutex_lock(&root_lock);
    const int source_root = path_root(&resolved_source);
    const int destination_root = path_root(&resolved_destination);
    const int root = source_root >= 0 && source_root == destination_root ?
                     fcntl(source_root, F_DUPFD_CLOEXEC, 0) : -1;
    const int root_error = errno;
    pthread_mutex_unlock(&root_lock);
    if (root < 0) {
        if (source_root >= 0 && destination_root >= 0 && source_root != destination_root &&
            (flags & 2u) != 0) {
            /* Different authorized roots must never be joined using a
             * host path. A regular file is copied via guest handles, then
             * deleted only after the copy is complete. */
            if (!nt_CopyFileA(from, to, (flags & 1u) == 0))
                return 0;
            if (nt_DeleteFileA(from))
                return 1;
            const uint32_t saved = nt_GetLastError();
            /* Without replacement, the newly copied file is ours to
             * remove if the source could not be deleted. */
            if ((flags & 1u) == 0)
                (void)nt_DeleteFileA(to);
            nt_SetLastError(saved);
            return 0;
        }
        nt_SetLastError(source_root < 0 || destination_root < 0 ? NT_ERROR_PATH_NOT_FOUND :
                        source_root != destination_root ? NT_ERROR_NOT_SAME_DEVICE : path_error(root_error, true));
        return 0;
    }
    char source_leaf[NAME_MAX + 1], destination_leaf[NAME_MAX + 1];
    const int source_parent = open_parent(root, resolved_source, source_leaf);
    const int source_error = errno;
    const int destination_parent = open_parent(root, resolved_destination, destination_leaf);
    const int destination_error = errno;
    close(root);
    if (source_parent < 0 || destination_parent < 0) {
        if (source_parent >= 0)
            close(source_parent);
        if (destination_parent >= 0)
            close(destination_parent);
        nt_SetLastError(path_error(source_parent < 0 ? source_error : destination_error, true));
        return 0;
    }

    /* A case-only rename must use the requested casing, not the resolved
     * Linux name, or the rename would become a no-op. */
    const char *rename_leaf = destination_leaf;
    const char *wanted = strrchr(resolved_destination, '/');
    wanted = wanted ? wanted + 1 : resolved_destination;
    struct stat src_dir, dst_dir;
    if (strcmp(source_leaf, wanted) != 0 && same_ascii_name(source_leaf, wanted) &&
        fstat(source_parent, &src_dir) == 0 && fstat(destination_parent, &dst_dir) == 0 &&
        src_dir.st_dev == dst_dir.st_dev && src_dir.st_ino == dst_dir.st_ino)
        rename_leaf = wanted;

    pthread_mutex_lock(&file_lock);
    struct stat info;
    int status = fstatat(source_parent, source_leaf, &info, AT_SYMLINK_NOFOLLOW);
    if (status == 0) {
        if (S_ISLNK(info.st_mode)) {
            errno = EACCES;
            status = -1;
        } else if (file_in_use(&info)) {
            errno = EBUSY;
            status = -1;
        } else {
            struct stat target;
            const int exists = fstatat(destination_parent, destination_leaf, &target,
                                       AT_SYMLINK_NOFOLLOW);
            if (exists == 0 && S_ISLNK(target.st_mode)) {
                errno = EACCES;
                status = -1;
            } else if (exists == 0 && (flags & 1u) != 0 && file_in_use(&target)) {
                errno = EBUSY;
                status = -1;
            } else if (exists < 0 && errno != ENOENT) {
                status = -1;
            } else {
                status = (int)syscall(SYS_renameat2, source_parent, source_leaf,
                                      destination_parent, rename_leaf,
                                      (flags & 1u) == 0 ? RENAME_NOREPLACE : 0);
            }
        }
    }
    const int error = errno;
    pthread_mutex_unlock(&file_lock);
    close(source_parent);
    close(destination_parent);
    if (status != 0)
        nt_SetLastError(path_error(error, false));
    return status == 0;
}

enum { CRT_APPEND = 0x0008, CRT_TEXT = 0x4000, CRT_BINARY = 0x8000 };
static _Atomic int default_mode = CRT_TEXT;
static int default_commit;

int *NT_MS_ABI nt_p_commode(void)
{
    return &default_commit;
}

int NT_MS_ABI nt_set_fmode(int mode)
{
    if (mode != CRT_TEXT && mode != CRT_BINARY) {
        errno = EINVAL;
        return EINVAL;
    }
    atomic_store(&default_mode, mode);
    return 0;
}

struct stream_mode {
    char host[3];
    int flags;
    bool text, readable, writable, explicit_translation;
    bool commit;
};

static bool parse_mode(const char *mode, struct stream_mode *result)
{
    if (mode == NULL || (*mode != 'r' && *mode != 'w' && *mode != 'a'))
        return false;
    const char kind = *mode++;
    bool update = false;
    int translation = 0;
    char commit = 0;
    for (; *mode != '\0'; ++mode) {
        if (*mode == '+' && !update)
            update = true;
        else if ((*mode == 'b' || *mode == 't') && translation == 0)
            translation = *mode == 'b' ? CRT_BINARY : CRT_TEXT;
        else if ((*mode == 'c' || *mode == 'n') && commit == 0)
            commit = *mode;
        else
            return false;
    }
    const bool readable = kind == 'r' || update;
    const bool writable = kind != 'r' || update;
    const int access = update ? O_RDWR : (readable ? O_RDONLY : O_WRONLY);
    const int creation = kind == 'r' ? 0 : O_CREAT | (kind == 'w' ? O_TRUNC : O_APPEND);
    *result = (struct stream_mode){{kind, update ? '+' : '\0', '\0'},
        access | creation, (translation == 0 ? atomic_load(&default_mode) : translation) == CRT_TEXT,
        readable, writable, translation != 0, commit == 'c' || (commit == 0 && default_commit != 0)};
    return true;
}

struct guest_stream {
    struct guest_stream *next;
    FILE *file;
    struct guest_file *opened;
    struct stream_mode mode;
    bool text_eof;
};

static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_stream *streams;
static struct guest_stream standard[3];

static struct guest_stream **find_stream(const void *handle)
{
    struct guest_stream **link = &streams;
    while (*link != NULL && *link != handle)
        link = &(*link)->next;
    return link;
}

void *NT_MS_ABI nt_acrt_iob_func(uint32_t index)
{
    if (index >= sizeof(standard) / sizeof(standard[0])) {
        errno = EINVAL;
        return NULL;
    }
    return &standard[index];
}

static bool set_win_standard_handles(const int fd[3])
{
    uintptr_t prepared[3] = {0};
    bool ok = true;
    pthread_mutex_lock(&file_lock);
    for (size_t i = 0; i < 3; ++i) {
        if (fd[i] < 0)
            continue;
        struct guest_file *slot = new_file();
        if (slot == NULL || next_handle > UINTPTR_MAX - 16) {
            errno = ENOMEM;
            ok = false;
            break;
        }
        const int duplicate = fcntl(fd[i], F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) {
            ok = false;
            break;
        }
        struct stat info;
        if (fstat(duplicate, &info) != 0) {
            const int error = errno;
            close(duplicate);
            errno = error;
            ok = false;
            break;
        }
        prepared[i] = next_handle;
        next_handle += 16;
        *slot = (struct guest_file){.owner = WIN_HANDLE, .handle = prepared[i], .fd = duplicate,
            .access = i == 0 ? FILE_READ : FILE_WRITE, .share = FILE_READ | FILE_WRITE,
            .device = info.st_dev, .inode = info.st_ino};
    }
    const int error = errno;
    for (size_t i = 0; i < 3; ++i) {
        struct guest_file *file = find_handle(ok ? initial_handles[i] : prepared[i]);
        if (file != NULL)
            close_file(file);
        if (ok)
            initial_handles[i] = standard_handles[i] = prepared[i];
    }
    pthread_mutex_unlock(&file_lock);
    errno = error;
    return ok;
}

bool nt_runtime_set_standard_streams(int input, int output, int error)
{
    const int fd[] = {input, output, error};
    FILE *files_to_attach[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        if (fd[i] < 0)
            continue;
        const int duplicate = fcntl(fd[i], F_DUPFD_CLOEXEC, 3);
        if (duplicate >= 0)
            files_to_attach[i] = fdopen(duplicate, i == 0 ? "r" : "w");
        if (files_to_attach[i] == NULL) {
            const int cause = errno;
            if (duplicate >= 0)
                close(duplicate);
            errno = cause;
            goto failed;
        }
    }
    if (!set_win_standard_handles(fd))
        goto failed;
    pthread_mutex_lock(&stream_lock);
    for (size_t i = 0; i < 3; ++i) {
        struct guest_stream *stream = &standard[i];
        if (stream->file != NULL) {
            *find_stream(stream) = stream->next;
            fclose(stream->file);
        }
        *stream = (struct guest_stream){.file = files_to_attach[i], .mode = {
            .text = true, .readable = i == 0, .writable = i != 0
        }};
        if (stream->file != NULL) {
            stream->next = streams;
            streams = stream;
        }
    }
    pthread_mutex_unlock(&stream_lock);
    return true;
failed: {
        const int cause = errno;
        for (size_t i = 0; i < 3; ++i) {
            if (files_to_attach[i] != NULL)
                fclose(files_to_attach[i]);
        }
        errno = cause;
        return false;
    }
}

static unsigned int mode_access(struct stream_mode mode)
{
    return (mode.readable ? FILE_READ : 0) | (mode.writable ? FILE_WRITE : 0);
}

static struct guest_stream *attach_stream(struct guest_file *opened, struct stream_mode mode)
{
    struct guest_stream *stream = malloc(sizeof(*stream));
    if (stream == NULL)
        return NULL;
    FILE *file = fdopen(opened->fd, mode.host);
    if (file == NULL) {
        const int error = errno;
        free(stream);
        errno = error;
        return NULL;
    }
    *stream = (struct guest_stream){.file = file, .opened = opened, .mode = mode};
    return stream;
}

static void register_stream(struct guest_stream *stream)
{
    stream->opened->owner = CRT_STREAM;
    pthread_mutex_lock(&stream_lock);
    stream->next = streams;
    streams = stream;
    pthread_mutex_unlock(&stream_lock);
}

void *NT_MS_ABI nt_fopen(const char *input, const char *mode)
{
    struct stream_mode parsed;
    char path[PATH_MAX];
    if (!parse_mode(mode, &parsed)) {
        errno = EINVAL;
        return NULL;
    }
    if (!guest_path(input, path))
        return NULL;
    const char *resolved = path;
    const int root = duplicate_root(&resolved);
    if (root < 0)
        return NULL;
    pthread_mutex_lock(&file_lock);
    struct guest_file *opened = new_file();
    bool created;
    const int fd = opened != NULL ?
        open_shared(opened, root, resolved, parsed.flags, mode_access(parsed), FILE_READ | FILE_WRITE, &created) : -1;
    int error = errno;
    close(root);
    struct guest_stream *stream = NULL;
    if (fd >= 0) {
        stream = attach_stream(opened, parsed);
        if (stream == NULL) {
            error = errno;
            close(fd);
        } else if ((parsed.flags & O_TRUNC) != 0 && !created && ftruncate(fd, 0) != 0) {
            error = errno;
            fclose(stream->file);
            free(stream);
            stream = NULL;
        } else {
            register_stream(stream);
        }
    }
    pthread_mutex_unlock(&file_lock);
    if (stream == NULL)
        errno = error == EBUSY ? EACCES : error;
    return stream;
}

int NT_MS_ABI nt_open_osfhandle(intptr_t handle, int flags)
{
    if ((flags & ~(CRT_TEXT | CRT_BINARY | CRT_APPEND)) != 0 ||
        (flags & (CRT_TEXT | CRT_BINARY)) == (CRT_TEXT | CRT_BINARY)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&file_lock);
    struct guest_file *file = find_handle((uintptr_t)handle);
    int result = -1;
    if (file == NULL) {
        errno = EBADF;
    } else {
        int options = (flags & CRT_APPEND) != 0 ? fcntl(file->fd, F_GETFL) : 0;
        if (options >= 0 && ((flags & CRT_APPEND) == 0 ||
                            fcntl(file->fd, F_SETFL, options | O_APPEND) == 0)) {
            file->text = (flags & CRT_BINARY) == 0 &&
                         ((flags & CRT_TEXT) != 0 || atomic_load(&default_mode) == CRT_TEXT);
            file->owner = CRT_DESCRIPTOR;
            result = 3 + (int)(file - files);
        }
    }
    pthread_mutex_unlock(&file_lock);
    return result;
}

static struct guest_file *find_descriptor(int descriptor)
{
    if (descriptor < 3 || descriptor - 3 >= FILE_TABLE_SIZE)
        return NULL;
    struct guest_file *file = &files[descriptor - 3];
    return file->owner == CRT_DESCRIPTOR ? file : NULL;
}

void *NT_MS_ABI nt_fdopen(int descriptor, const char *mode)
{
    struct stream_mode parsed;
    if (!parse_mode(mode, &parsed)) {
        errno = EINVAL;
        return NULL;
    }
    pthread_mutex_lock(&file_lock);
    struct guest_file *file = find_descriptor(descriptor);
    struct guest_stream *stream = NULL;
    if (file == NULL) {
        errno = EBADF;
    } else if ((mode_access(parsed) & ~file->access) != 0) {
        errno = EINVAL;
    } else {
        if (!parsed.explicit_translation)
            parsed.text = file->text;
        stream = attach_stream(file, parsed);
        if (stream != NULL)
            register_stream(stream);
    }
    pthread_mutex_unlock(&file_lock);
    return stream;
}

int NT_MS_ABI nt_close(int descriptor)
{
    pthread_mutex_lock(&file_lock);
    struct guest_file *file = find_descriptor(descriptor);
    const int result = file != NULL ? close_file(file) : -1;
    pthread_mutex_unlock(&file_lock);
    if (file == NULL)
        errno = EBADF;
    return result;
}

int NT_MS_ABI nt_fclose(void *handle)
{
    pthread_mutex_lock(&stream_lock);
    struct guest_stream **link = find_stream(handle);
    struct guest_stream *stream = *link;
    if (stream == NULL) {
        pthread_mutex_unlock(&stream_lock);
        errno = EINVAL;
        return EOF;
    }
    *link = stream->next;
    if (stream->opened == NULL) {
        const int result = fclose(stream->file);
        *stream = (struct guest_stream){0};
        pthread_mutex_unlock(&stream_lock);
        return result;
    }
    pthread_mutex_unlock(&stream_lock);
    pthread_mutex_lock(&file_lock);
    int result = 0;
    if (stream->mode.writable && stream->mode.commit) {
        if (fflush(stream->file) != 0 || fdatasync(fileno(stream->file)) != 0)
            result = EOF;
    }
    if (fclose(stream->file) != 0)
        result = EOF;
    free(stream->opened->path);
    stream->opened->path = NULL;
    stream->opened->owner = FREE_FILE;
    pthread_mutex_unlock(&file_lock);
    free(stream);
    return result;
}

static size_t text_read(struct guest_stream *stream, unsigned char *buffer, size_t bytes)
{
    size_t read = 0;
    while (read < bytes && !stream->text_eof) {
        int byte = fgetc(stream->file);
        if (byte == EOF || byte == 0x1a) {
            stream->text_eof = true;
            break;
        }
        if (byte == '\r') {
            const int next = fgetc(stream->file);
            if (next == '\n')
                byte = '\n';
            else if (next != EOF)
                ungetc(next, stream->file);
        }
        buffer[read++] = (unsigned char)byte;
    }
    return read;
}

size_t NT_MS_ABI nt_fread(void *buffer, size_t size, size_t count, void *handle)
{
    if (size == 0 || count == 0)
        return 0;
    if (buffer == NULL || count > SIZE_MAX / size) {
        errno = EINVAL;
        return 0;
    }
    pthread_mutex_lock(&stream_lock);
    struct guest_stream *stream = *find_stream(handle);
    size_t result = 0;
    if (stream == NULL || !stream->mode.readable)
        errno = EINVAL;
    else if (stream->mode.text)
        result = text_read(stream, buffer, size * count) / size;
    else
        result = fread(buffer, size, count, stream->file);
    pthread_mutex_unlock(&stream_lock);
    return result;
}

static size_t text_write(FILE *file, const unsigned char *buffer, size_t bytes)
{
    size_t written = 0;
    while (written < bytes) {
        const unsigned char byte = buffer[written];
        if ((byte == '\n' && fputc('\r', file) == EOF) || fputc(byte, file) == EOF)
            break;
        ++written;
    }
    return written;
}

size_t NT_MS_ABI nt_fwrite(const void *buffer, size_t size, size_t count, void *handle)
{
    if (size == 0 || count == 0)
        return 0;
    if (buffer == NULL || count > SIZE_MAX / size) {
        errno = EINVAL;
        return 0;
    }
    pthread_mutex_lock(&stream_lock);
    struct guest_stream *stream = *find_stream(handle);
    size_t result = 0;
    if (stream == NULL || !stream->mode.writable)
        errno = EINVAL;
    else if (stream->mode.text)
        result = text_write(stream->file, buffer, size * count) / size;
    else
        result = fwrite(buffer, size, count, stream->file);
    pthread_mutex_unlock(&stream_lock);
    return result;
}

void nt_runtime_flush_streams(void)
{
    pthread_mutex_lock(&stream_lock);
    for (struct guest_stream *stream = streams; stream != NULL; stream = stream->next) {
        if (stream->mode.writable && fflush(stream->file) == 0 && stream->mode.commit)
            fdatasync(fileno(stream->file));
    }
    pthread_mutex_unlock(&stream_lock);
}

_Static_assert(sizeof(struct nt_createfile2_params) == 32, "CREATEFILE2_EXTENDED_PARAMETERS layout");
_Static_assert(sizeof(struct nt_file_attribute_data) == 36, "WIN32_FILE_ATTRIBUTE_DATA layout");
_Static_assert(sizeof(struct nt_find_data_w) == 592, "WIN32_FIND_DATAW layout");

static bool wide_path(const uint16_t *input, char output[PATH_MAX])
{
    if (input == NULL || nt_runtime_wide_length(input, PATH_MAX) == PATH_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return false;
    }
    if (nt_WideCharToMultiByte(65001, 0x80, input, -1, output, PATH_MAX, NULL, NULL) == 0)
        return false;
    if (strncmp(output, "\\\\?\\", 4) == 0 &&
        (output[4] == 'C' || output[4] == 'c') && output[5] == ':' && separator(output[6]))
        memmove(output, output + 4, strlen(output + 4) + 1);
    return true;
}

static uint32_t path_result(const char *path, uint32_t size, uint16_t *buffer)
{
    const int32_t required = nt_MultiByteToWideChar(65001, 8, path, -1, NULL, 0);
    if (required == 0 || (uint32_t)required > size)
        return (uint32_t)required;
    if (buffer == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    return (uint32_t)(nt_MultiByteToWideChar(65001, 8, path, -1, buffer, required) - 1);
}

uintptr_t NT_MS_ABI nt_CreateFileW(const uint16_t *path, uint32_t access, uint32_t share,
                                  const void *security, uint32_t disposition, uint32_t flags,
                                  uintptr_t template_file)
{
    char narrow[PATH_MAX];
    return wide_path(path, narrow) ?
           nt_CreateFileA(narrow, access, share, security, disposition, flags, template_file) : UINTPTR_MAX;
}

_Static_assert(sizeof(struct nt_io_status_block) == 16 &&
               offsetof(struct nt_io_status_block, information) == 8, "AMD64 IO_STATUS_BLOCK");

static nt_status nt_file_error(uint32_t error)
{
    switch (error) {
    case NT_ERROR_FILE_NOT_FOUND: return (nt_status)UINT32_C(0xc0000034);
    case NT_ERROR_PATH_NOT_FOUND: return (nt_status)UINT32_C(0xc000003a);
    case NT_ERROR_FILE_EXISTS: return (nt_status)UINT32_C(0xc0000035);
    case NT_ERROR_SHARING_VIOLATION: return (nt_status)UINT32_C(0xc0000043);
    case NT_ERROR_INVALID_NAME: return (nt_status)UINT32_C(0xc0000033);
    case NT_ERROR_INVALID_HANDLE: return NT_STATUS_INVALID_HANDLE;
    case NT_ERROR_BROKEN_PIPE: return (nt_status)UINT32_C(0xc000014b);
    case NT_ERROR_INVALID_PARAMETER: return NT_STATUS_INVALID_PARAMETER;
    case NT_ERROR_NOT_ENOUGH_MEMORY: return NT_STATUS_NO_MEMORY;
    case NT_ERROR_ACCESS_DENIED: return NT_STATUS_ACCESS_DENIED;
    case NT_ERROR_NOT_SUPPORTED: return NT_STATUS_NOT_SUPPORTED;
    default: return NT_STATUS_UNSUCCESSFUL;
    }
}

nt_status NT_MS_ABI nt_NtCreateFile(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, struct nt_io_status_block *io,
    const int64_t *allocation, uint32_t file_attributes, uint32_t share,
    uint32_t disposition, uint32_t options, const void *ea, uint32_t ea_length)
{
    static const uint32_t win_dispositions[] = {
        0, OPEN_EXISTING, CREATE_NEW, OPEN_ALWAYS, TRUNCATE_EXISTING, CREATE_ALWAYS,
    };
    if (!handle || !io || !attributes || !attributes->name || !attributes->name->buffer)
        return NT_STATUS_ACCESS_VIOLATION;
    const struct nt_guest_unicode_string *name = attributes->name;
    const size_t length = name->length / sizeof(uint16_t);
    if (attributes->size != sizeof(*attributes) || disposition >= sizeof(win_dispositions) / sizeof(*win_dispositions) ||
        (name->length & 1u) || name->length > name->maximum_length || length >= PATH_MAX)
        return NT_STATUS_INVALID_PARAMETER;
    const uint32_t supported_access = UINT32_C(0xc0100003);
    if (!disposition || attributes->root || attributes->security || attributes->qos ||
        (attributes->attributes & ~0x42u) || (allocation && *allocation) || ea || ea_length ||
        (access & ~supported_access) || (options & ~0x60u) || (share & ~3u) ||
        (file_attributes & ~FILE_ATTRIBUTE_NORMAL))
        return NT_STATUS_NOT_SUPPORTED;
    if ((options & 0x20u) && !(access & 0x100000u))
        return NT_STATUS_INVALID_PARAMETER;

    static const uint16_t prefix[] = {'\\', '?', '?', '\\'};
    if (length < 7 || memcmp(name->buffer, prefix, sizeof(prefix)) ||
        (name->buffer[4] != 'C' && name->buffer[4] != 'c') ||
        name->buffer[5] != ':' || name->buffer[6] != '\\')
        return (nt_status)UINT32_C(0xc000003b); /* STATUS_OBJECT_PATH_SYNTAX_BAD */
    uint16_t path[PATH_MAX];
    for (size_t i = 4; i < length; ++i) {
        if (!name->buffer[i])
            return NT_STATUS_INVALID_PARAMETER;
        path[i - 4] = name->buffer[i];
    }
    path[length - 4] = 0;
    uint32_t desired = access & UINT32_C(0xc0000000);
    if (access & 1u)
        desired |= UINT32_C(0x80000000);
    if (access & 2u)
        desired |= UINT32_C(0x40000000);
    const struct security_attributes security = {sizeof(security), NULL, (attributes->attributes & 2u) != 0};
    const uint32_t saved = nt_GetLastError();
    const uintptr_t result = nt_CreateFileW(path, desired, share, &security,
                                            win_dispositions[disposition], file_attributes, 0);
    const uint32_t error = nt_GetLastError();
    nt_SetLastError(saved);
    if (result == UINTPTR_MAX)
        return nt_file_error(error);
    uintptr_t action = 1; /* FILE_OPENED */
    if (disposition == 2 || ((disposition == 3 || disposition == 5) && error == NT_ERROR_SUCCESS))
        action = 2; /* FILE_CREATED */
    else if (disposition == 4 || disposition == 5)
        action = 3; /* FILE_OVERWRITTEN */
    *io = (struct nt_io_status_block){NT_STATUS_SUCCESS, action};
    *handle = result;
    return NT_STATUS_SUCCESS;
}

nt_status NT_MS_ABI nt_NtOpenFile(uintptr_t *handle, uint32_t access,
    const struct nt_object_attributes *attributes, struct nt_io_status_block *io,
    uint32_t share, uint32_t options)
{
    return nt_NtCreateFile(handle, access, attributes, io, NULL, 0, share, 1, options, NULL, 0);
}

uintptr_t NT_MS_ABI nt_CreateFile2(const uint16_t *path, uint32_t access, uint32_t share,
                                  uint32_t disposition, const struct nt_createfile2_params *params)
{
    if (params != NULL && (params->size != sizeof(*params) || params->security_qos != 0)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return UINTPTR_MAX;
    }
    return nt_CreateFileW(path, access, share, params != NULL ? params->security : NULL, disposition,
                          params != NULL ? params->attributes | params->flags : 0,
                          params != NULL ? params->template_file : 0);
}

int32_t NT_MS_ABI nt_CreateDirectoryW(const uint16_t *path, const void *security)
{
    char narrow[PATH_MAX];
    return wide_path(path, narrow) && nt_CreateDirectoryA(narrow, security);
}

int32_t NT_MS_ABI nt_RemoveDirectoryW(const uint16_t *path)
{
    char narrow[PATH_MAX];
    return wide_path(path, narrow) && nt_RemoveDirectoryA(narrow);
}

int32_t NT_MS_ABI nt_DeleteFileW(const uint16_t *path)
{
    char narrow[PATH_MAX];
    return wide_path(path, narrow) && nt_DeleteFileA(narrow);
}

int32_t NT_MS_ABI nt_MoveFileExW(const uint16_t *source, const uint16_t *destination, uint32_t flags)
{
    char from[PATH_MAX], to[PATH_MAX];
    return wide_path(source, from) && wide_path(destination, to) && nt_MoveFileExA(from, to, flags);
}

int32_t NT_MS_ABI nt_MoveFileW(const uint16_t *source, const uint16_t *destination)
{
    return nt_MoveFileExW(source, destination, 0);
}

int32_t NT_MS_ABI nt_CopyFileW(const uint16_t *source, const uint16_t *destination,
                              int32_t fail_if_exists)
{
    char from[PATH_MAX], to[PATH_MAX];
    return wide_path(source, from) && wide_path(destination, to) &&
           nt_CopyFileA(from, to, fail_if_exists);
}

uint32_t NT_MS_ABI nt_GetCurrentDirectoryW(uint32_t size, uint16_t *buffer)
{
    char path[PATH_MAX + 3];
    pthread_mutex_lock(&root_lock);
    copy_dos_path(guest_cwd, path, sizeof(path));
    pthread_mutex_unlock(&root_lock);
    return path_result(path, size, buffer);
}

int32_t NT_MS_ABI nt_SetCurrentDirectoryW(const uint16_t *path)
{
    char narrow[PATH_MAX], canonical[PATH_MAX + 3], relative[PATH_MAX];
    if (!wide_path(path, narrow))
        return 0;
    const int fd = nt_runtime_open_guest_directory(narrow, canonical, sizeof(canonical));
    if (fd < 0 || !normalize_path(canonical, relative, true)) {
        const int error = errno;
        if (fd >= 0)
            close(fd);
        nt_SetLastError(path_error(error, true));
        return 0;
    }
    pthread_mutex_lock(&root_lock);
    strcpy(guest_cwd, relative);
    pthread_mutex_unlock(&root_lock);
    close(fd);
    return 1;
}

uint32_t NT_MS_ABI nt_GetFullPathNameW(const uint16_t *path, uint32_t size, uint16_t *buffer,
                                      uint16_t **leaf)
{
    char narrow[PATH_MAX], relative[PATH_MAX], absolute[PATH_MAX + 3];
    if (leaf != NULL)
        *leaf = NULL;
    if (!wide_path(path, narrow))
        return 0;
    if (!normalize_path(narrow, relative, true) || !copy_dos_path(relative, absolute, sizeof(absolute))) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const uint32_t length = path_result(absolute, size, buffer);
    if (length != 0 && length < size && leaf != NULL && relative[0] != '\0') {
        size_t index = length;
        while (index > 0 && buffer[index - 1] != '\\')
            --index;
        *leaf = buffer + index;
    }
    return length;
}

/* ANSI entry points are UTF-8 at this stage of the native bridge:
 * the true Windows ACP/OEM codepage model is tracked separately. */
static bool utf8_guest_to_wide(const char *path, uint16_t result[PATH_MAX])
{
    if (!path) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return false;
    }
    return nt_MultiByteToWideChar(65001, 8, path, -1, result, PATH_MAX) != 0;
}

uint32_t NT_MS_ABI nt_GetCurrentDirectoryA(uint32_t size, char *buffer)
{
    char full[PATH_MAX + 3];
    pthread_mutex_lock(&root_lock);
    const bool valid = copy_dos_path(guest_cwd, full, sizeof(full));
    pthread_mutex_unlock(&root_lock);
    if (!valid) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const size_t required = strlen(full) + 1;
    if (size < required)
        return (uint32_t)required;
    if (!buffer) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(buffer, full, required);
    return (uint32_t)required - 1;
}

int32_t NT_MS_ABI nt_SetCurrentDirectoryA(const char *path)
{
    uint16_t wide[PATH_MAX];
    return utf8_guest_to_wide(path, wide) && nt_SetCurrentDirectoryW(wide);
}

uint32_t NT_MS_ABI nt_GetFullPathNameA(const char *path, uint32_t size,
                                      char *buffer, char **leaf)
{
    char relative[PATH_MAX], full[PATH_MAX + 3];
    if (leaf)
        *leaf = NULL;
    if (!normalize_path(path, relative, true) ||
        !copy_dos_path(relative, full, sizeof(full))) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const size_t required = strlen(full) + 1;
    if (size < required)
        return (uint32_t)required;
    if (!buffer) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    memcpy(buffer, full, required);
    if (leaf && relative[0] != '\0') {
        char *slash = strrchr(buffer, '\\');
        *leaf = slash ? slash + 1 : NULL;
    }
    return (uint32_t)required - 1;
}

uint32_t NT_MS_ABI nt_GetFileAttributesA(const char *path)
{
    uint16_t wide[PATH_MAX];
    return utf8_guest_to_wide(path, wide) ?
           nt_GetFileAttributesW(wide) : UINT32_MAX;
}

int32_t NT_MS_ABI nt_GetFileAttributesExA(const char *path, int32_t level,
                                         struct nt_file_attribute_data *info)
{
    uint16_t wide[PATH_MAX];
    return utf8_guest_to_wide(path, wide) &&
           nt_GetFileAttributesExW(wide, level, info);
}

uint32_t NT_MS_ABI nt_GetTempPathA(uint32_t size, char *buffer)
{
    uint16_t wide[PATH_MAX];
    const uint32_t count = nt_GetTempPathW(PATH_MAX, wide);
    if (count == 0 || count >= PATH_MAX) {
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const int32_t required = nt_WideCharToMultiByte(65001, 0x80, wide, -1,
                                                    NULL, 0, NULL, NULL);
    if (required <= 0)
        return 0;
    if (size < (uint32_t)required)
        return (uint32_t)required;
    if (!buffer) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (nt_WideCharToMultiByte(65001, 0x80, wide, -1, buffer, required,
                               NULL, NULL) != required)
        return 0;
    return (uint32_t)required - 1;
}

static int duplicate_handle(uintptr_t handle, unsigned int access)
{
    pthread_mutex_lock(&file_lock);
    const struct guest_file *file = find_handle(handle);
    int fd = -1;
    if (file == NULL)
        errno = EBADF;
    else if ((access & ~file->access) != 0)
        errno = EACCES;
    else
        fd = fcntl(file->fd, F_DUPFD_CLOEXEC, 3);
    const int error = errno;
    pthread_mutex_unlock(&file_lock);
    errno = error;
    return fd;
}

static int32_t file_result(int fd, bool success)
{
    const int error = errno;
    if (fd >= 0)
        close(fd);
    if (!success)
        nt_SetLastError(path_error(error, false));
    return success;
}

int32_t NT_MS_ABI nt_ReadFile(uintptr_t handle, void *buffer, uint32_t size, uint32_t *done, void *overlap)
{
    if (done != NULL)
        *done = 0;
    if (done == NULL || (size != 0 && buffer == NULL) || overlap != NULL) {
        nt_SetLastError(overlap != NULL ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int fd = duplicate_handle(handle, FILE_READ);
    if (fd < 0)
        return file_result(fd, false);
    ssize_t received;
    do {
        received = read(fd, buffer, size);
    } while (received < 0 && errno == EINTR);
    if (received == 0 && size != 0) {
        struct stat info;
        if (fstat(fd, &info) == 0 && S_ISFIFO(info.st_mode)) {
            errno = EPIPE;
            received = -1;
        }
    }
    if (received >= 0)
        *done = (uint32_t)received;
    return file_result(fd, received >= 0);
}

int32_t NT_MS_ABI nt_WriteFile(uintptr_t handle, const void *buffer, uint32_t size, uint32_t *done, void *overlap)
{
    if (done != NULL)
        *done = 0;
    if (done == NULL || (size != 0 && buffer == NULL) || overlap != NULL) {
        nt_SetLastError(overlap != NULL ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int fd = duplicate_handle(handle, FILE_WRITE);
    if (fd < 0)
        return file_result(fd, false);
    ssize_t written;
    do {
        written = write(fd, buffer, size);
    } while (written < 0 && errno == EINTR);
    if (written >= 0)
        *done = (uint32_t)written;
    return file_result(fd, written >= 0);
}

/* CopyFile is built on the same guest HANDLE table and path confinement
 * as CreateFile. In particular, OPEN_ALWAYS does not truncate until
 * both descriptors are safely opened and the source/destination inodes
 * have been checked for identity. */
int32_t NT_MS_ABI nt_CopyFileA(const char *source, const char *destination,
                              int32_t fail_if_exists)
{
    const uint32_t generic_read = UINT32_C(0x80000000);
    const uint32_t generic_write = UINT32_C(0x40000000);
    const uintptr_t from = nt_CreateFileA(source, generic_read, FILE_READ, NULL,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (from == UINTPTR_MAX)
        return 0;

    const uintptr_t to = nt_CreateFileA(destination, generic_write, 0, NULL,
                                       fail_if_exists ? CREATE_NEW : OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, 0);
    if (to == UINTPTR_MAX) {
        const uint32_t error = nt_GetLastError();
        nt_CloseHandle(from);
        nt_SetLastError(error);
        return 0;
    }
    const bool created = fail_if_exists || nt_GetLastError() != NT_ERROR_ALREADY_EXISTS;

    /* Hard links and different casings of the same path are equally
     * forbidden as a copy destination: never truncate the source inode. */
    bool identical = false;
    pthread_mutex_lock(&file_lock);
    const struct guest_file *read_file = find_handle(from);
    const struct guest_file *write_file = find_handle(to);
    if (read_file && write_file)
        identical = read_file->device == write_file->device &&
                    read_file->inode == write_file->inode;
    pthread_mutex_unlock(&file_lock);

    uint32_t error = NT_ERROR_SUCCESS;
    int32_t success = 0;
    unsigned char *buffer = NULL;
    if (identical) {
        error = NT_ERROR_ACCESS_DENIED;
    } else if ((buffer = malloc(64u * 1024u)) == NULL) {
        error = NT_ERROR_NOT_ENOUGH_MEMORY;
    } else if (!nt_SetEndOfFile(to)) {
        error = nt_GetLastError();
    } else {
        bool finished = false;
        for (;;) {
            uint32_t received = 0;
            if (!nt_ReadFile(from, buffer, 64u * 1024u, &received, NULL)) {
                error = nt_GetLastError();
                break;
            }
            if (received == 0) {
                finished = true;
                break;
            }
            uint32_t position = 0;
            while (position < received) {
                uint32_t written = 0;
                if (!nt_WriteFile(to, buffer + position, received - position, &written, NULL)) {
                    error = nt_GetLastError();
                    break;
                }
                if (written == 0) {
                    error = NT_ERROR_GEN_FAILURE;
                    break;
                }
                position += written;
            }
            if (error != NT_ERROR_SUCCESS)
                break;
        }
        if (finished && nt_FlushFileBuffers(to))
            success = 1;
        else if (finished)
            error = nt_GetLastError();
    }
    free(buffer);
    if (!nt_CloseHandle(to) && success) {
        success = 0;
        error = nt_GetLastError();
    }
    if (!nt_CloseHandle(from) && success) {
        success = 0;
        error = nt_GetLastError();
    }
    if (!success) {
        /* A newly created partial destination is not a valid copy. */
        if (created)
            nt_DeleteFileA(destination);
        nt_SetLastError(error);
    }
    return success;
}

/* Native NTDLL file operations use the same guest HANDLE table and the
 * same underlying Linux open-file description as Win32 ReadFile/WriteFile.
 * A plain offset uses read/write and therefore updates the shared pointer;
 * an explicit non-negative offset uses pread/pwrite. The Windows async
 * event/APC contract is not implemented and must fail explicitly. */
enum { FILE_STANDARD_INFORMATION_CLASS = 5, FILE_POSITION_INFORMATION_CLASS = 14,
       FILE_END_OF_FILE_INFORMATION_CLASS = 20 };

struct nt_file_standard_information {
    int64_t allocation_size, end_of_file;
    uint32_t number_of_links;
    uint8_t delete_pending, directory;
    uint8_t padding[2];
};

struct nt_file_position_information {
    int64_t current_byte_offset;
};

_Static_assert(sizeof(struct nt_file_standard_information) == 24,
               "FILE_STANDARD_INFORMATION x64 layout");
_Static_assert(sizeof(struct nt_file_position_information) == 8,
               "FILE_POSITION_INFORMATION x64 layout");

static nt_status nt_file_complete(struct nt_io_status_block *io, nt_status status, uintptr_t information)
{
    if (io != NULL)
        *io = (struct nt_io_status_block){status, information};
    return status;
}

static nt_status nt_file_errno_status(int error)
{
    if (error == ESPIPE)
        return NT_STATUS_NOT_SUPPORTED; /* No seeking on pipes or consoles. */
    return nt_file_error(path_error(error, false));
}

static nt_status nt_guest_io(uintptr_t handle, uintptr_t event, void *apc, void *apc_context,
                             struct nt_io_status_block *io, void *buffer, uint32_t length,
                             const int64_t *offset, const uint32_t *key, bool write_operation)
{
    if (io == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (event != 0 || apc != NULL || apc_context != NULL || key != NULL)
        return nt_file_complete(io, NT_STATUS_NOT_SUPPORTED, 0);
    if (length != 0 && buffer == NULL)
        return nt_file_complete(io, NT_STATUS_ACCESS_VIOLATION, 0);
    /* The Win32 length is ULONG (32 bits); Linux AMD64 ssize_t is 64 bits,
     * so every possible length is representable by read/pread/write/pwrite. */
    if (offset != NULL && *offset < 0)
        return nt_file_complete(io, NT_STATUS_INVALID_PARAMETER, 0);

    const int fd = duplicate_handle(handle, write_operation ? FILE_WRITE : FILE_READ);
    if (fd < 0)
        return nt_file_complete(io, nt_file_errno_status(errno), 0);

    ssize_t transferred;
    do {
        if (offset != NULL) {
            transferred = write_operation ?
                pwrite(fd, buffer, length, (off_t)*offset) :
                pread(fd, buffer, length, (off_t)*offset);
        } else {
            transferred = write_operation ?
                write(fd, buffer, length) :
                read(fd, buffer, length);
        }
    } while (transferred < 0 && errno == EINTR);

    const int error = errno;
    (void)close(fd);
    if (transferred < 0)
        return nt_file_complete(io, nt_file_errno_status(error), 0);
    if (!write_operation && transferred == 0 && length > 0)
        return nt_file_complete(io, NT_STATUS_END_OF_FILE, 0);
    return nt_file_complete(io, NT_STATUS_SUCCESS, (uintptr_t)transferred);
}

nt_status NT_MS_ABI nt_NtReadFile(uintptr_t handle, uintptr_t event, void *apc,
    void *apc_context, struct nt_io_status_block *io, void *buffer, uint32_t length,
    const int64_t *offset, const uint32_t *key)
{
    return nt_guest_io(handle, event, apc, apc_context, io, buffer, length, offset, key, false);
}

nt_status NT_MS_ABI nt_NtWriteFile(uintptr_t handle, uintptr_t event, void *apc,
    void *apc_context, struct nt_io_status_block *io, const void *buffer, uint32_t length,
    const int64_t *offset, const uint32_t *key)
{
    /* The shared POSIX implementation never writes to the caller's buffer. */
    return nt_guest_io(handle, event, apc, apc_context, io, (void *)buffer,
                       length, offset, key, true);
}

nt_status NT_MS_ABI nt_NtQueryInformationFile(uintptr_t handle, struct nt_io_status_block *io,
                                                void *buffer, uint32_t length, uint32_t information_class)
{
    if (io == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (information_class != FILE_STANDARD_INFORMATION_CLASS &&
        information_class != FILE_POSITION_INFORMATION_CLASS)
        return nt_file_complete(io, NT_STATUS_INVALID_INFO_CLASS, 0);
    const uint32_t needed = information_class == FILE_STANDARD_INFORMATION_CLASS ?
                            sizeof(struct nt_file_standard_information) :
                            sizeof(struct nt_file_position_information);
    if (buffer == NULL)
        return nt_file_complete(io, NT_STATUS_ACCESS_VIOLATION, 0);
    if (length < needed)
        return nt_file_complete(io, NT_STATUS_INFO_LENGTH_MISMATCH, 0);

    const int fd = duplicate_handle(handle, 0);
    if (fd < 0)
        return nt_file_complete(io, nt_file_errno_status(errno), 0);

    int error = 0;
    if (information_class == FILE_STANDARD_INFORMATION_CLASS) {
        struct stat file;
        if (fstat(fd, &file) != 0) {
            error = errno;
        } else {
            const struct nt_file_standard_information info = {
                .allocation_size = (int64_t)file.st_blocks * 512,
                .end_of_file = file.st_size,
                .number_of_links = file.st_nlink > UINT32_MAX ? UINT32_MAX : (uint32_t)file.st_nlink,
                .delete_pending = 0,
                .directory = S_ISDIR(file.st_mode) != 0
            };
            memcpy(buffer, &info, sizeof(info));
        }
    } else {
        const off_t position = lseek(fd, 0, SEEK_CUR);
        if (position < 0) {
            error = errno;
        } else {
            const struct nt_file_position_information info = {(int64_t)position};
            memcpy(buffer, &info, sizeof(info));
        }
    }
    (void)close(fd);
    if (error != 0)
        return nt_file_complete(io, nt_file_errno_status(error), 0);
    return nt_file_complete(io, NT_STATUS_SUCCESS, needed);
}

nt_status NT_MS_ABI nt_NtSetInformationFile(uintptr_t handle, struct nt_io_status_block *io,
                                              const void *buffer, uint32_t length,
                                              uint32_t information_class)
{
    if (io == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    if (information_class != FILE_POSITION_INFORMATION_CLASS &&
        information_class != FILE_END_OF_FILE_INFORMATION_CLASS)
        return nt_file_complete(io, NT_STATUS_INVALID_INFO_CLASS, 0);
    if (buffer == NULL)
        return nt_file_complete(io, NT_STATUS_ACCESS_VIOLATION, 0);
    if (length < sizeof(struct nt_file_position_information))
        return nt_file_complete(io, NT_STATUS_INFO_LENGTH_MISMATCH, 0);

    struct nt_file_position_information info;
    memcpy(&info, buffer, sizeof(info));
    if (info.current_byte_offset < 0)
        return nt_file_complete(io, NT_STATUS_INVALID_PARAMETER, 0);

    const int fd = duplicate_handle(handle, information_class == FILE_END_OF_FILE_INFORMATION_CLASS ?
                                    FILE_WRITE : 0);
    if (fd < 0)
        return nt_file_complete(io, nt_file_errno_status(errno), 0);

    const bool success = information_class == FILE_POSITION_INFORMATION_CLASS ?
                         lseek(fd, (off_t)info.current_byte_offset, SEEK_SET) >= 0 :
                         ftruncate(fd, (off_t)info.current_byte_offset) == 0;
    const int error = errno;
    (void)close(fd);
    return nt_file_complete(io, success ? NT_STATUS_SUCCESS : nt_file_errno_status(error), 0);
}

nt_status NT_MS_ABI nt_NtFlushBuffersFile(uintptr_t handle, struct nt_io_status_block *io)
{
    if (io == NULL)
        return NT_STATUS_ACCESS_VIOLATION;
    /* Match Win32 FlushFileBuffers: a read-only guest HANDLE cannot flush. */
    const int fd = duplicate_handle(handle, FILE_WRITE);
    if (fd < 0)
        return nt_file_complete(io, nt_file_errno_status(errno), 0);
    const bool success = fsync(fd) == 0;
    const int error = errno;
    (void)close(fd);
    return nt_file_complete(io, success ? NT_STATUS_SUCCESS : nt_file_errno_status(error), 0);
}

int32_t NT_MS_ABI nt_FlushFileBuffers(uintptr_t handle)
{
    const int fd = duplicate_handle(handle, FILE_WRITE);
    return file_result(fd, fd >= 0 && fsync(fd) == 0);
}

int32_t NT_MS_ABI nt_GetFileSizeEx(uintptr_t handle, int64_t *size)
{
    if (size == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int fd = duplicate_handle(handle, 0);
    struct stat info;
    const bool ok = fd >= 0 && fstat(fd, &info) == 0;
    if (ok)
        *size = info.st_size;
    return file_result(fd, ok);
}

int32_t NT_MS_ABI nt_SetFilePointerEx(uintptr_t handle, int64_t offset, int64_t *position, uint32_t method)
{
    const int whence[] = {SEEK_SET, SEEK_CUR, SEEK_END};
    if (method >= sizeof(whence) / sizeof(whence[0]) || (method == 0 && offset < 0)) {
        nt_SetLastError(method == 0 ? NT_ERROR_NEGATIVE_SEEK : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int fd = duplicate_handle(handle, 0);
    const off_t result = fd >= 0 ? lseek(fd, offset, whence[method]) : -1;
    if (result >= 0 && position != NULL)
        *position = result;
    if (result < 0 && fd >= 0 && errno == EINVAL) {
        close(fd);
        nt_SetLastError(NT_ERROR_NEGATIVE_SEEK);
        return 0;
    }
    return file_result(fd, result >= 0);
}

int32_t NT_MS_ABI nt_SetEndOfFile(uintptr_t handle)
{
    const int fd = duplicate_handle(handle, FILE_WRITE);
    const off_t position = fd >= 0 ? lseek(fd, 0, SEEK_CUR) : -1;
    return file_result(fd, position >= 0 && ftruncate(fd, position) == 0);
}

uint32_t NT_MS_ABI nt_GetFileType(uintptr_t handle)
{
    const int fd = duplicate_handle(handle, 0);
    struct stat info;
    if (!file_result(fd, fd >= 0 && fstat(fd, &info) == 0))
        return 0;
    if (S_ISREG(info.st_mode) || S_ISDIR(info.st_mode))
        return 1;
    if (S_ISCHR(info.st_mode))
        return 2;
    if (S_ISFIFO(info.st_mode) || S_ISSOCK(info.st_mode))
        return 3;
    nt_SetLastError(NT_ERROR_SUCCESS);
    return 0;
}

static struct nt_file_time file_time(struct timespec time)
{
    if (time.tv_sec < -INT64_C(11644473600) ||
        time.tv_sec > INT64_C(1833029933770))
        return (struct nt_file_time){0};
    const uint64_t ticks = (uint64_t)(time.tv_sec + INT64_C(11644473600)) * UINT64_C(10000000) +
                           (uint64_t)time.tv_nsec / 100;
    return (struct nt_file_time){(uint32_t)ticks, (uint32_t)(ticks >> 32)};
}

static uint64_t ticks(struct nt_file_time time)
{
    return (uint64_t)time.high << 32 | time.low;
}

static struct nt_file_attribute_data file_attributes(int fd, const struct stat *stat_info)
{
    struct timespec created = stat_info->st_ctim;
    struct statx extended;
    if (statx(fd, "", AT_EMPTY_PATH | AT_STATX_DONT_SYNC, STATX_BTIME, &extended) == 0 &&
        (extended.stx_mask & STATX_BTIME) != 0)
        created = (struct timespec){extended.stx_btime.tv_sec, extended.stx_btime.tv_nsec};
    uint32_t attributes = S_ISDIR(stat_info->st_mode) ? 0x10u : 0;
    if ((stat_info->st_mode & 0222) == 0)
        attributes |= 1;
    const uint64_t size = S_ISDIR(stat_info->st_mode) ? 0 : (uint64_t)stat_info->st_size;
    return (struct nt_file_attribute_data){attributes != 0 ? attributes : FILE_ATTRIBUTE_NORMAL,
        file_time(created), file_time(stat_info->st_atim), file_time(stat_info->st_mtim),
        (uint32_t)(size >> 32), (uint32_t)size};
}

int32_t NT_MS_ABI nt_GetFileAttributesExW(const uint16_t *path, int32_t level,
                                         struct nt_file_attribute_data *info)
{
    if (level != 0 || info == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    char narrow[PATH_MAX], relative[PATH_MAX];
    if (!wide_path(path, narrow))
        return 0;
    if (!normalize_path(narrow, relative, true)) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const char *resolved = relative[0] != '\0' ? relative : ".";
    const int root = duplicate_root(&resolved);
    const int fd = root >= 0 ? confined_open(root, resolved, O_PATH, 0) : -1;
    const int error = errno;
    if (root >= 0)
        close(root);
    errno = error;
    struct stat stat_info;
    if (fd < 0 || fstat(fd, &stat_info) != 0)
        return file_result(fd, false);
    if (!S_ISREG(stat_info.st_mode) && !S_ISDIR(stat_info.st_mode)) {
        errno = EACCES;
        return file_result(fd, false);
    }
    *info = file_attributes(fd, &stat_info);
    return file_result(fd, true);
}

uint32_t NT_MS_ABI nt_GetFileAttributesW(const uint16_t *path)
{
    struct nt_file_attribute_data attributes;
    return nt_GetFileAttributesExW(path, 0, &attributes) ? attributes.attributes : UINT32_MAX;
}

int32_t NT_MS_ABI nt_AreFileApisANSI(void)
{
    /* File APIs in this process keep their ANSI mode. */
    return 1;
}

enum { FIND_LIMIT = 64 };
struct directory_search {
    uintptr_t handle;
    DIR *directory;
    char pattern[PATH_MAX];
};
static struct directory_search searches[FIND_LIMIT];
static uintptr_t next_search = 0x75000001;
static pthread_mutex_t search_lock = PTHREAD_MUTEX_INITIALIZER;

static struct directory_search *find_search(uintptr_t handle)
{
    for (size_t i = 0; i < FIND_LIMIT; ++i) {
        if (searches[i].handle == handle)
            return &searches[i];
    }
    return NULL;
}

static int next_entry(const struct directory_search *search, struct nt_find_data_w *data)
{
    const char *pattern = strcmp(search->pattern, "*.*") == 0 ? "*" : search->pattern;
    for (;;) {
        errno = 0;
        const struct dirent *entry = readdir(search->directory);
        if (entry == NULL)
            return errno;
        char normalized[PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            fnmatch(pattern, entry->d_name, FNM_CASEFOLD) != 0)
            continue;
        /* Add a rooted prefix to validate the entry independently of CWD. */
        char rooted[PATH_MAX];
        if (snprintf(rooted, sizeof(rooted), "/%s", entry->d_name) >= (int)sizeof(rooted) ||
            !guest_path(rooted, normalized) || strcmp(normalized, entry->d_name) != 0)
            continue;
        struct nt_find_data_w result = {0};
        if (nt_MultiByteToWideChar(65001, 8, entry->d_name, -1, result.name, 260) == 0)
            continue;
        const int fd = confined_open(dirfd(search->directory), entry->d_name, O_PATH, 0);
        if (fd < 0)
            continue;
        struct stat stat_info;
        const bool valid = fstat(fd, &stat_info) == 0 &&
                           (S_ISREG(stat_info.st_mode) || S_ISDIR(stat_info.st_mode));
        if (valid)
            result.info = file_attributes(fd, &stat_info);
        close(fd);
        if (valid) {
            *data = result;
            return -1;
        }
    }
}

uintptr_t NT_MS_ABI nt_FindFirstFileExW(const uint16_t *path, int32_t level, struct nt_find_data_w *data,
                                       int32_t operation, void *filter, uint32_t flags)
{
    if (data == NULL || (level != 0 && level != 1) || operation != 0 ||
        filter != NULL || (flags & ~2u) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return UINTPTR_MAX;
    }
    char narrow[PATH_MAX], canonical[PATH_MAX + 3];
    if (!wide_path(path, narrow))
        return UINTPTR_MAX;
    char *pattern = narrow;
    for (char *cursor = narrow; *cursor != '\0'; ++cursor) {
        if (separator(*cursor))
            pattern = cursor + 1;
    }
    if (*pattern == '\0' || strchr(pattern, ':') != NULL || strchr(pattern, '[') != NULL) {
        nt_SetLastError(NT_ERROR_INVALID_NAME);
        return UINTPTR_MAX;
    }
    struct directory_search search = {0};
    strcpy(search.pattern, pattern);
    if (pattern != narrow)
        *pattern = '\0';
    const int fd = nt_runtime_open_guest_directory(pattern == narrow ? "." : narrow,
                                                    canonical, sizeof(canonical));
    if (fd < 0) {
        nt_SetLastError(path_error(errno, true));
        return UINTPTR_MAX;
    }
    search.directory = fdopendir(fd);
    if (search.directory == NULL) {
        file_result(fd, false);
        return UINTPTR_MAX;
    }
    struct nt_find_data_w first;
    const int result = next_entry(&search, &first);
    if (result != -1) {
        closedir(search.directory);
        nt_SetLastError(result == 0 ? NT_ERROR_FILE_NOT_FOUND : path_error(result, false));
        return UINTPTR_MAX;
    }
    pthread_mutex_lock(&search_lock);
    struct directory_search *slot = find_search(0);
    uintptr_t handle = UINTPTR_MAX;
    if (slot != NULL && next_search != UINTPTR_MAX) {
        handle = next_search++;
        search.handle = handle;
        *slot = search;
        *data = first;
    }
    pthread_mutex_unlock(&search_lock);
    if (handle == UINTPTR_MAX) {
        closedir(search.directory);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    }
    return handle;
}

uintptr_t NT_MS_ABI nt_FindFirstFileW(const uint16_t *path, struct nt_find_data_w *data)
{
    return nt_FindFirstFileExW(path, 0, data, 0, NULL, 0);
}

int32_t NT_MS_ABI nt_FindNextFileW(uintptr_t handle, struct nt_find_data_w *data)
{
    if (data == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&search_lock);
    const struct directory_search *search = handle != 0 ? find_search(handle) : NULL;
    const int result = search != NULL ? next_entry(search, data) : EBADF;
    pthread_mutex_unlock(&search_lock);
    if (result != -1)
        nt_SetLastError(result == 0 ? NT_ERROR_NO_MORE_FILES : path_error(result, false));
    return result == -1;
}

int32_t NT_MS_ABI nt_FindClose(uintptr_t handle)
{
    pthread_mutex_lock(&search_lock);
    struct directory_search *search = handle != 0 ? find_search(handle) : NULL;
    DIR *directory = search != NULL ? search->directory : NULL;
    if (search != NULL)
        search->handle = 0;
    pthread_mutex_unlock(&search_lock);
    if (directory == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
        return 0;
    }
    return closedir(directory) == 0;
}

static bool open_path_name(uintptr_t handle, char path[PATH_MAX + 3])
{
    pthread_mutex_lock(&file_lock);
    const struct guest_file *file = find_handle(handle);
    const bool success = file != NULL && file->path != NULL &&
                         copy_dos_path(file->path, path, PATH_MAX + 3);
    pthread_mutex_unlock(&file_lock);
    if (!success)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return success;
}

uint32_t NT_MS_ABI nt_GetFinalPathNameByHandleW(uintptr_t handle, uint16_t *buffer, uint32_t size, uint32_t flags)
{
    if ((flags & ~12u) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_FLAGS);
        return 0;
    }
    char path[PATH_MAX + 7] = "\\\\?\\";
    if (!open_path_name(handle, path + 4))
        return 0;
    return path_result(path + ((flags & 4) != 0 ? 6 : 0), size, buffer);
}

struct basic_file_info { uint64_t created, accessed, written, changed; uint32_t attributes; };
struct standard_file_info { int64_t allocation, size; uint32_t links; uint8_t pending, directory; };
_Static_assert(sizeof(struct basic_file_info) == 40, "FILE_BASIC_INFO layout");
_Static_assert(sizeof(struct standard_file_info) == 24, "FILE_STANDARD_INFO layout");

int32_t NT_MS_ABI nt_GetFileInformationByHandleEx(uintptr_t handle, int32_t kind, void *info, uint32_t size)
{
    if (info == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (kind == 2) {
        char path[PATH_MAX + 3];
        if (!open_path_name(handle, path))
            return 0;
        uint16_t *name = nt_runtime_utf8_to_wide(path + 2);
        if (name == NULL) {
            nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }
        const uint32_t bytes = (uint32_t)(nt_runtime_wide_length(name, PATH_MAX) * sizeof(*name));
        const bool fits = size >= sizeof(bytes) && size - sizeof(bytes) >= bytes;
        if (fits) {
            memcpy(info, &bytes, sizeof(bytes));
            memcpy((char *)info + sizeof(bytes), name, bytes);
        } else {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        }
        free(name);
        return fits;
    }
    const size_t needed = kind == 0 ? sizeof(struct basic_file_info) :
                          kind == 1 ? sizeof(struct standard_file_info) : kind == 9 ? 8 : 0;
    if (needed == 0 || size < needed) {
        nt_SetLastError(needed == 0 ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const int fd = duplicate_handle(handle, 0);
    struct stat stat_info;
    if (fd < 0 || fstat(fd, &stat_info) != 0)
        return file_result(fd, false);
    const struct nt_file_attribute_data attr = file_attributes(fd, &stat_info);
    if (kind == 0) {
        const struct basic_file_info basic = {ticks(attr.created), ticks(attr.accessed), ticks(attr.written),
                                              ticks(file_time(stat_info.st_ctim)), attr.attributes};
        memcpy(info, &basic, sizeof(basic));
    } else if (kind == 1) {
        const struct standard_file_info standard_info = {
            (int64_t)((uint64_t)stat_info.st_blocks * 512), stat_info.st_size,
            (uint32_t)stat_info.st_nlink, stat_info.st_nlink == 0, S_ISDIR(stat_info.st_mode) != 0
        };
        memcpy(info, &standard_info, sizeof(standard_info));
    } else {
        const uint32_t data[] = {attr.attributes, 0};
        memcpy(info, data, sizeof(data));
    }
    return file_result(fd, true);
}

static struct timespec to_timespec(uint64_t ticks_value)
{
    if (ticks_value == 0)
        return (struct timespec){0, UTIME_OMIT};
    return (struct timespec){(int64_t)(ticks_value / UINT64_C(10000000)) - INT64_C(11644473600),
                             (long)(ticks_value % UINT64_C(10000000)) * 100};
}

int32_t NT_MS_ABI nt_SetFileInformationByHandle(uintptr_t handle, int32_t kind, const void *info, uint32_t size)
{
    if (info == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (kind == 6) {
        int64_t length;
        if (size < sizeof(length)) {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memcpy(&length, info, sizeof(length));
        if (length < 0) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        const int fd = duplicate_handle(handle, FILE_WRITE);
        return file_result(fd, fd >= 0 && ftruncate(fd, length) == 0);
    }
    if (kind == 0) {
        struct basic_file_info basic;
        if (size < sizeof(basic)) {
            nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        memcpy(&basic, info, sizeof(basic));
        if (basic.created != 0 || basic.changed != 0 || basic.attributes != 0 ||
            basic.accessed > INT64_MAX || basic.written > INT64_MAX) {
            nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
            return 0;
        }
        const struct timespec dates[] = {to_timespec(basic.accessed), to_timespec(basic.written)};
        const int fd = duplicate_handle(handle, FILE_WRITE);
        return file_result(fd, fd >= 0 && futimens(fd, dates) == 0);
    }
    nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
    return 0;
}

uintptr_t NT_MS_ABI nt_GetStdHandle(uint32_t which)
{
    const uint32_t index = (uint32_t)-10 - which;
    if (index >= 3) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return UINTPTR_MAX;
    }
    pthread_mutex_lock(&file_lock);
    const uintptr_t handle = standard_handles[index];
    pthread_mutex_unlock(&file_lock);
    return handle;
}

int32_t NT_MS_ABI nt_SetStdHandle(uint32_t which, uintptr_t handle)
{
    const uint32_t index = (uint32_t)-10 - which;
    if (index >= 3) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&file_lock);
    standard_handles[index] = handle;
    pthread_mutex_unlock(&file_lock);
    return 1;
}

uint32_t NT_MS_ABI nt_GetConsoleOutputCP(void)
{
    /* The console bridge writes UTF-8 to a Linux terminal. */
    return 65001;
}

int32_t NT_MS_ABI nt_GetConsoleMode(uintptr_t handle, uint32_t *mode)
{
    if (mode == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int fd = duplicate_handle(handle, 0);
    struct termios options;
    const bool ok = fd >= 0 && tcgetattr(fd, &options) == 0;
    if (ok) {
        if (handle == nt_GetStdHandle((uint32_t)-10)) {
            *mode = ((options.c_lflag & ISIG) != 0 ? 1u : 0) |
                    ((options.c_lflag & ICANON) != 0 ? 2u : 0) |
                    ((options.c_lflag & ECHO) != 0 ? 4u : 0);
        } else {
            *mode = (options.c_oflag & OPOST) != 0 ? 3u : 0;
        }
    } else if (fd >= 0) {
        errno = EBADF;
    }
    return file_result(fd, ok);
}

/* Native console operations use the same real Linux TTY descriptors that
 * GetStdHandle, ReadFile and GetConsoleMode expose to the guest. */
int32_t NT_MS_ABI nt_ReadConsoleW(uintptr_t handle, uint16_t *buffer, uint32_t count,
                                  uint32_t *done, const void *control)
{
    if (done == NULL || (count != 0 && buffer == NULL) || control != NULL) {
        nt_SetLastError(control != NULL ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *done = 0;
    const int fd = duplicate_handle(handle, FILE_READ);
    struct termios term;
    if (fd < 0 || tcgetattr(fd, &term) != 0) {
        if (fd >= 0)
            errno = EBADF;
        return file_result(fd, false);
    }
    static _Thread_local struct {
        uintptr_t handle;
        uint16_t low;
        bool ready;
    } pending;
    if (pending.handle != handle)
        pending.ready = false;
    pending.handle = handle;
    if (pending.ready && count != 0) {
        buffer[(*done)++] = pending.low;
        pending.ready = false;
    }
    while (*done < count) {
        if (!nt_runtime_console_wait(fd))
            return file_result(fd, false);
        char bytes[4];
        ssize_t got;
        do {
            got = read(fd, bytes, 1);
        } while (got < 0 && errno == EINTR);
        if (got < 0)
            return file_result(fd, false);
        if (got == 0)
            break;
        const unsigned char first = (unsigned char)bytes[0];
        unsigned int length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
                              first >= 0xe0 && first <= 0xef ? 3 :
                              first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        if (length == 0) {
            nt_SetLastError(NT_ERROR_NO_UNICODE_TRANSLATION);
            close(fd);
            return 0;
        }
        for (unsigned int i = 1; i < length; ++i) {
            do {
                got = read(fd, bytes + i, 1);
            } while (got < 0 && errno == EINTR);
            if (got != 1 || ((unsigned char)bytes[i] & 0xc0u) != 0x80u) {
                nt_SetLastError(NT_ERROR_NO_UNICODE_TRANSLATION);
                close(fd);
                return 0;
            }
        }
        uint16_t wide[2];
        const int32_t converted = nt_MultiByteToWideChar(65001, 8, bytes,
                                                          (int32_t)length, wide, 2);
        if (converted == 0) {
            close(fd);
            return 0;
        }
        buffer[(*done)++] = wide[0];
        if (converted == 2) {
            if (*done < count)
                buffer[(*done)++] = wide[1];
            else {
                pending.low = wide[1];
                pending.ready = true;
            }
        }
        if (first == '\n')
            break;
    }
    close(fd);
    return 1;
}

int32_t NT_MS_ABI nt_SetConsoleTitleA(const char *title)
{
    if (title == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const size_t length = strnlen(title, 8193);
    if (length > 8192) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const int fd = duplicate_handle(nt_GetStdHandle((uint32_t)-11), FILE_WRITE);
    struct termios term;
    if (fd < 0 || tcgetattr(fd, &term) != 0) {
        if (fd >= 0)
            errno = EBADF;
        return file_result(fd, false);
    }
    /* CP1252 is the current Win32 ANSI compatibility profile. Convert to
     * UTF-8 before sending an OSC 0 terminal title to the Linux console. */
    const int32_t units = nt_MultiByteToWideChar(1252, 0, title,
                                                  (int32_t)length, NULL, 0);
    uint16_t *wide = units >= 0 ? calloc((size_t)units + 1, sizeof(*wide)) : NULL;
    if (wide == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        close(fd);
        return 0;
    }
    if (length != 0 && nt_MultiByteToWideChar(1252, 0, title,
                                             (int32_t)length, wide, units) != units) {
        free(wide);
        close(fd);
        return 0;
    }
    const int32_t needed = units != 0 ?
        nt_WideCharToMultiByte(65001, 0, wide, units, NULL, 0, NULL, NULL) : 0;
    char *text = needed >= 0 ? malloc((size_t)needed + 6) : NULL;
    if (text == NULL) {
        free(wide);
        close(fd);
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    if (needed != 0 && nt_WideCharToMultiByte(65001, 0, wide, units, text + 4,
                                              needed, NULL, NULL) != needed) {
        free(wide);
        free(text);
        close(fd);
        return 0;
    }
    free(wide);
    /* Reject controls to prevent terminal escape sequence injection. */
    for (int32_t i = 0; i < needed; ++i) {
        if ((unsigned char)text[4 + i] < 0x20 || (unsigned char)text[4 + i] == 0x7f) {
            free(text);
            close(fd);
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
    }
    text[4 + needed] = 0;
    if (nt_runtime_console_title(text + 4)) {
        free(text);
        close(fd);
        return 1;
    }
    memcpy(text, "\x1b]0;", 4);
    text[4 + needed] = '\x07';
    size_t pos = 0, total = (size_t)needed + 5;
    while (pos < total) {
        ssize_t written = write(fd, text + pos, total - pos);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        pos += (size_t)written;
    }
    free(text);
    if (pos != total)
        return file_result(fd, false);
    close(fd);
    return 1;
}

int32_t NT_MS_ABI nt_WriteConsoleA(uintptr_t handle, const char *buffer, uint32_t count, uint32_t *done, void *reserved)
{
    if (reserved != NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    uint32_t mode;
    return nt_GetConsoleMode(handle, &mode) && nt_WriteFile(handle, buffer, count, done, NULL);
}

int32_t NT_MS_ABI nt_WriteConsoleW(uintptr_t handle, const uint16_t *buffer, uint32_t count, uint32_t *done, void *reserved)
{
    if (reserved != NULL || done == NULL || (count != 0 && buffer == NULL) || count > INT32_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    *done = 0;
    if (count == 0)
        return nt_WriteConsoleA(handle, NULL, 0, done, NULL);
    const int32_t bytes = nt_WideCharToMultiByte(65001, 0, buffer, (int32_t)count, NULL, 0, NULL, NULL);
    if (bytes == 0)
        return 0;
    char *utf8 = malloc((size_t)bytes);
    if (utf8 == NULL) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    nt_WideCharToMultiByte(65001, 0, buffer, (int32_t)count, utf8, bytes, NULL, NULL);
    uint32_t written;
    const bool ok = nt_WriteConsoleA(handle, utf8, (uint32_t)bytes, &written, NULL) &&
                    written == (uint32_t)bytes;
    free(utf8);
    if (ok)
        *done = count;
    return ok;
}
