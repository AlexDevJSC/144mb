#include "ear.h"
#include "ear_bones_bridge.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static EarScore a, b, other;
static EarEngine engine_a, engine_b, silent_engine;
static int16_t first[12000], second[12000], silent[12000];

static int is_nonzero(const int16_t *pcm, unsigned n) {
    unsigned i;
    for (i = 0; i < n; ++i) if (pcm[i] != 0) return 1;
    return 0;
}

int main(void) {
    EarMusicConfig cfg = {UINT32_C(0x12345678), 0, 0, EAR_STYLE_TAVERN};
    uint64_t original_hash;
    unsigned i, latest = 0;
    EarBonesBridge bridge;
    EarSfxRecipe recipe = {EAR_PATCH_NOISE, 50, 200, 1000.0f, 110.0f, 47};

    assert(ear_compose(&a, cfg));
    assert(ear_compose(&b, cfg));
    assert(a.count > 100 && a.count < EAR_MAX_EVENTS);
    assert(a.length_ticks == EAR_BARS * EAR_BEATS_PER_BAR * EAR_TICKS_PER_BEAT);
    assert(ear_score_hash(&a) == ear_score_hash(&b));
    original_hash = ear_score_hash(&a);
    cfg.seed++;
    assert(ear_compose(&other, cfg));
    assert(ear_score_hash(&other) != original_hash);
    for (i = 0; i < a.count; ++i) {
        assert(a.notes[i].tick < a.length_ticks);
        assert(a.notes[i].duration > 0);
        assert(a.notes[i].midi <= 127);
        assert(a.notes[i].patch <= EAR_PATCH_DRUM);
        if (i) assert(a.notes[i].tick >= a.notes[i - 1].tick);
        if (a.notes[i].tick == 15u * 4u * 480u + 3u * 480u &&
            a.notes[i].patch == EAR_PATCH_LUTE) latest++;
    }
    assert(latest >= 1);

    ear_init(&engine_a, &a);
    ear_init(&engine_b, &b);
    ear_render(&engine_a, first, 12000);
    ear_render(&engine_b, second, 2000);
    ear_render(&engine_b, second + 2000, 10000);
    assert(memcmp(first, second, sizeof(first)) == 0);
    assert(is_nonzero(first, 12000));
    ear_init(&silent_engine, NULL);
    ear_render(&silent_engine, silent, 12000);
    assert(!is_nonzero(silent, 12000));

    ear_trigger(&silent_engine, EAR_SFX_MAGIC, 17u);
    ear_render(&silent_engine, silent, 12000);
    assert(is_nonzero(silent, 12000));
    ear_init(&silent_engine, NULL);
    recipe.freq_end = 99999.0f;
    assert(!ear_trigger_recipe(&silent_engine, recipe));
    recipe.freq_end = 100.0f;
    assert(ear_trigger_recipe(&silent_engine, recipe));
    ear_render(&silent_engine, silent, 12000);
    assert(is_nonzero(silent, 12000));

    ear_init(&silent_engine, NULL);
    ear_bones_bridge_init(&bridge, &silent_engine, 0x13579u);
    ear_bones_emit(&bridge, EAR_BONES_FOOTSTEP, 100u);
    ear_render(&silent_engine, silent, 12000);
    assert(is_nonzero(silent, 12000));
    assert(ear_bones_scene(&bridge, &other, 1234u, EAR_STYLE_DUNGEON));
    assert(silent_engine.score == &other);
    assert(silent_engine.music_enabled);
    ear_set_volumes(&silent_engine, -12.0f, 99.0f);
    assert(silent_engine.music_volume == 0.0f);
    assert(silent_engine.sfx_volume == 1.0f);

    printf("PASS composition fixed-form, seed hash, sorted notes, stream independence, SFX, Bones adapter\n");
    printf("TAVERN_HASH %016llx NOTES %u\n",
           (unsigned long long)original_hash, (unsigned)a.count);
    return 0;
}
