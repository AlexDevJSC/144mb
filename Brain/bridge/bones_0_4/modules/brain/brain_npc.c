#include "brain.h"

BrainResult brain_npc_decide(const BrainNpc *npc, const BrainClock *clock,
                             const BrainPerception *perception, BrainIntent *out) {
    uint64_t minute;
    if (!npc || !clock || !out || !npc->id || npc->role < BRAIN_ROLE_VILLAGER || npc->role > BRAIN_ROLE_GUARD ||
        npc->health_percent > 100 || !clock->ticks_per_minute ||
        (perception && (perception->npc_id != npc->id || perception->threat_percent > 100)))
        return BRAIN_ERR_ARGUMENT;
    minute = clock->minute % BRAIN_MINUTES_PER_DAY;
    out->npc_id = npc->id;
    out->at_minute = clock->minute;
    out->destination = BRAIN_ANCHOR_HOME;
    out->activity = BRAIN_ACT_SLEEP;
    if (perception && perception->sees_threat && perception->threat_percent >= 50) {
        if (npc->role == BRAIN_ROLE_GUARD && npc->health_percent >= 35) {
            out->activity = BRAIN_ACT_DEFEND;
            out->destination = BRAIN_ANCHOR_THREAT;
        } else {
            out->activity = BRAIN_ACT_FLEE;
            out->destination = BRAIN_ANCHOR_HOME;
        }
        return BRAIN_OK;
    }
    if (minute < 360 || minute >= 1320) return BRAIN_OK;
    if (npc->role == BRAIN_ROLE_MERCHANT) {
        if (minute >= 480 && minute < 1080) {
            out->activity = BRAIN_ACT_TRADE;
            out->destination = BRAIN_ANCHOR_MARKET;
        } else out->activity = BRAIN_ACT_REST;
    } else if (npc->role == BRAIN_ROLE_GUARD) {
        if (minute >= 420 && minute < 1140) {
            out->activity = BRAIN_ACT_PATROL;
            out->destination = BRAIN_ANCHOR_PATROL;
        } else out->activity = BRAIN_ACT_REST;
    } else {
        if (minute >= 480 && minute < 1020) {
            out->activity = BRAIN_ACT_WORK;
            out->destination = BRAIN_ANCHOR_WORK;
        } else out->activity = BRAIN_ACT_REST;
    }
    return BRAIN_OK;
}
