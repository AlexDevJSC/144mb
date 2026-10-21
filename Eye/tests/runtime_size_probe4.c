/* Isolated release-size baseline; this is NOT a Windows PE or the full game. */
#include "eye4.h"
#include <stdio.h>
static uint32_t pixels[4096];
int main(void) {
    Eye4Recipe r[8];eye4_demo_recipes(r,EYE4_HYBRID);
    if(!eye4_generate(&r[EYE4_WINDOW],pixels,4096))return 1;
    printf("%016llx\n",(unsigned long long)eye_hash_pixels(pixels,4096));
    return 0;
}
