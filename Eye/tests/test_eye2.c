#include "eye2.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint8_t bytes[EYE_PACK_MAX_BYTES];
static EyeTextureBank bank;
static EyeRecipe rec[EYE_MAX_RECIPES];
static EyeRecipe restored[EYE_MAX_RECIPES];
static uint32_t pic1[EYE_SIZE * EYE_SIZE];
static uint32_t pic2[EYE_SIZE * EYE_SIZE];

static void verify_family(unsigned f) {
    EyeRecipe r = {(uint8_t)f, (uint8_t)(f & 3u), 1u, (uint8_t)(f & 7u),
                   12u, 9u, 13u, 0u, 0x845a32edu ^ f};
    unsigned x, y;
    assert(eye2_generate(&r, pic1, EYE_SIZE * EYE_SIZE));
    assert(eye2_generate(&r, pic2, EYE_SIZE * EYE_SIZE));
    assert(memcmp(pic1, pic2, sizeof(pic1)) == 0);
    for (y = 0; y < EYE_SIZE; ++y) for (x = 0; x < EYE_SIZE; ++x) {
        assert(pic1[y * EYE_SIZE + x] == eye2_pixel(&r, (int)x + 64, (int)y));
        assert(pic1[y * EYE_SIZE + x] == eye2_pixel(&r, (int)x, (int)y - 64));
        assert((pic1[y * EYE_SIZE + x] >> 24) == 255u);
    }
}
int main(void) {
    unsigned n = 999u, i;
    size_t len;
    for (i = 0; i < EYE_FAMILY_COUNT; ++i) verify_family(i);
    for (i = 0; i < 22u; ++i) {
        rec[i].family = (uint8_t)(i % EYE_FAMILY_COUNT);
        rec[i].palette = (uint8_t)(i % 4u);
        rec[i].scale = (uint8_t)(1u + (i % 4u));
        rec[i].pattern = (uint8_t)(i % 8u);
        rec[i].wear = (uint8_t)(i % 16u);
        rec[i].growth = (uint8_t)((i + 2u) % 16u);
        rec[i].contrast = (uint8_t)((i + 3u) % 16u);
        rec[i].flags = 0;
        rec[i].seed = 0xabc00121u + i;
    }
    len = eye2_pack_write(rec, 22u, bytes, sizeof bytes);
    assert(len == 276u);
    assert(bytes[3] == '2');
    assert(eye_bank_open(&bank, bytes, len));
    assert(bank.version == 2 && bank.count == 22);
    assert(eye_bank_render(&bank, 8, pic1, EYE_SIZE * EYE_SIZE));
    assert(eye2_generate(&rec[8], pic2, EYE_SIZE * EYE_SIZE));
    assert(!memcmp(pic1, pic2, sizeof pic1));
    assert(!eye_bank_render(&bank, 22, pic1, EYE_SIZE * EYE_SIZE));
    assert(!eye_pack_read(bytes, len, restored, EYE_MAX_RECIPES, &n));
    assert(eye2_pack_read(bytes, len, restored, EYE_MAX_RECIPES, &n) && n == 22u);
    assert(restored[0].seed == rec[0].seed && restored[21].seed == rec[21].seed);
    assert(eye2_material_from_embedded(bytes, len, 12u, pic1, EYE_SIZE * EYE_SIZE));
    assert(eye2_generate(&rec[12], pic2, EYE_SIZE * EYE_SIZE));
    assert(!memcmp(pic1, pic2, sizeof pic1));
    assert(!eye2_material_from_embedded(bytes, len, 22u, pic1, EYE_SIZE * EYE_SIZE));
    assert(!eye2_material_from_embedded(bytes, len, 12u, pic1, 0u));
    assert(!eye2_pack_read(bytes, len - 1u, restored, EYE_MAX_RECIPES, &n));
    bytes[20] ^= 0x22u;
    assert(!eye2_pack_read(bytes, len, restored, EYE_MAX_RECIPES, &n));
    bytes[20] ^= 0x22u;
    assert(eye2_pack_read(bytes, len, restored, EYE_MAX_RECIPES, &n));
    bytes[3] = '1';
    /* Corrupting or relabelling the format MUST NOT reinterpret a valid
       EYE2 recipe as an EYE1 recipe. EYE1 legacy packs stay untouched. */
    assert(!eye_bank_open(&bank, bytes, len));
    assert(!eye2_pack_read(bytes, len, restored, EYE_MAX_RECIPES, &n));
    bytes[3] = '2';
    assert(eye_bank_open(&bank, bytes, len) && bank.version == 2);
    { /* Legacy pixels must remain unchanged, including through the bank. */
        EyeRecipe legacy = {EYE_STONE, 0, 1, 0, 8, 4, 14, 0, UINT32_C(0x12345678)};
        size_t bytes1 = eye_pack_write(&legacy, 1, bytes, sizeof bytes);
        assert(bytes1 == 24u && eye_bank_open(&bank, bytes, bytes1));
        assert(bank.version == 1 && eye_bank_render(&bank, 0, pic1, EYE_SIZE * EYE_SIZE));
        assert(eye_hash_pixels(pic1, EYE_SIZE * EYE_SIZE) == UINT64_C(0xcb83ea8603a319b2));
    }
    rec[0].scale = 0;
    assert(!eye2_generate(&rec[0], pic1, EYE_SIZE * EYE_SIZE));
    assert(!eye2_pack_write(rec, 22u, bytes, sizeof bytes));
    rec[0].scale = 1;
    assert(!eye2_pack_write(rec, 22u, bytes, 100));
    printf("PASS EYE2: 8 familias, semilla, tiling, serializacion EYE2, codec antiguo aislado, memoria cliente\n");
    printf("example_EYE2_hash=%016" PRIx64 " format_bytes=%zu\n",
           eye_hash_pixels(pic2, EYE_SIZE * EYE_SIZE), len);
    return 0;
}
