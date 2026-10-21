#ifndef BONES_INTERNAL_H
#define BONES_INTERNAL_H
#include "../../include/bones/bones.h"
void bones_world_build_test_room(BonesWorld *world, uint64_t seed);
void bones_dispatch_events(BonesEngine *engine);
void bones_physics_update(BonesEngine *engine, const BonesInput *input, float dt);


/* Same actor controller for local player, remote players and NPCs. */
void bones_physics_step_actor(const BonesWorld *world, BonesPlayer *body,
                              const BonesInput *input, float dt);

#endif
