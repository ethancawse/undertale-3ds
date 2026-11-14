// Per-group Split placements (change bsgroup, UNDERTALE only; CMake leaves it out of DELTARUNE). maintainer's play1 reports
// (11-12, Split): the first phone fight (group 57) wants its box centred and the robot and the phone resting just
// above the top screen's bottom edge, on the hinge; the two-knight fights (groups 60 and 68) want the box, and the text
// in it, centred. Two scene placement modes past battlesplit's five (tools/layout/undertale-scenes.json "mode"; bsSceneS3
// admits them through BS_MODE_LAST). battlesplit calls CtrBsgroup_frameF once a split frame, from its box-follow frame
// (bsClearLayerF, after bsClearLayer14, before the room's draws and the present). It reads the scene's mode here, so
// battlesplit's AAC tenant (.aacout.text.battlesplit14) keeps its bytes and battlesplit's mode reads 0 for these two
// (no close seam); the place below is then set again each frame, before that frame is shown:
// - CTR_BSGROUP_HINGE (6, group 57): the top screen's last row is the band's last row, so with the band ending at the
//   robot's lowest ink row (room 236: its standing frames' ink ends 23 sprite rows below the origin, scale 2, at y 190)
//   the robot stands on the hinge. The bottom keeps the scene's lift (40: the box rows 250 to 385 centred, as every
//   centred fight) with (c)'s HP line and buttons at the foot. The phone rests 10 room px lower than the robot (its ink
//   ends at room 246 once it stops at y 200), so its own draws, and only that instance's (the marker object with the
//   phone's sprite), are drawn 10 room px higher: its rest is on the hinge too. Nothing else moves.
// - CTR_BSGROUP_BOX (7, groups 60 and 68): the box is centred in every turn by bsfollow (their bsFWho class is 4, the
//   lift that centres the box, uncapped); this mode adds the split line below.
// Both modes: the line BY_CALL, BY_ROW and the band clip read (the split's splitY and bandY) is the band's bottom (at
// most the standard band's) or the box top, whichever is higher. A shot or meteor inside the box is then on the bottom
// screen with the box, never only on the top screen's band rows, and one that straddles the line is on both, the box
// side cut at the box top (battlesplit17). What crosses the line goes on across the seam: from the box top (bottom
// screen) to the band's last row (top screen) in one game update, as the game moves it; no game state changes.
// Presentation only: no instance moves, no game variable or random number is read or written. Objects and sprites by
// index, as in UNDERTALE 1.08's Steam and GOG data (the builder's two files: both 1709 objects and 2583 sprites, the
// same indices); other data gets the line but no phone move. Own sections, appended after battlesplit's (late_reserve.ld).
#include <stddef.h>

#include "ctr_layout.h"
#include "data_win.h"
#include "instance.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#include "z_bsgroup.h"

#ifdef ENABLE_WAD16
#define BSG_CODE __attribute__((noinline, noipa, section(".text_bsgroup")))
#define BSG_BSS __attribute__((section(".bss_bsgroup_late,\"aw\",%nobits @")))
#define BSG_BAND_STANDARD 125 // z_battlesplit.c's BS_BAND_STANDARD
#define BSG_OBJECTS 1709      // UNDERTALE 1.08's objects (Steam and GOG)
#define BSG_SPRITES 2583      // and sprites
#define BSG_TOP_BORDER 760    // the box's top border object
#define BSG_PHONE 1365        // the marker object the phone fight's phone is an instance of
#define BSG_PHONE_SPRITE 336  // the phone's sprite (set at its creation; one frame)
#define BSG_PHONE_UP 10.0f    // room px the phone is drawn higher: its rest ink ends at 246, the robot's at 236

void CtrBsgroup_drawSprite6(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                            float yscale, float angleDeg, uint32_t color, float alpha);

// The split's drawSprite that CtrBsgroup_drawSprite6 hands every draw on to (battlesplit's routing), set each frame.
static void (*bsgNext)(Renderer*, int32_t, float, float, float, float, float, float, float, uint32_t, float) BSG_BSS;

BSG_CODE bool CtrBsgroup_place14(int32_t mode, CtrLayoutPlace* out) {
    if (out == nullptr || CTR_BSGROUP_HINGE > mode || mode > CTR_BSGROUP_LAST) return false;
    if (mode == CTR_BSGROUP_HINGE && 0 < out->bandRows && 240 >= out->bandRows) out->topDy = (int16_t) (240 - out->bandRows);
    return true;
}

// The first instance of object o (or a child) not destroyed, or nullptr.
static BSG_CODE Instance* bsgLive(Runner* runner, int32_t o) {
    if (runner->instancesByObject == nullptr) return nullptr;
    Instance** list = runner->instancesByObject[o];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (!list[j]->destroyed) return list[j];
    }
    return nullptr;
}

BSG_CODE void CtrBsgroup_frame14(Renderer* renderer, int32_t mode, int32_t bandRows, float viewTop, float perPx, float* split,
                                 RendererVtable* outer) {
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    if (runner == nullptr || split == nullptr || !(0.0f < perPx) || CTR_BSGROUP_HINGE > mode || mode > CTR_BSGROUP_LAST) return;
    DataWin* dw = renderer->dataWin;
    bool known = dw != nullptr && dw->objt.count == BSG_OBJECTS && dw->sprt.count == BSG_SPRITES;
    int32_t rows = bandRows > BSG_BAND_STANDARD ? BSG_BAND_STANDARD : bandRows;
    if (0 >= rows) return;
    float line = viewTop + perPx * (float) rows;
    Instance* top = known ? bsgLive(runner, BSG_TOP_BORDER) : nullptr;
    if (top != nullptr && line > top->y && top->y > viewTop) line = top->y;
    split[0] = line; // splitY
    split[1] = line; // bandY
    if (mode != CTR_BSGROUP_HINGE || !known || outer == nullptr) return;
    if (outer->drawSprite == nullptr || outer->drawSprite == CtrBsgroup_drawSprite6) return; // never itself next
    bsgNext = outer->drawSprite;
    outer->drawSprite = CtrBsgroup_drawSprite6;
}

// Mode 6's drawSprite: the phone's draws 10 room px higher, then every draw as battlesplit routes it.
BSG_CODE void CtrBsgroup_drawSprite6(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                     float yscale, float angleDeg, uint32_t color, float alpha) {
    VMContext* vm = renderer->runner != nullptr ? renderer->runner->vmContext : nullptr;
    Instance* inst = vm != nullptr ? vm->currentInstance : nullptr;
    if (inst != nullptr && vm->currentEventType == EVENT_DRAW && inst->objectIndex == BSG_PHONE && inst->spriteIndex == BSG_PHONE_SPRITE)
        y -= BSG_PHONE_UP;
    bsgNext(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

BSG_CODE void CtrBsgroup_frameF(Renderer* renderer, int32_t room, int32_t group, CtrLayoutPlace* place, float viewTop, float perPx,
                                float* split, RendererVtable* outer) {
    DataWin* dw = renderer != nullptr ? renderer->dataWin : nullptr;
    if (dw == nullptr || place == nullptr || 0 > room || 0 > group) return;
    const CtrLayoutScene* s = CtrLayout_scene(dw, room, group);
    int32_t mode = s != nullptr ? (int32_t) s->mode : 0;
    if (CTR_BSGROUP_HINGE > mode || mode > CTR_BSGROUP_LAST || !CtrBsgroup_place14(mode, place)) return;
    CtrBsgroup_frame14(renderer, mode, place->bandRows, viewTop, perPx, split, outer);
}
#endif // ENABLE_WAD16
