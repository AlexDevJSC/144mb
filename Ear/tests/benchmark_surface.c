#include "ear.h"
#include "ear_surface.h"
#include "surface_materials.h"
#include <stdint.h>
#include <stdio.h>
static EarEngine audio;
static int16_t buffer[1024];
int main(void) {
    P144SurfaceBinding binding;
    EarSurfaceEvent event = {42u, 1u, 10u, 5u, EAR_CONTACT_STEP,
                             EAR_ACOUSTIC_WOOD, 200u, 0u};
    ear_init(&audio, NULL);
    ear_trigger(&audio, EAR_SFX_STEP, 42u);
    if (!p144_surface_default_binding(P144_VIS_METAL, &binding)) return 1;
    event.acoustic = binding.acoustic_id;
    if (!ear_surface_event(&audio, &event, UINT32_C(0x1442026))) return 1;
    ear_render(&audio, buffer, 1024);
    printf("%d\n", buffer[12]);
    return 0;
}
