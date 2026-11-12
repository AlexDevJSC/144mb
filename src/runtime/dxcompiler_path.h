/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_DXCOMPILER_PATH_H
#define NT_RUNTIME_DXCOMPILER_PATH_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define NT_DXC_INSTALL_DIRECTORY "../lib/nt-native-linux/dxc/lib"
#define NT_DXC_SANDBOX_DIRECTORY "/nt-native-dxc"

static inline bool nt_dxc_install_path(const char *executable, char *directory, size_t capacity)
{
    const char *slash = strrchr(executable, '/');
    if (!slash)
        return false;
    int length = snprintf(directory, capacity, "%.*s/" NT_DXC_INSTALL_DIRECTORY,
                          (int)(slash - executable), executable);
    return length >= 0 && (size_t)length < capacity;
}

#endif
