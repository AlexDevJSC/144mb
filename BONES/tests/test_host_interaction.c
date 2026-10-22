/* Actual Skin prompt + Bones authoritative door interaction. Portable host
   logic test; E key in Win32 is covered by input dispatch in application. */
#include "../include/bones/modkit_host.h"
#include "../src/bones/interaction_core.h"
#include "../src/demo/test_house.h"
#include "../modules/skin/skin_modkit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    BonesFoundation f;
    BonesModHost *h;
    BonesModule house;
    BonesInteractionRegistry reg={0};
    BonesInteractionEntry door={0};
    BonesInteractionHit hit={0};
    BonesCamera camera;
    BonesCollider wall={0};
    assert(bones_foundation_init(&f,0x1440001u,0x01442026u,1u));
    assert(bones_house_register_colliders(&f.engine.world));
    house=bones_test_house_module();assert(bones_register_module(&f.engine,&house));
    h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_register_builtins(h)==BONES_MOD_OK);
    assert(bones_mod_host_start(h)==BONES_MOD_OK);
    assert(bones_skin_ui_enter_game()==BONES_MOD_OK);
    door.id=bones_house_door_id(f.engine.world.seed);
    door.kind=1;door.flags=BONES_INTERACTION_REQUIRE_COLLIDER;
    door.bounds.min=(BonesVec3){-.65f,0,1.94f};
    door.bounds.max=(BonesVec3){.65f,1.95f,2.04f};
    assert(bones_interaction_add(&reg,&door));
    memset(&camera,0,sizeof(camera));
    camera.position=(BonesVec3){0,1.65f,0};
    assert(bones_interaction_select(&f.engine.world,&reg,&camera,3.f,&hit));
    assert(hit.id==door.id);
    assert(bones_skin_ui_target(hit.id,1,"PUERTA")==BONES_MOD_OK);
    assert(bones_skin_ui_state()->target_valid);
    assert(bones_skin_ui_input(SKIN2_INTERACT)==SKIN2_INTERACT_REQUEST);
    /* Simulate host revalidation; Skin never removes collider itself. */
    assert(bones_interaction_select(&f.engine.world,&reg,&camera,3.f,&hit));
    assert(hit.id==door.id);
    assert(bones_house_open_door(&f.engine.world));
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_target(0,0,NULL)==BONES_MOD_OK);
    assert(!bones_skin_ui_state()->target_valid);
    assert(!bones_interaction_select(&f.engine.world,&reg,&camera,3.f,&hit));
    assert(bones_mod_host_frame(h)==BONES_MOD_OK); /* scene follows open door */
    /* A wall between eye and any interactable blocks selecting it. */
    wall.id=0x6789u;wall.solid=1;wall.render_box=0;
    wall.bounds.min=(BonesVec3){-.4f,0,.9f};
    wall.bounds.max=(BonesVec3){.4f,2,1.f};
    assert(bones_world_add_collider(&f.engine.world,&wall));
    assert(!bones_interaction_select(&f.engine.world,&reg,&camera,3.f,&hit));
    bones_mod_host_destroy(h);
    puts("PASS Skin E prompt + Bones ray, opening real collider, render scene");
    return 0;
}
