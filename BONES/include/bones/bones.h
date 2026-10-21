#ifndef BONES_CORE_H
#define BONES_CORE_H
/* Bones 0.2: C11 core with static complementary subsystems.
   Source API, no DLL ABI promise, no OS/renderer dependencies. */
#include <stdint.h>
#include <stdbool.h>

#define BONES_API_VERSION 2u
#define BONES_SUBSYSTEM_API_VERSION 1u
#define BONES_GENERATOR_VERSION 1u
#define BONES_TICK_RATE 60u
#define BONES_TICK_DT (1.0f / 60.0f)
#define BONES_MAX_COLLIDERS 16u
#define BONES_MAX_MODULES 8u /* legacy extension callbacks */
#define BONES_MAX_EVENTS 32u
#define BONES_MAX_QUADS 96u
#define BONES_MAX_SPRITES 8u

typedef struct { float x, y, z; } BonesVec3;
typedef struct { BonesVec3 min, max; } BonesAabb;
typedef struct { int64_t x, z; } BonesSectorCoord;

typedef enum {
    BONES_MAT_STONE = 0, BONES_MAT_FLOOR = 1, BONES_MAT_WOOD = 2
} BonesMaterial;

typedef struct {
    uint64_t id;
    BonesAabb bounds;
    uint8_t solid;
    uint8_t render_box; /* 0: invisible collider; 1: submit box quads */
    uint8_t material;
} BonesCollider;

typedef struct {
    uint64_t seed;
    uint32_t generator_version;
    uint32_t collider_count;
    BonesCollider colliders[BONES_MAX_COLLIDERS];
} BonesWorld;

typedef struct {
    BonesVec3 feet;      /* Y coordinate at bottom of upright capsule approximation */
    BonesVec3 velocity;  /* metres / second */
    float yaw, pitch;   /* radians */
    float radius, height, eye_height;
    bool grounded;
    bool jump_held; /* jump on key down transition; no automatic bunny hopping */
} BonesPlayer;

typedef struct {
    uint8_t forward, backward, left, right, sprint, jump;
    uint8_t yaw_left, yaw_right, pitch_up, pitch_down;
    float mouse_dx, mouse_dy; /* DELTAS applied exactly once per fixed tick */
} BonesInput;

typedef struct { BonesVec3 position; float yaw, pitch; } BonesCamera;
typedef struct { BonesVec3 position; float u, v; } BonesVertex;
typedef struct { BonesVertex vertices[4]; uint8_t material; } BonesQuad;
typedef struct { BonesVec3 feet; float width, height; uint32_t animation_tick; } BonesSprite;
typedef struct {
    BonesCamera camera;
    BonesQuad quads[BONES_MAX_QUADS];
    BonesSprite sprites[BONES_MAX_SPRITES];
    uint32_t quad_count, sprite_count;
} BonesScene;

/* Slots are unique and optional. Roles are provisional; the slot names are fixed.
   Each attached subsystem supplies its own context and code statically linked in C.
   A declared dependency is another slot, not a filename or a DLL. */
typedef enum {
    BONES_SLOT_EYE = 0, BONES_SLOT_EAR = 1, BONES_SLOT_BRAIN = 2,
    BONES_SLOT_SKIN = 3, BONES_SLOT_MOUTH = 4, BONES_SLOT_COUNT = 5
} BonesSubsystemSlot;
#define BONES_SLOT_BIT(slot) (1u << (unsigned)(slot))
#define BONES_ALL_SLOTS ((1u << BONES_SLOT_COUNT) - 1u)

typedef enum {
    BONES_EVENT_JUMP = 1, BONES_EVENT_LAND = 2, BONES_EVENT_CUSTOM = 256
} BonesEventType;
typedef struct {
    uint32_t type;            /* 1..255 reserved by Bones; 256+ integration-local */
    uint64_t source_id;
    uint64_t target_id;
} BonesEvent;

struct BonesEngine;
/* SUBSYSTEM CONTRACT v1. Descriptor is copied; pointed-to state belongs to
   integrator and MUST outlive registration -> bones_stop. No allocation or
   dynamic library is required. All callbacks are trusted application code. */
typedef struct BonesSubsystem {
    uint32_t version;         /* BONES_SUBSYSTEM_API_VERSION */
    uint32_t struct_size;     /* sizeof(BonesSubsystem), rejects layout drift */
    BonesSubsystemSlot slot;
    uint32_t requires;        /* bitmask of required subsystem slots */
    void *context;
    bool (*startup)(void *context, struct BonesEngine *engine);
    void (*shutdown)(void *context, struct BonesEngine *engine);
    void (*before_tick)(void *context, struct BonesEngine *engine, const BonesInput *input);
    void (*after_tick)(void *context, struct BonesEngine *engine);
    void (*on_event)(void *context, const struct BonesEngine *engine, const BonesEvent *event);
    void (*build_scene)(void *context, const struct BonesEngine *engine, BonesScene *scene);
    /* Only EYE can own the final framebuffer/presentation callback. */
    void (*present)(void *context, const BonesScene *scene);
} BonesSubsystem;

typedef struct BonesModule {
    uint32_t version;
    void *user_data;
    /* Called after core physics, in registration order. Module must obey tick contract. */
    void (*on_tick)(void *user_data, struct BonesEngine *engine);
    /* Called when creating a render snapshot. May append via bones_scene_add_* only. */
    void (*on_scene)(void *user_data, const struct BonesEngine *engine, BonesScene *scene);
} BonesModule;

typedef struct BonesEngine {
    BonesWorld world;
    BonesPlayer player;
    uint64_t tick;
    BonesModule modules[BONES_MAX_MODULES];
    uint32_t module_count;
    BonesSubsystem subsystems[BONES_SLOT_COUNT];
    uint8_t subsystem_present[BONES_SLOT_COUNT];
    uint8_t subsystem_order[BONES_SLOT_COUNT];
    uint8_t subsystem_count;
    uint8_t subsystem_started;
    uint8_t subsystem_transition; /* prevent reentrant attach/start/stop */
    uint8_t active_subsystems;
    uint8_t event_overflow;   /* sticky until explicit clear or bones_stop */
    uint32_t event_count;
    BonesEvent events[BONES_MAX_EVENTS];
} BonesEngine;

typedef struct {
    uint32_t version;
    void *user_data;
    void (*begin)(void *user_data, const BonesCamera *camera);
    void (*draw_quad)(void *user_data, const BonesCamera *camera, const BonesQuad *quad);
    void (*draw_sprite)(void *user_data, const BonesCamera *camera, const BonesSprite *sprite);
    void (*end)(void *user_data);
} BonesRenderBackend;

void bones_init(BonesEngine *engine, uint64_t seed);
/* Validated insertion for complementary world modules; duplicate IDs are rejected. */
bool bones_world_add_collider(BonesWorld *world, const BonesCollider *collider);
bool bones_world_remove_collider(BonesWorld *world, uint64_t id);
/* Rejects non-fixed or invalid dt; does not change engine on failure. */
bool bones_step(BonesEngine *engine, const BonesInput *input, float dt);
/* Legacy optional callbacks remain available for v0.1 integrations;
   avoid for new Ear/Eye/Brain/Skin/Mouth modules. */
bool bones_register_module(BonesEngine *engine, const BonesModule *module);
/* Register before simulation starts, only once per slot. Startup checks
   versions, dependencies, cycles and invokes hooks in a stable order. */
bool bones_attach_subsystem(BonesEngine *engine, const BonesSubsystem *subsystem);
bool bones_start(BonesEngine *engine);       /* idempotent; also auto-starts on first step */
void bones_stop(BonesEngine *engine);        /* inverse-order shutdown, idempotent */
bool bones_subsystem_attached(const BonesEngine *engine, BonesSubsystemSlot slot);
/* Ephemeral in-process notification queue: NOT save/network gameplay journal.
   Emit returns false on invalid/full queue, setting event_overflow if full. */
bool bones_emit_event(BonesEngine *engine, const BonesEvent *event);
void bones_clear_event_overflow(BonesEngine *engine);
uint64_t bones_sector_key(uint64_t seed, uint32_t generator_version, BonesSectorCoord sector, uint64_t domain);
bool bones_scene_add_quad(BonesScene *scene, const BonesQuad *quad);
bool bones_scene_add_sprite(BonesScene *scene, const BonesSprite *sprite);
void bones_make_scene(const BonesEngine *engine, BonesScene *out);
bool bones_render(const BonesEngine *engine, const BonesRenderBackend *backend);
/* Draw a previously built snapshot (no duplicate scene generation). */
bool bones_draw_scene(const BonesScene *scene, const BonesRenderBackend *backend);
/* Optional Eye plugin: scene assembled once, passed readonly to EYE.present.
   Returns false if no Eye plugin or startup fails. */
bool bones_render_frame(BonesEngine *engine);

#endif
