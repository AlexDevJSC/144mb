/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_DISPLAY_CONFIG_H
#define NTNATIVE_DISPLAY_CONFIG_H
#include "compat.h"

/* DISPLAYCONFIG's Win32 AMD64 layout, defined here without Windows headers.
 * Check the sizes against the MinGW-w64 types in the PE fixture. */
struct nt_display_luid {
    uint32_t low;
    int32_t high;
};
struct nt_display_rational {
    uint32_t numerator, denominator;
};
struct nt_display_region {
    uint32_t width, height;
};
struct nt_display_signal {
    uint64_t pixel_rate;
    struct nt_display_rational hsync, vsync;
    struct nt_display_region active, total;
    uint32_t video_standard, scanline;
};
struct nt_display_source_mode {
    uint32_t width, height, pixel_format;
    int32_t x, y;
};
struct nt_display_mode {
    uint32_t type, id;
    struct nt_display_luid adapter;
    union {
        struct nt_display_signal target;
        struct nt_display_source_mode source;
    } info;
};
struct nt_display_path_source {
    struct nt_display_luid adapter;
    uint32_t id, mode_index, status;
};
struct nt_display_path_target {
    struct nt_display_luid adapter;
    uint32_t id, mode_index, technology, rotation, scaling;
    struct nt_display_rational refresh;
    uint32_t scanline, available, status;
};
struct nt_display_path {
    struct nt_display_path_source source;
    struct nt_display_path_target target;
    uint32_t flags;
};
struct nt_display_device_header {
    uint32_t type, size;
    struct nt_display_luid adapter;
    uint32_t id;
};
struct nt_display_source_name {
    struct nt_display_device_header header;
    uint16_t name[32];
};
struct nt_display_target_name {
    struct nt_display_device_header header;
    uint32_t flags, technology;
    uint16_t edid_manufacturer, edid_product;
    uint32_t connector_instance;
    uint16_t friendly_name[64], device_path[128];
};

uint32_t NT_MS_ABI nt_GetDisplayConfigBufferSizes(uint32_t flags, uint32_t *paths,
                                                   uint32_t *modes);
uint32_t NT_MS_ABI nt_QueryDisplayConfig(uint32_t flags, uint32_t *paths,
                                         struct nt_display_path *path_array,
                                         uint32_t *modes, struct nt_display_mode *mode_array,
                                         uint32_t *topology);
uint32_t NT_MS_ABI nt_DisplayConfigGetDeviceInfo(struct nt_display_device_header *info);
#endif
