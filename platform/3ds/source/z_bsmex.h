// Mettaton EX in Split (change battlesplit, UNDERTALE): scene placement mode 8, called from z_battlesplit.c's frame
// (bsClearLayerF, after bsgroup's). z_bsmex.c has the design.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctr_battlesplit.h"
#include "renderer.h"

#define CTR_BSMEX_MODE 8  // the scene placement mode this file places (groups 80 and 81)
#define CTR_BSMEX_WHOLE 8 // the split's turn14 on a whole frame: battlesplit routes every draw to the arena layer

// battlesplit's one call, each split frame under preset A, before the room's draws: for a scene of mode
// CTR_BSMEX_MODE at (room, group), the place's lift (place->bottomDy) and the split's three lines (split: splitY,
// bandY, hideY, three floats in a row, z_battlesplit.c's Bs), and *whole: CTR_BSMEX_WHOLE on a whole frame, else
// not CTR_BSMEX_WHOLE. viewTop and perPx: the view's top (room) and room pixels a native row; frames: the split's
// frames drawn before this one (0: its first). place, split and whole are never null (the Bs's own fields). Nothing
// for any other scene, no scene, or no data.
void CtrBsmex_frameF(Renderer* renderer, int32_t room, int32_t group, CtrLayoutPlace* place, float viewTop, float perPx,
                     float* split, uint8_t* whole, uint32_t frames);

// exend (change battlesplit2, 11-15; z_bsmex.c's end, AAC): battlesplit's call in place of CtrBsmex_frameF (the same
// arguments): it, then in Mettaton EX's ending the centred place, never the whole one.
void CtrBsmex_frameX(Renderer* renderer, int32_t room, int32_t group, CtrLayoutPlace* place, float viewTop, float perPx,
                     float* split, uint8_t* whole, uint32_t frames);
// True when this frame's splitY is CtrBsmex_frameF's moved line (a mode 8 scene, Mettaton EX seen, the centred place):
// battlesplit's by_row draws then keep the band's line (bandY); the moved line is BY_CALL's alone.
bool CtrBsmex_rowsKept(void);

// bsmexr (change battlesplit2, 11-15; z_bsmex.c's end, AAC): battlesplit's call in place of CtrBsmex_frameX (the same
// arguments): it, then the whole place's end: held until the menu with his heart cohort gone, then a slide of
// BSM_SLIDE_FRAMES frames to the centred place (each step the centred place with the lift eased, and CTR_BSMEX_WHOLE so
// that every draw takes battlesplit's full route).
void CtrBsmex_frameY(Renderer* renderer, int32_t room, int32_t group, CtrLayoutPlace* place, float viewTop, float perPx,
                     float* split, uint8_t* whole, uint32_t frames);
// The slide's height this frame (smoothstep, above 0 and below 1: what the centred place puts on the top screen is shown
// that far from its whole place, on the bottom screen, to its centred one), 0 when this frame is not a slide step.
float CtrBsmex_slide(void);
