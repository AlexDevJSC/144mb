#include "bones_internal.h"
#include <string.h>
#include <math.h>

/* 64-bit avalanche. Distinct spatial domains never share a global RNG stream. */
static uint64_t mix64(uint64_t k) {
    k ^= k >> 30; k *= UINT64_C(0xbf58476d1ce4e5b9);
    k ^= k >> 27; k *= UINT64_C(0x94d049bb133111eb);
    return k ^ (k >> 31);
}
uint64_t bones_sector_key(uint64_t seed, uint32_t version, BonesSectorCoord c, uint64_t domain) {
    uint64_t h = mix64(seed ^ (uint64_t)version);
    h = mix64(h ^ (uint64_t)c.x);
    h = mix64(h ^ (uint64_t)c.z);
    return mix64(h ^ domain);
}
void bones_init(BonesEngine *e, uint64_t seed) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    bones_world_build_test_room(&e->world, seed);
    e->player.feet = (BonesVec3){0.0f, 0.0f, -7.0f};
    e->player.radius = .32f;
    e->player.height = 1.80f;
    e->player.eye_height = 1.65f;
    e->player.grounded = true;
}
bool bones_register_module(BonesEngine *e, const BonesModule *m) {
    if (!e || !m || m->version != BONES_API_VERSION || e->tick != 0 ||
        e->module_count >= BONES_MAX_MODULES || e->subsystem_started) return false;
    e->modules[e->module_count++] = *m;
    return true;
}
bool bones_step(BonesEngine *e, const BonesInput *i, float dt) {
    uint32_t n;
    if (!e || !i || !isfinite(dt) || fabsf(dt - BONES_TICK_DT) > 0.000001f ||
        !isfinite(i->mouse_dx) || !isfinite(i->mouse_dy)) return false;
    if (!bones_start(e)) return false;
    for (n = 0; n < e->active_subsystems; ++n) {
        BonesSubsystem *m = &e->subsystems[e->subsystem_order[n]];
        if (m->before_tick) m->before_tick(m->context, e, i);
    }
    bones_physics_update(e, i, BONES_TICK_DT);
    e->tick++;
    bones_dispatch_events(e);
    for (n = 0; n < e->active_subsystems; ++n) {
        BonesSubsystem *m = &e->subsystems[e->subsystem_order[n]];
        if (m->after_tick) m->after_tick(m->context, e);
    }
    bones_dispatch_events(e);
    for (n = 0; n < e->module_count; n++)
        if (e->modules[n].on_tick) e->modules[n].on_tick(e->modules[n].user_data, e);
    return true;
}
