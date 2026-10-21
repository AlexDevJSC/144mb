#include "bones_internal.h"
#include <string.h>

/* Stable priority in case several subsystems have no dependency ordering. */
static const uint8_t priority[BONES_SLOT_COUNT] = {
    BONES_SLOT_BRAIN, BONES_SLOT_EAR, BONES_SLOT_SKIN,
    BONES_SLOT_MOUTH, BONES_SLOT_EYE
};

bool bones_subsystem_attached(const BonesEngine *e, BonesSubsystemSlot slot) {
    return e && (unsigned)slot < BONES_SLOT_COUNT && e->subsystem_present[slot] != 0;
}

bool bones_attach_subsystem(BonesEngine *e, const BonesSubsystem *m) {
    uint32_t own;
    if (!e || !m || m->version != BONES_SUBSYSTEM_API_VERSION ||
        m->struct_size != sizeof(*m) || (unsigned)m->slot >= BONES_SLOT_COUNT ||
        e->tick != 0 || e->subsystem_started || e->subsystem_transition || e->active_subsystems ||
        e->subsystem_present[m->slot]) return false;
    own = BONES_SLOT_BIT(m->slot);
    if ((m->requires & ~BONES_ALL_SLOTS) != 0 || (m->requires & own) != 0)
        return false;
    if (m->present && m->slot != BONES_SLOT_EYE) return false;
    e->subsystems[m->slot] = *m;
    e->subsystem_present[m->slot] = 1;
    e->subsystem_count++;
    return true;
}

bool bones_start(BonesEngine *e) {
    uint32_t existing = 0, scheduled = 0;
    uint8_t count = 0;
    uint32_t slot, j;
    if (!e) return false;
    if (e->subsystem_transition) return false;
    if (e->subsystem_started) return true;
    /* Plan first, call nothing on missing dep or cycle. */
    for (slot = 0; slot < BONES_SLOT_COUNT; ++slot)
        if (e->subsystem_present[slot]) existing |= BONES_SLOT_BIT(slot);
    for (slot = 0; slot < BONES_SLOT_COUNT; ++slot)
        if (e->subsystem_present[slot] &&
            (e->subsystems[slot].requires & existing) != e->subsystems[slot].requires)
            return false;
    while (scheduled != existing) {
        bool found = false;
        for (j = 0; j < BONES_SLOT_COUNT; ++j) {
            uint32_t candidate = priority[j];
            uint32_t bit = BONES_SLOT_BIT(candidate);
            if (!(existing & bit) || (scheduled & bit)) continue;
            if ((e->subsystems[candidate].requires & scheduled) !=
                e->subsystems[candidate].requires) continue;
            e->subsystem_order[count++] = (uint8_t)candidate;
            scheduled |= bit;
            found = true;
            break;
        }
        if (!found) return false; /* cycle */
    }
    e->active_subsystems = 0;
    e->subsystem_transition = 1;
    for (j = 0; j < count; ++j) {
        BonesSubsystem *m = &e->subsystems[e->subsystem_order[j]];
        if (m->startup && !m->startup(m->context, e)) {
            while (e->active_subsystems) {
                BonesSubsystem *done = &e->subsystems[e->subsystem_order[--e->active_subsystems]];
                if (done->shutdown) done->shutdown(done->context, e);
            }
            e->subsystem_transition = 0;
            return false;
        }
        e->active_subsystems++;
    }
    e->subsystem_started = 1;
    e->subsystem_transition = 0;
    return true;
}

void bones_stop(BonesEngine *e) {
    if (!e || e->subsystem_transition) return;
    e->subsystem_transition = 1;
    while (e->active_subsystems) {
        BonesSubsystem *m = &e->subsystems[e->subsystem_order[--e->active_subsystems]];
        if (m->shutdown) m->shutdown(m->context, e);
    }
    e->subsystem_started = 0;
    e->event_count = 0;
    e->event_overflow = 0;
    e->subsystem_transition = 0;
}

bool bones_emit_event(BonesEngine *e, const BonesEvent *event) {
    if (!e || !event || event->type == 0) return false;
    if (e->event_count == BONES_MAX_EVENTS) {
        e->event_overflow = 1;
        return false;
    }
    e->events[e->event_count++] = *event;
    return true;
}

void bones_clear_event_overflow(BonesEngine *e) {
    if (e) e->event_overflow = 0;
}

/* Called after the physics tick; events created by handlers are consumed FIFO
   within the same tick until empty. The fixed capacity prevents unbounded
   callback-driven recursion and heap allocation. */
void bones_dispatch_events(BonesEngine *e) {
    uint32_t cursor = 0, j;
    while (cursor < e->event_count) {
        BonesEvent event = e->events[cursor++];
        for (j = 0; j < e->active_subsystems; ++j) {
            BonesSubsystem *m = &e->subsystems[e->subsystem_order[j]];
            if (m->on_event) m->on_event(m->context, e, &event);
        }
    }
    e->event_count = 0;
}

bool bones_render_frame(BonesEngine *e) {
    BonesSubsystem *eye;
    BonesScene scene;
    if (!e || !bones_subsystem_attached(e, BONES_SLOT_EYE) || !bones_start(e)) return false;
    eye = &e->subsystems[BONES_SLOT_EYE];
    if (!eye->present) return false;
    bones_make_scene(e, &scene);
    eye->present(eye->context, &scene);
    return true;
}
