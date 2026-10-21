#ifndef BRAIN_NPC_INTERACTION_H
#define BRAIN_NPC_INTERACTION_H
/* A pure semantic intent. Bones selects + checks an NPC target;
   Skin renders text/UI. THIS DOES NOT REGISTER THE E KEY. */
#include "brain.h"
#define BRAIN_INTERACTION_INTENT_VERSION 1u
#define BRAIN_NPC_REPLY_GREETING 1u
typedef struct {
    BrainId npc_id;
    uint32_t action; /* Semantic reply ID, not UI text. */
} BrainNpcInteractionIntent;
/* Caller must supply NPC already chosen/validated by Bones. */
BrainResult brain_npc_interaction_offer(const BrainNpc *npc,
                                        BrainNpcInteractionIntent *result);
#endif
