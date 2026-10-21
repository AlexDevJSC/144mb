#include "brain/brain.h"
#include <inttypes.h>
#include <stdio.h>

int main(void) {
    static BrainWorld brain; /* Intentional static storage, not a stack allocation. */
    BrainTown town = {0};
    BrainNpc merchant = {200, 100, 0, BRAIN_ROLE_MERCHANT, 100};
    BrainWallet player = {1, 200, {0,0,0}};
    BrainPurchase request = {{1,1}, 1, 100, BRAIN_FOOD, 3};
    BrainEvent event;
    BrainIntent intent;
    unsigned good;
    if (brain_world_init(&brain, (BrainWorldId){0,42}, 144, 1, 1, 7*60, 60) != BRAIN_OK) return 1;
    town.id = 100;
    town.population = 400;
    town.security = 65;
    for (good = 0; good < BRAIN_COMMODITY_COUNT; ++good) {
        town.goods[good] = (BrainCommodity){100, 20, 10, 100, 10};
    }
    if (brain_world_add_town(&brain, &town) != BRAIN_OK ||
        brain_world_add_npc(&brain, &merchant) != BRAIN_OK) return 2;
    (void)brain_world_advance(&brain, 60*60, 1); /* 08:00 */
    if (brain_world_intent(&brain, merchant.id, NULL, &intent) != BRAIN_OK) return 3;
    printf("Hora mundo: %02" PRIu64 ":%02" PRIu64 "\n",
           (brain.clock.minute % 1440)/60, brain.clock.minute%60);
    printf("NPC #200: actividad=%d (TRADE=2), destino=%d (MARKET=2)\n",
           (int)intent.activity, (int)intent.destination);
    if (brain_world_buy(&brain, &player, &request, &event, 1) != BRAIN_OK) return 4;
    printf("Compra evento #%" PRIu64 "/%" PRIu64 ": coste %" PRIu32 ", oro final %" PRIu32
           ", provisiones %" PRIu32 "\n", event.id.branch, event.id.serial, event.price_paid,
           player.gold, player.holdings[BRAIN_FOOD]);
    printf("Hash logico del mundo: %016" PRIx64 "\n", brain_world_hash(&brain));
    printf("Replay del evento: %s\n",
           brain_world_buy(&brain, &player, &request, &event, 1) == BRAIN_DUPLICATE
           ? "rechazado como duplicado (correcto)" : "ERROR");
    return 0;
}
