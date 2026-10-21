#include "skin/ui2.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t buf[1280*720];
static void fill(uint32_t color,int w,int h){int i;for(i=0;i<w*h;i++)buf[i]=color;}
static void test_alpha_and_bounds(void){
    Skin2Canvas c;
    assert(!skin2_canvas_init(&c,NULL,320,200,320));
    assert(!skin2_canvas_init(&c,buf,320,200,319));
    assert(!skin2_canvas_init(&c,buf,8193,1,8193));
    assert(skin2_canvas_init(&c,buf,320,200,320));
    fill(0xff204060u,320,200);
    skin2_fill(&c,2,3,1,1,0x00000000u);
    assert(buf[3*320+2]==0xff204060u);
    skin2_fill(&c,2,3,1,1,0x80ff0000u);
    /* exact source-over round-to-nearest, output opaque */
    assert(buf[3*320+2]==0xff902030u);
    skin2_fill(&c,-10000,-10000,10001,10001,0xffabcdefu);
    assert(buf[0]==0xffabcdefu);
    assert(buf[199*320+319]==0xff204060u);
    skin2_fill(&c,INT32_MAX,0,INT32_MAX,5,0xffffffffu);
    skin2_fill(&c,-10,-10,8,8,0xffffffffu);
    assert(buf[199*320+319]==0xff204060u);
}
static void test_navigation(void){
    Skin2Ui ui;Skin2View view={0};Skin2Action a;
    skin2_init(&ui);
    a=skin2_input(&ui,SKIN2_DOWN);assert(a==SKIN2_NO_ACTION&&ui.selection==1);
    a=skin2_input(&ui,SKIN2_ACCEPT);assert(a==SKIN2_LOAD);
    skin2_input(&ui,SKIN2_UP);assert(ui.selection==0);
    a=skin2_input(&ui,SKIN2_ACCEPT);assert(a==SKIN2_START&&ui.screen==SKIN2_GAME);
    a=skin2_input(&ui,SKIN2_OPEN_INV);assert(a==SKIN2_NO_ACTION&&ui.screen==SKIN2_INVENTORY);
    skin2_input(&ui,SKIN2_RIGHT);skin2_input(&ui,SKIN2_DOWN);
    assert(ui.selection==6);
    assert(skin2_input(&ui,SKIN2_ACCEPT)==SKIN2_NO_ACTION); /* UI never equips an item */
    assert(skin2_input(&ui,SKIN2_BACK)==SKIN2_NO_ACTION&&ui.screen==SKIN2_GAME);
    skin2_input(&ui,SKIN2_BACK);assert(ui.screen==SKIN2_PAUSE);
    skin2_input(&ui,SKIN2_DOWN);assert(skin2_input(&ui,SKIN2_ACCEPT)==SKIN2_SAVE);
    skin2_input(&ui,SKIN2_DOWN);assert(skin2_input(&ui,SKIN2_ACCEPT)==SKIN2_LOAD);
    skin2_input(&ui,SKIN2_DOWN);skin2_input(&ui,SKIN2_ACCEPT);
    assert(ui.screen==SKIN2_INVENTORY);
    skin2_input(&ui,SKIN2_BACK);assert(ui.screen==SKIN2_PAUSE);
    skin2_input(&ui,SKIN2_DOWN);skin2_input(&ui,SKIN2_DOWN);
    assert(ui.selection==2);
    skin2_input(&ui,SKIN2_DOWN);skin2_input(&ui,SKIN2_DOWN);
    assert(ui.selection==4);
    skin2_input(&ui,SKIN2_ACCEPT);assert(ui.screen==SKIN2_SETTINGS);
    assert(skin2_input(&ui,SKIN2_ACCEPT)==SKIN2_APPLY_SETTINGS&&ui.ui_scale_setting==1);
    skin2_input(&ui,SKIN2_BACK);assert(ui.screen==SKIN2_PAUSE);
    skin2_init(&ui);skin2_input(&ui,SKIN2_UP);assert(ui.selection==3);
    assert(skin2_input(&ui,SKIN2_ACCEPT)==SKIN2_QUIT);
    /* Not a mutation API. Rendering cannot change the supplied view. */
    memset(&view,0,sizeof(view));
}
static void test_pixel_output(void){
    Skin2Ui ui;Skin2View v={0},before;Skin2Canvas c;
    uint32_t sample, untouched;
    skin2_init(&ui);
    v.hp=88;v.hp_max=100;v.mp=23;v.mp_max=40;v.gold=1729;v.level=4;
    v.item_count=2;memcpy(v.items[0].name,"ESPADA",7);v.items[0].count=1;
    memcpy(v.items[1].name,"POCION",7);v.items[1].count=3;
    before=v;
    assert(skin2_canvas_init(&c,buf,320,200,320));
    fill(0xff315579u,320,200);
    ui.screen=SKIN2_GAME;
    skin2_draw(&ui,&v,&c);
    assert(buf[100*320+160]==0xff315579u); /* transparent world center */
    assert(buf[6*320+8]!=0xff315579u); /* HUD */
    assert(memcmp(&before,&v,sizeof(v))==0);
    sample=buf[100*320+160];
    ui.screen=SKIN2_INVENTORY;skin2_draw(&ui,&v,&c);
    assert(buf[100*320+160]!=sample);
    /* Separate UI 16:10 coordinates over 16:9 physical frame -> sidebars untouched. */
    assert(skin2_canvas_init(&c,buf,1280,720,1280));
    fill(0xff315579u,1280,720);
    skin2_draw(&ui,&v,&c);
    untouched=0xff315579u;
    assert(c.vx==64&&c.vw==1152&&c.vy==0&&c.vh==720);
    assert(buf[360*1280+0]==untouched);
    assert(buf[360*1280+1279]==untouched);
    assert(buf[360*1280+640]!=untouched);
    assert(memcmp(&before,&v,sizeof(v))==0);
    printf("PASS UI2: alpha, no-touch-world, inventario, 320x200, 1280x720 letterbox, estado inmutable\n");
}
int main(void){test_alpha_and_bounds();test_navigation();test_pixel_output();return 0;}
