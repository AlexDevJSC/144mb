/* SPDX-License-Identifier: MIT */
/* Freestanding MinGW-w64 PE fixture support, NOT a Win32 CRT import.
 * Modern GCC can lower large aggregate zero initializers to memset even
 * with -ffreestanding. -nostdlib correctly leaves no external CRT to
 * satisfy that compiler-generated call. Link ONLY into controlled test PE.
 *
 * Compile this translation unit with -fno-builtin and
 * -fno-tree-loop-distribute-patterns: otherwise GCC may transform this
 * implementation into a recursive call to itself. */
#include <stddef.h>
void *memset(void *destination, int value, size_t length)
{
    unsigned char *bytes = destination;
    const unsigned char fill = (unsigned char)value;
    for (size_t i = 0; i < length; ++i)
        bytes[i] = fill;
    return destination;
}
