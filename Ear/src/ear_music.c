#include "ear.h"

#include <string.h>

/* All composition decisions use integer arithmetic. Keep generator versioned. */
static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= UINT32_C(0x7feb352d);
    x ^= x >> 15; x *= UINT32_C(0x846ca68b);
    x ^= x >> 16; return x;
}

static uint32_t rng_next(uint32_t *state) {
    *state += UINT32_C(0x9e3779b9);
    return hash32(*state);
}

static int add_note(EarScore *s, uint32_t tick, uint16_t duration,
                    int midi, int patch, int velocity) {
    EarNote *n;
    if (s->count >= EAR_MAX_EVENTS || tick >= s->length_ticks ||
        midi < 0 || midi > 127 || velocity <= 0 || velocity > 127) return 0;
    n = &s->notes[s->count++];
    n->tick = tick; n->duration = duration; n->midi = (uint8_t)midi;
    n->patch = (uint8_t)patch; n->velocity = (uint8_t)velocity;
    n->reserved = 0;
    return 1;
}

static int compare_notes(const EarNote *a, const EarNote *b) {
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    if (a->patch != b->patch) return a->patch < b->patch ? -1 : 1;
    return a->midi < b->midi ? -1 : a->midi > b->midi;
}

static void sort_notes(EarScore *s) {
    unsigned i;
    /* At most 384 events, sort only on composition, never in audio callback. */
    for (i = 1; i < s->count; ++i) {
        EarNote key = s->notes[i];
        unsigned j = i;
        while (j > 0 && compare_notes(&s->notes[j - 1], &key) > 0) {
            s->notes[j] = s->notes[j - 1]; --j;
        }
        s->notes[j] = key;
    }
}

static int scale_note(int root, int mode, int degree) {
    static const int minor[7] = {0, 2, 3, 5, 7, 8, 10};
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11};
    static const int dorian[7] = {0, 2, 3, 5, 7, 9, 10};
    const int *scale = mode == 1 ? major : mode == 2 ? dorian : minor;
    int octave = 0;
    while (degree < 0) { degree += 7; --octave; }
    while (degree >= 7) { degree -= 7; ++octave; }
    return root + scale[degree] + octave * 12;
}

int ear_compose(EarScore *out, EarMusicConfig cfg) {
    /* 16-bar form A (4) - A' (4) - B (4) - A cadence (4).
       Musical rhythm, motifs, root movement and cadences are structural. */
    static const int minor_progression[4] = {0, 5, 3, 4};
    static const int major_progression[4] = {0, 3, 4, 0};
    static const int combat_progression[4] = {0, 6, 5, 4};
    static const int melodic_steps[8] = {0, 2, 4, 2, 1, 3, 2, 0};
    uint32_t random;
    int motif[8], progress[4], root, mode, b, i;
    if (!out || cfg.style > EAR_STYLE_COMBAT ||
        (cfg.bpm != 0 && (cfg.bpm < 45 || cfg.bpm > 180))) return 0;
    memset(out, 0, sizeof(*out));
    if (cfg.root == 0) cfg.root = cfg.style == EAR_STYLE_TAVERN ? 60 : 57;
    if (cfg.root < 48 || cfg.root > 71) return 0;
    if (cfg.bpm == 0) {
        static const uint16_t tempos[4] = {66, 104, 82, 132};
        cfg.bpm = tempos[cfg.style];
    }
    out->config = cfg;
    out->length_ticks = EAR_BARS * EAR_BEATS_PER_BAR * EAR_TICKS_PER_BEAT;
    root = cfg.root;
    mode = cfg.style == EAR_STYLE_TAVERN ? 1 :
           cfg.style == EAR_STYLE_WILDERNESS ? 2 : 0;
    random = hash32(cfg.seed ^ UINT32_C(0x45415211));
    for (i = 0; i < 8; ++i) {
        /* Repeatable melodic identity, with bounded deviation from skeleton. */
        int offset = (int)(rng_next(&random) % 3u) - 1;
        motif[i] = melodic_steps[i] + offset;
        if (motif[i] < 0) motif[i] = 0;
        if (motif[i] > 6) motif[i] = 6;
    }
    motif[0] = 0; motif[7] = 0; /* recognisable tonic anchors */
    for (i = 0; i < 4; ++i) {
        progress[i] = cfg.style == EAR_STYLE_TAVERN ? major_progression[i] :
                      cfg.style == EAR_STYLE_COMBAT ? combat_progression[i] :
                      minor_progression[i];
    }
    for (b = 0; b < (int)EAR_BARS; ++b) {
        const int section = b / 4;
        const int harmonic_degree = (b == 15) ? 0 : progress[b % 4];
        const int chord_root = scale_note(root, mode, harmonic_degree);
        const uint32_t base = (uint32_t)b * 4u * EAR_TICKS_PER_BEAT;
        const int motif_offset = (b % 2) * 4;
        const int strong = cfg.style == EAR_STYLE_COMBAT;
        const int melody_patch = cfg.style == EAR_STYLE_TAVERN ? EAR_PATCH_LUTE :
                                 strong ? EAR_PATCH_HORN : EAR_PATCH_FLUTE;
        const int chord_patch = cfg.style == EAR_STYLE_TAVERN ? EAR_PATCH_LUTE :
                                EAR_PATCH_HORN;
        int beat;
        /* Bass: half notes outline each bar's harmony. */
        if (!add_note(out, base, 840u, chord_root - 12, EAR_PATCH_BASS, 70) ||
            !add_note(out, base + 960u, 820u,
                      scale_note(root, mode, harmonic_degree + 4) - 19,
                      EAR_PATCH_BASS, 58)) return 0;
        /* Tonic, third, fifth chord, sparse enough for the lo-fi voice budget. */
        if (cfg.style == EAR_STYLE_DUNGEON || cfg.style == EAR_STYLE_WILDERNESS) {
            if (!add_note(out, base, 1720u, chord_root, chord_patch, 30) ||
                !add_note(out, base, 1720u,
                          scale_note(root, mode, harmonic_degree + 2), chord_patch, 25)) return 0;
        } else {
            for (beat = 0; beat < 4; ++beat) {
                int third = scale_note(root, mode, harmonic_degree + 2);
                int note = beat % 2 ? third : chord_root;
                if (!add_note(out, base + (uint32_t)beat * 480u, 340u,
                              note, chord_patch, 44)) return 0;
            }
        }
        for (beat = 0; beat < 4; ++beat) {
            int degree = motif[motif_offset + beat];
            int note;
            uint16_t duration = strong ? 350u : 410u;
            /* B section modulates contour; A and A' preserve motif. */
            if (section == 2) degree = (degree + 2 + (b % 2)) % 7;
            if (section == 1 && beat == 3) degree = (degree + 1) % 7;
            if (b == 15 && beat == 3) { degree = 0; duration = 450u; }
            note = scale_note(root + 12, mode, degree);
            if (!add_note(out, base + (uint32_t)beat * 480u,
                          duration, note, melody_patch, beat == 0 ? 90 : 72)) return 0;
        }
        /* A differentiated pulse adds rhythm without consuming a sample file. */
        if (strong || cfg.style == EAR_STYLE_TAVERN) {
            for (beat = 0; beat < 4; ++beat)
                if (!add_note(out, base + (uint32_t)beat * 480u, 96u,
                              36, EAR_PATCH_DRUM, beat == 0 ? 44 : 29)) return 0;
        }
    }
    /* Last beat of final bar is an explicit tonic, not a randomized flourish. */
    sort_notes(out);
    return 1;
}

uint64_t ear_score_hash(const EarScore *s) {
    uint64_t h = UINT64_C(14695981039346656037);
    uint32_t i;
    if (!s) return 0;
#define ADD_BYTE(v) do { h ^= (uint8_t)(v); h *= UINT64_C(1099511628211); } while (0)
#define ADD_U32(v) do { uint32_t x_ = (uint32_t)(v); int k_; \
    for (k_ = 0; k_ < 4; ++k_) ADD_BYTE(x_ >> (8 * k_)); } while (0)
    ADD_U32(EAR_API_VERSION);
    ADD_U32(s->config.seed); ADD_U32(s->config.bpm);
    ADD_BYTE(s->config.root); ADD_BYTE(s->config.style);
    ADD_U32(s->length_ticks); ADD_U32(s->count);
    for (i = 0; i < s->count; ++i) {
        const EarNote *n = &s->notes[i];
        ADD_U32(n->tick); ADD_U32(n->duration);
        ADD_BYTE(n->midi); ADD_BYTE(n->patch); ADD_BYTE(n->velocity);
    }
#undef ADD_U32
#undef ADD_BYTE
    return h;
}
