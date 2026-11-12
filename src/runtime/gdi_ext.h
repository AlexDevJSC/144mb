/* SPDX-License-Identifier: MIT */
#ifndef NTNATIVE_GDI_EXT_H
#define NTNATIVE_GDI_EXT_H
#include "compat.h"

struct nt_bitmap_info_header {
    uint32_t size;
    int32_t width, height;
    uint16_t planes, bits;
    uint32_t compression, image_size;
    int32_t x_pixels_per_meter, y_pixels_per_meter;
    uint32_t used_colors, important_colors;
};
uintptr_t NT_MS_ABI nt_CreateDIBSection(uintptr_t dc, const struct nt_bitmap_info_header *info,
                                        uint32_t usage, void **pixels, uintptr_t section, uint32_t offset);
int32_t NT_MS_ABI nt_GdiFlush(void);
uintptr_t NT_MS_ABI nt_CreateFontW(int32_t height, int32_t width, int32_t escapement, int32_t orientation,
                                  int32_t weight, uint32_t italic, uint32_t underline, uint32_t strike,
                                  uint32_t charset, uint32_t output_precision, uint32_t clip_precision,
                                  uint32_t quality, uint32_t pitch, const uint16_t *face);
int32_t NT_MS_ABI nt_FillRect(uintptr_t dc, const struct nt_rect *rect, uintptr_t brush);
int32_t NT_MS_ABI nt_DrawTextW(uintptr_t dc, uint16_t *text, int32_t count, struct nt_rect *rect, uint32_t flags);
int32_t NT_MS_ABI nt_AlphaBlend(uintptr_t destination, int32_t x, int32_t y, int32_t width, int32_t height,
                                uintptr_t source, int32_t sx, int32_t sy, int32_t sw, int32_t sh, uint32_t blend);
#endif
