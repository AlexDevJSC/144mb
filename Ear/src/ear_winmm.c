#ifdef _WIN32
#include "ear_winmm.h"
#include <string.h>

int ear_winmm_open(EarWinMM *out, EarEngine *engine) {
    WAVEFORMATEX fmt;
    unsigned i;
    if (!out || !engine) return 0;
    memset(out, 0, sizeof(*out));
    memset(&fmt, 0, sizeof(fmt));
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 1;
    fmt.nSamplesPerSec = EAR_SAMPLE_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 2;
    fmt.nAvgBytesPerSec = EAR_SAMPLE_RATE * 2u;
    if (waveOutOpen(&out->device, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return 0;
    out->engine = engine;
    for (i = 0; i < EAR_WINMM_BUFFERS; ++i) {
        WAVEHDR *h = &out->headers[i];
        h->lpData = (LPSTR)out->samples[i];
        h->dwBufferLength = sizeof(out->samples[i]);
        ear_render(engine, out->samples[i], EAR_WINMM_FRAMES);
        if (waveOutPrepareHeader(out->device, h, sizeof(*h)) != MMSYSERR_NOERROR)
            goto failure;
        if (waveOutWrite(out->device, h, sizeof(*h)) != MMSYSERR_NOERROR)
            goto failure;
    }
    out->ready = 1;
    return 1;
 failure:
    ear_winmm_close(out);
    return 0;
}

void ear_winmm_pump(EarWinMM *out) {
    unsigned i;
    if (!out || !out->ready) return;
    for (i = 0; i < EAR_WINMM_BUFFERS; ++i) {
        WAVEHDR *h = &out->headers[i];
        if ((h->dwFlags & (WHDR_DONE | WHDR_PREPARED)) ==
            (WHDR_DONE | WHDR_PREPARED)) {
            ear_render(out->engine, out->samples[i], EAR_WINMM_FRAMES);
            /* Device latency is about 4 x 512 / 22050 = 93 ms buffered. */
            (void)waveOutWrite(out->device, h, sizeof(*h));
        }
    }
}

void ear_winmm_close(EarWinMM *out) {
    unsigned i;
    if (!out || !out->device) return;
    waveOutReset(out->device);
    for (i = 0; i < EAR_WINMM_BUFFERS; ++i) {
        if (out->headers[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(out->device, &out->headers[i], sizeof(WAVEHDR));
    }
    waveOutClose(out->device);
    out->device = NULL;
    out->ready = 0;
}
#endif
