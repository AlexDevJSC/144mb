/* Both variants compile EXACTLY the same file. Dynamic input prevents
   LTO constant-folding away the contact material dispatch.
   This is a Linux ELF diagnostic, not a Windows PE/game size measurement. */
#include "ear.h"
#ifdef WITH_SURFACE
#include "ear_surface.h"
#include "surface_materials.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
static EarEngine audio;
static int16_t buffer[1024];
int main(int argc, char **argv) {
    unsigned family = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 2u;
    unsigned intensity = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 200u;
    uint32_t seed = (uint32_t)(family * 123u + intensity);
    ear_init(&audio, NULL);
    ear_trigger(&audio, EAR_SFX_STEP, seed);
#ifdef WITH_SURFACE
    {
        P144SurfaceBinding binding;
        EarSurfaceEvent e = { (uint64_t)seed, 1u, 10u, 5u, EAR_CONTACT_STEP,
                              EAR_ACOUSTIC_WOOD, (uint8_t)(intensity % 255u + 1u), 0u };
        if (!p144_surface_default_binding((uint16_t)(family % P144_VIS_COUNT), &binding)) return 1;
        e.acoustic = binding.acoustic_id;
        if (!ear_surface_event(&audio, &e, UINT32_C(0x1442026))) return 1;
    }
#endif
    ear_render(&audio, buffer, 1024);
    printf("%d\n", buffer[(seed % 100u) + 4u]);
    return 0;
}
