/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_SHELL_H
#define NT_RUNTIME_SHELL_H

#include "ntnative/runtime.h"
#include <stdint.h>

int32_t NT_MS_ABI nt_SHParseDisplayName(const uint16_t *name, void *context, void **pidl,
                                      uint32_t requested, uint32_t *attributes);
int32_t NT_MS_ABI nt_SHOpenFolderAndSelectItems(const void *folder, uint32_t count,
                                              const void *const *items, uint32_t flags);
uintptr_t NT_MS_ABI nt_ShellExecuteW(uintptr_t owner, const uint16_t *operation,
        const uint16_t *file, const uint16_t *parameters, const uint16_t *directory, int32_t show);

#endif
