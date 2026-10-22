/* Pixel-exact UI regression: no footer/button overlap or clipped HUD rows.
 * Uses only Skin ModKit; no Eye, Bones physics, or operating-system renderer. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../../BONES/modules/skin/skin_modkit.h"
static uint32_t a[320u*200u],b[320u*200u];
static void paint(Skin2Ui *ui,uint32_t *p){
    Skin2Canvas canvas;Skin2View view={0};
    for(unsigned i=0;i<320u*200u;i++)p[i]=0xff203040u;
    assert(skin2_canvas_init(&canvas,p,320,200,320));
    skin2_draw(ui,&view,&canvas);
}
static int differing_rect(int l,int top,int r,int bottom){
    int n=0;
    for(int y=top;y<bottom;y++)for(int x=l;x<r;x++)
        n+=(a[y*320+x]!=b[y*320+x]);
    return n;
}
int main(void){
    Skin2Ui ui;
    skin2_init(&ui);
    paint(&ui,a);
    for(int i=0;i<4;i++)skin2_input(&ui,SKIN2_DOWN);
    assert(ui.selection==4);
    paint(&ui,b);
    /* The last main-menu item changes, but the footer MUST NOT change. */
    assert(differing_rect(66,143,254,161)>0);
    assert(differing_rect(55,170,267,184)==0);
    skin2_enter_game(&ui);
    skin2_input(&ui,SKIN2_BACK);
    assert(ui.screen==SKIN2_PAUSE);
    paint(&ui,a);
    for(int i=0;i<3;i++)skin2_input(&ui,SKIN2_DOWN);
    assert(ui.selection==3);
    paint(&ui,b);
    assert(differing_rect(66,121,254,139)>0);
    assert(differing_rect(55,170,267,184)==0);
    /* Enlarged HUD should leave the last scanline unchanged; there must be
       at least one actual UI pixel on row 184 so the check is meaningful. */
    skin2_enter_game(&ui);
    ui.ui_scale_setting=1;
    paint(&ui,a);
    for(unsigned x=0;x<320;x++)assert(a[199u*320u+x]==0xff203040u);
    assert(a[184u*320u+11u]!=0xff203040u);
    puts("PASS Skin pixel layout: main/pause footer separate, 2x HUD inside 320x200");
    return 0;
}