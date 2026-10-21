#include "eye4.h"
#include <stdio.h>
int main(void){
    static uint32_t pixels[4096];Eye4Recipe recipes[8];
    eye4_demo_recipes(recipes,EYE4_HYBRID);
    if(!eye4_generate(&recipes[EYE4_ROOF],pixels,4096u))return 2;
    printf("%016llx\n",(unsigned long long)eye_hash_pixels(pixels,4096u));return 0;
}
