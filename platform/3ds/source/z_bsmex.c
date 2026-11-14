// Mettaton EX in Split (change battlesplit, UNDERTALE only; CMake leaves it out of DELTARUNE). maintainer, 11-13 00:15: from
// Mettaton's room the fight was not split (group 80 had no scene row: Original, so with Stylus the game went to the
// bottom screen), from the save point with the intro skipped it was (group 81, the close seam), and that split was
// "pretty unplayable": his shots crossed the seam a frame or two above the box. His plan, accepted
// (11-13): the box centred, and for the heart-to-hearts Mettaton and the box
// on the bottom screen as on PC. One scene placement mode, CTR_BSMEX_MODE (8), for groups 80 and 81
// (tools/layout/undertale-scenes.json). battlesplit calls CtrBsmex_frameF once a split frame under preset A, from its
// frame (bsClearLayerF, after bsgroup's CtrBsgroup_frameF), before the room's draws and the present. Two places:
// - Centred (every turn but the hearts, the menus, G80's prelude and intro): the scene's own place (the standard band
//   centred on the top screen, the bottom lifted the scene's lift, BS_A_UP: the box rows 250 to 385 centred, as every
//   centred fight, with (c)'s HP line and buttons at the foot). Once Mettaton EX has been seen in the split, the
//   BY_CALL line is the bottom screen's first row (splitY = hideY, room y 80) in place of the band's bottom: his shots
//   go on the bottom screen as their top edge passes it, and fall 85 rows on the SOUL's screen before the box top.
//   Mettaton (top class) stays whole in the band. Before (G80's prelude: Mettaton's box form, controller
//   his box form's controller, whose shots fall from above room 0 and whom the SOUL's own shot hits in the band
//   rows): the scene's line, the band's bottom, as every centred fight, so that hit shows on the top screen.
// - Whole (his heart turns): every draw on the arena layer (battlesplit's bsRouteAt56 reads *whole), the lift 0, and
//   splitY and bandY a screen above the view: the bottom screen shows the PC picture in one piece, box and Mettaton
//   at their PC places; the top screen shows the top layer's clear (black).
// When: Mettaton EX's Alarm 6 adds 1 to turns and gives the heart pose for turns 6, 13, 19, 20 and 25; its Step
// rewinds 25 to 20 (a heart turn too) as that attack starts. A change of turns (the enemy's speech starts) takes the
// whole place for a heart value, else centred. Whole goes back to centred once the game is out of the enemy's turn
// (global.mnfight not 1 or 2) and none of the heart cohort is alive (the hearts, their bursts, the falling limbs and
// the leg lines: each destroys itself). A split's first frame takes the place from the state at once (a heart value
// and the enemy's turn or the cohort on). No Mettaton EX (G80's intro): no change, so centred.
// The move: the lift goes at most BSM_STEP rows a frame (40 to 0 in 5 frames: "he comes down"), at once on the first
// frame. The routing changes at once, so going whole the picture slides down into the bottom screen.
// Presentation only: reads Mettaton EX's turns (a find in its variables, never an insert), global.mnfight and the
// instance lists; writes the split's place, its lines and the whole flag. No GML runs, no instance moves, no random
// number is drawn; every Draw event still runs once a frame. Objects by index, as in UNDERTALE 1.08's Steam and GOG
// data (the builder's two files: both 1709 objects, the same indices); other data keeps the scene's place. Own
// sections, appended after bsgroup's (late_reserve.ld). The fallback (11-13, only on maintainer's word
// after his play check): BSM_HEARTS set to every turn puts the whole fight on the bottom screen, 0 B.
#include <stddef.h>

#include "ctr_layout.h"
#include "data_win.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#include "z_bsmex.h"

#ifdef ENABLE_WAD16
#define BSM_CODE __attribute__((noinline, noipa, section(".text_bsmex")))
#define BSM_RO __attribute__((aligned(4), section(".rodata_bsmex")))
#define BSM_BSS __attribute__((section(".bss_bsmex_late,\"aw\",%nobits @")))
#define BSM_OBJECTS 1709       // UNDERTALE 1.08's objects (Steam and GOG)
#define BSM_EX 404             // Mettaton EX
#define BSM_HEARTS 0x02182040u // his turns 6, 13, 19, 20 and 25: the heart-to-hearts
#define BSM_STEP 8             // the most rows the lift moves a frame
#define BSM_LIFT_MOST 120      // a lift past this is not one of this mode's (taken as 0)
#define BSM_COHORT 6

// The heart cohort: the hearts' parent (its four hearts are its children: the lists hold descendants),
// the heart burst, the star burst, the falling limb and the two leg lines (indices in that order).
static const int16_t bsmCohort[BSM_COHORT] BSM_RO = { 421, 450, 449, 408, 417, 418 };
static const char bsmVars[] BSM_RO = "turns\0mnfight";

static const CtrLayoutPlace* bsmFor BSM_BSS; // the split (its place) the state below is for
static int32_t bsmVar[2] BSM_BSS;            // varNameMap ids of turns and mnfight, -1 none
static int16_t bsmGroup BSM_BSS;
static int8_t bsmTurns BSM_BSS;              // Mettaton EX's turns last seen, -1 none
static uint8_t bsmHere BSM_BSS;              // 1: a mode 8 scene, UNDERTALE 1.08's objects
static uint8_t bsmWhole BSM_BSS;             // 1: the whole place
static uint8_t bsmLift BSM_BSS;              // the centred place's lift (the scene's), rows
static uint8_t bsmSeen BSM_BSS;              // 1: Mettaton EX has been seen in this split

// The first instance of object o (or a child) not destroyed, or nullptr.
static BSM_CODE Instance* bsmLive(Runner* runner, int32_t o) {
    Instance** list = runner->instancesByObject[o];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (!list[j]->destroyed) return list[j];
    }
    return nullptr;
}

// A variable of inst as a small integer (0 to 63), else -1. IntRValueHashMap_findSlot's find, copied: the header's
// inline function emitted out of line would land in .text, not in this file's late section.
static BSM_CODE int32_t bsmInt(Instance* inst, int32_t var) {
    if (inst == nullptr || 0 > var || inst->selfVars.capacity == 0) return -1;
    const IntRValueHashMap* map = &inst->selfVars;
    uint32_t mask = map->mask, idx = ((uint32_t) var * 0x9E3779B9u) & mask;
    while (map->entries[idx].key != var) {
        if (map->entries[idx].key == INT_RVALUE_HASHMAP_EMPTY_KEY) return -1;
        idx = (idx + 1) & mask;
    }
    const RValue* v = &map->entries[idx].value;
    if (v->type == RVALUE_INT32) return 0 <= v->int32 && 64 > v->int32 ? v->int32 : -1;
    return v->type == RVALUE_REAL && 0 <= v->real && 64 > v->real ? (int32_t) v->real : -1;
}

// Whether one of the heart cohort is alive.
static BSM_CODE bool bsmCohortOn(Runner* runner) {
    for (int32_t k = 0; BSM_COHORT > k; k++) {
        if (bsmLive(runner, bsmCohort[k]) != nullptr) return true;
    }
    return false;
}

BSM_CODE void CtrBsmex_frameF(Renderer* renderer, int32_t room, int32_t group, CtrLayoutPlace* place, float viewTop, float perPx,
                              float* split, uint8_t* whole, uint32_t frames) {
    DataWin* dw = renderer != nullptr ? renderer->dataWin : nullptr;
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    if (dw == nullptr || runner == nullptr || !(0.0f < perPx)) return;
    VMContext* vm = runner->vmContext;
    // Each split, and each group, once: the scene's mode, its lift, the two names.
    if (bsmFor != place || frames == 0 || bsmGroup != group) {
        const CtrLayoutScene* s = 0 <= room && 0 <= group ? CtrLayout_scene(dw, room, group) : nullptr;
        bsmFor = place;
        bsmGroup = (int16_t) group;
        bsmTurns = -1;
        bsmWhole = 0;
        bsmSeen = 0;
        int32_t lift = -place->bottomDy;
        bsmLift = (uint8_t) (0 < lift && BSM_LIFT_MOST >= lift ? lift : 0);
        bsmHere = s != nullptr && s->mode == CTR_BSMEX_MODE && dw->objt.count == BSM_OBJECTS && vm != nullptr &&
                  runner->instancesByObject != nullptr;
        for (int32_t i = 0; 2 > i; i++) {
            ptrdiff_t slot = bsmHere != 0 ? shgeti(vm->varNameMap, (char*) (i == 0 ? bsmVars : bsmVars + sizeof("turns"))) : -1;
            bsmVar[i] = 0 > slot ? -1 : (int32_t) vm->varNameMap[slot].value;
        }
    }
    if (bsmHere == 0) return;
    Instance* ex = bsmLive(runner, BSM_EX);
    if (ex != nullptr) {
        bsmSeen = 1;
        int32_t turns = bsmInt(ex, bsmVar[0]), mn = bsmInt(vm->globalScopeInstance, bsmVar[1]);
        bool heart = 0 <= turns && 32 > turns && ((BSM_HEARTS >> turns) & 1u) != 0;
        bool enemy = mn == 1 || mn == 2;
        if (frames == 0) bsmWhole = heart && (enemy || bsmCohortOn(runner));
        else if (turns != bsmTurns) bsmWhole = heart;
        else if (bsmWhole != 0 && !enemy && !bsmCohortOn(runner)) bsmWhole = 0;
        bsmTurns = (int8_t) turns;
    }
    // The lift: at most BSM_STEP rows a frame toward the place's, at once on the split's first frame.
    int32_t want = bsmWhole != 0 ? 0 : bsmLift, now = -place->bottomDy;
    if (frames == 0 || 0 > now || now > BSM_LIFT_MOST) now = want;
    else now = want > now + BSM_STEP ? now + BSM_STEP : now - BSM_STEP > want ? now - BSM_STEP : want;
    place->bottomDy = (int16_t) -now;
    split[2] = viewTop + perPx * (float) now; // hideY: the bottom screen's first row
    if (bsmWhole != 0) {
        split[0] = split[1] = viewTop - perPx * 240.0f; // splitY and bandY: a screen above the view
        *whole = CTR_BSMEX_WHOLE;
    } else {
        if (bsmSeen != 0) split[0] = split[2]; // splitY: the BY_CALL line at the bottom screen's first row
        if (*whole == CTR_BSMEX_WHOLE) *whole = 0;
    }
}
#endif // ENABLE_WAD16
