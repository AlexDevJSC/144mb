/* Console LAN demo: Bones-authoritative host, remote keyboard client.
   Usage: mouth_lan_demo host [port] | mouth_lan_demo join IPV4 [port]
   MOUTH_DEMO_SECONDS=10 exits automatically (useful for smoke tests).
   No textures are transmitted: this is a network/physics prototype. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "bones/modkit_host.h"
#include "../include/mouth_lan.h"
/* Uses the installed Bones/modules/mouth/modkit.c, not a separate copy. */
extern int32_t bones_mod_mouth_query(uint32_t, BonesModDescriptor *);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define DEMO_WORLD_ID UINT64_C(0x0000000000014401)
#define DEMO_SEED UINT64_C(0x01442026)

#ifdef _WIN32
static int held(int key){return (GetAsyncKeyState(key)&0x8000)!=0;}
#endif
static void delay_ms(unsigned ms){
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;ts.tv_sec=ms/1000u;ts.tv_nsec=(long)(ms%1000u)*1000000L;
    nanosleep(&ts,NULL);
#endif
}
static void local_controls(BonesInput *input){
    memset(input,0,sizeof(*input));
#ifdef _WIN32
    input->forward=(uint8_t)held('W');input->backward=(uint8_t)held('S');
    input->left=(uint8_t)held('A');input->right=(uint8_t)held('D');
    input->yaw_left=(uint8_t)held(VK_LEFT);input->yaw_right=(uint8_t)held(VK_RIGHT);
    input->sprint=(uint8_t)held(VK_SHIFT);input->jump=(uint8_t)held(VK_SPACE);
#endif
}
static void remote_controls(MouthLan *l){
#ifdef _WIN32
    /* Test-only automation; normal Windows users keep keyboard controls. */
    if(getenv("MOUTH_DEMO_AUTOMOVE") && strcmp(getenv("MOUTH_DEMO_AUTOMOVE"),"1")==0){
        mouth_lan_client_controls(l,1,0,0,0,0);return;
    }
    int f=held('W')-held('S'),s=held('D')-held('A');
    int yaw=30*(held(VK_RIGHT)-held(VK_LEFT));
    mouth_lan_client_controls(l,f,s,yaw,held(VK_SHIFT),held(VK_SPACE));
#else
    /* POSIX headless smoke test advances the remote player. */
    mouth_lan_client_controls(l,1,0,0,0,0);
#endif
}
static int parse_port(const char *s,uint16_t *port){
    char *end=NULL;unsigned long p;
    if(!s||!*s)return 0;
    p=strtoul(s,&end,10);
    if(!end||*end||p==0||p>65535)return 0;
    *port=(uint16_t)p;return 1;
}
int main(int argc,char **argv){
    MouthLan *lan;MouthLanConfig cfg;
    BonesFoundation *foundation=NULL;BonesModHost *host=NULL;
    uint64_t last_ms,last_print,start_ms,tick_acc=0;
    uint16_t port=MOUTH_LAN_PORT;
    unsigned long demo_seconds=0;
    const char *seconds=getenv("MOUTH_DEMO_SECONDS");
    int is_host,done=0,ok=1;
    if(seconds&&*seconds)demo_seconds=strtoul(seconds,NULL,10);
    if(argc<2 || (strcmp(argv[1],"host")!=0 && strcmp(argv[1],"join")!=0)){
        fprintf(stderr,"Usage: mouth_lan_demo host [port] | join IPV4 [port]\n");
        return 2;
    }
    is_host=strcmp(argv[1],"host")==0;
    if(is_host&&argc>=3&&!parse_port(argv[2],&port))return 2;
    if(!is_host&&(argc<3||(argc>=4&&!parse_port(argv[3],&port))))return 2;
    lan=(MouthLan*)calloc(1,sizeof(*lan));
    if(!lan){fprintf(stderr,"Out of memory\n");return 3;}
    memset(&cfg,0,sizeof(cfg));
    cfg.world_id=DEMO_WORLD_ID;cfg.seed=DEMO_SEED;
    cfg.generator_version=BONES_GENERATOR_VERSION;cfg.port=port;
    if(is_host){
        uint64_t actor_id=0;
        foundation=(BonesFoundation*)calloc(1,sizeof(*foundation));
        if(!foundation||!bones_foundation_init(foundation,cfg.world_id,cfg.seed,1)||
           !bones_foundation_spawn(foundation,BONES_KIND_PLAYER,2,
                                   (BonesVec3){1.8f,0.0f,-4.0f},&actor_id)){
            fprintf(stderr,"Bones init/spawn failed\n");ok=0;goto cleanup;
        }
        cfg.remote_actor_id=actor_id;cfg.remote_owner=2;
        if(!mouth_lan_configure_host(lan,&cfg)){ok=0;goto cleanup;}
        host=bones_mod_host_create(foundation);
        if(!host||bones_mod_host_add(host,bones_mod_mouth_query)!=BONES_MOD_OK||
           bones_mod_host_start(host)!=BONES_MOD_OK){
            fprintf(stderr,"ModKit/Mouth startup failed (UDP port in use?)\n");
            ok=0;goto cleanup;
        }
        printf("Bones LAN host on UDP %u. Remote actor=%llu. Ctrl+C exits.\n",
               (unsigned)port,(unsigned long long)actor_id);
    }else{
        if(!mouth_lan_client_open(lan,&cfg,argv[2])){
            fprintf(stderr,"Cannot open UDP link to %s:%u\n",argv[2],(unsigned)port);
            ok=0;goto cleanup;
        }
        printf("Mouth LAN client -> %s:%u. Ctrl+C exits.\n",argv[2],(unsigned)port);
    }
    printf("Windows: WASD move; left/right turn; Shift sprint; Space jump.\n");
    last_ms=mouth_lan_clock_ms();last_print=last_ms;start_ms=last_ms;
    /* Fixed tick 60 Hz, independent of render. The accumulator uses 60*ms. */
    while(!done){
        uint64_t now=mouth_lan_clock_ms(),elapsed=now-last_ms;
        int steps=0;
        if(elapsed>250u)elapsed=250u;
        last_ms=now;tick_acc+=elapsed*60u;
        while(tick_acc>=1000u&&steps<5){
            tick_acc-=1000u;steps++;
            if(is_host){
                BonesInput input;local_controls(&input);
                if(bones_mod_host_step(host,&input)!=BONES_MOD_OK){ok=0;done=1;break;}
            }else{
                remote_controls(lan);mouth_lan_client_poll(lan);
            }
        }
        if(tick_acc>=1000u)tick_acc=0; /* Avoid the spiral of death. */
        if(now-last_print>=1000u){
            last_print=now;
            if(is_host){
                const BonesEntity *a=bones_foundation_find(foundation,1);
                const BonesEntity *b=bones_foundation_find(foundation,cfg.remote_actor_id);
                if(a&&b)printf("HOST tick=%llu status=%d | p1(%.2f %.2f) p2(%.2f %.2f) bytes(tx/rx)=%llu/%llu\n",
                    (unsigned long long)foundation->engine.tick,(int)lan->status,
                    a->body.feet.x,a->body.feet.z,b->body.feet.x,b->body.feet.z,
                    (unsigned long long)lan->bytes_sent,(unsigned long long)lan->bytes_received);
            }else if(lan->status==MOUTH_LAN_REJECTED){
                fprintf(stderr,"Host rejected incompatible world/seed/generator.\n");
                ok=0;done=1;
            }else if(lan->actor_count==2){
                printf("CLIENT tick=%llu p1(%.2f %.2f) p2(%.2f %.2f) bytes(tx/rx)=%llu/%llu\n",
                    (unsigned long long)lan->last_snapshot_tick,
                    lan->actors[0].x,lan->actors[0].z,
                    lan->actors[1].x,lan->actors[1].z,
                    (unsigned long long)lan->bytes_sent,(unsigned long long)lan->bytes_received);
            }else{
                printf("CLIENT waiting status=%d UDP sent=%llu\n",(int)lan->status,
                    (unsigned long long)lan->bytes_sent);
            }
            fflush(stdout);
        }
        if(demo_seconds&&now-start_ms>=((uint64_t)demo_seconds*1000u))done=1;
        delay_ms(1);
    }
cleanup:
    if(host)bones_mod_host_destroy(host);
    mouth_lan_close(lan);
    free(foundation);free(lan);
    return ok?0:1;
}
