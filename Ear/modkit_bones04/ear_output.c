/* Optional Windows PCM backend using only the OS-provided winmm.dll.
   No winmm.lib/-lwinmm flag needed in the Bones 0.4 release scripts. */
#ifndef EAR_BONES04_OUTPUT_C
#define EAR_BONES04_OUTPUT_C
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#define EAR_OUTPUT_BUFFERS 4u
#define EAR_OUTPUT_FRAMES 512u
typedef struct {
    HMODULE lib;
    HWAVEOUT device;
    MMRESULT (WINAPI *open)(LPHWAVEOUT,UINT,LPCWAVEFORMATEX,DWORD_PTR,DWORD_PTR,DWORD);
    MMRESULT (WINAPI *close)(HWAVEOUT);
    MMRESULT (WINAPI *reset)(HWAVEOUT);
    MMRESULT (WINAPI *prepare)(HWAVEOUT,LPWAVEHDR,UINT);
    MMRESULT (WINAPI *unprepare)(HWAVEOUT,LPWAVEHDR,UINT);
    MMRESULT (WINAPI *write)(HWAVEOUT,LPWAVEHDR,UINT);
    WAVEHDR header[EAR_OUTPUT_BUFFERS];
    int16_t pcm[EAR_OUTPUT_BUFFERS][EAR_OUTPUT_FRAMES];
    int ready;
} EarOutput;
static void ear_output_close(EarOutput *o) {
    unsigned i;
    if(!o)return;
    if(o->device) {
        if(o->reset)(void)o->reset(o->device);
        for(i=0;i<EAR_OUTPUT_BUFFERS;++i)
            if(o->header[i].dwFlags&WHDR_PREPARED)
                (void)o->unprepare(o->device,&o->header[i],(UINT)sizeof(WAVEHDR));
        if(o->close)(void)o->close(o->device);
    }
    if(o->lib)FreeLibrary(o->lib);
    memset(o,0,sizeof(*o));
}
static int ear_output_open(EarOutput *o,EarEngine *engine) {
    WAVEFORMATEX fmt;
    unsigned i;
    if(!o||!engine)return 0;
    memset(o,0,sizeof(*o));
    o->lib=LoadLibraryA("winmm.dll");
    if(!o->lib)return 0;
#define LOAD_FN(member,symbol) do { \
    FARPROC proc=GetProcAddress(o->lib,"waveOut" symbol); \
    if(!proc){ear_output_close(o);return 0;} \
    memcpy(&o->member,&proc,sizeof(proc)); \
} while(0)
    LOAD_FN(open,"Open");
    LOAD_FN(close,"Close");
    LOAD_FN(reset,"Reset");
    LOAD_FN(prepare,"PrepareHeader");
    LOAD_FN(unprepare,"UnprepareHeader");
    LOAD_FN(write,"Write");
#undef LOAD_FN
    memset(&fmt,0,sizeof(fmt));
    fmt.wFormatTag=WAVE_FORMAT_PCM;
    fmt.nChannels=1;
    fmt.nSamplesPerSec=EAR_SAMPLE_RATE;
    fmt.wBitsPerSample=16;
    fmt.nBlockAlign=2;
    fmt.nAvgBytesPerSec=EAR_SAMPLE_RATE*2u;
    if(o->open(&o->device,WAVE_MAPPER,&fmt,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR) {
        ear_output_close(o);return 0;
    }
    for(i=0;i<EAR_OUTPUT_BUFFERS;++i) {
        WAVEHDR *h=&o->header[i];
        h->lpData=(LPSTR)o->pcm[i];
        h->dwBufferLength=(DWORD)sizeof(o->pcm[i]);
        ear_render(engine,o->pcm[i],EAR_OUTPUT_FRAMES);
        if(o->prepare(o->device,h,(UINT)sizeof(*h))!=MMSYSERR_NOERROR||
           o->write(o->device,h,(UINT)sizeof(*h))!=MMSYSERR_NOERROR) {
            ear_output_close(o);return 0;
        }
    }
    o->ready=1;return 1;
}
static void ear_output_pump(EarOutput *o,EarEngine *engine) {
    unsigned i;
    if(!o||!o->ready)return;
    for(i=0;i<EAR_OUTPUT_BUFFERS;++i) {
        WAVEHDR *h=&o->header[i];
        if((h->dwFlags&(WHDR_PREPARED|WHDR_DONE))==(WHDR_PREPARED|WHDR_DONE)) {
            ear_render(engine,o->pcm[i],EAR_OUTPUT_FRAMES);
            if(o->write(o->device,h,(UINT)sizeof(*h))!=MMSYSERR_NOERROR) {
                ear_output_close(o);return;
            }
        }
    }
}
#else
typedef struct {int ready;} EarOutput;
static int ear_output_open(EarOutput *o,EarEngine *engine){(void)o;(void)engine;return 0;}
static void ear_output_pump(EarOutput *o,EarEngine *engine){(void)o;(void)engine;}
static void ear_output_close(EarOutput *o){if(o)o->ready=0;}
#endif
#endif
