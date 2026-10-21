#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H
/* Eye 0.5.2 PR #12, copied verbatim for integration regression until upstream merges.
 * Test-only file: production Eye remains owned by Eye team. */
#include <stddef.h>
#include <stdint.h>
#define EYE_DISPLAY_WIDTH 320u
#define EYE_DISPLAY_HEIGHT 200u
#define EYE_DISPLAY_PIXELS (EYE_DISPLAY_WIDTH * EYE_DISPLAY_HEIGHT)
#define EYE_DISPLAY_CLEAR_RGB UINT32_C(0x15202d)
typedef enum { EYE_DISPLAY_GAME = 0, EYE_DISPLAY_MENU_DIM_SCENE = 1, EYE_DISPLAY_MENU_GENERATED = 2 } EyeDisplayMode;
typedef struct {
    uint64_t hash;
    uint32_t black_rgb, clear_rgb, other_rgb, nonopaque, total;
    uint8_t all_black, clear_only;
} EyeFrameStats;
int eye_display_scan(const uint32_t *pixels, size_t pixel_count, EyeFrameStats *out);
int eye_display_prepare(uint32_t *dst,size_t dst_pixels,const uint32_t *scene,size_t scene_pixels,EyeDisplayMode mode,unsigned dim);
#endif
