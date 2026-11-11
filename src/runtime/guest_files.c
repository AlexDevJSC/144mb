/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "compat.h"
#include "user_folders.h"

#include <errno.h>
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
#include <unistd.h>

static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;
static int drive_root = -1;
static struct { int fd; bool active; } folder_roots[NT_USER_FOLDER_COUNT];

bool nt_runtime_set_file_root(const char *directory)
{
    const int next = directory != NULL ? open(directory, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory != NULL && next < 0)
        return false;
    pthread_mutex_lock(&root_lock);
    const int previous = drive_root;
    drive_root = next;
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

static bool guest_path(const char *input, char output[PATH_MAX])
{
    if (input == NULL || *input == '\0') {
        errno = EINVAL;
        return false;
    }
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
    while (*input != '\0') {
        while (separator(*input))
            ++input;
        const char *component = input;
        while (*input != '\0' && !separator(*input)) {
            const unsigned char byte = (unsigned char)*input;
            if (byte < 0x20 || byte > 0x7e || strchr("<>:\"|?*", byte) != NULL) {
                errno = EINVAL;
                return false;
            }
            ++input;
        }
        const size_t length = (size_t)(input - component);
        if (length == 0 || (length == 1 && component[0] == '.'))
            continue;
        if (length == 2 && memcmp(component, "..", 2) == 0) {
            errno = EACCES;
            return false;
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
    if (used == 0) {
        errno = EINVAL;
        return false;
    }
    output[used] = '\0';
    return true;
}

static int confined_open(int root, const char *path, int flags, unsigned int mode)
{
    const struct open_how how = {
        .flags = (uint64_t)(flags | O_CLOEXEC),
        .mode = mode,
        .resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV
    };
    return (int)syscall(SYS_openat2, root, path, &how, sizeof(how));
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

static int open_parent(int root, const char *path, const char **leaf)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        *leaf = path;
        return fcntl(root, F_DUPFD_CLOEXEC, 0);
    }
    char parent[PATH_MAX];
    const size_t length = (size_t)(slash - path);
    memcpy(parent, path, length);
    parent[length] = '\0';
    *leaf = slash + 1;
    return confined_open(root, parent, O_PATH | O_DIRECTORY, 0);
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
    if (path == NULL || strcmp(path, ".") == 0 || nt_module_name_equal(path, "C:\\") ||
        nt_module_name_equal(path, "C:/") || strcmp(path, "\\") == 0 || strcmp(path, "/") == 0)
        relative[0] = '\0';
    else if (!guest_path(path, relative))
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
    char relative[PATH_MAX];
    if (!guest_path(name, relative) || strcmp(name, relative) != 0 || strchr(relative, '/') != NULL)
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
        const char *leaf;
        fd = open_parent(root, resolved, &leaf);
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
    CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, OPEN_ALWAYS = 4,
    TRUNCATE_EXISTING = 5, FILE_ATTRIBUTE_NORMAL = 0x80
};

enum file_owner { FREE_FILE, WIN_HANDLE, CRT_DESCRIPTOR, CRT_STREAM };

struct guest_file {
    enum file_owner owner;
    uintptr_t handle;
    int fd;
    unsigned int access, share;
    dev_t device;
    ino_t inode;
    bool text;
};

static pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_file files[FILE_TABLE_SIZE];
static uintptr_t next_handle = 0x24000004;

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
    if ((!read_access && !write_access) || (desired & ~(generic_read | generic_write)) != 0 ||
        (share & ~(FILE_READ | FILE_WRITE)) != 0 || (flags & ~FILE_ATTRIBUTE_NORMAL) != 0 ||
        template_file != 0 || (attributes != NULL &&
                              (attributes->descriptor != NULL || attributes->inherit != 0))) {
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
    const char *leaf;
    const int parent = open_parent(root, resolved, &leaf);
    const int parent_error = errno;
    close(root);
    if (parent < 0) {
        nt_SetLastError(path_error(parent_error, true));
        return UINTPTR_MAX;
    }

    int options = read_access ? (write_access ? O_RDWR : O_RDONLY) : O_WRONLY;
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
            }
        }
    }
    pthread_mutex_unlock(&file_lock);
    close(parent);
    if (result == UINTPTR_MAX)
        nt_SetLastError(error == EEXIST ? NT_ERROR_FILE_EXISTS : path_error(error, false));
    else if (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
        nt_SetLastError(created ? NT_ERROR_SUCCESS : NT_ERROR_ALREADY_EXISTS);
    return result;
}

static int close_file(struct guest_file *file)
{
    const int result = close(file->fd);
    file->owner = FREE_FILE;
    return result;
}

int32_t NT_MS_ABI nt_CloseHandle(uintptr_t handle)
{
    pthread_mutex_lock(&file_lock);
    struct guest_file *file = find_handle(handle);
    const int result = file != NULL ? close_file(file) : -1;
    pthread_mutex_unlock(&file_lock);
    if (result != 0)
        nt_SetLastError(file != NULL ? path_error(errno, false) : NT_ERROR_INVALID_HANDLE);
    return result == 0;
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
    const char *leaf;
    const int parent = open_parent(root, resolved, &leaf);
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

int32_t NT_MS_ABI nt_MoveFileExA(const char *from, const char *to, uint32_t flags)
{
    char source[PATH_MAX], destination[PATH_MAX];
    if (flags > 1) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
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
        nt_SetLastError(source_root < 0 || destination_root < 0 ? NT_ERROR_PATH_NOT_FOUND :
                        source_root != destination_root ? NT_ERROR_NOT_SAME_DEVICE : path_error(root_error, true));
        return 0;
    }
    const char *source_leaf, *destination_leaf;
    const int source_parent = open_parent(root, resolved_source, &source_leaf);
    const int source_error = errno;
    const int destination_parent = open_parent(root, resolved_destination, &destination_leaf);
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

    pthread_mutex_lock(&file_lock);
    struct stat info;
    int status = fstatat(source_parent, source_leaf, &info, AT_SYMLINK_NOFOLLOW);
    if (status == 0) {
        if (file_in_use(&info) || (flags != 0 &&
            fstatat(destination_parent, destination_leaf, &info, AT_SYMLINK_NOFOLLOW) == 0 &&
            file_in_use(&info))) {
            errno = EBUSY;
            status = -1;
        } else {
            status = (int)syscall(SYS_renameat2, source_parent, source_leaf,
                                  destination_parent, destination_leaf,
                                  flags == 0 ? RENAME_NOREPLACE : 0);
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

bool nt_runtime_set_standard_streams(int input, int output, int error)
{
    const int fd[] = {input, output, error};
    FILE *files_to_attach[3] = {0};
    for (size_t i = 0; i < 3; ++i) {
        if (fd[i] < 0)
            continue;
        const int duplicate = fcntl(fd[i], F_DUPFD_CLOEXEC, 0);
        if (duplicate >= 0)
            files_to_attach[i] = fdopen(duplicate, i == 0 ? "r" : "w");
        if (files_to_attach[i] == NULL) {
            const int cause = errno;
            if (duplicate >= 0)
                close(duplicate);
            for (size_t j = 0; j < i; ++j) {
                if (files_to_attach[j] != NULL)
                    fclose(files_to_attach[j]);
            }
            errno = cause;
            return false;
        }
    }
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
