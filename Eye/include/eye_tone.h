#ifndef EYE_TONE_H
#define EYE_TONE_H

#include <stddef.h>
#include <stdint.h>

/* Optional display-only grade. Does not modify EYE1/EYE2 bytes or pixels
   unless explicitly requested by the caller. No global state. */
typedef enum {
    EYE_TONE_ORIGINAL = 0,
    EYE_TONE_AGED = 1
} EyeTone;

/* Packed pixel format: 0xFFRRGGBB. Deterministic, integer-only. */
uint32_t eye_tone_pixel(uint32_t pixel, EyeTone tone);
int eye_tone_apply(uint32_t *pixels, size_t count, EyeTone tone);

#endif
