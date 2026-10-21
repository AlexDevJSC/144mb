/* Windows-only manual audio playtest: real WinMM device, not a simulated sink.
   Ear and Skin may call ear_audio_get_status; no controls/menus are implemented. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../BONES/include/bones/modkit.h"
#include "../../BONES/modules/ear/ear_audio_control.h"
extern int32_t bones_mod_ear_query(uint32_t, BonesModDescriptor*);
static uint64_t test_tick(void *user){(void)user;return 100;}
static int32_t test_actor(void *user,uint64_t id,BonesModActor *out){
    (void)user;
    if(!out||out->size!=sizeof(*out)||id!=11)return BONES_MOD_MISSING;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->id=11;
    return BONES_MOD_OK;
}
static void event(BonesModDescriptor *d,uint32_t kind,uint64_t tick){
    BonesModEvent e={0};e.size=sizeof(e);e.source_id=11;
    e.type=kind;e.tick=tick;
    d->on_event(d->user_context,&e);
}
static void status(const char *when){
    EarAudioStatus a={0};a.size=sizeof(a);
    if(!ear_audio_get_status(&a)){puts("ERROR: invalid status");return;}
    printf("%s: running=%u device=%u paused=%u music=%u volume=%.2f sfx=%.2f "
           "error=%u (%s) native=%u attempts=%u frames=%llu "
           "submitted=%llu completed=%llu sfx=%u duplicate=%u\n",
       when,a.module_running,a.device_ready,a.paused,a.music_enabled,
       (double)a.music_volume,(double)a.sfx_volume,
       a.error,ear_audio_error_string(a.error),a.native_error,a.open_attempts,
       (unsigned long long)a.frames_generated,
       (unsigned long long)a.buffers_submitted,
       (unsigned long long)a.buffers_completed,a.events_played,a.events_duplicate);
}
int main(int argc,char **argv){
    BonesModDescriptor d={0};BonesModServices svc={0};
    EarAudioStatus a={0};unsigned i;int require_device=argc>1 && strcmp(argv[1],"--require-device")==0;
    d.size=sizeof(d);svc.size=sizeof(svc);svc.abi=BONES_MOD_ABI;
    svc.permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    svc.actor_get=test_actor;svc.tick=test_tick;
    if(bones_mod_ear_query(BONES_MOD_ABI,&d)!=BONES_MOD_OK ||
       d.start(d.user_context,&svc)!=BONES_MOD_OK){puts("FAIL: ModKit startup");return 5;}
    status("open");a.size=sizeof(a);ear_audio_get_status(&a);
    if(!a.device_ready){
        puts("WARNING: No valid Windows audio output. No claim of audible playback.");
        puts("Recoverable: connect a device and use ear_audio_retry_output().");
        d.stop(d.user_context);return require_device?2:0;
    }
    puts("Escucha: musica procedural de mazmorra; en ~1 s sonara un salto y luego aterrizaje.");
    for(i=0;i<250;i++){
        if(i==50){event(&d,BONES_EVT_JUMP,101);event(&d,BONES_EVT_JUMP,101);}
        if(i==80){event(&d,BONES_EVT_LAND,102);event(&d,BONES_EVT_LAND,102);}
        if(i==110){if(!ear_audio_pause())puts("FAIL: pause WinMM");}
        if(i==145){if(!ear_audio_resume())puts("FAIL: resume WinMM");}
        d.after_tick(d.user_context);
        Sleep(20);
    }
    status("after 5 seconds");ear_audio_get_status(&a);
    if(a.error || !a.device_ready || a.buffers_completed==0 ||
       a.frames_generated<=4*512 || a.events_played!=2 || a.events_duplicate!=2){
        puts("FAIL: output runtime/buffers/events; check diagnostic above");
        d.stop(d.user_context);return 3;
    }
    d.stop(d.user_context);status("closed");
    puts("PASS: WinMM accepted streamed PCM; listen to confirm audibility manually.");
    return 0;
}
