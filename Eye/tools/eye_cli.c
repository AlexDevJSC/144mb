#include "eye3.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EyeRecipe recipes[EYE_MAX_RECIPES];
static uint8_t packed[EYE_PACK_MAX_BYTES];
static uint32_t pixel_buffer[EYE_SIZE * EYE_SIZE];
static unsigned format_version;

static void usage(void) {
    fputs("Eye v0.3 - texturas retro por recetas compactas\n"
          "Uso:\n"
          "  eye compile <materiales.eye.txt> <materiales.eyep> (EYE1 legacy)\n"
          "  eye compile2 <materiales.eye.txt> <materiales.eyep> (EYE2)\n"
          "  eye compile3 <materiales.eye.txt> <materiales.eyep> (EYE3)\n"
          "  eye inspect <materiales.eyep>\n"
          "  eye render <materiales.eyep> <indice-desde-0> <imagen.ppm>\n"
          "  eye atlas <materiales.eyep> <atlas.ppm>\n"
          "  eye embed <materiales.eyep> <recetas.h>\n", stderr);
}
static int read_file(const char *path, uint8_t *dst, size_t limit, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    size_t count;
    int extra;
    if (!f) { perror(path); return 0; }
    count = fread(dst, 1, limit, f);
    extra = fgetc(f);
    if (ferror(f) || extra != EOF) { fprintf(stderr, "Entrada demasiado grande o ilegible: %s\n", path); fclose(f); return 0; }
    if (fclose(f) != 0) return 0;
    *bytes = count;
    return 1;
}
static int write_file(const char *path, const void *src, size_t bytes) {
    FILE *f = fopen(path, "wb");
    int good;
    if (!f) { perror(path); return 0; }
    good = fwrite(src, 1, bytes, f) == bytes;
    if (fclose(f) != 0) good = 0;
    if (!good) fprintf(stderr, "Error de escritura: %s\n", path);
    return good;
}
static int unsigned_field(const char *word, uint32_t *value) {
    char *end;
    unsigned long long n;
    if (!word || !word[0] || word[0] == '-' || word[0] == '+') return 0;
    errno = 0;
    n = strtoull(word, &end, 0);
    if (errno || *end != '\0' || n > UINT32_MAX) return 0;
    *value = (uint32_t)n;
    return 1;
}
static int parse_recipe_line(char *text, EyeRecipe *r, unsigned version) {
    char *words[10] = {0};
    int n = 0, i, family;
    uint32_t fields[7];
    char *tok = strtok(text, " \t\r\n");
    while (tok && n < 10) { words[n++] = tok; tok = strtok(NULL, " \t\r\n"); }
    if (n != 9 || tok) return 0;
    if (strlen(words[0]) > 31u) return 0;
    family = version == 3u ? eye3_family_from_name(words[1]) : eye_family_from_name(words[1]);
    if (family < 0) return 0;
    for (i = 0; i < 7; ++i) if (!unsigned_field(words[i + 2], &fields[i])) return 0;
    if (fields[0] > 255 || fields[1] > 255 || fields[2] > 255 ||
        fields[3] > 255 || fields[4] > 255 || fields[5] > 255) return 0;
    r->family = (uint8_t)family;
    r->palette = (uint8_t)fields[0];
    r->scale = (uint8_t)fields[1];
    r->pattern = (uint8_t)fields[2];
    r->wear = (uint8_t)fields[3];
    r->growth = (uint8_t)fields[4];
    r->contrast = (uint8_t)fields[5];
    r->flags = 0;
    r->seed = fields[6];
    return version == 3u ? eye3_valid(r) : eye_valid(r);
}
static int cmd_compile(const char *source, const char *destination, unsigned version) {
    FILE *f = fopen(source, "rb");
    char line[512];
    unsigned count = 0, number = 0;
    size_t bytes;
    if (!f) { perror(source); return 1; }
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        ++number;
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "Linea %u demasiado larga\n", number);
            fclose(f); return 2;
        }
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (count >= EYE_MAX_RECIPES || !parse_recipe_line(p, &recipes[count], version)) {
            fprintf(stderr, "Receta invalida en %s:%u\n", source, number);
            fclose(f); return 2;
        }
        ++count;
    }
    if (ferror(f)) { fclose(f); return 3; }
    fclose(f);
    if (count == 0) { fprintf(stderr, "El archivo no contiene materiales\n"); return 4; }
    bytes = version == 3u ? eye3_pack_write(recipes, count, packed, sizeof(packed)) :
            version == 2u ? eye2_pack_write(recipes, count, packed, sizeof(packed)) :
                            eye_pack_write(recipes, count, packed, sizeof(packed));
    if (!bytes || !write_file(destination, packed, bytes)) return 5;
    printf("EYE%d: %u recetas, %zu bytes (12 cabecera + %u x 12 bytes)\n", version, count, bytes, count);
    return 0;
}
static int open_pack(const char *path, unsigned *count, size_t *bytes) {
    if (!read_file(path, packed, sizeof(packed), bytes)) return 0;
    format_version = *bytes >= 4 && !memcmp(packed, "EYE3", 4) ? 3u :
                     *bytes >= 4 && !memcmp(packed, "EYE2", 4) ? 2u : 1u;
    int ok = format_version == 3u ? eye3_pack_read(packed, *bytes, recipes, EYE_MAX_RECIPES, count) :
             format_version == 2u ? eye2_pack_read(packed, *bytes, recipes, EYE_MAX_RECIPES, count) :
                                    eye_pack_read(packed, *bytes, recipes, EYE_MAX_RECIPES, count);
    if (!ok) {
        fprintf(stderr, "Pack invalido: formato, version, tamano, receta o checksum\n");
        return 0;
    }
    return 1;
}
static int cmd_inspect(const char *path) {
    unsigned count, i;
    size_t bytes;
    if (!open_pack(path, &count, &bytes)) return 1;
    printf("EYE%d | %u recetas | %zu bytes | 64x64 px por material\n", format_version, count, bytes);
    printf("indice familia  pal esc pat wear growth contraste semilla\n");
    for (i = 0; i < count; ++i) {
        const EyeRecipe *r = &recipes[i];
        printf("%5u  %-7s %3u %3u %3u %4u %6u %8u 0x%08" PRIx32 "\n",
               i, eye3_family_name(r->family), r->palette, r->scale,
               r->pattern, r->wear, r->growth, r->contrast, r->seed);
    }
    return 0;
}
static int write_ppm(const char *path, const uint32_t *pixels, unsigned width, unsigned height) {
    FILE *f = fopen(path, "wb");
    size_t i;
    int ok = 1;
    if (!f) { perror(path); return 0; }
    if (fprintf(f, "P6\n%u %u\n255\n", width, height) < 0) ok = 0;
    for (i = 0; ok && i < (size_t)width * height; ++i) {
        unsigned char triplet[3] = {
            (unsigned char)(pixels[i] >> 16),
            (unsigned char)(pixels[i] >> 8),
            (unsigned char)pixels[i]
        };
        if (fwrite(triplet, 1, 3, f) != 3) ok = 0;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) fprintf(stderr, "No se pudo escribir PPM: %s\n", path);
    return ok;
}
static int cmd_render(const char *source, const char *index_text, const char *output) {
    unsigned count;
    uint32_t idx;
    size_t bytes;
    if (!unsigned_field(index_text, &idx)) return 2;
    if (!open_pack(source, &count, &bytes)) return 1;
    if (idx >= count) { fprintf(stderr, "Indice fuera de rango: %" PRIu32 "\n", idx); return 2; }
    if (!(format_version == 3u ? eye3_generate(&recipes[idx], pixel_buffer, EYE_SIZE * EYE_SIZE) :
          format_version == 2u ? eye2_generate(&recipes[idx], pixel_buffer, EYE_SIZE * EYE_SIZE) :
                                 eye_generate(&recipes[idx], pixel_buffer, EYE_SIZE * EYE_SIZE))) return 3;
    if (!write_ppm(output, pixel_buffer, EYE_SIZE, EYE_SIZE)) return 4;
    printf("Material %" PRIu32 ": %s; hash %016" PRIx64 "\n",
           idx, eye3_family_name(recipes[idx].family),
           eye_hash_pixels(pixel_buffer, EYE_SIZE * EYE_SIZE));
    return 0;
}
static int cmd_atlas(const char *source, const char *output) {
    unsigned count, i, x, y;
    size_t bytes;
    const unsigned cols = 4, cell = EYE_SIZE + 4;
    unsigned rows, width, height;
    uint32_t *atlas;
    int ok;
    if (!open_pack(source, &count, &bytes)) return 1;
    rows = (count + cols - 1u) / cols;
    width = cols * cell + 4u;
    height = rows * cell + 4u;
    atlas = (uint32_t *)malloc((size_t)width * height * sizeof(uint32_t));
    if (!atlas) { fprintf(stderr, "Sin memoria para atlas\n"); return 2; }
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) atlas[y * width + x] = UINT32_C(0xff292a31);
    for (i = 0; i < count; ++i) {
        unsigned ox = 4u + (i % cols) * cell;
        unsigned oy = 4u + (i / cols) * cell;
        if (!(format_version == 3u ? eye3_generate(&recipes[i], pixel_buffer, EYE_SIZE * EYE_SIZE) :
              format_version == 2u ? eye2_generate(&recipes[i], pixel_buffer, EYE_SIZE * EYE_SIZE) :
                                     eye_generate(&recipes[i], pixel_buffer, EYE_SIZE * EYE_SIZE))) { free(atlas); return 3; }
        for (y = 0; y < EYE_SIZE; ++y)
            for (x = 0; x < EYE_SIZE; ++x)
                atlas[(oy + y) * width + (ox + x)] = pixel_buffer[y * EYE_SIZE + x];
    }
    ok = write_ppm(output, atlas, width, height);
    free(atlas);
    if (ok) printf("Atlas %u materiales: %u x %u px\n", count, width, height);
    return ok ? 0 : 4;
}
static int cmd_embed(const char *source, const char *output) {
    unsigned count;
    size_t bytes, i;
    FILE *f;
    if (!open_pack(source, &count, &bytes)) return 1;
    f = fopen(output, "wb");
    if (!f) { perror(output); return 2; }
    fprintf(f, "/* AUTO-GENERATED by Eye v0.3. DO NOT EDIT. */\n"
               "#ifndef EYE_PACK_EMBEDDED_H\n#define EYE_PACK_EMBEDDED_H\n"
               "#include <stdint.h>\n"
               "static const uint8_t eye_pack_embedded[%zu] = {\n", bytes);
    for (i = 0; i < bytes; ++i) {
        if (i % 12u == 0) fputs("    ", f);
        fprintf(f, "0x%02x%s", (unsigned)packed[i], i + 1u == bytes ? "" : ", ");
        if (i % 12u == 11u || i + 1u == bytes) fputc('\n', f);
    }
    fprintf(f, "};\n#define EYE_PACK_EMBEDDED_BYTES %zuu\n#endif\n", bytes);
    if (fclose(f) != 0) return 3;
    printf("Header C generado para %u recetas: %zu bytes binarios embebibles\n", count, bytes);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "compile") == 0) return cmd_compile(argv[2], argv[3], 1u);
    if (argc == 4 && strcmp(argv[1], "compile2") == 0) return cmd_compile(argv[2], argv[3], 2u);
    if (argc == 4 && strcmp(argv[1], "compile3") == 0) return cmd_compile(argv[2], argv[3], 3u);
    if (argc == 3 && strcmp(argv[1], "inspect") == 0) return cmd_inspect(argv[2]);
    if (argc == 5 && strcmp(argv[1], "render") == 0) return cmd_render(argv[2], argv[3], argv[4]);
    if (argc == 4 && strcmp(argv[1], "atlas") == 0) return cmd_atlas(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "embed") == 0) return cmd_embed(argv[2], argv[3]);
    usage();
    return 2;
}
