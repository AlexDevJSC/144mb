#include "ear.h"

#include <math.h>
#include <float.h>
#include <string.h>

#define EAR_PI 3.14159265358979323846f

static float clampf(float a, float lo, float hi) {
    return a < lo ? lo : a > hi ? hi : a;
}

/* MSVC C mode may not expose the C99 isfinite macro consistently. */
static int ear_finite(float x) {
    return x == x && x <= FLT_MAX && x >= -FLT_MAX;
}

static uint32_t noise_next(uint32_t *rng) {
    uint32_t x = *rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *rng = x ? x : UINT32_C(0x136db5a5);
    return *rng;
}

static float pitch_hz(int midi) {
    return 440.0f * powf(2.0f, ((float)midi - 69.0f) / 12.0f);
}

static uint32_t tick_to_frame(const EarScore *score, uint32_t ticks) {
    /* 64-bit arithmetic avoids overflow for music position. */
    uint64_t numerator = (uint64_t)ticks * EAR_SAMPLE_RATE * 60u;
    return (uint32_t)(numerator /
                      ((uint32_t)score->config.bpm * EAR_TICKS_PER_BEAT));
}

static void play_voice(EarVoice *v, EarPatch patch, float f0, float f1,
                       uint32_t frames, float level, uint32_t seed) {
    memset(v, 0, sizeof(*v));
    v->patch = (uint8_t)patch;
    v->freq_start = f0;
    v->freq_end = f1;
    v->length = frames > 0 ? frames : 1;
    v->level = level;
    v->rng = seed ? seed : UINT32_C(0x6bbd71e9);
    v->active = 1;
}

void ear_init(EarEngine *e, const EarScore *score) {
    if (!e) return;
    memset(e, 0, sizeof(*e));
    e->music_volume = .30f;
    e->sfx_volume = .37f;
    ear_set_score(e, score);
}

void ear_set_score(EarEngine *e, const EarScore *score) {
    unsigned i;
    if (!e) return;
    for (i = 0; i < EAR_MUSIC_VOICES; ++i) e->voices[i].active = 0;
    e->score = score;
    e->next_note = 0;
    e->loop_frame = 0;
    e->music_enabled = score && score->config.bpm >= 45 && score->length_ticks > 0;
    e->loop_frames = e->music_enabled ? tick_to_frame(score, score->length_ticks) : 0;
    if (!e->loop_frames) e->music_enabled = 0;
}

void ear_music_enable(EarEngine *e, int enabled) {
    unsigned i;
    if(!e)return;
    if(!enabled){
        e->music_enabled=0;
        for(i=0;i<EAR_MUSIC_VOICES;i++)e->voices[i].active=0;
    } else if(e->score && e->score->config.bpm>=45 && e->loop_frames) {
        e->music_enabled=1;
    }
}

void ear_set_volumes(EarEngine *e, float music, float sfx) {
    if (!e || !ear_finite(music) || !ear_finite(sfx)) return;
    e->music_volume = clampf(music, 0.0f, 1.0f);
    e->sfx_volume = clampf(sfx, 0.0f, 1.0f);
}

static void start_note(EarEngine *e, unsigned index) {
    const EarNote *n = &e->score->notes[index];
    EarVoice *chosen = NULL;
    uint32_t max_age = 0;
    unsigned i;
    for (i = 0; i < EAR_MUSIC_VOICES; ++i) {
        EarVoice *v = &e->voices[i];
        if (!v->active) { chosen = v; break; }
        if (v->age > max_age) { max_age = v->age; chosen = v; }
    }
    if (chosen) {
        float f = pitch_hz(n->midi);
        uint32_t frame_duration = tick_to_frame(e->score, n->duration);
        uint32_t event_seed = e->score->config.seed ^
                              ((uint32_t)index * UINT32_C(0x9e3779b9));
        play_voice(chosen, (EarPatch)n->patch, f, f, frame_duration,
                   (float)n->velocity / 127.0f, event_seed);
    }
}

int ear_trigger_recipe(EarEngine *e, EarSfxRecipe r) {
    EarVoice *chosen = NULL;
    uint32_t max_age = 0;
    unsigned i;
    if (!e || r.patch > EAR_PATCH_DRUM || r.volume > 127 ||
        r.duration_ms < 20 || r.duration_ms > 3000 ||
        !ear_finite(r.freq_start) || !ear_finite(r.freq_end) ||
        r.freq_start < 20.0f || r.freq_start > 8000.0f ||
        r.freq_end < 20.0f || r.freq_end > 8000.0f) return 0;
    for (i = EAR_MUSIC_VOICES; i < EAR_TOTAL_VOICES; ++i) {
        EarVoice *v = &e->voices[i];
        if (!v->active) { chosen = v; break; }
        if (v->age > max_age) { max_age = v->age; chosen = v; }
    }
    if (!chosen) return 0;
    play_voice(chosen, (EarPatch)r.patch, r.freq_start, r.freq_end,
               (uint32_t)r.duration_ms * EAR_SAMPLE_RATE / 1000u,
               (float)r.volume / 127.0f, r.seed);
    return 1;
}

void ear_trigger(EarEngine *e, EarSfx effect, uint32_t variation_seed) {
    static const EarSfxRecipe presets[] = {
        {EAR_PATCH_NOISE,  63, 110, 290.0f,  110.0f, 0}, /* footsteps */
        {EAR_PATCH_NOISE,  80, 240, 1850.0f, 220.0f, 0}, /* sword swoosh */
        {EAR_PATCH_DRUM,   95, 300, 160.0f,  48.0f, 0}, /* hit */
        {EAR_PATCH_BELL,   65, 350, 710.0f, 1070.0f, 0},/* pickup */
        {EAR_PATCH_BELL,   78, 920, 230.0f, 830.0f, 0}, /* magic */
        {EAR_PATCH_FLUTE,  56, 110, 660.0f, 660.0f, 0}, /* UI */
        {EAR_PATCH_HORN,   73, 570, 120.0f,  60.0f, 0}  /* door */
    };
    EarSfxRecipe recipe;
    if (effect < EAR_SFX_STEP || effect > EAR_SFX_DOOR) return;
    recipe = presets[effect];
    recipe.seed = variation_seed ^ UINT32_C(0x4541525f) ^
                  ((uint32_t)effect * UINT32_C(0x85ebca6b));
    /* bounded seeded pitch difference makes repeated steps less mechanical. */
    if (effect == EAR_SFX_STEP || effect == EAR_SFX_SWORD) {
        float variation = ((float)(variation_seed % 11u) - 5.0f) * .012f;
        recipe.freq_start *= 1.0f + variation;
        recipe.freq_end *= 1.0f + variation;
    }
    (void)ear_trigger_recipe(e, recipe);
}

static float sample_voice(EarVoice *v) {
    float time, freq, phase, sample, env, attack, release;
    if (!v->active) return 0.0f;
    if (v->age >= v->length) { v->active = 0; return 0.0f; }
    time = (float)v->age / (float)v->length;
    freq = v->freq_start + (v->freq_end - v->freq_start) * time;
    freq = clampf(freq, 20.0f, 8000.0f);
    v->phase += freq / (float)EAR_SAMPLE_RATE;
    if (v->phase >= 1.0f) v->phase -= 1.0f;
    phase = v->phase;
    switch (v->patch) {
        case EAR_PATCH_FLUTE:
            sample = sinf(2.0f * EAR_PI * phase) +
                     .09f * sinf(4.0f * EAR_PI * phase);
            break;
        case EAR_PATCH_LUTE:
            sample = (phase < .5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase);
            sample *= (1.0f - .35f * time);
            break;
        case EAR_PATCH_BASS:
            sample = .9f * sinf(2.0f * EAR_PI * phase) +
                     .15f * sinf(4.0f * EAR_PI * phase);
            break;
        case EAR_PATCH_HORN:
            sample = .7f * sinf(2.0f * EAR_PI * phase) +
                     .22f * sinf(6.0f * EAR_PI * phase);
            break;
        case EAR_PATCH_BELL:
            sample = .65f * sinf(2.0f * EAR_PI * phase) +
                     .22f * sinf(2.0f * EAR_PI * phase * 2.71f);
            break;
        case EAR_PATCH_NOISE: {
            float white = ((float)(noise_next(&v->rng) >> 8) / 8388608.0f) - 1.0f;
            v->filter += .20f * (white - v->filter);
            sample = white * .36f + v->filter * .64f;
            break;
        }
        case EAR_PATCH_DRUM: {
            float white = ((float)(noise_next(&v->rng) >> 8) / 8388608.0f) - 1.0f;
            sample = .64f * sinf(2.0f * EAR_PI * phase) + .36f * white;
            break;
        }
        default: sample = 0.0f; break;
    }
    attack = v->patch == EAR_PATCH_FLUTE || v->patch == EAR_PATCH_HORN ? .055f : .006f;
    release = v->patch == EAR_PATCH_LUTE || v->patch == EAR_PATCH_DRUM ||
              v->patch == EAR_PATCH_NOISE ? .70f : .24f;
    env = 1.0f;
    if (time < attack) env = time / attack;
    if (time > 1.0f - release) {
        float tail = (1.0f - time) / release;
        if (tail < env) env = tail;
    }
    if (v->patch == EAR_PATCH_BELL) env *= (1.0f - .8f * time);
    ++v->age;
    if (v->age >= v->length) v->active = 0;
    return sample * env * v->level;
}

void ear_render(EarEngine *e, int16_t *pcm, size_t frames) {
    size_t f;
    if (!e || !pcm) return;
    for (f = 0; f < frames; ++f) {
        float music = 0.0f, effects = 0.0f, mixed;
        unsigned i;
        if (e->music_enabled) {
            while (e->next_note < e->score->count &&
                   tick_to_frame(e->score, e->score->notes[e->next_note].tick) <= e->loop_frame) {
                start_note(e, e->next_note);
                ++e->next_note;
            }
        }
        for (i = 0; i < EAR_MUSIC_VOICES; ++i) music += sample_voice(&e->voices[i]);
        for (; i < EAR_TOTAL_VOICES; ++i) effects += sample_voice(&e->voices[i]);
        mixed = music * e->music_volume + effects * e->sfx_volume;
        /* Gentle limiter keeps peaks under 16-bit range. */
        mixed = mixed / (1.0f + fabsf(mixed));
        pcm[f] = (int16_t)(clampf(mixed, -.999f, .999f) * 32767.0f);
        if (e->music_enabled) {
            if (++e->loop_frame >= e->loop_frames) {
                e->loop_frame = 0;
                e->next_note = 0;
            }
        }
        ++e->frames_total;
    }
}
