#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H
/* Eye framebuffer presentation helpers. No dependency on Bones ABI, GDI or Skin.
 * All pixel data: 0xAARRGGBB, straight alpha (Skin composites onto opaque output).
 * Virtual resolution: 320x200. Skin owns ALL UI drawing and input logic. */
#include <stddef.h>
#include <stdint.h>
#define EYE_DISPLAY_WIDTH 320u
#define EYE_DISPLAY_HEIGHT 200u
#define EYE_DISPLAY_PIXELS (EYE_DISPLAY_WIDTH * EYE_DISPLAY_HEIGHT)
#define EYE_DISPLAY_CLEAR_RGB UINT32_C(0x15202d)

typedef enum {
    EYE_DISPLAY_GAME = 0,
    EYE_DISPLAY_MENU_DIM_SCENE = 1,
    EYE_DISPLAY_MENU_GENERATED = 2
} EyeDisplayMode;

typedef struct {
    uint64_t hash;
    uint32_t black_rgb;
    uint32_t clear_rgb;
    uint32_t other_rgb;
    uint32_t nonopaque;
    uint32_t total;
    uint8_t all_black;  /* EVERY pixel has RGB=0; ignores alpha. */
    uint8_t clear_only; /* EVERY pixel equals Eye's RGB clear colour. */
} EyeFrameStats;

/* Distinguishes a genuinely black Eye framebuffer from GDI presentation.
 * 'clear_only' is intentionally strict; crosshair may leave 16 non-clear
 * pixels even when no geometry survives clipping. Inspect other_rgb too. */
int eye_display_scan(const uint32_t *pixels, size_t pixel_count, EyeFrameStats *out);

/* Compose the BACKGROUND ONLY into caller-owned buffer. Do NOT call Skin here.
 * GAME: copy scene unchanged; MENU_DIM_SCENE: copy scene and darken it.
 * MENU_GENERATED: procedural decorative background, no stored image, no UI.
 * Dim: [0..255] amount of black, 0=no dimming, 255=black. Applied only
 * in MENU_DIM_SCENE. May be called repeatedly; no state/texture resets.
 * Destination MUST hold 320x200 uint32_t; source may be NULL only for
 * MENU_GENERATED. All outputs opaque. */
int eye_display_prepare(uint32_t *dst, size_t dst_pixels,
                        const uint32_t *scene, size_t scene_pixels,
                        EyeDisplayMode mode, unsigned dim);
#endif
