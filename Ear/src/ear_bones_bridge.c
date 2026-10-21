#include "ear_bones_bridge.h"

void ear_bones_bridge_init(EarBonesBridge *b, EarEngine *audio,
                           uint32_t world_seed) {
    if (!b) return;
    b->audio = audio;
    b->world_seed = world_seed;
}

void ear_bones_emit(EarBonesBridge *b, EarBonesEventKind kind, uint64_t event_id) {
    static const EarSfx event_to_sfx[] = {
        EAR_SFX_STEP, EAR_SFX_SWORD, EAR_SFX_HIT, EAR_SFX_PICKUP,
        EAR_SFX_MAGIC, EAR_SFX_UI, EAR_SFX_DOOR
    };
    uint32_t seed;
    if (!b || !b->audio || kind < 0 || kind > EAR_BONES_DOOR_OPEN) return;
    seed = (uint32_t)event_id ^ (uint32_t)(event_id >> 32) ^ b->world_seed;
    ear_trigger(b->audio, event_to_sfx[kind], seed);
}

int ear_bones_scene(EarBonesBridge *b, EarScore *storage,
                    uint32_t area_seed, EarStyle style) {
    EarMusicConfig cfg;
    if (!b || !storage || !b->audio) return 0;
    cfg.seed = b->world_seed ^ area_seed;
    cfg.root = 0; cfg.bpm = 0; cfg.style = (uint8_t)style;
    if (!ear_compose(storage, cfg)) return 0;
    ear_set_score(b->audio, storage);
    return 1;
}
