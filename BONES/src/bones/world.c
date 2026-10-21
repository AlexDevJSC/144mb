#include "bones_internal.h"
#include <string.h>
#include <math.h>

bool bones_world_add_collider(BonesWorld *w, const BonesCollider *c) {
    uint32_t i;
    if (!w || !c || !c->id || w->collider_count >= BONES_MAX_COLLIDERS ||
        !(c->bounds.min.x < c->bounds.max.x && c->bounds.min.y < c->bounds.max.y &&
          c->bounds.min.z < c->bounds.max.z) || c->material > BONES_MAT_WOOD ||
        !isfinite(c->bounds.min.x) || !isfinite(c->bounds.min.y) || !isfinite(c->bounds.min.z) ||
        !isfinite(c->bounds.max.x) || !isfinite(c->bounds.max.y) || !isfinite(c->bounds.max.z))
        return false;
    for (i=0;i<w->collider_count;i++) if (w->colliders[i].id == c->id) return false;
    w->colliders[w->collider_count++] = *c;
    return true;
}
bool bones_world_remove_collider(BonesWorld *w, uint64_t id) {
    uint32_t i,j;
    if (!w || !id) return false;
    for (i=0;i<w->collider_count;i++) if (w->colliders[i].id==id) {
        for(j=i+1;j<w->collider_count;j++) w->colliders[j-1]=w->colliders[j];
        w->collider_count--;
        return true;
    }
    return false;
}
static void add_box(BonesWorld *w, uint64_t id, float x0,float y0,float z0,
                    float x1,float y1,float z1, uint8_t material, uint8_t visible) {
    BonesCollider *b;
    BonesCollider candidate = {0};
    b = &candidate;
    b->id = id;
    b->bounds.min = (BonesVec3){x0,y0,z0};
    b->bounds.max = (BonesVec3){x1,y1,z1};
    b->solid = 1;
    b->render_box = visible;
    b->material = material;
    (void)bones_world_add_collider(w, b);
}
/* Test fixture: the same geometry supplies both physics and visuals. */
void bones_world_build_test_room(BonesWorld *w, uint64_t seed) {
    BonesSectorCoord sector = {0,0};
    uint64_t base;
    memset(w, 0, sizeof(*w));
    w->seed = seed;
    w->generator_version = BONES_GENERATOR_VERSION;
    base = bones_sector_key(seed, w->generator_version, sector, UINT64_C(0x424f4e45535f524d));
    add_box(w, base^1u, -8.5f,0,-12.5f, 8.5f,5,-12.0f, BONES_MAT_STONE,1);
    add_box(w, base^2u, -8.5f,0, 12.0f, 8.5f,5, 12.5f, BONES_MAT_STONE,1);
    add_box(w, base^3u, -8.5f,0,-12.0f,-8.0f,5, 12.0f, BONES_MAT_STONE,1);
    add_box(w, base^4u,  8.0f,0,-12.0f, 8.5f,5, 12.0f, BONES_MAT_STONE,1);
    add_box(w, base^5u, -4.25f,0,2.55f,-2.55f,3,4.25f, BONES_MAT_STONE,1);
    add_box(w, base^6u,  2.55f,0,2.55f, 4.25f,3,4.25f, BONES_MAT_STONE,1);
    /* NPC is a visual sprite plus a separate invisible physical volume. */
    add_box(w, base^7u, -.30f,0,6.70f, .30f,1.80f,7.30f, BONES_MAT_STONE,0);
}
