/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_MENU_VIEW_H
#define NTNATIVE_MENU_VIEW_H

#include "compat.h"
#include <SDL.h>

enum {
    NT_MENU_HEIGHT = 24, NT_MENU_ROW = 22, NT_MENU_DISABLED = 3, NT_MENU_CHECKED = 8,
    NT_MENU_SEPARATOR = 0x800, NT_MENU_DEPTH = 16,
    NT_COLOR_MENU = 4, NT_COLOR_WINDOW = 5, NT_COLOR_MENUTEXT = 7,
    NT_COLOR_HIGHLIGHT = 13, NT_COLOR_HIGHLIGHTTEXT = 14, NT_COLOR_GRAYTEXT = 17
};

struct nt_menu_view_item {
    uintptr_t submenu;
    uint32_t flags, id;
    char label[256];
};

struct nt_menu_popup {
    uintptr_t menu;
    int x, y;
    uint32_t first;
};

bool nt_runtime_attach_menu(uintptr_t menu, uintptr_t window);
void nt_runtime_detach_menu(uintptr_t menu, uintptr_t window, bool destroy);
bool nt_runtime_menu_item(uintptr_t menu, uint32_t index, struct nt_menu_view_item *item);
bool nt_runtime_menu_bounds(uintptr_t menu, const struct nt_menu_popup *popup, int width, int height,
                            uint32_t index, struct nt_rect *rect, struct nt_menu_view_item *item);
bool nt_runtime_draw_menu(SDL_Surface *surface, uintptr_t menu,
                          const struct nt_menu_popup *popups, size_t count);
uint32_t NT_MS_ABI nt_GetSysColor(int32_t index);

#endif
