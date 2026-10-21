#ifndef EYE3_H
#define EYE3_H

#include "eye2.h"

/* EYE3: 12-byte recipes, 12-byte header, enlarged family set.
   EYE1/EYE2 pixel algorithms and binary encoders remain unchanged. */
#define EYE3_FORMAT_MAJOR 3u
#define EYE3_FAMILY_COUNT 11u
#define EYE3_ROOF 8u
#define EYE3_COBBLE 9u
#define EYE3_CARVING 10u

int eye3_valid(const EyeRecipe *recipe);
uint32_t eye3_pixel(const EyeRecipe *recipe, int x, int y);
int eye3_generate(const EyeRecipe *recipe, uint32_t *pixels, size_t capacity);
size_t eye3_pack_write(const EyeRecipe *recipes, unsigned count,
                       uint8_t *dst, size_t capacity);
int eye3_pack_read(const uint8_t *bytes, size_t length, EyeRecipe *recipes,
                   unsigned capacity, unsigned *count);
const char *eye3_family_name(unsigned family);
int eye3_family_from_name(const char *name);

/* Signed Q16.16 UV. U=65536 is exactly one texture repeat.
   The output is a pixel from caller-provided (64x64, 0xFFRRGGBB) texture.
   Geometry remains Bones' responsibility. */
uint32_t eye_uv_sample_repeat(const uint32_t *texture, int32_t u_q16,
                              int32_t v_q16);

/* Optional fixed-size RAM cache. Caller owns all memory, no heap, no disk.
   Clear/reinit after switching the bank or regeneration algorithm. */
#define EYE_CACHE_LIMIT 16u
typedef struct {
    const EyeTextureBank *bank;
    uint32_t *texels;
    unsigned slots, used;
    unsigned material[EYE_CACHE_LIMIT];
    uint32_t age[EYE_CACHE_LIMIT], counter;
    unsigned hits, misses;
} EyeMaterialCache;
int eye_cache_init(EyeMaterialCache *cache, const EyeTextureBank *bank,
                   uint32_t *storage, size_t texel_capacity, unsigned slots);
const uint32_t *eye_cache_get(EyeMaterialCache *cache, unsigned material_index);

#endif
