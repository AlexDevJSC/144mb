#include "bones/bones.h" /* compile-time double */
#include "sw_renderer.h"
#include "eye_bones_compat.h"
#include "eye2.h"
#include <assert.h>
#include <stdio.h>

int eye_bones_example_upload(P144_Renderer *backend_renderer,
                             const EyeTextureBank *bank);

static EyeTextureBank bank;
static P144_Renderer renderer;
static uint8_t pack[EYE_PACK_MAX_BYTES];

int main(void) {
    EyeRecipe recipe[11] = {0};
    size_t len;
    unsigned i;
    for (i = 0; i < 11u; ++i) {
        recipe[i] = (EyeRecipe){(uint8_t)(i%8u),0u,1u,0u,7u,4u,13u,0u,i+101u};
    }
    len = eye2_pack_write(recipe, 11u, pack, sizeof pack);
    assert(len && eye_bank_open(&bank, pack, len));
    assert(eye_bones_example_upload(&renderer, &bank));
    assert(renderer.tex[0][0] == eye2_pixel(&recipe[0],0,0));
    assert(renderer.tex[1][0] == eye2_pixel(&recipe[10],0,0));
    assert(renderer.tex[2][0] == eye2_pixel(&recipe[7],0,0));
    puts("PASS example adapter source, test double only (NOT compiled with real Bones)");
    return 0;
}
