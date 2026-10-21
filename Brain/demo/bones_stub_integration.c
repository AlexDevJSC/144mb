/* This example is a FAKE Bones consumer, not the real Bones engine API.
   It demonstrates Brain->Bones intent transfer without linking to Win32. */
#include "brain/brain.h"
#include <stdio.h>

/* Bones-provided in real integration: navigation, sprite state, collisions. */
static void bones_consume_intent(const BrainIntent *action) {
    printf("[MOCK BONES] NPC %llu -> activity %d, anchor %d (minute %llu)\n",
           (unsigned long long)action->npc_id, (int)action->activity,
           (int)action->destination, (unsigned long long)action->at_minute);
}

int main(void) {
    static BrainWorld brain;
    BrainWorldId id = {0, 123};
    BrainTown town = {0};
    BrainNpc trader = {99, 50, 0, BRAIN_ROLE_MERCHANT, 100};
    BrainId visible_ids[] = {99};
    BrainPerception sensor_inputs[] = {{99, 0, 0}};
    BrainIntent intents[1];
    size_t written, i;
    unsigned c;
    town.id = 50;
    for (c = 0; c < BRAIN_COMMODITY_COUNT; ++c)
        town.goods[c] = (BrainCommodity){50, 5, 3, 50, 10};
    if (brain_world_init(&brain, id, 44, 1, 1, 9 * 60, 60) != BRAIN_OK ||
        brain_world_add_town(&brain, &town) != BRAIN_OK ||
        brain_world_add_npc(&brain, &trader) != BRAIN_OK) return 1;
    if (brain_bones_collect_intents(&brain, visible_ids, sensor_inputs,
                                    1, intents, 1, &written) != BRAIN_OK) return 2;
    for (i = 0; i < written; ++i) bones_consume_intent(&intents[i]);
    /* Actual Bones would resolve BRAIN_ANCHOR_MARKET to a navigable location. */
    return 0;
}
