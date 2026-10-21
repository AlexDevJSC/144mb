#ifndef BONES_EYE_ADAPTER_H
#define BONES_EYE_ADAPTER_H
#include "../../include/bones/bones.h"
#include "../../third_party/eye/sw_renderer.h"
/* Eye owns graphics data. Bones owns simulation and renderer callback contracts. */
typedef struct {
    P144_Renderer renderer;
    BonesRenderBackend backend;
} BonesEye;
void bones_eye_init(BonesEye *eye, uint32_t texture_seed);
/* Bridge for EYE1/EYE2 or any Eye-owned recipe source. The source fills a
   complete 64x64 ARGB 0xFFRRGGBB texture. Called only on explicit load,
   never per triangle/pixel/frame. Return false to keep prior texture.
   The public BonesMaterial IDs 0..2 retain their old meanings. */
typedef bool (*BonesEyeTextureGenerator)(void *context, uint32_t material_id,
                                         uint32_t seed, uint32_t *out_argb,
                                         uint32_t width, uint32_t height);
/* Returns count loaded, 0..P144_MAT_COUNT. Incompatible/null inputs => 0.
   A failed material retains its previous pixels. No allocations per pixel. */
unsigned bones_eye_load_textures(BonesEye *eye, BonesEyeTextureGenerator generator,
                                 void *context);
/* Static integration: the renderer remains private to Eye. */
BonesSubsystem bones_eye_subsystem(BonesEye *eye);
#endif
