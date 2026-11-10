/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_DESKTOP_RESOURCES_H
#define NTNATIVE_DESKTOP_RESOURCES_H

#include "compat.h"
#include <SDL.h>

/* The launcher supplies a bounded copy of the guest .rsrc section. */
bool nt_runtime_set_icon_resources(uintptr_t module, uint32_t section_rva,
                                   const void *section, size_t size);
SDL_Surface *nt_runtime_icon(uintptr_t handle);
SDL_Cursor *nt_runtime_cursor(uintptr_t handle);
void nt_runtime_close_desktop_resources(void);
uintptr_t NT_MS_ABI nt_LoadCursorA(uintptr_t instance, const char *name);
uintptr_t NT_MS_ABI nt_LoadIconA(uintptr_t instance, const char *name);

#endif
