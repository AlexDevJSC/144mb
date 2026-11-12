/* SPDX-License-Identifier: MIT */
#ifndef NT_RUNTIME_CONSOLE_H
#define NT_RUNTIME_CONSOLE_H

#include "ntnative/runtime.h"
#include <stdint.h>

int32_t NT_MS_ABI nt_AllocConsole(void);
int32_t NT_MS_ABI nt_AttachConsole(uint32_t process);
int32_t NT_MS_ABI nt_FreeConsole(void);
uintptr_t NT_MS_ABI nt_GetConsoleWindow(void);
void nt_runtime_close_console(void);
bool nt_runtime_console_title(const char *title);
bool nt_runtime_console_wait(int fd);

#endif
