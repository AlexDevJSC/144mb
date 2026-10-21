#ifndef SKIN_UI2_HEADER_H
#define SKIN_UI2_HEADER_H
/* Skin UI2: presentation-only toolkit, pure C11; no Bones/Eye/Windows includes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SKIN_UI2_VERSION 1u
#define SKIN_UI2_W 320
#define SKIN_UI2_H 200
#define SKIN_UI2_SLOTS 20
#define SKIN_UI2_ITEM_NAME 24

typedef enum { SKIN2_MAIN, SKIN2_GAME, SKIN2_PAUSE, SKIN2_INVENTORY, SKIN2_SETTINGS, SKIN2_LAN, SKIN2_CONTEXT } Skin2Screen;
typedef enum { SKIN2_UP, SKIN2_DOWN, SKIN2_LEFT, SKIN2_RIGHT, SKIN2_ACCEPT, SKIN2_BACK, SKIN2_OPEN_INV, SKIN2_INTERACT } Skin2Key;
typedef enum { SKIN2_NO_ACTION, SKIN2_START, SKIN2_LOAD, SKIN2_SAVE, SKIN2_QUIT, SKIN2_MAIN_REQUEST,
    SKIN2_RESUME, SKIN2_APPLY_SETTINGS, SKIN2_SINGLE, SKIN2_INTERACT_REQUEST } Skin2Action;

/* 0xAARRGGBB values; straight alpha; on little-endian machines bytes = B,G,R,A. */
typedef struct {
    uint32_t shadow, panel, paper, border_dark, border_light, text, muted,
             accent, selected, health, mana, success, danger;
} Skin2Theme;

typedef struct { char name[SKIN_UI2_ITEM_NAME]; uint16_t count; uint16_t icon; } Skin2Item;
/* This is read-only presentation data supplied by the host, NOT gameplay state. */
typedef struct {
    uint32_t hp, hp_max, mp, mp_max, gold, level;
    uint32_t item_count;
    Skin2Item items[SKIN_UI2_SLOTS];
    uint64_t tick;
    uint32_t actor_count;
    uint32_t available; /* Explicitly confirmed RPG fields only; 0 means no RPG data. */
} Skin2View;

#define SKIN2_AVAIL_HP 1u
#define SKIN2_AVAIL_MP 2u
#define SKIN2_AVAIL_GOLD 4u
#define SKIN2_AVAIL_INVENTORY 8u
typedef struct {
    Skin2Screen screen, previous;
    Skin2Theme theme;
    uint8_t selection;
    uint8_t ui_scale_setting; /* settings panel model; host decides when to apply */
    char notice[44];
    uint64_t target_id;
    uint8_t target_valid; /* Host/Bones is the ONLY authority on range and target. */
    char target_label[32];
} Skin2Ui;

/* Destination framebuffer belongs to application compositor, not Eye. No depth buffer. */
typedef struct {
    uint32_t *pixels;
    int width, height, stride; /* stride in pixels, >= width */
    int vx, vy, vw, vh;      /* separate virtual UI viewport; 16:10 letterbox */
} Skin2Canvas;

void skin2_init(Skin2Ui *ui);
Skin2Action skin2_input(Skin2Ui *ui, Skin2Key key);
void skin2_notice(Skin2Ui *ui, const char *notice);
void skin2_enter_main(Skin2Ui *ui);
void skin2_enter_game(Skin2Ui *ui); /* Host ACK after starting game, not speculative. */
void skin2_set_target(Skin2Ui *ui, uint64_t id, int host_valid, const char *label);
int skin2_blocks_gameplay(const Skin2Ui *ui);
bool skin2_canvas_init(Skin2Canvas *canvas, uint32_t *pixels, int width, int height, int stride);
/* UI is composed AFTER world, BEFORE final present; Eye's Z-buffer is never passed. */
void skin2_draw(const Skin2Ui *ui, const Skin2View *view, Skin2Canvas *canvas);
/* Host-owned 32-bit framebuffer, alpha SRC_OVER with rounding; 0 allocations/frame. */
void skin2_fill(Skin2Canvas *canvas, int x, int y, int w, int h, uint32_t argb);
/* For regression/benchmarks; no heap allocated by Skin2. */
size_t skin2_state_bytes(void);
#endif
