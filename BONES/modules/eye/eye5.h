#ifndef EYE5_H
#define EYE5_H

#include "eye4.h"
#include <stddef.h>
#include <stdint.h>

/* Standalone Eye 0.5 roof assets. Not a Bones public interface. */
#define EYE5_VERSION 5u
#define EYE5_TILE_PIXELS 4096u
#define EYE5_RECORD_BYTES 12u
#define EYE5_HEADER_BYTES 12u
#define EYE5_MAX_RECIPES 64u
#define EYE5_MAX_PACKET (EYE5_HEADER_BYTES + EYE5_RECORD_BYTES * EYE5_MAX_RECIPES)
#define EYE5_CACHE_MAX 16u
#define EYE5_MAX_SURFACES 16u

typedef enum { EYE5_SLATE=0, EYE5_CLAY=1, EYE5_WOOD=2, EYE5_THATCH=3 } Eye5RoofMaterial;
typedef enum { EYE5_GABLE=0, EYE5_HIP=1 } Eye5RoofForm;
typedef enum { EYE5_SURFACE_ROOF=0, EYE5_SURFACE_TRIM=1, EYE5_SURFACE_RIDGE=2 } Eye5SurfaceRole;

/* Canonical EYE5 record: [family, palette, width, course, stagger, wear,
 * edge, flags=0, seed:4 little-endian]. Width/course: 8 or 16 only, to
 * ensure exact seamless tiling over a 64x64 atlas on both axes. */
typedef struct {
    uint8_t family, palette, width, course, stagger, wear, edge, flags;
    uint32_t seed;
} Eye5Recipe;

typedef struct { Eye5Recipe recipe[EYE5_MAX_RECIPES]; unsigned count; } Eye5Bank;
typedef struct {
    const Eye5Bank *bank;
    uint32_t *arena; /* caller-owned: capacity >= 4096*slots uint32_t */
    uint8_t keys[EYE5_CACHE_MAX];
    uint32_t age[EYE5_CACHE_MAX];
    unsigned serial, count, slots, hits, misses;
} Eye5Cache;

typedef struct { float x,y,z,u,v; } Eye5Vertex;
typedef struct {
    Eye5Vertex vertex[4];
    uint8_t vertex_count; /* 3 or 4. A Bones 0.2 quad can duplicate v[2]. */
    uint8_t role; /* roof pixels or shared wooden trim */
} Eye5Surface;
typedef struct { Eye5Surface surface[EYE5_MAX_SURFACES]; unsigned count; } Eye5RoofMesh;
typedef struct {
    float half_width, half_depth, wall_height, roof_rise, overhang;
    uint8_t form;
} Eye5RoofSpec;

int eye5_valid(const Eye5Recipe *r);
int eye5_generate(const Eye5Recipe *recipe, uint32_t *out, size_t pixel_count);
size_t eye5_write(const Eye5Recipe *recipes, unsigned count, uint8_t *out, size_t capacity);
int eye5_read(const uint8_t *data, size_t size, Eye5Bank *out);
int eye5_cache_init(Eye5Cache *c, const Eye5Bank *bank, uint32_t *arena,
                    size_t pixel_capacity, unsigned slots);
const uint32_t *eye5_cache_get(Eye5Cache *c, unsigned material_id);
void eye5_demo_recipes(Eye5Recipe out[4]);

/* Eye owns visual assembly only. No colliders, world mutations, or scene
 * publication into Bones. Geometry is returned in simple independent data. */
int eye5_build_roof(const Eye5RoofSpec *spec, Eye5RoofMesh *out);
#endif
