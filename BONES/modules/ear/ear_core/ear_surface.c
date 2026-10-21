#include "ear_surface.h"

/* Sound is built from existing synthesis patches, with no recorded assets.
   Profiles encode two layers: a dull body, plus optional resonant/transient
   layer. Deliberately distinct for stone, wood and metal footsteps. */
typedef struct {
    uint8_t patch_a, patch_b;
    uint16_t time_a, time_b;
    uint16_t f0_a, f1_a, f0_b, f1_b;
    uint8_t level_a, level_b;
} SurfaceProfile;

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= UINT32_C(0x7feb352d);
    x ^= x >> 15;
    x *= UINT32_C(0x846ca68b);
    x ^= x >> 16;
    return x;
}

static float varied(uint16_t pitch, uint32_t key) {
    /* <= +/-3 percent variation, deterministic per contact, not per frame. */
    int shift = (int)(key % 7u) - 3;
    return (float)pitch * (1.0f + (float)shift * .01f);
}

int ear_surface_event(EarEngine *audio, const EarSurfaceEvent *event,
                      uint32_t world_seed) {
    /* Families: mineral, wood, metal, earth, vegetation, fabric. */
    static const SurfaceProfile steps[EAR_ACOUSTIC_COUNT] = {
        {EAR_PATCH_NOISE, EAR_PATCH_DRUM,  95,  62, 920, 100, 100,  58, 72, 24},
        {EAR_PATCH_LUTE,  EAR_PATCH_NOISE, 130,  65, 190,  95, 630, 155, 78, 18},
        {EAR_PATCH_BELL,  EAR_PATCH_NOISE, 340,  64, 880, 570, 990, 130, 62, 27},
        {EAR_PATCH_NOISE, EAR_PATCH_DRUM, 110,  65, 420,  82,  80,  45, 64, 16},
        {EAR_PATCH_NOISE, EAR_PATCH_LUTE,  145,  70, 1450,180, 260, 105, 65, 13},
        {EAR_PATCH_NOISE, EAR_PATCH_LUTE,  110,  65,  260,  72, 140,  85, 50,  9}
    };
    static const SurfaceProfile impacts[EAR_ACOUSTIC_COUNT] = {
        {EAR_PATCH_DRUM,  EAR_PATCH_NOISE, 190, 110, 220,  55, 1400, 260, 97, 32},
        {EAR_PATCH_LUTE,  EAR_PATCH_DRUM,  270,  82, 135,  65,  120,  45, 94, 23},
        {EAR_PATCH_BELL,  EAR_PATCH_DRUM,  610, 125, 650, 340,  180,  58, 84, 28},
        {EAR_PATCH_DRUM,  EAR_PATCH_NOISE, 170, 105, 155,  40,  380,  80, 69, 32},
        {EAR_PATCH_NOISE, EAR_PATCH_LUTE, 230, 140, 1300, 150, 190,  60, 76, 17},
        {EAR_PATCH_NOISE, EAR_PATCH_LUTE,  180, 100,  300,  80,  95,  42, 60, 15}
    };
    SurfaceProfile p;
    EarSfxRecipe r;
    uint32_t key;
    unsigned factor;
    int played;
    if (!audio || !event || event->acoustic >= EAR_ACOUSTIC_COUNT ||
        event->kind < EAR_CONTACT_STEP || event->kind > EAR_CONTACT_LANDING ||
        event->reserved != 0 || event->intensity == 0) return 0;
    key = mix32((uint32_t)event->event_id ^
                (uint32_t)(event->event_id >> 32) ^
                (uint32_t)event->surface_id ^
                (uint32_t)(event->surface_id >> 32) ^
                (uint32_t)event->source_id ^ event->tick ^ world_seed);
    p = event->kind == EAR_CONTACT_STEP ? steps[event->acoustic] :
                                             impacts[event->acoustic];
    /* LANDING reuses impact timbre, with slightly longer/noisier envelope. */
    if (event->kind == EAR_CONTACT_LANDING && p.time_a < 900u) p.time_a += 35u;
    /* Headroom. Weak footsteps should not have a minimum-loudness surprise. */
    factor = event->intensity;
    r.patch = p.patch_a;
    r.volume = (uint8_t)(((unsigned)p.level_a * factor + 254u) / 255u);
    r.duration_ms = p.time_a;
    r.freq_start = varied(p.f0_a, key);
    r.freq_end = varied(p.f1_a, key);
    r.seed = key;
    played = ear_trigger_recipe(audio, r);
    if (!played) return 0;
    if (p.level_b > 0 && factor >= 16u) {
        r.patch = p.patch_b;
        r.volume = (uint8_t)(((unsigned)p.level_b * factor + 254u) / 255u);
        r.duration_ms = p.time_b;
        r.freq_start = varied(p.f0_b, key >> 9);
        r.freq_end = varied(p.f1_b, key >> 9);
        r.seed = key ^ UINT32_C(0x572241ab);
        (void)ear_trigger_recipe(audio, r);
    }
    return 1;
}
