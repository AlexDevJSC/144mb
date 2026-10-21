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

static bool append_house(BonesScene *s, bool door_closed) {
    uint32_t initial;
    if (!s || s->quad_count > BONES_MAX_QUADS-BONES_HOUSE_QUADS) return false;
    initial=s->quad_count;
    /* 4 by 4 metre house, front z=2, facing player at z=-7.
       UV expressed as texture repeats, not pixel offsets or recipe indices. */
    /* Counterclockwise as viewed from OUTSIDE. Even though current Eye
       renders both sides, these normals will support future backface rules. */
    /* Front wall has a REAL doorway; door is a separate visual surface.
       Jambs + lintel match the three fixed collision pieces exactly. */
    if (!face(s,BONES_MAT_STONE,
        v(-.65f,0,2,1.35f,1.95f),v(-2,0,2,0,1.95f),
        v(-2,2.5f,2,0,-.55f),v(-.65f,2.5f,2,1.35f,-.55f))) goto fail;
    if (!face(s,BONES_MAT_STONE,
        v(2,0,2,1.35f,1.95f),v(.65f,0,2,0,1.95f),
        v(.65f,2.5f,2,0,-.55f),v(2,2.5f,2,1.35f,-.55f))) goto fail;
    if (!face(s,BONES_MAT_STONE,
        v(.65f,1.95f,2,1.3f,.55f),v(-.65f,1.95f,2,0,.55f),
        v(-.65f,2.5f,2,0,0),v(.65f,2.5f,2,1.3f,0))) goto fail;
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
    /* Door quad rendered only while matching door collider remains present. */
    if (door_closed && !face(s,BONES_MAT_WOOD,
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
bool bones_house_append_scene(BonesScene *s) { return append_house(s,true); }
uint64_t bones_house_door_id(uint64_t seed) {
    BonesSectorCoord sector={0,0};
    return bones_sector_key(seed,BONES_GENERATOR_VERSION,sector,UINT64_C(0x484f555345444f52));
}
static uint64_t house_id(const BonesWorld *world, uint64_t serial) {
    BonesSectorCoord sector={0,0};
    return bones_sector_key(world->seed,world->generator_version,sector,
                             UINT64_C(0x484f555345434f4c)) ^ serial;
}
static bool physical_wall(BonesWorld *w, uint64_t id,
                          float x0,float y0,float z0,float x1,float y1,float z1) {
    BonesCollider c={0};
    c.id=id; c.bounds.min=(BonesVec3){x0,y0,z0};
    c.bounds.max=(BonesVec3){x1,y1,z1};
    c.solid=1; c.render_box=0; c.material=BONES_MAT_STONE;
    return bones_world_add_collider(w,&c);
}
/* Register exactly the visible walls + door, not a solid box that would
   obstruct the entrance. All pieces must fit before mutating the world. */
bool bones_house_register_colliders(BonesWorld *w) {
    BonesWorld backup;
    if (!w || w->collider_count>BONES_MAX_COLLIDERS-BONES_HOUSE_COLLIDERS) return false;
    backup=*w;
    if (!physical_wall(w,house_id(w,1),-2,0,1.95f,-.65f,2.5f,2.05f) ||
        !physical_wall(w,house_id(w,2),.65f,0,1.95f,2,2.5f,2.05f) ||
        !physical_wall(w,house_id(w,3),-.65f,1.95f,1.95f,.65f,2.5f,2.05f) ||
        !physical_wall(w,house_id(w,4),-2,0,5.95f,2,2.5f,6.05f) ||
        !physical_wall(w,house_id(w,5),-2.05f,0,2,-1.95f,2.5f,6) ||
        !physical_wall(w,house_id(w,6),1.95f,0,2,2.05f,2.5f,6) ||
        !physical_wall(w,bones_house_door_id(w->seed),-.65f,0,1.94f,.65f,1.95f,2.04f)) {
        *w=backup; return false;
    }
    return true;
}
bool bones_house_open_door(BonesWorld *w) {
    return w && bones_world_remove_collider(w,bones_house_door_id(w->seed));
}
bool bones_house_append_world_scene(BonesScene *s, const BonesWorld *w) {
    uint32_t i;
    if(!w) return false;
    for(i=0;i<w->collider_count;i++)
        if(w->colliders[i].id==bones_house_door_id(w->seed)) return append_house(s,true);
    return append_house(s,false);
}
static void scene_hook(void *ctx, const BonesEngine *engine, BonesScene *scene) {
    (void)ctx;
    (void)bones_house_append_world_scene(scene,&engine->world);
}
BonesModule bones_test_house_module(void) {
    BonesModule m;
    memset(&m,0,sizeof(m));
    m.version=BONES_API_VERSION;
    m.on_scene=scene_hook;
    return m;
}
