/* Minimal representative EYE2 runtime size probe: NOT a Bones executable. */
#include "eye2.h"
#ifdef EYE_SIZE_PROBE_WITH_TONE
#include "eye_tone.h"
#endif
#include <stdio.h>
static uint32_t pixels[EYE_SIZE * EYE_SIZE];
int main(void) {
    EyeRecipe recipe = {EYE_STONE,0,1,0,9,3,12,0,0x12345678u};
    if (!eye2_generate(&recipe,pixels,EYE_SIZE*EYE_SIZE)) return 1;
#ifdef EYE_SIZE_PROBE_WITH_TONE
    if (!eye_tone_apply(pixels,EYE_SIZE*EYE_SIZE,EYE_TONE_AGED)) return 2;
#endif
    printf("%016llx\n",(unsigned long long)eye_hash_pixels(pixels,EYE_SIZE*EYE_SIZE));
    return 0;
}
