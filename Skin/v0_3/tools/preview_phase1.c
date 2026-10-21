#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../BONES/modules/skin/skin_modkit.h"
static uint64_t tick(void *unused){(void)unused;return 79;}
static int32_t actor_count(void *unused){(void)unused;return 2;}
static int32_t actor_at(void *unused,uint32_t index,BonesModActor *a){(void)unused;
    if(index>=2 || !a || a->size!=sizeof(*a))return BONES_MOD_EINVAL;
    a->id=index+1;return BONES_MOD_OK;
}
static uint32_t frame[320*200];
static int image(const char *name){
    FILE *f=fopen(name,"wb");unsigned i;
    if(!f)return 0;
    memset(frame,0x20,sizeof(frame));
    if(bones_skin_ui_compose(frame,320,200,320)!=BONES_MOD_OK)return 0;
    fprintf(f,"P6\n320 200\n255\n");
    for(i=0;i<320u*200u;i++){
        uint32_t c=frame[i];unsigned char bytes[3]={(unsigned char)(c>>16),(unsigned char)(c>>8),(unsigned char)c};
        if(fwrite(bytes,1,3,f)!=3){fclose(f);return 0;}
    }
    fclose(f);return 1;
}
int main(void){
    BonesModServices s={0};BonesModDescriptor d={0};
    s.size=sizeof(s);s.abi=BONES_MOD_ABI;s.permissions=BONES_PERMISSION_READ_TIME|BONES_PERMISSION_READ_ACTORS;
    s.tick=tick;s.actor_count=actor_count;s.actor_at=actor_at;
    d.size=sizeof(d);
    if(bones_mod_skin_query(BONES_MOD_ABI,&d)!=0||d.start(d.user_context,&s)!=0)return 1;
    if(!image("build/skin_menu.ppm"))return 2;
    if(bones_skin_ui_enter_game()!=0)return 3;
    d.after_tick(d.user_context);
    if(!image("build/skin_hud.ppm"))return 4;
    if(bones_skin_ui_target(122,1,"COFRE")!=0)return 5;
    if(!image("build/skin_target.ppm"))return 6;
    bones_skin_ui_input(SKIN2_OPEN_INV);
    if(!image("build/skin_inventory.ppm"))return 7;
    bones_skin_ui_enter_main();bones_skin_ui_input(SKIN2_DOWN);bones_skin_ui_input(SKIN2_DOWN);bones_skin_ui_input(SKIN2_ACCEPT);
    if(!image("build/skin_lan.ppm"))return 8;
    d.stop(d.user_context);
    return 0;
}
