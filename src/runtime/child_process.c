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
    if (!launch_child || process_security || thread_security || inherit_handles || environment ||
        (flags & ~4u) || startup->reserved || startup->reserved_bytes || startup->reserved_data ||
        (startup->flags & ~1u)) {
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
    pid_t pid = 0;
    int status_fd = -1, start_fd = -1;
    int error = launch_child(fd, path, converted ? converted : default_command, workdir,
                             (flags & 4u) != 0, &pid, &status_fd, &start_fd);
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
