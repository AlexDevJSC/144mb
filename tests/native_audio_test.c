/* SPDX-License-Identifier: MIT */
#define _XOPEN_SOURCE 700
#include "compat.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL FluidSynth line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)
#define MODULE "libfluidsynth-3.dll"
#define LOAD(type, name, signature) \
    type (NT_MS_ABI *name) signature = (type (NT_MS_ABI *) signature)nt_runtime_resolve(MODULE, #name); \
    CHECK(name != NULL)

int main(int argc, char **argv)
{
    if (nt_runtime_resolve(MODULE, "new_fluid_settings") == NULL) {
        puts("SKIP native FluidSynth ABI: libfluidsynth.so.3 unavailable");
        return argc == 2 ? 1 : 0;
    }
    LOAD(void *, new_fluid_settings, (void));
    LOAD(int, fluid_settings_setnum, (void *, const char *, double));
    LOAD(void, delete_fluid_settings, (void *));
    LOAD(void *, new_fluid_synth, (void *));
    LOAD(void, delete_fluid_synth, (void *));
    LOAD(int, fluid_synth_write_float, (void *, int, void *, int, int, void *, int, int));
    LOAD(int, fluid_synth_sfload, (void *, const char *, int));
    LOAD(int, fluid_synth_sfunload, (void *, int, int));
    LOAD(int, fluid_synth_noteon, (void *, int, int, int));
    LOAD(int, fluid_synth_noteoff, (void *, int, int));
    LOAD(void, fluid_synth_set_gain, (void *, float));
    LOAD(int, fluid_synth_get_active_voice_count, (void *));

    void *settings = new_fluid_settings();
    CHECK(settings != NULL && fluid_settings_setnum(settings, "synth.sample-rate", 44100) == 0);
    void *synth = new_fluid_synth(settings);
    CHECK(synth != NULL);
    fluid_synth_set_gain(synth, 0.7f);
    float left[1024], right[1024];
    for (size_t i = 0; i < 1024; ++i)
        left[i] = right[i] = NAN;
    CHECK(fluid_synth_write_float(synth, 1024, left, 0, 1, right, 0, 1) == 0);
    CHECK(fluid_synth_get_active_voice_count(synth) == 0);
    for (size_t i = 0; i < 1024; ++i)
        CHECK(fabsf(left[i]) < 0.000001f && fabsf(right[i]) < 0.000001f);

    if (argc == 2) {
        char source[PATH_MAX], guest_path[PATH_MAX];
        CHECK(realpath(argv[1], source) != NULL);
        char *filename = strrchr(source, '/');
        CHECK(filename != NULL && filename != source);
        *filename++ = '\0';
        CHECK(snprintf(guest_path, sizeof(guest_path), "C:\\%s", filename) < (int)sizeof(guest_path));
        CHECK(nt_runtime_set_file_root(source));
        CHECK(fluid_synth_sfload(synth, "C:\\..\\unavailable.sf2", 1) == -1);
        const int soundfont = fluid_synth_sfload(synth, guest_path, 1);
        CHECK(soundfont >= 0);
        CHECK(fluid_synth_noteon(synth, 0, 60, 100) == 0);
        CHECK(fluid_synth_write_float(synth, 1024, left, 0, 1, right, 0, 1) == 0);
        bool audible = false;
        for (size_t i = 0; i < 1024; ++i) {
            CHECK(isfinite(left[i]) && isfinite(right[i]));
            audible |= fabsf(left[i]) > 0.00001f || fabsf(right[i]) > 0.00001f;
        }
        CHECK(audible && fluid_synth_get_active_voice_count(synth) > 0);
        CHECK(fluid_synth_noteoff(synth, 0, 60) == 0);
        CHECK(fluid_synth_sfunload(synth, soundfont, 0) == 0 && nt_runtime_set_file_root(NULL));
        puts("PASS FluidSynth loaded the Linux SoundFont through C: and rendered a MIDI note");
    }
    delete_fluid_synth(synth);
    delete_fluid_settings(settings);
    puts("PASS native FluidSynth settings, synthesis and cleanup");
    return 0;
}
