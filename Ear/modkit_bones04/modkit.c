/* Ear 0.4.0 for BONES ModKit ABI 1. DO NOT compile included auxiliaries twice.
   Bones v0.4 build discovers only modules/ear/modkit.c. */
#include "../../include/bones/modkit.h"
#include <stdint.h>
#include <string.h>
#include <float.h>
#include "ear_audio_control.h"
#include "ear_core/ear.h"
#include "ear_core/ear_surface.h"
#include "ear_core/ear_music.c"
#include "ear_core/ear_synth.c"
#include "ear_core/ear_surface.c"
#include "ear_output.c"

#define EAR_MODKIT_VERSION 4u
#define EAR_EVENT_HISTORY 16u

typedef struct {
    uint64_t actor_id, tick;
    uint32_t type;
} EarEventKey;
typedef struct {
    EarScore score;
    EarEngine synth;
    EarOutput output;
    const BonesModServices *services; /* borrowed while started */
    EarEventKey recent[EAR_EVENT_HISTORY];
    uint32_t history_count,history_next;
    uint32_t events_played,events_duplicate,events_invalid,received_events;
    uint32_t running;
} EarModState;
static EarModState ear_state;
typedef struct {uint32_t enabled, paused;float music,sfx;} EarPrefs;
static EarPrefs ear_prefs={1u,0u,.30f,.37f};

static int ear_valid_volume(float volume){
    return volume==volume && volume>=0.0f && volume<=1.0f && volume<=FLT_MAX;
}
static void ear_apply_settings(void){
    ear_set_volumes(&ear_state.synth,ear_prefs.music,ear_prefs.sfx);
    ear_music_enable(&ear_state.synth,ear_prefs.enabled);
}
const char *ear_audio_error_string(uint32_t error){
    static const char *names[]={
        "none", "unsupported platform", "winmm.dll unavailable",
        "WinMM symbol missing", "waveOutOpen failed / no output device",
        "waveOutPrepareHeader failed", "waveOutWrite failed",
        "waveOutPause failed", "waveOutRestart failed",
        "waveOutReset failed", "waveOutUnprepareHeader failed",
        "waveOutClose failed"
    };
    return error<sizeof(names)/sizeof(names[0])?names[error]:"unknown audio error";
}
int ear_audio_get_status(EarAudioStatus *out){
    const EarOutput *o=&ear_state.output;
    if(!out || out->size!=sizeof(*out))return 0;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);
    out->api_version=EAR_AUDIO_CONTROL_API;
    out->module_running=ear_state.running;
    out->device_ready=o->ready;
    out->paused=o->ready?o->paused:ear_prefs.paused;
    out->music_enabled=ear_prefs.enabled;
    out->music_volume=ear_prefs.music;
    out->sfx_volume=ear_prefs.sfx;
    out->error=o->error;
    out->native_error=o->native_error;
    out->frames_generated=o->frames_generated;
    out->buffers_submitted=o->buffers_submitted;
    out->buffers_completed=o->buffers_completed;
    out->events_played=ear_state.events_played;
    out->events_duplicate=ear_state.events_duplicate;
    out->events_invalid=ear_state.events_invalid;
    out->open_attempts=o->open_attempts;
    return 1;
}
int ear_audio_enable_music(int enable){
    if(enable!=0 && enable!=1)return 0;
    ear_prefs.enabled=(uint32_t)enable;
    if(ear_state.running)ear_music_enable(&ear_state.synth,enable);
    return 1;
}
int ear_audio_set_music_volume(float v){
    if(!ear_valid_volume(v))return 0;
    ear_prefs.music=v;
    if(ear_state.running)ear_set_volumes(&ear_state.synth,v,ear_prefs.sfx);
    return 1;
}
int ear_audio_set_sfx_volume(float v){
    if(!ear_valid_volume(v))return 0;
    ear_prefs.sfx=v;
    if(ear_state.running)ear_set_volumes(&ear_state.synth,ear_prefs.music,v);
    return 1;
}
int ear_audio_pause(void){
    if(!ear_state.running){ear_prefs.paused=1;return 1;}
    if(!ear_state.output.ready){ear_prefs.paused=1;return 0;}
    if(!ear_output_pause(&ear_state.output))return 0;
    ear_prefs.paused=1;return 1;
}
int ear_audio_resume(void){
    if(!ear_state.running){ear_prefs.paused=0;return 1;}
    if(!ear_state.output.ready){ear_prefs.paused=0;return 0;}
    if(!ear_output_resume(&ear_state.output))return 0;
    ear_prefs.paused=0;return 1;
}
int ear_audio_retry_output(void){
    if(!ear_state.running)return 0;
    if(ear_state.output.ready)return 1;
    if(!ear_output_open(&ear_state.output,&ear_state.synth))return 0;
    if(ear_prefs.paused && !ear_output_pause(&ear_state.output)){
        uint32_t native=ear_state.output.native_error;
        ear_output_close(&ear_state.output);
        ear_state.output.error=EAR_AUDIO_ERROR_PAUSE;
        ear_state.output.native_error=native;
        return 0;
    }
    return 1;
}
void ear_audio_pump(void){
    if(ear_state.running && !ear_state.output.paused)
        ear_output_pump(&ear_state.output,&ear_state.synth);
}

static uint32_t ear_event_seed(const BonesModEvent *e){
    uint32_t x=(uint32_t)e->tick^(uint32_t)e->source_id^
        (uint32_t)(e->source_id>>32)^e->type*UINT32_C(0x9e3779b9);
    x^=x>>16;x*=UINT32_C(0x7feb352d);x^=x>>15;
    x*=UINT32_C(0x846ca68b);x^=x>>16;
    return x;
}
static int32_t ear_mod_start(void *user,const BonesModServices *svc){
    EarModState *s=(EarModState*)user;
    EarMusicConfig cfg={UINT32_C(0x01442026),0,0,EAR_STYLE_DUNGEON};
    if(!s||!svc||svc->size!=sizeof(*svc)||svc->abi!=BONES_MOD_ABI||
       (svc->permissions&(BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME))!=
          (BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME)||
       !svc->actor_get||!svc->tick)return BONES_MOD_EINVAL;
    memset(s,0,sizeof(*s));
    if(!ear_compose(&s->score,cfg))return BONES_MOD_EINVAL;
    ear_init(&s->synth,&s->score);
    ear_apply_settings();
    s->services=svc;
    s->running=1;
    /* Fail-open: user sees/surfaces diagnostics but gameplay continues. */
    (void)ear_audio_retry_output();
    return BONES_MOD_OK;
}
static void ear_mod_stop(void *user){
    EarModState *s=(EarModState*)user;
    if(!s)return;
    ear_output_close(&s->output);
    s->services=NULL;s->running=0;
}
static int ear_seen(const EarModState *s,const BonesModEvent *ev){
    uint32_t i;
    for(i=0;i<s->history_count;i++){
        const EarEventKey *k=&s->recent[i];
        if(k->actor_id==ev->source_id && k->tick==ev->tick && k->type==ev->type)
            return 1;
    }
    return 0;
}
static void ear_remember(EarModState *s,const BonesModEvent *ev){
    EarEventKey *key=&s->recent[s->history_next];
    key->actor_id=ev->source_id;key->tick=ev->tick;key->type=ev->type;
    s->history_next=(s->history_next+1)%EAR_EVENT_HISTORY;
    if(s->history_count<EAR_EVENT_HISTORY)s->history_count++;
}
static void ear_mod_event(void *user,const BonesModEvent *ev){
    EarModState *s=(EarModState*)user;BonesModActor actor;EarSfxRecipe r;uint32_t random;
    if(!s||!s->running||!ev)return;
    if(ev->size!=sizeof(*ev)){s->events_invalid++;return;}
    if(ev->type!=BONES_EVT_JUMP && ev->type!=BONES_EVT_LAND)return;
    s->received_events++;
    if(ear_seen(s,ev)){s->events_duplicate++;return;}
    memset(&actor,0,sizeof(actor));actor.size=sizeof(actor);
    if(!ev->source_id || s->services->actor_get(s->services->host_context,
       ev->source_id,&actor)!=BONES_MOD_OK || actor.size!=sizeof(actor)){
        s->events_invalid++;return;
    }
    random=ear_event_seed(ev);
    r.patch=ev->type==BONES_EVT_LAND?EAR_PATCH_DRUM:EAR_PATCH_NOISE;
    r.volume=ev->type==BONES_EVT_LAND?88:35;
    r.duration_ms=ev->type==BONES_EVT_LAND?200:100;
    r.freq_start=(ev->type==BONES_EVT_LAND?160.0f:580.0f)+(float)(random%29u);
    r.freq_end=ev->type==BONES_EVT_LAND?58.0f:180.0f;
    r.seed=random;
    if(ear_trigger_recipe(&s->synth,r)){
        ear_remember(s,ev);
        s->events_played++;
    }
}
static void ear_mod_after(void *user){(void)user;ear_audio_pump();}
int32_t bones_mod_ear_query(uint32_t abi,BonesModDescriptor *out){
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));out->size=sizeof(*out);out->abi=BONES_MOD_ABI;
    memcpy(out->id,"ear",sizeof("ear"));out->role=BONES_ROLE_EAR;
    out->implementation_version=EAR_MODKIT_VERSION;out->state_schema_version=0;
    out->required_roles=0;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    out->user_context=&ear_state;out->start=ear_mod_start;out->stop=ear_mod_stop;
    out->after_tick=ear_mod_after;out->on_event=ear_mod_event;
    return BONES_MOD_OK;
}
#ifdef EAR_MODKIT_DLL
BONES_MOD_EXPORT int32_t BONES_MOD_CALL bones_mod_query_v1(uint32_t abi,BonesModDescriptor *out){
    return bones_mod_ear_query(abi,out);
}
#endif
#ifdef EAR_MODKIT_TEST
int ear_mod_test_render(int16_t *pcm,size_t frames){
    if(!ear_state.running||!pcm||frames>16384u)return 0;
    ear_render(&ear_state.synth,pcm,frames);return 1;
}
/* Keep v0.3 test hooks compatible with the previous test harness. */
uint32_t ear_mod_test_played(void){return ear_state.events_played;}
uint32_t ear_mod_test_invalid(void){return ear_state.events_invalid;}
uint32_t ear_mod_test_receives(void){return ear_state.received_events;}
#endif
