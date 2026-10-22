#ifndef EYE_MODKIT_EXT_H
#define EYE_MODKIT_EXT_H
/* Static app-to-Eye helpers. Not additions to Bones ModKit ABI1. */
#include "eye5.h"
#include "eye_display.h"
#include <stdint.h>
#include <stddef.h>
int bones_eye_mod_roof_preview(const Eye5RoofSpec *spec,float x,float y,float z,unsigned material_id);
int bones_eye_mod_roof_texture_preview(int enabled);
const uint32_t *bones_eye_mod_pixels(void);
uint32_t bones_eye_mod_triangles(void);
/* Build a 320x200 opaque UI base in application-owned memory. The caller
 * subsequently invokes bones_skin_ui_compose(dst,320,200,320). */
int bones_eye_mod_prepare_base(uint32_t *dst,size_t capacity,
                               EyeDisplayMode mode,unsigned dim);
/* Stats of the most recent EYE-rendered (pre-Skin, pre-GDI) frame. */
int bones_eye_mod_frame_stats(EyeFrameStats *out,uint64_t *frame_serial);
#endif
