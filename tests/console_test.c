/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "compat.h"
#include "winfile.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    const int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        puts("SKIP console PTY regression: no pseudoterminal available");
        if (master >= 0)
            close(master);
        return 0;
    }
    const char *name = ptsname(master);
    const int slave = name != NULL ? open(name, O_RDWR | O_NOCTTY | O_CLOEXEC) : -1;
    if (slave < 0 || !nt_runtime_set_standard_streams(slave, slave, slave)) {
        fprintf(stderr, "PTY standard-handle preparation failed: %s\n", strerror(errno));
        close(master);
        if (slave >= 0)
            close(slave);
        return 1;
    }
    close(slave);
    uint32_t mode = 0;
    const uintptr_t input = nt_GetStdHandle((uint32_t)-10);
    if (!nt_GetConsoleMode(input, &mode) || (mode & 2u) == 0)
        return 2;
    if (!nt_SetConsoleTitleA("Native-Console-144"))
        return 3;
    struct pollfd ready = {.fd = master, .events = POLLIN};
    if (poll(&ready, 1, 500) <= 0)
        return 4;
    char title_bytes[128] = {0};
    const ssize_t title_size = read(master, title_bytes, sizeof(title_bytes));
    const char expected[] = "\x1b]0;Native-Console-144\x07";
    if (title_size != (ssize_t)(sizeof(expected) - 1) ||
        memcmp(title_bytes, expected, sizeof(expected) - 1) != 0)
        return 5;
    if (nt_SetConsoleTitleA("bad\x1b]8;;injected") != 0)
        return 6;

    const char utf8[] = "caf\xc3\xa9\n";
    if (write(master, utf8, sizeof(utf8) - 1) != (ssize_t)sizeof(utf8) - 1)
        return 7;
    uint16_t wide[16] = {0};
    uint32_t done = 0;
    if (!nt_ReadConsoleW(input, wide, 16, &done, NULL) || done != 5 ||
        wide[0] != 'c' || wide[1] != 'a' || wide[2] != 'f' ||
        wide[3] != 0x00e9 || wide[4] != '\n')
        return 8;
    if (nt_ReadConsoleW(UINTPTR_MAX, wide, 1, &done, NULL) != 0)
        return 9;
    if (nt_ReadConsoleW(input, wide, 1, &done, (void *)1) != 0)
        return 10;
    close(master);
    puts("PASS ReadConsoleW UTF-8, SetConsoleTitleA and error contracts over a Linux PTY");
    return 0;
}
