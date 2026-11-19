/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char memory[66];
    void *(* volatile fill)(void *, int, size_t) = memset;
    for (size_t i = 0; i < sizeof(memory); ++i)
        memory[i] = 0x6b;
    if (fill(memory + 1, 0x1a7, 64) != memory + 1)
        return 1;
    if (memory[0] != 0x6b || memory[65] != 0x6b)
        return 2;
    for (size_t i = 1; i < 65; ++i)
        if (memory[i] != 0xa7)
            return 3;
    if (fill(memory + 12, 0, 0) != memory + 12 ||
        memory[12] != 0xa7)
        return 4;
    if (fill(memory + 2, 0, 63) != memory + 2 ||
        memory[1] != 0xa7 || memory[65] != 0x6b)
        return 5;
    for (size_t i = 2; i < 65; ++i)
        if (memory[i] != 0)
            return 6;
    puts("PASS freestanding fixture memset: return, cast, bounds, zero length");
    return 0;
}
