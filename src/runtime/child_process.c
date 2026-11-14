/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "child_process.h"
#include "winbase.h"
#include "winfile.h"
#include "../pe/pe_bytes.h"

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { COMMAND_LIMIT = 32768, ERROR_BAD_EXE_FORMAT = 193 };
_Static_assert(sizeof(struct nt_startup_info) == 104, "STARTUPINFOW ABI");
_Static_assert(sizeof(struct nt_process_information) == 24, "PROCESS_INFORMATION ABI");

/* This is a guest-only environment snapshot. The child runner inherits
 * its original sanitized Linux environment; guest values never enter
 * posix_spawn's envp (in particular no LD_PRELOAD/PATH injection). */
enum { CHILD_ENV_UNITS = 32767, CHILD_ENV_MAX_BYTES = 131072, CHILD_ENV_MAX_ENTRIES = 1024 };

uint32_t nt_runtime_build_child_environment(const uint16_t *block, int *output)
{
    if (block == NULL || output == NULL)
        return NT_ERROR_INVALID_PARAMETER;
    *output = -1;
    char *buffer = malloc(CHILD_ENV_MAX_BYTES);
    if (buffer == NULL)
        return NT_ERROR_NOT_ENOUGH_MEMORY;
    size_t source_at = 0, bytes = 0, entries = 0;
    uint32_t error = NT_ERROR_INVALID_PARAMETER;
    bool terminated = false;
    while (source_at < CHILD_ENV_UNITS) {
        const size_t len = nt_runtime_wide_length(block + source_at,
                                                  CHILD_ENV_UNITS - source_at);
        if (len == CHILD_ENV_UNITS - source_at)
            goto fail;
        if (len == 0) {
            /* After the last entry NUL, the block has one extra NUL.
             * An empty block consists of exactly two NULs. */
            if (bytes == 0) {
                if (block[1] != 0)
                    goto fail;
                buffer[bytes++] = '\0';
            }
            buffer[bytes++] = '\0';
            terminated = true;
            break;
        }
        if (++entries > CHILD_ENV_MAX_ENTRIES)
            goto fail;
        bool equals = false;
        for (size_t i = 1; i < len; ++i)
            if (block[source_at + i] == '=') {
                equals = true;
                break;
            }
        if (block[source_at] == '=') {
            /* Drive current-directory pseudo variables need their own
             * NT state model; do not discard them silently. */
            error = NT_ERROR_NOT_SUPPORTED;
            goto fail;
        }
        if (!equals)
            goto fail;
        char *entry = nt_runtime_utf16_to_utf8(block + source_at, len);
        if (entry == NULL)
            goto fail;
        const size_t n = strlen(entry) + 1;
        if (n > CHILD_ENV_MAX_BYTES - bytes) {
            free(entry);
            goto fail;
        }
        memcpy(buffer + bytes, entry, n);
        free(entry);
        bytes += n;
        source_at += len + 1;
    }
    if (!terminated || bytes < 2)
        goto fail;
    int fd = memfd_create("nt-child-environment", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        error = errno == ENOSYS ? NT_ERROR_NOT_SUPPORTED : NT_ERROR_GEN_FAILURE;
        goto fail;
    }
    size_t copied = 0;
    while (copied < bytes) {
        ssize_t wrote = write(fd, buffer + copied, bytes - copied);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            close(fd);
            error = NT_ERROR_GEN_FAILURE;
            goto fail;
        }
        copied += (size_t)wrote;
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW |
                                F_SEAL_SHRINK | F_SEAL_SEAL) != 0) {
        close(fd);
        error = NT_ERROR_GEN_FAILURE;
        goto fail;
    }
    free(buffer);
    *output = fd;
    return NT_ERROR_SUCCESS;
fail:
    free(buffer);
    return error;
}

bool nt_runtime_read_child_environment(int fd, char ***entries, char **storage)
{
    if (entries == NULL || storage == NULL || fd < 0)
        return false;
    *entries = NULL;
    *storage = NULL;
    struct stat info;
    const int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    const int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & required) != required ||
        fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size < 2 || info.st_size > CHILD_ENV_MAX_BYTES)
        return false;
    const size_t length = (size_t)info.st_size;
    char *data = malloc(length);
    if (data == NULL)
        return false;
    size_t done = 0;
    while (done < length) {
        ssize_t got = pread(fd, data + done, length - done, (off_t)done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            free(data);
            return false;
        }
        done += (size_t)got;
    }
    if (data[length - 1] != '\0' || data[length - 2] != '\0') {
        free(data);
        return false;
    }
    char **list = calloc(CHILD_ENV_MAX_ENTRIES + 1u, sizeof(*list));
    if (list == NULL) {
        free(data);
        return false;
    }
    size_t at = 0, count = 0;
    while (at < length && data[at] != '\0') {
        if (count == CHILD_ENV_MAX_ENTRIES) {
            free(list);
            free(data);
            return false;
        }
        const size_t n = strnlen(data + at, length - at);
        if (n == length - at || n == 0 ||
            memchr(data + at + 1, '=', n - 1) == NULL) {
            free(list);
            free(data);
            return false;
        }
        list[count++] = data + at;
        at += n + 1;
    }
    if (at + 1 != length && !(at == 0 && length == 2)) {
        free(list);
        free(data);
        return false;
    }
    *entries = list;
    *storage = data;
    return true;
}

static nt_child_launcher launch_child;

void nt_runtime_set_child_launcher(nt_child_launcher launcher)
{
    /* The worker sets this before starting any guest thread. */
    launch_child = launcher;
}

static bool is_amd64_pe(int fd)
{
    struct stat info;
    unsigned char dos[64], header[26];
    if (fstat(fd, &info) != 0 || pread(fd, dos, sizeof(dos), 0) != (ssize_t)sizeof(dos) ||
        dos[0] != 'M' || dos[1] != 'Z')
        return false;
    uint32_t offset = read_u32(dos + 0x3c);
    return (uint64_t)offset + sizeof(header) <= (uint64_t)info.st_size &&
           pread(fd, header, sizeof(header), offset) == (ssize_t)sizeof(header) &&
           read_u32(header) == 0x00004550 && read_u16(header + 4) == 0x8664 &&
           read_u16(header + 20) >= 2 && (read_u16(header + 22) & 0x2000) == 0 &&
           read_u16(header + 24) == 0x020b;
}

static uint32_t open_application(const uint16_t *application, const uint16_t *command,
                                 char path[PATH_MAX], int *fd)
{
    size_t length;
    if (!application) {
        if (!command)
            return NT_ERROR_INVALID_PARAMETER;
        while (*command == ' ' || *command == '\t')
            ++command;
        bool quoted = *command == '"';
        application = command + quoted;
        command = application;
        while (*command && (quoted ? *command != '"' : *command != ' ' && *command != '\t'))
            ++command;
        if (quoted && *command != '"')
            return NT_ERROR_INVALID_PARAMETER;
        length = (size_t)(command - application);
    } else {
        length = nt_runtime_wide_length(application, PATH_MAX);
    }
    if (!length || length >= PATH_MAX)
        return NT_ERROR_INVALID_PARAMETER;
    char *input = nt_runtime_utf16_to_utf8(application, length);
    if (!input)
        return NT_ERROR_INVALID_PARAMETER;
    bool exists = false;
    int handle = -1;
    if (nt_runtime_check_guest_file(input, path, PATH_MAX, &exists) && exists)
        handle = nt_runtime_open_guest_file_readonly(input);
    if (handle < 0 && !strchr(input, '\\') && !strchr(input, '/') && !strchr(input, '.')) {
        char with_extension[PATH_MAX];
        int size = snprintf(with_extension, sizeof(with_extension), "%s.exe", input);
        if (size > 0 && (size_t)size < sizeof(with_extension) &&
            nt_runtime_check_guest_file(with_extension, path, PATH_MAX, &exists) && exists)
            handle = nt_runtime_open_guest_file_readonly(with_extension);
    }
    free(input);
    if (handle < 0)
        return NT_ERROR_FILE_NOT_FOUND;
    if (!is_amd64_pe(handle)) {
        close(handle);
        return ERROR_BAD_EXE_FORMAT;
    }
    *fd = handle;
    return 0;
}

static bool current_directory(const uint16_t *requested, char directory[PATH_MAX])
{
    uint16_t inherited[PATH_MAX];
    if (!requested) {
        uint32_t copied = nt_GetCurrentDirectoryW(PATH_MAX, inherited);
        if (!copied || copied >= PATH_MAX)
            return false;
        requested = inherited;
    }
    size_t length = nt_runtime_wide_length(requested, PATH_MAX);
    if (!length || length == PATH_MAX)
        return false;
    char *narrow = nt_runtime_utf16_to_utf8(requested, length);
    if (!narrow)
        return false;
    const int fd = nt_runtime_open_guest_directory(narrow, directory, PATH_MAX);
    free(narrow);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

int32_t NT_MS_ABI nt_CreateProcessW(const uint16_t *application, const uint16_t *command,
        const void *process_security, const void *thread_security, int32_t inherit_handles,
        uint32_t flags, const void *environment, const uint16_t *directory,
        const struct nt_startup_info *startup, struct nt_process_information *info)
{
    if (info)
        *info = (struct nt_process_information){0};
    size_t length = command ? nt_runtime_wide_length(command, COMMAND_LIMIT) : 0;
    if (!startup || startup->size != sizeof(*startup) || !info || length == COMMAND_LIMIT ||
        (!application && !length)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!launch_child || process_security || thread_security ||
        (flags & ~(4u | 0x400u)) || startup->reserved || startup->reserved_bytes || startup->reserved_data ||
        (startup->flags & ~(1u | 0x100u))) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (environment != NULL && (flags & 0x400u) == 0) {
        /* ANSI environment blocks need the guest's codepage converter;
         * reinterpretation as UTF-16 would be incorrect. */
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    char path[PATH_MAX], workdir[PATH_MAX], default_command[PATH_MAX + 3];
    if (!current_directory(directory, workdir)) {
        nt_SetLastError(NT_ERROR_PATH_NOT_FOUND);
        return 0;
    }
    int fd = -1;
    uint32_t result = open_application(application, command, path, &fd);
    if (result) {
        nt_SetLastError(result);
        return 0;
    }
    char *converted = length ? nt_runtime_utf16_to_utf8(command, length) : NULL;
    if (length && !converted) {
        close(fd);
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    snprintf(default_command, sizeof(default_command), "\"%s\"", path);
    uint16_t *inherited = environment == NULL ? nt_GetEnvironmentStringsW() : NULL;
    const uint16_t *source = environment == NULL ? inherited : environment;
    int environment_fd = -1;
    uint32_t env_error = source == NULL ? NT_ERROR_NOT_ENOUGH_MEMORY :
                         nt_runtime_build_child_environment(source, &environment_fd);
    if (inherited != NULL)
        (void)nt_FreeEnvironmentStringsW(inherited);
    if (env_error != NT_ERROR_SUCCESS) {
        free(converted);
        close(fd);
        nt_SetLastError(env_error);
        return 0;
    }
    /* STARTF_USESTDHANDLES is meaningful only with bInheritHandles=TRUE
     * and three guest HANDLEs that the child can actually open. */
    uintptr_t stdio[3] = {startup->input, startup->output, startup->error};
    const uintptr_t *std_handles = (startup->flags & 0x100u) != 0 ? stdio : NULL;
    struct nt_child_file_transfer files;
    const uint32_t inherited_error =
        nt_runtime_snapshot_inherited_files(inherit_handles != 0, &files, std_handles);
    if (inherited_error != NT_ERROR_SUCCESS) {
        close(environment_fd);
        free(converted);
        close(fd);
        nt_SetLastError(inherited_error);
        return 0;
    }
    pid_t pid = 0;
    int status_fd = -1, start_fd = -1;
    int error = launch_child(fd, path, converted ? converted : default_command, workdir,
                             (flags & 4u) != 0, environment_fd, &files,
                             &pid, &status_fd, &start_fd);
    nt_runtime_release_inherited_files(&files);
    close(environment_fd);
    free(converted);
    close(fd);
    if (error) {
        nt_SetLastError(error == ENOMEM || error == EAGAIN ? NT_ERROR_NOT_ENOUGH_MEMORY : NT_ERROR_GEN_FAILURE);
        return 0;
    }
    if (!nt_runtime_watch_child(pid, status_fd, start_fd, info)) {
        close(status_fd);
        if (start_fd >= 0)
            close(start_fd);
        kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    return 1;
}
