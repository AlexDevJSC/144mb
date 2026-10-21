#ifndef EYE_H
#define EYE_H

#include <stddef.h>
#include <stdint.h>

/* EYE1: portable material synthesizer for Proyecto 1.44. No heap or OS calls. */
#define EYE_SIZE 64u
#define EYE_RECIPE_BYTES 12u
#define EYE_HEADER_BYTES 12u
#define EYE_MAX_RECIPES 255u
#define EYE_PACK_MAX_BYTES (EYE_HEADER_BYTES + EYE_RECIPE_BYTES * EYE_MAX_RECIPES)
#define EYE_FAMILY_COUNT 8u

typedef enum {
    EYE_STONE = 0, EYE_BRICK = 1, EYE_WOOD = 2, EYE_DIRT = 3,
    EYE_METAL = 4, EYE_CLOTH = 5, EYE_FOLIAGE = 6, EYE_PLASTER = 7
} EyeFamily;

typedef struct {
    uint8_t family;       /* 0..7 */
    uint8_t palette;      /* 0..3, family-specific hues */
    uint8_t scale;        /* 1..4, pattern frequency */
    uint8_t pattern;      /* 0..7, stagger/geometry variation */
    uint8_t wear;         /* 0..15, erosion/chips */
    uint8_t growth;       /* 0..15, moss/rust/grime per family */
    uint8_t contrast;     /* 0..15, strength of lighting/noise */
    uint8_t flags;        /* must be 0 for format version 1 */
    uint32_t seed;
} EyeRecipe;

/* Packed pixel format = 0xFFRRGGBB. All coordinates wrap modulo 64. */
int eye_valid(const EyeRecipe *recipe);
uint32_t eye_pixel(const EyeRecipe *recipe, int x, int y);
int eye_generate(const EyeRecipe *recipe, uint32_t *rgba, size_t pixel_capacity);

/* Exact 12-byte recipe + 12-byte header, never memcpy a C struct on disk. */
size_t eye_pack_write(const EyeRecipe *recipes, unsigned count, uint8_t *dst, size_t capacity);
int eye_pack_read(const uint8_t *data, size_t bytes, EyeRecipe *out,
                  unsigned capacity, unsigned *count);

uint64_t eye_hash_pixels(const uint32_t *pixels, size_t count);
const char *eye_family_name(unsigned family);
int eye_family_from_name(const char *name); /* lowercase ASCII family identifiers */

#endif
