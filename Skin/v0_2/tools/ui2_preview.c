#include "skin/ui2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t pixels[1280*720];
static void background(int w,int h){
    int x,y;
    for(y=0;y<h;y++)for(x=0;x<w;x++){
        unsigned brick=(unsigned)(((x/27)+((y/16)&1))%5);
        unsigned v=(unsigned)(54+(y*33)/h)+(brick*3u);
        unsigned mortar=((y%16)==0 || (x+((y/16)&1)*14)%27==0)?15u:0;
        pixels[y*w+x]=0xff000000u|((v-mortar)<<16)|((v-mortar-4u)<<8)|(v-mortar-7u);
    }
}
static void ppm(const char *path,int w,int h){
    FILE *f=fopen(path,"wb");int j,i;
    if(!f){perror(path);exit(1);}
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(j=0;j<h;j++)for(i=0;i<w;i++){
        uint32_t v=pixels[j*w+i];
        unsigned char b[3]={(unsigned char)(v>>16),(unsigned char)(v>>8),(unsigned char)v};
        if(fwrite(b,1,3,f)!=3){fclose(f);exit(1);}
    }
    if(fclose(f)!=0)exit(1);
}
int main(void){
    Skin2Ui ui;Skin2View v={0};Skin2Canvas c;
    static const char *names[]={"ESPADA HIERRO","POCION ROJA","ESCUDO ROTO","LLAVE COBRE","ANTORCHA","MONEDAS","ARCO CORTO"};
    int screen;
    skin2_init(&ui);v.hp=86;v.hp_max=100;v.mp=21;v.mp_max=55;v.gold=1530;v.level=4;
    v.item_count=7;
    for(screen=0;screen<7;screen++){
        (void)snprintf(v.items[screen].name,SKIN_UI2_ITEM_NAME,"%s",names[screen]);
        v.items[screen].count=(uint16_t)(screen+1);
        v.items[screen].icon=(uint16_t)screen;
    }
    for(screen=0;screen<5;screen++){
        int w=screen==4?1280:320, h=screen==4?720:200;
        const char *path[]={"build/eye_skin_menu_320.ppm","build/eye_skin_pausa_320.ppm",
            "build/eye_skin_inventario_320.ppm","build/eye_skin_hud_320.ppm",
            "build/eye_skin_inventario_1280.ppm"};
        ui.screen=(Skin2Screen[]){SKIN2_MAIN,SKIN2_PAUSE,SKIN2_INVENTORY,SKIN2_GAME,SKIN2_INVENTORY}[screen];
        background(w,h);
        if(!skin2_canvas_init(&c,pixels,w,h,w))return 1;
        skin2_draw(&ui,&v,&c);ppm(path[screen],w,h);
    }
    printf("UI2 demo: five frames 320x200 / 1280x720; no Eye textures required. Skin2Ui state=%zu bytes\n",skin2_state_bytes());
    return 0;
}
