/* Manual Windows hardware acceptance test for Ear 0.4 / Bones ModKit ABI 1.
   Hardware + hearing are required to certify audible playback.
   --allow-no-device is CI-only and never certifies an audio device.
   --retry-device exercises explicit recovery: disconnect before launch, then
   reconnect when prompted. --listen-confirm requests a human audible check.
   The game itself never uses this tool or its console I/O. */
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
static void emit_event(BonesModDescriptor *d,uint32_t kind,uint64_t tick){
    BonesModEvent e={0};e.size=sizeof(e);e.source_id=11;
    e.type=kind;e.tick=tick;d->on_event(d->user_context,&e);
}
static int read_status(const char *stage,EarAudioStatus *out){
    EarAudioStatus a={0};a.size=sizeof(a);
    if(!ear_audio_get_status(&a)){
        fprintf(stderr,"FAIL %s: ear_audio_get_status\n",stage);return 0;
    }
    printf("%s: running=%u device=%u paused=%u music=%u musicVol=%.2f "
           "sfxVol=%.2f error=%u (%s) native=%u attempts=%u frames=%llu "
           "submitted=%llu completed=%llu sfx=%u duplicate=%u\n",
           stage,a.module_running,a.device_ready,a.paused,a.music_enabled,
           (double)a.music_volume,(double)a.sfx_volume,
           a.error,ear_audio_error_string(a.error),a.native_error,a.open_attempts,
           (unsigned long long)a.frames_generated,
           (unsigned long long)a.buffers_submitted,
           (unsigned long long)a.buffers_completed,a.events_played,a.events_duplicate);
    if(out)*out=a;
    return 1;
}
static int stop_checked(BonesModDescriptor *d){
    EarAudioStatus a;
    d->stop(d->user_context);
    if(!read_status("closed",&a))return 0;
    if(a.module_running || a.device_ready || a.paused ||
       a.error!=EAR_AUDIO_ERROR_NONE) {
        fprintf(stderr,"FAIL: device close/reset/unprepare or module stop (error=%u native=%u)\n",
                a.error,a.native_error);
        return 0;
    }
    return 1;
}
static int has_flag(int argc,char **argv,const char *flag){
    int i;for(i=1;i<argc;i++)if(strcmp(argv[i],flag)==0)return 1;
    return 0;
}
static int confirm_hearing(void){
    char answer[32];
    puts("CONFIRMACION HUMANA: ¿Has oido musica de mazmorra, salto y aterrizaje?");
    puts("Responde SI (s) solo si los TRES son audibles y la pausa/volumen se perciben.");
    fputs("Respuesta s/n: ",stdout);fflush(stdout);
    if(!fgets(answer,sizeof(answer),stdin))return 0;
    return answer[0]=='s'||answer[0]=='S';
}
int main(int argc,char **argv){
    BonesModDescriptor d={0};BonesModServices svc={0};
    EarAudioStatus a={0},p={0};
    uint32_t attempts_before=0;
    unsigned i;
    const int allow_absent=has_flag(argc,argv,"--allow-no-device");
    const int retry_device=has_flag(argc,argv,"--retry-device");
    const int listen=has_flag(argc,argv,"--listen-confirm");
    int manual_good=1,ok=1;
    d.size=sizeof(d);svc.size=sizeof(svc);svc.abi=BONES_MOD_ABI;
    svc.permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    svc.actor_get=test_actor;svc.tick=test_tick;
    if(bones_mod_ear_query(BONES_MOD_ABI,&d)!=BONES_MOD_OK ||
       !d.start||!d.stop||!d.on_event||!d.after_tick ||
       d.start(d.user_context,&svc)!=BONES_MOD_OK){
        puts("FAIL: ModKit ABI/startup");return 5;
    }
    if(!read_status("open",&a)){d.stop(d.user_context);return 5;}
    if(retry_device && a.device_ready){
        puts("FAIL: --retry-device requires output DISCONNECTED before startup.");
        d.stop(d.user_context);return 6;
    }
    if(!a.device_ready && retry_device){
        puts("RECOVERY: WinMM failed as expected. Connect/enable the output now.");
        puts("Press ENTER after Windows recognizes the output device.");
        (void)getchar();
        if(!ear_audio_retry_output()){
            (void)read_status("retry failed",&a);
            d.stop(d.user_context);return 2;
        }
        (void)read_status("retry successful",&a);
        if(a.open_attempts<2u)ok=0;
    }
    if(!a.device_ready){
        puts("NO DEVICE: documented recoverable output failure; NO audible claim.");
        if(a.error==EAR_AUDIO_ERROR_NONE || a.open_attempts==0u)ok=0;
        d.stop(d.user_context);
        return allow_absent && ok?0:2;
    }
    if(a.error || a.buffers_submitted!=4u || a.frames_generated!=4u*512u){
        puts("FAIL: device did not prime four initial buffers");
        d.stop(d.user_context);return 3;
    }
    attempts_before=a.open_attempts;
    puts("ESCUCHA: 5 segundos musica DUNGEON; salto ~1s, aterrizaje ~1.6s.");
    puts("Volumen musical cambia a ~1.8s; pausa entre 2.2 y 2.9 segundos.");
    puts("El volumen SFX cambia por separado y se alterna music on/off.");
    for(i=0;i<250u;i++){
        if(i==50u){emit_event(&d,BONES_EVT_JUMP,101);emit_event(&d,BONES_EVT_JUMP,101);}
        if(i==80u){emit_event(&d,BONES_EVT_LAND,102);emit_event(&d,BONES_EVT_LAND,102);}
        if(i==90u){
            if(!ear_audio_set_music_volume(.10f))ok=0;
            if(!read_status("music 10%",&a)||a.music_volume!=.10f)ok=0;
        }
        if(i==110u){
            if(!ear_audio_pause())ok=0;
            if(!read_status("paused",&p)||!p.paused)ok=0;
        }
        if(i==120u){
            if(!ear_audio_set_music_volume(.55f) ||
               !ear_audio_set_sfx_volume(.65f))ok=0;
            if(!read_status("separate volumes",&a) ||
                a.music_volume!=.55f||a.sfx_volume!=.65f)ok=0;
        }
        if(i==140u){
            if(!read_status("pause held",&a) || !a.paused ||
               a.buffers_submitted!=p.buffers_submitted)ok=0;
        }
        if(i==145u){
            if(!ear_audio_resume())ok=0;
            if(!read_status("resumed",&a)||a.paused)ok=0;
        }
        if(i==175u){
            if(!ear_audio_enable_music(0))ok=0;
            if(!read_status("music disabled",&a)||a.music_enabled)ok=0;
        }
        if(i==195u){
            if(!ear_audio_enable_music(1))ok=0;
            if(!read_status("music enabled",&a)||!a.music_enabled)ok=0;
        }
        /* Second half deliberately pumps with NO Bones tick callback:
           validates Win32-host pumping while game simulation is paused. */
        if(i<200u)d.after_tick(d.user_context);
        else ear_audio_pump();
        Sleep(20);
    }
    if(!read_status("after 5 seconds",&a))ok=0;
    if(a.error || !a.device_ready || a.buffers_completed==0u ||
       a.buffers_submitted<=4u || a.frames_generated<=4u*512u ||
       a.events_played!=2u || a.events_duplicate!=2u ||
       a.open_attempts!=attempts_before)ok=0;
    if(!stop_checked(&d))ok=0;
    if(!ok){
        puts("FAIL: hardware state, buffer progress, controls, SFX, or cleanup.");
        return 3;
    }
    puts("PASS: Windows WinMM open/stream/pause/resume/volume/close and SFX counters.");
    puts("AVISO: Sin confirmacion humana aun NO queda probada la audibilidad.");
    if(listen){
        manual_good=confirm_hearing();
        if(!manual_good){
            puts("FAIL: La audibilidad no esta confirmada.");
            return 7;
        }
        puts("PASS AUDIBLE: confirmado manualmente por el probador presente.");
    }
    return 0;
}
