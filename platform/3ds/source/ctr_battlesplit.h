// The split battle layout (change battlesplit): the enemy and its grid on the top screen; the box, SOUL, attacks,
// text, HUD and buttons on the bottom.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctr_display.h"

// settings.ini battle_layout (and --ini battle_layout=N): 0 Original, 1 Split A, 2 Split C, 3 Top screen (the whole
// battle on the top screen: Original, with the game moved up for it when the player's game is below, and no step 3b
// move down with the game on top; ctr_layout.h's CTR_LAYOUT_BATTLE_TOP).
enum { CTR_BATTLE_LAYOUT_ORIGINAL = 0, CTR_BATTLE_LAYOUT_A = 1, CTR_BATTLE_LAYOUT_C = 2, CTR_BATTLE_LAYOUT_TOP = 3,
       CTR_BATTLE_LAYOUT_COUNT = 4 };

// Where the two layers land, in screen pixels.
typedef struct {
    int16_t bandRows; // the top shows application surface rows 0..bandRows-1 (native rows, 0.5 per room pixel)
    int16_t topDy;    // ... from top screen row topDy, centred across as today
    int16_t bottomDy; // the arena layer's row 0 is at bottom screen row bottomDy (column 0 at column 0)
} CtrLayoutPlace;

// The one layout function: a preset and the encounter's band height (native rows) to the places. False for
// Original or an unknown preset.
bool CtrLayout_place(int32_t layout, int32_t bandRows, CtrLayoutPlace* out);

// What the screens do, from the stored settings. Today each value is the setting as stored; a later "3DS mode"
// setting changes only this function (lead, 2026-11-07).
typedef struct {
    bool dialogueBottom;
    int32_t battleLayout;
} CtrLayoutPolicy;
void CtrLayout_policy(const CtrDisplaySettings* settings, CtrLayoutPolicy* out);

// The battle_layout in effect: the options menu's change, else --ini's, else settings.ini's.
int32_t CtrBattlesplit_setting(void);
// The options menu's change: in effect from the next frame (an admitted battle splits or stops), and written to
// settings.ini when the menu saves. Out-of-range values are ignored.
void CtrBattlesplit_setSetting(int32_t value);

// The renderer's side (ctr_renderer.c's end), for z_battlesplit.c.
struct Renderer;
bool CtrRenderer_bsAllowed(struct Renderer* renderer);
int32_t CtrRenderer_bsPrepare(struct Renderer* renderer, bool on);
bool CtrRenderer_bsBegun(struct Renderer* renderer);
void CtrRenderer_bsClearLayer(struct Renderer* renderer);
void CtrRenderer_bsLayer(struct Renderer* renderer, int32_t layer);
bool CtrRenderer_bsAppBound(struct Renderer* renderer);
void CtrRenderer_bsPresent(struct Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy);
uint32_t CtrRenderer_bsMoveGame(struct Renderer* renderer, bool below, uint32_t saved);
// Placement (c), the split default (appended blocks of both files): CtrRenderer_bsPresent with the bottom in two
// pieces when CtrBattlesplit_seam() names a seam row (0: one piece, (b)). CtrLayout_bottomSeam: true when the bottom
// is two pieces this frame; screen rows from *seamY down show the layer unlifted, rows above *seamY - *lift show it
// lifted as CtrLayout_bottomView says, and the rows between are black (design section 20).
void CtrRenderer_bsPresent5(struct Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy);
int32_t CtrBattlesplit_seam(void);
// The battle grid's strip (ctr_renderer.c, battlesplit14): its rows black before the grid's draw, copied to the top after.
bool CtrRenderer_bsStripClear14(struct Renderer* renderer, float x0, float y0, float x1, float y1);
void CtrRenderer_bsStripCopy14(struct Renderer* renderer, int32_t rows, int32_t topDy);
bool CtrLayout_bottomSeam(int32_t* seamY, int32_t* lift);
