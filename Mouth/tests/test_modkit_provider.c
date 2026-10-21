/* Bones 0.4 ModKit ABI1 + Mouth drop-in provider compatibility. */
#include "../include/mouth_lan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The entry actually discovered by scripts/gen_modkit_registry.py. */
extern int32_t bones_mod_mouth_query(uint32_t, BonesModDescriptor *);

int main(void) {
    BonesModDescriptor d;
    MouthLan context;
    MouthLanConfig cfg;
    memset(&d, 0, sizeof(d));
    d.size = (uint32_t)sizeof(d);
    assert(bones_mod_mouth_query(BONES_MOD_ABI+1, &d)==BONES_MOD_EINVAL);
    assert(bones_mod_mouth_query(BONES_MOD_ABI, &d)==BONES_MOD_OK);
    assert(d.abi==BONES_MOD_ABI && d.size==sizeof(d));
    assert(d.role==BONES_ROLE_MOUTH);
    assert(d.implementation_version>=3 && d.state_schema_version==0);
    assert(d.start==NULL && d.before_tick==NULL && d.after_tick==NULL);
    assert(d.requested_permissions==0); /* No LAN => no socket, no mutation. */
    memset(&cfg, 0, sizeof(cfg));
    cfg.world_id=UINT64_C(0x1440001);
    cfg.seed=UINT64_C(0x01442026);
    cfg.generator_version=1;cfg.port=14444;
    cfg.remote_actor_id=UINT64_C(4294967298);cfg.remote_owner=2;
    assert(mouth_lan_configure_host(&context, &cfg));
    memset(&d, 0, sizeof(d));d.size=sizeof(d);
    assert(bones_mod_mouth_query(BONES_MOD_ABI, &d)==BONES_MOD_OK);
    assert(d.role==BONES_ROLE_MOUTH && d.start && d.stop &&
           d.before_tick && d.after_tick);
    assert((d.requested_permissions & BONES_PERMISSION_SEND_COMMANDS)!=0);
    assert((d.requested_permissions & BONES_PERMISSION_READ_ACTORS)!=0);
    assert((d.requested_permissions & BONES_PERMISSION_READ_TIME)!=0);
    assert(d.requested_permissions & BONES_PERMISSION_READ_SNAPSHOT ? 0 : 1);
    /* The module does not start a UDP socket before a verified Bones host. */
    assert(!context.socket_active);
    mouth_lan_close(&context);
    puts("PASS: Bones ABI1 Mouth provider offline/online descriptors.");
    return 0;
}
