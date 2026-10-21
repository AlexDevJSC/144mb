#ifndef P144_BRAIN_H
#define P144_BRAIN_H
/* Brain 0.1 -- C11, platform-independent simulation library.
   This is a versioned source ABI for Bones to integrate, not a final save ABI. */
#include <stddef.h>
#include <stdint.h>

#define BRAIN_API_VERSION 1u
#define BRAIN_MAX_TOWNS 16u
#define BRAIN_MAX_NPCS 128u
#define BRAIN_MAX_FACTIONS 16u
#define BRAIN_MAX_RELATIONS 48u
#define BRAIN_MAX_EVENTS 256u
#define BRAIN_COMMODITY_COUNT 3u
#define BRAIN_MAX_STOCK 1000000u
#define BRAIN_MAX_GOLD 1000000000u
#define BRAIN_MINUTES_PER_DAY 1440u

typedef uint64_t BrainId; /* Caller assigns stable, nonzero logical IDs. */
typedef struct { uint64_t hi, lo; } BrainWorldId; /* Not the seed. */
typedef struct { uint64_t branch, serial; } BrainEventId; /* PBW lineage. */

typedef enum {
    BRAIN_OK = 0,
    BRAIN_DUPLICATE = 1, /* Identical event already applied, no changes. */
    BRAIN_ERR_ARGUMENT = -1,
    BRAIN_ERR_NOT_FOUND = -2,
    BRAIN_ERR_CAPACITY = -3,
    BRAIN_ERR_FUNDS = -4,
    BRAIN_ERR_STOCK = -5,
    BRAIN_ERR_CONFLICT = -6, /* Event ID used for different request. */
    BRAIN_ERR_OVERFLOW = -7,
    BRAIN_ERR_NOT_HOST = -8
} BrainResult;

typedef enum {
    BRAIN_FOOD = 0,
    BRAIN_MATERIAL = 1,
    BRAIN_GOODS = 2
} BrainCommodityType;

typedef enum {
    BRAIN_ROLE_VILLAGER = 0,
    BRAIN_ROLE_MERCHANT = 1,
    BRAIN_ROLE_GUARD = 2
} BrainRole;

typedef enum {
    BRAIN_ACT_SLEEP = 0,
    BRAIN_ACT_WORK = 1,
    BRAIN_ACT_TRADE = 2,
    BRAIN_ACT_PATROL = 3,
    BRAIN_ACT_REST = 4,
    BRAIN_ACT_FLEE = 5,
    BRAIN_ACT_DEFEND = 6
} BrainActivity;

typedef enum {
    BRAIN_ANCHOR_HOME = 0,
    BRAIN_ANCHOR_WORK = 1,
    BRAIN_ANCHOR_MARKET = 2,
    BRAIN_ANCHOR_PATROL = 3,
    BRAIN_ANCHOR_THREAT = 4
} BrainAnchor;

typedef enum {
    BRAIN_EVENT_PURCHASE = 1,
    BRAIN_EVENT_FACTION_CHANGE = 2
} BrainEventType;

typedef struct {
    uint64_t minute;             /* Absolute game time; day = minute / 1440. */
    uint32_t tick_remainder;     /* Fractional minute in fixed ticks. */
    uint32_t ticks_per_minute;   /* Configured by Bones; NOT canonical yet. */
} BrainClock;

typedef struct {
    uint32_t stock;              /* [0 .. BRAIN_MAX_STOCK]. */
    uint32_t production_per_day;
    uint32_t consumption_per_day;
    uint32_t desired_stock;      /* Must be >0. */
    uint32_t base_price;         /* Gold per item, always >0. */
} BrainCommodity;

typedef struct {
    BrainId id;
    int64_t sector_x, sector_y;  /* 64-bit sector coordinates in master. */
    uint32_t population;
    uint16_t security;           /* [0..100] prototype scale. */
    BrainCommodity goods[BRAIN_COMMODITY_COUNT];
} BrainTown;

typedef struct {
    BrainId id;
    BrainId town_id;
    BrainId faction_id;          /* 0 = no faction. */
    BrainRole role;
    uint16_t health_percent;     /* [0..100]. */
} BrainNpc;

typedef struct {
    BrainId npc_id;
    uint16_t threat_percent;     /* 0..100, computed externally by Bones. */
    uint8_t sees_threat;         /* 0/1; sight test belongs to Bones. */
} BrainPerception;

typedef struct {
    BrainId npc_id;
    BrainActivity activity;
    BrainAnchor destination;     /* Symbolic: Bones resolves physical position. */
    uint64_t at_minute;
} BrainIntent;

typedef struct { BrainId id; int16_t influence; } BrainFaction;
typedef struct { BrainId a, b; int16_t standing; } BrainRelation; /* sorted a < b */

typedef struct {
    BrainId actor_id;
    uint32_t gold;
    uint32_t holdings[BRAIN_COMMODITY_COUNT]; /* Temporary prototype wallet. */
} BrainWallet;

typedef struct {
    BrainEventId event_id;       /* Stable branch/serial supplied by host. */
    BrainId actor_id;
    BrainId town_id;
    BrainCommodityType commodity;
    uint32_t quantity;
} BrainPurchase;

typedef struct {
    BrainEventId event_id;
    BrainId actor_id;
    BrainId faction_a;
    BrainId faction_b;
    int16_t delta;
} BrainStandingChange;

typedef struct {
    BrainEventId id;
    BrainEventType type;
    BrainId actor_id;
    BrainId target_a, target_b;
    int32_t arg_a, arg_b; /* Purchase: good/qty; relation: delta/unused. */
    uint32_t price_paid; /* Purchase only. */
} BrainEvent;

typedef struct {
    BrainWorldId world_id;
    uint64_t seed;
    uint32_t generator_version;
    uint32_t rules_version;
    BrainClock clock;
    BrainTown towns[BRAIN_MAX_TOWNS];
    BrainNpc npcs[BRAIN_MAX_NPCS];
    BrainFaction factions[BRAIN_MAX_FACTIONS];
    BrainRelation relations[BRAIN_MAX_RELATIONS];
    BrainEvent events[BRAIN_MAX_EVENTS]; /* Prototype dedup ledger, bounded. */
    uint16_t town_count, npc_count, faction_count, relation_count, event_count;
} BrainWorld;

/* Clock and stateless random. */
BrainResult brain_clock_init(BrainClock *clock, uint64_t minute, uint32_t ticks_per_minute);
BrainResult brain_clock_advance(BrainClock *clock, uint64_t ticks, uint64_t *days_elapsed);
uint64_t brain_random_key(uint64_t seed, uint64_t domain, uint64_t object_id);

/* Market rules: finite, integer-only, O(1) aggregate distant updates. */
BrainResult brain_market_validate(const BrainCommodity *commodity);
BrainResult brain_market_advance(BrainCommodity *commodity, uint64_t days);
uint32_t brain_market_price(const BrainCommodity *commodity);

/* NPC priority + routine; no pathfinding, no render or physics calls. */
BrainResult brain_npc_decide(const BrainNpc *npc, const BrainClock *clock,
                             const BrainPerception *perception, BrainIntent *out);

/* World integration. Register by stable ID; order independent for towns/NPCs. */
BrainResult brain_world_init(BrainWorld *world, BrainWorldId world_id,
                              uint64_t seed, uint32_t generator_version,
                              uint32_t rules_version, uint64_t start_minute,
                              uint32_t ticks_per_minute);
BrainResult brain_world_add_town(BrainWorld *world, const BrainTown *town);
BrainResult brain_world_add_npc(BrainWorld *world, const BrainNpc *npc);
BrainResult brain_world_add_faction(BrainWorld *world, const BrainFaction *faction);
BrainResult brain_world_advance(BrainWorld *world, uint64_t ticks, int is_host);
BrainResult brain_world_intent(const BrainWorld *world, BrainId npc_id,
                               const BrainPerception *perception, BrainIntent *out);
BrainResult brain_world_buy(BrainWorld *world, BrainWallet *wallet,
                             const BrainPurchase *request, BrainEvent *out, int is_host);
BrainResult brain_world_change_standing(BrainWorld *world,
                                         const BrainStandingChange *request,
                                         BrainEvent *out, int is_host);
uint64_t brain_world_hash(const BrainWorld *world);

/* Bones adapter. Returns symbolic decisions for nearby NPCs, never geometry. */
BrainResult brain_bones_collect_intents(const BrainWorld *world,
                                         const BrainId *nearby_npc_ids,
                                         const BrainPerception *perceptions,
                                         size_t count, BrainIntent *outputs,
                                         size_t output_capacity,
                                         size_t *output_count);

#endif
