/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { COMMAND_LINE_MAX = 32767, MAX_ARGUMENTS = 4096 };

static struct {
    uintptr_t module;
    const char *path, *line;
    char **argv;
    char *arguments;
    int argc;
} guest;

static void clear_argv(void)
{
    free(guest.argv);
    free(guest.arguments);
    guest.argv = NULL;
    guest.arguments = NULL;
    guest.argc = 0;
}

bool nt_runtime_set_guest_image(uintptr_t module, const char *path, const char *command_line)
{
    if (module != 0 && (path == NULL || *path == '\0' || command_line == NULL ||
        strnlen(path, COMMAND_LINE_MAX) == COMMAND_LINE_MAX ||
        strnlen(command_line, COMMAND_LINE_MAX) == COMMAND_LINE_MAX))
        return false;
    clear_argv();
    guest.module = module;
    guest.path = module != 0 ? path : NULL;
    guest.line = module != 0 ? command_line : NULL;
    return true;
}

uintptr_t nt_runtime_guest_module(void)
{
    return guest.module;
}

uint32_t NT_MS_ABI nt_GetModuleFileNameA(uintptr_t module, char *buffer, uint32_t size)
{
    if (guest.module == 0 || (module != 0 && module != guest.module)) {
        nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (size == 0 || buffer == NULL) {
        nt_SetLastError(size == 0 ? NT_ERROR_INSUFFICIENT_BUFFER : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const size_t length = strlen(guest.path);
    if (length >= size) {
        memcpy(buffer, guest.path, size - 1);
        buffer[size - 1] = '\0';
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
        return size;
    }
    memcpy(buffer, guest.path, length + 1);
    return (uint32_t)length;
}

static bool separator(char byte)
{
    return byte == ' ' || byte == '\t';
}

static const char *program_name(const char *p, char **copy)
{
    bool quoted = false;
    while (*p != '\0' && (quoted || !separator(*p))) {
        if (*p == '"')
            quoted = !quoted;
        else if (copy != NULL)
            *(*copy)++ = *p;
        ++p;
    }
    return p;
}

char *NT_MS_ABI nt_get_narrow_winmain_command_line(void)
{
    if (guest.line == NULL) {
        errno = EINVAL;
        return NULL;
    }
    const char *p = program_name(guest.line, NULL);
    while (separator(*p))
        ++p;
    return (char *)p;
}

static const char *copy_argument(const char *p, char **output)
{
    bool quoted = false;
    while (*p != '\0' && (quoted || !separator(*p))) {
        const char *first = p;
        while (*p == '\\')
            ++p;
        size_t slashes = (size_t)(p - first);
        if (*p != '"') {
            while (slashes != 0) {
                *(*output)++ = '\\';
                --slashes;
            }
            if (*p == '\0' || (!quoted && separator(*p)))
                break;
            *(*output)++ = *p++;
        } else {
            for (size_t i = 0; i < slashes / 2; ++i)
                *(*output)++ = '\\';
            ++p;
            if ((slashes & 1) != 0 || (quoted && *p == '"')) {
                *(*output)++ = '"';
                if ((slashes & 1) == 0)
                    ++p;
            } else {
                quoted = !quoted;
            }
        }
    }
    return p;
}

int NT_MS_ABI nt_configure_narrow_argv(int mode)
{
    if (mode == 0) {
        clear_argv();
        return 0;
    }
    if (mode != 1 || guest.line == NULL) {
        errno = mode == 2 ? ENOTSUP : EINVAL;
        return -1;
    }
    const bool empty_line = *guest.line == '\0';
    const char *line = empty_line ? guest.path : guest.line;
    char **argv = calloc(MAX_ARGUMENTS + 1, sizeof(*argv));
    char *arguments = malloc(strlen(line) + 2);
    if (argv == NULL || arguments == NULL) {
        free(argv);
        free(arguments);
        errno = ENOMEM;
        return -1;
    }
    char *out = arguments;
    int count = 1;
    argv[0] = out;
    const char *p;
    if (empty_line) {
        const size_t length = strlen(line);
        memcpy(out, line, length);
        out += length;
        p = line + length;
    } else {
        p = program_name(line, &out);
    }
    *out++ = '\0';
    while (*p != '\0') {
        while (separator(*p))
            ++p;
        if (*p == '\0')
            break;
        if (count == MAX_ARGUMENTS) {
            free(argv);
            free(arguments);
            errno = E2BIG;
            return -1;
        }
        argv[count++] = out;
        p = copy_argument(p, &out);
        *out++ = '\0';
    }
    clear_argv();
    guest.argv = argv;
    guest.arguments = arguments;
    guest.argc = count;
    return 0;
}

const char *const *nt_runtime_guest_argv(int *count)
{
    *count = guest.argc;
    return (const char *const *)guest.argv;
}

static int worker_status_fd = -1;

static _Noreturn void finish_worker(uint32_t code)
{
    ssize_t result;
    do {
        result = write(worker_status_fd, &code, sizeof(code));
    } while (result < 0 && errno == EINTR);
    _Exit(result == sizeof(code) ? EXIT_SUCCESS : EXIT_FAILURE);
}

bool nt_runtime_run_fixture(nt_int_callback entry, uint32_t *exit_code)
{
    if (entry == NULL || exit_code == NULL || worker_status_fd >= 0)
        return false;
    int channel[2];
    if (pipe(channel) != 0)
        return false;
    const pid_t child = fork();
    if (child == 0) {
        close(channel[0]);
        worker_status_fd = channel[1];
        finish_worker((uint32_t)entry());
    }
    close(channel[1]);
    if (child < 0) {
        close(channel[0]);
        return false;
    }
    uint32_t code;
    ssize_t length;
    do {
        length = read(channel[0], &code, sizeof(code));
    } while (length < 0 && errno == EINTR);
    close(channel[0]);
    int status;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || length != sizeof(code))
        return false;
    *exit_code = code;
    return true;
}

int32_t NT_MS_ABI nt_TerminateProcess(uintptr_t process, uint32_t code)
{
    if (process == UINTPTR_MAX && worker_status_fd >= 0)
        finish_worker(code);
    nt_SetLastError(process == UINTPTR_MAX ? NT_ERROR_ACCESS_DENIED : NT_ERROR_INVALID_HANDLE);
    return 0;
}

void NT_MS_ABI nt_exit(int code)
{
    if (worker_status_fd >= 0) {
        nt_cexit();
        finish_worker((uint32_t)code);
    }
    errno = EPERM;
}

void NT_MS_ABI nt_quick_exit(int code)
{
    if (worker_status_fd >= 0)
        finish_worker((uint32_t)code);
    errno = EPERM;
}

void NT_MS_ABI nt_terminate(void)
{
    nt_quick_exit(3);
}
