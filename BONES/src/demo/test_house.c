#include "test_house.h"
#include <string.h>

static BonesVertex v(float x, float y, float z, float u, float w) {
    BonesVertex out = {{x,y,z},u,w};
    return out;
}
static bool face(BonesScene *scene, uint8_t material,
                 BonesVertex a, BonesVertex b, BonesVertex c, BonesVertex d) {
    BonesQuad q = {{a,b,c,d},material};
    return bones_scene_add_quad(scene,&q);
}
/* Degenerate last triangle: indices 0-1-2 form a true triangle,
   0-2-3 has zero area because 3 equals 2. No cropped texture asset. */
static bool gable(BonesScene *s, uint8_t material,
                  BonesVertex a, BonesVertex b, BonesVertex c) {
    return face(s,material,a,b,c,c);
}

bool bones_house_append_scene(BonesScene *s) {
    uint32_t initial;
    if (!s || s->quad_count > BONES_MAX_QUADS-BONES_HOUSE_QUADS) return false;
    initial=s->quad_count;
    /* 4 by 4 metre house, front z=2, facing player at z=-7.
       UV expressed as texture repeats, not pixel offsets or recipe indices. */
    /* Counterclockwise as viewed from OUTSIDE. Even though current Eye
       renders both sides, these normals will support future backface rules. */
    if (!face(s,BONES_MAT_STONE,
        v(2,0,2,4,2.5f),v(-2,0,2,0,2.5f),v(-2,2.5f,2,0,0),v(2,2.5f,2,4,0))) goto fail;
    if (!face(s,BONES_MAT_STONE,
        v(-2,0,6,0,2.5f),v(2,0,6,4,2.5f),v(2,2.5f,6,4,0),v(-2,2.5f,6,0,0))) goto fail;
    if (!face(s,BONES_MAT_STONE,
        v(-2,0,2,0,2.5f),v(-2,0,6,4,2.5f),v(-2,2.5f,6,4,0),v(-2,2.5f,2,0,0))) goto fail;
    if (!face(s,BONES_MAT_STONE,
        v(2,0,6,4,2.5f),v(2,0,2,0,2.5f),v(2,2.5f,2,0,0),v(2,2.5f,6,4,0))) goto fail;
    /* Floor interior: same FLOOR ID as the room, no new texture. */
    if (!face(s,BONES_MAT_FLOOR,
        v(-2,.015f,2,0,0),v(-2,.015f,6,0,4),
        v(2,.015f,6,4,4),v(2,.015f,2,4,0))) goto fail;
    /* The visible door is a surface, not a functional opening yet. */
    if (!face(s,BONES_MAT_WOOD,
        v(.65f,.03f,1.97f,1.3f,1.9f),v(-.65f,.03f,1.97f,0,1.9f),
        v(-.65f,1.93f,1.97f,0,0),v(.65f,1.93f,1.97f,1.3f,0))) goto fail;
    if (!gable(s,BONES_MAT_STONE,
        v(2,2.5f,2,4,1.4f),v(-2,2.5f,2,0,1.4f),v(0,3.9f,2,2,0))) goto fail;
    if (!gable(s,BONES_MAT_STONE,
        v(-2,2.5f,6,0,1.4f),v(2,2.5f,6,4,1.4f),v(0,3.9f,6,2,0))) goto fail;
    /* Roof pitched quads are planar with normals facing sky, no new texture. */
    if (!face(s,BONES_MAT_WOOD,
        v(-2.25f,2.45f,1.75f,0,0),v(-2.25f,2.45f,6.25f,0,4.5f),
        v(0,3.99f,6.25f,2.75f,4.5f),v(0,3.99f,1.75f,2.75f,0))) goto fail;
    if (!face(s,BONES_MAT_WOOD,
        v(0,3.99f,1.75f,0,0),v(0,3.99f,6.25f,0,4.5f),
        v(2.25f,2.45f,6.25f,2.75f,4.5f),v(2.25f,2.45f,1.75f,2.75f,0))) goto fail;
    return true;
fail:
    s->quad_count=initial;
    return false;
}
static void scene_hook(void *ctx, const BonesEngine *engine, BonesScene *scene) {
    (void)ctx; (void)engine;
    (void)bones_house_append_scene(scene);
}
BonesModule bones_test_house_module(void) {
    BonesModule m;
    memset(&m,0,sizeof(m));
    m.version=BONES_API_VERSION;
    m.on_scene=scene_hook;
    return m;
}
