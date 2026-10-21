/* Offline WAV preview generator. NOT a shipping-game dependency. */
#include "ear_surface.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEMO_FRAMES (EAR_SAMPLE_RATE * 3u / 4u)
static EarEngine audio;

static void u16(FILE *f, uint16_t v) {
    fputc((int)(v & 255u), f); fputc((int)((v >> 8) & 255u), f);
}
static void u32(FILE *f, uint32_t v) { u16(f, (uint16_t)v); u16(f, (uint16_t)(v >> 16)); }
static int write_pcm(FILE *f) {
    int16_t samples[1024];
    unsigned i, left = DEMO_FRAMES;
    fwrite("RIFF", 1, 4, f); u32(f, 36u + left * 2u);
    fwrite("WAVEfmt ", 1, 8, f); u32(f, 16u);
    u16(f, 1); u16(f, 1); u32(f, EAR_SAMPLE_RATE);
    u32(f, EAR_SAMPLE_RATE * 2u); u16(f, 2); u16(f, 16);
    fwrite("data", 1, 4, f); u32(f, left * 2u);
    while (left) {
        unsigned n = left < 1024u ? left : 1024u;
        ear_render(&audio, samples, n);
        for (i = 0; i < n; ++i) u16(f, (uint16_t)samples[i]);
        left -= n;
    }
    return !ferror(f);
}
int main(int argc, char **argv) {
    static const char *category[] = {"stone", "wood", "metal", "earth", "vegetation", "fabric"};
    static const char *event_name[] = {"footstep", "impact", "landing"};
    EarSurfaceEvent ev = {1234u, 1u, 15u, 42u, 0, 0, 210, 0};
    unsigned i, j;
    FILE *f;
    if (argc != 4) {
        fprintf(stderr, "usage: ear_surface_tool footstep|impact|landing stone|wood|metal|earth|vegetation|fabric out.wav\n");
        return 2;
    }
    for (i = 0; i < 3; ++i) if (!strcmp(argv[1], event_name[i])) break;
    for (j = 0; j < EAR_ACOUSTIC_COUNT; ++j) if (!strcmp(argv[2], category[j])) break;
    if (i == 3 || j == EAR_ACOUSTIC_COUNT) return 2;
    ev.kind = (uint8_t)(i + 1);
    ev.acoustic = (uint8_t)j;
    ear_init(&audio, NULL);
    if (!ear_surface_event(&audio, &ev, UINT32_C(0x1442026))) return 3;
    f = fopen(argv[3], "wb");
    if (!f) { perror(argv[3]); return 4; }
    if (!write_pcm(f) || fclose(f)) return 5;
    printf("%s %s: %u frames PCM written to %s\n", argv[1], argv[2], DEMO_FRAMES, argv[3]);
    return 0;
}
