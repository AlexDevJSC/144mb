#include "brain/brain_interaction.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    BrainNpc n={0};BrainNpcInteractionIntent out={0};
    n.id=42;n.role=BRAIN_ROLE_MERCHANT;n.health_percent=100;
    assert(brain_npc_interaction_offer(&n,&out)==BRAIN_OK);
    assert(out.npc_id==42 && out.action==BRAIN_NPC_REPLY_GREETING);
    assert(brain_npc_interaction_offer(NULL,&out)==BRAIN_ERR_ARGUMENT);
    assert(brain_npc_interaction_offer(&n,NULL)==BRAIN_ERR_ARGUMENT);
    n.id=0;assert(brain_npc_interaction_offer(&n,&out)==BRAIN_ERR_ARGUMENT);
    puts("PASS Brain interaction pure intent (no E binding / no UI / no target selection)");
    return 0;
}
