#ifndef EYE_BONES_04_EXT_H
#define EYE_BONES_04_EXT_H
/* Eye-owned OPTIONAL development helpers; these are NOT ModKit ABI services.
 * Available only in the statically linked Eye provider. */
#include "eye5.h"
#include <stdint.h>
int bones_eye_mod_roof_preview(const Eye5RoofSpec *spec,
                                float x,float y,float z,unsigned material_id);
int bones_eye_mod_roof_texture_preview(int enabled);
const uint32_t *bones_eye_mod_pixels(void);
uint32_t bones_eye_mod_triangles(void);
#endif
