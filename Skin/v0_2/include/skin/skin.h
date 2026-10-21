#ifndef SKIN_144_H
#define SKIN_144_H
/* Skin v0.1 : independent module for Bones v0.1 (C11). */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "bones/bones.h"

#define SKIN_VERSION 1u
#define SKIN_MAX_EVENTS 128u
#define SKIN_MAX_SAVE_BYTES 32768u

typedef struct { uint64_t hi,lo; } SkinWorldId;
typedef enum {
    SKIN_EVT_PLAYER_SAMPLE=1, SKIN_EVT_GROUND_CHANGED=2,
    SKIN_EVT_COLLIDER_ADDED=3, SKIN_EVT_COLLIDER_REMOVED=4,
    SKIN_EVT_CONFIRMED_ACTION=5, SKIN_EVT_COLLIDER_UPDATED=6
} SkinEventKind;
typedef struct {
    uint64_t sequence,tick,entity_id,argument;
    uint32_t kind;
} SkinEvent;

typedef struct {
    SkinWorldId world_id;
    uint64_t branch_id;
    uint64_t expected_seed;
    uint32_t expected_generator_version;
    uint64_t next_event_sequence;
    uint64_t save_generation;
    BonesPlayer last_player;
    uint64_t known_colliders[BONES_MAX_COLLIDERS];
    BonesCollider observed_colliders[BONES_MAX_COLLIDERS];
    uint32_t known_count,event_count;
    SkinEvent events[SKIN_MAX_EVENTS];
    bool observed,dirty,history_truncated;
} Skin;

bool skin_init(Skin *skin,const BonesEngine *engine,SkinWorldId id,uint64_t branch_id);
/* Register as a BonesModule before the first simulation tick. */
bool skin_attach(Skin *skin,BonesEngine *engine);
/* Skin watches gameplay after each confirmed Bones tick. No implicit I/O. */
void skin_observe(Skin *skin,const BonesEngine *engine);
/* Only call after Bones host has accepted the action. No generic merge semantics. */
bool skin_record_confirmed(Skin *skin,const BonesEngine *engine,uint64_t entity_id,uint64_t action_code);
/* A/B double-slot saves: slot with newest valid generation wins. */
bool skin_save(Skin *skin,const BonesEngine *engine,const char *base_path);
bool skin_load(Skin *skin,BonesEngine *engine,const char *base_path);
/* Utility validation for UI status: check whether either slot is recoverable. */
bool skin_has_valid_save(const Skin *skin,const BonesEngine *engine,const char *base_path);

/* UI: independent of Eye backend; overlay onto its 32bpp BGRA/RGBX framebuffer. */
typedef enum {SKIN_MENU_MAIN=0,SKIN_MENU_GAME=1,SKIN_MENU_PAUSE=2} SkinScreen;
typedef enum {SKIN_KEY_UP=1,SKIN_KEY_DOWN=2,SKIN_KEY_ACCEPT=3,SKIN_KEY_BACK=4} SkinKey;
typedef enum {
  SKIN_ACTION_NONE=0,SKIN_ACTION_START=1,SKIN_ACTION_RESUME=2,
  SKIN_ACTION_SAVE=3,SKIN_ACTION_LOAD=4,SKIN_ACTION_TO_MAIN=5,SKIN_ACTION_QUIT=6
} SkinAction;
typedef struct { uint32_t background,panel,edge,text,muted,accent,success,error; } SkinPalette;
typedef struct {
    SkinScreen screen;
    uint8_t selection;
    SkinPalette palette;
    char notice[80];
    bool notice_error;
} SkinUi;
void skin_ui_init(SkinUi *ui);
SkinAction skin_ui_key(SkinUi *ui,SkinKey key);
void skin_ui_notice(SkinUi *ui,const char *message,bool error);
/* Skips painting the world if asked to; non-destructive overlay in GAME mode. */
void skin_ui_draw(const SkinUi *ui,const BonesEngine *engine,uint32_t *bgra,int width,int height);
/* Substitute for unavailable final Eye artwork: intentionally placeholder. */
void skin_ui_placeholder_background(uint32_t *bgra,int width,int height,uint32_t tick);
#endif
