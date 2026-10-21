#include "eye5.h"
#include <stdio.h>
int main(void){
    static uint32_t pixels[4096];Eye4Recipe recipes[8];Eye5Recipe roofs[4];Eye5RoofMesh mesh;
    Eye5RoofSpec s={2.2f,1.5f,3.2f,1.25f,0.18f,EYE5_GABLE};
    eye4_demo_recipes(recipes,EYE4_HYBRID);eye5_demo_recipes(roofs);
    if(!eye4_generate(&recipes[EYE4_ROOF],pixels,4096u))return 2;
    if(!eye5_generate(&roofs[0],pixels,4096u)||!eye5_build_roof(&s,&mesh))return 3;
    printf("%016llx %u\n",(unsigned long long)eye_hash_pixels(pixels,4096u),mesh.count);return 0;
}
