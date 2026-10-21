#include "eye.h"

#include <string.h>

/* No float, OS, heap, shared PRNG, or implementation-defined overflow. */
static uint32_t hash32(uint32_t n) {
    n ^= n >> 16;
    n *= UINT32_C(0x7feb352d);
    n ^= n >> 15;
    n *= UINT32_C(0x846ca68b);
    n ^= n >> 16;
    return n;
}

static uint32_t noise(uint32_t seed, unsigned x, unsigned y, uint32_t salt) {
    return hash32(seed ^ hash32((uint32_t)x + salt) ^
                  hash32((uint32_t)y ^ (salt * UINT32_C(0x9e3779b9))));
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static unsigned wrap(int n) { return (unsigned)n & 63u; }
static uint32_t rgb(int r, int g, int b) {
    return UINT32_C(0xff000000) | ((uint32_t)clamp(r, 0, 255) << 16) |
           ((uint32_t)clamp(g, 0, 255) << 8) | (uint32_t)clamp(b, 0, 255);
}

/* A periodic, integer-only 2D smooth value noise layer; step divides 64. */
static int smooth_noise(uint32_t seed, unsigned x, unsigned y, unsigned step, uint32_t salt) {
    unsigned mask = 64u / step - 1u;
    unsigned gx = x / step, gy = y / step;
    unsigned fx = x % step, fy = y % step;
    unsigned ax = fx * fx * (3u * step - 2u * fx) / (step * step);
    unsigned ay = fy * fy * (3u * step - 2u * fy) / (step * step);
    unsigned c00 = noise(seed, gx & mask, gy & mask, salt) >> 24;
    unsigned c10 = noise(seed, (gx + 1u) & mask, gy & mask, salt) >> 24;
    unsigned c01 = noise(seed, gx & mask, (gy + 1u) & mask, salt) >> 24;
    unsigned c11 = noise(seed, (gx + 1u) & mask, (gy + 1u) & mask, salt) >> 24;
    unsigned a = (c00 * (step - ax) + c10 * ax) / step;
    unsigned b = (c01 * (step - ax) + c11 * ax) / step;
    return (int)((a * (step - ay) + b * ay) / step) - 128;
}

/* Family-specific pigment presets; computed pixels use a restricted light ramp. */
static const uint8_t pigments[8][4][3] = {
    {{122,120,110},{103,112,128},{154,131,103},{108,121,100}}, /* stone */
    {{155,87,67},{174,132,87},{104,100,106},{129,72,57}},     /* brick */
    {{131,85,48},{96,64,47},{158,116,70},{86,70,55}},         /* wood */
    {{118,99,69},{91,98,70},{132,110,90},{99,83,77}},          /* dirt */
    {{129,139,148},{138,117,78},{99,109,110},{150,111,103}},  /* metal */
    {{106,70,125},{112,51,55},{66,92,125},{150,131,101}},     /* cloth */
    {{68,104,51},{109,113,49},{48,91,83},{124,95,52}},         /* foliage */
    {{156,148,124},{136,119,106},{154,147,143},{129,143,120}} /* plaster */
};

int eye_valid(const EyeRecipe *r) {
    return r && r->family < EYE_FAMILY_COUNT && r->palette <= 3u &&
           r->scale >= 1u && r->scale <= 4u && r->pattern <= 7u &&
           r->wear <= 15u && r->growth <= 15u && r->contrast <= 15u && r->flags == 0u;
}

/* Daggerfall-inspired low-resolution motifs, not copies of Bethesda assets. */
uint32_t eye_pixel(const EyeRecipe *r, int px, int py) {
    if (!eye_valid(r)) return 0u;
    unsigned x = wrap(px), y = wrap(py);
    unsigned grain = noise(r->seed, x, y, UINT32_C(0x81)) >> 24;
    unsigned cluster = noise(r->seed, x / 4u, y / 4u, UINT32_C(0x82)) >> 24;
    int macro = smooth_noise(r->seed, x, y, 16, UINT32_C(0x21));
    int medium = smooth_noise(r->seed, x, y, 8, UINT32_C(0x22));
    int fine = smooth_noise(r->seed, x, y, 4, UINT32_C(0x23));
    int light = 0, special = 0;
    const uint8_t *p = pigments[r->family][r->palette];
    int scale = (int)r->scale;
    int pattern = (int)r->pattern;
    int organic = (macro * 3 + medium * 2 + fine) / 6;
    int grit = ((int)grain - 128) / 8;
    int width = 16 >> (scale - 1);
    int period_x, period_y, yy, xx, stagger;
    uint32_t block_hash;

    switch (r->family) {
    case EYE_STONE:
    case EYE_BRICK: {
        int brick = r->family == EYE_BRICK;
        int row_height = brick ? (8 >> (scale - 1)) : (16 >> (scale - 1));
        yy = (int)y % row_height;
        stagger = ((int)y / row_height + pattern) & 1;
        xx = ((int)x + stagger * width / 2) % width;
        period_x = (((int)x + stagger * width / 2) / width) % (64 / width);
        period_y = (int)y / row_height;
        block_hash = noise(r->seed, (unsigned)period_x, (unsigned)period_y, UINT32_C(0x31));
        light = (brick ? 2 : 0) + organic / 3 + grit + (int)((block_hash >> 27) & 15u) - 8;
        if (xx == 0 || yy == 0) { light = -91; special = 1; }
        else if (xx == 1 || yy == 1) light += 17;
        else if (xx == width - 1 || yy == row_height - 1) light -= 23;
        if ((grain < (unsigned)(r->wear * 2u + 2u)) && xx > 1 && yy > 1) light -= 41;
        if ((block_hash & 31u) < 2u && yy > 2 && xx > 2 && ((x ^ y) & 3u) == 0u) light -= 26;
        break;
    }
    case EYE_WOOD: {
        int plank = 16 >> (scale - 1);
        int seam = (int)x % plank;
        int wood_grain = ((int)x * 3 + fine / 4 + medium / 6 + pattern * 3) & 15;
        light = macro / 5 + medium / 3 + grit / 3 + (wood_grain < 3 ? -17 : 3);
        if (seam == 0) light = -78;
        else if (seam == 1) light += 19;
        else if (seam == plank - 1) light -= 25;
        if (((int)y + pattern * 3) % (32 >> (scale - 1)) == 0) light -= 25;
        if (r->wear > 6u && grain < r->wear * 2u) light -= 19;
        break;
    }
    case EYE_DIRT:
        light = organic * 2 / 3 + grit * 2;
        if (grain > 242u && cluster > 130u) light += 39; /* small pebbles */
        if (grain < r->growth * 5u && cluster > 115u) special = 2; /* grass */
        break;
    case EYE_METAL: {
        int plate = 32 >> (scale - 1);
        xx = (int)x % plate; yy = (int)y % plate;
        light = macro / 6 + fine / 5 + grit / 3;
        if (xx == 0 || yy == 0) light = -67;
        else if (xx == 1 || yy == 1) light += 31;
        else if (xx == plate - 1 || yy == plate - 1) light -= 27;
        if ((xx == 3 || xx == plate - 4) && (yy == 3 || yy == plate - 4)) {
            light = -58; special = 1;
        }
        if (grain < r->growth * 6u && cluster > 135u) special = 3;
        break;
    }
    case EYE_CLOTH: {
        unsigned wx = (x + (unsigned)pattern) & 3u;
        unsigned wy = (y + (unsigned)pattern) & 3u;
        light = organic / 4 + grit / 4 + ((wx == 0u) ? 14 : -4) + ((wy == 0u) ? -15 : 4);
        if ((x % (unsigned)(16 >> (scale - 1))) == 0u) light -= 18;
        if ((y % (unsigned)(16 >> (scale - 1))) == 0u) light -= 12;
        if (r->wear > 7u && grain < r->wear * 2u) light -= 30;
        break;
    }
    case EYE_FOLIAGE:
        light = organic / 2 + grit;
        if (cluster < 50u && macro < 20) light -= 42;
        if ((grain & 7u) == 0u) light += 33;
        if ((x + (unsigned)pattern * 3u + (unsigned)(fine + 128) / 16u) % (unsigned)(16 >> (scale - 1)) == 0u)
            light -= 24;
        break;
    case EYE_PLASTER:
        light = organic / 3 + fine / 5 + grit / 2;
        if (grain < r->wear + 2u && cluster > 80u) light -= 49;
        if (r->growth > 3u && cluster < r->growth * 6u) special = 2;
        break;
    default: return 0u;
    }

    /* This material ramp is intentionally quantized to evoke indexed VGA art. */
    light = (light * (8 + (int)r->contrast)) / 16;
    light = clamp(light, -110, 70);
    light = (light / 9) * 9;
    int rr = (int)p[0] + light;
    int gg = (int)p[1] + light;
    int bb = (int)p[2] + light;
    if (special == 1) { rr -= 17; gg -= 17; bb -= 13; }
    if (special == 2) { rr = rr * 2 / 3; gg = gg * 3 / 4 + 25; bb = bb * 2 / 3; }
    if (special == 3) { rr += 30; gg -= 23; bb -= 26; }
    if (r->growth > 0u && (r->family == EYE_STONE || r->family == EYE_BRICK || r->family == EYE_WOOD)) {
        unsigned mask = noise(r->seed, x / 4u, y / 4u, UINT32_C(0x72)) >> 24;
        int chance = (int)(r->growth * 9u) + (organic / 2);
        if (mask < (unsigned)clamp(chance, 0, 170) && (y > 32u || special == 1)) {
            rr = rr * 2 / 3;
            gg = gg * 4 / 5 + 18;
            bb = bb * 2 / 3;
        }
    }
    return rgb(rr, gg, bb);
}

int eye_generate(const EyeRecipe *r, uint32_t *rgba, size_t pixel_capacity) {
    unsigned y, x;
    if (!eye_valid(r) || !rgba || pixel_capacity < EYE_SIZE * EYE_SIZE) return 0;
    for (y = 0; y < EYE_SIZE; ++y)
        for (x = 0; x < EYE_SIZE; ++x)
            rgba[y * EYE_SIZE + x] = eye_pixel(r, (int)x, (int)y);
    return 1;
}

static uint32_t checksum(const uint8_t *bytes, size_t count) {
    uint32_t h = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < count; ++i) { h ^= bytes[i]; h *= UINT32_C(16777619); }
    return h;
}

static void put_u32(uint8_t *p, uint32_t n) {
    p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8);
    p[2] = (uint8_t)(n >> 16); p[3] = (uint8_t)(n >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t eye_pack_write(const EyeRecipe *recipes, unsigned count, uint8_t *dst, size_t capacity) {
    size_t bytes, i;
    if (!recipes || !dst || count == 0u || count > EYE_MAX_RECIPES) return 0;
    bytes = EYE_HEADER_BYTES + count * EYE_RECIPE_BYTES;
    if (capacity < bytes) return 0;
    for (i = 0; i < count; ++i) if (!eye_valid(&recipes[i])) return 0;
    dst[0] = 'E'; dst[1] = 'Y'; dst[2] = 'E'; dst[3] = '1';
    dst[4] = EYE_RECIPE_BYTES; dst[5] = (uint8_t)count;
    dst[6] = 0; dst[7] = 0;
    for (i = 0; i < count; ++i) {
        const EyeRecipe *r = &recipes[i];
        uint8_t *p = &dst[EYE_HEADER_BYTES + i * EYE_RECIPE_BYTES];
        p[0] = r->family; p[1] = r->palette; p[2] = r->scale; p[3] = r->pattern;
        p[4] = r->wear; p[5] = r->growth; p[6] = r->contrast; p[7] = r->flags;
        put_u32(p + 8, r->seed);
    }
    put_u32(dst + 8, checksum(dst + EYE_HEADER_BYTES, bytes - EYE_HEADER_BYTES));
    return bytes;
}

int eye_pack_read(const uint8_t *data, size_t bytes, EyeRecipe *out,
                  unsigned capacity, unsigned *count) {
    unsigned n, i;
    if (!data || !out || !count || bytes < EYE_HEADER_BYTES) return 0;
    if (memcmp(data, "EYE1", 4) != 0 || data[4] != EYE_RECIPE_BYTES ||
        data[6] != 0 || data[7] != 0) return 0;
    n = data[5];
    if (n == 0u || n > capacity || bytes != EYE_HEADER_BYTES + n * EYE_RECIPE_BYTES ||
        get_u32(data + 8) != checksum(data + EYE_HEADER_BYTES, bytes - EYE_HEADER_BYTES)) return 0;
    /* Validate every record prior to writing any output recipe. */
    for (i = 0; i < n; ++i) {
        const uint8_t *p = data + EYE_HEADER_BYTES + i * EYE_RECIPE_BYTES;
        EyeRecipe r = {p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],get_u32(p + 8)};
        if (!eye_valid(&r)) return 0;
    }
    for (i = 0; i < n; ++i) {
        const uint8_t *p = data + EYE_HEADER_BYTES + i * EYE_RECIPE_BYTES;
        EyeRecipe r = {p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],get_u32(p + 8)};
        out[i] = r;
    }
    *count = n;
    return 1;
}

uint64_t eye_hash_pixels(const uint32_t *pixels, size_t count) {
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    unsigned j;
    if (!pixels) return 0;
    for (i = 0; i < count; ++i)
        for (j = 0; j < 4; ++j) {
            h ^= (uint8_t)(pixels[i] >> (8u * j));
            h *= UINT64_C(1099511628211);
        }
    return h;
}

static const char *const families[] = {
    "stone", "brick", "wood", "dirt", "metal", "cloth", "foliage", "plaster"
};
const char *eye_family_name(unsigned f) { return f < EYE_FAMILY_COUNT ? families[f] : "unknown"; }
int eye_family_from_name(const char *name) {
    unsigned i;
    if (!name) return -1;
    for (i = 0; i < EYE_FAMILY_COUNT; ++i)
        if (strcmp(name, families[i]) == 0) return (int)i;
    return -1;
}
