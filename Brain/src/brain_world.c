#include "brain/brain.h"
#include <limits.h>
#include <string.h>

static BrainTown *find_town(BrainWorld *w, BrainId id) {
    uint16_t i;
    for (i = 0; i < w->town_count; ++i) if (w->towns[i].id == id) return &w->towns[i];
    return NULL;
}
static const BrainNpc *find_npc(const BrainWorld *w, BrainId id) {
    uint16_t i;
    for (i = 0; i < w->npc_count; ++i) if (w->npcs[i].id == id) return &w->npcs[i];
    return NULL;
}
static int has_faction(const BrainWorld *w, BrainId id) {
    uint16_t i;
    for (i = 0; i < w->faction_count; ++i) if (w->factions[i].id == id) return 1;
    return 0;
}
static int event_id_equal(BrainEventId a, BrainEventId b) {
    return a.branch == b.branch && a.serial == b.serial;
}
static const BrainEvent *find_event(const BrainWorld *w, BrainEventId id) {
    uint16_t i;
    for (i = 0; i < w->event_count; ++i) if (event_id_equal(w->events[i].id, id)) return &w->events[i];
    return NULL;
}

BrainResult brain_world_init(BrainWorld *w, BrainWorldId id, uint64_t seed,
                              uint32_t generator_version, uint32_t rules_version,
                              uint64_t start, uint32_t tpm) {
    if (!w || !(id.hi || id.lo) || !generator_version || !rules_version || !tpm)
        return BRAIN_ERR_ARGUMENT;
    memset(w, 0, sizeof(*w));
    w->world_id = id;
    w->seed = seed;
    w->generator_version = generator_version;
    w->rules_version = rules_version;
    return brain_clock_init(&w->clock, start, tpm);
}

BrainResult brain_world_add_town(BrainWorld *w, const BrainTown *town) {
    uint16_t i, idx;
    BrainTown copy;
    if (!w || !town || !town->id || town->security > 100) return BRAIN_ERR_ARGUMENT;
    if (w->town_count >= BRAIN_MAX_TOWNS) return BRAIN_ERR_CAPACITY;
    for (i = 0; i < BRAIN_COMMODITY_COUNT; ++i)
        if (brain_market_validate(&town->goods[i]) != BRAIN_OK) return BRAIN_ERR_ARGUMENT;
    idx = 0;
    while (idx < w->town_count && w->towns[idx].id < town->id) ++idx;
    if (idx < w->town_count && w->towns[idx].id == town->id) return BRAIN_ERR_CONFLICT;
    copy = *town;
    memmove(&w->towns[idx + 1], &w->towns[idx],
            (size_t)(w->town_count - idx) * sizeof(w->towns[0]));
    w->towns[idx] = copy;
    w->town_count++;
    return BRAIN_OK;
}

BrainResult brain_world_add_faction(BrainWorld *w, const BrainFaction *faction) {
    uint16_t idx;
    BrainFaction copy;
    if (!w || !faction || !faction->id || faction->influence < -100 ||
        faction->influence > 100) return BRAIN_ERR_ARGUMENT;
    if (w->faction_count >= BRAIN_MAX_FACTIONS) return BRAIN_ERR_CAPACITY;
    idx = 0;
    while (idx < w->faction_count && w->factions[idx].id < faction->id) ++idx;
    if (idx < w->faction_count && w->factions[idx].id == faction->id) return BRAIN_ERR_CONFLICT;
    copy = *faction;
    memmove(&w->factions[idx + 1], &w->factions[idx],
            (size_t)(w->faction_count - idx) * sizeof(w->factions[0]));
    w->factions[idx] = copy;
    w->faction_count++;
    return BRAIN_OK;
}

BrainResult brain_world_add_npc(BrainWorld *w, const BrainNpc *npc) {
    uint16_t idx;
    BrainNpc copy;
    if (!w || !npc || !npc->id || !npc->town_id || npc->role < BRAIN_ROLE_VILLAGER || npc->role > BRAIN_ROLE_GUARD ||
        npc->health_percent > 100) return BRAIN_ERR_ARGUMENT;
    if (w->npc_count >= BRAIN_MAX_NPCS) return BRAIN_ERR_CAPACITY;
    if (!find_town(w, npc->town_id) || (npc->faction_id && !has_faction(w, npc->faction_id)))
        return BRAIN_ERR_NOT_FOUND;
    idx = 0;
    while (idx < w->npc_count && w->npcs[idx].id < npc->id) ++idx;
    if (idx < w->npc_count && w->npcs[idx].id == npc->id) return BRAIN_ERR_CONFLICT;
    copy = *npc;
    memmove(&w->npcs[idx + 1], &w->npcs[idx],
            (size_t)(w->npc_count - idx) * sizeof(w->npcs[0]));
    w->npcs[idx] = copy;
    w->npc_count++;
    return BRAIN_OK;
}

BrainResult brain_world_advance(BrainWorld *w, uint64_t ticks, int is_host) {
    BrainClock next;
    uint64_t days = 0;
    uint16_t i;
    uint32_t j;
    BrainResult status;
    if (!w) return BRAIN_ERR_ARGUMENT;
    if (!is_host) return BRAIN_ERR_NOT_HOST;
    for (i = 0; i < w->town_count; ++i)
        for (j = 0; j < BRAIN_COMMODITY_COUNT; ++j)
            if (brain_market_validate(&w->towns[i].goods[j]) != BRAIN_OK)
                return BRAIN_ERR_ARGUMENT;
    next = w->clock;
    status = brain_clock_advance(&next, ticks, &days);
    if (status != BRAIN_OK) return status;
    /* Daily constant-rate aggregate update is O(town count), even for years. */
    for (i = 0; i < w->town_count; ++i)
        for (j = 0; j < BRAIN_COMMODITY_COUNT; ++j)
            (void)brain_market_advance(&w->towns[i].goods[j], days);
    w->clock = next;
    return BRAIN_OK;
}

BrainResult brain_world_intent(const BrainWorld *w, BrainId npc_id,
                               const BrainPerception *perception, BrainIntent *out) {
    const BrainNpc *npc;
    if (!w || !out || !npc_id) return BRAIN_ERR_ARGUMENT;
    npc = find_npc(w, npc_id);
    if (!npc) return BRAIN_ERR_NOT_FOUND;
    return brain_npc_decide(npc, &w->clock, perception, out);
}

BrainResult brain_world_buy(BrainWorld *w, BrainWallet *wallet,
                             const BrainPurchase *request, BrainEvent *out, int is_host) {
    BrainTown *town;
    BrainCommodity *good;
    const BrainEvent *existing;
    BrainEvent record = {0};
    uint64_t total;
    if (!w || !wallet || !request || !out || (!request->event_id.branch || !request->event_id.serial) ||
        !request->actor_id || !request->town_id || wallet->actor_id != request->actor_id ||
        request->commodity >= BRAIN_COMMODITY_COUNT || !request->quantity ||
        wallet->gold > BRAIN_MAX_GOLD) return BRAIN_ERR_ARGUMENT;
    if (!is_host) return BRAIN_ERR_NOT_HOST;
    record.id = request->event_id;
    record.type = BRAIN_EVENT_PURCHASE;
    record.actor_id = request->actor_id;
    record.target_a = request->town_id;
    record.arg_a = (int32_t)request->commodity;
    if (request->quantity > INT32_MAX) return BRAIN_ERR_ARGUMENT;
    record.arg_b = (int32_t)request->quantity;
    existing = find_event(w, request->event_id);
    if (existing) {
        if (existing->type == record.type && existing->actor_id == record.actor_id &&
            existing->target_a == record.target_a && existing->arg_a == record.arg_a &&
            existing->arg_b == record.arg_b) { *out = *existing; return BRAIN_DUPLICATE; }
        return BRAIN_ERR_CONFLICT;
    }
    if (w->event_count == BRAIN_MAX_EVENTS) return BRAIN_ERR_CAPACITY;
    town = find_town(w, request->town_id);
    if (!town) return BRAIN_ERR_NOT_FOUND;
    good = &town->goods[request->commodity];
    if (brain_market_validate(good) != BRAIN_OK) return BRAIN_ERR_ARGUMENT;
    if (request->quantity > good->stock) return BRAIN_ERR_STOCK;
    if (wallet->holdings[request->commodity] > BRAIN_MAX_STOCK ||
        request->quantity > BRAIN_MAX_STOCK - wallet->holdings[request->commodity])
        return BRAIN_ERR_CAPACITY;
    total = (uint64_t)brain_market_price(good) * request->quantity;
    if (total > BRAIN_MAX_GOLD || total > wallet->gold) return BRAIN_ERR_FUNDS;
    /* Atomic in-memory prototype: all preconditions checked before mutation. */
    wallet->gold -= (uint32_t)total;
    wallet->holdings[request->commodity] += request->quantity;
    good->stock -= request->quantity;
    record.price_paid = (uint32_t)total;
    w->events[w->event_count++] = record;
    *out = record;
    return BRAIN_OK;
}

BrainResult brain_world_change_standing(BrainWorld *w,
                                         const BrainStandingChange *req,
                                         BrainEvent *out, int is_host) {
    BrainRelation *relation = NULL;
    BrainId a, b;
    BrainEvent ev = {0};
    const BrainEvent *existing;
    int standing;
    uint16_t index;
    if (!w || !req || !out || (!req->event_id.branch || !req->event_id.serial) || !req->actor_id ||
        !req->faction_a || !req->faction_b || req->faction_a == req->faction_b ||
        req->delta < -100 || req->delta > 100) return BRAIN_ERR_ARGUMENT;
    if (!is_host) return BRAIN_ERR_NOT_HOST;
    a = req->faction_a < req->faction_b ? req->faction_a : req->faction_b;
    b = req->faction_a < req->faction_b ? req->faction_b : req->faction_a;
    ev.id = req->event_id;
    ev.type = BRAIN_EVENT_FACTION_CHANGE;
    ev.actor_id = req->actor_id;
    ev.target_a = a; ev.target_b = b;
    ev.arg_a = req->delta;
    existing = find_event(w, req->event_id);
    if (existing) {
        if (existing->type == ev.type && existing->actor_id == ev.actor_id &&
            existing->target_a == ev.target_a && existing->target_b == ev.target_b &&
            existing->arg_a == ev.arg_a) { *out = *existing; return BRAIN_DUPLICATE; }
        return BRAIN_ERR_CONFLICT;
    }
    if (!has_faction(w, a) || !has_faction(w, b)) return BRAIN_ERR_NOT_FOUND;
    if (w->event_count >= BRAIN_MAX_EVENTS) return BRAIN_ERR_CAPACITY;
    index = 0;
    while (index < w->relation_count && (w->relations[index].a < a ||
           (w->relations[index].a == a && w->relations[index].b < b))) ++index;
    if (index < w->relation_count && w->relations[index].a == a &&
        w->relations[index].b == b) relation = &w->relations[index];
    if (!relation) {
        if (w->relation_count >= BRAIN_MAX_RELATIONS) return BRAIN_ERR_CAPACITY;
        memmove(&w->relations[index + 1], &w->relations[index],
                (size_t)(w->relation_count - index) * sizeof(w->relations[0]));
        relation = &w->relations[index];
        relation->a = a; relation->b = b; relation->standing = 0;
        ++w->relation_count;
    }
    standing = (int)relation->standing + req->delta;
    if (standing < -100) standing = -100;
    if (standing > 100) standing = 100;
    relation->standing = (int16_t)standing;
    w->events[w->event_count++] = ev;
    *out = ev;
    return BRAIN_OK;
}

/* Portable field-wise FNV-1a; never hash C padding or pointers. */
static void h64(uint64_t *h, uint64_t value) {
    unsigned b;
    for (b = 0; b < 8; ++b) {
        *h ^= (value >> (b * 8)) & UINT64_C(255);
        *h *= UINT64_C(1099511628211);
    }
}
uint64_t brain_world_hash(const BrainWorld *w) {
    uint64_t h = UINT64_C(14695981039346656037);
    uint16_t i;
    uint32_t g;
    if (!w) return 0;
    h64(&h, BRAIN_API_VERSION); h64(&h, w->world_id.hi); h64(&h, w->world_id.lo);
    h64(&h, w->seed); h64(&h, w->generator_version); h64(&h, w->rules_version);
    h64(&h, w->clock.minute); h64(&h, w->clock.tick_remainder);
    h64(&h, w->clock.ticks_per_minute);
    h64(&h, w->town_count);
    for (i = 0; i < w->town_count; ++i) {
        const BrainTown *t = &w->towns[i];
        h64(&h, t->id); h64(&h, (uint64_t)t->sector_x); h64(&h, (uint64_t)t->sector_y);
        h64(&h, t->population); h64(&h, t->security);
        for (g = 0; g < BRAIN_COMMODITY_COUNT; ++g) {
            const BrainCommodity *c = &t->goods[g];
            h64(&h, c->stock); h64(&h, c->production_per_day);
            h64(&h, c->consumption_per_day); h64(&h, c->desired_stock);
            h64(&h, c->base_price);
        }
    }
    h64(&h, w->npc_count);
    for (i = 0; i < w->npc_count; ++i) {
        const BrainNpc *n = &w->npcs[i];
        h64(&h, n->id); h64(&h, n->town_id); h64(&h, n->faction_id);
        h64(&h, n->role); h64(&h, n->health_percent);
    }
    h64(&h, w->faction_count);
    for (i = 0; i < w->faction_count; ++i) {
        h64(&h, w->factions[i].id); h64(&h, (uint16_t)w->factions[i].influence);
    }
    h64(&h, w->relation_count);
    for (i = 0; i < w->relation_count; ++i) {
        h64(&h, w->relations[i].a); h64(&h, w->relations[i].b);
        h64(&h, (uint16_t)w->relations[i].standing);
    }
    h64(&h, w->event_count);
    for (i = 0; i < w->event_count; ++i) {
        const BrainEvent *e = &w->events[i];
        h64(&h, e->id.branch); h64(&h, e->id.serial);
        h64(&h, e->type); h64(&h, e->actor_id);
        h64(&h, e->target_a); h64(&h, e->target_b);
        h64(&h, (uint32_t)e->arg_a); h64(&h, (uint32_t)e->arg_b);
        h64(&h, e->price_paid);
    }
    return h;
}
