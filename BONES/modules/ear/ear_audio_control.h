#ifndef P144_EAR_AUDIO_CONTROL_H
#define P144_EAR_AUDIO_CONTROL_H
/* Ear 0.4.0: no Skin, Bones, Win32 or UI dependency in this interface.
   All calls occur on the single game/application thread. NOT thread-safe. */
#include <stdint.h>
#define EAR_AUDIO_CONTROL_API 1u

typedef enum EarAudioError {
    EAR_AUDIO_ERROR_NONE=0,
    EAR_AUDIO_ERROR_UNSUPPORTED=1,
    EAR_AUDIO_ERROR_LIBRARY=2,
    EAR_AUDIO_ERROR_SYMBOL=3,
    EAR_AUDIO_ERROR_OPEN=4,
    EAR_AUDIO_ERROR_PREPARE=5,
    EAR_AUDIO_ERROR_WRITE=6,
    EAR_AUDIO_ERROR_PAUSE=7,
    EAR_AUDIO_ERROR_RESUME=8,
    EAR_AUDIO_ERROR_RESET=9,
    EAR_AUDIO_ERROR_UNPREPARE=10,
    EAR_AUDIO_ERROR_CLOSE=11
} EarAudioError;

typedef struct EarAudioStatus {
    uint32_t size;                /* caller sets sizeof(EarAudioStatus) */
    uint32_t api_version;
    uint32_t module_running;
    uint32_t device_ready;        /* waveOutOpen and all primed buffers accepted */
    uint32_t paused;              /* transport pause; affects music AND SFX */
    uint32_t music_enabled;       /* affects music only; SFX remain enabled */
    float music_volume;           /* normalized [0,1] */
    float sfx_volume;             /* normalized [0,1] */
    uint32_t error;               /* EarAudioError, recoverable */
    uint32_t native_error;        /* MMRESULT or Win32 GetLastError */
    uint64_t frames_generated;    /* samples submitted to output, not audible proof */
    uint64_t buffers_submitted;
    uint64_t buffers_completed;
    uint32_t events_played;       /* accepted JUMP/LAND */
    uint32_t events_duplicate;
    uint32_t events_invalid;
    uint32_t open_attempts;
} EarAudioStatus;

int ear_audio_get_status(EarAudioStatus *out);
/* A constant, human-readable explanation for a recoverable diagnostic code. */
const char *ear_audio_error_string(uint32_t error);
int ear_audio_enable_music(int enable);
int ear_audio_set_music_volume(float normalized);
int ear_audio_set_sfx_volume(float normalized);
/* Pause/resume do NOT close/reopen the device. Return 1 on success,
   0 when device absent or WinMM operation fails; setting remains remembered. */
int ear_audio_pause(void);
int ear_audio_resume(void);
/* Explicit recovery only. No retry loop in each frame/tick. */
int ear_audio_retry_output(void);
/* Pump completed buffers; safe for app main loop as well as Bones after_tick. */
void ear_audio_pump(void);
#endif
