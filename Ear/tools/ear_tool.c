/* Ear offline authoring tool (NOT included in the game executable). */
#include "ear.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EarScore score;
static EarEngine audio;

static int style_from_string(const char *s) {
    if (strcmp(s, "dungeon") == 0) return EAR_STYLE_DUNGEON;
    if (strcmp(s, "tavern") == 0) return EAR_STYLE_TAVERN;
    if (strcmp(s, "wilderness") == 0) return EAR_STYLE_WILDERNESS;
    if (strcmp(s, "combat") == 0) return EAR_STYLE_COMBAT;
    return -1;
}
static int sfx_from_string(const char *s) {
    const char *options[] = {"step", "sword", "hit", "pickup", "magic", "ui", "door"};
    unsigned i;
    for (i = 0; i < sizeof(options)/sizeof(options[0]); ++i)
        if (strcmp(s, options[i]) == 0) return (int)i;
    return -1;
}
static int patch_from_string(const char *s) {
    const char *options[] = {"flute", "lute", "bass", "horn", "bell", "noise", "drum"};
    unsigned i;
    for (i = 0; i < sizeof(options)/sizeof(options[0]); ++i)
        if (strcmp(s, options[i]) == 0) return (int)i;
    return -1;
}

static void le16(FILE *f, uint16_t n) {
    fputc((int)(n & 255u), f); fputc((int)((n >> 8) & 255u), f);
}
static void le32(FILE *f, uint32_t n) {
    le16(f, (uint16_t)n); le16(f, (uint16_t)(n >> 16));
}

static int write_wav(const char *path, uint32_t frames) {
    FILE *f;
    int16_t pcm[2048];
    uint32_t remaining = frames;
    if (frames == 0 || frames > 180u * EAR_SAMPLE_RATE) return 0;
    f = fopen(path, "wb");
    if (!f) { perror(path); return 0; }
    fwrite("RIFF", 1, 4, f); le32(f, frames * 2u + 36u);
    fwrite("WAVEfmt ", 1, 8, f); le32(f, 16u);
    le16(f, 1u); le16(f, 1u); le32(f, EAR_SAMPLE_RATE);
    le32(f, EAR_SAMPLE_RATE * 2u); le16(f, 2u); le16(f, 16u);
    fwrite("data", 1, 4, f); le32(f, frames * 2u);
    while (remaining > 0) {
        unsigned i, n = remaining < 2048u ? remaining : 2048u;
        ear_render(&audio, pcm, n);
        for (i = 0; i < n; ++i) le16(f, (uint16_t)pcm[i]);
        remaining -= n;
    }
    if (fclose(f) != 0) { perror(path); return 0; }
    printf("EAR: %s (%u Hz mono 16-bit, %.2f s, %u bytes)\n",
           path, EAR_SAMPLE_RATE, (double)frames / EAR_SAMPLE_RATE, 44u + frames * 2u);
    return 1;
}

static int parse_uint(const char *s, uint32_t *out) {
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(s, &end, 0);
    if (errno || end == s || *end || value > UINT32_MAX) return 0;
    *out = (uint32_t)value;
    return 1;
}

static void usage(void) {
    puts("Ear v0.1 - synthesis from code and deterministic musical composition");
    puts("  ear_tool music dungeon|tavern|wilderness|combat SEED output.wav [SECONDS]");
    puts("  ear_tool sfx step|sword|hit|pickup|magic|ui|door output.wav [SEED]");
    puts("  ear_tool recipe 'patch:startHz:endHz:durationMs:volume:seed' output.wav");
    puts("  patches: flute lute bass horn bell noise drum");
    puts("  example: ear_tool recipe 'noise:1800:100:300:95:42' sword.wav");
}

int main(int argc, char **argv) {
    uint32_t seed = 0, secs;
    int choice;
    if (argc < 2) { usage(); return 2; }
    if (strcmp(argv[1], "music") == 0 && (argc == 5 || argc == 6)) {
        EarMusicConfig cfg;
        choice = style_from_string(argv[2]);
        if (choice < 0 || !parse_uint(argv[3], &seed)) { usage(); return 2; }
        cfg.seed = seed; cfg.root = 0; cfg.bpm = 0; cfg.style = (uint8_t)choice;
        if (!ear_compose(&score, cfg)) return 3;
        ear_init(&audio, &score);
        secs = (score.length_ticks * 60u + (EAR_TICKS_PER_BEAT * cfg.bpm) - 1u) /
               (EAR_TICKS_PER_BEAT * score.config.bpm);
        if (argc == 6 && (!parse_uint(argv[5], &secs) || secs > 180)) return 2;
        printf("score: %u notes, %u bpm, 16 bars, hash=%016llx\n",
               (unsigned)score.count, (unsigned)score.config.bpm,
               (unsigned long long)ear_score_hash(&score));
        /* Full cue uses exact duration, preventing silence at end of demo. */
        return write_wav(argv[4], argc == 6 ? secs * EAR_SAMPLE_RATE :
                         (uint32_t)((uint64_t)score.length_ticks * 60u * EAR_SAMPLE_RATE /
                         (score.config.bpm * EAR_TICKS_PER_BEAT))) ? 0 : 4;
    }
    if (strcmp(argv[1], "sfx") == 0 && (argc == 4 || argc == 5)) {
        choice = sfx_from_string(argv[2]);
        if (choice < 0 || (argc == 5 && !parse_uint(argv[4], &seed))) { usage(); return 2; }
        ear_init(&audio, NULL);
        ear_trigger(&audio, (EarSfx)choice, seed);
        return write_wav(argv[3], EAR_SAMPLE_RATE) ? 0 : 4;
    }
    if (strcmp(argv[1], "recipe") == 0 && argc == 4) {
        char patch_name[32], trailing = 0;
        float f0, f1;
        unsigned duration, volume, recipe_seed;
        EarSfxRecipe recipe;
        int count = sscanf(argv[2], "%31[^:]:%f:%f:%u:%u:%u%c",
                           patch_name, &f0, &f1, &duration, &volume, &recipe_seed,
                           &trailing);
        choice = count == 6 ? patch_from_string(patch_name) : -1;
        if (choice < 0 || duration > 3000 || volume > 127) { usage(); return 2; }
        recipe.patch = (uint8_t)choice;
        recipe.freq_start = f0; recipe.freq_end = f1;
        recipe.duration_ms = (uint16_t)duration;
        recipe.volume = (uint8_t)volume; recipe.seed = recipe_seed;
        ear_init(&audio, NULL);
        if (!ear_trigger_recipe(&audio, recipe)) return 3;
        return write_wav(argv[3], (uint32_t)(duration + 200u) * EAR_SAMPLE_RATE / 1000u)
               ? 0 : 4;
    }
    usage();
    return 2;
}
