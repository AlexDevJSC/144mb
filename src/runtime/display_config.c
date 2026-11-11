/* SPDX-License-Identifier: MIT */
#include "display_config.h"
#include "desktop.h"

#include <SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    NT_QDC_ALL_PATHS = 1, NT_QDC_ONLY_ACTIVE_PATHS = 2,
    NT_QDC_DATABASE_CURRENT = 4,
    NT_PATH_ACTIVE = 1, NT_SOURCE_IN_USE = 1,
    NT_TARGET_IN_USE = 1, NT_OUTPUT_OTHER = UINT32_MAX,
    NT_ROTATION_IDENTITY = 1, NT_SCALING_IDENTITY = 1,
    NT_SCAN_PROGRESSIVE = 1, NT_PIXEL_32BPP = 4,
    NT_MODE_SOURCE = 1, NT_MODE_TARGET = 2,
    NT_DEVICE_SOURCE_NAME = 1, NT_DEVICE_TARGET_NAME = 2,
    NT_MAX_DISPLAYS = 64,
    NT_WIN_ERROR_INVALID_PARAMETER = 87, NT_WIN_ERROR_INSUFFICIENT_BUFFER = 122,
    NT_WIN_ERROR_NOT_SUPPORTED = 50, NT_WIN_ERROR_GEN_FAILURE = 31
};

_Static_assert(sizeof(struct nt_display_luid) == 8, "Windows LUID");
_Static_assert(sizeof(struct nt_display_signal) == 48, "DISPLAYCONFIG_VIDEO_SIGNAL_INFO");
_Static_assert(sizeof(struct nt_display_source_mode) == 20, "DISPLAYCONFIG_SOURCE_MODE");
_Static_assert(sizeof(struct nt_display_mode) == 64, "DISPLAYCONFIG_MODE_INFO");
_Static_assert(sizeof(struct nt_display_path_source) == 20, "DISPLAYCONFIG_PATH_SOURCE_INFO");
_Static_assert(sizeof(struct nt_display_path_target) == 48, "DISPLAYCONFIG_PATH_TARGET_INFO");
_Static_assert(sizeof(struct nt_display_path) == 72, "DISPLAYCONFIG_PATH_INFO");
_Static_assert(sizeof(struct nt_display_device_header) == 20, "DISPLAYCONFIG_DEVICE_INFO_HEADER");
_Static_assert(sizeof(struct nt_display_source_name) == 84, "DISPLAYCONFIG_SOURCE_DEVICE_NAME");
_Static_assert(sizeof(struct nt_display_target_name) == 420, "DISPLAYCONFIG_TARGET_DEVICE_NAME");

static uint32_t count_displays(uint32_t flags, uint32_t *count)
{
    if (flags != NT_QDC_ALL_PATHS && flags != NT_QDC_ONLY_ACTIVE_PATHS &&
        flags != NT_QDC_DATABASE_CURRENT)
        return NT_WIN_ERROR_INVALID_PARAMETER;
    if (!nt_runtime_desktop_active())
        return NT_WIN_ERROR_NOT_SUPPORTED;
    const int displays = SDL_GetNumVideoDisplays();
    if (displays < 0 || displays > NT_MAX_DISPLAYS)
        return NT_WIN_ERROR_GEN_FAILURE;
    *count = (uint32_t)displays;
    return 0;
}

static uint32_t display_geometry(uint32_t index, SDL_Rect *rect, SDL_DisplayMode *mode)
{
    if (SDL_GetDisplayBounds((int)index, rect) != 0 || rect->w <= 0 || rect->h <= 0)
        return NT_WIN_ERROR_GEN_FAILURE;
    *mode = (SDL_DisplayMode){0};
    if (SDL_GetCurrentDisplayMode((int)index, mode) != 0)
        *mode = (SDL_DisplayMode){0};
    return 0;
}

static void copy_utf16(uint16_t *destination, size_t capacity, const char *source)
{
    if (source == NULL || capacity == 0)
        return;
    uint16_t *wide = nt_runtime_utf8_to_wide(source);
    if (wide == NULL)
        return;
    size_t count = nt_runtime_wide_length(wide, capacity - 1);
    memcpy(destination, wide, count * sizeof(*wide));
    destination[count] = 0;
    free(wide);
}

uint32_t NT_MS_ABI nt_GetDisplayConfigBufferSizes(uint32_t flags, uint32_t *paths, uint32_t *modes)
{
    if (paths == NULL || modes == NULL)
        return NT_WIN_ERROR_INVALID_PARAMETER;
    uint32_t count = 0;
    const uint32_t status = count_displays(flags, &count);
    if (status != 0)
        return status;
    *paths = count;
    *modes = count * 2;
    return 0;
}

uint32_t NT_MS_ABI nt_QueryDisplayConfig(uint32_t flags, uint32_t *paths,
                                        struct nt_display_path *path_array,
                                        uint32_t *modes, struct nt_display_mode *mode_array,
                                        uint32_t *topology)
{
    if (paths == NULL || modes == NULL ||
        (flags == NT_QDC_DATABASE_CURRENT ? topology == NULL : topology != NULL))
        return NT_WIN_ERROR_INVALID_PARAMETER;
    uint32_t displays = 0;
    const uint32_t status = count_displays(flags, &displays);
    if (status != 0)
        return status;
    if (*paths < displays || *modes < displays * 2)
        return NT_WIN_ERROR_INSUFFICIENT_BUFFER;
    if ((displays != 0 && (path_array == NULL || mode_array == NULL)))
        return NT_WIN_ERROR_INVALID_PARAMETER;

    for (uint32_t i = 0; i < displays; ++i) {
        SDL_Rect rect;
        SDL_DisplayMode current;
        if (display_geometry(i, &rect, &current) != 0)
            return NT_WIN_ERROR_GEN_FAILURE;
        const uint32_t refresh = current.refresh_rate > 0 ? (uint32_t)current.refresh_rate : 0;
        struct nt_display_path path = {0};
        path.source.id = i;
        path.source.mode_index = i * 2;
        path.source.status = NT_SOURCE_IN_USE;
        path.target.id = i;
        path.target.mode_index = i * 2 + 1;
        path.target.technology = NT_OUTPUT_OTHER;
        path.target.rotation = NT_ROTATION_IDENTITY;
        path.target.scaling = NT_SCALING_IDENTITY;
        path.target.refresh = (struct nt_display_rational){refresh, 1};
        path.target.scanline = NT_SCAN_PROGRESSIVE;
        path.target.available = 1;
        path.target.status = NT_TARGET_IN_USE;
        path.flags = NT_PATH_ACTIVE;
        path_array[i] = path;

        struct nt_display_mode source = {0}, target = {0};
        source.type = NT_MODE_SOURCE;
        source.id = i;
        source.info.source = (struct nt_display_source_mode){
            (uint32_t)rect.w, (uint32_t)rect.h, NT_PIXEL_32BPP, rect.x, rect.y
        };
        target.type = NT_MODE_TARGET;
        target.id = i;
        target.info.target.vsync = (struct nt_display_rational){refresh, 1};
        target.info.target.active = (struct nt_display_region){
            (uint32_t)rect.w, (uint32_t)rect.h
        };
        target.info.target.total = target.info.target.active;
        target.info.target.scanline = NT_SCAN_PROGRESSIVE;
        /* Linux SDL does not expose the physical pixel clock or exact
         * blanking intervals. Never fabricate them. */
        mode_array[i * 2] = source;
        mode_array[i * 2 + 1] = target;
    }
    *paths = displays;
    *modes = displays * 2;
    if (topology != NULL)
        *topology = displays > 1 ? 4u : 1u;
    return 0;
}

uint32_t NT_MS_ABI nt_DisplayConfigGetDeviceInfo(struct nt_display_device_header *info)
{
    if (info == NULL)
        return NT_WIN_ERROR_INVALID_PARAMETER;
    const uint32_t type = info->type, size = info->size, id = info->id;
    if (info->adapter.low != 0 || info->adapter.high != 0)
        return NT_WIN_ERROR_INVALID_PARAMETER;
    const uint32_t expected = type == NT_DEVICE_SOURCE_NAME ?
        sizeof(struct nt_display_source_name) :
        type == NT_DEVICE_TARGET_NAME ? sizeof(struct nt_display_target_name) : 0;
    if (expected == 0)
        return NT_WIN_ERROR_NOT_SUPPORTED;
    if (size != expected)
        return NT_WIN_ERROR_INVALID_PARAMETER;
    uint32_t count = 0;
    const uint32_t result = count_displays(NT_QDC_ONLY_ACTIVE_PATHS, &count);
    if (result != 0)
        return result;
    if (id >= count)
        return NT_WIN_ERROR_INVALID_PARAMETER;
    if (type == NT_DEVICE_SOURCE_NAME) {
        struct nt_display_source_name *source = (void *)info;
        memset((unsigned char *)source + sizeof(source->header), 0,
               sizeof(*source) - sizeof(source->header));
        char device[32];
        snprintf(device, sizeof(device), "\\\\.\\DISPLAY%u", id + 1);
        copy_utf16(source->name, 32, device);
    } else {
        struct nt_display_target_name *target = (void *)info;
        memset((unsigned char *)target + sizeof(target->header), 0,
               sizeof(*target) - sizeof(target->header));
        target->technology = NT_OUTPUT_OTHER;
        copy_utf16(target->friendly_name, 64, SDL_GetDisplayName((int)id));
        /* No EDID manufacturer, physical device path or plug-in identity
         * is available from the portable SDL interface. */
    }
    return 0;
}
