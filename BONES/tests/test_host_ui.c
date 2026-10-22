/* Actual Eye+Skin 0.4 ModKit integration in one host, without a window.
   Does not prove GDI or human visual interaction. */
#include "../include/bones/modkit_host.h"
#include "../modules/skin/skin_modkit.h"
#include "../modules/eye/eye_modkit_ext.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t base[EYE_DISPLAY_PIXELS],composed[EYE_DISPLAY_PIXELS];
static int different(const uint32_t *a,const uint32_t *b) {
    size_t i;
    for(i=0;i<EYE_DISPLAY_PIXELS;i++)if(a[i]!=b[i])return 1;
    return 0;
}
int main(void) {
    BonesFoundation foundation;
    BonesModHost *h;
    BonesModule house;
    BonesInput idle={0};
    Skin2View view={0};
    const Skin2Ui *ui;
    const uint32_t *eye_pixels;
    uint32_t sample0;
    uint64_t tick;
    assert(bones_foundation_init(&foundation,0x1440001u,0x01442026u,1u));
    assert(bones_house_register_colliders(&foundation.engine.world));
    house=bones_test_house_module();
    assert(bones_register_module(&foundation.engine,&house));
    h=bones_mod_host_create(&foundation);assert(h);
    assert(bones_mod_register_builtins(h)==BONES_MOD_OK);
    assert(bones_mod_host_start(h)==BONES_MOD_OK);
    ui=bones_skin_ui_state();assert(ui&&ui->screen==SKIN2_MAIN);
    assert(bones_skin_ui_blocks_gameplay());
    assert(bones_eye_mod_prepare_base(base,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_GENERATED,128u));
    memcpy(composed,base,sizeof(base));
    assert(bones_skin_ui_compose(composed,320,200,320)==BONES_MOD_OK);
    assert(different(base,composed));
    /* Real UI request, host ACK after world initialization. */
    assert(bones_skin_ui_input(SKIN2_ACCEPT)==SKIN2_START);
    assert(bones_skin_ui_enter_game()==BONES_MOD_OK);
    assert(!bones_skin_ui_blocks_gameplay());
    assert(bones_mod_host_step(h,&idle)==BONES_MOD_OK);
    assert(bones_mod_host_frame(h)==BONES_MOD_OK);
    eye_pixels=bones_eye_mod_pixels();assert(eye_pixels);
    sample0=eye_pixels[12345];
    assert(bones_eye_mod_prepare_base(base,EYE_DISPLAY_PIXELS,EYE_DISPLAY_GAME,0u));
    view.tick=foundation.engine.tick;
    view.actor_count=foundation.entity_count;
    view.available=0; /* Never invent HP/MP/items/gold */
    assert(bones_skin_ui_update(&view)==BONES_MOD_OK);
    memcpy(composed,base,sizeof(base));
    assert(bones_skin_ui_compose(composed,320,200,320)==BONES_MOD_OK);
    assert(different(base,composed));
    assert(eye_pixels[12345]==sample0); /* Skin never owns Eye's frame */
    assert(bones_skin_ui_input(SKIN2_OPEN_INV)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_state()->screen==SKIN2_INVENTORY);
    assert(bones_skin_ui_blocks_gameplay());
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_state()->screen==SKIN2_GAME);
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_NO_ACTION);
    assert(bones_skin_ui_state()->screen==SKIN2_PAUSE);
    assert(bones_eye_mod_prepare_base(base,EYE_DISPLAY_PIXELS,EYE_DISPLAY_MENU_DIM_SCENE,128));
    tick=foundation.engine.tick;
    memcpy(composed,base,sizeof(base));
    assert(bones_skin_ui_compose(composed,320,200,320)==BONES_MOD_OK);
    assert(different(base,composed));
    assert(foundation.engine.tick==tick);
    assert(bones_skin_ui_input(SKIN2_BACK)==SKIN2_RESUME);
    assert(bones_skin_ui_state()->screen==SKIN2_GAME);
    bones_mod_host_destroy(h);
    puts("PASS: real Eye+Skin modules menu/game/hud/inventory/pause/composition");
    return 0;
}
