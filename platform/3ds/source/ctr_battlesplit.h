// The split battle layout (change battlesplit): the enemy and its grid on the top screen; the box, SOUL, attacks,
// text, HUD and buttons on the bottom.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctr_display.h"

// settings.ini battle_layout (and --ini battle_layout=N): 0 Original, 1 Split A, 2 Split C, 3 Top screen (the whole
// battle on the top screen: Original, with the game moved up for it when the player's game is below, and no step 3b
// move down with the game on top; ctr_layout.h's CTR_LAYOUT_BATTLE_TOP). Top screen left the menu with the 3DS
// Options page (optplan, maintainer 11-15), so 3 is refused like any value out of range: a release build loads Original, a
// dev build its default (devsplit's Split; dev builds are lab-only). Its code paths stay, unreached.
enum { CTR_BATTLE_LAYOUT_ORIGINAL = 0, CTR_BATTLE_LAYOUT_A = 1, CTR_BATTLE_LAYOUT_C = 2, CTR_BATTLE_LAYOUT_TOP = 3,
       CTR_BATTLE_LAYOUT_COUNT = 3 };

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
// The Split's flash (change battlesplit2, 11-14; z_battlesplit.c's and ctr_renderer.c's ends): a split frame's cover draws
// that went on both layers, their colour (0xBBGGRR) and alpha as the game drew them, in draw order. Present
// (CtrRenderer_bsPresentFlash) puts them, in that order, on the top screen's rows and columns outside the band.
#define CTR_BS_FLASH_MAX 4
typedef struct {
    uint32_t color[CTR_BS_FLASH_MAX];
    float alpha[CTR_BS_FLASH_MAX];
    // Each draw's place in the renderer's ring (CtrRenderer_bsFlashMark before and after it): from and gen[0] before, to
    // and gen[1] after (gen: vertexCount - vertexUsed, which changes only when the ring starts over). gen[0] != gen[1]:
    // the ring started over during the draw, its vertices from from to the old part's end and from 0 to to.
    uint32_t from[CTR_BS_FLASH_MAX], to[CTR_BS_FLASH_MAX], gen[CTR_BS_FLASH_MAX][2];
    uint32_t n;
} CtrBsFlash;
void CtrRenderer_bsFlashMark(struct Renderer* renderer, uint32_t mark[2]);
void CtrRenderer_bsPresentFlash(struct Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy, CtrBsFlash* flash);
// After it, the same notes on the bottom screen's rows that present drew nothing on (the lift's empty rows: change
// battlesplit2, 11-15). z_battlesplit.c presents through this one.
void CtrRenderer_bsPresentGap(struct Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy, CtrBsFlash* flash);
// The hinge rows (change battlesplit2, 11-15; z_battlesplit.c's end, UNDERTALE only): the rows of the screens' pitch between
// the top screen's last row and the bottom screen's first on this console: settings.ini's hinge_rows (0 to 160) as read
// at boot, else the model's (the hinge table; 0 for a model it does not know).
int CtrBattlesplit_hingeRows(void);
