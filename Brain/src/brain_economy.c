#include "brain/brain.h"
#include <limits.h>

BrainResult brain_market_validate(const BrainCommodity *c) {
    if (!c || !c->desired_stock || !c->base_price || c->stock > BRAIN_MAX_STOCK ||
        c->desired_stock > BRAIN_MAX_STOCK || c->production_per_day > BRAIN_MAX_STOCK ||
        c->consumption_per_day > BRAIN_MAX_STOCK)
        return BRAIN_ERR_ARGUMENT;
    return BRAIN_OK;
}

BrainResult brain_market_advance(BrainCommodity *c, uint64_t days) {
    uint32_t amount;
    if (brain_market_validate(c) != BRAIN_OK) return BRAIN_ERR_ARGUMENT;
    if (!days || c->production_per_day == c->consumption_per_day) return BRAIN_OK;
    if (c->production_per_day > c->consumption_per_day) {
        amount = c->production_per_day - c->consumption_per_day;
        /* Avoid integer multiplication overflow using ceiling division. */
        if (days >= ((uint64_t)BRAIN_MAX_STOCK - c->stock + amount - 1u) / amount)
            c->stock = BRAIN_MAX_STOCK;
        else c->stock += (uint32_t)(days * amount);
    } else {
        amount = c->consumption_per_day - c->production_per_day;
        if (days >= ((uint64_t)c->stock + amount - 1u) / amount)
            c->stock = 0;
        else c->stock -= (uint32_t)(days * amount);
    }
    return BRAIN_OK;
}

uint32_t brain_market_price(const BrainCommodity *c) {
    int64_t pct, delta, result;
    if (brain_market_validate(c) != BRAIN_OK) return 0;
    /* 100% at desired stock, +/-50% for shortage/surplus up to the cap.
       Smallest price is 1. Calculations are strictly integer. */
    delta = (int64_t)c->desired_stock - (int64_t)c->stock;
    pct = 100 + (50 * delta) / (int64_t)c->desired_stock;
    if (pct < 50) pct = 50;
    if (pct > 200) pct = 200;
    result = ((int64_t)c->base_price * pct + 99) / 100;
    if (result < 1) return 1;
    if (result > BRAIN_MAX_GOLD) return BRAIN_MAX_GOLD;
    return (uint32_t)result;
}
