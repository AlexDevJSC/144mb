#ifndef EAR_BONES_BRIDGE_H
#define EAR_BONES_BRIDGE_H

#include "ear.h"

/* Ear's proposed adapter: NO claim these are existing Bones public symbols. */
#define EAR_BONES_BRIDGE_VERSION 1u

typedef enum {
    EAR_BONES_FOOTSTEP = 0,
    EAR_BONES_ATTACK_SWING,
    EAR_BONES_DAMAGE,
    EAR_BONES_ITEM_ACQUIRED,
    EAR_BONES_SPELL_CAST,
    EAR_BONES_UI_CONFIRM,
    EAR_BONES_DOOR_OPEN
} EarBonesEventKind;

typedef struct {
    EarEngine *audio;
    uint32_t world_seed;
} EarBonesBridge;

void ear_bones_bridge_init(EarBonesBridge *bridge, EarEngine *audio,
                           uint32_t world_seed);
/* Caller controls event IDs. In future Bones core this is a committed event. */
void ear_bones_emit(EarBonesBridge *bridge, EarBonesEventKind kind,
                    uint64_t event_id);
/* Music seeded per area; caller keeps storage for the new score alive. */
int ear_bones_scene(EarBonesBridge *bridge, EarScore *score_storage,
                    uint32_t area_seed, EarStyle style);

#endif
