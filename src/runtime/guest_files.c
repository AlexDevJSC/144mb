/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "compat.h"

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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static pthread_mutex_t root_lock = PTHREAD_MUTEX_INITIALIZER;
static int drive_root = -1;

bool nt_runtime_set_file_root(const char *directory)
{
    const int next = directory != NULL ? open(directory, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory != NULL && next < 0)
        return false;
    pthread_mutex_lock(&root_lock);
    const int previous = drive_root;
    drive_root = next;
    pthread_mutex_unlock(&root_lock);
    if (previous >= 0)
        close(previous);
    return true;
}

static int duplicate_root(void)
{
    pthread_mutex_lock(&root_lock);
    int result = -1;
    if (drive_root >= 0)
        result = fcntl(drive_root, F_DUPFD_CLOEXEC, 0);
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

static int open_regular(int root, const char *path, int flags)
{
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
        if (fd >= 0 || errno != EEXIST)
            return fd;
    }
    return -1;
}

static int open_parent(int root, char path[PATH_MAX], const char **leaf)
{
    char *slash = strrchr(path, '/');
    if (slash == NULL) {
        *leaf = path;
        return fcntl(root, F_DUPFD_CLOEXEC, 0);
    }
    *slash = '\0';
    *leaf = slash + 1;
    return confined_open(root, path, O_PATH | O_DIRECTORY, 0);
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
    default:
        return NT_ERROR_GEN_FAILURE;
    }
}

enum path_operation { MAKE_DIRECTORY, REMOVE_FILE, REMOVE_DIRECTORY };

static int32_t change_path(const char *input, enum path_operation operation)
{
    char path[PATH_MAX];
    if (!guest_path(input, path)) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const int root = duplicate_root();
    if (root < 0) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const char *leaf;
    const int parent = open_parent(root, path, &leaf);
    const int parent_error = errno;
    close(root);
    if (parent < 0) {
        nt_SetLastError(path_error(parent_error, true));
        return 0;
    }

    int status;
    if (operation == MAKE_DIRECTORY) {
        status = mkdirat(parent, leaf, 0777);
    } else {
        struct stat info;
        status = fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW);
        if (status == 0 && S_ISLNK(info.st_mode)) {
            errno = EACCES;
            status = -1;
        }
        if (status == 0)
            status = unlinkat(parent, leaf, operation == REMOVE_DIRECTORY ? AT_REMOVEDIR : 0);
    }
    const int error = errno;
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
    const int root = duplicate_root();
    if (root < 0) {
        nt_SetLastError(path_error(errno, true));
        return 0;
    }
    const char *source_leaf, *destination_leaf;
    const int source_parent = open_parent(root, source, &source_leaf);
    const int source_error = errno;
    const int destination_parent = open_parent(root, destination, &destination_leaf);
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

    const int status = (int)syscall(SYS_renameat2, source_parent, source_leaf,
                                    destination_parent, destination_leaf,
                                    flags == 0 ? RENAME_NOREPLACE : 0);
    const int error = errno;
    close(source_parent);
    close(destination_parent);
    if (status != 0)
        nt_SetLastError(path_error(error, false));
    return status == 0;
}

enum { CRT_TEXT = 0x4000, CRT_BINARY = 0x8000 };
static _Atomic int default_mode = CRT_TEXT;

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
    bool text, readable, writable;
};

static bool parse_mode(const char *mode, struct stream_mode *result)
{
    if (mode == NULL || (*mode != 'r' && *mode != 'w' && *mode != 'a'))
        return false;
    const char kind = *mode++;
    bool update = false;
    int translation = 0;
    for (; *mode != '\0'; ++mode) {
        if (*mode == '+' && !update)
            update = true;
        else if ((*mode == 'b' || *mode == 't') && translation == 0)
            translation = *mode == 'b' ? CRT_BINARY : CRT_TEXT;
        else
            return false;
    }
    const bool readable = kind == 'r' || update;
    const bool writable = kind != 'r' || update;
    const int access = update ? O_RDWR : (readable ? O_RDONLY : O_WRONLY);
    const int creation = kind == 'r' ? 0 : O_CREAT | (kind == 'w' ? O_TRUNC : O_APPEND);
    *result = (struct stream_mode){{kind, update ? '+' : '\0', '\0'},
        access | creation, (translation == 0 ? atomic_load(&default_mode) : translation) == CRT_TEXT,
        readable, writable};
    return true;
}

struct guest_stream {
    struct guest_stream *next;
    FILE *file;
    struct stream_mode mode;
    bool text_eof;
};

static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_stream *streams;

static struct guest_stream **find_stream(const void *handle)
{
    struct guest_stream **link = &streams;
    while (*link != NULL && *link != handle)
        link = &(*link)->next;
    return link;
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
    const int root = duplicate_root();
    if (root < 0)
        return NULL;
    const int fd = open_regular(root, path, parsed.flags);
    const int open_error = errno;
    close(root);
    if (fd < 0) {
        errno = open_error;
        return NULL;
    }
    FILE *file = fdopen(fd, parsed.host);
    if (file == NULL) {
        const int error = errno;
        close(fd);
        errno = error;
        return NULL;
    }
    struct guest_stream *stream = malloc(sizeof(*stream));
    if (stream == NULL) {
        fclose(file);
        errno = ENOMEM;
        return NULL;
    }
    if ((parsed.flags & O_TRUNC) != 0 && ftruncate(fd, 0) != 0) {
        const int error = errno;
        fclose(file);
        free(stream);
        errno = error;
        return NULL;
    }
    pthread_mutex_lock(&stream_lock);
    *stream = (struct guest_stream){streams, file, parsed, false};
    streams = stream;
    pthread_mutex_unlock(&stream_lock);
    return stream;
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
    pthread_mutex_unlock(&stream_lock);
    const int result = fclose(stream->file);
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
        if (stream->mode.writable)
            fflush(stream->file);
    }
    pthread_mutex_unlock(&stream_lock);
}
