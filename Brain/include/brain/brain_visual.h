#ifndef P144_BRAIN_VISUAL_H
#define P144_BRAIN_VISUAL_H
/* Reference-only visual persistence experiment (schema 1).
   NOT a SAVE/PBW implementation and NOT an Eye material generator.
   Ownership of global persistence is not assigned by Usos de los chats.txt. */
#include "brain.h"
#include <stddef.h>
#include <stdint.h>

#define BRAIN_VISUAL_SCHEMA_VERSION 1u
#define BRAIN_VISUAL_HEADER_BYTES 48u
#define BRAIN_VISUAL_RECORD_BYTES 56u
#define BRAIN_VISUAL_MAX_RECORDS 512u /* test/prototype guard, not game policy */
#define BRAIN_VIS_OVERRIDE_MATERIAL 1u
#define BRAIN_VIS_OVERRIDE_VARIANT 2u

typedef enum {
    BRAIN_VIS_OK = 0,
    BRAIN_VIS_DUPLICATE = 1,
    BRAIN_VIS_ERR_ARGUMENT = -1,
    BRAIN_VIS_ERR_CAPACITY = -2,
    BRAIN_VIS_ERR_MALFORMED = -3,
    BRAIN_VIS_ERR_WORLD = -4,
    BRAIN_VIS_ERR_EYE_VERSION = -5,
    BRAIN_VIS_ERR_CONFLICT = -6,
    BRAIN_VIS_ERR_NOT_FOUND = -7
} BrainVisualResult;

typedef enum {
    BRAIN_VIS_STATE_DEFAULT = 0,
    BRAIN_VIS_STATE_SCORCHED = 1,
    BRAIN_VIS_STATE_BROKEN = 2,
    BRAIN_VIS_STATE_DESTROYED = 3,
    BRAIN_VIS_STATE_PAINTED = 4
} BrainVisualState;

typedef struct {
    BrainWorldId world_id;     /* World identity is not the world seed. */
    uint64_t world_seed;
    uint32_t world_generator_version;
    uint32_t eye_generator_version; /* Opaque assigned ID provided by Eye. */
} BrainVisualContext;

typedef struct {
    int64_t sector_x, sector_y;
    BrainId object_id;         /* Stable world/Bones ID; NEVER a material ID. */
    uint32_t surface_id;       /* Stable local slot, not an array/draw index. */
} BrainVisualSurfaceKey;

typedef struct {
    BrainVisualSurfaceKey key;
    uint32_t material_override; /* Flags decide presence; 0 is a valid Eye ID. */
    uint32_t variant_override;
    BrainEventId cause;        /* Persistent event ref; NOT an event journal. */
    uint8_t state;             /* Logical change with gameplay meaning. */
    uint8_t flags;
} BrainVisualOverride;

typedef struct {
    uint32_t material_id;   /* Eye's material catalog ID, not entity identity. */
    uint32_t variant_id;    /* Eye's version-specific recipe variant. */
    uint32_t eye_generator_version;
    uint8_t state;
} BrainVisualRef;

typedef struct {
    BrainVisualContext context;
    BrainVisualOverride *entries; /* Caller-owned storage, no allocations. */
    size_t count, capacity;
} BrainVisualSet;

/* `set` stores only gameplay-relevant sparse changes, never decorative pixels. */
BrainVisualResult brain_visual_init(BrainVisualSet *set, BrainVisualContext ctx,
                                     BrainVisualOverride *storage, size_t capacity);
BrainVisualResult brain_visual_upsert(BrainVisualSet *set, BrainVisualOverride change);
BrainVisualResult brain_visual_find(const BrainVisualSet *set,
                                     BrainVisualSurfaceKey key, BrainVisualOverride *out);
BrainVisualResult brain_visual_resolve(const BrainVisualSet *set,
                                        BrainVisualSurfaceKey key,
                                        BrainVisualRef eye_generated_base,
                                        BrainVisualRef *out);
/* No mutation, no pixel output, and no silent upgrade of Eye recipes. */
BrainVisualResult brain_visual_check_eye_version(uint32_t saved_version,
                                                   const uint32_t *available_versions,
                                                   size_t count);
/* Canonical little-endian reference stream: 48 bytes header + 56 per override.
   Output includes checksum for accidental corruption, not authentication.
   File I/O and crash-safe saving belong to a future assigned SAVE owner. */
BrainVisualResult brain_visual_encode(const BrainVisualSet *set,
                                        uint8_t *bytes, size_t capacity, size_t *written);
BrainVisualResult brain_visual_decode(BrainVisualSet *out,
                                        BrainVisualContext expected_world,
                                        const uint8_t *bytes, size_t length,
                                        BrainVisualOverride *storage, size_t capacity);
#endif
