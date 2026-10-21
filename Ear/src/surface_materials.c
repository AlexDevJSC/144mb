#include "surface_materials.h"

int p144_surface_binding_valid(const P144SurfaceBinding *binding) {
    return binding && binding->acoustic_id < P144_ACOUSTIC_COUNT &&
           binding->flags == 0;
}

int p144_surface_default_binding(uint16_t family, P144SurfaceBinding *out) {
    static const uint8_t acoustic_for_family[P144_VIS_COUNT] = {
        P144_ACOUSTIC_MINERAL, P144_ACOUSTIC_MINERAL, P144_ACOUSTIC_WOOD,
        P144_ACOUSTIC_EARTH, P144_ACOUSTIC_METAL, P144_ACOUSTIC_FABRIC,
        P144_ACOUSTIC_VEGETATION, P144_ACOUSTIC_MINERAL
    };
    if (!out || family >= P144_VIS_COUNT) return 0;
    out->visual_material_id = family;
    out->acoustic_id = acoustic_for_family[family];
    out->flags = 0;
    return 1;
}
