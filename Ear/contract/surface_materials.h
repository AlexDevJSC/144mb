#ifndef P144_SURFACE_MATERIALS_H
#define P144_SURFACE_MATERIALS_H

#include "p144_acoustic_ids.h"
#include <stdint.h>

/* Proposed SHARED AUTHORING RECORD; the visual ID is an OPAQUE value for Ear.
   Owners: Bones stores/resolves; Eye reads visual_material_id; Ear receives
   *only* acoustic_id from Bones contact notifications. */
typedef struct {
    uint16_t visual_material_id;
    uint8_t acoustic_id;
    uint8_t flags;            /* reserved 0 for this prototype */
} P144SurfaceBinding;

/* Suggested defaults only for the eight visual families supplied by Eye.
   These enums are NOT claimed to match Eye's real numeric material IDs. */
typedef enum {
    P144_VIS_STONE, P144_VIS_BRICK, P144_VIS_WOOD, P144_VIS_EARTH,
    P144_VIS_METAL, P144_VIS_FABRIC, P144_VIS_VEGETATION, P144_VIS_PLASTER,
    P144_VIS_COUNT
} P144VisualFamilyProposal;

/* Returns 1 for valid family and binds acoustic category; allows overrides. */
int p144_surface_default_binding(uint16_t visual_family,
                                 P144SurfaceBinding *binding);
int p144_surface_binding_valid(const P144SurfaceBinding *binding);

#endif
