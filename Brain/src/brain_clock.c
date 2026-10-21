#include "brain/brain.h"
#include <limits.h>

BrainResult brain_clock_init(BrainClock *c, uint64_t minute, uint32_t tpm) {
    if (!c || !tpm) return BRAIN_ERR_ARGUMENT;
    c->minute = minute;
    c->tick_remainder = 0;
    c->ticks_per_minute = tpm;
    return BRAIN_OK;
}

BrainResult brain_clock_advance(BrainClock *c, uint64_t ticks, uint64_t *days) {
    uint64_t add_minutes, rest;
    if (!c || !c->ticks_per_minute || c->tick_remainder >= c->ticks_per_minute)
        return BRAIN_ERR_ARGUMENT;
    add_minutes = ticks / c->ticks_per_minute;
    rest = ticks % c->ticks_per_minute + c->tick_remainder;
    if (rest >= c->ticks_per_minute) { ++add_minutes; rest -= c->ticks_per_minute; }
    if (add_minutes > UINT64_MAX - c->minute) return BRAIN_ERR_OVERFLOW;
    if (days) *days = (c->minute + add_minutes) / BRAIN_MINUTES_PER_DAY -
                      c->minute / BRAIN_MINUTES_PER_DAY;
    c->minute += add_minutes;
    c->tick_remainder = (uint32_t)rest;
    return BRAIN_OK;
}
