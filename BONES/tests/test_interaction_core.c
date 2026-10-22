#include "../src/bones/interaction_core.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static BonesWorld world;
static BonesInteractionRegistry registry;
static BonesCamera camera;
static BonesInteractionHit hit;
static BonesCollider occluder;
int main(void) {
    BonesEngine engine;
    BonesInteractionEntry door;
    bones_init(&engine,3443);
    world=engine.world;
    assert(bones_house_register_colliders(&world));
    memset(&registry,0,sizeof(registry));
    memset(&door,0,sizeof(door));
    door.id=bones_house_door_id(world.seed);door.kind=10;
    door.flags=BONES_INTERACTION_REQUIRE_COLLIDER;
    door.bounds.min=(BonesVec3){-.65f,0,1.94f};
    door.bounds.max=(BonesVec3){.65f,1.95f,2.04f};
    assert(bones_interaction_add(&registry,&door));
    assert(!bones_interaction_add(&registry,&door));
    memset(&camera,0,sizeof(camera));
    camera.position=(BonesVec3){0,1.65f,0};
    assert(bones_interaction_select(&world,&registry,&camera,3.0f,&hit));
    assert(hit.id==door.id && hit.distance>1.9f && hit.distance<2.0f);
    assert(!bones_interaction_select(&world,&registry,&camera,1.0f,&hit));
    camera.yaw=1.57079632679f;
    assert(!bones_interaction_select(&world,&registry,&camera,3.0f,&hit));
    camera.yaw=0;
    camera.pitch=.6f;assert(!bones_interaction_select(&world,&registry,&camera,3.f,&hit));
    camera.pitch=0;
    memset(&occluder,0,sizeof(occluder));
    occluder.id=44;occluder.solid=1;
    occluder.bounds.min=(BonesVec3){-.5f,0,.5f};
    occluder.bounds.max=(BonesVec3){.5f,2,.55f};
    assert(bones_world_add_collider(&world,&occluder));
    assert(!bones_interaction_select(&world,&registry,&camera,3.f,&hit));
    assert(bones_world_remove_collider(&world,44));
    assert(bones_interaction_select(&world,&registry,&camera,3.f,&hit));
    assert(bones_house_open_door(&world));
    assert(!bones_interaction_select(&world,&registry,&camera,3.f,&hit));
    puts("PASS: target selection, range, yaw/pitch, occlusion, stable door ID, door open");
    return 0;
}
