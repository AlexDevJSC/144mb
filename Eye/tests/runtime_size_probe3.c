/* Isolated runtime probe EYE3: not a Windows PE and not Bones. */
#include "eye3.h"
#include <stdio.h>
static uint32_t pixels[EYE_SIZE*EYE_SIZE];
int main(void) {
    EyeRecipe roof={EYE3_ROOF,0,1,2,8,1,12,0,UINT32_C(0x14430006)};
    if(!eye3_generate(&roof,pixels,EYE_SIZE*EYE_SIZE))return 1;
    printf("%016llx %08x\n", (unsigned long long)eye_hash_pixels(pixels,4096u),
        (unsigned)eye_uv_sample_repeat(pixels,65536,0));
    return 0;
}
