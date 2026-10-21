#ifndef SKIN_MODKIT_PORT_H
#define SKIN_MODKIT_PORT_H
/* Skin 0.3: project UI bridge. Standalone interface for app/game code.
   Call only from the engine's single-threaded main/render loop.
   Bones ModKit registration uses bones_mod_skin_query in modkit.c.
   No BonesEngine/Window/Eye/Z-buffer access or gameplay authority. */
#include "skin_ui2.h"
#include "../../include/bones/modkit.h"

int32_t bones_mod_skin_query(uint32_t abi, BonesModDescriptor *out);
/* Inputs are UI intents, not gameplay commands. Caller handles actions. */
Skin2Action bones_skin_ui_input(Skin2Key key);
/* Return nonzero while any menu overlays gameplay; host must zero BonesInput. */
int bones_skin_ui_blocks_gameplay(void);
/* Explicit ACK only after host has created/loaded the session. */
int32_t bones_skin_ui_enter_game(void);
int32_t bones_skin_ui_enter_main(void);
/* Bones decides target/range/validity; Skin does not raycast or collide. */
int32_t bones_skin_ui_target(uint64_t target_id,int host_valid,const char *label);
/* Host-supplied (read-only) RPG display facts; not inferred from position. */
int32_t bones_skin_ui_update(const Skin2View *view);
/* Called by the app AFTER Eye has rendered into an APP-OWNED copy of frame.
   Never pass Eye's const framebuffer or any depth buffer. */
int32_t bones_skin_ui_compose(uint32_t *pixels,int width,int height,int stride);
/* No dependency on Eye: host may read lightweight observed state. */
uint32_t bones_skin_ui_actor_count(void);
uint64_t bones_skin_ui_last_tick(void);
const Skin2Ui *bones_skin_ui_state(void);
#endif
