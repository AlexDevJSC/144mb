#ifndef BONES_INTERACTION_CORE_H
#define BONES_INTERACTION_CORE_H
/* BONES experimental internal interaction resolver. NOT a ModKit ABI change.
   The host must query and validate again when an action is COMMITTED, not just
   rely on a target remembered by a remote client or drawn in the HUD. */
#include "../../include/bones/bones.h"

#define BONES_INTERACTION_CAPACITY 16u
#define BONES_INTERACTION_REQUIRE_COLLIDER 1u
typedef struct {
    uint64_t id;
    BonesAabb bounds;
    uint32_t kind;
    uint32_t flags;
} BonesInteractionEntry;
typedef struct {
    uint32_t count;
    BonesInteractionEntry entries[BONES_INTERACTION_CAPACITY];
} BonesInteractionRegistry;
typedef struct {
    uint64_t id;
    uint32_t kind;
    float distance;
    BonesVec3 point;
} BonesInteractionHit;
bool bones_interaction_add(BonesInteractionRegistry *registry,const BonesInteractionEntry *entry);
bool bones_interaction_select(const BonesWorld *world,const BonesInteractionRegistry *registry,
                              const BonesCamera *camera,float max_distance,BonesInteractionHit *out);
#endif
