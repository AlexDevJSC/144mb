/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "winbase.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

enum { COMMAND_LINE_MAX = 32767, MAX_ARGUMENTS = 4096 };

static struct {
    uintptr_t module;
    const char *path, *line;
    uint16_t *wide_path, *wide_line;
    size_t image_size, tls_callback_count;
    struct nt_guest_tls tls;
    const unsigned char *tls_callbacks;
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
    uint16_t *wide_path = module != 0 ? nt_runtime_utf8_to_wide(path) : NULL;
    uint16_t *wide_line = module != 0 ? nt_runtime_utf8_to_wide(command_line) : NULL;
    if (module != 0 && (wide_path == NULL || wide_line == NULL)) {
        free(wide_path);
        free(wide_line);
        return false;
    }
    clear_argv();
    free(guest.wide_path);
    free(guest.wide_line);
    guest.module = module;
    guest.path = module != 0 ? path : NULL;
    guest.line = module != 0 ? command_line : NULL;
    guest.wide_path = wide_path;
    guest.wide_line = wide_line;
    guest.image_size = 0;
    guest.tls = (struct nt_guest_tls){0};
    guest.tls_callbacks = NULL;
    guest.tls_callback_count = 0;
    return true;
}

void nt_runtime_set_guest_image_extent(size_t size) { guest.image_size = size; }

void nt_runtime_set_guest_tls(const struct nt_guest_tls *initial, const unsigned char *callbacks, size_t count)
{
    guest.tls = *initial;
    guest.tls_callbacks = callbacks;
    guest.tls_callback_count = count;
}

const struct nt_guest_tls *nt_runtime_guest_tls(void) { return &guest.tls; }

void nt_runtime_notify_tls(uint32_t reason)
{
    typedef void (NT_MS_ABI *tls_callback)(void *, uint32_t, void *);
    for (size_t i = 0; i < guest.tls_callback_count; ++i) {
        tls_callback callback;
        memcpy(&callback, guest.tls_callbacks + i * sizeof(callback), sizeof(callback));
        callback((void *)guest.module, reason, NULL);
    }
}

uintptr_t nt_runtime_guest_image_at(uintptr_t address)
{
    return guest.module != 0 && address >= guest.module && address - guest.module < guest.image_size ?
           guest.module : 0;
}

char *NT_MS_ABI nt_GetCommandLineA(void) { return (char *)guest.line; }
uint16_t *NT_MS_ABI nt_GetCommandLineW(void) { return guest.wide_line; }

uint32_t NT_MS_ABI nt_GetModuleFileNameW(uintptr_t module, uint16_t *buffer, uint32_t size)
{
    if (guest.module == 0 || (module != 0 && module != guest.module)) {
        nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
        return 0;
    }
    if (size == 0 || buffer == NULL) {
        nt_SetLastError(size == 0 ? NT_ERROR_INSUFFICIENT_BUFFER : NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    size_t length = nt_runtime_wide_length(guest.wide_path, COMMAND_LINE_MAX);
    const bool truncated = length >= size;
    if (truncated)
        length = size - 1;
    memcpy(buffer, guest.wide_path, length * sizeof(*buffer));
    buffer[length] = 0;
    if (truncated)
        nt_SetLastError(NT_ERROR_INSUFFICIENT_BUFFER);
    return truncated ? size : (uint32_t)length;
}

uintptr_t NT_MS_ABI nt_RtlPcToFileHeader(const void *address, uintptr_t *image)
{
    const uintptr_t base = nt_runtime_guest_image_at((uintptr_t)address);
    if (image != NULL)
        *image = base;
    return base;
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

void NT_MS_ABI nt_ExitProcess(uint32_t code)
{
    (void)nt_TerminateProcess(UINTPTR_MAX, code);
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
    if (worker_status_fd >= 0) {
        nt_c_exit();
        finish_worker((uint32_t)code);
    }
    errno = EPERM;
}

void NT_MS_ABI nt_terminate(void)
{
    nt_quick_exit(3);
}

uint16_t **NT_MS_ABI nt_CommandLineToArgvW(const uint16_t *command, int32_t *count)
{
    if (command == NULL || count == NULL) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const bool empty = *command == 0;
    if (empty)
        command = guest.wide_path;
    if (command == NULL) {
        nt_SetLastError(NT_ERROR_MOD_NOT_FOUND);
        return NULL;
    }
    const size_t length = nt_runtime_wide_length(command, COMMAND_LINE_MAX);
    if (length == COMMAND_LINE_MAX) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return NULL;
    }
    const size_t pointers = length + 2;
    uint16_t **argv = (uint16_t **)nt_LocalAlloc(0, pointers * sizeof(*argv) +
                                                   (length + 2) * sizeof(**argv));
    if (argv == NULL)
        return NULL;
    uint16_t *out = (uint16_t *)(argv + pointers);
    const uint16_t *in = command;
    int32_t argc = 0;
    argv[argc++] = out;
    if (empty) {
        memcpy(out, command, (length + 1) * sizeof(*out));
    } else {
        const bool program_quoted = *in == '"';
        if (program_quoted)
            ++in;
        while (*in != 0 && (program_quoted ? *in != '"' : *in != ' ' && *in != '\t'))
            *out++ = *in++;
        if (program_quoted && *in == '"')
            ++in;
        *out++ = 0;
        while (*in != 0) {
            while (*in == ' ' || *in == '\t')
                ++in;
            if (*in == 0)
                break;
            argv[argc++] = out;
            bool quoted = false;
            while (*in != 0 && (quoted || (*in != ' ' && *in != '\t'))) {
                size_t slashes = 0;
                while (*in == '\\') {
                    ++slashes;
                    ++in;
                }
                if (*in == '"') {
                    for (size_t i = 0; i < slashes / 2; ++i)
                        *out++ = '\\';
                    ++in;
                    if (slashes % 2 != 0) {
                        *out++ = '"';
                    } else if (quoted && *in == '"') {
                        *out++ = *in++;
                    } else {
                        quoted = !quoted;
                    }
                } else {
                    for (size_t i = 0; i < slashes; ++i)
                        *out++ = '\\';
                    if (*in == 0 || (!quoted && (*in == ' ' || *in == '\t')))
                        break;
                    *out++ = *in++;
                }
            }
            *out++ = 0;
        }
    }
    argv[argc] = NULL;
    *count = argc;
    return argv;
}

static _Thread_local uint32_t com_initializations, com_mode;

int32_t NT_MS_ABI nt_CoInitializeEx(void *reserved, uint32_t mode)
{
    if (reserved != NULL || (mode & ~UINT32_C(0x0e)) != 0)
        return (int32_t)UINT32_C(0x80070057);
    const uint32_t apartment = mode & 2;
    if (com_initializations != 0 && com_mode != apartment)
        return (int32_t)UINT32_C(0x80010106);
    if (com_initializations == UINT32_MAX)
        return (int32_t)UINT32_C(0x80004005);
    com_mode = apartment;
    return com_initializations++ == 0 ? 0 : 1;
}

void NT_MS_ABI nt_CoUninitialize(void)
{
    if (com_initializations != 0)
        --com_initializations;
}

static pthread_mutex_t protection_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t region_protection(uintptr_t address, uintptr_t end)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps == NULL)
        return 0;
    uint32_t protection = 0;
    uintptr_t cursor = address;
    char line[4096];
    while (cursor < end && fgets(line, sizeof(line), maps) != NULL) {
        unsigned long start, limit;
        char mode[5];
        if (sscanf(line, "%lx-%lx %4s", &start, &limit, mode) != 3 || limit <= cursor)
            continue;
        if (start > cursor)
            break;
        if (protection == 0)
            protection = mode[2] == 'x' ? (mode[0] == 'r' ? 0x20 : 0x10) :
                         mode[1] == 'w' ? 4 : mode[0] == 'r' ? 2 : 1;
        cursor = limit;
    }
    fclose(maps);
    return cursor >= end ? protection : 0;
}

int32_t NT_MS_ABI nt_VirtualProtect(void *address, size_t size, uint32_t protection, uint32_t *previous)
{
    const uintptr_t start = (uintptr_t)address;
    if (size == 0 || previous == NULL || size - 1 > UINTPTR_MAX - start ||
        nt_runtime_guest_image_at(start) == 0 || nt_runtime_guest_image_at(start + size - 1) == 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    int native;
    switch (protection) {
    case 1: native = PROT_NONE; break;
    case 2: native = PROT_READ; break;
    case 4: native = PROT_READ | PROT_WRITE; break;
    case 0x10: native = PROT_EXEC; break;
    case 0x20: native = PROT_EXEC | PROT_READ; break;
    default:
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    const uintptr_t aligned = start - start % (uintptr_t)page;
    const uintptr_t last = start + size - 1;
    if (nt_runtime_guest_image_at(aligned) == 0 ||
        last - last % (uintptr_t)page > UINTPTR_MAX - (uintptr_t)page) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    const uintptr_t end = last - last % (uintptr_t)page + (uintptr_t)page;
    if (nt_runtime_guest_image_at(end - 1) == 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&protection_lock);
    const uint32_t old = region_protection(aligned, end);
    const bool ok = old != 0 && mprotect((void *)aligned, end - aligned, native) == 0;
    pthread_mutex_unlock(&protection_lock);
    if (ok)
        *previous = old;
    else
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
    return ok;
}

uint64_t NT_MS_ABI nt_VerSetConditionMask(uint64_t mask, uint32_t fields, uint8_t condition)
{
    if (condition > 7)
        return mask;
    for (unsigned int i = 0; i < 8; ++i) {
        if ((fields & (1u << i)) != 0)
            mask = (mask & ~(UINT64_C(7) << (3 * i))) | (uint64_t)condition << (3 * i);
    }
    return mask;
}

static bool compare_version(uint32_t current, uint32_t required, uint32_t condition)
{
    switch (condition) {
    case 1: return current == required;
    case 2: return current > required;
    case 3: return current >= required;
    case 4: return current < required;
    case 5: return current <= required;
    case 6: return (current & required) == required;
    case 7: return (current & required) != 0;
    default: return false;
    }
}

int32_t NT_MS_ABI nt_VerifyVersionInfoW(const struct nt_os_version_info *version,
                                       uint32_t fields, uint64_t mask)
{
    _Static_assert(sizeof(struct nt_os_version_info) == 284, "OSVERSIONINFOEXW layout");
    if (version == NULL || version->size != sizeof(*version) || fields == 0 || (fields & ~255u) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    /* Report the Win32 compatibility profile, independently of the host kernel version. */
    const uint32_t current[] = {0, 10, 19045, 2, 0, 0, 0x100, 1};
    const uint32_t required[] = {version->minor, version->major, version->build, version->platform,
                                 version->sp_minor, version->sp_major, version->suite, version->product};
    const unsigned int order[] = {1, 0, 5, 4, 2, 3, 6, 7};
    bool version_decided = false;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
        const unsigned int bit = order[i];
        if ((fields & (1u << bit)) == 0)
            continue;
        const uint32_t condition = (uint32_t)(mask >> (3 * bit)) & 7;
        if ((bit == 6 && condition < 6) || (bit != 6 && (condition == 0 || condition > 5))) {
            nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (i < 4) {
            if (version_decided || (current[bit] == required[bit] && condition != 1))
                continue;
            version_decided = current[bit] != required[bit];
        }
        if (!compare_version(current[bit], required[bit], condition)) {
            nt_SetLastError(1150); /* ERROR_OLD_WIN_VERSION */
            return 0;
        }
    }
    /* Strict comparisons also fail when all requested hierarchy fields are equal. */
    if (!version_decided) {
        for (size_t i = 0; i < 4; ++i) {
            const unsigned int bit = order[i], condition = (unsigned int)(mask >> (3 * bit)) & 7;
            if ((fields & (1u << bit)) != 0 && (condition == 2 || condition == 4)) {
                nt_SetLastError(1150);
                return 0;
            }
        }
    }
    return 1;
}
