/* Developer-only visual comparison of the SAME EYE2 generated pixels before
   and after optional EyeTone grading. PPM output is not shipped with game. */
#include "eye2.h"
#include "eye_tone.h"
#include <stdio.h>
#include <stdlib.h>

#define COUNT 8u
#define WIDTH (COUNT * EYE_SIZE)
static uint8_t pack[EYE_PACK_MAX_BYTES];
static EyeTextureBank bank;
static uint32_t pixels[COUNT][EYE_SIZE * EYE_SIZE];
static unsigned ids[COUNT] = {0u, 4u, 7u, 10u, 12u, 14u, 16u, 19u};

static int draw_row(FILE *f, unsigned y, EyeTone tone) {
    uint8_t rgb[3];
    for (unsigned m = 0; m < COUNT; ++m) {
        for (unsigned x = 0; x < EYE_SIZE; ++x) {
            uint32_t p = eye_tone_pixel(pixels[m][y * EYE_SIZE + x], tone);
            rgb[0] = (uint8_t)(p >> 16);
            rgb[1] = (uint8_t)(p >> 8);
            rgb[2] = (uint8_t)p;
            if (fwrite(rgb, 1, 3, f) != 3) return 0;
        }
    }
    return 1;
}

static int draw_ppm(const char *path, EyeTone tone) {
    FILE *f = fopen(path, "wb");
    int ok = 1;
    if (!f) return 0;
    if (fprintf(f, "P6\n%u %u\n255\n", WIDTH, EYE_SIZE) < 0) ok = 0;
    for (unsigned y = 0; y < EYE_SIZE && ok; ++y) ok = draw_row(f, y, tone);
    if (fclose(f)) ok = 0;
    return ok;
}

int main(int argc, char **argv) {
    long total;
    FILE *f;
    if (argc != 4) {
        fprintf(stderr, "Uso: eye_tone_preview <materiales.eyep> <normal.ppm> <apagado.ppm>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) return 2;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return 2; }
    total = ftell(f);
    if (total < 0 || (size_t)total > sizeof(pack) ||
        fseek(f, 0, SEEK_SET) || fread(pack, 1, (size_t)total, f) != (size_t)total) {
        fclose(f); return 2;
    }
    fclose(f);
    if (!eye_bank_open(&bank, pack, (size_t)total) || bank.count <= ids[COUNT - 1]) return 3;
    /* Decode once per material. Tone changes are purely a cheap pixel pass. */
    for (unsigned m = 0; m < COUNT; ++m)
        if (!eye_bank_render(&bank, ids[m], pixels[m], EYE_SIZE * EYE_SIZE)) return 3;
    if (!draw_ppm(argv[2], EYE_TONE_ORIGINAL) || !draw_ppm(argv[3], EYE_TONE_AGED)) return 4;
    puts("OK: original y envejecido desde iguales recetas y semillas EYE2");
    return 0;
}
