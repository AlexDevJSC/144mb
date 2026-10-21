#include "eye2.h"
#include "materiales_embebidos.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static EyeRecipe materials[EYE_MAX_RECIPES];
static uint32_t canvas[EYE_SIZE * EYE_SIZE];
int main(void) {
    unsigned count = 0;
    if (!eye2_pack_read(eye_pack_embedded, EYE_PACK_EMBEDDED_BYTES,
                        materials, EYE_MAX_RECIPES, &count)) return 1;
    if (count != 22u || !eye2_generate(&materials[0], canvas, EYE_SIZE * EYE_SIZE)) return 2;
    printf("Embedded pack decoded, %u materials, texture hash=%016" PRIx64 "\n",
           count, eye_hash_pixels(canvas, EYE_SIZE * EYE_SIZE));
    return 0;
}
