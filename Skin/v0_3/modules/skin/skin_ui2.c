#include "skin_ui2.h"
#include "skin_font.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint32_t blend(uint32_t dst,uint32_t src){
    unsigned a=src>>24, ia=255u-a;
    unsigned r=(((src>>16)&255u)*a + ((dst>>16)&255u)*ia +127u)/255u;
    unsigned g=(((src>>8)&255u)*a + ((dst>>8)&255u)*ia +127u)/255u;
    unsigned b=((src&255u)*a + (dst&255u)*ia +127u)/255u;
    return 0xff000000u|(r<<16)|(g<<8)|b;
}
bool skin2_canvas_init(Skin2Canvas *c,uint32_t *p,int w,int h,int stride){
    int vw,vh;
    if(!c||!p||w<1||h<1||w>8192||h>8192||stride<w||stride>8192)return false;
    /* UI coordinates are in a virtual 320x200 space even if Eye renders at 320x200. */
    if((int64_t)w*SKIN_UI2_H<=(int64_t)h*SKIN_UI2_W){vw=w;vh=(int)((int64_t)w*SKIN_UI2_H/SKIN_UI2_W);}
    else {vh=h;vw=(int)((int64_t)h*SKIN_UI2_W/SKIN_UI2_H);}
    *c=(Skin2Canvas){p,w,h,stride,(w-vw)/2,(h-vh)/2,vw,vh};
    return true;
}
void skin2_fill(Skin2Canvas *c,int x,int y,int w,int h,uint32_t rgba){
    int left,top,right,bottom,i,j;
    if(!c||!c->pixels||w<=0||h<=0||!(rgba>>24))return;
    /* Clamp in virtual units to prevent invalid reads and oversized loops. */
    if(x>=SKIN_UI2_W||y>=SKIN_UI2_H||x+(int64_t)w<=0||y+(int64_t)h<=0)return;
    left=x<0?0:(x>SKIN_UI2_W?SKIN_UI2_W:x);
    top=y<0?0:(y>SKIN_UI2_H?SKIN_UI2_H:y);
    right=(int64_t)x+w>SKIN_UI2_W?SKIN_UI2_W:((int64_t)x+w<0?0:(int)((int64_t)x+w));
    bottom=(int64_t)y+h>SKIN_UI2_H?SKIN_UI2_H:((int64_t)y+h<0?0:(int)((int64_t)y+h));
    if(left>=right||top>=bottom)return;
    left=c->vx+(int)((int64_t)left*c->vw/SKIN_UI2_W);
    right=c->vx+(int)((int64_t)right*c->vw/SKIN_UI2_W);
    top=c->vy+(int)((int64_t)top*c->vh/SKIN_UI2_H);
    bottom=c->vy+(int)((int64_t)bottom*c->vh/SKIN_UI2_H);
    if(left<0||top<0||right>c->width||bottom>c->height)return;
    if((rgba>>24)==255u){for(j=top;j<bottom;j++)for(i=left;i<right;i++)c->pixels[j*c->stride+i]=rgba;}
    else {for(j=top;j<bottom;j++)for(i=left;i<right;i++){
        uint32_t *p=&c->pixels[j*c->stride+i]; *p=blend(*p,rgba);
    }}
}
/* Tiny reusable nine-patch made of flat fills / repeated 2px edge cuts.
   No full-frame, button, or window images stored by Skin. */
static void panel(Skin2Canvas *c,const Skin2Theme *t,int x,int y,int w,int h){
    int n;
    skin2_fill(c,x+3,y+3,w,h,0x72000000u);
    skin2_fill(c,x+2,y+2,w-4,h-4,t->panel);
    skin2_fill(c,x+2,y,w-4,2,t->border_light);
    skin2_fill(c,x+2,y+h-2,w-4,2,t->border_dark);
    skin2_fill(c,x,y+2,2,h-4,t->border_light);
    skin2_fill(c,x+w-2,y+2,2,h-4,t->border_dark);
    skin2_fill(c,x+1,y+1,2,2,t->accent);
    skin2_fill(c,x+w-3,y+1,2,2,t->accent);
    skin2_fill(c,x+1,y+h-3,2,2,t->accent);
    skin2_fill(c,x+w-3,y+h-3,2,2,t->accent);
    for(n=x+7;n<x+w-7;n+=8){
        skin2_fill(c,n,y+2,2,1,t->border_dark);
        skin2_fill(c,n,y+h-3,2,1,t->border_light);
    }
}
static void text(Skin2Canvas *c,const char *s,int x,int y,int scale,uint32_t color){
    int j,k,count=0;
    if(!s||scale<1||scale>3)return;
    while(*s&&count<48){
        const uint8_t *g=skin_font_rows(*s++);
        for(j=0;j<7;j++)for(k=0;k<5;k++)if(g[j] & (1u<<(4-k)))
            skin2_fill(c,x+k*scale,y+j*scale,scale,scale,color);
        x+=6*scale;count++;
    }
}
static void button(Skin2Canvas *c,const Skin2Theme *t,int x,int y,int w,int h,const char *name,bool selected){
    if(selected){skin2_fill(c,x,y,w,h,t->selected);skin2_fill(c,x,y,2,h,t->accent);}
    else skin2_fill(c,x,y,w,h,0x63221918u);
    text(c,name,x+7,y+(h-7)/2,1,selected?t->text:t->muted);
}
static void number(Skin2Canvas *c,int x,int y,uint32_t v,uint32_t color){
    char digits[24];(void)snprintf(digits,sizeof(digits),"%u",(unsigned)v);text(c,digits,x,y,1,color);
}
static void bar(Skin2Canvas *c,const Skin2Theme *t,int x,int y,int w,uint32_t v,uint32_t maximum,uint32_t color){
    skin2_fill(c,x,y,w,7,t->border_dark);
    skin2_fill(c,x+1,y+1,w-2,5,0xff171817u);
    if(maximum&&v){
        uint32_t filled=(uint32_t)(((uint64_t)(v>maximum?maximum:v)*(unsigned)(w-2))/maximum);
        if(filled>(unsigned)(w-2))filled=(unsigned)(w-2);
        skin2_fill(c,x+1,y+1,(int)filled,5,color);
    }
}
void skin2_init(Skin2Ui *ui){
    if(!ui)return;
    memset(ui,0,sizeof(*ui));ui->screen=SKIN2_MAIN;
    memcpy(ui->lan_port,"14444",6);
    /* Provisional proposed palette, NOT colors already supplied by Eye. */
    ui->theme=(Skin2Theme){0xc8000000u,0xf026211du,0xff332d24u,0xff48382bu,
        0xff9b8558u,0xffefe5ca,0xffb3a88e,0xffd2ab65,0xff51412au,
        0xffac5144,0xff526c9c,0xff829567,0xffb54c43};
}
void skin2_notice(Skin2Ui *ui,const char *notice){
    if(!ui)return;
    (void)snprintf(ui->notice,sizeof(ui->notice),"%.43s",notice?notice:"");
}
void skin2_enter_main(Skin2Ui *ui){
    if(ui){ui->screen=SKIN2_MAIN;ui->selection=0;ui->target_id=0;ui->target_valid=0;ui->notice[0]=0;}
}
void skin2_enter_game(Skin2Ui *ui){
    if(ui){ui->screen=SKIN2_GAME;ui->selection=0;ui->notice[0]=0;}
}
void skin2_set_target(Skin2Ui *ui,uint64_t id,int host_valid,const char *label){
    if(!ui)return;
    ui->target_valid=(uint8_t)(host_valid && id!=0);
    ui->target_id=ui->target_valid?id:0;
    (void)snprintf(ui->target_label,sizeof(ui->target_label),"%.31s",ui->target_valid&&label?label:"");
}
/* Only UI validation. Bones/Mouth must still authenticate world and connection. */
static int lan_form_valid(const Skin2Ui *ui,unsigned *port_out){
    const char *p=ui->lan_ipv4; unsigned parts=0,port=0; const char *q=ui->lan_port;
    while(parts<4){
        unsigned val=0,digits=0;
        while(*p>='0'&&*p<='9'&&digits<3){val=10u*val+(unsigned)(*p++-'0');digits++;}
        if(!digits||val>255 || (*p>='0'&&*p<='9'))return 0;
        parts++;
        if(parts==4){if(*p)return 0;}else if(*p++!='.')return 0;
    }
    if(!*q)return 0;
    while(*q){if(*q<'0'||*q>'9')return 0;port=port*10u+(unsigned)(*q++-'0');if(port>65535u)return 0;}
    if(!port)return 0;
    if(port_out)*port_out=port;
    return 1;
}
void skin2_lan_status(Skin2Ui *ui,unsigned phase,unsigned error,unsigned port,int identity_ready){
    if(!ui)return;
    ui->lan_phase=(uint8_t)(phase<=8u?phase:0u);
    ui->lan_error=(uint8_t)(error<=9u?error:1u);
    ui->lan_current_port=(uint16_t)(port<=65535u?port:0u);
    ui->lan_identity_ready=(uint8_t)(identity_ready!=0);
}
int skin2_lan_character(Skin2Ui *ui,uint32_t ascii){
    char *p;size_t len,cap;int is_ip;
    if(!ui||ui->screen!=SKIN2_LAN_JOIN||ui->selection>1)return 0;
    is_ip=ui->selection==0;
    p=is_ip?ui->lan_ipv4:ui->lan_port;
    cap=is_ip?sizeof(ui->lan_ipv4):sizeof(ui->lan_port);
    len=strlen(p);
    if(ascii==8u){if(len){p[len-1]=0;return 1;}return 0;}
    if((ascii<'0'||ascii>'9')&&!(is_ip&&ascii=='.'))return 0;
    if(len+1>=cap)return 0;
    p[len]=(char)ascii;p[len+1]=0;return 1;
}
int skin2_lan_request(const Skin2Ui *ui,char ipv4[16],uint16_t *port){
    unsigned n;
    if(!ui||!ipv4||!port||ui->screen!=SKIN2_LAN_JOIN||!ui->lan_identity_ready||!lan_form_valid(ui,&n))return 0;
    memcpy(ipv4,ui->lan_ipv4,16);*port=(uint16_t)n;return 1;
}
static const char *lan_phase_text(unsigned p){
    static const char * const names[]={"SIN SESION","HOST PREPARADO","ESPERANDO CLIENTE","CONECTANDO", "CONECTADO","RECONECTANDO","INCOMPATIBLE","TIEMPO AGOTADO","CONEXION PERDIDA"};
    return p<=8u?names[p]:"ESTADO DESCONOCIDO";
}
static const char *lan_error_text(unsigned p){
    static const char * const names[]={"SIN ERROR","ARGUMENTO INVALIDO","IP INVALIDA","ERROR SOCKET", "OCUPADO","NO PREPARADO","MUNDO INCOMPATIBLE","PROTOCOLO INCOMPAT.","TIEMPO AGOTADO","CONEXION PERDIDA"};
    return p<=9u?names[p]:"ERROR DESCONOCIDO";
}
int skin2_blocks_gameplay(const Skin2Ui *ui){return !ui||ui->screen!=SKIN2_GAME;}
static int menu_count(Skin2Screen s){return s==SKIN2_MAIN?5:s==SKIN2_PAUSE?4:s==SKIN2_SETTINGS?3:0;}
Skin2Action skin2_input(Skin2Ui *ui,Skin2Key key){
    int count;
    if(!ui)return SKIN2_NO_ACTION;
    if(key==SKIN2_BACK){
        if(ui->screen==SKIN2_LAN_JOIN||ui->screen==SKIN2_LAN_STATUS){ui->screen=SKIN2_LAN;ui->selection=0;}
        else if(ui->screen==SKIN2_INVENTORY||ui->screen==SKIN2_SETTINGS||ui->screen==SKIN2_LAN||ui->screen==SKIN2_CONTEXT){
            ui->screen=ui->previous;ui->selection=0;
        }else if(ui->screen==SKIN2_PAUSE){skin2_enter_game(ui);return SKIN2_RESUME;}
        else if(ui->screen==SKIN2_GAME){ui->screen=SKIN2_PAUSE;ui->selection=0;}
        return SKIN2_NO_ACTION;
    }
    if(key==SKIN2_OPEN_INV && (ui->screen==SKIN2_GAME||ui->screen==SKIN2_PAUSE)){
        ui->previous=ui->screen;ui->screen=SKIN2_INVENTORY;ui->selection=0;return SKIN2_NO_ACTION;
    }
    if(key==SKIN2_INTERACT&&ui->screen==SKIN2_GAME&&ui->target_valid){
        ui->previous=SKIN2_GAME;ui->screen=SKIN2_CONTEXT;ui->selection=0;
        return SKIN2_INTERACT_REQUEST; /* Never change gameplay or transactions here. */
    }
    if(ui->screen==SKIN2_LAN){
        if(key==SKIN2_UP)ui->selection=(uint8_t)((ui->selection+3u)%4u);
        else if(key==SKIN2_DOWN)ui->selection=(uint8_t)((ui->selection+1u)%4u);
        else if(key==SKIN2_ACCEPT){
            if(ui->selection==0){
                if(!ui->lan_identity_ready){skin2_notice(ui,"MUNDO CONOCIDO REQUERIDO");return SKIN2_NO_ACTION;}
                return SKIN2_LAN_CREATE_REQUEST;
            }
            if(ui->selection==1){ui->screen=SKIN2_LAN_JOIN;ui->selection=0;}
            if(ui->selection==2){ui->screen=SKIN2_LAN_STATUS;ui->selection=0;}
            if(ui->selection==3){ui->screen=ui->previous;ui->selection=0;}
        }
        return SKIN2_NO_ACTION;
    }
    if(ui->screen==SKIN2_LAN_JOIN){
        if(key==SKIN2_UP)ui->selection=(uint8_t)((ui->selection+3u)%4u);
        else if(key==SKIN2_DOWN)ui->selection=(uint8_t)((ui->selection+1u)%4u);
        else if(key==SKIN2_ACCEPT){
            if(ui->selection==3){ui->screen=SKIN2_LAN;ui->selection=0;}
            else if(ui->selection==2){
                if(!ui->lan_identity_ready)skin2_notice(ui,"FALTAN MUNDO Y SEMILLA");
                else if(!lan_form_valid(ui,NULL))skin2_notice(ui,"IP O PUERTO INVALIDO");
                else return SKIN2_LAN_JOIN_REQUEST;
            }
        }
        return SKIN2_NO_ACTION;
    }
    if(ui->screen==SKIN2_LAN_STATUS){
        if(key==SKIN2_ACCEPT){
            if(ui->lan_phase==0u){skin2_notice(ui,"SIN SESION ACTIVA");return SKIN2_NO_ACTION;}
            return SKIN2_LAN_DISCONNECT_REQUEST;
        }
        return SKIN2_NO_ACTION;
    }
    if(ui->screen==SKIN2_GAME||ui->screen==SKIN2_CONTEXT)return SKIN2_NO_ACTION;
    if(ui->screen==SKIN2_INVENTORY){
        if(key==SKIN2_LEFT&&ui->selection%5)ui->selection--;
        else if(key==SKIN2_RIGHT&&ui->selection%5<4)ui->selection++;
        else if(key==SKIN2_UP&&ui->selection>=5)ui->selection-=5;
        else if(key==SKIN2_DOWN&&ui->selection<15)ui->selection+=5;
        return SKIN2_NO_ACTION;
    }
    count=menu_count(ui->screen);
    if(!count)return SKIN2_NO_ACTION;
    if(key==SKIN2_UP){ui->selection=(uint8_t)((ui->selection+count-1)%count);return SKIN2_NO_ACTION;}
    if(key==SKIN2_DOWN){ui->selection=(uint8_t)((ui->selection+1)%count);return SKIN2_NO_ACTION;}
    if(key!=SKIN2_ACCEPT)return SKIN2_NO_ACTION;
    if(ui->screen==SKIN2_SETTINGS){
        if(ui->selection==0){ui->ui_scale_setting^=1;return SKIN2_APPLY_SETTINGS;}
        if(ui->selection==1){skin2_notice(ui,"AUDIO: PENDIENTE EAR");return SKIN2_NO_ACTION;}
        ui->screen=ui->previous;ui->selection=0;return SKIN2_NO_ACTION;
    }
    if(ui->screen==SKIN2_MAIN){
        switch(ui->selection){
        case 0:return SKIN2_START; /* Host must create a new world and ACK. */
        case 1:return SKIN2_SINGLE; /* Host decides; Skin never starts gameplay. */
        case 2:ui->previous=SKIN2_MAIN;ui->screen=SKIN2_LAN;ui->selection=0;break;
        case 3:ui->previous=SKIN2_MAIN;ui->screen=SKIN2_SETTINGS;ui->selection=0;break;
        case 4:return SKIN2_QUIT;
        }
    }else if(ui->screen==SKIN2_PAUSE){
        switch(ui->selection){
        case 0:skin2_enter_game(ui);return SKIN2_RESUME;
        case 1:ui->previous=SKIN2_PAUSE;ui->screen=SKIN2_INVENTORY;ui->selection=0;break;
        case 2:ui->previous=SKIN2_PAUSE;ui->screen=SKIN2_SETTINGS;ui->selection=0;break;
        case 3:return SKIN2_MAIN_REQUEST; /* Host confirms transition. */
        }
    }
    return SKIN2_NO_ACTION;
}
static void hud(Skin2Canvas *c,const Skin2Theme *t,const Skin2Ui *ui,const Skin2View *v){
    char buf[48];
    skin2_fill(c,3,3,142,16,0xc91d1c1au);
    (void)snprintf(buf,sizeof(buf),"TICK %llu",(unsigned long long)v->tick);
    text(c,buf,7,7,1,t->text);
    (void)snprintf(buf,sizeof(buf),"ACTORES %u",(unsigned)v->actor_count);
    text(c,buf,165,7,1,t->text);
    if(v->available & SKIN2_AVAIL_HP){
        skin2_fill(c,3,23,118,11,0xc91d1c1au);
        text(c,"PV",7,25,1,t->text);bar(c,t,24,25,88,v->hp,v->hp_max,t->health);
    }
    if(v->available & SKIN2_AVAIL_MP){
        skin2_fill(c,123,23,118,11,0xc91d1c1au);
        text(c,"PM",127,25,1,t->text);bar(c,t,144,25,88,v->mp,v->mp_max,t->mana);
    }
    if(v->available & SKIN2_AVAIL_GOLD){
        skin2_fill(c,244,23,73,11,0xc91d1c1au);
        text(c,"ORO",248,25,1,t->accent);number(c,269,25,v->gold,t->text);
    }
    if(ui->target_valid){
        skin2_fill(c,97,150,126,15,0xdd171716u);
        text(c,"E - INTERACTUAR",108,154,1,t->text);
    }
    skin2_fill(c,3,ui->ui_scale_setting?183:185,314,ui->ui_scale_setting?17:12,0xc91d1c1au);
    text(c,ui->ui_scale_setting?"I / ESC":"I: INVENTARIO  ESC: PAUSA",8,ui->ui_scale_setting?184:188,ui->ui_scale_setting?2:1,t->text);
}
static void simple_menu(Skin2Canvas *c,const Skin2Theme *t,const Skin2Ui *ui){
    static const char *main_items[]={"NUEVA PARTIDA","UN JUGADOR","LAN EXPERIMENTAL","OPCIONES","SALIR"};
    static const char *pause_items[]={"CONTINUAR","INVENTARIO","OPCIONES","MENU PRINCIPAL"};
    int i,count=menu_count(ui->screen),dy=22;
    panel(c,t,53,10,214,180);
    text(c,"PROYECTO 1.44",116,24,1,t->accent);
    text(c,ui->screen==SKIN2_PAUSE?"PAUSA":"AVENTURA",137,37,1,t->text);
    skin2_fill(c,67,49,186,1,t->border_light);
    for(i=0;i<count;i++){
        const char *name=ui->screen==SKIN2_PAUSE?pause_items[i]:main_items[i];
        button(c,t,66,55+i*dy,188,18,name,ui->selection==i);
    }
    text(c,"FLECHAS / INTRO / ESC",93,174,1,t->muted);
}
static void inventory(Skin2Canvas *c,const Skin2Theme *t,const Skin2Ui *ui,const Skin2View *v){
    unsigned i;
    const int x=11,y=10,cell=34;
    char str[64];
    panel(c,t,x,y,298,180);
    text(c,"INVENTARIO",25,22,1,t->accent);
    skin2_fill(c,204,25,1,151,t->border_light);
    for(i=0;i<SKIN_UI2_SLOTS;i++){
        int sx=20+(int)(i%5u)*36, sy=39+(int)(i/5u)*31;
        bool selected=(unsigned)ui->selection==i;
        skin2_fill(c,sx,sy,cell,28,selected?t->selected:0xff342e25u);
        skin2_fill(c,sx,sy,cell,1,selected?t->accent:t->border_dark);
        skin2_fill(c,sx,sy+27,cell,1,t->border_dark);
        if((v->available & SKIN2_AVAIL_INVENTORY) && i<v->item_count && v->items[i].count){
            uint32_t icon=v->items[i].icon%3u==0?t->accent:v->items[i].icon%3u==1?t->mana:t->health;
            skin2_fill(c,sx+11,sy+4,12,13,icon);
            skin2_fill(c,sx+13,sy+6,8,9,t->panel);
            skin2_fill(c,sx+15,sy+8,4,5,icon);
            number(c,sx+23,sy+18,v->items[i].count,t->text);
        }
    }
    text(c,"OBJETO",213,42,1,t->accent);
    if((v->available & SKIN2_AVAIL_INVENTORY)&&ui->selection<v->item_count&&v->items[ui->selection].count){
        /* Fixed-size strings from the host; never trust unbounded pointers. */
        (void)snprintf(str,sizeof(str),"%.13s",v->items[ui->selection].name);
        text(c,str,213,58,1,t->text);
        text(c,"CANT.",213,79,1,t->muted);
        number(c,213,91,v->items[ui->selection].count,t->text);
    }else text(c,"VACIO",213,58,1,t->muted);
    if(v->available & SKIN2_AVAIL_GOLD){text(c,"ORO",213,115,1,t->accent);number(c,213,126,v->gold,t->text);}
    if(!(v->available & SKIN2_AVAIL_INVENTORY))text(c,"NO DISPONIBLE",213,144,1,t->danger);
    text(c,"ESC: VOLVER",213,168,1,t->muted);
}
static void settings(Skin2Canvas *c,const Skin2Theme *t,const Skin2Ui *ui){
    panel(c,t,53,25,214,145);
    text(c,"OPCIONES",133,41,1,t->accent);
    button(c,t,70,66,180,18,ui->ui_scale_setting?"HUD AMPLIADO":"HUD NORMAL",ui->selection==0);
    button(c,t,70,90,180,18,"AUDIO: PENDIENTE",ui->selection==1);
    button(c,t,70,114,180,18,"VOLVER",ui->selection==2);
    text(c,"ESC PARA VOLVER",103,150,1,t->muted);
}
static void lan_panel(Skin2Canvas *c,const Skin2Theme *t,const Skin2Ui *ui){
    static const char *options[]={"CREAR PARTIDA LAN","UNIRSE POR IP","ESTADO DE SESION","VOLVER"};
    unsigned i;
    char line[48];
    panel(c,t,29,15,262,170);
    text(c,"LAN EXPERIMENTAL",103,28,1,t->accent);
    if(ui->screen==SKIN2_LAN){
        for(i=0;i<4;i++)button(c,t,56,50+(int)i*23,208,19,options[i],ui->selection==i);
        text(c,ui->lan_identity_ready?"MUNDO CONOCIDO":"REQUIERE MUNDO PREVIO",58,153,1,ui->lan_identity_ready?t->success:t->danger);
    }else if(ui->screen==SKIN2_LAN_JOIN){
        text(c,"IPV4",54,47,1,t->muted);
        button(c,t,53,57,216,18,ui->lan_ipv4[0]?ui->lan_ipv4:"INTRODUCE IPV4",ui->selection==0);
        text(c,"PUERTO",54,80,1,t->muted);
        button(c,t,53,89,216,18,ui->lan_port,ui->selection==1);
        button(c,t,53,115,105,17,"CONECTAR",ui->selection==2);
        button(c,t,166,115,103,17,"VOLVER",ui->selection==3);
        text(c,"MUNDO Y SEMILLA REQUERIDOS",57,146,1,t->danger);
        text(c,"TECLEA IP / PUERTO",76,161,1,t->muted);
    }else{
        text(c,lan_phase_text(ui->lan_phase),65,56,1,t->text);
        text(c,lan_error_text(ui->lan_error),65,78,1,ui->lan_error?t->danger:t->success);
        (void)snprintf(line,sizeof(line),"PUERTO %u",(unsigned)ui->lan_current_port);
        text(c,line,65,102,1,t->muted);
        button(c,t,65,129,190,20,"DESCONECTAR",ui->lan_phase!=0u);
        text(c,"ESC: VOLVER",112,164,1,t->muted);
    }
}
static void pending_panel(Skin2Canvas *c,const Skin2Theme *t,const Skin2Ui *ui){
    panel(c,t,27,35,266,130);
    text(c,"INTERACCION",118,53,1,t->accent);
    text(c,ui->target_label[0]?ui->target_label:"OBJETO",80,81,1,t->text);
    text(c,"ESPERANDO A BONES",97,104,1,t->muted);
    text(c,"ESC: VOLVER",122,141,1,t->muted);
}
void skin2_draw(const Skin2Ui *ui,const Skin2View *view,Skin2Canvas *c){
    const Skin2Theme *t;
    if(!ui||!view||!c||!c->pixels)return;
    t=&ui->theme;
    if(ui->screen==SKIN2_GAME){hud(c,t,ui,view);return;}
    skin2_fill(c,0,0,SKIN_UI2_W,SKIN_UI2_H,t->shadow);
    if(ui->screen==SKIN2_INVENTORY)inventory(c,t,ui,view);
    else if(ui->screen==SKIN2_SETTINGS)settings(c,t,ui);
    else if(ui->screen==SKIN2_LAN||ui->screen==SKIN2_LAN_JOIN||ui->screen==SKIN2_LAN_STATUS)lan_panel(c,t,ui);
    else if(ui->screen==SKIN2_CONTEXT)pending_panel(c,t,ui);
    else simple_menu(c,t,ui);
    if(ui->notice[0]&&ui->screen!=SKIN2_INVENTORY){
        skin2_fill(c,50,181,220,12,0xee221d16u);
        text(c,ui->notice,55,183,1,t->text);
    }
}
size_t skin2_state_bytes(void){return sizeof(Skin2Ui);}
