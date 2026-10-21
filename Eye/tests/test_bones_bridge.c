#include "bones/bones.h"        /* TEST DOUBLE, not original Bones */
#include "sw_renderer.h"        /* TEST DOUBLE, not original Bones */
#include "eye_bones_compat.h"
#include "eye_bones_bridge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

_Static_assert(P144_MAT_COUNT == EYE_BONES_LEGACY_SLOTS,
               "Legacy Bones slot count changed; renegotiate contract");
_Static_assert(P144_TEX_SIZE == EYE_SIZE,
               "Legacy Bones texture dimension changed; renegotiate contract");

static P144_Renderer display;
static EyeTextureBank bank;
static uint8_t pack[EYE_PACK_MAX_BYTES];
static uint32_t expected[EYE_SIZE * EYE_SIZE];

int main(void) {
    EyeRecipe r[3] = {
        {EYE_STONE, 0, 1, 0, 10, 2, 14, 0, 0xa1b2c3d4u},
        {EYE_DIRT, 0, 1, 0,  7, 2, 12, 0, 0xb2c3d4e5u},
        {EYE_WOOD, 1, 2, 2,  3, 0, 13, 0, 0xc3d4e5f6u}
    };
    unsigned material_ids[3] = {0u, 1u, 2u};
    size_t len = eye2_pack_write(r, 3u, pack, sizeof pack);
    assert(len == 48u && eye_bank_open(&bank, pack, len));
    for (unsigned i = 0; i < 3; ++i) {
        for (unsigned j = 0; j < EYE_BONES_LEGACY_TEXELS; ++j)
            display.tex[i][j] = UINT32_C(0xdeadbeef);
    }
    material_ids[2] = 3u; /* invalid map => no partial modifications */
    assert(!eye_bones_legacy_fill(&bank, material_ids, &display.tex[0][0],
                  sizeof display.tex / sizeof display.tex[0][0]));
    assert(display.tex[0][0] == UINT32_C(0xdeadbeef) &&
           display.tex[1][2048] == UINT32_C(0xdeadbeef));
    material_ids[2] = 2u;
    assert(!eye_bones_legacy_fill(&bank, material_ids, &display.tex[0][0],
                  EYE_BONES_LEGACY_TEXELS * 2u));
    assert(eye_bones_legacy_fill(&bank, material_ids, &display.tex[0][0],
                  sizeof display.tex / sizeof display.tex[0][0]));
    for (unsigned i = 0; i < 3; ++i) {
        assert(eye_bank_render(&bank, i, expected, EYE_BONES_LEGACY_TEXELS));
        assert(!memcmp(expected, display.tex[i], sizeof expected));
        assert((display.tex[i][0] >> 24) == 255u);
    }
    /* The legacy EYE1 bank should also fill the same consumer texels. */
    len = eye_pack_write(r, 3u, pack, sizeof pack);
    assert(len == 48u && eye_bank_open(&bank, pack, len) && bank.version == 1u);
    assert(eye_bones_legacy_fill(&bank, material_ids, &display.tex[0][0],
                  sizeof display.tex / sizeof display.tex[0][0]));
    assert(eye_bank_render(&bank, 1u, expected, EYE_BONES_LEGACY_TEXELS));
    assert(!memcmp(expected, display.tex[1], sizeof expected));
    puts("PASS: Eye bank EYE1/EYE2 -> legacy texture layout, API macros 2/1 (mock only)");
    return 0;
}
