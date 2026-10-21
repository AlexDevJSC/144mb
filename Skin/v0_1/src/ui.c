#include "skin/skin.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

/* 5x7 bitmap font stored as 7 rows of 5 bits, fallback '?'.
   Uppercase uppercase-only to keep the executable small. */
typedef struct {char c;uint8_t row[7];} Glyph;
static const Glyph glyphs[]={
{'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
{'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
{'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
{'G',{14,17,16,23,17,17,14}}, {'H',{17,17,17,31,17,17,17}},
{'I',{31,4,4,4,4,4,31}}, {'J',{7,2,2,2,18,18,12}},
{'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
{'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
{'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
{'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
{'S',{15,16,16,14,1,1,30}}, {'T',{31,4,4,4,4,4,4}},
{'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
{'W',{17,17,17,21,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
{'Y',{17,17,10,4,4,4,4}}, {'Z',{31,1,2,4,8,16,31}},
{'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
{'2',{14,17,1,2,4,8,31}}, {'3',{30,1,1,14,1,1,30}},
{'4',{2,6,10,18,31,2,2}}, {'5',{31,16,16,30,1,1,30}},
{'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,4,4,4}},
{'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
{'.',{0,0,0,0,0,12,12}}, {':',{0,12,12,0,12,12,0}},
{'-',{0,0,0,31,0,0,0}}, {'/',{1,1,2,4,8,16,16}},
{'?',{14,17,1,2,4,0,4}}, {'>',{16,8,4,2,4,8,16}},
{' ',{0,0,0,0,0,0,0}}, {'=',{0,31,0,31,0,0,0}},
};
static const uint8_t *glyph(char c) {
    unsigned i;
    if(c>='a'&&c<='z')c=(char)(c-'a'+'A');
    for(i=0;i<sizeof(glyphs)/sizeof(glyphs[0]);i++)if(glyphs[i].c==c)return glyphs[i].row;
    return glyphs[sizeof(glyphs)/sizeof(glyphs[0])-4].row;
}
static void rect(uint32_t *p,int w,int h,int x,int y,int rw,int rh,uint32_t c) {
    int yy,xx,ex=x+rw,ey=y+rh;
    if(!p||w<=0||h<=0||rw<=0||rh<=0)return;
    if(x<0)x=0;
    if(y<0)y=0;
    if(ex>w)ex=w;
    if(ey>h)ey=h;
    for(yy=y;yy<ey;yy++)for(xx=x;xx<ex;xx++)p[yy*w+xx]=0xff000000u|(c&0xffffffu);
}
static void outline(uint32_t *p,int w,int h,int x,int y,int rw,int rh,uint32_t c) {
    rect(p,w,h,x,y,rw,1,c);rect(p,w,h,x,y+rh-1,rw,1,c);
    rect(p,w,h,x,y,1,rh,c);rect(p,w,h,x+rw-1,y,1,rh,c);
}
static void string(uint32_t *p,int w,int h,int x,int y,const char *s,uint32_t color) {
    int i=0;
    if(!s)return;
    while(*s&&i<60) {
        int yy,xx;const uint8_t *g=glyph(*s++);
        for(yy=0;yy<7;yy++)for(xx=0;xx<5;xx++)if(g[yy]&(1u<<(4-xx)))
            rect(p,w,h,x+i*6+xx,y+yy,1,1,color);
        i++;
    }
}
static int menu_count(SkinScreen screen) {return screen==SKIN_MENU_MAIN?3:screen==SKIN_MENU_PAUSE?4:0;}
void skin_ui_init(SkinUi *ui) {
    if(!ui)return;
    memset(ui,0,sizeof(*ui));ui->screen=SKIN_MENU_MAIN;
    ui->palette=(SkinPalette){0x10121b,0x1f2631,0x8a744e,0xe9dfc8,
         0xa8acb0,0xbb9151,0x86c5a1,0xd77a72};
}
void skin_ui_notice(SkinUi *ui,const char *message,bool error) {
    if(!ui)return;
    if(!message)message="";
    (void)snprintf(ui->notice,sizeof(ui->notice),"%s",message);
    ui->notice_error=error;
}
SkinAction skin_ui_key(SkinUi *ui,SkinKey key) {
    int count;SkinAction result=SKIN_ACTION_NONE;
    if(!ui)return result;
    if(ui->screen==SKIN_MENU_GAME) {
        if(key==SKIN_KEY_BACK){ui->screen=SKIN_MENU_PAUSE;ui->selection=0;}
        return result;
    }
    if(key==SKIN_KEY_BACK) {
        if(ui->screen==SKIN_MENU_PAUSE) {ui->screen=SKIN_MENU_GAME;ui->selection=0;}
        return result;
    }
    count=menu_count(ui->screen);
    if(key==SKIN_KEY_UP)ui->selection=(uint8_t)((ui->selection+count-1)%count);
    if(key==SKIN_KEY_DOWN)ui->selection=(uint8_t)((ui->selection+1)%count);
    if(key!=SKIN_KEY_ACCEPT)return result;
    if(ui->screen==SKIN_MENU_MAIN) {
        switch(ui->selection) {
        case 0:result=SKIN_ACTION_START;break;
        case 1:result=SKIN_ACTION_LOAD;break;
        case 2:result=SKIN_ACTION_QUIT;break;
        }
    }else if(ui->screen==SKIN_MENU_PAUSE) {
        switch(ui->selection) {
        case 0:result=SKIN_ACTION_RESUME;break;
        case 1:result=SKIN_ACTION_SAVE;break;
        case 2:result=SKIN_ACTION_LOAD;break;
        case 3:result=SKIN_ACTION_TO_MAIN;break;
        }
    }
    if(result==SKIN_ACTION_RESUME||result==SKIN_ACTION_START) {
        ui->screen=SKIN_MENU_GAME;ui->selection=0;skin_ui_notice(ui,"",false);
    }
    if(result==SKIN_ACTION_TO_MAIN){ui->screen=SKIN_MENU_MAIN;ui->selection=0;skin_ui_notice(ui,"",false);}
    return result;
}
void skin_ui_placeholder_background(uint32_t *p,int w,int h,uint32_t t) {
    int x,y;
    if(!p||w<=0||h<=0||w>4096||h>4096)return;
    for(y=0;y<h;y++)for(x=0;x<w;x++) {
        int offset=(x+(y/16%2)*11)%32;
        unsigned v=(unsigned)((x*11+y*13+(int)t*0)&7)+((y%16<2||offset<2)?14u:40u);
        unsigned b=(v*3u)/4u;
        p[y*w+x]=0xff000000u | (v<<16) | ((v-3u)<<8) | b;
    }
}
void skin_ui_draw(const SkinUi *ui,const BonesEngine *e,uint32_t *p,int w,int h) {
    static const char *main_items[]={"NUEVA PARTIDA","CONTINUAR","SALIR"};
    static const char *pause_items[]={"VOLVER","GUARDAR","CARGAR","MENU PRINCIPAL"};
    const SkinPalette *c;
    int x,y,i,menu_y,count;
    char status[90];
    if(!ui||!p||w<=0||h<=0)return;
    c=&ui->palette;
    if(ui->screen==SKIN_MENU_GAME) {
        /* Layered on top of Eye's frame, no overwriting the entire scene. */
        rect(p,w,h,3,3,w-6,15,c->panel);
        if(e) {
            (void)snprintf(status,sizeof(status),"TICK %llu  POS %.1f %.1f %.1f",
                (unsigned long long)e->tick,e->player.feet.x,e->player.feet.y,e->player.feet.z);
            string(p,w,h,8,7,status,c->text);
        }
        rect(p,w,h,3,h-15,w-6,12,c->panel);
        string(p,w,h,8,h-12,"ESC: PAUSA",c->muted);
        if(ui->notice[0])string(p,w,h,w/2-((int)strlen(ui->notice)*3),h-12,ui->notice,
               ui->notice_error?c->error:c->success);
        return;
    }
    x=(w-240)/2; y=(h-166)/2;
    rect(p,w,h,x,y,240,166,c->background);
    outline(p,w,h,x,y,240,166,c->edge);
    outline(p,w,h,x+4,y+4,232,158,c->panel);
    string(p,w,h,x+25,y+17,"PROYECTO 1.44",c->accent);
    string(p,w,h,x+85,y+31,"S K I N",c->text);
    rect(p,w,h,x+17,y+46,206,1,c->edge);
    menu_y=y+57;
    count=menu_count(ui->screen);
    for(i=0;i<count;i++) {
        bool selected=i==ui->selection;
        if(selected) {rect(p,w,h,x+17,menu_y+i*19,206,16,c->panel);
                      outline(p,w,h,x+17,menu_y+i*19,206,16,c->accent);}
        string(p,w,h,x+28,menu_y+i*19+4,
               ui->screen==SKIN_MENU_MAIN?main_items[i]:pause_items[i],
               selected?c->text:c->muted);
    }
    string(p,w,h,x+18,y+140,"FLECHAS / ENTER / ESC",c->muted);
    if(ui->notice[0]) {
        /* Shortened to panel width: more detailed status belongs to native log. */
        char short_text[34];
        (void)snprintf(short_text,sizeof(short_text),"%.32s",ui->notice);
        string(p,w,h,x+18,y+151,short_text,ui->notice_error?c->error:c->success);
    }
}
