#include "eye.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint32_t frame1[EYE_SIZE * EYE_SIZE];
static uint32_t frame2[EYE_SIZE * EYE_SIZE];
static uint8_t bytes[EYE_PACK_MAX_BYTES];
static EyeRecipe outputs[EYE_MAX_RECIPES];

int main(void) {
    EyeRecipe r[2] = {
        {EYE_STONE, 0, 1, 0, 8, 4, 14, 0, UINT32_C(0x12345678)},
        {EYE_WOOD, 2, 2, 1, 4, 0, 12, 0, UINT32_C(0xbadc0ffe)}
    };
    uint64_t hash1, hash2;
    unsigned n = 777u;
    size_t length;
    int i;
    assert(eye_valid(&r[0]));
    assert(eye_generate(&r[0], frame1, EYE_SIZE * EYE_SIZE));
    assert(eye_generate(&r[0], frame2, EYE_SIZE * EYE_SIZE));
    hash1 = eye_hash_pixels(frame1, EYE_SIZE * EYE_SIZE);
    assert(hash1 == eye_hash_pixels(frame2, EYE_SIZE * EYE_SIZE));
    /* Golden hashes lock visual behavior for format EYE1 on conforming C11. */
    assert(hash1 == UINT64_C(0xcb83ea8603a319b2));
    for (i = 0; i < 64; ++i) {
        assert(eye_pixel(&r[0], i, 0) == eye_pixel(&r[0], i, 64));
        assert(eye_pixel(&r[0], -1, i) == eye_pixel(&r[0], 63, i));
        assert(eye_pixel(&r[0], i, -64) == eye_pixel(&r[0], i, 0));
    }
    assert(eye_generate(&r[1], frame2, EYE_SIZE * EYE_SIZE));
    hash2 = eye_hash_pixels(frame2, EYE_SIZE * EYE_SIZE);
    assert(hash1 != hash2);
    assert(hash2 == UINT64_C(0x7255a43ef5107113));
    assert(!eye_generate(&r[1], frame1, EYE_SIZE * EYE_SIZE - 1u));
    assert(!eye_generate(NULL, frame1, EYE_SIZE * EYE_SIZE));
    assert(!eye_generate(&r[1], NULL, EYE_SIZE * EYE_SIZE));

    assert(eye_pack_write(r, 0, bytes, sizeof(bytes)) == 0);
    assert(eye_pack_write(r, 2, bytes, 35u) == 0);
    length = eye_pack_write(r, 2, bytes, sizeof(bytes));
    assert(length == 36u);
    assert(bytes[0] == 'E' && bytes[1] == 'Y' && bytes[2] == 'E' && bytes[3] == '1');
    assert(bytes[12] == EYE_STONE && bytes[24] == EYE_WOOD);
    assert(bytes[20] == 0x78 && bytes[21] == 0x56 && bytes[22] == 0x34 && bytes[23] == 0x12);
    assert(eye_pack_read(bytes, length, outputs, EYE_MAX_RECIPES, &n));
    assert(n == 2);
    assert(outputs[0].seed == r[0].seed);
    assert(outputs[1].seed == r[1].seed);
    assert(!eye_pack_read(bytes, length - 1u, outputs, EYE_MAX_RECIPES, &n));
    assert(!eye_pack_read(bytes, length, outputs, 1u, &n));
    bytes[20] ^= 1u;
    assert(!eye_pack_read(bytes, length, outputs, EYE_MAX_RECIPES, &n));
    bytes[20] ^= 1u;
    assert(eye_pack_read(bytes, length, outputs, EYE_MAX_RECIPES, &n));
    bytes[3] = '2';
    assert(!eye_pack_read(bytes, length, outputs, EYE_MAX_RECIPES, &n));
    bytes[3] = '1';
    r[0].family = 255;
    assert(!eye_valid(&r[0]));
    assert(eye_pack_write(r, 2, bytes, sizeof(bytes)) == 0);
    assert(eye_pixel(&r[0], 1, 1) == 0);
    {
        /* Exercise every family and the less common pattern frequencies. */
        unsigned f;
        for (f = 0; f < EYE_FAMILY_COUNT; ++f) {
            EyeRecipe t = {(uint8_t)f, 0, 3, 4, 7, 7, 10, 0, UINT32_C(0xdeadbeef)};
            assert(eye_generate(&t, frame1, EYE_SIZE * EYE_SIZE));
            assert(eye_pixel(&t, 0, 0) == eye_pixel(&t, 64, 0));
            t.scale = 4;
            assert(eye_generate(&t, frame1, EYE_SIZE * EYE_SIZE));
            assert(eye_pixel(&t, 11, 5) == eye_pixel(&t, 75, 5));
        }
    }

    assert(strcmp(eye_family_name(EYE_METAL), "metal") == 0);
    assert(eye_family_from_name("foliage") == EYE_FOLIAGE);
    assert(eye_family_from_name("fake") == -1);

    printf("PASS: bitmap determinista y periodico, validacion de pack, checksum, endian, limites\n");
    printf("stone=%016" PRIx64 " wood=%016" PRIx64 " pack=%zu bytes\n", hash1, hash2, length);
    return 0;
}
