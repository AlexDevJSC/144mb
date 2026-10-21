#include "brain/brain.h"
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static BrainWorld a, b;

static BrainTown town_make(BrainId id) {
    BrainTown t = {0};
    unsigned j;
    t.id = id;
    t.sector_x = -345;
    t.sector_y = 1024;
    t.population = 500;
    t.security = 60;
    for (j = 0; j < BRAIN_COMMODITY_COUNT; ++j) {
        t.goods[j].stock = 100;
        t.goods[j].production_per_day = (uint32_t)(20 + j);
        t.goods[j].consumption_per_day = 10;
        t.goods[j].desired_stock = 100;
        t.goods[j].base_price = 10;
    }
    return t;
}
static void common_init(BrainWorld *world) {
    BrainTown t = town_make(500);
    BrainNpc villager = {17, 500, 0, BRAIN_ROLE_VILLAGER, 100};
    BrainNpc merchant = {9, 500, 0, BRAIN_ROLE_MERCHANT, 80};
    BrainNpc guard = {2, 500, 0, BRAIN_ROLE_GUARD, 100};
    BrainFaction f1 = {11, 60}, f2 = {6, -10};
    assert(brain_world_init(world, (BrainWorldId){0, 1001}, 0x144, 1, 1, 7 * 60, 60) == BRAIN_OK);
    assert(brain_world_add_town(world, &t) == BRAIN_OK);
    assert(brain_world_add_faction(world, &f1) == BRAIN_OK);
    assert(brain_world_add_faction(world, &f2) == BRAIN_OK);
    assert(brain_world_add_npc(world, &villager) == BRAIN_OK);
    assert(brain_world_add_npc(world, &merchant) == BRAIN_OK);
    assert(brain_world_add_npc(world, &guard) == BRAIN_OK);
}

static void test_clock_and_random(void) {
    BrainClock clock;
    uint64_t days;
    assert(brain_clock_init(&clock, 1439, 60) == BRAIN_OK);
    assert(brain_clock_advance(&clock, 59, &days) == BRAIN_OK);
    assert(days == 0 && clock.minute == 1439 && clock.tick_remainder == 59);
    assert(brain_clock_advance(&clock, 1, &days) == BRAIN_OK);
    assert(days == 1 && clock.minute == 1440 && clock.tick_remainder == 0);
    assert(brain_clock_advance(&clock, UINT64_MAX, &days) == BRAIN_OK);
    assert(clock.tick_remainder == UINT64_MAX % 60);
    clock.minute = UINT64_MAX - 1u;
    assert(brain_clock_advance(&clock, 120, &days) == BRAIN_ERR_OVERFLOW);
    assert(brain_clock_init(&clock, 0, 0) == BRAIN_ERR_ARGUMENT);
    assert(brain_random_key(7, 2, 10) == brain_random_key(7, 2, 10));
    assert(brain_random_key(7, 2, 10) != brain_random_key(7, 3, 10));
    assert(brain_random_key(7, 2, 10) != brain_random_key(7, 2, 11));
}

static void test_routines(void) {
    BrainIntent intent;
    BrainPerception perception = {9, 80, 1};
    BrainId ids[3] = {17, 9, 2};
    BrainPerception perceptions[3] = {{17,0,0},{9,80,1},{2,80,1}};
    BrainIntent outputs[3];
    size_t count = 0;
    common_init(&a);
    assert(brain_world_intent(&a, 9, NULL, &intent) == BRAIN_OK);
    assert(intent.activity == BRAIN_ACT_REST);
    assert(brain_world_advance(&a, 60 * 60, 0) == BRAIN_ERR_NOT_HOST);
    assert(brain_world_advance(&a, 60 * 60, 1) == BRAIN_OK);
    assert(brain_world_intent(&a, 9, NULL, &intent) == BRAIN_OK);
    assert(intent.activity == BRAIN_ACT_TRADE && intent.destination == BRAIN_ANCHOR_MARKET);
    assert(brain_world_intent(&a, 9, &perception, &intent) == BRAIN_OK);
    assert(intent.activity == BRAIN_ACT_FLEE);
    assert(brain_bones_collect_intents(&a, ids, perceptions, 3, outputs, 3, &count) == BRAIN_OK);
    assert(count == 3);
    assert(outputs[0].activity == BRAIN_ACT_WORK);
    assert(outputs[1].activity == BRAIN_ACT_FLEE);
    assert(outputs[2].activity == BRAIN_ACT_DEFEND);
    perceptions[2].npc_id = 17;
    assert(brain_bones_collect_intents(&a, ids, perceptions, 3, outputs, 3, &count) == BRAIN_ERR_ARGUMENT);
    assert(count == 2);
    assert(brain_bones_collect_intents(&a, ids, NULL, 3, outputs, 2, &count) == BRAIN_ERR_ARGUMENT);
}

static void test_aggregate(void) {
    BrainCommodity c = {100, 25, 40, 100, 10};
    BrainCommodity d = c;
    BrainCommodity surplus = {100, 100, 0, 100, 10};
    uint64_t hash0;
    unsigned i;
    for (i = 0; i < 300; ++i) assert(brain_market_advance(&c, 1) == BRAIN_OK);
    assert(brain_market_advance(&d, 300) == BRAIN_OK);
    assert(c.stock == d.stock && c.stock == 0);
    assert(brain_market_price(&c) == 15);
    assert(brain_market_advance(&surplus, UINT64_MAX) == BRAIN_OK);
    assert(surplus.stock == BRAIN_MAX_STOCK);
    assert(brain_market_price(&surplus) == 5);
    common_init(&a);
    common_init(&b);
    assert(brain_world_hash(&a) == brain_world_hash(&b));
    hash0 = brain_world_hash(&a);
    assert(brain_world_advance(&a, 60u * 1440u * 730u, 1) == BRAIN_OK);
    assert(brain_world_advance(&b, 60u * 1440u * 365u, 1) == BRAIN_OK);
    assert(brain_world_advance(&b, 60u * 1440u * 365u, 1) == BRAIN_OK);
    assert(brain_world_hash(&a) == brain_world_hash(&b));
    assert(brain_world_hash(&a) != hash0);
}

static void test_purchases(void) {
    BrainWallet wallet = {700, 1000, {0,0,0}};
    BrainPurchase req = {{1,101}, 700, 500, BRAIN_FOOD, 3};
    BrainEvent event = {0}, replay = {0};
    uint64_t hash;
    common_init(&a);
    assert(brain_market_price(&a.towns[0].goods[0]) == 10);
    assert(brain_world_buy(&a, &wallet, &req, &event, 0) == BRAIN_ERR_NOT_HOST);
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_OK);
    assert(wallet.gold == 970 && wallet.holdings[0] == 3);
    assert(a.towns[0].goods[0].stock == 97 && event.price_paid == 30);
    hash = brain_world_hash(&a);
    assert(brain_world_buy(&a, &wallet, &req, &replay, 1) == BRAIN_DUPLICATE);
    assert(brain_world_hash(&a) == hash);
    assert(wallet.gold == 970 && wallet.holdings[0] == 3 && replay.id.branch == event.id.branch && replay.id.serial == event.id.serial);
    req.quantity = 4;
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_ERR_CONFLICT);
    req.event_id.serial = 102; req.quantity = 1000;
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_ERR_STOCK);
    req.quantity = 80;
    wallet.gold = 15;
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_ERR_FUNDS);
    assert(wallet.gold == 15 && wallet.holdings[0] == 3);
    wallet.gold = 970;
    assert(brain_world_hash(&a) == hash);
    req.event_id.serial = 103; req.quantity = 2;
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_OK);
    assert(wallet.holdings[0] == 5 && a.towns[0].goods[0].stock == 95);
    assert(event.price_paid >= 20);
    /* Same serial on another PBW branch is a DIFFERENT semantic event. */
    req.event_id.branch = 2;
    req.event_id.serial = 101;
    req.quantity = 1;
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_OK);
    assert(event.id.branch == 2 && wallet.holdings[0] == 6);
    req.event_id.serial = 0;
    assert(brain_world_buy(&a, &wallet, &req, &event, 1) == BRAIN_ERR_ARGUMENT);
}

static void test_faction_changes(void) {
    BrainStandingChange ev = {{1,501}, 700, 11, 6, -30};
    BrainEvent out;
    uint64_t hash;
    common_init(&a);
    assert(brain_world_change_standing(&a, &ev, &out, 1) == BRAIN_OK);
    assert(a.relation_count == 1 && a.relations[0].standing == -30);
    hash = brain_world_hash(&a);
    assert(brain_world_change_standing(&a, &ev, &out, 1) == BRAIN_DUPLICATE);
    assert(brain_world_hash(&a) == hash);
    ev.event_id.serial = 502; ev.delta = -100;
    assert(brain_world_change_standing(&a, &ev, &out, 1) == BRAIN_OK);
    assert(a.relations[0].standing == -100);
    ev.event_id.serial = 503; ev.delta = 100;
    assert(brain_world_change_standing(&a, &ev, &out, 1) == BRAIN_OK);
    assert(a.relations[0].standing == 0);
}

static void test_registration_order_and_caps(void) {
    BrainWorld *w = &a;
    BrainWorld *v = &b;
    BrainTown t1 = town_make(2), t2 = town_make(1);
    t2.goods[BRAIN_FOOD].stock = 1000;
    BrainNpc n1 = {9, 2, 0, BRAIN_ROLE_MERCHANT, 80};
    BrainNpc n2 = {1, 1, 0, BRAIN_ROLE_VILLAGER, 90};
    BrainFaction f1 = {15,10}, f2 = {7,20};
    BrainWallet wallet = {700, BRAIN_MAX_GOLD, {0,0,0}};
    BrainPurchase purchase = {{1,1}, 700, 1, BRAIN_FOOD, 1};
    BrainEvent event;
    unsigned i;
    assert(brain_world_init(w, (BrainWorldId){0, 1001}, 4, 1, 1, 0, 1) == BRAIN_OK);
    assert(brain_world_init(v, (BrainWorldId){0, 1001}, 4, 1, 1, 0, 1) == BRAIN_OK);
    assert(brain_world_add_town(w, &t1) == BRAIN_OK);
    assert(brain_world_add_town(w, &t2) == BRAIN_OK);
    assert(brain_world_add_town(v, &t2) == BRAIN_OK);
    assert(brain_world_add_town(v, &t1) == BRAIN_OK);
    assert(brain_world_add_faction(w, &f1) == BRAIN_OK);
    assert(brain_world_add_faction(w, &f2) == BRAIN_OK);
    assert(brain_world_add_faction(v, &f2) == BRAIN_OK);
    assert(brain_world_add_faction(v, &f1) == BRAIN_OK);
    assert(brain_world_add_npc(w, &n1) == BRAIN_OK);
    assert(brain_world_add_npc(w, &n2) == BRAIN_OK);
    assert(brain_world_add_npc(v, &n2) == BRAIN_OK);
    assert(brain_world_add_npc(v, &n1) == BRAIN_OK);
    assert(brain_world_hash(w) == brain_world_hash(v));
    assert(brain_world_add_town(w, &t1) == BRAIN_ERR_CONFLICT);
    for (i = 0; i < BRAIN_MAX_EVENTS; ++i) {
        purchase.event_id.serial = i + 1u;
        /* Ensure the world can fill its bounded ledger without any UB. */
        assert(brain_world_buy(w, &wallet, &purchase, &event, 1) == BRAIN_OK);
    }
    purchase.event_id.serial = BRAIN_MAX_EVENTS + 1u;
    assert(brain_world_buy(w, &wallet, &purchase, &event, 1) == BRAIN_ERR_CAPACITY);
}

int main(void) {
    test_clock_and_random();
    test_routines();
    test_aggregate();
    test_purchases();
    test_faction_changes();
    test_registration_order_and_caps();
    puts("PASS Brain v0.1: reloj, claves, NPC, intents Bones, economia, comercio atomico,");
    puts("idempotencia, facciones, simulacion agregada, hash canonico, limites");
    printf("tamano BrainWorld=%zu bytes | hash tras pruebas=%016" PRIx64 "\n",
           sizeof(BrainWorld), brain_world_hash(&a));
    return 0;
}
