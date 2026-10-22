#include "brain/brain_interaction.h"
BrainResult brain_npc_interaction_offer(const BrainNpc *npc,
                                        BrainNpcInteractionIntent *out) {
    if (!npc || !out || !npc->id ||
        npc->role < BRAIN_ROLE_VILLAGER || npc->role > BRAIN_ROLE_GUARD ||
        npc->health_percent > 100) return BRAIN_ERR_ARGUMENT;
    out->npc_id=npc->id;
    out->action=BRAIN_NPC_REPLY_GREETING;
    return BRAIN_OK;
}
