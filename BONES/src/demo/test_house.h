#ifndef BONES_TEST_HOUSE_H
#define BONES_TEST_HOUSE_H
#include "../../include/bones/bones.h"
/* Demo-only house layout: geometry and physics share an explicitly described
   portal. This is NOT part of the public Bones API or Eye's production assets. */
#define BONES_HOUSE_QUADS 12u
#define BONES_HOUSE_COLLIDERS 7u
uint64_t bones_house_door_id(uint64_t world_seed);
bool bones_house_register_colliders(BonesWorld *world);
bool bones_house_open_door(BonesWorld *world); /* Demo toggle: removes door blocker */
bool bones_house_append_scene(BonesScene *scene);
bool bones_house_append_world_scene(BonesScene *scene, const BonesWorld *world);
/* Legacy visual test-hook used until a canonical scene-source API is agreed. */
BonesModule bones_test_house_module(void);
#endif
