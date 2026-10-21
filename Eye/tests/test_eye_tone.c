#include "eye2.h"
#include "eye_tone.h"
#include "eye_bones_bridge.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t pix[4096];
static uint32_t original[4096];
static uint32_t a[3*4096];
static uint32_t b[3*4096];
static uint8_t bytes[EYE_PACK_MAX_BYTES];
static EyeTextureBank bank;

int main(void) {
    EyeRecipe recipes[3] = {
        {EYE_STONE,0,1,0,9,2,14,0,0xabcdaabb},
        {EYE_DIRT,0,1,0,9,2,14,0,0xbccdaabb},
        {EYE_WOOD,0,1,0,9,2,14,0,0xccddaabb}
    };
    const unsigned ids[3] = {0,1,2};
    size_t n = eye2_pack_write(recipes,3,bytes,sizeof bytes);
    unsigned changed = 0;
    assert(n==48 && eye_bank_open(&bank,bytes,n));
    assert(eye_bank_render(&bank,0,pix,4096));
    memcpy(original,pix,sizeof original);
    assert(eye_tone_apply(pix,4096,EYE_TONE_ORIGINAL));
    assert(memcmp(original,pix,sizeof original)==0);
    assert(!eye_tone_apply(NULL,4096,EYE_TONE_AGED));
    assert(!eye_tone_apply(pix,4096,(EyeTone)99));
    assert(memcmp(original,pix,sizeof original)==0);
    assert(eye_tone_apply(pix,4096,EYE_TONE_AGED));
    for (unsigned i=0;i<4096;i++) {
        if (pix[i]!=original[i]) changed++;
        assert((pix[i]>>24)==255);
    }
    assert(changed>3000);
    assert(eye_bones_legacy_fill(&bank,ids,a,3*4096));
    assert(eye_bones_legacy_fill_tone(&bank,ids,b,3*4096,EYE_TONE_AGED));
    for (unsigned i=0;i<3*4096;i++) assert(b[i] == eye_tone_pixel(a[i],EYE_TONE_AGED));
    assert(!eye_bones_legacy_fill_tone(&bank,ids,b,3*4096,(EyeTone)99));
    puts("PASS tono opcional: 12B/receta intactos, 3 slots de Bones, bytes EYE2 sin cambios");
    return 0;
}
