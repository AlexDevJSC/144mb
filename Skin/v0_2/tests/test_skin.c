#include "skin/skin.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static BonesEngine engine;
static Skin skin;
static const SkinWorldId id={UINT64_C(0x74cc5),UINT64_C(0x55110)};
static const char *base="build/skin_test_slot";
static void cleanup(void){remove("build/skin_test_slot.a");remove("build/skin_test_slot.b");remove("build/skin_test_slot.tmp");}
static void init(void){bones_init(&engine,0x1442026);assert(skin_init(&skin,&engine,id,9));assert(skin_attach(&skin,&engine));}
static void step(int n,int walking){BonesInput in={0};int i;in.forward=(uint8_t)walking;for(i=0;i<n;i++)assert(bones_step(&engine,&in,BONES_TICK_DT));}
static BonesCollider cube(uint64_t cid) {
    BonesCollider c={0};c.id=cid;c.solid=1;c.render_box=1;c.material=1;
    c.bounds.min=(BonesVec3){1,0,1};c.bounds.max=(BonesVec3){2,2,2};return c;
}
static void truncate_last_bytes(const char *path) {
    FILE *f=fopen(path,"rb");FILE *g;long sz;uint8_t buf[32768];
    assert(f);assert(fseek(f,0,SEEK_END)==0);sz=ftell(f);assert(sz>30);rewind(f);
    assert(fread(buf,1,(size_t)sz,f)==(size_t)sz);fclose(f);
    g=fopen(path,"wb");assert(g);assert(fwrite(buf,1,(size_t)sz-4,g)==(size_t)sz-4);assert(fclose(g)==0);
}
static void corrupt_last_byte(const char *path) {
    FILE *f=fopen(path,"rb+");int c;assert(f);
    assert(fseek(f,-1,SEEK_END)==0);c=fgetc(f);assert(c!=EOF);
    assert(fseek(f,-1,SEEK_END)==0);assert(fputc(c^0xff,f)!=EOF);assert(fclose(f)==0);
}
static void test_save_recovery(void) {
    BonesInput in={0};Skin other;uint64_t seq;
    const BonesCollider c=cube(400);
    cleanup();init();
    assert(!skin_has_valid_save(&skin,&engine,base));
    assert(bones_world_add_collider(&engine.world,&c));step(1,0);
    assert(skin.event_count==1&&skin.events[0].kind==SKIN_EVT_COLLIDER_ADDED);
    assert(skin_record_confirmed(&skin,&engine,400,123));
    step(60,1);assert(skin.event_count>=4);
    assert(skin_save(&skin,&engine,base));assert(skin.save_generation==1);
    assert(skin_has_valid_save(&skin,&engine,base));
    assert(bones_world_remove_collider(&engine.world,400));step(1,0);
    assert(skin_save(&skin,&engine,base));assert(skin.save_generation==2);
    seq=skin.next_event_sequence;
    assert(skin_load(&skin,&engine,base));assert(engine.world.collider_count==0);
    assert(skin.next_event_sequence==seq);
    corrupt_last_byte("build/skin_test_slot.b");
    assert(skin_load(&skin,&engine,base));assert(engine.world.collider_count==1);
    assert(skin.save_generation==1);
    assert(skin.next_event_sequence<seq);
    /* A broken latest save must not destroy the earlier valid one. */
    assert(skin_save(&skin,&engine,base));
    assert(skin.save_generation==2);
    /* Incompatible world/branch must not be accepted. */
    other=skin;other.world_id.lo++;
    assert(!skin_has_valid_save(&other,&engine,base));
    other=skin;other.branch_id++;
    assert(!skin_has_valid_save(&other,&engine,base));
    (void)in;
    cleanup();
}
static void test_seed_guard(void) {
    cleanup();init();
    engine.world.seed++;
    assert(!skin_save(&skin,&engine,base));
    assert(!skin_record_confirmed(&skin,&engine,99,12));
    engine.world.seed--;
    assert(skin_save(&skin,&engine,base));
    cleanup();
}
static void test_observation_and_overflow(void) {
    BonesCollider c=cube(222);unsigned i;
    init();
    assert(bones_world_add_collider(&engine.world,&c));step(1,0);
    assert(skin.event_count==1);
    step(1,0);assert(skin.event_count==1);
    engine.world.colliders[0].bounds.max.x+=1.0f;
    step(1,0);
    assert(skin.event_count==2 && skin.events[1].kind==SKIN_EVT_COLLIDER_UPDATED);
    assert(bones_world_remove_collider(&engine.world,222));step(1,0);
    assert(skin.event_count==3&&skin.events[2].kind==SKIN_EVT_COLLIDER_REMOVED);
    for(i=0;i<SKIN_MAX_EVENTS+10;i++)(void)skin_record_confirmed(&skin,&engine,222,1);
    assert(skin.event_count==SKIN_MAX_EVENTS);
    assert(skin.history_truncated);
    assert(skin.dirty);
}
static void test_transaction_and_corrupt(void) {
    Skin before;BonesEngine copy;
    cleanup();init();step(30,1);
    assert(skin_save(&skin,&engine,base));
    before=skin;copy=engine;
    corrupt_last_byte("build/skin_test_slot.a");
    assert(!skin_load(&skin,&engine,base));
    assert(memcmp(&skin,&before,sizeof(skin))==0);
    assert(memcmp(&engine,&copy,sizeof(engine))==0);
    cleanup();
    assert(skin_save(&skin,&engine,base));
    before=skin;copy=engine;
    truncate_last_bytes("build/skin_test_slot.a");
    assert(!skin_load(&skin,&engine,base));
    assert(memcmp(&skin,&before,sizeof(skin))==0);
    assert(memcmp(&engine,&copy,sizeof(engine))==0);
    cleanup();
}
static void write_ppm(const char *path,const uint32_t *pix,int w,int h) {
    FILE *f=fopen(path,"wb");int i;assert(f);
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(i=0;i<w*h;i++) {
        uint32_t c=pix[i];unsigned char b[3]={(uint8_t)(c>>16),(uint8_t)(c>>8),(uint8_t)c};
        assert(fwrite(b,1,3,f)==3);
    }
    assert(fclose(f)==0);
}
static void test_ui(void) {
    static uint32_t pixels[320*200];SkinUi ui; SkinAction a;
    bones_init(&engine,1);skin_ui_init(&ui);
    skin_ui_placeholder_background(pixels,320,200,0);
    skin_ui_draw(&ui,&engine,pixels,320,200);
    write_ppm("build/skin_menu.ppm",pixels,320,200);
    a=skin_ui_key(&ui,SKIN_KEY_DOWN);assert(a==SKIN_ACTION_NONE&&ui.selection==1);
    a=skin_ui_key(&ui,SKIN_KEY_ACCEPT);assert(a==SKIN_ACTION_LOAD);
    a=skin_ui_key(&ui,SKIN_KEY_UP);assert(a==SKIN_ACTION_NONE&&ui.selection==0);
    a=skin_ui_key(&ui,SKIN_KEY_ACCEPT);assert(a==SKIN_ACTION_START&&ui.screen==SKIN_MENU_GAME);
    skin_ui_placeholder_background(pixels,320,200,2);
    skin_ui_draw(&ui,&engine,pixels,320,200);
    write_ppm("build/skin_hud.ppm",pixels,320,200);
    skin_ui_key(&ui,SKIN_KEY_BACK);assert(ui.screen==SKIN_MENU_PAUSE);
    skin_ui_key(&ui,SKIN_KEY_DOWN);a=skin_ui_key(&ui,SKIN_KEY_ACCEPT);
    assert(a==SKIN_ACTION_SAVE);
    skin_ui_placeholder_background(pixels,320,200,4);
    skin_ui_draw(&ui,&engine,pixels,320,200);
    write_ppm("build/skin_pausa.ppm",pixels,320,200);
    skin_ui_key(&ui,SKIN_KEY_BACK);assert(ui.screen==SKIN_MENU_GAME);
    printf("PASS UX: menu principal, HUD, pausa y navegacion\n");
}
int main(void) {
    test_seed_guard();puts("PASS VALIDACION: cambio de semilla bloqueado");
    test_save_recovery();puts("PASS SAVE: slots A/B, checksum, recuperacion, IDs, eventos");
    test_observation_and_overflow();puts("PASS EVENT: deteccion y limite visible");
    test_transaction_and_corrupt();puts("PASS ATOMICIDAD LOGICA: rechazo de carga sin modificar estado");
    test_ui();
    return 0;
}
