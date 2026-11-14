// Per-group Split placements (change bsgroup, UNDERTALE): scene placement modes past battlesplit's five, called from
// z_battlesplit.c's first place (bsPlace14) and its frame (bsClearLayer14). z_bsgroup.c has the design.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ctr_battlesplit.h"
#include "renderer.h"

#define CTR_BSGROUP_HINGE 6 // the enemy rests on the hinge: the band's last row is the top screen's last row
#define CTR_BSGROUP_BOX 7   // the box centred in every turn (the lift is bsfollow's), the split line at the box top
#define CTR_BSGROUP_LAST 7  // the last mode this file places

// The encounter's first place for a mode past battlesplit's: out is bsPlaceS3's (the scene's band and lift), which
// this changes for the mode. True: the place holds.
bool CtrBsgroup_place14(int32_t mode, CtrLayoutPlace* out);

// Each split frame, under preset A, before the frame's draws: split points at the split's splitY, then bandY (two
// floats in a row, z_battlesplit.c's Bs), the line BY_CALL, BY_ROW and the band clip read; outer is the split's
// renderer table, filled this frame. bandRows, viewTop and perPx: the place's band, the view's top (room) and room
// pixels a native row.
void CtrBsgroup_frame14(Renderer* renderer, int32_t mode, int32_t bandRows, float viewTop, float perPx, float* split,
                        RendererVtable* outer);

// battlesplit's one call, each split frame under preset A (its bsClearLayerF, after bsClearLayer14): the scene's mode
// for (room, group); for a mode of this file, place14 on place (the split's), then frame14 at place's band. Nothing
// for any other scene, no scene, a negative room or group, or no data.
void CtrBsgroup_frameF(Renderer* renderer, int32_t room, int32_t group, CtrLayoutPlace* place, float viewTop, float perPx,
                       float* split, RendererVtable* outer);
