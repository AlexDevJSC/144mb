/*
 * ADAPTER SOURCE EXAMPLE, NOT A REPLACEMENT BONES SUBSYSTEM.
 * When copied to Bones/src/adapters/eye_adapter.c, remove any duplicate
 * function naming and use actual renderer member names from EyeAdapter.
 *
 * This translation unit is compiled against a TEST DOUBLE in the Eye CI;
 * compiling against BONES 0.2 real source remains a separate acceptance gate.
 */
#include "bones/bones.h"
#include "sw_renderer.h"
#include "eye_bones_compat.h"
#include "eye_bones_bridge.h"

_Static_assert(P144_TEX_SIZE == EYE_SIZE,
               "Bones texture resolution changed, revisit bridge");
_Static_assert(P144_MAT_COUNT == EYE_BONES_LEGACY_SLOTS,
               "Bones legacy material count changed, revisit bridge");

int eye_bones_example_upload(P144_Renderer *backend_renderer,
                             const EyeTextureBank *bank) {
    const unsigned bones_to_eye[EYE_BONES_LEGACY_SLOTS] = {0u, 10u, 7u};
    if (!backend_renderer) return 0;
    return eye_bones_legacy_fill(bank, bones_to_eye,
        &backend_renderer->tex[0][0],
        sizeof backend_renderer->tex / sizeof backend_renderer->tex[0][0]);
}
