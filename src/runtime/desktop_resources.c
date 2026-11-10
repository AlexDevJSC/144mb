/* SPDX-License-Identifier: MIT */
#include "desktop.h"
#include "desktop_resources.h"
#include "../pe/pe_bytes.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_ICONS = 64, MAX_RESOURCES = 4 * 1024 * 1024, ICON_SIZE = 32 };
#define DIRECTORY_BIT UINT32_C(0x80000000)

static struct {
    uintptr_t module;
    unsigned char *bytes;
    uint32_t rva;
    size_t size;
} resources;

static struct {
    uintptr_t handle, module;
    uint16_t id;
    SDL_Surface *surface;
} icons[MAX_ICONS];
static uintptr_t next_icon = 0x2a00000a;

static struct {
    uint16_t id;
    SDL_SystemCursor shape;
    SDL_Cursor *cursor;
} cursors[] = {
    {32512, SDL_SYSTEM_CURSOR_ARROW, NULL},
    {32513, SDL_SYSTEM_CURSOR_IBEAM, NULL},
    {32514, SDL_SYSTEM_CURSOR_WAIT, NULL},
    {32515, SDL_SYSTEM_CURSOR_CROSSHAIR, NULL},
    {32642, SDL_SYSTEM_CURSOR_SIZENWSE, NULL},
    {32643, SDL_SYSTEM_CURSOR_SIZENESW, NULL},
    {32644, SDL_SYSTEM_CURSOR_SIZEWE, NULL},
    {32645, SDL_SYSTEM_CURSOR_SIZENS, NULL},
    {32646, SDL_SYSTEM_CURSOR_SIZEALL, NULL},
    {32648, SDL_SYSTEM_CURSOR_NO, NULL},
    {32649, SDL_SYSTEM_CURSOR_HAND, NULL},
    {32650, SDL_SYSTEM_CURSOR_WAITARROW, NULL}
};

bool nt_runtime_set_icon_resources(uintptr_t module, uint32_t rva, const void *section, size_t size)
{
    if (!nt_runtime_desktop_active() || size > MAX_RESOURCES ||
        (size != 0 && (module == 0 || rva == 0 || section == NULL || size < 16 ||
                       size > UINT32_MAX - rva)))
        return false;
    unsigned char *copy = NULL;
    if (size != 0) {
        copy = malloc(size);
        if (copy == NULL)
            return false;
        memcpy(copy, section, size);
    }
    free(resources.bytes);
    resources.bytes = copy;
    resources.module = size != 0 ? module : 0;
    resources.rva = rva;
    resources.size = size;
    /* Existing shared handles remain valid. Retire their lookup keys. */
    for (size_t i = 0; i < MAX_ICONS; ++i) {
        if (icons[i].module != 0)
            icons[i].module = UINTPTR_MAX;
    }
    return true;
}

static const unsigned char *resource_bytes(uint32_t offset, size_t length)
{
    return resources.bytes != NULL && contains(resources.size, offset, length) ?
           resources.bytes + offset : NULL;
}

static uint32_t resource_entry(uint32_t offset, uint16_t id, bool language)
{
    const unsigned char *header = resource_bytes(offset, 16);
    if (header == NULL)
        return UINT32_MAX;
    const size_t names = read_u16(header + 12), count = read_u16(header + 14);
    const unsigned char *entries = resource_bytes(offset + 16, (names + count) * 8);
    if (entries == NULL)
        return UINT32_MAX;
    uint32_t fallback = UINT32_MAX;
    for (size_t i = names; i < names + count; ++i) {
        const uint32_t name = read_u32(entries + i * 8);
        const uint32_t next = read_u32(entries + i * 8 + 4);
        if (name == id)
            return next;
        if (language && (fallback == UINT32_MAX || name == 0))
            fallback = next;
    }
    return fallback;
}

static const unsigned char *find_resource(uint16_t type, uint16_t id, size_t *size)
{
    const uint32_t type_entry = resource_entry(0, type, false);
    if (type_entry == UINT32_MAX || (type_entry & DIRECTORY_BIT) == 0)
        return NULL;
    const uint32_t id_entry = resource_entry(type_entry & ~DIRECTORY_BIT, id, false);
    if (id_entry == UINT32_MAX || (id_entry & DIRECTORY_BIT) == 0)
        return NULL;
    const uint32_t language = resource_entry(id_entry & ~DIRECTORY_BIT, 0x409, true);
    if ((language & DIRECTORY_BIT) != 0)
        return NULL;
    const unsigned char *data = resource_bytes(language, 16);
    if (data == NULL || read_u32(data) < resources.rva)
        return NULL;
    *size = read_u32(data + 4);
    return resource_bytes(read_u32(data) - resources.rva, *size);
}

static SDL_Surface *decode_icon(const unsigned char *dib, size_t size)
{
    if (size < 40)
        return NULL;
    const uint32_t header = read_u32(dib);
    const uint32_t width = read_u32(dib + 4), doubled_height = read_u32(dib + 8);
    const uint32_t bits = read_u16(dib + 14);
    if (header < 40 || header > size || width == 0 || width > 256 ||
        doubled_height == 0 || doubled_height > 512 || doubled_height % 2 != 0 ||
        read_u16(dib + 12) != 1 || read_u32(dib + 16) != 0 ||
        (bits != 1 && bits != 4 && bits != 8 && bits != 24 && bits != 32))
        return NULL;
    const uint32_t height = doubled_height / 2;
    uint32_t palette = read_u32(dib + 32);
    if (bits <= 8 && palette == 0)
        palette = 1u << bits;
    if (palette > 256 || (bits <= 8 && palette > (1u << bits)))
        return NULL;
    const size_t color_row = ((width * bits + 31) / 32) * 4;
    const size_t mask_row = ((width + 31) / 32) * 4;
    const size_t offset = (size_t)header + palette * 4;
    if (!contains(size, offset, (color_row + mask_row) * height))
        return NULL;
    const unsigned char *pixels = dib + offset, *mask = pixels + color_row * height;
    bool has_alpha = false;
    if (bits == 32) {
        for (size_t i = 3; i < color_row * height; i += 4)
            has_alpha |= pixels[i] != 0;
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, (int)width, (int)height, 32,
                                                           SDL_PIXELFORMAT_RGBA32);
    if (surface == NULL)
        return NULL;
    for (uint32_t y = 0; y < height; ++y) {
        const size_t row = height - y - 1;
        const unsigned char *source = pixels + row * color_row;
        const unsigned char *alpha = mask + row * mask_row;
        unsigned char *target = (unsigned char *)surface->pixels + y * (size_t)surface->pitch;
        for (uint32_t x = 0; x < width; ++x, target += 4) {
            const unsigned char *color;
            if (bits <= 8) {
                const uint32_t bit = x * bits;
                const uint32_t index = source[bit / 8] >> (8 - bits - bit % 8) & ((1u << bits) - 1);
                if (index >= palette) {
                    SDL_FreeSurface(surface);
                    return NULL;
                }
                color = dib + header + index * 4;
            } else {
                color = source + x * (bits / 8);
            }
            target[0] = color[2];
            target[1] = color[1];
            target[2] = color[0];
            target[3] = has_alpha ? color[3] : (alpha[x / 8] & (0x80u >> (x % 8))) != 0 ? 0 : 255;
        }
    }
    return surface;
}

static SDL_Surface *module_icon(uint16_t id)
{
    size_t group_size = 0;
    const unsigned char *group = find_resource(14, id, &group_size); /* RT_GROUP_ICON */
    if (group == NULL || group_size < 6 || read_u16(group) != 0 || read_u16(group + 2) != 1)
        return NULL;
    const size_t count = read_u16(group + 4);
    if (count == 0 || count > 256 || !contains(group_size, 6, count * 14))
        return NULL;
    SDL_Surface *best = NULL;
    int best_score = INT_MAX;
    for (size_t i = 0; i < count; ++i) {
        const unsigned char *entry = group + 6 + i * 14;
        const int width = entry[0] != 0 ? entry[0] : 256;
        const int height = entry[1] != 0 ? entry[1] : 256;
        const int score = (abs(width - ICON_SIZE) + abs(height - ICON_SIZE)) * 65536 -
                          (int)read_u16(entry + 6);
        if (score >= best_score)
            continue;
        size_t size = 0;
        const unsigned char *data = find_resource(3, read_u16(entry + 12), &size); /* RT_ICON */
        SDL_Surface *candidate = data != NULL && read_u32(entry + 8) == size ?
                                 decode_icon(data, size) : NULL;
        if (candidate != NULL) {
            if (best != NULL)
                SDL_FreeSurface(best);
            best = candidate;
            best_score = score;
        }
    }
    return best;
}

static SDL_Surface *system_icon(uint16_t id)
{
    if (id < 32512 || id > 32516)
        return NULL;
    static const uint8_t shapes[][7] = {
        {31, 17, 31, 17, 17, 17, 31}, /* window */
        {17, 17, 10, 4, 10, 17, 17}, /* error */
        {14, 17, 1, 2, 4, 0, 4},   /* question */
        {4, 4, 4, 4, 4, 0, 4},      /* warning */
        {4, 0, 12, 4, 4, 4, 14}     /* information */
    };
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, ICON_SIZE, ICON_SIZE, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface == NULL)
        return NULL;
    const uint32_t border = SDL_MapRGBA(surface->format, 34, 52, 68, 255);
    const uint32_t center = SDL_MapRGBA(surface->format, id == 32513 || id == 32515 ? 163 : 43,
                                      id == 32513 ? 45 : 107, 155, 255);
    const uint32_t white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);
    SDL_FillRect(surface, NULL, 0);
    SDL_Rect frame = {1, 1, 30, 30};
    SDL_FillRect(surface, &frame, border);
    SDL_Rect inner = {3, 3, 26, 26};
    SDL_FillRect(surface, &inner, center);
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 5; ++x) {
            if ((shapes[id - 32512][y] & (16 >> x)) != 0) {
                SDL_Rect pixel = {11 + x * 2, 9 + y * 2, 2, 2};
                SDL_FillRect(surface, &pixel, white);
            }
        }
    }
    return surface;
}

SDL_Surface *nt_runtime_icon(uintptr_t handle)
{
    if (nt_runtime_desktop_active() && handle != 0) {
        for (size_t i = 0; i < MAX_ICONS; ++i) {
            if (icons[i].handle == handle)
                return icons[i].surface;
        }
    }
    return NULL;
}

uintptr_t NT_MS_ABI nt_LoadIconA(uintptr_t instance, const char *name)
{
    const uintptr_t id = (uintptr_t)name;
    if (!nt_runtime_desktop_active() || id == 0 || id > UINT16_MAX || instance == UINTPTR_MAX) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    size_t free_slot = MAX_ICONS;
    for (size_t i = 0; i < MAX_ICONS; ++i) {
        if (icons[i].handle == 0 && free_slot == MAX_ICONS)
            free_slot = i;
        if (icons[i].handle != 0 && icons[i].module == instance && icons[i].id == id)
            return icons[i].handle;
    }
    if (free_slot == MAX_ICONS || next_icon > UINTPTR_MAX - 16) {
        nt_SetLastError(NT_ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    SDL_Surface *surface = instance == 0 ? system_icon((uint16_t)id) :
                           instance == resources.module ? module_icon((uint16_t)id) : NULL;
    if (surface == NULL) {
        nt_SetLastError(1814); /* ERROR_RESOURCE_NAME_NOT_FOUND */
        return 0;
    }
    icons[free_slot].handle = next_icon;
    icons[free_slot].module = instance;
    icons[free_slot].id = (uint16_t)id;
    icons[free_slot].surface = surface;
    next_icon += 16;
    return icons[free_slot].handle;
}

static size_t cursor_index(uintptr_t id)
{
    for (size_t i = 0; i < sizeof(cursors) / sizeof(cursors[0]); ++i) {
        if (cursors[i].id == id)
            return i;
    }
    return sizeof(cursors) / sizeof(cursors[0]);
}

SDL_Cursor *nt_runtime_cursor(uintptr_t handle)
{
    const size_t i = cursor_index(handle);
    return i < sizeof(cursors) / sizeof(cursors[0]) ? cursors[i].cursor : NULL;
}

uintptr_t NT_MS_ABI nt_LoadCursorA(uintptr_t instance, const char *name)
{
    const uintptr_t id = (uintptr_t)name;
    const size_t i = cursor_index(id);
    if (!nt_runtime_desktop_active() || instance != 0 || i == sizeof(cursors) / sizeof(cursors[0])) {
        nt_SetLastError(NT_ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (cursors[i].cursor == NULL)
        cursors[i].cursor = SDL_CreateSystemCursor(cursors[i].shape);
    if (cursors[i].cursor == NULL) {
        nt_SetLastError(NT_ERROR_GEN_FAILURE);
        return 0;
    }
    return id;
}

void nt_runtime_close_desktop_resources(void)
{
    SDL_SetCursor(SDL_GetDefaultCursor());
    for (size_t i = 0; i < sizeof(cursors) / sizeof(cursors[0]); ++i) {
        if (cursors[i].cursor != NULL)
            SDL_FreeCursor(cursors[i].cursor);
        cursors[i].cursor = NULL;
    }
    for (size_t i = 0; i < MAX_ICONS; ++i) {
        if (icons[i].surface != NULL)
            SDL_FreeSurface(icons[i].surface);
    }
    memset(icons, 0, sizeof(icons));
    nt_runtime_set_icon_resources(0, 0, NULL, 0);
}
