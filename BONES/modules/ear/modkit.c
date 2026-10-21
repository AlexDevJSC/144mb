/* Ear for Bones v0.4.0: ModKit ABI 1, source-linked static module.
   The Bones Windows scripts compile only each module's modkit.c; therefore
   private C implementation files are deliberately included here as one TU.
   Do not compile the included .c files separately in the same target. */
#include "../../include/bones/modkit.h"
#include <stdint.h>
#include <string.h>
#include "ear_core/ear.h"
#include "ear_core/ear_surface.h"
#include "ear_core/ear_music.c"
#include "ear_core/ear_synth.c"
#include "ear_core/ear_surface.c"
#include "ear_output.c"

#define EAR_MODKIT_VERSION 3u

typedef struct {
    EarScore score;
    EarEngine synth;
    EarOutput output;
    const BonesModServices *services; /* borrowed until stop */
    uint32_t received_events;
    uint32_t played_events;
    uint32_t invalid_events;
    int running;
} EarModState;
static EarModState ear_state;

static uint32_t ear_event_seed(const BonesModEvent *e) {
    uint32_t x=(uint32_t)e->tick ^ (uint32_t)e->source_id ^
             (uint32_t)(e->source_id>>32) ^ e->type*UINT32_C(0x9e3779b9);
    x^=x>>16;x*=UINT32_C(0x7feb352d);x^=x>>15;
    x*=UINT32_C(0x846ca68b);x^=x>>16;
    return x;
}
static int32_t ear_mod_start(void *user,const BonesModServices *svc) {
    EarModState *s=(EarModState *)user;
    EarMusicConfig cfg={UINT32_C(0x01442026),0,0,EAR_STYLE_DUNGEON};
    if(!s||!svc||svc->size!=sizeof(*svc)||svc->abi!=BONES_MOD_ABI||
       (svc->permissions & (BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME))!=
       (BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME)||
       !svc->actor_get||!svc->tick)return BONES_MOD_EINVAL;
    memset(s,0,sizeof(*s));
    if(!ear_compose(&s->score,cfg))return BONES_MOD_EINVAL;
    ear_init(&s->synth,&s->score);
    s->services=svc;
    /* Audio device is optional: headless runs never fail merely for no sound. */
    (void)ear_output_open(&s->output,&s->synth);
    s->running=1;
    return BONES_MOD_OK;
}
static void ear_mod_stop(void *user) {
    EarModState *s=(EarModState *)user;
    if(!s)return;
    ear_output_close(&s->output);
    s->services=NULL;
    s->running=0;
}
static void ear_mod_event(void *user,const BonesModEvent *ev) {
    EarModState *s=(EarModState *)user;
    BonesModActor actor;
    EarSfxRecipe r;
    uint32_t random;
    if(!s||!s->running||!ev)return;
    if(ev->size!=sizeof(*ev)){s->invalid_events++;return;}
    if(ev->type!=BONES_EVT_JUMP&&ev->type!=BONES_EVT_LAND)return;
    s->received_events++;
    /* ModKit ABI1 contains no surface or intensity field: generic SFX only.
       Never infer acoustic material from Eye rendering or target_id. */
    memset(&actor,0,sizeof(actor));actor.size=(uint32_t)sizeof(actor);
    if(!ev->source_id||s->services->actor_get(s->services->host_context,
         ev->source_id,&actor)!=BONES_MOD_OK||actor.size!=sizeof(actor)){
        s->invalid_events++;return;
    }
    random=ear_event_seed(ev);
    r.patch=ev->type==BONES_EVT_LAND?EAR_PATCH_DRUM:EAR_PATCH_NOISE;
    r.volume=ev->type==BONES_EVT_LAND?88:35;
    r.duration_ms=ev->type==BONES_EVT_LAND?200:100;
    r.freq_start=(ev->type==BONES_EVT_LAND?160.0f:580.0f)+(float)(random%29u);
    r.freq_end=ev->type==BONES_EVT_LAND?58.0f:180.0f;
    r.seed=random;
    if(ear_trigger_recipe(&s->synth,r))s->played_events++;
}
static void ear_mod_after(void *user) {
    EarModState *s=(EarModState *)user;
    if(s&&s->running)ear_output_pump(&s->output,&s->synth);
}
int32_t bones_mod_ear_query(uint32_t abi,BonesModDescriptor *out) {
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));
    out->size=(uint32_t)sizeof(*out);
    out->abi=BONES_MOD_ABI;
    memcpy(out->id,"ear",sizeof("ear"));
    out->role=BONES_ROLE_EAR;
    out->implementation_version=EAR_MODKIT_VERSION;
    out->state_schema_version=0;
    out->required_roles=0;
    out->requested_permissions=BONES_PERMISSION_READ_ACTORS|BONES_PERMISSION_READ_TIME;
    out->user_context=&ear_state;
    out->start=ear_mod_start;
    out->stop=ear_mod_stop;
    out->after_tick=ear_mod_after;
    out->on_event=ear_mod_event;
    return BONES_MOD_OK;
}
#ifdef EAR_MODKIT_DLL
/* DLL/dev only; do not export this name from every static specialist. */
BONES_MOD_EXPORT int32_t BONES_MOD_CALL bones_mod_query_v1(
        uint32_t abi,BonesModDescriptor *out) {
    return bones_mod_ear_query(abi,out);
}
#endif
#ifdef EAR_MODKIT_TEST
/* Test-only functions, omitted in the compact release. */
int ear_mod_test_render(int16_t *pcm,size_t frames) {
    if(!ear_state.running||!pcm||frames>16384u)return 0;
    ear_render(&ear_state.synth,pcm,frames);return 1;
}
uint32_t ear_mod_test_played(void){return ear_state.played_events;}
uint32_t ear_mod_test_invalid(void){return ear_state.invalid_events;}
uint32_t ear_mod_test_receives(void){return ear_state.received_events;}
#endif
