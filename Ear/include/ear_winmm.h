#ifndef EAR_WINMM_H
#define EAR_WINMM_H

/* This is the only public module including Windows types. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "ear.h"
#define EAR_WINMM_BUFFERS 4
#define EAR_WINMM_FRAMES 512

typedef struct {
    HWAVEOUT device;
    WAVEHDR headers[EAR_WINMM_BUFFERS];
    int16_t samples[EAR_WINMM_BUFFERS][EAR_WINMM_FRAMES];
    EarEngine *engine;
    int ready;
} EarWinMM;

int ear_winmm_open(EarWinMM *out, EarEngine *engine);
void ear_winmm_pump(EarWinMM *out);
void ear_winmm_close(EarWinMM *out);
#endif
#endif
