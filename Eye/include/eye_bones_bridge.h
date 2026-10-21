#ifndef EYE_BONES_BRIDGE_H
#define EYE_BONES_BRIDGE_H

#include "eye3.h"
#include "eye_tone.h"

/* Bones 0.2 retains third_party/eye/sw_renderer.[ch] from v0.1. Its legacy
 * material renderer has three 64x64 BGRA-in-memory/0xFFRRGGBB texel slots.
 * Eye does NOT implement a new BONES_SLOT_EYE descriptor. Call from existing
 * Eye adapter AFTER its p144_renderer_init() and BEFORE presenting a frame.
 *
 * No BONES headers are required by this library. The integration caller owns
 * the target memory. Native Bones integration is pending a real source build. */
#define EYE_BONES_LEGACY_SLOTS 3u
#define EYE_BONES_LEGACY_TEXELS (EYE_SIZE * EYE_SIZE)

/* material_ids map Bones material slots [stone, floor, wood] to Eye recipe
 * indices; duplicate indices are legal. Destination: three contiguous blocks
 * of EYE_SIZE*EYE_SIZE uint32_t pixels. Returns zero without changing target
 * on invalid arguments. Call once when the Eye backend is initialized, or
 * when its textures must be regenerated; NEVER for each pixel/frame. */
int eye_bones_legacy_fill(const EyeTextureBank *bank,
                          const unsigned material_ids[EYE_BONES_LEGACY_SLOTS],
                          uint32_t *target,
                          size_t target_capacity);

/* Same layout as eye_bones_legacy_fill, optional grading for all 3 slots. */
int eye_bones_legacy_fill_tone(const EyeTextureBank *bank,
                          const unsigned material_ids[EYE_BONES_LEGACY_SLOTS],
                          uint32_t *target, size_t target_capacity,
                          EyeTone tone);

#endif
