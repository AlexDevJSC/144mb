/* Host/Client LAN loopback using actual Mouth ModKit plus Skin status.
   This verifies transport and authoritative Bones actor; not a 3D GUI test. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "../include/bones/modkit_host.h"
#include "../modules/mouth/mouth_lan.h"
#include "../modules/skin/skin_modkit.h"
#include "../src/demo/test_house.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
static void delay_ms(unsigned ms){Sleep(ms);}
#else
#include <time.h>
static void delay_ms(unsigned ms){
    struct timespec ts;ts.tv_sec=ms/1000;ts.tv_nsec=(long)(ms%1000)*1000000L;
    (void)nanosleep(&ts,NULL);
}
#endif
int main(void) {
    BonesFoundation f;
    BonesModHost *h;
    BonesModule house;
    MouthLan host={0},client={0};
    MouthLanConfig cfg={0},join={0};
    MouthSessionInfo status={0};
    MouthLanActor actors[MOUTH_LAN_MAX_ACTORS]={0};
    BonesInput idle={0};
    uint64_t npc=0,remote=0,tick=0;
    int seen=0,k;
    assert(bones_foundation_init(&f,0x1440001u,0x01442026u,1u));
    assert(bones_house_register_colliders(&f.engine.world));
    assert(bones_foundation_spawn(&f,BONES_KIND_NPC,0,(BonesVec3){1.35f,0,0},&npc));
    assert(bones_foundation_spawn(&f,BONES_KIND_PLAYER,2,(BonesVec3){-1.25f,0,-3},&remote));
    cfg.world_id=f.world_id;
    cfg.seed=f.engine.world.seed;cfg.generator_version=BONES_GENERATOR_VERSION;
    cfg.port=14993;cfg.remote_actor_id=remote;cfg.remote_owner=2;
    assert(mouth_session_create(&host,&cfg)==MOUTH_SESSION_OK);
    house=bones_test_house_module();
    assert(bones_register_module(&f.engine,&house));
    h=bones_mod_host_create(&f);assert(h);
    assert(bones_mod_register_builtins(h)==BONES_MOD_OK);
    assert(bones_mod_host_start(h)==BONES_MOD_OK);
    join.world_id=cfg.world_id;join.seed=cfg.seed;
    join.generator_version=cfg.generator_version;join.port=cfg.port;
    assert(mouth_session_join(&client,&join,"127.0.0.1")==MOUTH_SESSION_OK);
    for(k=0;k<300;k++) {
        mouth_lan_client_controls(&client,1,0,0,0,0);
        (void)mouth_session_poll(&client);
        assert(bones_mod_host_step(h,&idle)==BONES_MOD_OK);
        (void)mouth_session_poll(&client);
        if(mouth_session_copy_snapshot(&client,actors,&tick)==MOUTH_SESSION_OK) {
            assert(actors[0].actor_id==1);
            assert(actors[1].actor_id==remote && actors[1].owner==2);
            seen=1;
        }
        delay_ms(8);
    }
    assert(seen);
    status.size=sizeof(status);
    assert(mouth_session_get_status(&client,&status)==MOUTH_SESSION_OK);
    assert(status.phase==MOUTH_SESSION_CONNECTED);
    assert(status.bytes_received>0);
    assert(bones_skin_ui_lan_status(status.phase,status.error,status.port,1)==BONES_MOD_OK);
    assert(bones_skin_ui_state()->lan_phase==MOUTH_SESSION_CONNECTED);
    assert(fabsf(f.engine.player.feet.x)<.001f && fabsf(f.engine.player.feet.z+7.f)<.001f);
    assert(fabsf(bones_foundation_find(&f,remote)->body.feet.z+3.f)>.02f);
    assert(tick>0);
    (void)mouth_session_disconnect(&client);
    bones_mod_host_destroy(h);
    (void)mouth_session_disconnect(&host);
    printf("PASS LAN real host+client loopback, remote=%llu snapshot=%llu\n",
           (unsigned long long)remote,(unsigned long long)tick);
    return 0;
}
