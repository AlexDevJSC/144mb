#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../../BONES/modules/skin/skin_modkit.h"

static uint64_t tick_value=27;
static uint64_t time_cb(void *p){(void)p;return tick_value;}
static int32_t count_cb(void *p){(void)p;return 2;}
static int32_t actor_cb(void *p,uint32_t i,BonesModActor *out){
    (void)p;
    if(!out||out->size!=sizeof(*out)||i>=2)return BONES_MOD_EINVAL;
    out->id=i+1;out->kind=i==0?1:2;return BONES_MOD_OK;
}
static BonesModServices make_services(uint32_t perms){
    BonesModServices s={0};
    s.size=(uint32_t)sizeof(s);s.abi=BONES_MOD_ABI;s.permissions=perms;
    s.tick=time_cb;s.actor_count=count_cb;s.actor_at=actor_cb;
    return s;
}
static uint32_t pixels[320*200];
static uint32_t wide[800*450];
static uint64_t hash(const uint32_t *p,size_t n){
    uint64_t h=UINT64_C(1469598103934665603);size_t i;
    for(i=0;i<n;i++){h^=p[i];h*=UINT64_C(1099511628211);}
    return h;
}
int main(void){
    BonesModDescriptor d;BonesModServices svc;BonesModEvent ev;Skin2View v={0};
    uint64_t before,after;
    uint32_t i;
    memset(&d,0,sizeof(d));d.size=sizeof(d);
    assert(bones_mod_skin_query(0,&d)==BONES_MOD_EINVAL);
    assert(bones_mod_skin_query(BONES_MOD_ABI,NULL)==BONES_MOD_EINVAL);
    assert(bones_mod_skin_query(BONES_MOD_ABI,&d)==BONES_MOD_OK);
    assert(strcmp(d.id,"skin")==0&&d.role==BONES_ROLE_SKIN&&d.abi==BONES_MOD_ABI);
    assert(d.state_schema_version==0&&d.required_roles==0);
    assert(!d.present&&!d.build_scene&&!d.before_tick);
    assert((d.requested_permissions&(BONES_PERMISSION_SEND_COMMANDS|BONES_PERMISSION_WRITE_SCENE|BONES_PERMISSION_RESTORE_SNAPSHOT))==0);
    assert(bones_skin_ui_state()==NULL);
    assert(bones_skin_ui_compose(pixels,320,200,320)==BONES_MOD_BAD_STATE);
    svc=make_services(BONES_PERMISSION_READ_TIME);
    assert(d.start(d.user_context,&svc)==BONES_MOD_EINVAL);
    svc=make_services(d.requested_permissions);
    assert(d.start(d.user_context,&svc)==BONES_MOD_OK);
    assert(bones_skin_ui_state()!=NULL&&bones_skin_ui_last_tick()==27);
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_START);
    assert(bones_skin_ui_state()->screen==SKIN2_GAME);
    assert(bones_skin_ui_update(NULL)==BONES_MOD_EINVAL);
    v.hp=42;v.hp_max=80;v.mp=10;v.mp_max=20;v.gold=90;
    v.item_count=1;strcpy(v.items[0].name,"POCION");v.items[0].count=3;
    assert(bones_skin_ui_update(&v)==BONES_MOD_OK);
    v.item_count=21;assert(bones_skin_ui_update(&v)==BONES_MOD_EINVAL);
    memset(pixels,0x11,sizeof(pixels));before=hash(pixels,320*200);
    assert(bones_skin_ui_compose(pixels,320,200,320)==BONES_MOD_OK);
    after=hash(pixels,320*200);assert(before!=after);
    assert(bones_skin_ui_compose(NULL,320,200,320)==BONES_MOD_EINVAL);
    assert(bones_skin_ui_input(SKIN2_OPEN_INV)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_state()->screen==SKIN2_INVENTORY);
    assert(bones_skin_ui_compose(pixels,320,200,320)==BONES_MOD_OK);
    for(i=0;i<800*450;i++)wide[i]=0xff102030u;
    assert(bones_skin_ui_compose(wide,800,450,800)==BONES_MOD_OK);
    assert(wide[0]==0xff102030u); /* 16:10 UI has horizontal letterbox on 16:9 */
    tick_value=30;d.after_tick(d.user_context);
    assert(bones_skin_ui_last_tick()==30&&bones_skin_ui_actor_count()==2);
    memset(&ev,0,sizeof(ev));ev.size=sizeof(ev);ev.type=BONES_EVT_LAND;
    d.on_event(d.user_context,&ev);
    assert(!strcmp(bones_skin_ui_state()->notice,"ATERRIZAJE"));
    d.stop(d.user_context);
    assert(bones_skin_ui_state()==NULL);
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_update(&v)==BONES_MOD_BAD_STATE);
    puts("PASS Skin v0.3: ABI1, permisos, lifecycle, menu, inventario, overlay y eventos");
    return 0;
}
