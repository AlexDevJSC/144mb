#include "eye_tone.h"

static unsigned clamp_byte(int a) {
    return (unsigned)(a < 0 ? 0 : (a > 255 ? 255 : a));
}

uint32_t eye_tone_pixel(uint32_t rgb, EyeTone tone) {
    if (tone != EYE_TONE_AGED) return rgb;
    int r = (int)((rgb >> 16) & 255u);
    int g = (int)((rgb >>  8) & 255u);
    int b = (int)(rgb & 255u);
    /* ITU-inspired integer luminance, 3/8 desaturation. */
    int lum = (77 * r + 150 * g + 29 * b + 128) >> 8;
    r = (5 * r + 3 * lum + 4) >> 3;
    g = (5 * g + 3 * lum + 4) >> 3;
    b = (5 * b + 3 * lum + 4) >> 3;
    /* A subtle dusty brown-gray cast; keeps carved geometry visible. */
    r = ((7 * r + 112 + 4) >> 3) - 3;
    g = ((7 * g + 107 + 4) >> 3) - 3;
    b = ((7 * b +  99 + 4) >> 3) - 3;
    return (rgb & UINT32_C(0xff000000)) |
           (clamp_byte(r) << 16) | (clamp_byte(g) << 8) | clamp_byte(b);
}

int eye_tone_apply(uint32_t *pixels, size_t count, EyeTone tone) {
    size_t i;
    if (!pixels || (tone != EYE_TONE_ORIGINAL && tone != EYE_TONE_AGED)) return 0;
    if (tone == EYE_TONE_AGED)
        for (i = 0; i < count; ++i) pixels[i] = eye_tone_pixel(pixels[i], tone);
    return 1;
}
