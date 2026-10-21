/* Examples only. These are NOT network, AI, save or gameplay implementations.
   A module can use these service entry points without editing engine internals. */
#include "../include/bones/foundation.h"

/* Brain issues an INPUT request for its NPC for NEXT tick. */
bool example_brain_request_move(BonesFoundation *f,uint64_t npc_id,uint64_t serial) {
    BonesCommand c={0};
    if(!f) return false;
    c.actor_id=npc_id;c.issuer=0;c.sequence=serial;
    c.tick=f->engine.tick+1;c.forward=1;
    return bones_foundation_submit(f,&c);
}
/* Mouth MUST authenticate identity before passing the already-validated
   issuer ID. The foundation rejects impersonation and stale sequence/tick. */
bool example_mouth_host_request(BonesFoundation *f,
                                const BonesCommand *decoded_from_peer,
                                uint32_t authenticated_owner) {
    BonesCommand c;
    if(!f || !decoded_from_peer) return false;
    c=*decoded_from_peer;c.issuer=authenticated_owner;
    return bones_foundation_submit(f,&c);
}
/* Skin owns disk I/O, version migrations, durability and fusion.
   Bones supplies only canonical state bytes + pending facts. */
bool example_skin_capture(const BonesFoundation *f,uint8_t *buffer,
                          size_t capacity,size_t *written) {
    return bones_foundation_export(f,buffer,capacity,written);
}
/* Ear can read spatial positions; Eye can turn them into draw commands.
   Neither should mutate the entity or physics through const pointers. */
const BonesEntity *example_eye_ear_query_actor(const BonesFoundation *f,uint64_t id) {
    return bones_foundation_find(f,id);
}
