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

static void check_graphics(void)
{
    BOUND("gdi32.dll", "CreateFontA", nt_CreateFontA);
    BOUND("gdi32.dll", "CreateFontIndirectA", nt_CreateFontIndirectA);
    BOUND("gdi32.dll", "Ellipse", nt_Ellipse);
    BOUND("gdi32.dll", "ExtTextOutW", nt_ExtTextOutW);
    BOUND("gdi32.dll", "GetDeviceCaps", nt_GetDeviceCaps);
    BOUND("gdi32.dll", "GetTextExtentPoint32A", nt_GetTextExtentPoint32A);
    BOUND("gdi32.dll", "GetTextExtentPoint32W", nt_GetTextExtentPoint32W);
    BOUND("gdi32.dll", "GetTextMetricsA", nt_GetTextMetricsA);
    BOUND("gdi32.dll", "Polygon", nt_Polygon);
    BOUND("gdi32.dll", "Polyline", nt_Polyline);
    enum { RED = 0x0000ff, WHITE = 0xffffff };
    const uintptr_t dc = nt_runtime_create_surface(120, 80);
    CHECK(dc != 0 && nt_GetDeviceCaps(dc, 8) == 120 && nt_GetDeviceCaps(dc, 10) == 80 &&
          nt_GetDeviceCaps(dc, 12) == 32 && nt_GetDeviceCaps(dc, 88) == 96);
    const uintptr_t brush = nt_CreateSolidBrush(RED);
    const uintptr_t pen = nt_CreatePen(0, 1, WHITE);
    const uintptr_t old_brush = nt_SelectObject(dc, brush), old_pen = nt_SelectObject(dc, pen);
    CHECK(brush != 0 && pen != 0 && old_brush != 0 && old_pen != 0);
    CHECK(nt_Ellipse(dc, 2, 2, 18, 18) && nt_runtime_read_pixel(dc, 10, 10) == RED &&
          nt_runtime_read_pixel(dc, 18, 18) == 0);
    const struct nt_point triangle[] = {{20, 2}, {36, 2}, {26, 18}};
    const struct nt_point line[] = {{40, 2}, {56, 2}, {56, 20}};
    CHECK(nt_Polygon(dc, triangle, 3) && nt_runtime_read_pixel(dc, 26, 6) == RED);
    CHECK(nt_Polyline(dc, line, 3) && nt_runtime_read_pixel(dc, 46, 2) == WHITE &&
          nt_runtime_read_pixel(dc, 56, 10) == WHITE && nt_runtime_read_pixel(dc, 45, 10) == 0);
    CHECK(!nt_Polygon(dc, triangle, 2) && last_error() == NT_ERROR_INVALID_PARAMETER);

    const uintptr_t bold = nt_CreateFontA(-18, 0, 0, 0, 700, 1, 0, 0, 0, 0, 0, 0, 0,
                                         "DejaVu Sans");
    const struct nt_logfont definition = {.height = -20, .weight = 400, .face = "DejaVu Sans"};
    const uintptr_t normal = nt_CreateFontIndirectA(&definition);
    CHECK(bold != 0 && normal != 0 && nt_SelectObject(dc, bold) == nt_GetStockObject(13));
    struct nt_text_metrics metrics;
    CHECK(nt_GetTextMetricsA(dc, &metrics) && metrics.weight == 700 && metrics.italic &&
          metrics.ascent > 0 && metrics.height == metrics.ascent + metrics.descent);
    CHECK(!nt_DeleteObject(bold) && last_error() == NT_ERROR_ACCESS_DENIED);
    CHECK(nt_SelectObject(dc, normal) == bold && nt_DeleteObject(bold));
    const uint16_t wide[] = {'C', 'a', 'f', 0xe9};
    struct nt_size narrow_size, wide_size;
    CHECK(nt_GetTextExtentPoint32A(dc, "Caf\xe9", 4, &narrow_size) &&
          nt_GetTextExtentPoint32W(dc, wide, 4, &wide_size) &&
          narrow_size.width > 0 && narrow_size.width == wide_size.width &&
          narrow_size.height == wide_size.height);
    const struct nt_rect text = {1, 30, 30, 62};
    CHECK(nt_ExtTextOutW(dc, 2, 31, 2 | 4, &text, wide, 4, NULL));
    unsigned int black = 0, white = 0;
    for (int y = text.top; y < text.bottom; ++y) {
        for (int x = text.left; x < text.right; ++x) {
            const uint32_t pixel = nt_runtime_read_pixel(dc, x, y);
            black += pixel == 0;
            white += pixel == WHITE;
        }
    }
    CHECK(black > 0 && white > black && nt_runtime_read_pixel(dc, text.right, 45) == 0);
    CHECK(!nt_ExtTextOutW(dc, 0, 0, 0x10, NULL, wide, 4, NULL) &&
          last_error() == NT_ERROR_NOT_SUPPORTED);
    CHECK(nt_DeleteDC(dc) && nt_DeleteObject(normal) && nt_DeleteObject(brush) && nt_DeleteObject(pen));
    puts("PASS Cairo fonts, ANSI/UTF-16 metrics, clipped glyphs and GDI shape pixels");
}

int main(void)
{
    check_file_handles();
    check_startup();
    check_exit();
    check_graphics();
    return EXIT_SUCCESS;
}
