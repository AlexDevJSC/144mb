#ifndef P144_SW_RENDERER_H
#define P144_SW_RENDERER_H

#include <stdint.h>

#define P144_VIEW_WIDTH  320
#define P144_VIEW_HEIGHT 200
#define P144_TEX_SIZE     64

typedef struct { float x, y, z; } P144_Vec3;
typedef struct { P144_Vec3 pos; float yaw, pitch; } P144_Camera;
typedef struct { P144_Vec3 pos; float u, v; } P144_Vertex;
typedef struct { P144_Vertex v[4]; } P144_Quad;

typedef enum {
    P144_MAT_STONE = 0,
    P144_MAT_FLOOR = 1,
    P144_MAT_WOOD = 2,
    P144_MAT_COUNT = 3
} P144_Material;

typedef struct {
    uint32_t pixels[P144_VIEW_WIDTH * P144_VIEW_HEIGHT];
    float depth[P144_VIEW_WIDTH * P144_VIEW_HEIGHT];
    uint32_t tex[P144_MAT_COUNT][P144_TEX_SIZE * P144_TEX_SIZE];
    uint32_t seed;
    uint32_t triangles;
} P144_Renderer;

/* Rendering is CPU-only, independent of Win32 and persistent storage. */
void p144_renderer_init(P144_Renderer *r, uint32_t seed);
void p144_renderer_begin(P144_Renderer *r, uint32_t clear_rgb);
void p144_renderer_quad(P144_Renderer *r, const P144_Camera *camera,
                        const P144_Quad *quad, P144_Material material);
void p144_renderer_billboard(P144_Renderer *r, const P144_Camera *camera,
                             P144_Vec3 foot, float width, float height,
                             uint32_t ticks);
void p144_renderer_crosshair(P144_Renderer *r);
uint64_t p144_renderer_image_hash(const P144_Renderer *r);

#endif
