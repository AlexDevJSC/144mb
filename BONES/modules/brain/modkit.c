/* Brain v0.3 / Bones v0.4 ModKit ABI 1 adapter.
   Install this whole folder at BONES/modules/brain/. No core changes.
   Bones Windows/Makefile currently compiles modkit.c ONLY, so include the
   decision implementation once in this translation unit. */
#include "../../include/bones/modkit.h"
#include "brain.h"
#include <stdint.h>
#include <string.h>

#include "brain_npc.c" /* Unmodified Brain 0.2 decision algorithm, local header. */

#define BRAIN_MOD_TICKS_PER_MINUTE 60u /* DEMO rule, not product canon. */
#define BRAIN_MOD_START_MINUTE 480u     /* Demo world begins at 08:00. */
#define BRAIN_MOD_NPC_KIND 2u           /* Bones 0.4 BONES_KIND_NPC. */

typedef struct {
    const BonesModServices *services;
    uint64_t commands_accepted;
    uint64_t commands_rejected;
} BrainModRuntime;
static BrainModRuntime brain_runtime; /* one authoritative world instance */

static int32_t brain_start(void *user, const BonesModServices *s) {
    BrainModRuntime *state = (BrainModRuntime *)user;
    const uint32_t required = BONES_PERMISSION_READ_ACTORS |
                              BONES_PERMISSION_READ_TIME |
                              BONES_PERMISSION_SEND_COMMANDS;
    if (!state || !s || s->size != sizeof(*s) || s->abi != BONES_MOD_ABI ||
        (s->permissions & required) != required || !s->host_context ||
        !s->tick || !s->actor_count || !s->actor_at || !s->submit_move)
        return BONES_MOD_EINVAL;
    memset(state, 0, sizeof(*state));
    state->services = s;
    return BONES_MOD_OK;
}

static void brain_stop(void *user) {
    BrainModRuntime *state = (BrainModRuntime *)user;
    if (state) state->services = NULL;
}

static void brain_before_tick(void *user) {
    BrainModRuntime *state = (BrainModRuntime *)user;
    const BonesModServices *s;
    uint64_t tick, minute;
    int32_t count, index;
    BrainClock clock;
    if (!state || !(s = state->services)) return;
    tick = s->tick(s->host_context);
    if (tick == UINT64_MAX || tick / BRAIN_MOD_TICKS_PER_MINUTE >
                              UINT64_MAX - BRAIN_MOD_START_MINUTE) return;
    count = s->actor_count(s->host_context);
    if (count < 0 || count > (int32_t)BONES_MOD_MAX_ACTORS) return;
    minute = BRAIN_MOD_START_MINUTE + tick / BRAIN_MOD_TICKS_PER_MINUTE;
    clock.minute = minute;
    clock.tick_remainder = (uint32_t)(tick % BRAIN_MOD_TICKS_PER_MINUTE);
    clock.ticks_per_minute = BRAIN_MOD_TICKS_PER_MINUTE;

    for (index = 0; index < count; ++index) {
        BonesModActor actor;
        BrainNpc npc;
        BrainIntent intention;
        BonesModCommand command;
        uint64_t phase;
        memset(&actor, 0, sizeof(actor));
        actor.size = (uint32_t)sizeof(actor);
        if (s->actor_at(s->host_context, (uint32_t)index, &actor) != BONES_MOD_OK ||
            actor.owner != 0 || actor.kind != BRAIN_MOD_NPC_KIND || !actor.id)
            continue;
        memset(&npc, 0, sizeof(npc));
        npc.id = actor.id;
        npc.role = BRAIN_ROLE_GUARD; /* Temporary fallback: Bones has no NPC role. */
        npc.health_percent = 100;    /* BonesModActor has no health field. */
        if (brain_npc_decide(&npc, &clock, NULL, &intention) != BRAIN_OK) continue;
        if (intention.activity != BRAIN_ACT_PATROL) continue;

        memset(&command, 0, sizeof(command));
        command.size = (uint32_t)sizeof(command);
        command.actor_id = actor.id;
        command.tick = tick + 1;
        command.sequence = tick + 1; /* Single command per actor/tick, replay-safe. */
        command.issuer = 0;          /* Brain can only issue NPC owner=0. */
        /* Simple cyclical walk; NOT navigation or collision avoidance. */
        phase = (tick + (actor.id & UINT64_C(255))) % UINT64_C(240);
        command.forward = 1;
        command.yaw_milliradians = phase < 60 ? 30 : 0;
        if (s->submit_move(s->host_context, &command) == BONES_MOD_OK)
            ++state->commands_accepted;
        else
            ++state->commands_rejected;
    }
}

int32_t bones_mod_brain_query(uint32_t abi, BonesModDescriptor *out) {
    if (abi != BONES_MOD_ABI || !out || out->size != sizeof(*out))
        return BONES_MOD_EINVAL;
    memset(out, 0, sizeof(*out));
    out->size = (uint32_t)sizeof(*out);
    out->abi = BONES_MOD_ABI;
    memcpy(out->id, "brain", sizeof("brain"));
    out->role = BONES_ROLE_BRAIN;
    out->implementation_version = 3;
    out->state_schema_version = 0; /* ModKit has no private-state save API. */
    out->requested_permissions = BONES_PERMISSION_READ_ACTORS |
                                 BONES_PERMISSION_READ_TIME |
                                 BONES_PERMISSION_SEND_COMMANDS;
    out->user_context = &brain_runtime;
    out->start = brain_start;
    out->stop = brain_stop;
    out->before_tick = brain_before_tick;
    return BONES_MOD_OK;
}

/* Optional developer DLL. Static build exports only bones_mod_brain_query. */
#ifdef BRAIN_MOD_BUILD_SHARED
BONES_MOD_EXPORT int32_t BONES_MOD_CALL
bones_mod_query_v1(uint32_t abi, BonesModDescriptor *out) {
    return bones_mod_brain_query(abi, out);
}
#endif
