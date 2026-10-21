/* Public session API regression, no GUI and no Windows-specific code.
   Starts/stops only local UDP sockets: Bones authoritative host starts later. */
#include "../include/mouth_lan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    MouthLan lan={0},other={0};
    MouthLanConfig cfg={0};
    MouthSessionInfo info={0};
    MouthLanActor snap[MOUTH_LAN_MAX_ACTORS];
    uint64_t tick=0;
    assert(MOUTH_SESSION_API_VERSION==1u);
    assert(mouth_session_get_status(&lan,&info)==MOUTH_SESSION_INVALID_ARGUMENT);
    info.size=sizeof(info);
    assert(mouth_session_get_status(&lan,&info)==MOUTH_SESSION_OK);
    assert(info.phase==MOUTH_SESSION_OFFLINE && !info.socket_open);
    assert(mouth_session_copy_snapshot(&lan,snap,&tick)==MOUTH_SESSION_NOT_READY);
    assert(mouth_session_create(&lan,&cfg)==MOUTH_SESSION_INVALID_ARGUMENT);
    cfg.world_id=UINT64_C(0x1440001);
    cfg.seed=UINT64_C(0x01442026);
    cfg.generator_version=1;cfg.port=14444;
    cfg.remote_actor_id=UINT64_C(4294967298);cfg.remote_owner=2;
    assert(mouth_session_create(&lan,&cfg)==MOUTH_SESSION_OK);
    info.size=sizeof(info);
    assert(mouth_session_get_status(&lan,&info)==MOUTH_SESSION_OK);
    assert(info.phase==MOUTH_SESSION_HOST_PREPARED && info.is_host && !info.socket_open);
    assert(info.remote_actor_id==cfg.remote_actor_id && info.remote_owner==2);
    assert(mouth_session_poll(&lan)==MOUTH_SESSION_NOT_READY);
    assert(mouth_session_join(&lan,&cfg,"127.0.0.1")==MOUTH_SESSION_BUSY);
    assert(mouth_session_disconnect(&lan)==MOUTH_SESSION_OK);
    assert(mouth_session_disconnect(&lan)==MOUTH_SESSION_OK); /* idempotent */
    info.size=sizeof(info);
    assert(mouth_session_get_status(&lan,&info)==MOUTH_SESSION_OK);
    assert(info.phase==MOUTH_SESSION_OFFLINE && !info.socket_open && !info.error);
    assert(mouth_session_join(&other,&cfg,"not_an_ip")==MOUTH_SESSION_INVALID_ADDRESS);
    assert(!other.socket_active);
    assert(mouth_session_join(&other,&cfg,"0.0.0.0")==MOUTH_SESSION_INVALID_ADDRESS);
    assert(mouth_session_join(&other,&cfg,"255.255.255.255")==MOUTH_SESSION_INVALID_ADDRESS);
    assert(mouth_session_join(&other,&cfg,"127.0.0.1")==MOUTH_SESSION_OK);
    info.size=sizeof(info);
    assert(mouth_session_get_status(&other,&info)==MOUTH_SESSION_OK);
    assert(info.phase==MOUTH_SESSION_JOINING && info.socket_open && !info.is_host);
    assert(mouth_session_create(&other,&cfg)==MOUTH_SESSION_BUSY);
    assert(mouth_session_copy_snapshot(&other,snap,&tick)==MOUTH_SESSION_NOT_READY);
    assert(mouth_session_disconnect(&other)==MOUTH_SESSION_OK);
    info.size=sizeof(info);
    assert(mouth_session_get_status(&other,&info)==MOUTH_SESSION_OK);
    assert(info.phase==MOUTH_SESSION_OFFLINE && !info.socket_open);
    puts("PASS Mouth session API: offline, create, join IP validation, disconnect, snapshots.");
    return 0;
}
