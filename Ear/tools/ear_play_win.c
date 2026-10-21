/* Windows-only playback smoke test; NOT distributed with the RPG. */
#include "ear.h"
#include "ear_winmm.h"
#include <stdio.h>
#include <stdlib.h>

static EarScore score;
static EarEngine audio;
static EarWinMM player;

int main(int argc, char **argv) {
    EarMusicConfig theme = {144u, 0, 0, EAR_STYLE_TAVERN};
    unsigned time_ms = 0;
    if (argc > 1) theme.seed = (uint32_t)strtoul(argv[1], NULL, 0);
    if (!ear_compose(&score, theme)) return 2;
    ear_init(&audio, &score);
    if (!ear_winmm_open(&player, &audio)) {
        fputs("ERROR: no se pudo abrir waveOut.\n", stderr);
        return 3;
    }
    puts("Ear: reproduciendo muestra en waveOut de Windows, 10 segundos.");
    while (time_ms < 10000u) {
        if (time_ms == 2000u) ear_trigger(&audio, EAR_SFX_PICKUP, 123u);
        if (time_ms == 5500u) ear_trigger(&audio, EAR_SFX_SWORD, 321u);
        ear_winmm_pump(&player);
        Sleep(10);
        time_ms += 10u;
    }
    ear_winmm_close(&player);
    return 0;
}
