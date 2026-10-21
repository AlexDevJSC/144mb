#ifndef BONES_FOUNDATION_H
#define BONES_FOUNDATION_H
/* Experimental additive API for Bones 0.3. Bones 0.2 public header remains
   byte-identical. This is a SOURCE API, not stable binary plugin ABI. */
#include "bones.h"
#include <stddef.h>

#define BONES_FOUNDATION_VERSION 1u
#define BONES_FOUNDATION_MAX_ENTITIES 32u
#define BONES_FOUNDATION_MAX_COMMANDS 32u
#define BONES_FOUNDATION_MAX_FACTS 64u
#define BONES_FOUNDATION_SNAPSHOT_MAX 8192u

typedef enum { BONES_KIND_PLAYER=1, BONES_KIND_NPC=2, BONES_KIND_OBJECT=3 } BonesEntityKind;
typedef struct {
    uint64_t id;                 /* scoped by world_id; no pointer/address identity */
    uint32_t owner;              /* 0 = world/server, >0 assigned client/actor */
    uint8_t kind;
    uint8_t enabled;
    uint64_t last_sequence;      /* replay protection within this session */
    BonesPlayer body;            /* shared movement and collision rules */
} BonesEntity;
/* Commands are INPUTS, not decisions about damage/loot/economy.
   Tick equals the NEXT simulation tick. ONE command per actor per tick.
   The caller asserting issuer identity must be trusted/authenticated by Mouth. */
typedef struct {
    uint64_t actor_id;
    uint64_t tick;
    uint64_t sequence;
    uint32_t issuer;
    int16_t forward;             /* -1, 0 or +1; not untrusted floating-point */
    int16_t strafe;
    int16_t yaw_milliradians;    /* limited to [-300,300] per tick */
    uint8_t sprint;
    uint8_t jump;
} BonesCommand;
typedef enum { BONES_FACT_SPAWN=1, BONES_FACT_REMOVE=2 } BonesFactKind;
typedef struct {
    uint64_t sequence, tick, subject;
    uint32_t kind;
} BonesFact;
/* No file I/O, no UDP, no UI. Per-world runtime owns the old engine.
   Stable slots hold entities; identifiers NEVER derive from array indices. */
typedef struct {
    BonesEngine engine;
    uint64_t world_id;           /* DISTINCT from procedural seed */
    uint32_t branch_id;          /* branch namespace, assigned by Skin; nonzero */
    uint64_t next_entity_id;
    uint64_t next_fact_sequence;
    uint32_t entity_count;
    BonesEntity entities[BONES_FOUNDATION_MAX_ENTITIES];
    uint8_t in_step;
    uint64_t executing_tick;
    uint32_t command_count;
    BonesCommand commands[BONES_FOUNDATION_MAX_COMMANDS];
    uint32_t fact_count;
    BonesFact facts[BONES_FOUNDATION_MAX_FACTS];
} BonesFoundation;

/* Branch must be unique among diverging saves from the same world.
   Stable IDs: (uint64_t)branch_id<<32 | local serial.
   ID=1 is reserved for the initial local player; namespace 0 is base.
   This does NOT implement branch ancestry or merging; Skin owns those. */
bool bones_foundation_init(BonesFoundation *f, uint64_t world_id,
                           uint64_t seed, uint32_t branch_id);
const BonesEntity *bones_foundation_find(const BonesFoundation *f, uint64_t id);
/* Changes affecting persistent identity add facts; journal is bounded.
   Caller must explicitly ACK facts only after they are durably handled. */
bool bones_foundation_spawn(BonesFoundation *f, BonesEntityKind kind,
                            uint32_t owner, BonesVec3 feet, uint64_t *out_id);
bool bones_foundation_remove(BonesFoundation *f, uint64_t entity_id);
/* Host-side authority boundary. A network peer cannot call blindly as 'issuer'. */
bool bones_foundation_submit(BonesFoundation *f, const BonesCommand *cmd);
/* Run legacy engine and other entities with SAME physics routine; null input
   means an idle local player. Must never call bones_step separately. */
bool bones_foundation_step(BonesFoundation *f, const BonesInput *local_input);
uint32_t bones_foundation_read_facts(const BonesFoundation *f, uint64_t after,
                                     BonesFact *out, uint32_t capacity);
bool bones_foundation_ack_facts(BonesFoundation *f, uint64_t through);
/* Canonical little-endian buffer, CRC32 and fixed version. Caller owns storage.
   For Skin: snapshot bytes, NOT a safe-save implementation, merge or file format.
   For Mouth: snapshot baseline, NOT delta protocol or transport. */
bool bones_foundation_export(const BonesFoundation *f, uint8_t *dst,
                             size_t capacity, size_t *written);
bool bones_foundation_restore(BonesFoundation *f, const uint8_t *src, size_t length);
uint64_t bones_foundation_hash(const BonesFoundation *f);
/* Scene wrapper includes extra remote players/NPCs; returns false on full
   sprite buffer instead of silently omitting an actor. */
bool bones_foundation_make_scene(const BonesFoundation *f, BonesScene *scene);
bool bones_foundation_render_frame(BonesFoundation *f);

/* Plug-in folders export a BonesSubsystem factory with the existing v1
   descriptor. Generated registry is source-driven, not DLL loading. */
bool bones_register_project_modules(BonesFoundation *f, void *eye_context);
#endif
