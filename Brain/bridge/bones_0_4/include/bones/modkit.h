#ifndef BONES_MODKIT_H
#define BONES_MODKIT_H
/* BONES MODKIT 1: C binary interface for trusted modules, NOT a sandbox.
   C ABI only; caller and module must use compatible host ABI/toolchain.
   Modules MUST NOT include bones.h/foundation.h or access BonesEngine.
   Single-threaded callbacks, no unload/hot reload during execution.
   Public types are fixed-size except function pointers and opaque contexts. */
#include <stdint.h>
#define BONES_MOD_ABI 1u
#define BONES_MOD_LIMIT 16u
#define BONES_MOD_ID_LEN 32u
#define BONES_MOD_QUAD_LIMIT 96u
#define BONES_MOD_SPRITE_LIMIT 8u
#define BONES_MOD_MAX_ACTORS 32u
#define BONES_MOD_OK 0
#define BONES_MOD_EINVAL (-1)
#define BONES_MOD_DENIED (-2)
#define BONES_MOD_FULL (-3)
#define BONES_MOD_MISSING (-4)
#define BONES_MOD_BAD_STATE (-5)

/* Five exclusive providers + optional extension modules. */
#define BONES_ROLE_EYE 0u
#define BONES_ROLE_EAR 1u
#define BONES_ROLE_BRAIN 2u
#define BONES_ROLE_SKIN 3u
#define BONES_ROLE_MOUTH 4u
#define BONES_ROLE_EXTENSION 5u
#define BONES_ROLE_BIT(x) (1u << (x))
#define BONES_ROLE_PROVIDER_MASK 31u

/* Host grants these by role, NEVER trust a module's requested privileges.
   If DLL modules are untrusted, run them in a separate process instead. */
#define BONES_PERMISSION_READ_ACTORS   0x0001u
#define BONES_PERMISSION_SEND_COMMANDS 0x0002u
#define BONES_PERMISSION_READ_SNAPSHOT 0x0004u
#define BONES_PERMISSION_WRITE_SCENE   0x0008u
#define BONES_PERMISSION_EMIT_EVENTS   0x0010u
#define BONES_PERMISSION_READ_FACTS    0x0020u
#define BONES_PERMISSION_READ_TIME     0x0040u
#define BONES_PERMISSION_RESTORE_SNAPSHOT 0x0080u
#define BONES_PERMISSION_LIST_MODULES 0x0100u

#define BONES_EVT_JUMP 1u
#define BONES_EVT_LAND 2u
#define BONES_EVT_CUSTOM 256u

typedef struct {
    uint32_t size;
    uint64_t id;
    uint32_t owner;             /* 0: host/Brain; >0: Mouth-authenticated */
    uint32_t kind;
    float x,y,z, vx,vy,vz, yaw,pitch;
    uint32_t grounded;
} BonesModActor;

typedef struct {
    uint32_t size;
    uint64_t actor_id, tick, sequence;
    uint32_t issuer;
    int32_t forward, strafe;    /* -1 .. 1 */
    int32_t yaw_milliradians;   /* -300 .. 300 */
    uint32_t sprint, jump;
} BonesModCommand;

typedef struct {
    uint32_t size;
    char id[BONES_MOD_ID_LEN];
    uint32_t role, implementation_version, state_schema_version;
} BonesModInfo;

typedef struct {
    uint32_t size;
    uint32_t kind;
    uint64_t sequence, tick, subject;
} BonesModFact;

typedef struct {
    uint32_t size;
    uint32_t type;
    uint64_t source_id, target_id, tick;
} BonesModEvent;

typedef struct {float x,y,z,u,v;} BonesModVertex;
typedef struct {
    uint32_t size;
    BonesModVertex vertices[4];
    uint32_t material_id;       /* Legacy scene only permits 0..2 pending Eye+Soul approval */
    uint32_t flags;             /* 0 until sidedness/culling approved */
} BonesModQuad;
typedef struct {
    uint32_t size;
    float x,y,z,width,height;
    uint32_t animation_tick;
} BonesModSprite;
typedef struct {
    uint32_t size;
    float camera_x,camera_y,camera_z,camera_yaw,camera_pitch;
    uint32_t quad_count,sprite_count;
    const BonesModQuad *quads;       /* valid only during frame callback */
    const BonesModSprite *sprites;   /* never free; no mutation */
    uint64_t tick;
} BonesModFrame;

typedef struct BonesModServices {
    uint32_t size, abi, permissions;
    void *host_context;              /* opaque capability-bound handle */
    uint64_t (*tick)(void *host_context);
    int32_t (*actor_count)(void *host_context);
    int32_t (*actor_at)(void *host_context, uint32_t index, BonesModActor *out);
    int32_t (*actor_get)(void *host_context, uint64_t id, BonesModActor *out);
    int32_t (*submit_move)(void *host_context, const BonesModCommand *cmd);
    int32_t (*snapshot_export)(void *host_context, uint8_t *dst, uint32_t cap, uint32_t *written);
    int32_t (*snapshot_restore_at_start)(void *host_context, const uint8_t *src, uint32_t len);
    int32_t (*facts_since)(void *host_context, uint64_t after, BonesModFact *dst, uint32_t cap);
    int32_t (*module_count)(void *host_context);
    int32_t (*module_at)(void *host_context, uint32_t index, BonesModInfo *out);
    int32_t (*emit_event)(void *host_context, const BonesModEvent *ev);
    int32_t (*scene_add_quad)(void *host_context, const BonesModQuad *quad);
    int32_t (*scene_add_sprite)(void *host_context, const BonesModSprite *sprite);
} BonesModServices;

typedef struct BonesModDescriptor {
    uint32_t size, abi;
    char id[BONES_MOD_ID_LEN];       /* lower ascii [a-z0-9_-], NUL-terminated */
    uint32_t role;
    uint32_t implementation_version; /* monotonically increasing by author; >=1 */
    uint32_t state_schema_version;  /* 0: no private persistent state yet */
    uint32_t required_roles;         /* provider dependency bitmask */
    uint32_t requested_permissions;  /* subset of role's allowed permissions */
    void *user_context;              /* module-owned; lives until stop */
    int32_t (*start)(void *user, const BonesModServices *services);
    void (*stop)(void *user);
    void (*before_tick)(void *user); /* before foundation step: commands for upcoming tick */
    void (*after_tick)(void *user);  /* after foundation step: commands for NEXT tick */
    void (*on_event)(void *user, const BonesModEvent *event);
    void (*build_scene)(void *user); /* services scene_add_* only here */
    void (*present)(void *user, const BonesModFrame *frame); /* Eye only */
} BonesModDescriptor;

typedef int32_t (*BonesModQuery)(uint32_t host_abi, BonesModDescriptor *out);
/* Export exact C symbol 'bones_mod_query_v1' for DLL/shared libraries. */
#ifdef _WIN32
 #define BONES_MOD_EXPORT __declspec(dllexport)
 #define BONES_MOD_CALL __cdecl
#else
 #define BONES_MOD_EXPORT __attribute__((visibility("default")))
 #define BONES_MOD_CALL
#endif
typedef int32_t (BONES_MOD_CALL *BonesDynamicQuery)(uint32_t, BonesModDescriptor*);
#endif
