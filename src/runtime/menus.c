/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "compat.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

enum {
    MAX_MENUS = 128,
    MAX_ITEMS = 256,
    MAX_LABEL = 4096,
    MF_GRAYED = 0x01,
    MF_DISABLED = 0x02,
    MF_CHECKED = 0x08,
    MF_POPUP = 0x10,
    MF_MENUBARBREAK = 0x20,
    MF_MENUBREAK = 0x40,
    MF_BYPOSITION = 0x400,
    MF_SEPARATOR = 0x800
};

struct menu;

struct menu_item {
    uint32_t id;
    uint32_t flags;
    struct menu *child;
    char *label;
};

struct menu {
    uintptr_t handle;
    struct menu *parent;
    struct menu_item *items;
    size_t count, capacity;
};

static pthread_mutex_t menu_lock = PTHREAD_MUTEX_INITIALIZER;
static struct menu menus[MAX_MENUS];
static uintptr_t next_menu = 0x23000003;

static struct menu *find_menu(uintptr_t handle)
{
    if (handle != 0) {
        for (size_t i = 0; i < MAX_MENUS; ++i) {
            if (menus[i].handle == handle)
                return &menus[i];
        }
    }
    return NULL;
}

uintptr_t NT_MS_ABI nt_CreateMenu(void)
{
    pthread_mutex_lock(&menu_lock);
    if (next_menu <= UINTPTR_MAX - 16) {
        for (size_t i = 0; i < MAX_MENUS; ++i) {
            if (menus[i].handle == 0) {
                const uintptr_t handle = next_menu;
                next_menu += 16;
                menus[i] = (struct menu){.handle = handle};
                pthread_mutex_unlock(&menu_lock);
                return handle;
            }
        }
    }
    pthread_mutex_unlock(&menu_lock);
    nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
    return 0;
}

static uint32_t append_item(struct menu *menu, uint32_t flags, uintptr_t value,
                             const char *label)
{
    if (menu == NULL)
        return NT_ERROR_INVALID_HANDLE;
    if (menu->count == MAX_ITEMS)
        return NT_ERROR_NOT_ENOUGH_MEMORY;
    struct menu *child = NULL;
    if ((flags & MF_POPUP) != 0) {
        child = find_menu(value);
        if (child == NULL)
            return NT_ERROR_INVALID_HANDLE;
        if (child->parent != NULL)
            return NT_ERROR_ACCESS_DENIED;
        for (struct menu *ancestor = menu; ancestor != NULL; ancestor = ancestor->parent) {
            if (child == ancestor)
                return NT_ERROR_INVALID_PARAMETER;
        }
    }
    char *copy = NULL;
    if ((flags & MF_SEPARATOR) == 0) {
        if (label == NULL || strnlen(label, MAX_LABEL) == MAX_LABEL)
            return NT_ERROR_INVALID_PARAMETER;
        copy = strdup(label);
        if (copy == NULL)
            return NT_ERROR_NOT_ENOUGH_MEMORY;
    }
    if (menu->count == menu->capacity) {
        const size_t capacity = menu->capacity == 0 ? 8 : menu->capacity * 2;
        struct menu_item *items = realloc(menu->items, capacity * sizeof(*items));
        if (items == NULL) {
            free(copy);
            return NT_ERROR_NOT_ENOUGH_MEMORY;
        }
        menu->items = items;
        menu->capacity = capacity;
    }
    menu->items[menu->count++] = (struct menu_item){(uint32_t)value, flags, child, copy};
    if (child != NULL)
        child->parent = menu;
    return NT_ERROR_SUCCESS;
}

int32_t NT_MS_ABI nt_AppendMenuA(uintptr_t handle, uint32_t flags, uintptr_t value,
                                 const char *label)
{
    const uint32_t supported = MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_POPUP |
                               MF_MENUBARBREAK | MF_MENUBREAK | MF_SEPARATOR;
    if ((flags & ~supported) != 0) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if ((flags & (MF_POPUP | MF_SEPARATOR)) == (MF_POPUP | MF_SEPARATOR) ||
        ((flags & (MF_POPUP | MF_SEPARATOR)) == 0 && value > UINT32_MAX)) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return 0;
    }
    pthread_mutex_lock(&menu_lock);
    const uint32_t error = append_item(find_menu(handle), flags, value, label);
    pthread_mutex_unlock(&menu_lock);
    if (error != NT_ERROR_SUCCESS)
        nt_SetLastError(error);
    return error == NT_ERROR_SUCCESS;
}

static void release_menu(struct menu *menu)
{
    for (size_t i = 0; i < menu->count; ++i) {
        struct menu_item *item = &menu->items[i];
        if (item->child != NULL)
            release_menu(item->child);
        free(item->label);
    }
    free(menu->items);
    *menu = (struct menu){0};
}

int32_t NT_MS_ABI nt_DestroyMenu(uintptr_t handle)
{
    pthread_mutex_lock(&menu_lock);
    struct menu *menu = find_menu(handle);
    if (menu == NULL || menu->parent != NULL) {
        const uint32_t error = menu == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_ACCESS_DENIED;
        pthread_mutex_unlock(&menu_lock);
        nt_SetLastError(error);
        return 0;
    }
    release_menu(menu);
    pthread_mutex_unlock(&menu_lock);
    return 1;
}

int32_t NT_MS_ABI nt_GetMenuItemCount(uintptr_t handle)
{
    pthread_mutex_lock(&menu_lock);
    const struct menu *menu = find_menu(handle);
    const int32_t count = menu != NULL ? (int32_t)menu->count : -1;
    pthread_mutex_unlock(&menu_lock);
    if (count < 0)
        nt_SetLastError(NT_ERROR_INVALID_HANDLE);
    return count;
}

static struct menu_item *find_command(struct menu *menu, uint32_t id)
{
    for (size_t i = 0; i < menu->count; ++i) {
        struct menu_item *item = &menu->items[i];
        if (item->child != NULL) {
            struct menu_item *nested = find_command(item->child, id);
            if (nested != NULL)
                return nested;
        } else if ((item->flags & MF_SEPARATOR) == 0 && item->id == id) {
            return item;
        }
    }
    return NULL;
}

static int32_t change_state(uintptr_t handle, uint32_t id, uint32_t flags, uint32_t mask)
{
    if ((flags & ~(mask | MF_BYPOSITION)) != 0) {
        nt_SetLastError(NT_ERROR_INVALID_PARAMETER);
        return -1;
    }
    pthread_mutex_lock(&menu_lock);
    struct menu *menu = find_menu(handle);
    struct menu_item *item = NULL;
    if (menu != NULL) {
        if ((flags & MF_BYPOSITION) != 0)
            item = id < menu->count ? &menu->items[id] : NULL;
        else
            item = find_command(menu, id);
    }
    int32_t previous = -1;
    if (item != NULL && (item->flags & MF_SEPARATOR) == 0) {
        previous = (int32_t)(item->flags & mask);
        item->flags = (item->flags & ~mask) | (flags & mask);
    }
    pthread_mutex_unlock(&menu_lock);
    if (previous < 0)
        nt_SetLastError(menu == NULL ? NT_ERROR_INVALID_HANDLE : NT_ERROR_INVALID_PARAMETER);
    return previous;
}

uint32_t NT_MS_ABI nt_CheckMenuItem(uintptr_t handle, uint32_t id, uint32_t flags)
{
    return (uint32_t)change_state(handle, id, flags, MF_CHECKED);
}

int32_t NT_MS_ABI nt_EnableMenuItem(uintptr_t handle, uint32_t id, uint32_t flags)
{
    return change_state(handle, id, flags, MF_GRAYED | MF_DISABLED);
}
