/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KERNEL "kernel32.dll"
#define CRT(group) "api-ms-win-crt-" group "-l1-1-0.dll"
#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "FAIL line %d %s\n", __LINE__, #test); \
        exit(EXIT_FAILURE); \
    } \
} while (0)
#define BOUND(module, name, function) \
    CHECK(nt_runtime_resolve(module, name) == (nt_guest_proc)(function))

static uint32_t last_error(void)
{
    typedef uint32_t (NT_MS_ABI *get_error)(void);
    return ((get_error)nt_runtime_resolve(KERNEL, "GetLastError"))();
}

static void check_file_handles(void)
{
    BOUND(KERNEL, "CreateFileA", nt_CreateFileA);
    BOUND(KERNEL, "CloseHandle", nt_CloseHandle);
    BOUND(CRT("stdio"), "_open_osfhandle", nt_open_osfhandle);
    BOUND(CRT("stdio"), "_close", nt_close);
    BOUND(CRT("math"), "_fdopen", nt_fdopen);
    const uint32_t read = UINT32_C(0x80000000), write = UINT32_C(0x40000000);
    char root[] = "/tmp/nt-mines-services.XXXXXX";
    CHECK(mkdtemp(root) != NULL && nt_runtime_set_file_root(root));
    CHECK(nt_CreateFileA("missing\\scores", read, 1, NULL, 3, 0, 0) == UINTPTR_MAX &&
          last_error() == NT_ERROR_PATH_NOT_FOUND);
    CHECK(nt_CreateFileA("C:\\..\\outside", read, 1, NULL, 3, 0, 0) == UINTPTR_MAX &&
          last_error() == NT_ERROR_ACCESS_DENIED);
    const uintptr_t created = nt_CreateFileA("C:\\scores", write, 0, NULL, 1, 0x80, 0);
    CHECK(created != UINTPTR_MAX);
    CHECK(nt_CreateFileA("scores", write, 3, NULL, 1, 0, 0) == UINTPTR_MAX &&
          last_error() == NT_ERROR_FILE_EXISTS);
    CHECK(nt_CreateFileA("scores", read, 3, NULL, 3, 0, 0) == UINTPTR_MAX &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    CHECK(nt_DeleteFileA("scores") == 0 && last_error() == NT_ERROR_SHARING_VIOLATION);
    CHECK(nt_fopen("scores", "rb") == NULL && errno == EACCES);
    CHECK(nt_open_osfhandle((intptr_t)created, 0xc000) == -1 && errno == EINVAL);
    const int fd = nt_open_osfhandle((intptr_t)created, 0x4000);
    CHECK(fd >= 3 && !nt_CloseHandle(created) && last_error() == NT_ERROR_INVALID_HANDLE);
    CHECK(nt_fdopen(fd, "r") == NULL && errno == EINVAL);
    void *stream = nt_fdopen(fd, "w");
    CHECK(stream != NULL && nt_fwrite("42\n", 1, 3, stream) == 3);
    CHECK(nt_close(fd) == -1 && errno == EBADF);
    CHECK(nt_MoveFileExA("scores", "other", 0) == 0 &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    CHECK(nt_fclose(stream) == 0);

    const uintptr_t existing = nt_CreateFileA("scores", read, 1, NULL, 4, 0, 0);
    CHECK(existing != UINTPTR_MAX && last_error() == NT_ERROR_ALREADY_EXISTS);
    CHECK(nt_CreateFileA("scores", write, 3, NULL, 2, 0, 0) == UINTPTR_MAX &&
          last_error() == NT_ERROR_SHARING_VIOLATION);
    const int raw = nt_open_osfhandle((intptr_t)existing, 0x8000);
    stream = nt_fdopen(raw, "r");
    char bytes[8] = {0};
    CHECK(stream != NULL && nt_fread(bytes, 1, sizeof(bytes), stream) == 4);
    CHECK(memcmp(bytes, "42\r\n", 4) == 0 && nt_fclose(stream) == 0);

    const uintptr_t truncated = nt_CreateFileA("scores", read | write, 3, NULL, 5, 0, 0);
    CHECK(truncated != UINTPTR_MAX);
    CHECK(nt_close(nt_open_osfhandle((intptr_t)truncated, 0)) == 0);
    stream = nt_fopen("scores", "rb");
    CHECK(stream != NULL && nt_fread(bytes, 1, 1, stream) == 0 && nt_fclose(stream) == 0);
    CHECK(nt_DeleteFileA("scores") && nt_runtime_set_file_root(NULL) && rmdir(root) == 0);
    puts("PASS shared Win32 files and ownership from HANDLE to CRT fd to FILE");
}

static void check_startup(void)
{
    BOUND(KERNEL, "GetModuleFileNameA", nt_GetModuleFileNameA);
    BOUND(CRT("runtime"), "_configure_narrow_argv", nt_configure_narrow_argv);
    BOUND(CRT("runtime"), "_get_narrow_winmain_command_line", nt_get_narrow_winmain_command_line);
    BOUND(CRT("runtime"), "_initialize_narrow_environment", nt_initialize_narrow_environment);

    const uintptr_t image = 0x140000000;
    const char path[] = "C:\\Games and tests\\fixture.exe";
    const char cmd[] = "\"C:\\Games and tests\\fixture.exe\" \t --one \"two words\" \"\" a\\\"b";
    CHECK(nt_runtime_set_guest_image(image, path, cmd) && nt_GetModuleHandleW(NULL) == image);
    char buffer[64];
    CHECK(nt_GetModuleFileNameA(0, buffer, sizeof(buffer)) == strlen(path) && strcmp(buffer, path) == 0);
    CHECK(nt_GetModuleFileNameA(image, buffer, 5) == 5 && buffer[4] == '\0' &&
          last_error() == NT_ERROR_INSUFFICIENT_BUFFER);
    CHECK(nt_GetModuleFileNameA(image + 1, buffer, sizeof(buffer)) == 0 &&
          last_error() == NT_ERROR_MOD_NOT_FOUND);
    CHECK(strcmp(nt_get_narrow_winmain_command_line(), "--one \"two words\" \"\" a\\\"b") == 0);
    CHECK(nt_configure_narrow_argv(1) == 0);
    int count;
    const char *const *args = nt_runtime_guest_argv(&count);
    CHECK(count == 5 && strcmp(args[0], path) == 0 && strcmp(args[1], "--one") == 0 &&
          strcmp(args[2], "two words") == 0 && args[3][0] == '\0' &&
          strcmp(args[4], "a\"b") == 0 && args[5] == NULL);
    CHECK(nt_configure_narrow_argv(2) == -1 && errno == ENOTSUP);
    CHECK(nt_configure_narrow_argv(0) == 0 && nt_runtime_guest_argv(&count) == NULL && count == 0);
    CHECK(nt_runtime_set_guest_image(0, NULL, NULL));

    char value[] = "PROFILE=fixture";
    const char *const environment[] = {value, NULL};
    nt_runtime_set_guest_environment(environment);
    CHECK(nt_initialize_narrow_environment() == 0 && nt_initialize_narrow_environment() == 0);
    value[8] = 'x';
    CHECK(strcmp(nt_getenv("profile"), "fixture") == 0);
    nt_runtime_set_guest_environment(NULL);
    puts("PASS guest module name, command line, argument parsing and copied environment");
}

static int marker_fd = -1;
static void NT_MS_ABI exit_marker(void)
{
    if (write(marker_fd, "x", 1) != 1)
        nt_quick_exit(5);
}

static int NT_MS_ABI normal_exit(void)
{
    if (nt_crt_atexit(exit_marker) != 0)
        return 1;
    nt_exit(23);
    return 2;
}

static int NT_MS_ABI fixture_quick_exit(void)
{
    if (nt_crt_atexit(exit_marker) != 0)
        return 1;
    nt_quick_exit(24);
    return 2;
}

static int NT_MS_ABI terminate_process(void)
{
    nt_TerminateProcess(nt_GetCurrentProcess(), UINT32_C(0x82345678));
    return 2;
}

static int NT_MS_ABI crt_terminate(void)
{
    nt_terminate();
    return 2;
}

static void check_exit(void)
{
    BOUND(KERNEL, "TerminateProcess", nt_TerminateProcess);
    BOUND(CRT("runtime"), "exit", nt_exit);
    BOUND(CRT("runtime"), "_exit", nt_quick_exit);
    BOUND(CRT("runtime"), "terminate", nt_terminate);
    CHECK(!nt_TerminateProcess(nt_GetCurrentProcess(), 9) && last_error() == NT_ERROR_ACCESS_DENIED);
    int channel[2];
    CHECK(pipe(channel) == 0);
    marker_fd = channel[1];
    uint32_t code = 0;
    CHECK(nt_runtime_run_fixture(normal_exit, &code) && code == 23);
    CHECK(nt_runtime_run_fixture(fixture_quick_exit, &code) && code == 24);
    CHECK(nt_runtime_run_fixture(terminate_process, &code) && code == UINT32_C(0x82345678));
    CHECK(nt_runtime_run_fixture(crt_terminate, &code) && code == 3);
    CHECK(close(channel[1]) == 0);
    char bytes[8] = {0};
    CHECK(read(channel[0], bytes, sizeof(bytes)) == 1 && bytes[0] == 'x' && close(channel[0]) == 0);
    puts("PASS guest termination, callback order and full 32-bit status from Linux child");
}

int main(void)
{
    check_file_handles();
    check_startup();
    check_exit();
    return EXIT_SUCCESS;
}
