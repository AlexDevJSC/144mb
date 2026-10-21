#include "eye2.h"
#include "eye3.h"

#include <string.h>

/* Intentionally integer-only: portable outputs and no floating-point rounding. */
static uint32_t mix32(uint32_t n) {
    n ^= n >> 16;
    n *= UINT32_C(0x7feb352d);
    n ^= n >> 15;
    n *= UINT32_C(0x846ca68b);
    n ^= n >> 16;
    return n;
}
static unsigned rnd(uint32_t seed, unsigned x, unsigned y, uint32_t domain) {
    return mix32(seed ^ mix32((uint32_t)x + domain) ^
                 mix32((uint32_t)y + domain * UINT32_C(0x9e3779b9))) >> 24;
}
static int ci(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int absi(int x) { return x < 0 ? -x : x; }
static uint32_t color(int r, int g, int b) {
    return UINT32_C(0xff000000) | ((uint32_t)ci(r, 0, 255) << 16) |
           ((uint32_t)ci(g, 0, 255) << 8) | (uint32_t)ci(b, 0, 255);
}
/* Interpolation uses fractional values in 0..256; 64-periodic in both axes. */
static int layer(uint32_t seed, unsigned x, unsigned y, unsigned size,
                 uint32_t domain) {
    const unsigned count = 64u / size;
    unsigned gx = x / size, gy = y / size;
    unsigned fx = x % size, fy = y % size;
    unsigned sx = fx * 256u / size, sy = fy * 256u / size;
    sx = (sx * sx * (768u - 2u * sx)) >> 16;
    sy = (sy * sy * (768u - 2u * sy)) >> 16;
    int a = (int)rnd(seed, gx % count, gy % count, domain) - 128;
    int b = (int)rnd(seed, (gx + 1u) % count, gy % count, domain) - 128;
    int c = (int)rnd(seed, gx % count, (gy + 1u) % count, domain) - 128;
    int d = (int)rnd(seed, (gx + 1u) % count, (gy + 1u) % count, domain) - 128;
    int ab = (a * (int)(256u - sx) + b * (int)sx) / 256;
    int cd = (c * (int)(256u - sx) + d * (int)sx) / 256;
    return (ab * (int)(256u - sy) + cd * (int)sy) / 256;
}
static const uint8_t pigment[8][4][3] = {
    {{121,117,104},{104,112,117},{152,132,106},{106,116,92}},
    {{152,79,57},{164,126,84},{107,108,108},{118,65,55}},
    {{126,78,44},{83,54,38},{151,107,67},{105,88,66}},
    {{109,91,67},{85,98,68},{130,105,80},{98,79,74}},
    {{127,141,150},{134,115,80},{101,111,108},{140,103,96}},
    {{104,75,127},{123,52,60},{63,88,118},{141,127,92}},
    {{58,96,47},{118,112,46},{40,93,75},{124,88,50}},
    {{150,146,126},{127,113,100},{165,155,144},{129,144,120}}
};

/* Tile-local cracks: jittered polyline, derived from cell ID, not pixel traversal. */
static int crack_line(uint32_t seed, unsigned cx, unsigned cy, int lx, int ly,
                      int width, int height) {
    unsigned a = rnd(seed, cx, cy, 0x9879u);
    if (a < 130u) return 0;
    int start = 2 + (int)(rnd(seed, cx, cy, 0x9c21u) % (unsigned)(width - 3));
    int end = 2 + (int)(rnd(seed, cx, cy, 0x9521u) % (unsigned)(width - 3));
    int t = ci(ly - 2, 0, height - 4);
    int line_x = (start * (height - 4 - t) + end * t) / (height - 4);
    line_x += (int)(rnd(seed, cx + (unsigned)(ly / 3), cy, 0x9d11u) % 3u) - 1;
    return ly >= 2 && ly < height - 2 && absi(lx - line_x) == 0;
}

uint32_t eye2_pixel(const EyeRecipe *r, int px, int py) {
    if (!eye_valid(r)) return 0u;
    unsigned x = (unsigned)px & 63u, y = (unsigned)py & 63u;
    const uint8_t *p = pigment[r->family][r->palette];
    unsigned n = rnd(r->seed, x, y, 0xa77u);
    unsigned d = rnd(r->seed, x >> 2, y >> 2, 0xb92u);
    int n16 = layer(r->seed, x, y, 16u, 0xc21u);
    int n8  = layer(r->seed, x, y, 8u, 0xc22u);
    int n4  = layer(r->seed, x, y, 4u, 0xc23u);
    int n2  = layer(r->seed, x, y, 2u, 0xc24u);
    int rough = (n16 * 5 + n8 * 4 + n4 * 2 + n2) / 12;
    int spec = 0, shade = 0;
    int form = (int)r->scale;
    int wear = (int)r->wear;
    int growth = (int)r->growth;
    int pattern = (int)r->pattern;
    /* Palette shading: not a white-noise overlay, but a family-specific relief. */
    int rbase = (int)p[0], gbase = (int)p[1], bbase = (int)p[2];
    switch (r->family) {
    case EYE_STONE:
    case EYE_BRICK: {
        int brick = r->family == EYE_BRICK;
        /* Cell dimensions tile a 64x64 square perfectly; course staggering. */
        int cw = (brick ? 32 : 32) >> (form - 1);
        int ch = (brick ? 16 : 16) >> (form - 1);
        int row = (int)y / ch;
        int stagger = (row + pattern) & 1;
        int ax = ((int)x + stagger * (cw / 2)) & 63;
        int col = ax / cw, lx = ax % cw, ly = (int)y % ch;
        unsigned material = rnd(r->seed, (unsigned)col, (unsigned)row, 0x416u);
        unsigned fleck = rnd(r->seed, (unsigned)col, (unsigned)row, 0x418u);
        int inset = 1 + (((int)n8 > 24 && wear > 9) ? 1 : 0);
        int edge = lx < inset || ly < inset || lx >= cw - inset || ly >= ch - inset;
        /* Slightly individual blocks, direction of light from above-left. */
        shade = (int)(material & 31u) - 16 + rough / 4 + n4 / 5 + ((int)n - 128) / 16;
        if (edge) {
            shade -= 67;
            if (lx == 0 || ly == 0) shade -= 9;
            if (wear > 7 && ((n + fleck) & 15u) < (unsigned)((wear - 7) / 2)) shade += 27;
        } else {
            if (lx == inset || ly == inset) shade += 25;
            if (lx == cw - inset - 1 || ly == ch - inset - 1) shade -= 19;
            if (wear > 3 && d < (unsigned)(wear * 3) && rough < 0) shade -= 22;
            if (cw >= 8 && ch >= 8 && crack_line(r->seed, (unsigned)col,
                (unsigned)row, lx, ly, cw, ch) && wear >= 7) shade -= 41;
        }
        if (brick) { rbase += 5; gbase -= 1; }
        if (growth > 0 && n16 < growth * 5 - 75 && d < (unsigned)(growth * 9)) {
            spec = 1; /* clustered moss/efflorescence */
        }
        break;
    }
    case EYE_WOOD: {
        int width = 32 >> (form - 1);
        int lx = (int)x % width;
        int plank = (int)x / width;
        unsigned cell = rnd(r->seed, (unsigned)plank, 0, 0x510u);
        int warp = (n8 * 3 + n4 * 2) / 40;
        int bands = (((int)x * 5 + warp + pattern * 3) & 31);
        int knot_y = 8 + (int)(rnd(r->seed, (unsigned)plank, 1, 0x511u) & 31u);
        int knot_x = width / 2 + (int)(cell & 3u) - 2;
        int dx = (lx - knot_x) * 3, dy = (int)y - knot_y;
        int ring = absi(dx * dx + dy * dy * 2 - 45);
        shade = (int)(cell & 31u) - 16 + rough / 7 +
                (bands < 5 ? -23 : bands < 9 ? 10 : 2) + ((int)n - 128) / 18;
        if (lx == 0) shade -= 77;
        else if (lx == 1) shade += 26;
        else if (lx == width - 1) shade -= 22;
        if (ring < 27 && (cell & 4u)) shade -= 35;
        if (dx * dx + dy * dy * 2 < 14 && (cell & 4u)) shade -= 68;
        if (((int)y + (plank & 1) * 32) % 64 == 0) shade -= 35;
        if (wear > 8 && d < (unsigned)(wear * 4) && n8 > 0) shade -= 22;
        break;
    }
    case EYE_DIRT: {
        unsigned st = rnd(r->seed, x >> 3, y >> 3, 0x610u);
        int peb_x = 1 + (int)(st & 7u), peb_y = 1 + (int)((st >> 3) & 7u);
        int dx = (int)(x & 7u) - peb_x, dy = (int)(y & 7u) - peb_y;
        shade = rough / 2 + n4 / 4 + ((int)n - 128) / 6;
        if (dx * dx + dy * dy <= 2 && st > 145u) { shade += 40; spec = 2; }
        if (growth && d < (unsigned)(growth * 11) && n8 > -20 &&
            (x & 3u) == ((y + st) & 3u)) spec = 1;
        if (wear > 9 && n16 < -35 && n4 < -25) shade -= 19;
        break;
    }
    case EYE_METAL: {
        int plate = 32 >> (form - 1), lx = (int)x % plate, ly = (int)y % plate;
        int bevel = lx == 0 || ly == 0 || lx == plate - 1 || ly == plate - 1;
        shade = rough / 4 + n4 / 4 + ((int)n - 128) / 12;
        if (bevel) shade = -64;
        else if (lx == 1 || ly == 1) shade += 34;
        else if (lx == plate - 2 || ly == plate - 2) shade -= 27;
        if (plate >= 8 && (lx == 3 || lx == plate - 4) &&
            (ly == 3 || ly == plate - 4)) { shade -= 52; spec = 3; }
        if (growth && n8 < growth * 6 - 85 && d < (unsigned)(growth * 9)) spec = 4;
        break;
    }
    case EYE_CLOTH: {
        int wx = ((int)x + pattern) & 3, wy = ((int)y + pattern) & 3;
        int gap = 32 >> (form - 1);
        shade = rough / 5 + (wx < 2 ? 11 : -11) + (wy < 2 ? -12 : 10);
        if ((int)x % gap < 2 || (int)y % gap < 2) shade -= 22;
        /* Alternating diagonal brocade flecks; pattern controls geometry. */
        if ((pattern & 1) && (((x + y) & 15u) == 0u)) shade += 25;
        if (wear > 6 && d < (unsigned)(wear * 3) && n4 < -20) shade -= 34;
        break;
    }
    case EYE_FOLIAGE: {
        unsigned cell = rnd(r->seed, x >> 3, y >> 3, 0x810u);
        int lx = (int)(x & 7u), ly = (int)(y & 7u);
        int cx = 1 + (int)(cell & 7u), cy = 1 + (int)((cell >> 3) & 7u);
        int dist = (lx - cx) * (lx - cx) + (ly - cy) * (ly - cy);
        shade = rough / 2 + n4 / 3 + ((int)n - 128) / 10;
        if (dist < 14) shade += 22;
        else shade -= 28;
        if (absi(lx - ly) <= 1 && dist < 12) shade += 14;
        if (dist < 4 && cell > 140u) spec = 1;
        break;
    }
    case EYE_PLASTER: {
        shade = rough / 3 + n4 / 3 + ((int)n - 128) / 10;
        if (wear > 5 && d < (unsigned)(wear * 7) &&
            n16 + n8 / 3 < wear * 3 - 85) { shade -= 29; spec = 5; }
        if (wear > 8) {
            int crack = (int)((x * 3u + y * 5u + (unsigned)(n8 + 128) / 4u) & 63u);
            if (crack == 0 && n16 < 20) shade -= 65;
        }
        if (growth > 4 && n16 < growth * 5 - 75 && d < (unsigned)(growth * 8)) spec = 1;
        break;
    }
    default: return 0;
    }
    shade = ci(shade * (9 + (int)r->contrast) / 17, -98, 75);
    shade = (shade / 7) * 7; /* VGA style limited luminance ramp. */
    if (spec == 1) { rbase = rbase * 2 / 3; gbase = gbase * 3 / 4 + 30; bbase = bbase * 2 / 3; }
    if (spec == 2) { rbase += 27; gbase += 28; bbase += 25; }
    if (spec == 3) { rbase -= 25; gbase -= 19; bbase -= 15; }
    if (spec == 4) { rbase += 26; gbase -= 20; bbase -= 20; }
    if (spec == 5) { rbase -= 16; gbase -= 11; bbase -= 7; }
    return color(rbase + shade, gbase + shade, bbase + shade);
}

int eye2_generate(const EyeRecipe *recipe, uint32_t *pixels, size_t capacity) {
    unsigned x, y;
    if (!eye_valid(recipe) || !pixels || capacity < EYE_SIZE * EYE_SIZE) return 0;
    for (y = 0; y < EYE_SIZE; ++y)
        for (x = 0; x < EYE_SIZE; ++x)
            pixels[y * EYE_SIZE + x] = eye2_pixel(recipe, (int)x, (int)y);
    return 1;
}
/* EYE2 integrity binds BOTH format identification and recipe bytes.
 * EYE1 historical packs use a payload-only checksum; do not alter them. */
static uint32_t checksum2(const uint8_t *p, size_t n) {
    uint32_t h = UINT32_C(2166136261);
    size_t i;
    for (i = 0; i < n; ++i) {
        if (i >= 8u && i < 12u) continue; /* checksum field */
        h ^= p[i];
        h *= UINT32_C(16777619);
    }
    return h;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
size_t eye2_pack_write(const EyeRecipe *recipes, unsigned count,
                       uint8_t *dst, size_t cap) {
    size_t len, i;
    if (!recipes || !dst || !count || count > EYE_MAX_RECIPES) return 0;
    len = EYE_HEADER_BYTES + count * EYE_RECIPE_BYTES;
    if (cap < len) return 0;
    for (i = 0; i < count; ++i) if (!eye_valid(&recipes[i])) return 0;
    dst[0] = 'E'; dst[1] = 'Y'; dst[2] = 'E'; dst[3] = '2';
    dst[4] = (uint8_t)EYE_RECIPE_BYTES; dst[5] = (uint8_t)count;
    dst[6] = 0; dst[7] = 0;
    for (i = 0; i < count; ++i) {
        const EyeRecipe *r = recipes + i;
        uint8_t *p = dst + EYE_HEADER_BYTES + EYE_RECIPE_BYTES * i;
        p[0] = r->family; p[1] = r->palette; p[2] = r->scale;
        p[3] = r->pattern; p[4] = r->wear; p[5] = r->growth;
        p[6] = r->contrast; p[7] = r->flags;
        put32(p + 8, r->seed);
    }
    put32(dst + 8, checksum2(dst, len));
    return len;
}
int eye2_pack_read(const uint8_t *data, size_t len, EyeRecipe *out,
                   unsigned capacity, unsigned *count) {
    unsigned n, i;
    if (!data || !out || !count || len < EYE_HEADER_BYTES || memcmp(data,"EYE2",4) ||
        data[4] != EYE_RECIPE_BYTES || data[6] || data[7]) return 0;
    n = data[5];
    if (!n || n > capacity || len != EYE_HEADER_BYTES + EYE_RECIPE_BYTES * (size_t)n ||
        get32(data + 8) != checksum2(data, len)) return 0;
    for (i = 0; i < n; ++i) {
        const uint8_t *p = data + EYE_HEADER_BYTES + EYE_RECIPE_BYTES * (size_t)i;
        EyeRecipe r = {p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],get32(p+8)};
        if (!eye_valid(&r)) return 0; /* transactional: no output mutated on failure */
    }
    for (i = 0; i < n; ++i) {
        const uint8_t *p = data + EYE_HEADER_BYTES + EYE_RECIPE_BYTES * (size_t)i;
        EyeRecipe r = {p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],get32(p+8)};
        out[i] = r;
    }
    *count = n;
    return 1;
}
int eye2_material_from_embedded(const uint8_t *pack, size_t bytes,
                                unsigned material_index,
                                uint32_t *pixels, size_t capacity) {
    EyeRecipe recipes[EYE_MAX_RECIPES];
    unsigned count = 0;
    if (!pixels || material_index >= EYE_MAX_RECIPES ||
        !eye2_pack_read(pack, bytes, recipes, EYE_MAX_RECIPES, &count) ||
        material_index >= count) return 0;
    return eye2_generate(&recipes[material_index], pixels, capacity);
}

int eye_bank_open(EyeTextureBank *bank, const uint8_t *data, size_t length) {
    EyeTextureBank tmp;
    unsigned version = 0;
    if (!bank || !data || length < EYE_HEADER_BYTES) return 0;
    if (!memcmp(data, "EYE1", 4)) version = 1;
    else if (!memcmp(data, "EYE2", 4)) version = 2;
    else if (!memcmp(data, "EYE3", 4)) version = 3;
    if (!version) return 0;
    if (!(version == 1 ? eye_pack_read(data, length, tmp.recipes, EYE_MAX_RECIPES, &tmp.count) :
          version == 2 ? eye2_pack_read(data, length, tmp.recipes, EYE_MAX_RECIPES, &tmp.count) :
                         eye3_pack_read(data, length, tmp.recipes, EYE_MAX_RECIPES, &tmp.count))) return 0;
    tmp.version = version;
    *bank = tmp;
    return 1;
}
int eye_bank_render(const EyeTextureBank *bank, unsigned index, uint32_t *pixels, size_t cap) {
    if (!bank || index >= bank->count || !pixels) return 0;
    if (bank->version == 1) return eye_generate(&bank->recipes[index], pixels, cap);
    if (bank->version == 2) return eye2_generate(&bank->recipes[index], pixels, cap);
    if (bank->version == 3) return eye3_generate(&bank->recipes[index], pixels, cap);
    return 0;
}
