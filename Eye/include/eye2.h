#ifndef EYE2_H
#define EYE2_H

#include "eye.h"

/* EYE2 preserves the 12-byte recipe layout but alters the synthesis algorithm.
   Magic EYE2 prevents confusing modern pixels with historical EYE1 pixels. */
#define EYE2_FORMAT_MAJOR 2u

/* Validation intentionally shares exactly the EYE1 field constraints for now. */
int eye2_generate(const EyeRecipe *recipe, uint32_t *pixels, size_t capacity);
uint32_t eye2_pixel(const EyeRecipe *recipe, int x, int y);
size_t eye2_pack_write(const EyeRecipe *recipes, unsigned count,
                       uint8_t *dst, size_t capacity);
int eye2_pack_read(const uint8_t *data, size_t length,
                   EyeRecipe *out, unsigned capacity, unsigned *count);

/* C11 runtime bank: Bones keeps authority over scene/tick/physics, Eye owns
   synthesis only. Supports EYE1, EYE2 and EYE3 packs concurrently; include eye3.h for new families. */
typedef struct {
    EyeRecipe recipes[EYE_MAX_RECIPES];
    unsigned count;
    unsigned version;
} EyeTextureBank;

int eye_bank_open(EyeTextureBank *bank, const uint8_t *bytes, size_t length);
int eye_bank_render(const EyeTextureBank *bank, unsigned material_index,
                    uint32_t *pixels, size_t capacity);

/* OS-free, Bones-independent: material decoder boundary for the engine.
   Caller owns pixels and the cache; does NOT override Bones physics/ticks. */
int eye2_material_from_embedded(const uint8_t *pack, size_t bytes,
                                unsigned material_index,
                                uint32_t *pixels, size_t pixel_capacity);

#endif
