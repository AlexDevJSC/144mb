/* SPDX-License-Identifier: MIT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "console.h"
#include "dialogs.h"
#include "winbase.h"
#include "windesktop.h"
#include "winfile.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

enum { CONSOLE_LOG = 100, CONSOLE_INPUT, CONSOLE_TIMER = 1 };
static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uintptr_t window_handle;
static bool attached;
static int master_fd = -1, stop_fd = -1;
static pthread_t reader;
static dev_t slave_device;
static char line[2048];
static size_t line_length;
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char output[65536];
static size_t output_length;

static void *read_console_output(void *unused)
{
    (void)unused;
    struct pollfd fds[] = {{master_fd, POLLIN, 0}, {stop_fd, POLLIN, 0}};
    unsigned char bytes[4096];
    for (;;) {
        int ready = poll(fds, 2, -1);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0 || fds[1].revents || !(fds[0].revents & POLLIN))
            return NULL;
        ssize_t count = read(master_fd, bytes, sizeof(bytes));
        if (count < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (count <= 0)
            return NULL;
        pthread_mutex_lock(&output_lock);
        if (output_length + (size_t)count > sizeof(output)) {
            size_t discard = output_length + (size_t)count - sizeof(output);
            output_length -= discard;
            memmove(output, output + discard, output_length);
        }
        memcpy(output + output_length, bytes, (size_t)count);
        output_length += (size_t)count;
        pthread_mutex_unlock(&output_lock);
    }
}

static void write_line(uintptr_t window)
{
    line[line_length] = 0;
    intptr_t row = nt_SendDlgItemMessageA(window, CONSOLE_LOG, NT_LB_ADDSTRING, 0, (intptr_t)line);
    if (row < 0) {
        nt_SendDlgItemMessageA(window, CONSOLE_LOG, NT_LB_RESETCONTENT, 0, 0);
        row = nt_SendDlgItemMessageA(window, CONSOLE_LOG, NT_LB_ADDSTRING, 0, (intptr_t)line);
    }
    if (row >= 0)
        nt_SendDlgItemMessageA(window, CONSOLE_LOG, NT_LB_SETCURSEL, (uintptr_t)row, 0);
    line_length = 0;
}
static void drain_console(uintptr_t window)
{
    unsigned char bytes[4096];
    for (unsigned int batch = 0; batch < 8; ++batch) {
        pthread_mutex_lock(&output_lock);
        size_t count = output_length < sizeof(bytes) ? output_length : sizeof(bytes);
        memcpy(bytes, output, count);
        output_length -= count;
        memmove(output, output + count, output_length);
        pthread_mutex_unlock(&output_lock);
        if (!count)
            break;
        for (size_t i = 0; i < count; ++i) {
            unsigned char byte = bytes[i];
            if (byte == '\n') {
                write_line(window);
            } else if ((byte >= 0x20 && byte != 0x7f) || byte == '\t') {
                if (line_length == sizeof(line) - 1)
                    write_line(window);
                line[line_length++] = (char)byte;
            }
        }
    }
}

static void reset_console(void)
{
    if (stop_fd >= 0) {
        const uint64_t stop = 1;
        ssize_t written;
        do {
            written = write(stop_fd, &stop, sizeof(stop));
        } while (written < 0 && errno == EINTR);
        pthread_join(reader, NULL);
        close(stop_fd);
        stop_fd = -1;
    }
    if (master_fd >= 0)
        close(master_fd);
    master_fd = -1;
    line_length = 0;
    output_length = 0;
    slave_device = 0;
    attached = false;
    atomic_store(&window_handle, 0);
}

static intptr_t NT_MS_ABI console_proc(uintptr_t window, uint32_t message,
                                      uintptr_t wparam, intptr_t lparam)
{
    (void)lparam;
    if (message == NT_WM_TIMER) {
        drain_console(window);
        return 1;
    }
    if (message == NT_WM_COMMAND && (uint16_t)wparam == NT_IDOK) {
        char input[1025];
        size_t length = nt_GetDlgItemTextA(window, CONSOLE_INPUT, input, (int32_t)sizeof(input) - 1);
        input[length++] = '\n';
        if (write(master_fd, input, length) == (ssize_t)length)
            nt_SendDlgItemMessageA(window, CONSOLE_INPUT, NT_WM_SETTEXT, 0, (intptr_t)"");
        return 1;
    }
    if (message == NT_WM_CLOSE) {
        nt_DestroyWindow(window);
        return 1;
    }
    if (message == NT_WM_NCDESTROY && window == atomic_load(&window_handle)) {
        pthread_mutex_lock(&console_lock);
        reset_console();
        nt_runtime_set_standard_streams(STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO);
        pthread_mutex_unlock(&console_lock);
    }
    return 0;
}

uintptr_t NT_MS_ABI nt_GetConsoleWindow(void)
{
    return atomic_load(&window_handle);
}
int32_t NT_MS_ABI nt_AttachConsole(uint32_t process)
{
    if (process != UINT32_MAX && process != (uint32_t)getppid()) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&console_lock);
    if (attached) {
        pthread_mutex_unlock(&console_lock);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    uint32_t mode;
    attached = nt_GetConsoleMode(nt_GetStdHandle((uint32_t)-10), &mode) ||
               nt_GetConsoleMode(nt_GetStdHandle((uint32_t)-11), &mode) ||
               nt_GetConsoleMode(nt_GetStdHandle((uint32_t)-12), &mode);
    bool ok = attached;
    pthread_mutex_unlock(&console_lock);
    if (!ok)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return ok;
}

int32_t NT_MS_ABI nt_AllocConsole(void)
{
    if (!nt_runtime_desktop_active()) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    pthread_mutex_lock(&console_lock);
    if (attached) {
        pthread_mutex_unlock(&console_lock);
        nt_SetLastError(NT_ERROR_ACCESS_DENIED);
        return 0;
    }
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    int slave = master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0 ?
        ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
    struct stat info;
    uintptr_t window = 0;
    if (slave >= 0 && fstat(slave, &info) == 0)
        window = nt_runtime_create_dialog("Console", 0, 650, 425, console_proc, 0);
    master_fd = master;
    if (window) {
        stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (stop_fd >= 0 && pthread_create(&reader, NULL, read_console_output, NULL) != 0) {
            close(stop_fd);
            stop_fd = -1;
        }
    }
    bool ok = window && stop_fd >= 0 &&
        nt_runtime_add_dialog_control(window, "LISTBOX", "", CONSOLE_LOG, 0, 12, 12, 626, 350) &&
        nt_runtime_add_dialog_control(window, "EDIT", "", CONSOLE_INPUT, NT_WS_TABSTOP, 12, 376, 532, 30) &&
        nt_runtime_add_dialog_control(window, "BUTTON", "Enter", NT_IDOK, NT_WS_TABSTOP | 1u, 554, 376, 84, 30) &&
        nt_SetTimer(window, CONSOLE_TIMER, 30, NULL) &&
        nt_runtime_set_standard_streams(slave, slave, slave);
    if (slave >= 0)
        close(slave);
    if (ok) {
        slave_device = info.st_rdev;
        attached = true;
        atomic_store(&window_handle, window);
    } else {
        reset_console();
    }
    pthread_mutex_unlock(&console_lock);
    if (!ok) {
        if (window)
            nt_DestroyWindow(window);
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    nt_runtime_focus_control(window, nt_runtime_dialog_item(window, CONSOLE_INPUT), false);
    nt_ShowWindow(window, 5);
    return 1;
}

int32_t NT_MS_ABI nt_FreeConsole(void)
{
    pthread_mutex_lock(&console_lock);
    uintptr_t window = atomic_load(&window_handle);
    if (!window)
        attached = false;
    pthread_mutex_unlock(&console_lock);
    if (window && !nt_runtime_desktop_active()) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    return !window || nt_DestroyWindow(window);
}
void nt_runtime_close_console(void)
{
    pthread_mutex_lock(&console_lock);
    reset_console();
    pthread_mutex_unlock(&console_lock);
}
bool nt_runtime_console_title(const char *title)
{
    uintptr_t window = atomic_load(&window_handle);
    return window && nt_runtime_desktop_active() && nt_SetWindowTextA(window, title);
}
bool nt_runtime_console_wait(int fd)
{
    uintptr_t window = atomic_load(&window_handle);
    struct stat info;
    if (!window || !nt_runtime_desktop_active() || fstat(fd, &info) != 0 || info.st_rdev != slave_device)
        return true;
    struct pollfd readable = {fd, POLLIN, 0};
    for (;;) {
        int ready = poll(&readable, 1, 25);
        if (ready > 0)
            return true;
        if (ready < 0 && errno != EINTR)
            return false;
        struct nt_message message;
        while (nt_PeekMessageW(&message, window, 0, 0, 0) > 0 && message.message != 0x12) {
            if (nt_PeekMessageW(&message, window, 0, 0, 1) <= 0)
                break;
            if (!nt_IsDialogMessageA(window, &message)) {
                nt_TranslateMessage(&message);
                nt_DispatchMessageW(&message);
            }
            if (window != atomic_load(&window_handle)) {
                errno = EIO;
                return false;
            }
        }
    }
}
