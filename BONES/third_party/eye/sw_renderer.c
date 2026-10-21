#include "sw_renderer.h"

#include <math.h>
#include <string.h>

typedef struct { float x, y, z, u, v; } ViewVertex;
typedef struct { float x, y, invz, uinvz, vinvz; } ScreenVertex;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= UINT32_C(0x7feb352d);
    x ^= x >> 15;
    x *= UINT32_C(0x846ca68b);
    x ^= x >> 16;
    return x;
}

static uint32_t rgb(unsigned r, unsigned g, unsigned b) {
    return UINT32_C(0xff000000) | ((r & 255u) << 16) | ((g & 255u) << 8) | (b & 255u);
}

static int intmax(int a, int b) { return a > b ? a : b; }
static int intmin(int a, int b) { return a < b ? a : b; }

/* Pure, versioned-by-code material synthesis; no rand() or global seed. */
static uint32_t material_pixel(uint32_t seed, int material, int x, int y) {
    unsigned n = hash32(seed ^ (uint32_t)(x * 2654435761u) ^
                        (uint32_t)(y * 2246822519u) ^ (uint32_t)material);
    unsigned grain = (n >> 24) & 15u;
    if (material == P144_MAT_STONE) {
        int row = y / 16;
        int shifted = (x + (row & 1) * 8) & 15;
        unsigned block = hash32(seed ^ (uint32_t)(row * 13 + (x + (row & 1) * 8) / 16 * 123));
        if ((y & 15) <= 1 || shifted <= 1) return rgb(44 + grain / 2, 42 + grain / 2, 40 + grain / 2);
        unsigned grey = 87u + (block & 31u) + grain;
        return rgb(grey, grey - 5u, grey - 12u);
    }
    if (material == P144_MAT_FLOOR) {
        int line = (y & 15) <= 1 || ((x + ((y / 16) & 1) * 16) & 31) <= 1;
        if (line) return rgb(47, 46, 43);
        unsigned value = 82u + (hash32(seed ^ (uint32_t)((x / 32) * 73 + (y / 16) * 59)) & 31u) + grain;
        return rgb(value, value - 8u, value - 16u);
    }
    if ((y & 15) < 2) return rgb(47, 28, 15);
    unsigned lines = ((unsigned)(x + (int)(n & 3u)) & 7u) == 0u ? 17u : 0u;
    return rgb(106u + grain - lines, 67u + grain / 2u - lines / 2u, 37u + grain / 3u);
}

void p144_renderer_init(P144_Renderer *r, uint32_t seed) {
    int material, y, x;
    memset(r, 0, sizeof(*r));
    r->seed = seed;
    for (material = 0; material < P144_MAT_COUNT; ++material)
        for (y = 0; y < P144_TEX_SIZE; ++y)
            for (x = 0; x < P144_TEX_SIZE; ++x)
                r->tex[material][y * P144_TEX_SIZE + x] = material_pixel(seed, material, x, y);
}

void p144_renderer_begin(P144_Renderer *r, uint32_t clear_rgb) {
    int i;
    for (i = 0; i < P144_VIEW_WIDTH * P144_VIEW_HEIGHT; ++i) {
        r->pixels[i] = UINT32_C(0xff000000) | (clear_rgb & UINT32_C(0xffffff));
        r->depth[i] = 0.0f; /* closer surfaces have a larger inverse-z. */
    }
    r->triangles = 0;
}

static ViewVertex world_to_view(const P144_Camera *c, P144_Vertex in) {
    float sy = sinf(c->yaw), cy = cosf(c->yaw);
    float sp = sinf(c->pitch), cp = cosf(c->pitch);
    float dx = in.pos.x - c->pos.x;
    float dy = in.pos.y - c->pos.y;
    float dz = in.pos.z - c->pos.z;
    float x = cy * dx - sy * dz;
    float z = sy * dx + cy * dz;
    ViewVertex out;
    out.x = x;
    out.y = cp * dy - sp * z;
    out.z = sp * dy + cp * z;
    out.u = in.u;
    out.v = in.v;
    return out;
}

static ViewVertex lerp_view(ViewVertex a, ViewVertex b, float t) {
    ViewVertex v;
    v.x = a.x + (b.x - a.x) * t;
    v.y = a.y + (b.y - a.y) * t;
    v.z = a.z + (b.z - a.z) * t;
    v.u = a.u + (b.u - a.u) * t;
    v.v = a.v + (b.v - a.v) * t;
    return v;
}

static ScreenVertex project(ViewVertex v) {
    const float focal = 208.5f; /* about 75 degrees horizontal FOV */
    ScreenVertex s;
    s.invz = 1.0f / v.z;
    s.x = (float)P144_VIEW_WIDTH * 0.5f + v.x * focal * s.invz;
    s.y = (float)P144_VIEW_HEIGHT * 0.5f - v.y * focal * s.invz;
    s.uinvz = v.u * s.invz;
    s.vinvz = v.v * s.invz;
    return s;
}

static float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
}

static uint32_t sample(P144_Renderer *r, int material, float u, float v) {
    /* Repeating UVs, including when UV is negative. */
    int tx = (int)floorf(u * (float)P144_TEX_SIZE) & (P144_TEX_SIZE - 1);
    int ty = (int)floorf(v * (float)P144_TEX_SIZE) & (P144_TEX_SIZE - 1);
    return r->tex[material][ty * P144_TEX_SIZE + tx];
}

static void raster_triangle(P144_Renderer *r, ScreenVertex a, ScreenVertex b,
                            ScreenVertex c, int material) {
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (fabsf(area) < 0.0001f) return;
    float minfx = fminf(a.x, fminf(b.x, c.x));
    float maxfx = fmaxf(a.x, fmaxf(b.x, c.x));
    float minfy = fminf(a.y, fminf(b.y, c.y));
    float maxfy = fmaxf(a.y, fmaxf(b.y, c.y));
    /* Clamp prior to float->int conversion: important for offscreen vertices. */
    if (maxfx < 0.0f || maxfy < 0.0f || minfx >= P144_VIEW_WIDTH || minfy >= P144_VIEW_HEIGHT)
        return;
    int x0 = intmax(0, (int)floorf(fmaxf(-1.0f, minfx)));
    int x1 = intmin(P144_VIEW_WIDTH - 1, (int)ceilf(fminf(P144_VIEW_WIDTH, maxfx)));
    int y0 = intmax(0, (int)floorf(fmaxf(-1.0f, minfy)));
    int y1 = intmin(P144_VIEW_HEIGHT - 1, (int)ceilf(fminf(P144_VIEW_HEIGHT, maxfy)));
    float inv_area = 1.0f / area;
    int x, y;
    ++r->triangles;
    for (y = y0; y <= y1; ++y) {
        for (x = x0; x <= x1; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = edge(b.x, b.y, c.x, c.y, px, py) * inv_area;
            float w1 = edge(c.x, c.y, a.x, a.y, px, py) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.00001f || w1 < -0.00001f || w2 < -0.00001f) continue;
            float invz = w0 * a.invz + w1 * b.invz + w2 * c.invz;
            int offset = y * P144_VIEW_WIDTH + x;
            if (invz <= r->depth[offset] || invz <= 0.0f) continue;
            float u = (w0 * a.uinvz + w1 * b.uinvz + w2 * c.uinvz) / invz;
            float v = (w0 * a.vinvz + w1 * b.vinvz + w2 * c.vinvz) / invz;
            r->pixels[offset] = sample(r, material, u, v);
            r->depth[offset] = invz;
        }
    }
}

/* Clip one triangle to near z=0.08, then triangulate the resulting polygon. */
static void draw_view_triangle(P144_Renderer *r, ViewVertex a, ViewVertex b,
                               ViewVertex c, int material) {
    const float near_z = 0.08f;
    ViewVertex in[5] = {a, b, c};
    ViewVertex out[5];
    int count = 3, next_count = 0, i;
    for (i = 0; i < count; ++i) {
        ViewVertex current = in[i], previous = in[(i + count - 1) % count];
        int inside = current.z >= near_z, previous_inside = previous.z >= near_z;
        if (inside != previous_inside) {
            float t = (near_z - previous.z) / (current.z - previous.z);
            out[next_count++] = lerp_view(previous, current, t);
        }
        if (inside) out[next_count++] = current;
    }
    if (next_count < 3) return;
    for (i = 1; i + 1 < next_count; ++i)
        raster_triangle(r, project(out[0]), project(out[i]), project(out[i + 1]), material);
}

void p144_renderer_quad(P144_Renderer *r, const P144_Camera *camera,
                        const P144_Quad *quad, P144_Material material) {
    ViewVertex v[4];
    int i;
    if (!r || !camera || !quad || material < 0 || material >= P144_MAT_COUNT) return;
    for (i = 0; i < 4; ++i) v[i] = world_to_view(camera, quad->v[i]);
    draw_view_triangle(r, v[0], v[1], v[2], material);
    draw_view_triangle(r, v[0], v[2], v[3], material);
}

static void draw_sprite_triangle(P144_Renderer *r, ScreenVertex a, ScreenVertex b,
                                 ScreenVertex c, uint32_t ticks) {
    float area = edge(a.x, a.y, b.x, b.y, c.x, c.y);
    if (fabsf(area) < 0.0001f) return;
    float minfx = fminf(a.x, fminf(b.x, c.x));
    float maxfx = fmaxf(a.x, fmaxf(b.x, c.x));
    float minfy = fminf(a.y, fminf(b.y, c.y));
    float maxfy = fmaxf(a.y, fmaxf(b.y, c.y));
    if (maxfx < 0.0f || maxfy < 0.0f || minfx >= P144_VIEW_WIDTH || minfy >= P144_VIEW_HEIGHT) return;
    int x0 = intmax(0, (int)floorf(fmaxf(-1.0f, minfx)));
    int x1 = intmin(P144_VIEW_WIDTH - 1, (int)ceilf(fminf(P144_VIEW_WIDTH, maxfx)));
    int y0 = intmax(0, (int)floorf(fmaxf(-1.0f, minfy)));
    int y1 = intmin(P144_VIEW_HEIGHT - 1, (int)ceilf(fminf(P144_VIEW_HEIGHT, maxfy)));
    float inv_area = 1.0f / area;
    int x, y;
    ++r->triangles;
    for (y = y0; y <= y1; ++y) {
        for (x = x0; x <= x1; ++x) {
            float px = (float)x + .5f, py = (float)y + .5f;
            float w0 = edge(b.x, b.y, c.x, c.y, px, py) * inv_area;
            float w1 = edge(c.x, c.y, a.x, a.y, px, py) * inv_area;
            float w2 = 1.0f - w0 - w1;
            if (w0 < -0.00001f || w1 < -0.00001f || w2 < -0.00001f) continue;
            float invz = w0 * a.invz + w1 * b.invz + w2 * c.invz;
            int offset = y * P144_VIEW_WIDTH + x;
            if (invz <= r->depth[offset] || invz <= 0.0f) continue;
            float u = (w0 * a.uinvz + w1 * b.uinvz + w2 * c.uinvz) / invz;
            float v = (w0 * a.vinvz + w1 * b.vinvz + w2 * c.vinvz) / invz;
            float sx = (u - .5f) * 2.0f, sy = (v - .5f) * 2.0f;
            /* Transparent background; a stylized robed humanoid, not an RPG NPC. */
            int head = sx * sx / .28f + (sy + .53f) * (sy + .53f) / .13f < 1.0f;
            int hood = sx * sx / .38f + (sy + .44f) * (sy + .44f) / .22f < 1.0f;
            int torso = (sy > -.30f && sy < .63f && fabsf(sx) < .32f + .25f * (sy + .3f));
            int arms = sy > -.12f && sy < .30f && fabsf(sx) < .79f;
            int body = hood || torso || arms;
            if (!body) continue;
            uint32_t color = rgb(57, 62, 80);
            if (torso) color = rgb(105, 52 + (ticks / 25u) % 8u, 51);
            if (head) color = rgb(167, 147, 111);
            if (hood && !head) color = rgb(52, 45, 61);
            if (head && sy < -.51f && fabsf(sx) > .12f && fabsf(sx) < .23f) color = rgb(22, 19, 15);
            if (sy > .53f && fabsf(sx) < .08f) color = rgb(38, 27, 24);
            r->pixels[offset] = color;
            r->depth[offset] = invz;
        }
    }
}

void p144_renderer_billboard(P144_Renderer *r, const P144_Camera *camera,
                             P144_Vec3 foot, float width, float height,
                             uint32_t ticks) {
    float dx = cosf(camera->yaw) * width * .5f;
    float dz = -sinf(camera->yaw) * width * .5f;
    P144_Vertex quad[4] = {
        {{foot.x - dx, foot.y + height, foot.z - dz}, 0, 0},
        {{foot.x + dx, foot.y + height, foot.z + dz}, 1, 0},
        {{foot.x + dx, foot.y, foot.z + dz}, 1, 1},
        {{foot.x - dx, foot.y, foot.z - dz}, 0, 1}
    };
    ViewVertex v[4];
    int i;
    for (i = 0; i < 4; ++i) v[i] = world_to_view(camera, quad[i]);
    /* Billboard is omitted when near plane crosses it; quad clipping is TODO. */
    for (i = 0; i < 4; ++i) if (v[i].z < .08f) return;
    draw_sprite_triangle(r, project(v[0]), project(v[1]), project(v[2]), ticks);
    draw_sprite_triangle(r, project(v[0]), project(v[2]), project(v[3]), ticks);
}

void p144_renderer_crosshair(P144_Renderer *r) {
    int cx = P144_VIEW_WIDTH / 2, cy = P144_VIEW_HEIGHT / 2;
    int d;
    for (d = -4; d <= 4; ++d) {
        if (d == 0) continue;
        r->pixels[cy * P144_VIEW_WIDTH + (cx + d)] = UINT32_C(0xffe5dabe);
        r->pixels[(cy + d) * P144_VIEW_WIDTH + cx] = UINT32_C(0xffe5dabe);
    }
}

uint64_t p144_renderer_image_hash(const P144_Renderer *r) {
    uint64_t h = UINT64_C(14695981039346656037);
    int i, byte;
    for (i = 0; i < P144_VIEW_WIDTH * P144_VIEW_HEIGHT; ++i) {
        uint32_t value = r->pixels[i];
        for (byte = 0; byte < 4; ++byte) {
            h ^= (uint8_t)(value >> (byte * 8));
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}
