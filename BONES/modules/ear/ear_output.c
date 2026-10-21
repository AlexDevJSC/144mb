/* Private, synchronous streaming adapter. Do not compile separately:
   modkit.c includes this C file as part of Bones' single source module. */
#ifndef EAR_OUTPUT_PRIVATE_C
#define EAR_OUTPUT_PRIVATE_C
#include <string.h>
#include <stdint.h>

#define EAR_OUTPUT_BUFFERS 4u
#define EAR_OUTPUT_FRAMES 512u

#if defined(EAR_FAKE_OUTPUT)
/* Deterministic test-only fake, not a real audio device. */
typedef struct {
    uint32_t ready, paused, error, native_error;
    uint64_t frames_generated,buffers_submitted,buffers_completed;
    uint32_t open_attempts;
} EarOutput;
static void ear_output_close(EarOutput *o) {if(o)o->ready=0;}
static int ear_output_open(EarOutput *o,EarEngine *engine) {
    int16_t pcm[EAR_OUTPUT_FRAMES];unsigned i;
    if(!o||!engine)return 0;
    o->open_attempts++;
#ifdef EAR_FAKE_NO_DEVICE
    o->ready=0;o->error=EAR_AUDIO_ERROR_OPEN;o->native_error=2;return 0;
#endif
    o->error=0;o->native_error=0;o->ready=1;
    for(i=0;i<EAR_OUTPUT_BUFFERS;i++) {
        ear_render(engine,pcm,EAR_OUTPUT_FRAMES);
        o->frames_generated+=EAR_OUTPUT_FRAMES;o->buffers_submitted++;
    }
    return 1;
}
static int ear_output_pause(EarOutput *o){if(!o||!o->ready)return 0;o->paused=1;return 1;}
static int ear_output_resume(EarOutput *o){if(!o||!o->ready)return 0;o->paused=0;return 1;}
static void ear_output_pump(EarOutput *o,EarEngine *engine){
    unsigned i;int16_t pcm[EAR_OUTPUT_FRAMES];
    if(!o||!o->ready||o->paused||!engine)return;
    for(i=0;i<2;i++) {
        o->buffers_completed++;
        ear_render(engine,pcm,EAR_OUTPUT_FRAMES);
        o->frames_generated+=EAR_OUTPUT_FRAMES;o->buffers_submitted++;
    }
}
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
typedef struct {
    HMODULE lib;
    HWAVEOUT device;
    MMRESULT (WINAPI *open)(LPHWAVEOUT,UINT,LPCWAVEFORMATEX,DWORD_PTR,DWORD_PTR,DWORD);
    MMRESULT (WINAPI *close)(HWAVEOUT);
    MMRESULT (WINAPI *reset)(HWAVEOUT);
    MMRESULT (WINAPI *prepare)(HWAVEOUT,LPWAVEHDR,UINT);
    MMRESULT (WINAPI *unprepare)(HWAVEOUT,LPWAVEHDR,UINT);
    MMRESULT (WINAPI *write)(HWAVEOUT,LPWAVEHDR,UINT);
    MMRESULT (WINAPI *pause)(HWAVEOUT);
    MMRESULT (WINAPI *restart)(HWAVEOUT);
    WAVEHDR header[EAR_OUTPUT_BUFFERS];
    int16_t pcm[EAR_OUTPUT_BUFFERS][EAR_OUTPUT_FRAMES];
    uint32_t ready, paused, error, native_error;
    uint64_t frames_generated,buffers_submitted,buffers_completed;
    uint32_t open_attempts;
} EarOutput;

static void ear_output_close(EarOutput *o) {
    unsigned i;MMRESULT err;
    if(!o)return;
    if(o->device) {
        if(o->reset) {
            err=o->reset(o->device);
            if(err!=MMSYSERR_NOERROR) {o->error=EAR_AUDIO_ERROR_RESET;o->native_error=err;}
        }
        if(o->unprepare)for(i=0;i<EAR_OUTPUT_BUFFERS;i++) {
            if(o->header[i].dwFlags & WHDR_PREPARED) {
                err=o->unprepare(o->device,&o->header[i],(UINT)sizeof(WAVEHDR));
                if(err!=MMSYSERR_NOERROR){o->error=EAR_AUDIO_ERROR_UNPREPARE;o->native_error=err;}
            }
        }
        if(o->close){
            err=o->close(o->device);
            if(err!=MMSYSERR_NOERROR){o->error=EAR_AUDIO_ERROR_CLOSE;o->native_error=err;}
        }
        o->device=NULL;
    }
    if(o->lib){FreeLibrary(o->lib);o->lib=NULL;}
    o->ready=0;o->paused=0;
}
static int ear_output_failed(EarOutput *o,uint32_t reason,uint32_t native){
    ear_output_close(o);
    o->error=reason;o->native_error=native;
    return 0;
}
static int ear_output_open(EarOutput *o,EarEngine *engine) {
    WAVEFORMATEX fmt;unsigned i;MMRESULT err;
    if(!o||!engine)return 0;
    o->open_attempts++;
    o->error=0;o->native_error=0;
    o->lib=LoadLibraryA("winmm.dll");
    if(!o->lib)return ear_output_failed(o,EAR_AUDIO_ERROR_LIBRARY,GetLastError());
#define LOAD_FN(member, suffix) do { \
    FARPROC proc=GetProcAddress(o->lib,"waveOut" suffix); \
    if(!proc)return ear_output_failed(o,EAR_AUDIO_ERROR_SYMBOL,GetLastError()); \
    memcpy(&o->member,&proc,sizeof(o->member)); \
} while(0)
    LOAD_FN(open,"Open");LOAD_FN(close,"Close");LOAD_FN(reset,"Reset");
    LOAD_FN(prepare,"PrepareHeader");LOAD_FN(unprepare,"UnprepareHeader");
    LOAD_FN(write,"Write");LOAD_FN(pause,"Pause");LOAD_FN(restart,"Restart");
#undef LOAD_FN
    memset(&fmt,0,sizeof(fmt));fmt.wFormatTag=WAVE_FORMAT_PCM;
    fmt.nChannels=1;fmt.nSamplesPerSec=EAR_SAMPLE_RATE;fmt.wBitsPerSample=16;
    fmt.nBlockAlign=2;fmt.nAvgBytesPerSec=EAR_SAMPLE_RATE*2u;
    err=o->open(&o->device,WAVE_MAPPER,&fmt,0,0,CALLBACK_NULL);
    if(err!=MMSYSERR_NOERROR)return ear_output_failed(o,EAR_AUDIO_ERROR_OPEN,err);
    for(i=0;i<EAR_OUTPUT_BUFFERS;i++){
        WAVEHDR *h=&o->header[i];
        memset(h,0,sizeof(*h));h->lpData=(LPSTR)o->pcm[i];
        h->dwBufferLength=(DWORD)sizeof(o->pcm[i]);
        ear_render(engine,o->pcm[i],EAR_OUTPUT_FRAMES);
        o->frames_generated+=EAR_OUTPUT_FRAMES;
        err=o->prepare(o->device,h,(UINT)sizeof(*h));
        if(err!=MMSYSERR_NOERROR)return ear_output_failed(o,EAR_AUDIO_ERROR_PREPARE,err);
        err=o->write(o->device,h,(UINT)sizeof(*h));
        if(err!=MMSYSERR_NOERROR)return ear_output_failed(o,EAR_AUDIO_ERROR_WRITE,err);
        o->buffers_submitted++;
    }
    o->ready=1;return 1;
}
static int ear_output_pause(EarOutput *o) {
    MMRESULT err;
    if(!o||!o->ready)return 0;
    if(o->paused)return 1;
    err=o->pause(o->device);
    if(err!=MMSYSERR_NOERROR){o->error=EAR_AUDIO_ERROR_PAUSE;o->native_error=err;return 0;}
    o->paused=1;return 1;
}
static int ear_output_resume(EarOutput *o) {
    MMRESULT err;
    if(!o||!o->ready)return 0;
    if(!o->paused)return 1;
    err=o->restart(o->device);
    if(err!=MMSYSERR_NOERROR){o->error=EAR_AUDIO_ERROR_RESUME;o->native_error=err;return 0;}
    o->paused=0;return 1;
}
static void ear_output_pump(EarOutput *o,EarEngine *engine) {
    unsigned i;MMRESULT err;
    if(!o||!o->ready||o->paused||!engine)return;
    for(i=0;i<EAR_OUTPUT_BUFFERS;i++) {
        WAVEHDR *h=&o->header[i];
        if((h->dwFlags & (WHDR_DONE|WHDR_PREPARED))==(WHDR_DONE|WHDR_PREPARED)) {
            o->buffers_completed++;
            ear_render(engine,o->pcm[i],EAR_OUTPUT_FRAMES);
            o->frames_generated+=EAR_OUTPUT_FRAMES;
            err=o->write(o->device,h,(UINT)sizeof(*h));
            if(err!=MMSYSERR_NOERROR) {
                /* recoverable failure, never reopen continuously in a tick */
                (void)ear_output_failed(o,EAR_AUDIO_ERROR_WRITE,err);
                return;
            }
            o->buffers_submitted++;
        }
    }
}
#else
/* Headless engine: synthesis works; no output device is falsely reported. */
typedef struct {
    uint32_t ready,paused,error,native_error;
    uint64_t frames_generated,buffers_submitted,buffers_completed;
    uint32_t open_attempts;
} EarOutput;
static void ear_output_close(EarOutput *o){if(o)o->ready=0;}
static int ear_output_open(EarOutput *o,EarEngine *e){
    (void)e;if(!o)return 0;
    o->open_attempts++;o->error=EAR_AUDIO_ERROR_UNSUPPORTED;return 0;
}
static int ear_output_pause(EarOutput *o){(void)o;return 0;}
static int ear_output_resume(EarOutput *o){(void)o;return 0;}
static void ear_output_pump(EarOutput *o,EarEngine *e){(void)o;(void)e;}
#endif
#endif
