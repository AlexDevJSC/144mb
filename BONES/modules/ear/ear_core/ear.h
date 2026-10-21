#ifndef EAR_H
#define EAR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EAR_API_VERSION 1u
#define EAR_SAMPLE_RATE 22050u
#define EAR_TICKS_PER_BEAT 480u
#define EAR_BARS 16u
#define EAR_BEATS_PER_BAR 4u
#define EAR_MAX_EVENTS 384u
#define EAR_MUSIC_VOICES 16u
#define EAR_SFX_VOICES 8u
#define EAR_TOTAL_VOICES (EAR_MUSIC_VOICES + EAR_SFX_VOICES)

typedef enum {
    EAR_STYLE_DUNGEON = 0,
    EAR_STYLE_TAVERN = 1,
    EAR_STYLE_WILDERNESS = 2,
    EAR_STYLE_COMBAT = 3
} EarStyle;

typedef enum {
    EAR_PATCH_FLUTE = 0,
    EAR_PATCH_LUTE = 1,
    EAR_PATCH_BASS = 2,
    EAR_PATCH_HORN = 3,
    EAR_PATCH_BELL = 4,
    EAR_PATCH_NOISE = 5,
    EAR_PATCH_DRUM = 6
} EarPatch;

typedef enum {
    EAR_SFX_STEP = 0,
    EAR_SFX_SWORD = 1,
    EAR_SFX_HIT = 2,
    EAR_SFX_PICKUP = 3,
    EAR_SFX_MAGIC = 4,
    EAR_SFX_UI = 5,
    EAR_SFX_DOOR = 6
} EarSfx;

typedef struct {
    uint32_t seed;
    uint16_t bpm;       /* 45..180. Zero selects a style-dependent default. */
    uint8_t root;       /* MIDI note for root (48..71 recommended). */
    uint8_t style;      /* EarStyle */
} EarMusicConfig;

typedef struct {
    uint32_t tick;
    uint16_t duration;  /* ticks, 480 per quarter */
    uint8_t midi;       /* 0..127 */
    uint8_t patch;      /* EarPatch */
    uint8_t velocity;   /* 1..127 */
    uint8_t reserved;
} EarNote;

typedef struct {
    EarMusicConfig config;
    uint32_t length_ticks;
    uint16_t count;
    EarNote notes[EAR_MAX_EVENTS];
} EarScore;

/* User programmable SFX recipe; generates sound, not an asset. */
typedef struct {
    uint8_t patch;      /* one of EarPatch */
    uint8_t volume;     /* 0..127 */
    uint16_t duration_ms; /* 20..3000 */
    float freq_start;   /* 20..8000 Hz */
    float freq_end;     /* 20..8000 Hz */
    uint32_t seed;
} EarSfxRecipe;

typedef struct {
    float phase;
    float freq_start;
    float freq_end;
    float level;
    float filter;
    uint32_t rng;
    uint32_t age;
    uint32_t length;
    uint8_t patch;
    uint8_t active;
} EarVoice;

typedef struct {
    const EarScore *score; /* borrowed; caller must keep it alive */
    uint64_t frames_total;
    uint32_t loop_frame;
    uint32_t loop_frames;
    uint16_t next_note;
    uint8_t music_enabled;
    float music_volume;
    float sfx_volume;
    EarVoice voices[EAR_TOTAL_VOICES];
} EarEngine;

/* C11 portable, no file system, Win32 headers or allocation in audio core. */
int ear_compose(EarScore *out, EarMusicConfig config);
void ear_init(EarEngine *engine, const EarScore *score);
void ear_set_score(EarEngine *engine, const EarScore *score);
/* Disable stops music notes but preserves SFX and score position. */
void ear_music_enable(EarEngine *engine, int enabled);
void ear_set_volumes(EarEngine *engine, float music, float sfx);
void ear_render(EarEngine *engine, int16_t *pcm, size_t frames);
void ear_trigger(EarEngine *engine, EarSfx effect, uint32_t variation_seed);
int ear_trigger_recipe(EarEngine *engine, EarSfxRecipe recipe);
uint64_t ear_score_hash(const EarScore *score);

#ifdef __cplusplus
}
#endif
#endif
