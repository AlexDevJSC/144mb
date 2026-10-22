/* Skin provider for Bones ModKit ABI 1 (Bones 0.4.0).
   Unity-compiled via modkit.c to match the existing single-file build scripts.
   The host owns simulation, snapshots, rendering and input dispatch. */
#include "skin_modkit.h"
#include <string.h>

typedef struct {
    const BonesModServices *services; /* borrowed, valid only while started */
    Skin2Ui ui;
    Skin2View view;
    uint64_t last_tick;
    uint32_t actor_count;
    uint8_t started;
} SkinModState;

static SkinModState skin_mod_state;

static int32_t skin_mod_start(void *context, const BonesModServices *services) {
    SkinModState *s=(SkinModState *)context;
    const uint32_t needed=BONES_PERMISSION_READ_TIME|BONES_PERMISSION_READ_ACTORS;
    if (!s||!services||services->size!=sizeof(*services)||
        services->abi!=BONES_MOD_ABI||
        (services->permissions&needed)!=needed||
        !services->tick||!services->actor_count||!services->actor_at) return BONES_MOD_EINVAL;
    memset(s,0,sizeof(*s));
    s->services=services;
    skin2_init(&s->ui);
    s->view.hp_max=0u; /* Bones has no character HP/mana/inventory yet. */
    s->last_tick=services->tick(services->host_context);
    s->view.tick=s->last_tick;
    s->started=1;
    return BONES_MOD_OK;
}
static void skin_mod_stop(void *context) {
    SkinModState *s=(SkinModState *)context;
    if(s)memset(s,0,sizeof(*s));
}
static void skin_mod_after_tick(void *context) {
    SkinModState *s=(SkinModState *)context;
    int32_t actors;
    if(!s||!s->started||!s->services)return;
    s->last_tick=s->services->tick(s->services->host_context);
    actors=s->services->actor_count(s->services->host_context);
    if(actors>=0 && actors <= (int32_t)BONES_MOD_MAX_ACTORS)
        s->actor_count=(uint32_t)actors;
    s->view.tick=s->last_tick;
    s->view.actor_count=s->actor_count;
}
static void skin_mod_on_event(void *context,const BonesModEvent *event) {
    SkinModState *s=(SkinModState *)context;
    if(!s||!s->started||!event||event->size!=sizeof(*event))return;
    if(event->type==BONES_EVT_JUMP)skin2_notice(&s->ui,"SALTO");
    else if(event->type==BONES_EVT_LAND)skin2_notice(&s->ui,"ATERRIZAJE");
}

int32_t bones_mod_skin_query(uint32_t abi,BonesModDescriptor *out) {
    if(abi!=BONES_MOD_ABI||!out||out->size!=sizeof(*out))return BONES_MOD_EINVAL;
    memset(out,0,sizeof(*out));
    out->size=(uint32_t)sizeof(*out);
    out->abi=BONES_MOD_ABI;
    memcpy(out->id,"skin",sizeof("skin"));
    out->role=BONES_ROLE_SKIN;
    out->implementation_version=5u;  /* Skin v0.3; version of implementation */
    out->state_schema_version=0u;    /* UI has no private persistent format */
    out->required_roles=0u;         /* Headless/offline compatible; Eye optional */
    out->requested_permissions=BONES_PERMISSION_READ_TIME|BONES_PERMISSION_READ_ACTORS;
    out->user_context=&skin_mod_state;
    out->start=skin_mod_start;
    out->stop=skin_mod_stop;
    out->after_tick=skin_mod_after_tick;
    out->on_event=skin_mod_on_event;
    /* Neither present nor build_scene: Eye exclusively owns 3D presentation. */
    return BONES_MOD_OK;
}
Skin2Action bones_skin_ui_input(Skin2Key key) {
    if(!skin_mod_state.started||key<SKIN2_UP||key>SKIN2_INTERACT)return SKIN2_NO_ACTION;
    return skin2_input(&skin_mod_state.ui,key);
}
int32_t bones_skin_ui_update(const Skin2View *view) {
    uint32_t i;
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    if(!view||view->item_count>SKIN_UI2_SLOTS)return BONES_MOD_EINVAL;
    /* Copy only. Normalize all untrusted/non-terminated item name buffers. */
    skin_mod_state.view=*view;
    /* Canonical ModKit values cannot be overridden with guesses from UI callers. */
    skin_mod_state.view.tick=skin_mod_state.last_tick;
    skin_mod_state.view.actor_count=skin_mod_state.actor_count;
    skin_mod_state.view.available &= SKIN2_AVAIL_HP|SKIN2_AVAIL_MP|SKIN2_AVAIL_GOLD|SKIN2_AVAIL_INVENTORY;
    for(i=0;i<SKIN_UI2_SLOTS;i++)
        skin_mod_state.view.items[i].name[SKIN_UI2_ITEM_NAME-1]=0;
    return BONES_MOD_OK;
}
int32_t bones_skin_ui_compose(uint32_t *pixels,int width,int height,int stride) {
    Skin2Canvas canvas;
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    if(!skin2_canvas_init(&canvas,pixels,width,height,stride))return BONES_MOD_EINVAL;
    skin2_draw(&skin_mod_state.ui,&skin_mod_state.view,&canvas);
    return BONES_MOD_OK;
}
uint32_t bones_skin_ui_actor_count(void){return skin_mod_state.started?skin_mod_state.actor_count:0u;}
uint64_t bones_skin_ui_last_tick(void){return skin_mod_state.started?skin_mod_state.last_tick:0u;}
const Skin2Ui *bones_skin_ui_state(void){return skin_mod_state.started?&skin_mod_state.ui:NULL;}

int bones_skin_ui_blocks_gameplay(void){return skin2_blocks_gameplay(skin_mod_state.started?&skin_mod_state.ui:NULL);}
int32_t bones_skin_ui_enter_game(void){
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    skin2_enter_game(&skin_mod_state.ui);return BONES_MOD_OK;
}
int32_t bones_skin_ui_target(uint64_t id,int host_valid,const char *label){
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    skin2_set_target(&skin_mod_state.ui,id,host_valid,label);return BONES_MOD_OK;
}

int32_t bones_skin_ui_enter_main(void){
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    skin2_enter_main(&skin_mod_state.ui);return BONES_MOD_OK;
}

int32_t bones_skin_ui_lan_status(unsigned phase,unsigned error,unsigned port,int known){
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    if(phase>8u||error>9u||port>65535u)return BONES_MOD_EINVAL;
    skin2_lan_status(&skin_mod_state.ui,phase,error,port,known);
    return BONES_MOD_OK;
}
int32_t bones_skin_ui_lan_text(uint32_t ascii){
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    return skin2_lan_character(&skin_mod_state.ui,ascii)?BONES_MOD_OK:BONES_MOD_EINVAL;
}
int32_t bones_skin_ui_lan_request(char ipv4[16],uint16_t *port){
    if(!skin_mod_state.started)return BONES_MOD_BAD_STATE;
    return skin2_lan_request(&skin_mod_state.ui,ipv4,port)?BONES_MOD_OK:BONES_MOD_EINVAL;
}
