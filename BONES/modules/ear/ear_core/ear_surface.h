#ifndef EAR_SURFACE_H
#define EAR_SURFACE_H

#include "ear.h"
#include "p144_acoustic_ids.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic sound families, not Eye texture or mesh identifiers.
   Stable numeric IDs are part of the Ear <-> Bones contract v1. */
#define EAR_SURFACE_API_VERSION 1u

typedef P144AcousticCategory EarAcousticMaterial;
/* Ear-readable aliases; numerical values live in the neutral contract. */
enum {
    EAR_ACOUSTIC_MINERAL = P144_ACOUSTIC_MINERAL,
    EAR_ACOUSTIC_WOOD = P144_ACOUSTIC_WOOD,
    EAR_ACOUSTIC_METAL = P144_ACOUSTIC_METAL,
    EAR_ACOUSTIC_EARTH = P144_ACOUSTIC_EARTH,
    EAR_ACOUSTIC_VEGETATION = P144_ACOUSTIC_VEGETATION,
    EAR_ACOUSTIC_FABRIC = P144_ACOUSTIC_FABRIC,
    EAR_ACOUSTIC_COUNT = P144_ACOUSTIC_COUNT
};

typedef enum {
    EAR_CONTACT_STEP = 1,
    EAR_CONTACT_IMPACT = 2,
    EAR_CONTACT_LANDING = 3
} EarContactKind;

/* Logical, transient contact EVENT already resolved by the world/physics.
   NO pointer, physics collider, Eye type or OS handle crosses the boundary.
   This is a proposed additive Bones payload; Bones 0.2.0 does NOT yet
   expose these fields in its existing BonesEvent {type, source_id,target_id}.
   Serialisation, if ever needed, must be field-by-field and versioned. */
typedef struct {
    uint64_t event_id;        /* variation key; zero is legal in local prototypes */
    uint64_t source_id;       /* who stepped/hit */
    uint64_t surface_id;      /* world surface/collider identifier */
    uint32_t tick;            /* logical simulation tick */
    uint8_t kind;            /* EarContactKind */
    uint8_t acoustic;        /* EarAcousticMaterial */
    uint8_t intensity;       /* 1..255 from Bones, zero = no audible contact */
    uint8_t reserved;
} EarSurfaceEvent;

/* Returns 1 if input accepted and synthesized, 0 on invalid input.
   This modifies ONLY Ear audio state, never Eyes textures or Bones physics.
   Caller serialises operations with ear_render (no concurrent access). */
int ear_surface_event(EarEngine *audio, const EarSurfaceEvent *event,
                      uint32_t world_seed);

#ifdef __cplusplus
}
#endif
#endif
