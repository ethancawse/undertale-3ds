// The split battle layout (change battlesplit, UNDERTALE): in a battle room, the enemy and its grid on the top screen;
// the box, SOUL, attacks, battle text, HUD and buttons on the bottom, on black.
// This unit decides which layer each draw goes to (by the drawing instance's object class) and when a
// frame is split; ctr_renderer.c's end has the layer itself and the present. Original (the default) arms nothing:
// one room-index compare per frame.
//
// Links: CtrRenderer_beginFrame (inputlate -> here -> real), CtrRenderer_endFrame (nopromo -> here -> postkick),
// Runner_drawViews (--wrap, main.c's call), and the argui settings chain (smalltext -> here -> argui) for
// battle_layout. All code, constants and state are late (late_reserve.ld): no earlier address moves, no BSS.
#if defined(ENABLE_WAD16) && defined(__3DS__)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_win.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#include "ctr_battlesplit.h"
#include "ctr_battletouch.h"
#include "ctr_display.h"
#include "ctr_bottom_view.h"
#include "ctr_wrap_chain.h"
#include "z_bsgroup.h"
#include "z_bsmex.h"

#define BS_CODE __attribute__((noinline, section(".text_battlesplit")))
#define BS_STATE __attribute__((section(".data_battlesplit")))

// Test builds that wrap Runner_drawViews themselves never split.
#if defined(CTRPORT_ASG_PROBE) || defined(CTRPORT_HALF_CENSUS)
#define BS_DRAWVIEWS_WRAPPED 0
#define __wrap_Runner_drawViews Battlesplit_drawViewsUnlinked // their wrapper has the name
#else
#define BS_DRAWVIEWS_WRAPPED 1
#endif

#ifdef CTRPORT_BS_PLACESWITCH
// Test builds only (build.sh bsplace; never a release build): L switches preset A's placement between (b), the whole
// bottom moved up by the encounter's lift, and (c), the same lift for the layer's rows above BS_C_SEAM while the HP
// line and the buttons (the rows from BS_C_SEAM down) stay at the bottom. Every fight starts in (c), maintainer's pick, and
// battle layout is Split unless settings.ini says otherwise (the playtest needs no settings.ini edits); a release build
// takes each fight's bsSeamB entry. battle_place_c = 1 or 0 (--ini or settings.ini) forces (c) or (b). In (c) a
// bottom draw that crosses the seam (its known place, as the lift's guard sees it) is logged once per object and
// battle.
#include <3ds.h>
#define BS_C_SEAM 196
int32_t CtrRenderer_bsSeam; // the layer row CtrRenderer_bsPresent splits at; 0: one piece
static int8_t bsPlaceC = 2; // 0: the seam table (bsSeamB), 1: (b), 2: (c); (c) until L (maintainer's playtest)
// The L switch is off (battlesplit11): BIT(12) is no key, the HID never sets it. L also pages the options menu's
// Legal and Credits pages, and the battle's frames still end here under the menu: in maintainer's 11-08 playtest 11 L
// presses there during Toriel's fight left every later fight in (b), its buttons raised (Rosalina's screenshot
// combo starts with L too).
#define BS_KEY_NONE BIT(12)
static uint64_t bsPlaceTick;
static const void* bsSeamOwner;
static int16_t bsSeamSeen[48];
static uint32_t bsSeamCount;
static const char bsPlaceFmt[] = " battle_place_c = %d";
static const char bsLogPlace[] = "battlesplit: placement (%c)\n";
static const char bsLogSeam[] = "battlesplit: room %d group %d: %s crosses the (c) seam (%d,%d to %d,%d)\n";
#endif

bool CtrScreens_gameBelow(void);
extern bool CtrScreens_onBottom;
extern uint8_t CtrScreens_game;
extern bool CtrScreens_gameGiven;
extern const char LoadScreen_settingsPath[];
bool __real_CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __real_CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
bool CtrArgui_chainOverrideBattlesplit(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void CtrArgui_chainSaveEditsBattlesplit(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path,
                                        CtrDisplayCaps caps);
void __real_CtrRenderer_beginFrame(Renderer* renderer); int32_t CtrBattlesplit_prepareBsr(Renderer* renderer, bool on);
void __real_Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks);
void __wrap_Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks);

// The standard band: surface rows 0..124, room y 0..249 (the enemy, its bubbles and the grid above the box).
#define BS_BAND_STANDARD 125

// Object classes. TOP is the default: the grid, enemies, bubbles, damage numbers, slashes. BY_CALL (an encounter's
// table only): each draw call by its own place, for an object whose one Draw event draws above and inside the box.
// COVER (an encounter's table only): top, but a draw that covers the whole view goes on both (an overlay that dims
// what was drawn before it, on the box's screen too).
// BS_SOUL (name table only): a SOUL object. BS_LEDGER and BS_HAZARD are working bits while the classes are built;
// BS_HIT marks an object the in-box detector has already logged, BS_CHECK a draw the detector looks at.
// BS_GUARD (a draw's want only): a bottom draw the lift's guard looks at (bsGuard).
enum { BS_TOP = 0, BS_BOTTOM = 1, BS_BY_ROW = 2, BS_BOTH = 3, BS_BY_CALL = 4, BS_COVER = 5, BS_CLASS = 7, BS_PREFIX = 4,
       BS_SOUL = 8, BS_CHECK = 8, BS_LEDGER = 0x10, BS_GUARD = 0x10, BS_HAZARD = 0x20, BS_OWN = 0x40, BS_HIT = 0x40, BS_SEEN = 0x80 };

// The battle room (306 in UNDERTALE 1.08). A room with a split scene of its own in the scene table (the first Flowey,
// 307) splits too, as group BS_ROOM_SCENE (battlesplit10, at the end of this file). The others stay Original: the fast
// dodges (308, step 4) and New Home's story scenes (309), as battlecat classes them. Omega Flowey (room_floweyx) is no
// battle room.
static const char bsRoom[] = "room_battle";
static const char bsGroupVar[] = "battlegroup";
// Objects whose instance turns the rest of the battle Original (one way, until the room changes): the Annoying Dog of
// Toriel's hard-mode interruption (group 22's ending, battlecat ORIGINAL).
// The second (the SOUL) counts only in a bench run with battle_probe bit 4: the control that a trigger alive when the
// battle starts gives no split frame.
#define BS_TRIGGERS 2
static const char bsTriggers[] = "obj_hard_dog\0obj_heart\0";
// The admissions, encounter tables and places are the scene table's (change layout S3, at the end of this file). A
// place's lift is up to 12 rows under the highest row the fight drew in the dense check; those are sampled heights,
// not bounds, so the lift has a runtime guard (bsGuard): a bottom draw that reaches the hidden rows ends the lift for
// the rest of that battle.
#define BS_CONST9 __attribute__((section(".rodata_battlesplit9")))
// The box, for the detector: left, top, right, bottom.
static const char bsBorders[] = "obj_lborder\0obj_uborder\0obj_rborder\0obj_dborder\0";
// Stylus with an Original battle (design section 10, step 3b): the rooms that move the game to the bottom screen
// (room_battle's own test is its group), then the one that keeps it there (the game over after a battle).
#define BS_TOUCH_STARTS 3
static const char bsTouchRooms[] = "room_floweybattle\0room_fastbattle\0room_storybattle\0room_gameover\0";
static const char bsLogTouch[] = "battlesplit: room %d group %d: the game goes to the %s screen (Stylus, Original battle)\n";
static const char bsLogTouchDrop[] = "battlesplit: room %d: the menu moved the game; the Stylus move is dropped\n";
static const char bsTop[] = "top";
static const char bsBottomName[] = "bottom";
static const char bsKeyFmt[] = " battle_layout = %d";
static const char bsProbeFmt[] = " battle_probe = %d";
// battle_probe's values: 0 to 31; development builds with stalecarry (CMakeLists.txt) take 0 to 63, for its 32: the
// death flash watch (z_stalecarry.c, the accessor at the end of this file).
#ifdef CTRPORT_STALECARRY
#define BS_PROBE_END 64
#else
#define BS_PROBE_END 32
#endif
static const char bsLogHit[] = "battlesplit: room %d group %d: %s draws on the top layer %s (%d,%d to %d,%d; box top %d, band %d)\n";
static const char bsInBox[] = "in the box";
static const char bsBelow[] = "below the band";
static const char bsNoPlace[] = "at no known place";
static const char bsSaveFmt[] = "battle_layout = %d\n";
static const char bsRead[] = "r";
static const char bsAppend[] = "a";
static const char bsLogGuard[] = "battlesplit: room %d group %d: %s draws in the %d rows the lift hides (%d,%d to %d,%d): the bottom "
                                 "goes back to its PC place for the rest of the battle\n";
static const char bsLogArm[] = "battlesplit: room %d split, layout %d, band %d rows, top at %d, bottom at %d\n";
static const char bsLogDisarm[] = "battlesplit: room %d back to the player's screens after %u split frames; layer switches up to %u a frame\n";
static const char bsLogObject[] = "battlesplit: room %d draws %s on layer %d\n";
static const char bsUnnamed[] = "?";
static const char bsLogFail[] = "battlesplit: room %d stays Original (no memory for the split)\n";
static const char bsLogTrigger[] = "battlesplit: room %d group %d goes Original for the rest of the battle (%s)\n";
static const char bsLogLedger[] = "battlesplit: %u objects go on the bottom as SOUL hazards by collision events alone\n";

// battlesplit14f: one of Muffet's draws that reaches above room row 0 (bsTopDraw14), replayed at present.
typedef struct {
    int32_t tpagIndex;
    float x, y, originX, originY, xscale, yscale, angleDeg, alpha;
    uint32_t color;
    float high; // room y of its highest row (a turned draw's: at most)
} BsTop14;
#define BS_TOPS14 24 // her body is at most 37 draws a frame, the few at her head reach above row 0

// One battle room's split: the table put in front of the renderer's during drawViews and the object classes.
typedef struct {
    RendererVtable outer;
    RendererVtable* inner;
    Renderer* renderer;
    uint8_t* cls;       // per object index: class, BS_SEEN once drawn
    uint32_t count;
    Instance* lastInst; // the class cache: the last drawing instance and its layer
    int32_t lastObject;
    int32_t lastWant;
    int32_t layer;      // the layer draws go to now
    float splitY;       // room y from which BY_ROW and BY_CALL draws go to the bottom (the standard band's bottom)
    float bandY;        // room y of the band's bottom (the encounter's band: what the top screen shows)
    float hideY;        // room y above which the lift (preset A, bottomDy < 0) hides the arena layer; viewTop: no lift
    float prim[4];      // the open primitive's vertices' place (left > right: none yet), for the guard
    CtrLayoutPlace place;
    int32_t layout;
    int32_t room;
    int32_t group;      // the battle group the classes are for
    bool probe;         // the detector is on (bench runs only)
    bool boxOk;         // box holds this frame's box
    uint8_t handF;      // R6-1: 1 a split handed over from an entry's flight whose first follow frame is not taken yet
                        // (bsClearLayerF). In this Bs, so a split made again never inherits it. In the padding before box.
    float box[4];       // left, top, right, bottom (room), grown by BS_BOX_MARGIN
    float viewLeft, viewTop, viewRight, viewBottom; // the view, in the room
    float* hitBottom;   // the detector: per object, the lowest draw logged
    uint32_t frames, switches, maxSwitches;
    // The menu-pick log (.text_battlesplit6, at the end of this file). Appended: the fields above keep their places.
    float pickSeamY;     // room y of BS_C_SEAM: text from splitY down to here is in the box rows
    uint32_t boxText;    // this frame's text draws in the box rows
    uint32_t pickAt;     // the pick's split frame (frames), 0 none
    uint32_t pickBlack;  // split frames since the pick with nothing drawn on the bottom
    VMContext* pickVm;   // the VM pickVar is for
    int32_t pickVar[3];  // varNameMap ids of global.myfight, mnfight, bmenuno; -1 none
    int16_t pickMenu;    // the bmenuno picked (1 FIGHT, 2 ACT, 3 ITEM, 4 MERCY)
    uint8_t pickHow;     // 0 a script, 1 A, 2 the pen
    bool pickRow;        // last split frame ended with the button row up
    // The pick log's second version (c26rev5's F1-F3; read by its fixes, v8). Appended after the first's fields.
    int32_t pickFrame;   // the pick's frame (runner->frameCount)
    uint32_t pickSeq[2]; // what the bottom screen showed at each present from the pick, 2 bits each (bsPickSample8)
    int16_t pickGroup;   // the group the pick fields are for
    uint8_t pickShown;   // presents recorded in pickSeq
    int8_t pickTextAt;   // frames from the pick to the first box-row text call; -1 none yet
    // The pick log v2's fixes (.text_battlesplit8, c26rev5's F4 on P6). Appended.
    uint8_t pickPieces;  // the seam at the pick: 0 one piece, 1 two pieces
    // The capture hook (.text_battlesplit9). Appended.
    uint8_t capLogged;   // bit per layer: a capture from it logged in this battle
    // The close seam (.aacout.text.battlesplit14). In the padding after capLogged: sizeof (Bs) is the same.
    uint8_t turn14;      // mode 5's attack: 0 the menu box, 1 centred, 2 close seam, 3 whole, 4 off, 5 whole until the menu box (bsPlaceTurn14)
    int8_t mode14;       // the encounter's close-seam mode (bsMode14), 0 none
    // Muffet above room row 0 (battlesplit14f, .aacout.text.battlesplit14). Appended.
    int16_t muffet14;    // obj_spiderb_body's object index in a mode 5 battle, else -1
    uint8_t tops14;      // her draws in top14 this frame
    BsTop14 top14[BS_TOPS14];
    // Pyrope's heat wave (battlesplit15, .aacout.text.battlesplit14). In spare14: sizeof (Bs) is the same.
    int16_t heat15;      // obj_heatwaver's object index + 1; 0 not looked up yet in this battle, -1 none
    // The encounter's end triggers (battlesplit16, .aacout.text.battlesplit14). In spare14: sizeof (Bs) is the same.
    int16_t end16[2];    // objects of class "original" in the encounter's scene, index + 1 (bsEnds16); 0 none
    // spare14[0]: battlesplit17's band clip state (bsBoth17, __wrap_CtrRenderer_bsLayer); spare14[1] free.
    uint8_t spare14[2];  // keeps sizeof (Bs) an ARM immediate: bsMake's calloc size stays one mov, its bytes the same
} Bs;
_Static_assert(sizeof(Bs) == 0x630, "bsMake's calloc size must stay an ARM immediate (one mov, as 14e's 0x204)");
_Static_assert(__builtin_offsetof(Bs, box) == __builtin_offsetof(Bs, boxOk) + 3, "handF must stay in boxOk's padding");
#define BS_BOX_MARGIN 16.0f

typedef struct {
    int8_t given;    // --ini battle_layout + 1; 0: none
    int8_t file;     // settings.ini's battle_layout + 1; 0: none
    int8_t probe;    // battle_probe (--ini's, else settings.ini's), honoured only while a bench runs: 1 the in-box
                     // detector, 2 admit groups that have a table, 4 the SOUL counts as a trigger, 8 Stylus on (the
                     // game moves to the bottom screen for Original battles, CtrScreens_gameGiven set while it is there),
                     // 16 every group lifted BS_A_UP under preset A (the lift guard's test)
    bool fileRead;
    bool armed;      // this frame is split (beginFrame to endFrame)
    bool shown;      // the last frame begun was split (CtrLayout_bottomView)
    bool roomBattle; // lastRoom is a battle room
    bool failed;     // no memory for the split in lastRoom, or a trigger: Original until the room changes
    bool ledgerLogged;
    int8_t menu;        // the options menu's battle_layout + 1; 0: unchanged (in the padding before lastRoom)
    uint8_t touch;      // BS_TOUCH_*: the game is on the bottom screen for Stylus
    uint8_t touchSaved; // the display fields CtrRenderer_bsMoveGame narrowed, for the way back
    int32_t lastRoom;
    DataWin* namesFor;            // the data file the indices below are for
    VMContext* varFor;            // the VM groupVar is for
    int32_t battleRoom;           // room_battle's index, -1 none
    int32_t groupVar;             // varNameMap id of battlegroup, -1 none
    int32_t trigger[BS_TRIGGERS]; // object indices, -1 none
    int32_t border[4];            // the box's border objects (bsBorders), -1 none
    Bs* bs;
} BsState;
enum { BS_TOUCH_ON = 1, BS_TOUCH_GIVEN = 2 }; // GIVEN: this change set CtrScreens_gameGiven (a test)
static BsState bsState BS_STATE = { .lastRoom = -1, .battleRoom = -1, .groupVar = -1, .trigger = { -1, -1 }, .border = { -1, -1, -1, -1 } };
bool CtrBench_running(void);
// The menu-pick log's layer clear (at the end of this file).
static void bsClearLayer6(Renderer* renderer);
// The pick log's present and split end (v8, since c26rev5's fixes).
static void bsPresent8(Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy);
static void bsLeave8(Renderer* renderer, bool on);
// battlesplit14f (at the end of this file): Muffet's rows above room row 0 on the top screen, then bsPresent8.
static void bsPresent14(Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy);
// The layer's clear with the capture hook (Muffet's fake border), at the end of this file.
static void bsClearLayer9(Renderer* renderer);
// battlesplit10 (at the end of this file): a room's own split scene (the first Flowey): the room as a battle room,
// its group, its admission, its classes and its place, from the scene table.
static int32_t bsTouchRoom10(Runner* runner, int32_t room);
static int32_t bsGroup10(Runner* runner);
static bool bsGroupSplits10(int32_t group);
static bool bsBuildClasses10(Bs* bs, DataWin* dw, int32_t group);
static bool bsPlace10(int32_t layout, int32_t group, CtrLayoutPlace* out);
// battlesplit11 (at the end of this file): the lift's guard without a move.
static void bsGuard11(Bs* bs, const float* r);
// battlesplit11f (at the end of this file): the router in place of bsRouteAt (same calls).
static bool bsRouteAt11f(Renderer* renderer, int32_t want, const float* r);
// group 56 (at the end of this file, .text_bsfollow): bsRouteAt for 11f's plain route, a BS_BOTH copy cut in mode 5.
static bool bsRouteAt56(Renderer* renderer, int32_t want, const float* r);
// battlesplit17 (at the end of this file, AAC): bsRouteAt for an automatic straddle, its arena copy cut at the band.
static bool bsBoth17(Renderer* renderer, int32_t want, const float* r);
// battlesplit18 (at the end of this file, AAC): bsTouchRoom10 and the frame's CtrBattlesplit_setting with the move up
// (the same calls, other targets).
static int32_t bsTouchRoom18(Runner* runner, int32_t room);
int32_t CtrBattlesplit_layout18(void);
// battlesplit13 (ctr_renderer.c, appended): CtrRenderer_bsBegun with the split's bottom target made again when the
// screen set-up inside beginFrame dropped it. Called in place of CtrRenderer_bsBegun (same call).
bool CtrRenderer_bsBegun13(Renderer* renderer); bool CtrBattlesplit_begunBsr(Renderer* renderer); // battlesplit-rework

// ===[ Settings ]===

static BS_CODE int8_t bsParse(const char* line) {
    int value;
    if (sscanf(line, bsKeyFmt, &value) == 1 && value >= 0 && CTR_BATTLE_LAYOUT_COUNT > value) return (int8_t) (value + 1);
    return 0;
}

static BS_CODE int8_t bsReadFile(const char* path, int8_t* probe) {
    int8_t found = 0;
    FILE* f = fopen(path, bsRead);
    if (f == nullptr) return 0;
    char line[128];
    while (fgets(line, sizeof(line), f) != nullptr) {
        int8_t v = bsParse(line);
        if (v != 0) found = v;
        int p;
        if (probe != nullptr && sscanf(line, bsProbeFmt, &p) == 1 && p >= 0 && BS_PROBE_END > p) *probe = (int8_t) p;
#ifdef CTRPORT_BS_PLACESWITCH
        // settings.ini's battle_place_c too, for a run with no --ini (the .3dsx from the card, Azahar).
        if (probe != nullptr && sscanf(line, bsPlaceFmt, &p) == 1) bsPlaceC = p != 0 ? 2 : 1;
#endif
    }
    fclose(f);
    return found;
}

static BS_CODE void bsReadSettings(void) {
    BsState* st = &bsState;
    if (st->fileRead) return;
    st->fileRead = true;
    int8_t given = st->probe; // --ini's battle_probe wins over the file's
    st->file = bsReadFile(LoadScreen_settingsPath, &st->probe);
    if (given != 0) st->probe = given;
}

BS_CODE int32_t CtrBattlesplit_setting(void) {
    BsState* st = &bsState;
    if (st->menu != 0) return st->menu - 1;
    if (st->given != 0) return st->given - 1;
    bsReadSettings();
#if defined(CTRPORT_BS_PLACESWITCH) || defined(CTRPORT_BS_DEVSPLIT) // dev builds: CMakeLists.txt, devsplit
    return st->file != 0 ? st->file - 1 : CTR_BATTLE_LAYOUT_A; // the playtest: Split by default
#else
    return st->file != 0 ? st->file - 1 : CTR_BATTLE_LAYOUT_ORIGINAL;
#endif
}

BS_CODE void CtrBattlesplit_setSetting(int32_t value) {
    if (value >= 0 && CTR_BATTLE_LAYOUT_COUNT > value) bsState.menu = (int8_t) (value + 1);
}

// The test switches: none outside a bench run.
static BS_CODE int32_t bsProbe(void) {
    bsReadSettings();
    return bsState.probe != 0 && CtrBench_running() ? bsState.probe : 0;
}

// --ini lines: for this run only (a save never writes them). battle_probe too, so console rows can set the test
// switches (still honoured only while a bench runs).
BS_CODE bool CtrArgui_chainOverrideBattlesplit(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    int8_t v = bsParse(line);
    int p;
#ifdef CTRPORT_BS_PLACESWITCH
    if (v == 0 && sscanf(line, bsPlaceFmt, &p) == 1) {
        bsPlaceC = p != 0 ? 2 : 1;
        return true;
    }
#endif
    if (v == 0 && sscanf(line, bsProbeFmt, &p) == 1 && p >= 0 && BS_PROBE_END > p) {
        bsState.probe = (int8_t) p;
        return true;
    }
    if (v == 0) return __real_CtrArgui_override(settings, line, caps);
    bsState.given = v;
    return true;
}

// The options menu's save rewrites the display lines and drops the others: settings.ini's battle_layout line goes back,
// or the menu's value once it has changed it.
BS_CODE void CtrArgui_chainSaveEditsBattlesplit(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path,
                                                CtrDisplayCaps caps) {
    int8_t v = bsState.menu != 0 ? bsState.menu : bsReadFile(path, nullptr);
    __real_CtrArgui_saveEdits(before, after, path, caps);
    if (v == 0) return;
    FILE* f = fopen(path, bsAppend);
    if (f == nullptr) return;
    fprintf(f, bsSaveFmt, (int) (v - 1));
    fclose(f);
}

// ===[ Layout ]===

// Preset A's bottom screen: the arena layer moved up BS_A_UP rows, so the menu box (layer rows 125-195) is centred
// (85-155) and the HUD and buttons follow it (maintainer 2026-11-08, variant b, provisional). Layer rows
// 0-39 are not shown: the fights that draw there keep their bottom in place (no lift; design section 15).
#define BS_A_UP 40

BS_CODE bool CtrLayout_place(int32_t layout, int32_t bandRows, CtrLayoutPlace* out) {
    if (out == nullptr || 0 >= bandRows || bandRows > 240) return false;
    out->bandRows = (int16_t) bandRows;
    if (layout == CTR_BATTLE_LAYOUT_A) {
        out->topDy = (int16_t) ((240 - bandRows) / 2);
        out->bottomDy = -BS_A_UP;
        return true;
    }
    if (layout == CTR_BATTLE_LAYOUT_C) {
        out->topDy = (int16_t) (240 - bandRows);
        out->bottomDy = (int16_t) (2 - bandRows);
        return true;
    }
    return false;
}

BS_CODE void CtrLayout_policy(const CtrDisplaySettings* settings, CtrLayoutPolicy* out) {
    out->dialogueBottom = settings != nullptr && settings->dialogueBottom;
    out->battleLayout = CtrBattlesplit_setting();
}

// The strong definition of battletouch's weak one (ctr_bottom_view.h). With the game on the bottom screen it answers as
// battletouch's does: the view's corner (the room's, without views) and its width over 320. On a split frame the
// bottom screen shows the arena layer, room rows at 2 per pixel from the view's top, moved down by bottomDy rows.
BS_CODE bool CtrLayout_bottomView(Runner* runner, CtrBottomView* out) {
    if (runner == nullptr || out == nullptr) return false;
    Bs* bs = bsState.bs;
    bool split = bsState.shown && bs != nullptr;
    if (!split && !CtrScreens_onBottom) return false;
    if (runner->viewsEnabled && runner->views[0].enabled) {
        GMLCamera* cam = Runner_getCameraForView(runner, 0);
        if (cam == nullptr || 0 >= cam->viewWidth) return false;
        *out = (CtrBottomView) { cam->viewX, cam->viewY, (float) cam->viewWidth / 320.0f };
    } else {
        if (runner->currentRoom == nullptr) return false;
        *out = (CtrBottomView) { 0.0f, 0.0f, (float) runner->currentRoom->width / 320.0f };
    }
    if (split) out->roomY0 -= out->roomPerPx * (float) bs->place.bottomDy;
    return true;
}

// ===[ Classes ]===

// The class an object takes from its nearest parent with a class of its own (BS_TOP if none).
static BS_CODE uint8_t bsInherit(const uint8_t* cls, DataWin* dw, uint32_t i, uint8_t own) {
    uint32_t n = dw->objt.count;
    if ((cls[i] & own) != 0) return (uint8_t) (cls[i] & BS_CLASS);
    int32_t j = dw->objt.objects[i].parentId;
    for (int32_t depth = 0; 32 > depth && j >= 0 && (uint32_t) j < n; depth++) {
        if ((cls[j] & own) != 0) return (uint8_t) (cls[j] & BS_CLASS);
        j = dw->objt.objects[j].parentId;
    }
    return BS_TOP;
}

// The hazard ledger: BS_LEDGER on the SOUL objects and their parents, then BS_HAZARD on each object (without a name
// entry) that has a collision event with one of them, and on each object a SOUL object has a collision event with.
static BS_CODE void bsLedger(uint8_t* cls, DataWin* dw) {
    uint32_t n = dw->objt.count;
    for (uint32_t i = 0; n > i; i++) {
        if ((cls[i] & BS_SOUL) == 0) continue;
        int32_t j = (int32_t) i;
        for (int32_t depth = 0; 32 > depth && j >= 0 && (uint32_t) j < n; depth++) {
            cls[j] |= BS_LEDGER;
            j = dw->objt.objects[j].parentId;
        }
    }
    for (uint32_t i = 0; n > i; i++) {
        const ObjectEventList* list = &dw->objt.objects[i].eventLists[EVENT_COLLISION];
        for (uint32_t k = 0; list->eventCount > k; k++) {
            uint32_t other = list->events[k].eventSubtype;
            if (other >= n) continue;
            if ((cls[other] & BS_LEDGER) != 0) cls[i] |= BS_HAZARD;
            if ((cls[i] & BS_SOUL) != 0) cls[other] |= BS_HAZARD;
        }
    }
}

static BS_CODE int32_t bsObject(DataWin* dw, const char* want) {
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        const char* name = dw->objt.objects[i].name;
        if (name != nullptr && strcmp(name, want) == 0) return (int32_t) i;
    }
    return -1;
}

// The battle room's index and the trigger and border objects', once per data file.
static BS_CODE void bsResolve(DataWin* dw) {
    BsState* st = &bsState;
    if (st->namesFor == dw) return;
    st->namesFor = dw;
    st->battleRoom = -1;
    for (uint32_t i = 0; dw->room.count > i; i++) {
        const char* name = dw->room.rooms[i].name;
        if (name != nullptr && strcmp(name, bsRoom) == 0) {
            st->battleRoom = (int32_t) i;
            break;
        }
    }
    const char* t = bsTriggers;
    for (int32_t k = 0; BS_TRIGGERS > k; k++, t += strlen(t) + 1) st->trigger[k] = bsObject(dw, t);
    t = bsBorders;
    for (int32_t k = 0; 4 > k; k++, t += strlen(t) + 1) st->border[k] = bsObject(dw, t);
}

// global.battlegroup, or -1 (as z_roomplan.c's battleGroup).
static BS_CODE int32_t bsGroup(Runner* runner) {
    BsState* st = &bsState;
    VMContext* ctx = runner->vmContext;
    if (ctx == nullptr || ctx->globalScopeInstance == nullptr) return -1;
    if (st->varFor != ctx) {
        ptrdiff_t slot = shgeti(ctx->varNameMap, (char*) bsGroupVar);
        st->groupVar = 0 > slot ? -1 : (int32_t) ctx->varNameMap[slot].value;
        st->varFor = ctx;
    }
    if (0 > st->groupVar) return -1;
    RValue* value = IntRValueHashMap_findSlot(&ctx->globalScopeInstance->selfVars, st->groupVar);
    if (value == nullptr) return -1;
    if (value->type == RVALUE_REAL) return (int32_t) value->real;
    if (value->type == RVALUE_INT32) return value->int32;
    return -1;
}

// The first live instance of an object (or a child), or nullptr.
static BS_CODE Instance* bsLive(Runner* runner, int32_t o) {
    if (0 > o || runner->instancesByObject == nullptr) return nullptr;
    Instance** list = runner->instancesByObject[o];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (!list[j]->destroyed) return list[j];
    }
    return nullptr;
}

// The first trigger object with a live instance, or -1. The SOUL (the last) only as the probe's control.
static BS_CODE int32_t bsTriggered(Runner* runner, int32_t probe) {
    int32_t count = (probe & 4) != 0 ? BS_TRIGGERS : BS_TRIGGERS - 1;
    for (int32_t k = 0; count > k; k++) {
        if (bsLive(runner, bsState.trigger[k]) != nullptr) return k;
    }
    return -1;
}

// The drawing instance's class this draw: BS_TOP, BS_BOTTOM, BS_BOTH, BS_BY_CALL or BS_COVER (BY_ROW resolved), with
// BS_CHECK when the detector should see the draw's place.
static BS_CODE int32_t bsWant(Bs* bs, Runner* runner) {
    VMContext* vm = runner->vmContext;
    Instance* inst = vm->currentInstance;
    if (inst == nullptr || vm->currentEventType != EVENT_DRAW) return BS_TOP;
    int32_t object = inst->objectIndex;
    if (inst == bs->lastInst && object == bs->lastObject) return bs->lastWant;
    int32_t want = BS_TOP;
    if (0 <= object && bs->count > (uint32_t) object) {
        uint8_t c = bs->cls[object];
        bs->cls[object] = (uint8_t) (c | BS_SEEN);
        c &= BS_CLASS;
        want = c == BS_BY_ROW ? (inst->y >= bs->splitY ? BS_BOTTOM : BS_TOP) : (int32_t) c;
        // An object the detector has logged is not looked at again.
        if (bs->probe && (want == BS_TOP || want >= BS_BY_CALL) && (bs->cls[object] & BS_HIT) == 0) want |= BS_CHECK;
        if (want == BS_BOTTOM && bs->hideY > bs->viewTop) want |= BS_GUARD;
    }
    bs->lastInst = inst;
    bs->lastObject = object;
    bs->lastWant = want;
    return want;
}

static BS_CODE void bsplitSwitch(Bs* bs, Renderer* renderer, int32_t layer) {
    if (bs->layer == layer) return;
    CtrRenderer_bsLayer(renderer, layer);
    bs->layer = layer;
    bs->switches++;
}

// The detector (bench runs with battle_probe bit 1): a draw left on the top layer that lands in the box (grown by
// BS_BOX_MARGIN) or below the band is lost from the SOUL's screen. A line for an object's first such draw, and again
// each time one reaches more than 8 room px further down (so an enemy's body near the box cannot hide a hazard the
// same object draws later); a draw at no known place, once.
static BS_CODE void bsDetect(Bs* bs, const float* r) {
    int32_t object = bs->lastObject;
    if (0 > object || bs->count <= (uint32_t) object) return;
    const char* why = nullptr;
    if (r[0] > r[2]) {
        why = bsNoPlace;
    } else if (bs->boxOk && bs->box[2] >= r[0] && r[2] >= bs->box[0] && bs->box[3] >= r[1] && r[3] >= bs->box[1]) {
        why = bsInBox;
    } else if (r[3] > bs->bandY + 2.0f && bs->viewBottom > bs->bandY + 2.0f && bs->viewBottom > r[1] && bs->viewRight > r[0] &&
               r[2] > bs->viewLeft) {
        why = bsBelow;
    }
    if (why == nullptr) return;
    if (why == bsNoPlace) {
        bs->cls[object] |= BS_HIT;
        bs->lastWant &= ~BS_CHECK;
    } else if (bs->hitBottom != nullptr) {
        if (bs->hitBottom[object] + 8.0f >= r[3]) return;
        bs->hitBottom[object] = r[3];
    }
    const char* name = bs->renderer->dataWin != nullptr ? bs->renderer->dataWin->objt.objects[object].name : nullptr;
    logInfo(bsLogHit, (int) bs->room, (int) bs->group, name != nullptr ? name : bsUnnamed, why, (int) r[0], (int) r[1], (int) r[2],
            (int) r[3], bs->boxOk ? (int) (bs->box[1] + BS_BOX_MARGIN) : -1, (int) bs->bandY);
}

// The lift's guard. Preset A moves the bottom screen up (bottomDy < 0), which hides the arena layer's rows above
// hideY. The encounter places come from sampled pictures, so a bottom draw that still reaches those rows (its place
// known, its top above hideY, its bottom inside the view) puts the bottom back where the PC has it, from this frame
// to the end of the battle (the room or the group changes), with one log line.
static BS_CODE void bsGuard(Bs* bs, const float* r) {
#ifdef CTRPORT_BS_PLACESWITCH
    float seamY = bs->viewTop + (bs->viewBottom - bs->viewTop) * ((float) BS_C_SEAM / 240.0f);
    if (CtrBattlesplit_seam() != 0 && r[0] <= r[2] && seamY > r[1] && r[3] > seamY && 0 <= bs->lastObject) {
        if (bsSeamOwner != bs) bsSeamOwner = bs, bsSeamCount = 0;
        uint32_t i = 0;
        while (bsSeamCount > i && bsSeamSeen[i] != bs->lastObject) i++;
        if (i == bsSeamCount && sizeof(bsSeamSeen) / sizeof(bsSeamSeen[0]) > i) {
            bsSeamSeen[bsSeamCount++] = (int16_t) bs->lastObject;
            const char* n = bs->count > (uint32_t) bs->lastObject && bs->renderer->dataWin != nullptr
                                ? bs->renderer->dataWin->objt.objects[bs->lastObject].name
                                : nullptr;
            logInfo(bsLogSeam, (int) bs->room, (int) bs->group, n != nullptr ? n : bsUnnamed, (int) r[0], (int) r[1], (int) r[2],
                    (int) r[3]);
        }
    }
#endif
    if (r[0] > r[2] || r[1] >= bs->hideY || bs->viewTop >= r[3] || r[0] >= bs->viewRight || bs->viewLeft >= r[2]) return;
    int32_t up = -bs->place.bottomDy, object = bs->lastObject;
    bs->place.bottomDy = 0;
    bs->hideY = bs->viewTop;
    bs->lastWant &= ~BS_GUARD; // the class cache: the same instance's next draws are not checked again
    const char* name = 0 <= object && bs->count > (uint32_t) object && bs->renderer->dataWin != nullptr
                           ? bs->renderer->dataWin->objt.objects[object].name
                           : nullptr;
    logInfo(bsLogGuard, (int) bs->room, (int) bs->group, name != nullptr ? name : bsUnnamed, (int) up, (int) r[0], (int) r[1],
            (int) r[2], (int) r[3]);
}

// Before a draw: on to its layer. r is the draw's place in the room (left, top, right, bottom; left > right: not
// known), looked at only for BY_CALL, COVER and the detector. True for BS_BOTH (the caller draws it on both).
static BS_CODE bool bsRouteAt(Renderer* renderer, int32_t want, const float* r) {
    Bs* bs = bsState.bs;
    if (want >= BS_BY_CALL) {
        int32_t c = want & BS_CLASS;
        // BY_CALL: bottom when the draw's top edge is at or below the band's bottom (the box top).
        if (c == BS_BY_CALL) c = r[2] >= r[0] && r[1] >= bs->splitY ? BS_BOTTOM : BS_TOP;
        // COVER: both when the draw covers the view (a pixel's slack each side).
        if (c == BS_COVER) {
            bool covers = r[2] >= r[0] && bs->viewLeft + 1.0f >= r[0] && bs->viewTop + 1.0f >= r[1] &&
                          r[2] + 1.0f >= bs->viewRight && r[3] + 1.0f >= bs->viewBottom;
            c = covers ? BS_BOTH : BS_TOP;
        }
        if ((want & BS_CHECK) != 0 && c == BS_TOP) bsDetect(bs, r);
        if ((want & BS_GUARD) != 0 && c == BS_BOTTOM) bsGuard11(bs, r);
        want = c;
    }
    if (want == BS_BOTH) {
        bsplitSwitch(bs, renderer, BS_TOP);
        // Drawn twice only onto the application surface: a game surface would take it twice.
        return CtrRenderer_bsAppBound(renderer);
    }
    bsplitSwitch(bs, renderer, want);
    return false;
}

// After a BS_BOTH draw on the top layer: its copy on the arena layer, then back.
#define BS_AGAIN(call)                          \
    do {                                        \
        Bs* bs_ = bsState.bs;                   \
        bsplitSwitch(bs_, renderer, BS_BOTTOM); \
        call;                                   \
        bsplitSwitch(bs_, renderer, BS_TOP);    \
    } while (0)
// PLACE fills float r_[4]; it runs only when the class needs the place (BY_CALL, or the detector).
#define BS_ROUTED(PLACE, call)                         \
    do {                                               \
        int32_t want_ = bsWant(bsState.bs, renderer->runner); \
        float r_[4] = { 1.0f, 0.0f, 0.0f, 0.0f };      \
        if (want_ >= BS_BY_CALL) { PLACE; }            \
        bool both_ = bsRouteAt11f(renderer, want_, r_); \
        call;                                          \
        if (both_) BS_AGAIN(call);                     \
    } while (0)
#define BS_IN (bsState.bs->inner)
#define BS_NOPLACE ((void) 0)
#define BS_BOX(a, b, c, d) bsBox(r_, (a), (b), (c), (d))

static BS_CODE void bsBox(float* r, float x1, float y1, float x2, float y2) {
    r[0] = x1 < x2 ? x1 : x2;
    r[2] = x1 < x2 ? x2 : x1;
    r[1] = y1 < y2 ? y1 : y2;
    r[3] = y1 < y2 ? y2 : y1;
}

static BS_CODE void bsGrow(float* r, float x, float y) {
    if (r[0] > r[2]) {
        r[0] = r[2] = x;
        r[1] = r[3] = y;
        return;
    }
    if (r[0] > x) r[0] = x;
    if (x > r[2]) r[2] = x;
    if (r[1] > y) r[1] = y;
    if (y > r[3]) r[3] = y;
}

// A sprite's place: its texture item's target rectangle, scaled about the origin and turned by angleDeg (GameMaker:
// counterclockwise, y down). An unknown item: the point.
static BS_CODE void bsSpritePlace(float* r, Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                  float yscale, float angleDeg) {
    DataWin* dw = renderer->dataWin;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    if (dw != nullptr && 0 <= tpagIndex && dw->tpag.count > (uint32_t) tpagIndex) {
        TexturePageItem* t = &dw->tpag.items[tpagIndex];
        x0 = ((float) t->targetX - originX) * xscale;
        y0 = ((float) t->targetY - originY) * yscale;
        x1 = x0 + (float) t->targetWidth * xscale;
        y1 = y0 + (float) t->targetHeight * yscale;
    }
    r[0] = 1.0f;
    r[2] = 0.0f;
    float c = 1.0f, s = 0.0f;
    if (angleDeg != 0.0f) {
        float a = angleDeg * 0.017453292f;
        c = cosf(a);
        s = sinf(a);
    }
    const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    for (int32_t k = 0; 4 > k; k++) bsGrow(r, x + xs[k] * c + ys[k] * s, y - xs[k] * s + ys[k] * c);
}

static BS_CODE void bsPointsPlace(float* r, const float* xs, const float* ys, int32_t count, int32_t stride) {
    r[0] = 1.0f;
    r[2] = 0.0f;
    for (int32_t k = 0; count > k; k++) bsGrow(r, xs[k * stride], ys[k * stride]);
}

static BS_CODE void bsQuadPlace(float* r, float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4) {
    bsBox(r, x1, y1, x2, y2);
    bsGrow(r, x3, y3);
    bsGrow(r, x4, y4);
}

static BS_CODE void bsDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                 float yscale, float angleDeg, uint32_t color, float alpha) {
    BS_ROUTED(bsSpritePlace(r_, renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg),
              BS_IN->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha));
}
static BS_CODE void bsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x,
                                     float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color,
                                     float alpha) {
    BS_ROUTED(BS_BOX(x, y, x + srcW * xscale, y + srcH * yscale),
              BS_IN->drawSpritePart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color,
                                    alpha));
}
static BS_CODE void bsDrawSpritePartColor(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH,
                                          float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY,
                                          uint32_t color1, uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    BS_ROUTED(BS_BOX(x, y, x + srcW * xscale, y + srcH * yscale),
              BS_IN->drawSpritePartColor(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY,
                                         color1, color2, color3, color4, alpha));
}
static BS_CODE void bsDrawSpritePos(Renderer* renderer, int32_t tpagIndex, float x1, float y1, float x2, float y2, float x3, float y3,
                                    float x4, float y4, float alpha) {
    BS_ROUTED(bsQuadPlace(r_, x1, y1, x2, y2, x3, y3, x4, y4), BS_IN->drawSpritePos(renderer, tpagIndex, x1, y1, x2, y2, x3, y3, x4, y4, alpha));
}
static BS_CODE void bsDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    BS_ROUTED(BS_BOX(x1, y1, x2, y2), BS_IN->drawRectangle(renderer, x1, y1, x2, y2, color, alpha, outline));
}
static BS_CODE void bsDrawRectangleColor(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color1, uint32_t color2,
                                         uint32_t color3, uint32_t color4, float alpha, bool outline) {
    BS_ROUTED(BS_BOX(x1, y1, x2, y2), BS_IN->drawRectangleColor(renderer, x1, y1, x2, y2, color1, color2, color3, color4, alpha, outline));
}
static BS_CODE void bsDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    BS_ROUTED(BS_BOX(x1, y1, x2, y2), BS_IN->drawLine(renderer, x1, y1, x2, y2, width, color, alpha));
}
static BS_CODE void bsDrawTriangle(Renderer* renderer, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t color1,
                                   uint32_t color2, uint32_t color3, float alpha, bool outline) {
    BS_ROUTED(bsQuadPlace(r_, x1, y1, x2, y2, x3, y3, x3, y3), BS_IN->drawTriangle(renderer, x1, y1, x2, y2, x3, y3, color1, color2, color3, alpha, outline));
}
static BS_CODE void bsDrawTriangleFan(Renderer* renderer, float cx, float cy, const float* xs, const float* ys, int32_t pointCount,
                                      uint32_t centerColor, uint32_t edgeColor, float alpha) {
    BS_ROUTED((bsPointsPlace(r_, xs, ys, pointCount, 1), bsGrow(r_, cx, cy)), BS_IN->drawTriangleFan(renderer, cx, cy, xs, ys, pointCount, centerColor, edgeColor, alpha));
}
static BS_CODE void bsDrawLines(Renderer* renderer, const float* xy, int32_t count, float width, uint32_t color, float alpha) {
    BS_ROUTED(bsPointsPlace(r_, xy, xy + 1, count, 2), BS_IN->drawLines(renderer, xy, count, width, color, alpha));
}
static BS_CODE void bsDrawLineColor(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color1, uint32_t color2,
                                    float alpha) {
    BS_ROUTED(BS_BOX(x1, y1, x2, y2), BS_IN->drawLineColor(renderer, x1, y1, x2, y2, width, color1, color2, alpha));
}
// Text: its first line's corner (the font's size is not known here).
static BS_CODE void bsDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                               float lineSeparation) {
    BS_ROUTED(BS_BOX(x, y, x, y), BS_IN->drawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation));
}
static BS_CODE void bsDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                    int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    BS_ROUTED(BS_BOX(x, y, x, y), BS_IN->drawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation));
}
static BS_CODE void bsDrawTextUI(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                 int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    BS_ROUTED(BS_BOX(x, y, x, y), BS_IN->drawTextUI(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation));
}
// A primitive's vertices are gathered from primitiveBegin, for the lift's guard.
static BS_CODE void bsPrimitiveBegin(Renderer* renderer, int32_t primitiveType) {
    bsState.bs->prim[0] = 1.0f;
    bsState.bs->prim[2] = 0.0f;
    BS_IN->primitiveBegin(renderer, primitiveType);
}
static BS_CODE void bsPrimitiveBeginTexture(Renderer* renderer, int32_t primitiveType, int32_t texture) {
    bsState.bs->prim[0] = 1.0f;
    bsState.bs->prim[2] = 0.0f;
    BS_IN->primitiveBeginTexture(renderer, primitiveType, texture);
}
static BS_CODE void bsDrawVertex(Renderer* renderer, float x, float y, float z, uint32_t color, float alpha, float u, float v) {
    bsGrow(bsState.bs->prim, x, y);
    BS_IN->drawVertex(renderer, x, y, z, color, alpha, u, v);
}
// The vertices go to the GPU at primitiveEnd, once: a BOTH primitive stays on the top layer. BY_CALL and the detector
// still treat its place as not known (BY_CALL leaves it on the top layer, the detector logs it for review); only the
// guard looks at the vertices' place (BS_GUARD is set only for a bottom draw).
static BS_CODE void bsPrimitiveEnd(Renderer* renderer) {
    Bs* bs = bsState.bs;
    int32_t want = bsWant(bs, renderer->runner);
    const float none[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    bsRouteAt11f(renderer, want == BS_BOTH ? BS_TOP : want, (want & BS_GUARD) != 0 ? bs->prim : none);
    BS_IN->primitiveEnd(renderer);
}
// A vertex buffer's place: its submitted vertices' positions (clamped as ctrDrawVertexBuffer clamps them).
static BS_CODE void bsBufferPlace(float* r, const VertexBuffer* buffer, int32_t offset, int32_t count) {
    r[0] = 1.0f;
    r[2] = 0.0f;
    if (buffer == nullptr || buffer->format == nullptr || buffer->data == nullptr || buffer->format->stride == 0) return;
    const VertexFormat* f = buffer->format;
    int32_t n = (int32_t) (buffer->size / f->stride), e = 0;
    while (f->numElements > e && (f->elements[e].usage != VERTEX_USAGE_POSITION ||
                                  (f->elements[e].type != VERTEX_TYPE_FLOAT2 && f->elements[e].type != VERTEX_TYPE_FLOAT3)))
        e++;
    if (e >= f->numElements) return;
    if (0 > offset) offset = 0;
    if (offset > n) offset = n;
    if (0 > count || count > n - offset) count = n - offset;
    for (int32_t i = 0; count > i; i++) {
        float xy[2];
        memcpy(xy, buffer->data + (size_t) (offset + i) * f->stride + f->elements[e].offset, sizeof(xy));
        bsGrow(r, xy[0], xy[1]);
    }
}
static BS_CODE void bsDrawVertexBuffer(Renderer* renderer, VertexBuffer* buffer, int32_t primitive, int32_t texture, int32_t offset,
                                       int32_t count) {
    BS_ROUTED(bsBufferPlace(r_, buffer, offset, count), BS_IN->drawVertexBuffer(renderer, buffer, primitive, texture, offset, count));
}
// draw_clear (and drawViews' own clear) clears both layers.
static BS_CODE void bsClearScreen(Renderer* renderer, uint32_t color, float alpha) {
    Bs* bs = bsState.bs;
    bsplitSwitch(bs, renderer, BS_TOP);
    BS_IN->clearScreen(renderer, color, alpha);
    if (CtrRenderer_bsAppBound(renderer)) BS_AGAIN(BS_IN->clearScreen(renderer, color, alpha));
}
static BS_CODE void bsDrawTile(Renderer* renderer, RoomTile* tile, float offsetX, float offsetY) {
    BS_ROUTED(BS_BOX((float) tile->x + offsetX, (float) tile->y + offsetY, (float) tile->x + offsetX + (float) tile->width * tile->scaleX,
                     (float) tile->y + offsetY + (float) tile->height * tile->scaleY),
              BS_IN->drawTile(renderer, tile, offsetX, offsetY));
}
// Tiled draws cover the room.
static BS_CODE void bsDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale,
                                      float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha) {
    BS_ROUTED(BS_BOX(0.0f, 0.0f, roomW, roomH),
              BS_IN->drawSpriteTiled(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha));
}
static BS_CODE void bsDrawSurface(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                  float x, float y, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    BS_ROUTED(BS_BOX(x, y, x + (float) srcWidth * xscale, y + (float) srcHeight * yscale),
              BS_IN->drawSurface(renderer, surfaceID, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg, color, alpha));
}
static BS_CODE void bsDrawSurfaceColor(Renderer* renderer, int32_t surfaceID, int32_t srcLeft, int32_t srcTop, int32_t srcWidth,
                                       int32_t srcHeight, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color1,
                                       uint32_t color2, uint32_t color3, uint32_t color4, float alpha) {
    BS_ROUTED(BS_BOX(x, y, x + (float) srcWidth * xscale, y + (float) srcHeight * yscale),
              BS_IN->drawSurfaceColor(renderer, surfaceID, srcLeft, srcTop, srcWidth, srcHeight, x, y, xscale, yscale, angleDeg, color1, color2,
                                      color3, color4, alpha));
}
static BS_CODE void bsDrawSurfaceTiled(Renderer* renderer, int32_t surfaceID, float x, float y, float xscale, float yscale, float roomW,
                                       float roomH, uint32_t color, float alpha) {
    BS_ROUTED(BS_BOX(0.0f, 0.0f, roomW, roomH), BS_IN->drawSurfaceTiled(renderer, surfaceID, x, y, xscale, yscale, roomW, roomH, color, alpha));
}
static BS_CODE void bsDrawTiledPart(Renderer* renderer, int32_t tpagIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH, float dstX,
                                    float dstY, float dstW, float dstH, uint32_t color, float alpha) {
    BS_ROUTED(BS_BOX(dstX, dstY, dstX + dstW, dstY + dstH),
              BS_IN->drawTiledPart(renderer, tpagIndex, srcX, srcY, srcW, srcH, dstX, dstY, dstW, dstH, color, alpha));
}

// This frame's outer table: the renderer's as it is now, with the draws routed (an optional entry stays empty).
static BS_CODE void bsInstall(Bs* bs, RendererVtable* inner) {
    RendererVtable* o = &bs->outer;
    *o = *inner;
    bs->inner = inner;
    o->drawSprite = bsDrawSprite;
    o->drawSpritePart = bsDrawSpritePart;
    o->drawSpritePartColor = bsDrawSpritePartColor;
    o->drawSpritePos = bsDrawSpritePos;
    o->drawRectangle = bsDrawRectangle;
    o->drawRectangleColor = bsDrawRectangleColor;
    o->drawLine = bsDrawLine;
    o->drawTriangle = bsDrawTriangle;
    if (inner->drawTriangleFan != nullptr) o->drawTriangleFan = bsDrawTriangleFan;
    if (inner->drawLines != nullptr) o->drawLines = bsDrawLines;
    o->drawLineColor = bsDrawLineColor;
    o->drawText = bsDrawText;
    o->drawTextColor = bsDrawTextColor;
    o->drawTextUI = bsDrawTextUI;
    if (inner->primitiveBegin != nullptr) o->primitiveBegin = bsPrimitiveBegin;
    if (inner->primitiveBeginTexture != nullptr) o->primitiveBeginTexture = bsPrimitiveBeginTexture;
    if (inner->drawVertex != nullptr) o->drawVertex = bsDrawVertex;
    o->primitiveEnd = bsPrimitiveEnd;
    o->drawVertexBuffer = bsDrawVertexBuffer;
    o->clearScreen = bsClearScreen;
    if (inner->drawTile != nullptr) o->drawTile = bsDrawTile;
    if (inner->drawSpriteTiled != nullptr) o->drawSpriteTiled = bsDrawSpriteTiled;
    o->drawSurface = bsDrawSurface;
    o->drawSurfaceColor = bsDrawSurfaceColor;
    if (inner->drawSurfaceTiled != nullptr) o->drawSurfaceTiled = bsDrawSurfaceTiled;
    if (inner->drawTiledPart != nullptr) o->drawTiledPart = bsDrawTiledPart;
}

// The detector's frame: the view and the box (the four border objects' sides, grown by BS_BOX_MARGIN).
static BS_CODE void bsProbeFrame(Bs* bs, Runner* runner) {
    Instance* side[4];
    bs->boxOk = true;
    for (int32_t k = 0; 4 > k; k++) {
        side[k] = bsLive(runner, bsState.border[k]);
        if (side[k] == nullptr) bs->boxOk = false;
    }
    if (!bs->boxOk) return;
    bs->box[0] = side[0]->x - BS_BOX_MARGIN;
    // Above a box below the band, the top layer is right: the margin stops at the band's bottom.
    float top = side[1]->y;
    bs->box[1] = top + 2.0f >= bs->bandY && bs->bandY > top - BS_BOX_MARGIN ? bs->bandY : top - BS_BOX_MARGIN;
    bs->box[2] = side[2]->x + BS_BOX_MARGIN;
    bs->box[3] = side[3]->y + BS_BOX_MARGIN;
}

// The close seam's place for this frame (battlesplit14, at the end of this file), then bsClearLayer9.
static void bsClearLayer14(Renderer* renderer);
// Part C's flight hook (W1, at this file's end): the unarmed path's draw goes there.
static void bsDrawViewsC(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks);

// main.c's Draw: on a split frame, the room's draws go through the outer table, and the arena layer starts black.
BS_CODE void __wrap_Runner_drawViews(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks) {
    Bs* bs = bsState.bs;
    Renderer* renderer = runner->renderer;
    if (!bsState.armed || bs == nullptr || renderer == nullptr || renderer != bs->renderer || renderer->vtable == nullptr) {
        bsDrawViewsC(runner, gameW, gameH, debugShowCollisionMasks);
        return;
    }
    RendererVtable* inner = renderer->vtable;
    bsInstall(bs, inner);
    bs->lastInst = nullptr;
    bs->lastObject = -1;
    bs->lastWant = BS_TOP;
    bs->prim[0] = 1.0f; // no primitive open
    bs->prim[2] = 0.0f;
    bs->layer = BS_TOP;
    bs->switches = 0;
    GMLCamera* camera = Runner_getCameraForView(runner, 0);
    float perPx = camera != nullptr && 0 < camera->viewWidth ? (float) camera->viewWidth / 320.0f : 2.0f;
    bs->viewLeft = camera != nullptr ? camera->viewX : 0.0f;
    bs->viewTop = camera != nullptr ? camera->viewY : 0.0f;
    bs->viewRight = bs->viewLeft + perPx * 320.0f;
    bs->viewBottom = bs->viewTop + perPx * 240.0f;
    bs->splitY = bs->viewTop + perPx * (float) BS_BAND_STANDARD;
    bs->bandY = bs->viewTop + perPx * (float) bs->place.bandRows;
    bs->hideY = bs->viewTop + (bs->layout == CTR_BATTLE_LAYOUT_A && 0 > bs->place.bottomDy ? perPx * (float) -bs->place.bottomDy : 0.0f);
    if (bs->probe) bsProbeFrame(bs, runner);
    bsClearLayer6(renderer);
    renderer->vtable = &bs->outer;
    __real_Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
    if (renderer->vtable == &bs->outer) renderer->vtable = inner;
    CtrRenderer_bsLayer(renderer, BS_TOP);
    bs->layer = BS_TOP;
    bs->frames++;
    if (bs->switches > bs->maxSwitches) bs->maxSwitches = bs->switches;
}

// ===[ Arming ]===

static BS_CODE void bsFree(Bs* bs) {
#ifdef CTRPORT_BS_PLACESWITCH
    bsSeamOwner = nullptr;
#endif
    if (bs->cls != nullptr) {
        for (uint32_t i = 0; bs->count > i; i++) {
            if ((bs->cls[i] & BS_SEEN) == 0) continue;
            const char* name = bs->renderer->dataWin != nullptr ? bs->renderer->dataWin->objt.objects[i].name : nullptr;
            logInfo(bsLogObject, (int) bs->room, name != nullptr ? name : bsUnnamed, (int) (bs->cls[i] & BS_CLASS));
        }
        free(bs->cls);
    }
    free(bs->hitBottom);
    logInfo(bsLogDisarm, (int) bs->room, (unsigned) bs->frames, (unsigned) bs->maxSwitches);
    free(bs);
}

// The scene table's (change layout S3, this file's end): the calls below go there.
static bool bsGroupSplitsS3(int32_t group);
static bool bsHasTableS3(int32_t group);
static bool bsPlaceS3(int32_t layout, int32_t group, CtrLayoutPlace* out);
static bool bsBuildClassesS3(Bs* bs, DataWin* dw, int32_t group);

static BS_CODE Bs* bsMake(Renderer* renderer, Runner* runner, int32_t layout, int32_t group, bool probe) {
    Bs* bs = (Bs*) calloc(1, sizeof(Bs));
    if (bs == nullptr) return nullptr;
    bs->renderer = renderer;
    bs->room = runner->currentRoomIndex;
    bs->layout = layout;
    bs->group = group;
    bs->probe = probe;
    bs->lastObject = -1;
    if (!bsPlace10(layout, group, &bs->place) || !bsBuildClasses10(bs, runner->dataWin, group)) {
        free(bs);
        return nullptr;
    }
    if (probe) {
        bs->hitBottom = (float*) malloc(sizeof(float) * (bs->count != 0 ? bs->count : 1u));
        if (bs->hitBottom != nullptr) {
            for (uint32_t i = 0; bs->count > i; i++) bs->hitBottom[i] = -1.0e9f;
        }
    }
    logInfo(bsLogArm, (int) bs->room, (int) layout, (int) bs->place.bandRows, (int) bs->place.topDy, (int) bs->place.bottomDy);
    return bs;
}

// ===[ Stylus with an Original battle (step 3b) ]===

// 2 a room whose battle moves the game to the bottom screen (room_battle's is decided by its group), 1 a room that
// keeps it there, 0 elsewhere. Omega Flowey's room (room_floweyx) is in neither.
static BS_CODE int32_t bsTouchRoom(Runner* runner, int32_t room) {
    if (room == bsState.battleRoom) return 2;
    if (0 > room || (uint32_t) room >= runner->dataWin->room.count) return 0;
    const char* name = runner->dataWin->room.rooms[room].name;
    if (name == nullptr) return 0;
    int32_t k = 0;
    for (const char* t = bsTouchRooms; *t != '\0'; t += strlen(t) + 1, k++) {
        if (strcmp(name, t) == 0) return BS_TOUCH_STARTS > k ? 2 : 1;
    }
    return 0;
}

// Asriel (255, 256) and the Lost Souls inside his fight (87-90) stay where the player put the game: moving back to the
// top needs the top screen's 384,000 B of VRAM in the most starved scenes (ruling 3b, with Omega Flowey).
static BS_CODE bool bsTouchGroup(int32_t group) {
    return group != 255 && group != 256 && (87 > group || group > 90);
}

// Stylus on: battletouch's setting outside a test, battle_probe bit 8 in one.
static BS_CODE bool bsTouchOn(void) {
    if (CtrBench_running()) return (bsProbe() & 8) != 0;
    return CtrBattletouch_modeNow() != CTR_BATTLETOUCH_OFF;
}

// The move, in this frame's configureScreens (before the real beginFrame, outside the GPU frame). The game was on the
// top screen when it went below, so it goes back to the top.
static BS_CODE void bsTouchMove(Renderer* renderer, int32_t room, int32_t group, bool below) {
    BsState* st = &bsState;
    if (below) {
        st->touch = BS_TOUCH_ON;
        if (CtrBench_running() && !CtrScreens_gameGiven) {
            CtrScreens_gameGiven = true;
            st->touch |= BS_TOUCH_GIVEN;
        }
        CtrScreens_game = 1;
        uint32_t CtrLayout_moveGame(Renderer*, bool, uint32_t); st->touchSaved = (uint8_t) CtrLayout_moveGame(renderer, true, 0); // only with both targets held (R10)
    } else {
        CtrScreens_game = 0;
        if ((st->touch & BS_TOUCH_GIVEN) != 0) CtrScreens_gameGiven = false;
        st->touch = 0;
        { uint32_t CtrLayout_moveGame(Renderer*, bool, uint32_t); CtrLayout_moveGame(renderer, false, st->touchSaved); }
    }
    logInfo(bsLogTouch, (int) room, (int) group, below ? bsBottomName : bsTop);
}

// W1's twins of CtrBattlesplit_layout18 and bsGroupSplits10 (moved fights, this file's end): the same calls.
static int32_t bsLayoutE(void);
static bool bsGroupSplitsE(int32_t group);

// Before the frame (before Step): whether it is split, from the room and the setting.
BS_CODE void CtrRenderer_chainBeginFrameBattlesplit(Renderer* renderer) {
    BsState* st = &bsState;
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    bool want = false;
    int32_t layout = CTR_BATTLE_LAYOUT_ORIGINAL;
    int32_t group = -1, probe = 0;
    int32_t touchRoom = -1; // a room whose battle moves the game below, on its first frame
    if (BS_DRAWVIEWS_WRAPPED && runner != nullptr && runner->renderer == renderer && runner->dataWin != nullptr) {
        int32_t room = runner->currentRoomIndex;
        // A Game screen change in the menu wins: the game stays where the player put it.
        if (st->touch != 0 && CtrScreens_game != 1) {
            if ((st->touch & BS_TOUCH_GIVEN) != 0) CtrScreens_gameGiven = false;
            st->touch = 0;
            logInfo(bsLogTouchDrop, (int) room);
        }
        if (room != st->lastRoom) {
            bsResolve(runner->dataWin);
            st->lastRoom = room;
            st->roomBattle = room == st->battleRoom && room >= 0;
            st->failed = false;
            int32_t where = bsTouchRoom18(runner, room);
            if (st->touch != 0 && where == 0) bsTouchMove(renderer, room, -1, false);
            if (st->touch == 0 && where == 2 && CtrScreens_game == 0 && bsTouchOn()) touchRoom = room;
        }
        if (st->roomBattle && !st->failed) layout = bsLayoutE();
        // Original costs one room compare a frame; the group and the triggers are read only with a layout chosen.
        if (layout != CTR_BATTLE_LAYOUT_ORIGINAL) {
            group = bsGroup10(runner);
            probe = bsProbe();
            // Before the first split frame too: a trigger alive when the battle starts gives no split frame.
            int32_t hit = bsTriggered(runner, probe);
            if (hit >= 0) {
                st->failed = true;
                const char* t = bsTriggers;
                for (int32_t k = 0; hit > k; k++) t += strlen(t) + 1;
                logInfo(bsLogTrigger, (int) room, (int) group, t);
            }
            bool admitted = bsGroupSplitsE(group) || ((probe & 2) != 0 && bsHasTableS3(group));
            want = !st->failed && admitted && CtrRenderer_bsAllowed(renderer) && !CtrScreens_gameBelow();
            if (!want) layout = CTR_BATTLE_LAYOUT_ORIGINAL;
        }
        if (touchRoom >= 0 && !want) {
            int32_t g = st->roomBattle ? (group >= 0 ? group : bsGroup10(runner)) : -1;
            if (!st->roomBattle || bsTouchGroup(g)) bsTouchMove(renderer, touchRoom, g, true);
        }
    }
    // A new group in the same split gets its own table: the classes are made again, the layer is kept.
    CtrLayoutPlace place;
    if (want && st->bs != nullptr && st->bs->group != group && bsPlace10(st->bs->layout, group, &place) &&
        st->bs->place.bandRows == place.bandRows && st->bs->place.bottomDy == place.bottomDy) {
        Bs* bs = st->bs;
        free(bs->cls);
        bs->cls = nullptr;
        bs->count = 0;
        bs->lastInst = nullptr;
        bs->lastObject = -1;
        if (bsBuildClasses10(bs, runner->dataWin, group)) bs->group = group;
    }
    if (st->bs != nullptr && (!want || st->bs->renderer != renderer || st->bs->layout != layout || st->bs->room != st->lastRoom ||
                              st->bs->group != group || st->bs->probe != ((probe & 1) != 0))) {
        bsLeave8(st->bs->renderer, false);
        bsFree(st->bs);
        st->bs = nullptr;
    }
    bool split = false;
    if (want) {
        if (st->bs == nullptr) st->bs = bsMake(renderer, runner, layout, group, (probe & 1) != 0);
        if (st->bs == nullptr) {
            st->failed = true;
            logInfo(bsLogFail, (int) st->lastRoom);
        } else {
            split = CtrBattlesplit_prepareBsr(renderer, true) > 0; // battlesplit-rework: bottom held lit
        }
    }
    st->armed = false;
    __real_CtrRenderer_beginFrame(renderer);
    st->armed = split && CtrBattlesplit_begunBsr(renderer); // battlesplit-rework: bsBegun13 after the hold
    st->shown = st->armed;
}

// In place of present, on a split frame.
BS_CODE void CtrRenderer_chainEndFrameBattlesplit(Renderer* renderer) {
    Bs* bs = bsState.bs;
    if (bsState.armed && bs != nullptr && bs->renderer == renderer) {
#ifdef CTRPORT_BS_PLACESWITCH
        uint64_t now = svcGetSystemTick();
        if ((hidKeysDown() & BS_KEY_NONE) != 0 && now - bsPlaceTick > (uint64_t) SYSCLOCK_ARM11 / 4u) {
            bsPlaceTick = now;
            bsPlaceC = CtrBattlesplit_seam() != 0 ? 1 : 2;
            logInfo(bsLogPlace, bsPlaceC == 2 ? 'c' : 'b');
        }
        CtrRenderer_bsSeam = CtrBattlesplit_seam();
#endif
        bsPresent14(renderer, bs->place.bandRows, bs->place.topDy, bs->place.bottomDy);
    }
    bsState.armed = false;
    CTR_ENDFRAME_POSTKICK(renderer);
}

// ===[ Placement (c) ]===
// maintainer (11-08, review131) picked (c) as the default: the layer's rows above BS_C_SEAM move up by the encounter's
// lift (the box centred, as in (b)), and the rows from BS_C_SEAM down (the HP line and FIGHT/ACT/ITEM/MERCY) stay at
// the very bottom, with the lift's rows between them black. A fight whose shots cross the seam would be torn there,
// so it keeps (b), the whole bottom moved up: bsSeamB, one bit per group (design section 20). At lift 0 the two are
// the same picture. Appended after batch 1's constants (code and table), so nothing before them moves.
#define BS_CONST5 __attribute__((section(".rodata_battlesplit5")))
#define BS_CODE5 __attribute__((noinline, section(".text_battlesplit5")))
#ifndef BS_C_SEAM
#define BS_C_SEAM 196
#endif
// (b) for these groups: the ADAPT survey's crossings (42, 49, 50, 52, 55, 60, 68, 76, 77, 78, 135, 140) and every lifted
// fight the seam sweep saw cross (one boot per model, all 65, Old and New the same: 6, 30, 31, 33, 36, 44, 61-67,
// 79, 82, 84, 85, 120, 125-127, 131-134; then 41 from batch 2a's tour; design section 20). Not 20 (Napstablook): his
// close seam (battlesplit14) keeps the HP line and the buttons at the foot; his tears crossing it were approved.
// Not 49, 52 and 78 (the Royal Guards, Pyrope): the box-follow lift (this file's end) lifts them only while nothing
// crosses the seam, so they are (c), the HP line and the buttons at the foot. r5 (the centred default, 11-13):
// every group above is followed now, lifted only in its menu (its attacks' cap is 0, the PC place), so they are (c)
// too: the HP line and the buttons stay at their PC rows when the menu box is centred (140 since its row, bsgroup).
static const uint8_t bsSeamB[33] BS_CONST5 = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// The layer row the bottom splits at this frame: BS_C_SEAM in (c), 0 for one piece ((b), or no lift).
BS_CODE5 int32_t CtrBattlesplit_seam(void) {
    Bs* bs = bsState.bs;
    if (bs == nullptr || bs->place.bottomDy >= 0) return 0;
#ifdef CTRPORT_BS_PLACESWITCH
    if (bsPlaceC != 0) return bsPlaceC == 2 ? BS_C_SEAM : 0;
#endif
    int32_t g = bs->group;
    bool b = g >= 0 && (int32_t) (sizeof(bsSeamB) * 8u) > g && (bsSeamB[g >> 3] & (1u << (g & 7))) != 0;
    return b ? 0 : BS_C_SEAM;
}

// For touch (battletouch): true when the bottom is two pieces this frame. Screen rows from *seamY down show the layer
// unlifted (the HP line and buttons), rows above *seamY - *lift show it lifted (CtrLayout_bottomView), and the rows
// between are black. The lift can end mid-battle (the guard), so callers ask every frame.
BS_CODE5 bool CtrLayout_bottomSeam(int32_t* seamY, int32_t* lift) {
    Bs* bs = bsState.bs;
    if (!bsState.shown || bs == nullptr) return false;
    int32_t seam = CtrBattlesplit_seam();
    if (seam == 0) return false;
    if (seamY != nullptr) *seamY = seam;
    if (lift != nullptr) *lift = -bs->place.bottomDy;
    return true;
}

// ===[ Menu-pick log (playtest1 P1) ]===
// One line per FIGHT/ACT/ITEM/MERCY pick in a split battle, so a console log can time the bottom after a pick: the
// frame id, the frames from the pick to the first frame with text in the box rows (0: the pick's own frame), and how
// many split frames in between had nothing drawn on the bottom (a black bottom). The pick is the frame whose Step
// moved global.bmenuno off the button row (global.myfight, global.mnfight and bmenuno all 0 the split frame before).
// The text counted is the box's (layer rows from the standard band down to BS_C_SEAM): the HP line is below it. Own
// section, appended after batch 2a's tables (.text_battlesplit6), so nothing before it moves; the layer clear's call
// into it replaces a call of the same shape. The line is v8's (bsPresent8, below); the first version's present
// (bsPresent6) went in the post-G8 move commit.
#include <3ds.h>
#define BS_CODE6 __attribute__((noinline, noipa, section(".text_battlesplit6")))
#define BS_CONST6 __attribute__((section(".rodata_battlesplit6")))
static const char bsPickVars[] BS_CONST6 = "myfight\0mnfight\0bmenuno\0";
#define BS_PICK_WAIT 30 // split frames without box text before the line is given anyway (after -1)

static BS_CODE6 void bsPickText(float y) {
    Bs* bs = bsState.bs;
    if (bs != nullptr && y >= bs->splitY && bs->pickSeamY > y) bs->boxText++;
}
static BS_CODE6 void bsDrawText6(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                 float lineSeparation) {
    bsDrawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation);
    bsPickText(y);
}
static BS_CODE6 void bsDrawTextColor6(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                      int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    bsDrawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
    bsPickText(y);
}
static BS_CODE6 void bsDrawTextUI6(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                   int32_t c1, int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    bsDrawTextUI(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
    bsPickText(y);
}

// In drawViews, in place of CtrRenderer_bsClearLayer (bsInstall has just filled the outer table): the frame's count
// starts, and the text draws count on their way through. (battlesplit14: bsClearLayer14 first, in place of bsClearLayer9.)
static void bsClearLayerE(Renderer* renderer); // the SOUL's entry (W1, this file's end): bsClearLayer14 outside it
static BS_CODE6 void bsClearLayer6(Renderer* renderer) {
    bsClearLayerE(renderer);
    Bs* bs = bsState.bs;
    if (bs == nullptr) return;
    bs->boxText = 0;
    bs->pickSeamY = bs->viewTop + (bs->viewBottom - bs->viewTop) * ((float) BS_C_SEAM / 240.0f);
    bs->outer.drawText = bsDrawText6;
    bs->outer.drawTextColor = bsDrawTextColor6;
    bs->outer.drawTextUI = bsDrawTextUI6;
}

// IntRValueHashMap_findSlot's lookup, copied: a second call of the header's inline function made GCC emit it out of
// line in .text.* and stop inlining it into bsGroup, which moved bsGroup and everything after it.
static BS_CODE6 RValue* bsPickFind(IntRValueHashMap* map, int32_t key) {
    if (map->capacity == 0) return nullptr;
    uint32_t mask = map->mask, idx = ((uint32_t) key * 0x9E3779B9u) & mask;
    while (true) {
        int32_t slotKey = map->entries[idx].key;
        if (slotKey == key) return &map->entries[idx].value;
        if (slotKey == INT_RVALUE_HASHMAP_EMPTY_KEY) return nullptr;
        idx = (idx + 1) & mask;
    }
}

// A global's value as an integer (the three bsPickVars), or -1.
static BS_CODE6 int32_t bsPickGlobal(Bs* bs, Runner* runner, int32_t k) {
    VMContext* ctx = runner->vmContext;
    if (ctx == nullptr || ctx->globalScopeInstance == nullptr) return -1;
    if (bs->pickVm != ctx) {
        const char* n = bsPickVars;
        for (int32_t i = 0; 3 > i; i++, n += strlen(n) + 1) {
            ptrdiff_t slot = shgeti(ctx->varNameMap, (char*) n);
            bs->pickVar[i] = 0 > slot ? -1 : (int32_t) ctx->varNameMap[slot].value;
        }
        bs->pickVm = ctx;
    }
    if (0 > bs->pickVar[k]) return -1;
    RValue* value = bsPickFind(&ctx->globalScopeInstance->selfVars, bs->pickVar[k]);
    if (value == nullptr) return -1;
    if (value->type == RVALUE_REAL) return (int32_t) value->real;
    if (value->type == RVALUE_INT32) return value->int32;
    return -1;
}

// ===[ Menu-pick log: the sampler's constants (v8 below; v2 went in the post-G8 move commit) ]===
#include "ctr_frame_pacer.h"
#define BS_PICK_INSET 8.0f // room px inside each border object's place: past the border's own 5
#define BS_PICK_TEXT 12    // lit pixels inside the box that make box text (a glyph has more)
#define BS_PICK_SEQ 32     // presents recorded

// ===[ The pick log v2's fixes (c26rev5 on P6: F2-F4; F1 is CtrRenderer_bsLeave8's) ]===
// bsPresent8 and bsLeave8, from the two callers v2's bsPresent7 and bsLeave7 had (v2 went in the post-G8 move
// commit). F2: the bottom screen's buffer is read (bsPickSample8) only in test builds, on the pen log's switch
// (CTRPORT_PEN_LOG: CMakeLists.txt defines it only without
// the release source URL), so a public build's new code neither has the sampler nor calls it; its line names no screen
// classes and, as v2's with nothing read, comes at BS_PICK_WAIT. F3: the classes are named as sampled: K is no bright
// pixel on a coarse grid, T possible box text (a white bullet in the box counts too); not pixel or frame proof. F4: one
// piece or two is the seam at the pick, not at the line (a group change can change the seam; in the bsplace test
// build, so can L). Test builds also say when the seam at the line differs ("one piece, two at the end"). That
// call is also what keeps the bsplace test build's function order as dv6's: GCC orders this file's functions by
// its call graph, and without a CtrBattlesplit_seam call on the split end's path (v2's bsPickEnd7 had one)
// .text_battlesplit and the log strings in .rodata reorder in that build. Own sections
// (.rodata_battlesplit8, .text_battlesplit8).
#define BS_CODE8 __attribute__((noinline, noipa, section(".text_battlesplit8")))
#define BS_CONST8 __attribute__((section(".rodata_battlesplit8")))
#ifdef CTRPORT_PEN_LOG
static const char bsLogPick8[] BS_CONST8 =
    "battlesplit: room %d group %d: menu pick %d (%s) on frame %d (split frame %u): first box-row text call after %d "
    "frames (%u frames before it with no layer switch); bottom from the pick, sampled each present: %s (sampled "
    "classes: K no bright pixel on a coarse grid, E box without text, T possible box text, ? not read or no box); "
    "bottom in %s%s\n";
static const char bsPickSeen8[4] __attribute__((section(".rodata_battlesplitpen"))) = { '?', 'K', 'E', 'T' };
#else
static const char bsLogPick8[] BS_CONST8 =
    "battlesplit: room %d group %d: menu pick %d (%s) on frame %d (split frame %u): first box-row text call after %d "
    "frames (%u frames before it with no layer switch); bottom in %s%s\n";
#endif
static const char bsPickWhy8[4][40] BS_CONST8 = { "", "; not finished: the group changed", "; not finished: the split ended",
                                                  "; not finished: another pick" };
static const char bsPickHow8[3][7] BS_CONST8 = { "script", "A", "pen" };
#ifdef CTRPORT_PEN_LOG
static const char bsPickPieces8[4][27] BS_CONST8 = { "one piece", "two pieces", "one piece, two at the end",
                                                  "two pieces, one at the end" };
#else
static const char bsPickPieces8[2][11] BS_CONST8 = { "one piece", "two pieces" };
#endif

#ifdef CTRPORT_PEN_LOG
// The pick log's first live instance of an object and its screen sampler (test builds only).
static __attribute__((noinline, noipa, section(".text_battlesplitpen"))) Instance* bsLive8(Runner* runner, int32_t o) {
    if (0 > o || runner->instancesByObject == nullptr) return nullptr;
    Instance** list = runner->instancesByObject[o];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (!list[j]->destroyed) return list[j];
    }
    return nullptr;
}

// The bottom screen as it shows now: 1 black, 2 the box without text, 3 box text, 0 not read. The buffer is portrait
// RGB565 (BOTTOM_TRANSFER_FLAGS), each 240-pixel run one screen column, bottom to top. Black: no pixel of a coarse grid
// has a channel above an eighth. Text: green at half or more (white and yellow, not the red SOUL) inside the live box
// (the four border objects, as the detector finds them, BS_PICK_INSET in), every column of every second row, at the
// rows the bottom shows them (lifted above the seam). So a white bullet inside the box counts as text too; after a
// pick the box holds the menu's text, not bullets. No box (no border instance): 0.
static __attribute__((noinline, noipa, section(".text_battlesplitpen"))) int32_t bsPickSample8(const Bs* bs, Runner* runner) {
    u16 fw = 0, fh = 0;
    const u8* fb = CtrPacer_shownFramebuffer(GFX_BOTTOM, GFX_LEFT, &fw, &fh);
    if (fb == nullptr || fw != 240 || fh != 320 || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return 0;
    const uint16_t* px = (const uint16_t*) fb;
    GSPGPU_InvalidateDataCache((void*) fb, 240u * 320u * 2u);
    bool lit = false;
    for (uint32_t x = 0; 320 > x && !lit; x += 4) {
        for (uint32_t y = 0; 240 > y; y += 4) {
            uint16_t c = px[x * 240u + 239u - y];
            if ((c >> 11) >= 4u || ((c >> 5) & 63u) >= 8u || (c & 31u) >= 4u) {
                lit = true;
                break;
            }
        }
    }
    if (!lit) return 1;
    Instance* side[4];
    for (int32_t k = 0; 4 > k; k++) {
        side[k] = bsLive8(runner, bsState.border[k]);
        if (side[k] == nullptr) return 0;
    }
    float perPx = (bs->viewRight - bs->viewLeft) / 320.0f;
    if (!(perPx > 0.0f)) return 0;
    int32_t x0 = (int32_t) ceilf((side[0]->x + BS_PICK_INSET - bs->viewLeft) / perPx);
    int32_t x1 = (int32_t) floorf((side[2]->x - BS_PICK_INSET - bs->viewLeft) / perPx);
    int32_t y0 = (int32_t) ceilf((side[1]->y + BS_PICK_INSET - bs->viewTop) / perPx);
    int32_t y1 = (int32_t) floorf((side[3]->y - BS_PICK_INSET - bs->viewTop) / perPx);
    if (0 > x0) x0 = 0;
    if (x1 > 319) x1 = 319;
    int32_t dy = bs->place.bottomDy, seam = CtrBattlesplit_seam(), text = 0;
    for (int32_t ly = y0; y1 >= ly && BS_PICK_TEXT > text; ly += 2) {
        int32_t y = seam != 0 && ly >= seam ? ly : ly + dy;
        if (0 > y || y >= 240) continue;
        for (int32_t x = x0; x1 >= x; x++) {
            if (((px[(uint32_t) x * 240u + 239u - (uint32_t) y] >> 5) & 63u) >= 32u) text++;
        }
    }
    return text >= BS_PICK_TEXT ? 3 : 2;
}
#endif

// The line for the pending pick (why: 0 finished, 1 the group changed, 2 the split ended, 3 a new pick), then none
// pending.
static BS_CODE8 void bsPickEnd8(Bs* bs, int32_t why) {
#ifdef CTRPORT_PEN_LOG
    char seen[BS_PICK_SEQ + 1];
    uint32_t n = bs->pickShown;
    for (uint32_t i = 0; n > i; i++) seen[i] = bsPickSeen8[(bs->pickSeq[i >> 4] >> ((i & 15u) * 2u)) & 3u];
    seen[n] = '\0';
    uint32_t pieces = bs->pickPieces != 0 ? 1u : 0u;
    if ((CtrBattlesplit_seam() != 0 ? 1u : 0u) != pieces) pieces += 2u;
    logInfo(bsLogPick8, (int) bs->room, (int) bs->pickGroup, (int) bs->pickMenu, bsPickHow8[bs->pickHow], (int) bs->pickFrame,
            (unsigned) bs->pickAt, (int) bs->pickTextAt, (unsigned) bs->pickBlack, seen, bsPickPieces8[pieces],
            bsPickWhy8[why]);
#else
    logInfo(bsLogPick8, (int) bs->room, (int) bs->pickGroup, (int) bs->pickMenu, bsPickHow8[bs->pickHow], (int) bs->pickFrame,
            (unsigned) bs->pickAt, (int) bs->pickTextAt, (unsigned) bs->pickBlack, bsPickPieces8[bs->pickPieces != 0 ? 1 : 0],
            bsPickWhy8[why]);
#endif
    bs->pickAt = 0;
}

// In endFrame: the frame presented, then the pick looked for, (test builds) the screen looked
// at, the line.
static BS_CODE8 void bsPresent8(Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy) {
    CtrRenderer_bsPresent5(renderer, band, topDy, bottomDy);
    Bs* bs = bsState.bs;
    Runner* runner = renderer->runner;
    if (bs == nullptr || runner == nullptr) return;
    if (bs->pickGroup != bs->group) {
        if (bs->pickAt != 0) bsPickEnd8(bs, 1);
        bs->pickGroup = (int16_t) bs->group;
        bs->pickRow = false;
    }
    int32_t fight = bsPickGlobal(bs, runner, 0), mn = bsPickGlobal(bs, runner, 1), menu = bsPickGlobal(bs, runner, 2);
    if (bs->pickRow && fight == 0 && mn == 0 && 1 <= menu && menu <= 4) {
        uint32_t down = hidKeysDown(), touch = down | hidKeysHeld() | hidKeysUp();
        if (bs->pickAt != 0) bsPickEnd8(bs, 3);
        bs->pickAt = bs->frames;
        bs->pickFrame = runner->frameCount;
        bs->pickBlack = 0;
        bs->pickMenu = (int16_t) menu;
        bs->pickHow = (down & KEY_A) != 0 ? 1 : (touch & KEY_TOUCH) != 0 ? 2 : 0;
        bs->pickPieces = CtrBattlesplit_seam() != 0 ? 1 : 0;
        bs->pickSeq[0] = bs->pickSeq[1] = 0;
        bs->pickShown = 0;
        bs->pickTextAt = -1;
    }
    bs->pickRow = fight == 0 && mn == 0 && menu == 0;
    if (bs->pickAt == 0) return;
    int32_t after = (int32_t) (bs->frames - bs->pickAt);
    if (0 > bs->pickTextAt) {
        if (bs->boxText != 0) bs->pickTextAt = (int8_t) after;
        else if (bs->switches == 0) bs->pickBlack++;
    }
#ifdef CTRPORT_PEN_LOG
    int32_t seen = bsPickSample8(bs, runner);
    if (BS_PICK_SEQ > bs->pickShown) {
        bs->pickSeq[bs->pickShown >> 4] |= (uint32_t) seen << ((bs->pickShown & 15u) * 2u);
        bs->pickShown++;
    }
    if (seen == 3 && 0 <= bs->pickTextAt && after >= bs->pickTextAt + 2) {
        bsPickEnd8(bs, 0);
        return;
    }
#endif
    if (after >= BS_PICK_WAIT) bsPickEnd8(bs, 0);
}

// As the split ends: a pending pick's line first, then CtrRenderer_bsLeave8 (ctr_renderer.c, declared here only).
void CtrRenderer_bsLeave8(Renderer* renderer, bool on);
static BS_CODE8 void bsLeave8(Renderer* renderer, bool on) {
    Bs* bs = bsState.bs;
    if (bs != nullptr && bs->pickAt != 0) bsPickEnd8(bs, 2);
    CtrRenderer_bsLeave8(renderer, on);
}

// ===[ Captures of the box (Muffet's fake border) ]===
// sprite_create_from_surface of the application surface on a split frame: the box's rows are drawn on the arena layer,
// so a capture of them from the application surface's half view would read black. CtrRenderer_bsSpriteFrom9
// (ctr_renderer.c) reads the layer instead when the region reaches below the split (splitY), else the half view as
// before; only for her fake border's own capture (group 56, obj_fakeborderdraw drawing), any other capture goes
// straight to the inner table, as with no hook. The hook is put in the outer table by the layer's clear (bsClearLayer9 in place of
// CtrRenderer_bsClearLayer in bsClearLayer6, same call); off a split frame the outer table is not in use. Own sections
// (.rodata_battlesplit9, .text_battlesplit9), appended after the pick log v2's fixes, so nothing before them moves.
#define BS_CODE9 __attribute__((noinline, noipa, section(".text_battlesplit9")))
static const char bsLogCapture9[] BS_CONST9 =
    "battlesplit: room %d group %d: a capture of the application surface (%d,%d %dx%d) reads the %s\n";
static const char bsCaptureFrom9[2][16] BS_CONST9 = { "half view", "arena layer" };
int32_t CtrRenderer_bsSpriteFrom9(Renderer* renderer, RendererVtable* inner, float split, int32_t surfaceID, int32_t x, int32_t y, int32_t w,
                                  int32_t h, bool removeback, bool smooth, int32_t xorig, int32_t yorig, int32_t* layer);

// Only Muffet's fake border is read from the layer (c26rev5 F1): every other capture keeps the PC's source, the half
// view. Undyne's death wave (group 47, obj_undynesiner2) captures her top-layer body in a region that reaches below the
// split; the layer has black there.
static bool bsCapMuffet9(Bs* bs, Runner* runner);

static BS_CODE9 int32_t bsSprite9(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback,
                                  bool smooth, int32_t xorig, int32_t yorig) {
    Bs* bs = bsState.bs;
    RendererVtable* inner = bs != nullptr ? bs->inner : nullptr;
    if (inner == nullptr || inner->createSpriteFromSurface == nullptr) return -1;
    Runner* runner = renderer->runner;
    if (!bsState.armed || runner == nullptr || surfaceID != runner->applicationSurfaceId || bs->viewTop >= bs->viewBottom ||
        !bsCapMuffet9(bs, runner))
        return inner->createSpriteFromSurface(renderer, surfaceID, x, y, w, h, removeback, smooth, xorig, yorig);
    int32_t layer = 0;
    float split = (bs->splitY - bs->viewTop) / (bs->viewBottom - bs->viewTop);
    int32_t sprite = CtrRenderer_bsSpriteFrom9(renderer, inner, split, surfaceID, x, y, w, h, removeback, smooth, xorig, yorig, &layer);
    uint8_t bit = (uint8_t) (layer != 0 ? 2 : 1);
    if ((bs->capLogged & bit) == 0) {
        bs->capLogged |= bit;
        logInfo(bsLogCapture9, (int) bs->room, (int) bs->group, (int) x, (int) y, (int) w, (int) h, bsCaptureFrom9[layer != 0 ? 1 : 0]);
    }
    return sprite;
}

// In bsClearLayer6, in place of CtrRenderer_bsClearLayer: the layer cleared, then the capture hook in the outer table.
static BS_CODE9 void bsClearLayer9(Renderer* renderer) {
    CtrRenderer_bsClearLayer(renderer);
    Bs* bs = bsState.bs;
    if (bs != nullptr && bs->inner != nullptr && bs->inner->createSpriteFromSurface != nullptr) bs->outer.createSpriteFromSurface = bsSprite9;
}

// The capture's own: Muffet's encounter, and the drawing instance is her fake border. The name is compared by hand:
// with strcmp of this constant, GCC lays out four functions of the first battlesplit block and six of its constants
// in another order (base addresses move).
#define BS_MUFFET 56
static const char bsCapOwner9[] BS_CONST9 = "obj_fakeborderdraw";
static BS_CODE9 bool bsCapMuffet9(Bs* bs, Runner* runner) {
    if (bs->group != BS_MUFFET || runner->vmContext == nullptr) return false;
    Instance* inst = runner->vmContext->currentInstance;
    if (inst == nullptr || runner->dataWin == nullptr || 0 > inst->objectIndex || (uint32_t) inst->objectIndex >= runner->dataWin->objt.count)
        return false;
    const char* name = runner->dataWin->objt.objects[inst->objectIndex].name;
    if (name == nullptr) return false;
    const char* want = bsCapOwner9;
    while (*want != 0 && *name == *want) name++, want++;
    return *name == *want;
}

// ===[ The scene table (change layout S3) ]===
// The admissions, places and encounter tables from the scene table (ctr_layout.h's S3: romfs:/layout/undertale.lst,
// which build.sh resolves from tools/layout/undertale-scenes.json against the player's data.win). Without a good one
// (no every-room row: no file, or one that failed a check) no encounter is admitted: every battle is Original (lead
// 11-09; the compiled tables that were the fallback went in the post-G8 move commit). A scene with a placement
// mode other than 0 to BS_MODE_LAST (1 to 5: the close seam, battlesplit14; 6 and 7: z_bsgroup.c's; 8: z_bsmex.c's)
// is not admitted.
// Its own section, appended after layout's (late_reserve.ld), so nothing before it moves.
#include "ctr_layout.h"
#define BS_MODE_LAST 8
#define BS_CODE_S3 __attribute__((noinline, noipa, section(".text_bsscene")))

// The encounter's scene (room_battle, group) with placement mode 0 to BS_MODE_LAST, or nullptr; *any: the every-room
// row, nullptr without a good table.
static BS_CODE_S3 const CtrLayoutScene* bsSceneS3(DataWin* dw, int32_t group, const CtrLayoutScene** any) {
    *any = CtrLayout_scene(dw, -1, -1);
    if (*any == nullptr || 0 > group || 0 > bsState.battleRoom) return nullptr;
    const CtrLayoutScene* s = CtrLayout_scene(dw, bsState.battleRoom, group);
    return s != nullptr && BS_MODE_LAST >= s->mode ? s : nullptr;
}

// Admitted (split) or a bench run's battle_probe bit 2 (the encounter has entries of its own); no table: neither.
static BS_CODE_S3 bool bsAdmitS3(int32_t group, bool split) {
    const CtrLayoutScene* any;
    const CtrLayoutScene* s = bsSceneS3(bsState.namesFor, group, &any);
    return s != nullptr && (split ? (s->flags & CTR_LAYOUT_SCENE_SPLIT) != 0 : s->count != 0);
}

static BS_CODE_S3 bool bsGroupSplitsS3(int32_t group) {
    return bsAdmitS3(group, true);
}

static BS_CODE_S3 bool bsHasTableS3(int32_t group) {
    return bsAdmitS3(group, false);
}

static BS_CODE_S3 bool bsPlaceS3(int32_t layout, int32_t group, CtrLayoutPlace* out) {
    const CtrLayoutScene* any;
    const CtrLayoutScene* s = bsSceneS3(bsState.namesFor, group, &any);
    int32_t band = BS_BAND_STANDARD, up = BS_A_UP;
    if (s != nullptr) {
        if (s->band != 0) band = s->band;
        if (s->lift != CTR_LAYOUT_LIFT_DEFAULT) up = s->lift;
    }
    if ((bsProbe() & 16) != 0) up = BS_A_UP; // the guard's test: every group lifted
    if (!CtrLayout_place(layout, band, out)) return false;
    if (layout == CTR_BATTLE_LAYOUT_A) out->bottomDy = (int16_t) -up;
    return true;
}

// A scene's entries over the classes: the bits in mask from the entry, BS_OWN set.
static BS_CODE_S3 void bsEntriesS3(uint8_t* cls, const CtrLayoutScene* s, uint32_t mask) {
    if (s == nullptr) return;
    const CtrLayoutEntry* e = CtrLayout_entries(s);
    for (uint32_t k = 0; s->count > k; k++) {
        uint8_t* c = &cls[e[k].object];
        *c = (uint8_t) ((*c & ~mask) | (e[k].cls & mask) | BS_OWN);
    }
}

// The classes from the table's rows: the every-room row's, then the hazard ledger (a hazard without an entry is
// bottom), then the encounter's entries over both. Indices checked when the table was read.
static BS_CODE_S3 bool bsBuildClassesS3(Bs* bs, DataWin* dw, int32_t group) {
    const CtrLayoutScene* any;
    const CtrLayoutScene* s = bsSceneS3(dw, group, &any);
    uint32_t n = dw->objt.count;
    uint8_t* cls = (uint8_t*) calloc(n != 0 ? n : 1u, 1);
    if (cls == nullptr) return false;
    bsEntriesS3(cls, any, BS_CLASS | BS_SOUL);
    bsLedger(cls, dw);
    uint32_t added = 0;
    for (uint32_t i = 0; n > i; i++) {
        if ((cls[i] & (BS_HAZARD | BS_OWN)) != BS_HAZARD) continue;
        if (bsInherit(cls, dw, i, BS_OWN) != BS_BOTTOM) added++;
        cls[i] = (uint8_t) ((cls[i] & ~3u) | BS_BOTTOM | BS_OWN);
    }
    bsEntriesS3(cls, s, BS_CLASS);
    for (uint32_t i = 0; n > i; i++) cls[i] = (uint8_t) ((cls[i] & BS_OWN) | bsInherit(cls, dw, i, BS_OWN));
    for (uint32_t i = 0; n > i; i++) cls[i] &= BS_CLASS;
    if (!bsState.ledgerLogged) {
        bsState.ledgerLogged = true;
        logInfo(bsLogLedger, (unsigned) added);
    }
    bs->cls = cls;
    bs->count = n;
    return true;
}

// ===[ A room's own split scene (the first Flowey, room_floweybattle), battlesplit10: own section, appended ]===
// maintainer (11-08): the opening tutorial splits like the other fights: Flowey on top, the box and the SOUL on the bottom,
// centred; a pellet may be drawn on the bottom while it starts above. The room is no encounter (global.battlegroup
// says nothing there), so the scene table gives it a scene of its own, the room alone (room_floweybattle, no group:
// its band, lift and entries in tools/layout/undertale-scenes.json). A room with such a scene, split and placement
// mode 0, counts as a battle room (bsTouchRoom10, in place of bsTouchRoom where the room changes) and its group is
// BS_ROOM_SCENE (bsGroup10, in place of bsGroup), past the groups' bits, which only bsGroupSplits10 lets in. Its place
// and classes are the encounter's way (bsPlaceS3, bsBuildClassesS3) with the room's scene. Without a good table there
// is no room scene: the room stays Original.
#define BS_CODE10 __attribute__((noinline, noipa, section(".text_battlesplit10")))
#define BS_ROOM_SCENE 307

// The room's own scene, split and with placement mode 0, or nullptr; nullptr without a good table (no every-room row,
// as bsSceneS3).
static BS_CODE10 const CtrLayoutScene* bsRoomScene10(DataWin* dw, int32_t room) {
    if (dw == nullptr || 0 > room || CtrLayout_scene(dw, -1, -1) == nullptr) return nullptr;
    const CtrLayoutScene* s = CtrLayout_scene(dw, room, -1);
    return s != nullptr && s->mode == 0 && (s->flags & CTR_LAYOUT_SCENE_SPLIT) != 0 ? s : nullptr;
}

// bsTouchRoom, and a room with its own split scene is a battle room. Called once a room change, after roomBattle is set.
static BS_CODE10 int32_t bsTouchRoom10(Runner* runner, int32_t room) {
    if (bsRoomScene10(runner->dataWin, room) != nullptr) bsState.roomBattle = true;
    return bsTouchRoom(runner, room);
}

// bsGroup, or BS_ROOM_SCENE in a room with its own split scene.
static BS_CODE10 int32_t bsGroup10(Runner* runner) {
    int32_t room = runner->currentRoomIndex;
    if (room != bsState.battleRoom && bsRoomScene10(runner->dataWin, room) != nullptr) return BS_ROOM_SCENE;
    return bsGroup(runner);
}

// The room scene of the room the split is in (bsState.lastRoom: set before the group is read), for BS_ROOM_SCENE.
static BS_CODE10 const CtrLayoutScene* bsRoomNow10(DataWin* dw, int32_t group) {
    return group == BS_ROOM_SCENE ? bsRoomScene10(dw, bsState.lastRoom) : nullptr;
}

static BS_CODE10 bool bsGroupSplits10(int32_t group) {
    if (group == BS_ROOM_SCENE) return bsRoomNow10(bsState.namesFor, group) != nullptr;
    return bsGroupSplitsS3(group);
}

// The encounter's place with its close seam (battlesplit14, at the end of this file), in place of bsPlaceS3.
static bool bsPlace14(int32_t layout, int32_t group, CtrLayoutPlace* out);
static bool bsBuildClasses14(Bs* bs, DataWin* dw, int32_t group);

// bsPlaceS3's place from the room's scene for BS_ROOM_SCENE, else bsPlace14's (in place of bsPlaceS3 in its two
// callers, same calls).
static BS_CODE10 bool bsPlace10(int32_t layout, int32_t group, CtrLayoutPlace* out) {
    const CtrLayoutScene* s = bsRoomNow10(bsState.namesFor, group);
    if (s == nullptr) return bsPlace14(layout, group, out);
    int32_t band = BS_BAND_STANDARD, up = BS_A_UP;
    if (s->band != 0) band = s->band;
    if (s->lift != CTR_LAYOUT_LIFT_DEFAULT) up = s->lift;
    if ((bsProbe() & 16) != 0) up = BS_A_UP; // the guard's test: every group lifted
    if (!CtrLayout_place(layout, band, out)) return false;
    if (layout == CTR_BATTLE_LAYOUT_A) out->bottomDy = (int16_t) -up;
    return true;
}

// bsBuildClassesS3's classes with the room's scene for BS_ROOM_SCENE: the every-room row, the ledger and its hazards,
// the room's entries, the parents. Else bsBuildClassesS3's (in place of it in its two callers, same calls).
static BS_CODE10 bool bsBuildClasses10(Bs* bs, DataWin* dw, int32_t group) {
    const CtrLayoutScene* s = bsRoomNow10(dw, group);
    const CtrLayoutScene* any = s != nullptr ? CtrLayout_scene(dw, -1, -1) : nullptr;
    if (any == nullptr) return bsBuildClasses14(bs, dw, group);
    uint32_t n = dw->objt.count;
    uint8_t* cls = (uint8_t*) calloc(n != 0 ? n : 1u, 1);
    if (cls == nullptr) return false;
    bsEntriesS3(cls, any, BS_CLASS | BS_SOUL);
    bsLedger(cls, dw);
    uint32_t added = 0;
    for (uint32_t i = 0; n > i; i++) {
        if ((cls[i] & (BS_HAZARD | BS_OWN)) != BS_HAZARD) continue;
        if (bsInherit(cls, dw, i, BS_OWN) != BS_BOTTOM) added++;
        cls[i] = (uint8_t) ((cls[i] & ~3u) | BS_BOTTOM | BS_OWN);
    }
    bsEntriesS3(cls, s, BS_CLASS);
    for (uint32_t i = 0; n > i; i++) cls[i] = (uint8_t) ((cls[i] & BS_OWN) | bsInherit(cls, dw, i, BS_OWN));
    for (uint32_t i = 0; n > i; i++) cls[i] &= BS_CLASS;
    if (!bsState.ledgerLogged) {
        bsState.ledgerLogged = true;
        logInfo(bsLogLedger, (unsigned) added);
    }
    bs->cls = cls;
    bs->count = n;
    return true;
}

// ===[ Napstablook's tears, the (b) fights without a lift, and the lift's guard without a move: battlesplit11 ]===
// maintainer (11-08, dv7, Old 3DS): when Napstablook cried upward to make his hat, the box on the bottom screen jumped down,
// and his tears were on the bottom screen, not with him on top. The tears (blt_crybullet, his first attack;
// blt_streambullet, the streams; blt_crybullet2, the hat's) are hazards, so they were bottom: drawn on the bottom
// screen above the box, and the hat's, which rise to room y 79, reached the rows preset A's lift hides, so the guard
// put the bottom back to its PC place for the rest of the battle. Ruled: the box never moves mid-battle.
// - The tears are on both screens (the scene table's entries for his encounter, 20): with him on the top screen while
//   above the band, and on the bottom where the PC has them. By call lost them: a falling tear inside the box was on
//   neither screen (lift 0 pictures, f13t), so they are both, which needs no place. None have children.
// - The buttons and the HP line sit at their PC rows in every split fight (maintainer, 11-08: they were raised in some). A
//   fight in bsSeamB had placement (b), the whole bottom moved up by its lift, buttons too, because its shots cross
//   BS_C_SEAM and (c) would tear them there. Those fights now have no lift in the scene table (lift 0): the bottom is
//   the PC's lower half in one piece (no seam, no hidden rows), the box at its PC place. Toriel's fight (22) joins
//   them: its low-HP fire crossed the seam in maintainer's 11-08 playtest.
// - The guard no longer moves the bottom. A bottom draw that reaches the hidden rows makes its object BS_BOTH for the
//   rest of the battle (the top shows the rows above the band, the bottom what it can), with one log line. That draw
//   itself stays where it was routed.
#define BS_CONST11 __attribute__((section(".rodata_battlesplit11")))
// bsGuard11 without noipa: bsRouteAt (BS_CODE, inlined calls analysed) calls it, and with noipa the draw calls that
// call bsRouteAt compiled smaller, so everything after them moved (128 B on Old). Without it they are byte for byte
// the same, only the call's target differs.
#define BS_CODE11G __attribute__((noinline, section(".text_battlesplit11")))
static const char bsLogGuard11[] BS_CONST11 = "battlesplit: room %d group %d: %s draws in the %d rows the lift hides (%d,%d to %d,%d): on both "
                                              "screens for the rest of the battle\n";

// In place of bsGuard in bsRouteAt (same call): a draw that does not reach the hidden rows goes on to bsGuard (its
// seam log; it returns before the move); one that does makes its object BS_BOTH.
static BS_CODE11G void bsGuard11(Bs* bs, const float* r) {
    if (r[0] > r[2] || r[1] >= bs->hideY || bs->viewTop >= r[3] || r[0] >= bs->viewRight || bs->viewLeft >= r[2]) {
        bsGuard(bs, r);
        return;
    }
    int32_t object = bs->lastObject;
    if (0 > object || bs->count <= (uint32_t) object) return;
    bs->cls[object] = (uint8_t) ((bs->cls[object] & ~(uint8_t) BS_CLASS) | BS_BOTH);
    bs->lastWant = BS_BOTH; // the class cache: the same instance's next draws
    const char* name = bs->renderer->dataWin != nullptr ? bs->renderer->dataWin->objt.objects[object].name : nullptr;
    logInfo(bsLogGuard11, (int) bs->room, (int) bs->group, name != nullptr ? name : bsUnnamed, (int) -bs->place.bottomDy, (int) r[0],
            (int) r[1], (int) r[2], (int) r[3]);
}

// ===[ The router and the runs (c26rev5's F1 and F2 on battlesplit11), battlesplit11f: own section, appended ]===
// - BY_CALL across the band: a draw that starts above the standard line goes up, and the top screen shows the arena
//   down to the band only. One that also reaches below the band now goes on both screens (each shows its part), so
//   no rows of it are lost (c26rev5's F1, rows 249-261 with Flowey's band ending at 260).
// - The guard's own draw: bsGuard11 makes the object BS_BOTH from its next draw on; the draw that showed it reaches
//   the hidden rows now goes on both screens too, the box still where it was (c26rev5's F2).
// - The runs (runner.c's utkill): a run's later instances are drawn onto the layer of the run's first draw, without
//   the table's wrappers, so a class decided by the draw's place (BY_ROW, BY_CALL, COVER, and a bottom draw the
//   guard looks at) was decided once for the whole run (c26rev5's F1: a second friendly pellet below the band stayed
//   on the top layer). Each later draw is now routed as the wrappers would route it; one that goes elsewhere than
//   the run's layer leaves the run and takes the full way, which routes it. Draws whose class does not depend on the
//   place (TOP, BOTTOM without the guard) are not looked at; BS_BOTH draws never prove plain (two draws).
#define BS_CODE11F __attribute__((noinline, section(".text_battlesplit11f")))
#define BS_CODE11FN __attribute__((noinline, noipa, section(".text_battlesplit11f")))

// BY_CALL's layer for a draw at r: up when its place is not known or it starts above the standard line and ends
// within the band; on both when it starts above the line and reaches below the band; else down.
static BS_CODE11F int32_t bsByCall11f(const Bs* bs, const float* r) {
    if (r[0] > r[2]) return BS_TOP;
    if (r[1] >= bs->splitY) return BS_BOTTOM;
    return r[3] > bs->bandY ? BS_BOTH : BS_TOP;
}

// In place of bsRouteAt in BS_ROUTED and bsPrimitiveEnd (same calls): a BY_CALL draw across the band, and a guarded
// bottom draw the guard makes BS_BOTH, on both screens; everything else as bsRouteAt routes it. Without noipa: old
// code calls it, as it called bsRouteAt (which has none). It calls only bsByCall11f and bsRouteAt (the guard through
// bsRouteAt, as before), so the compiler's order of the old functions, and of their constants, stays.
static BS_CODE11F bool bsRouteAt11f(Renderer* renderer, int32_t want, const float* r) {
    Bs* bs = bsState.bs;
    if (want >= BS_BY_CALL && (want & BS_CLASS) == BS_BY_CALL && bsByCall11f(bs, r) == BS_BOTH) return bsBoth17(renderer, BS_BOTH, r);
    bool both = bsRouteAt56(renderer, want, r); // group 56: bsRouteAt, and in mode 5 a BS_BOTH copy cut
    // bsRouteAt's guard (bsGuard11) made the object BS_BOTH on this draw: it goes on both too, the top first.
    if ((want & BS_GUARD) != 0 && (want & BS_CLASS) == BS_BOTTOM && bs->lastWant == BS_BOTH) return bsBoth17(renderer, BS_BOTH, r);
    return both;
}

#if defined(UTK_PLAIN_RUNS)
// ctr_renderer.c: the run's kept draw (its first): its texture item, and in f its origin, scale and angle.
int32_t CtrUtk_bsRunDraw11f(float* f);
bool __real_CtrUtk_plainAt(Renderer* renderer, float x, float y);
int32_t __real_CtrUtk_plainBatch(Renderer* renderer, const float* xy, int32_t n);
bool __real_CtrUtk_turnedKept(float x, float y, float angleDeg);
bool __real_CtrUtk_turnedFast(Renderer* renderer, float x, float y, float angleDeg);
bool __wrap_CtrUtk_plainAt(Renderer* renderer, float x, float y);
int32_t __wrap_CtrUtk_plainBatch(Renderer* renderer, const float* xy, int32_t n);
bool __wrap_CtrUtk_turnedKept(float x, float y, float angleDeg);
bool __wrap_CtrUtk_turnedFast(Renderer* renderer, float x, float y, float angleDeg);

// The split's state when the run's later draws need routing: a split frame's room draws (the outer table in front),
// the run's object (the class cache's: the run's first draw set it) of a class that depends on the place.
static BS_CODE11FN Bs* bsRunWatch11f(Renderer* renderer) {
    Bs* bs = bsState.bs;
    if (!bsState.armed || bs == nullptr || renderer == nullptr || renderer != bs->renderer || renderer->vtable != &bs->outer) return nullptr;
    int32_t object = bs->lastObject;
    if (0 > object || bs->count <= (uint32_t) object) return nullptr;
    int32_t c = bs->cls[object] & BS_CLASS;
    if (c == BS_TOP || c == BS_BOTH || (c == BS_BOTTOM && !(bs->hideY > bs->viewTop))) return nullptr;
    return bs;
}

// bsSpritePlace's place, the same arithmetic in the same order (a copy: a new call to bsSpritePlace would change how
// the compiler specialises it for the old wrappers' calls, and with that their code).
static BS_CODE11FN void bsSpritePlace11f(float* r, Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
                                         float xscale, float yscale, float angleDeg) {
    DataWin* dw = renderer->dataWin;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    if (dw != nullptr && 0 <= tpagIndex && dw->tpag.count > (uint32_t) tpagIndex) {
        TexturePageItem* t = &dw->tpag.items[tpagIndex];
        x0 = ((float) t->targetX - originX) * xscale;
        y0 = ((float) t->targetY - originY) * yscale;
        x1 = x0 + (float) t->targetWidth * xscale;
        y1 = y0 + (float) t->targetHeight * yscale;
    }
    float c = 1.0f, s = 0.0f;
    if (angleDeg != 0.0f) {
        float a = angleDeg * 0.017453292f;
        c = cosf(a);
        s = sinf(a);
    }
    const float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
    for (int32_t k = 0; 4 > k; k++) {
        float px = x + xs[k] * c + ys[k] * s, py = y - xs[k] * s + ys[k] * c;
        if (k == 0) {
            r[0] = r[2] = px;
            r[1] = r[3] = py;
            continue;
        }
        if (r[0] > px) r[0] = px;
        if (px > r[2]) r[2] = px;
        if (r[1] > py) r[1] = py;
        if (py > r[3]) r[3] = py;
    }
}

// Whether the run's later draw at (x, y), turned by angleDeg, goes to another layer than the run's: bsWant's and
// bsRouteAt11f's choice for it (the place is bsSpritePlace's, as the sprite wrapper's), the guard's test for a guarded
// bottom draw (true when it reaches the hidden rows: the full way runs the guard).
static BS_CODE11FN bool bsRunLeaves11f(Bs* bs, float x, float y, float angleDeg, bool turned) {
    int32_t c = bs->cls[bs->lastObject] & BS_CLASS;
    if (c == BS_BY_ROW) c = y >= bs->splitY ? BS_BOTTOM : BS_TOP;
    if (c >= BS_BY_CALL || (c == BS_BOTTOM && bs->hideY > bs->viewTop)) {
        float f[5], r[4];
        int32_t tpag = CtrUtk_bsRunDraw11f(f);
        bsSpritePlace11f(r, bs->renderer, tpag, x, y, f[0], f[1], f[2], f[3], turned ? angleDeg : f[4]);
        if (c == BS_BY_CALL) {
            c = bsByCall11f(bs, r);
        } else if (c == BS_COVER) {
            bool covers = r[2] >= r[0] && bs->viewLeft + 1.0f >= r[0] && bs->viewTop + 1.0f >= r[1] && r[2] + 1.0f >= bs->viewRight &&
                          r[3] + 1.0f >= bs->viewBottom;
            c = covers ? BS_BOTH : BS_TOP;
        } else if (!(r[0] > r[2] || r[1] >= bs->hideY || bs->viewTop >= r[3] || r[0] >= bs->viewRight || bs->viewLeft >= r[2])) {
            return true;
        }
    }
    if (c == BS_BOTH && !CtrRenderer_bsAppBound(bs->renderer)) c = BS_TOP;
    return c != bs->layer;
}

// runner.c's run draws (link wraps, UNDERTALE only): a later draw that leaves the run is not drawn here (false, or
// not counted), and the runner draws it the full way.
__attribute__((noinline, noipa, section(".text_battlesplit11f"))) bool __wrap_CtrUtk_plainAt(Renderer* renderer, float x, float y) {
    Bs* bs = bsRunWatch11f(renderer);
    if (bs != nullptr && bsRunLeaves11f(bs, x, y, 0.0f, false)) return false;
    return __real_CtrUtk_plainAt(renderer, x, y);
}
__attribute__((noinline, noipa, section(".text_battlesplit11f"))) int32_t __wrap_CtrUtk_plainBatch(Renderer* renderer, const float* xy, int32_t n) {
    Bs* bs = bsRunWatch11f(renderer);
    if (bs == nullptr) return __real_CtrUtk_plainBatch(renderer, xy, n);
    int32_t m = 0;
    while (n > m && !bsRunLeaves11f(bs, xy[2 * m], xy[2 * m + 1], 0.0f, false)) m++;
    return m > 0 ? __real_CtrUtk_plainBatch(renderer, xy, m) : 0;
}
__attribute__((noinline, noipa, section(".text_battlesplit11f"))) bool __wrap_CtrUtk_turnedKept(float x, float y, float angleDeg) {
    Bs* bs = bsState.bs != nullptr ? bsRunWatch11f(bsState.bs->renderer) : nullptr;
    if (bs != nullptr && bsRunLeaves11f(bs, x, y, angleDeg, true)) return false;
    return __real_CtrUtk_turnedKept(x, y, angleDeg);
}
__attribute__((noinline, noipa, section(".text_battlesplit11f"))) bool __wrap_CtrUtk_turnedFast(Renderer* renderer, float x, float y, float angleDeg) {
    Bs* bs = bsRunWatch11f(renderer);
    if (bs != nullptr && bsRunLeaves11f(bs, x, y, angleDeg, true)) return false;
    return __real_CtrUtk_turnedFast(renderer, x, y, angleDeg);
}
#endif

// ===[ The close seam (Napstablook, Mad Dummy: maintainer 11-08/11-09), battlesplit14: in the AAC slot ]===
// A scene's placement mode 1 to 5 (tools/layout/undertale-scenes.json) puts its fight on a close seam under preset A:
// the top screen ends with the band's last row and the bottom screen starts two rows above it (the box's top edge at
// row 2), so what crosses the band goes on across the seam, and the rows from BS_C_SEAM down (the HP line and the
// buttons) stay where they are (CtrBattlesplit_seam: no close-seam group is in bsSeamB). The box never straddles the
// seam:
// - modes 1 and 2: the seam follows the box: each frame's band is the row obj_uborder is on, at most the standard
//   band. When the game resizes its box the top screen's rows slide with its top edge; between, nothing moves.
// - modes 3 and 4: the scene's band, for a fight whose box moves up and down inside an attack (its tallest box top).
// - mode 5 (Muffet, maintainer 11-09): per attack, the close seam following the box as in mode 1, the box centred on the
//   bottom screen (the standard band centred on top, the bottom lifted BS_A_UP, (c)), or the bottom whole; bsPlaceTurn14.
// - modes 2 and 4: the battle grid (obj_battlebg's sprite) is drawn so its last row is the band's last row, wherever
//   the band is, so it stays still on the top screen and none of it lands on the bottom. Its rows that then sit above
//   room row 0 are drawn a second time at the foot of the top layer (rows no screen shows), over black, and copied
//   above the band on the top screen at once (ctr_renderer.c's CtrRenderer_bsStripClear14 and CtrRenderer_bsStripCopy14,
//   14c), before any later draw can land on those rows, so the whole grid shows.
// Presentation only: no instance moves, the draws keep their places in the room. The calls of bsPlaceS3 (bsPlace10's),
// bsBuildClassesS3 (bsBuildClasses10's) and bsClearLayer9 (bsClearLayer6's) are pointed here, the same arguments; bsSceneS3 admits the modes (an in-place edit). CtrBattlesplit_seam names BS_C_SEAM
// on every close seam (bsSeamB). Late code before this section keeps its places only with these exact call edges: a
// call of CtrLayout_scene from drawViews' path, or a static function's address taken here, reorders .text_battlesplit
// and .text_battlesplit10 (GCC's function order follows the call graph), hence bs->mode14 and the extern drawSprite.
#ifdef ENABLE_WAD16 // AAC tenants carry an explicit guard (test_aacslot_shared.py)
#define BS_CODE14 __attribute__((noinline, noipa, section(".aacout.text.battlesplit14")))
#define BS_MODE_FOLLOW 2 // modes 1 and 2 follow the box
#define BS_SEAM_MIN 3    // the band's least rows while following
#define BS_GRID_WIDE 512 // a grid draw is at least this wide (its two items are 609 and 639 texels)
#define BS_MODE_TURN 5   // per attack: centred, following (as 1) or the bottom whole (bsPlaceTurn14)
#define BS_ORIGINAL 6    // a scene entry's class "original" (build_scenes.py): an end trigger (bsEnds16), drawn as top
#define BS_ENDS16 2      // end triggers kept per encounter
_Static_assert(sizeof(((Bs*) 0)->end16) == BS_ENDS16 * sizeof(int16_t), "end16 holds BS_ENDS16 objects");
static const char bsGrid14[] __attribute__((section(".aacout.rodata.battlesplit14"))) = "obj_battlebg";
// Mode 5's attack: obj_spiderb's turnamt as her box narrows is the attack her next turn makes (obj_spiderb's step, at
// mnfight 2: the bullet generator's type, then turnamt + 1; 4, 9 and 15 are the pet's climbs).
static const char bsTurn14[] __attribute__((section(".aacout.rodata.battlesplit14"))) = "obj_spiderb\0turnamt";

// The encounter's close-seam mode (1 to BS_MODE_TURN), 0 for any other place. z_bsgroup.c's modes (6, 7) read 0 here:
// their place and line are set from bsClearLayerF each frame (CtrBsgroup_frameF), so this section's bytes stay.
static BS_CODE14 int32_t bsMode14(DataWin* dw, int32_t group) {
    if (dw == nullptr || 0 > group || 0 > bsState.battleRoom || CtrLayout_scene(dw, -1, -1) == nullptr) return 0;
    const CtrLayoutScene* s = CtrLayout_scene(dw, bsState.battleRoom, group);
    return s != nullptr && BS_MODE_TURN >= s->mode ? s->mode : 0;
}

// The close seam at a band of seam rows: the band's last row is the top screen's last row, and the bottom screen
// starts two rows above it.
static BS_CODE14 void bsSeam14(CtrLayoutPlace* p, int32_t seam) {
    p->bandRows = (int16_t) seam;
    p->topDy = (int16_t) (240 - seam);
    p->bottomDy = (int16_t) (2 - seam);
}

// A close-seam place: preset A with the top screen's last row the band's (CtrLayout_place's own A centres the band).
static BS_CODE14 bool bsClose14(const Bs* bs) {
    return bs != nullptr && bs->layout == CTR_BATTLE_LAYOUT_A && 0 > bs->place.bottomDy && bs->place.topDy + bs->place.bandRows == 240;
}

// bsPlaceS3's place, then the close seam at the scene's band (at most the standard band) for a scene with a close-seam
// mode (not under the guard's test, which lifts every group BS_A_UP). The scene's band stays preset C's.
static BS_CODE14 bool bsPlace14(int32_t layout, int32_t group, CtrLayoutPlace* out) {
    if (!bsPlaceS3(layout, group, out)) return false;
    int32_t mode = layout == CTR_BATTLE_LAYOUT_A && (bsProbe() & 16) == 0 ? bsMode14(bsState.namesFor, group) : 0;
    if (mode != 0 && mode != BS_MODE_TURN) bsSeam14(out, out->bandRows > BS_BAND_STANDARD ? BS_BAND_STANDARD : out->bandRows);
    return true;
}

// The encounter's end triggers (battlesplit16): every object whose resolved class is "original" (BS_ORIGINAL, which
// bsBuildClassesS3 keeps as it keeps every class under BS_CLASS): one its scene names, or an unlisted child of one.
// Each is drawn as top from here and kept in bs->end16 (index + 1, by index); bsClearLayer14 turns the rest of the
// battle Original once one has an instance, as bsTriggers' objects do (Asgore's ending, group 101's scene: battlecat
// ORIGINAL). Returns how many there are; one past int16_t counts as more than BS_ENDS16. More than BS_ENDS16 could
// leave an ending unwatched (c26rev5's F1 on 16), so bsBuildClasses14 then keeps the encounter Original whole.
static BS_CODE14 uint32_t bsEnds16(Bs* bs) {
    uint32_t k = 0;
    for (uint32_t i = 0; BS_ENDS16 > i; i++) bs->end16[i] = 0;
    for (uint32_t i = 0; bs->cls != nullptr && bs->count > i; i++) {
        if (bs->cls[i] != BS_ORIGINAL) continue;
        bs->cls[i] = BS_TOP;
        if (32767 <= i) k = BS_ENDS16;
        else if (BS_ENDS16 > k) bs->end16[k] = (int16_t) (i + 1);
        k++;
    }
    return k;
}

static const char bsLogEnds16b[] __attribute__((section(".aacout.rodata.battlesplit14"))) =
    "battlesplit: group %d has %u end objects (class original and its unlisted children), over %d: the battle stays Original\n";

// bsBuildClassesS3's classes, then the encounter's close-seam mode for bsClearLayer14 and its end triggers (bsEnds16)
// (in place of bsBuildClassesS3 in bsBuildClasses10, same call). Too many end triggers: the class table freed and
// false, so bsMake gives no split (bsState.failed until the room index changes) and a group change ends the split.
static BS_CODE14 bool bsBuildClasses14(Bs* bs, DataWin* dw, int32_t group) {
    if (!bsBuildClassesS3(bs, dw, group)) return false;
    bs->mode14 = (int8_t) bsMode14(dw, group);
    uint32_t ends = bsEnds16(bs);
    if (BS_ENDS16 >= ends) return true;
    logInfo(bsLogEnds16b, (int) group, (unsigned) ends, (int) BS_ENDS16);
    free(bs->cls);
    bs->cls = nullptr;
    bs->count = 0;
    for (uint32_t i = 0; BS_ENDS16 > i; i++) bs->end16[i] = 0;
    return false;
}

// The drawSprite entry while a grid mode is on: obj_battlebg's draws go up or down to end on the band's last row, and
// the rows above room row 0 again at the foot of the layer, over black, then straight to the top screen (c26rev5's F1:
// copied at present, a later draw low in the room could overwrite them first); every other draw as bsDrawSprite.
// Each grid draw makes black, stages and copies all the rows it reaches, and only those (c26rev5's LOW on 14c: a
// second grid draw that copied rows staged by an earlier one also copied what a draw between them left there). UNDERTALE
// has one grid draw a frame: room_battle places one obj_battlebg, which has no Draw event (one draw_self a step) and
// which no code creates. Extern: see the section's head.
void CtrBattlesplit_drawSprite14(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
                                 float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
BS_CODE14 void CtrBattlesplit_drawSprite14(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
                                           float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    Bs* bs = bsState.bs;
    DataWin* dw = renderer->dataWin;
    Instance* inst = renderer->runner != nullptr ? renderer->runner->vmContext->currentInstance : nullptr;
    const TexturePageItem* t = dw != nullptr && 0 <= tpagIndex && dw->tpag.count > (uint32_t) tpagIndex ? &dw->tpag.items[tpagIndex] : nullptr;
    float perPx = bs != nullptr ? (bs->viewBottom - bs->viewTop) / 240.0f : 0.0f;
    if (t == nullptr || inst == nullptr || BS_GRID_WIDE > t->boundingWidth || yscale != 1.0f || angleDeg != 0.0f || 0.0f >= perPx ||
        0 > inst->objectIndex || dw->objt.count <= (uint32_t) inst->objectIndex || dw->objt.objects[inst->objectIndex].name == nullptr ||
        strcmp(dw->objt.objects[inst->objectIndex].name, bsGrid14) != 0) {
        bsDrawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        return;
    }
    float top = y + (float) t->targetY - originY;
    // Whole layer rows: the grid's lines land on the same rows of the sprite as in the Original, only moved.
    float dy = perPx * floorf((bs->viewTop + perPx * (float) bs->place.bandRows - (top + (float) t->targetHeight)) / perPx);
    bsDrawSprite(renderer, tpagIndex, x, y + dy, originX, originY, xscale, yscale, angleDeg, color, alpha);
    int32_t rows = (int32_t) ceilf((bs->viewTop - (top + dy)) / perPx);
    if (0 >= rows || rows > bs->place.topDy) return;
    // The layer's last rows this draw reaches: black first, as above room row 0 in the Original (the frame's earlier
    // draws may sit there).
    float y0 = bs->viewTop + perPx * (float) (240 - rows), y1 = bs->viewTop + perPx * 240.0f;
    if (!CtrRenderer_bsStripClear14(renderer, bs->viewLeft, y0, bs->viewRight, y1)) return;
    // The same draw 240 rows lower: its rows above room row 0 land on the layer's last rows. The layer is the top one
    // (obj_battlebg is a top object), and the draw goes through the renderer's own table, as the game's did. Then on
    // the top screen at once.
    bs->inner->drawSprite(renderer, tpagIndex, x, y + dy + perPx * 240.0f, originX, originY, xscale, yscale, angleDeg, color, alpha);
    CtrRenderer_bsStripCopy14(renderer, rows, bs->place.topDy);
}

// Mode 5's attacks, by obj_spiderb's turnamt as her box narrows. The Muffet census (a scratch build logging, per frame
// and object, the draws within 40 rows of obj_uborder; sweep of turnamt 0-15, Old, Steam) saw a single draw cross the
// box's top edge only in attack 0 (the purple poured from her hands that turns the SOUL) and the pet's climbs
// 4, 9 and 15 (the box itself, obj_fakeborderdraw, and the spiders coming down):
// - BS_TURN_CENTRED, the others: nothing reaches between her and the box, so the box stays centred.
// - BS_TURN_WHOLE, the climbs: the shaken box (obj_fakeborderdraw's capture, tilted) reaches room y 415, below BS_C_SEAM
//   (392), where (c) would leave its corner at the HP line while the rest moves up (and a close seam's would cross the
//   seam under her), so the bottom is the PC's lower half in one piece, as before mode 5; the spiders come down on the
//   bottom screen, where the box climbs to.
// - attack 0: the close seam, following the box (11-12: centred, its pour would leap from the top
//   screen's last rows to the bottom screen's first; the hinge-gap paper takes that crossing). None (no obj_spiderb or
//   no counter): the close seam too.
// - Any later value, 16 to 31 (bits 16 to 31 of BS_TURN_CENTRED): centred (maintainer 11-12, group 56 item 5: the receipt
//   turn's box was not centred; it is not attack 0, so it narrowed at a value past 15, which took the close seam).
//   Attacks 16 to 19 come only if the telegram never does (obj_spiderbulletgen: 16 makes the vertical spiders of the
//   climbs' kind, from two strings above the box, without a climb; 17 to 19 make nothing); 20 is none.
#define BS_TURN_CENTRED 0xffff7deeu
#define BS_TURN_WHOLE 0x8210u
#define BS_TURN_WIDE 32.0f    // the menu box's left edge (room x)
#define BS_TURN_NARROW 197.0f // her attacks' box's (its left edge moves only as it narrows or widens; her climbs move the others)

// strcmp's equal without a call of strcmp, and below no call of bsProbe or CtrBench_running: a first call of an old
// callee from this section reorders .text_battlesplit and its strings (GCC's order follows the call graph).
static BS_CODE14 bool bsSame14(const char* a, const char* b) {
    while (*a != 0 && *a == *b) a++, b++;
    return *a == *b;
}

// The place for the attack her box narrows for (bsTurn14): 1 centred, 2 the close seam, 3 the bottom whole.
static BS_CODE14 uint8_t bsTurnPlace14(Runner* runner) {
    DataWin* dw = runner->dataWin;
    VMContext* ctx = runner->vmContext;
    if (dw == nullptr || ctx == nullptr) return 2;
    int32_t o = -1;
    for (uint32_t i = 0; dw->objt.count > i && 0 > o; i++) {
        const char* name = dw->objt.objects[i].name;
        if (name != nullptr && bsSame14(name, bsTurn14)) o = (int32_t) i;
    }
    Instance* inst = bsLive(runner, o);
    ptrdiff_t slot = inst != nullptr ? shgeti(ctx->varNameMap, (char*) (bsTurn14 + sizeof("obj_spiderb"))) : -1;
    RValue* value = 0 <= slot ? bsPickFind(&inst->selfVars, (int32_t) ctx->varNameMap[slot].value) : nullptr;
    int32_t turn = value == nullptr ? -1 : value->type == RVALUE_REAL ? (int32_t) value->real : value->type == RVALUE_INT32 ? value->int32 : -1;
    if (0 > turn || turn >= 32) return 2;
    return ((BS_TURN_CENTRED >> turn) & 1u) != 0 ? 1 : ((BS_TURN_WHOLE >> turn) & 1u) != 0 ? 3 : 2;
}

// Mode 5's place this frame. The box's left edge says how far it has narrowed (0 the menu box, 1 the attack box); the
// attack's place (bsTurnPlace14) is taken as it starts to narrow and kept in bs->turn14 until it is the menu box again,
// and the place goes from centred to it as the box narrows and back as it widens after, so it moves only while the game
// moves the box. Centred (the menu too): the standard band centred on the top screen and the bottom lifted BS_A_UP, with
// (c)'s HP line and buttons at the foot (CtrBattlesplit_seam: 56 is not in bsSeamB). The close seam follows obj_uborder
// as modes 1 and 2; the bottom whole is not lifted (one piece, nothing hidden), the top as centred.
// A split that begins with the box already narrowed (Split turned on in an attack: turnamt has moved on to the next
// attack by then) starts at 5, the bottom whole, and takes no attack until it has seen the menu box (c26rev5 R-bs14e F1).
static BS_CODE14 void bsPlaceTurn14(Bs* bs, Runner* runner, float perPx) {
    Instance* edge = bsLive(runner, bsState.border[1]);
    Instance* left = bsLive(runner, bsState.border[0]);
    if (edge == nullptr || left == nullptr) return;
    float w = (left->x - BS_TURN_WIDE) / (BS_TURN_NARROW - BS_TURN_WIDE);
    if (0.0f >= w) bs->turn14 = 0;
    else if (bs->turn14 == 0) bs->turn14 = bsTurnPlace14(runner);
    if (bs->turn14 == 1 || 0.0f >= w) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    int32_t seam = (int32_t) floorf((edge->y - bs->viewTop) / perPx);
    seam = seam > BS_BAND_STANDARD ? BS_BAND_STANDARD : BS_SEAM_MIN > seam ? BS_SEAM_MIN : seam;
    float band = (float) BS_BAND_STANDARD, top = (float) ((240 - BS_BAND_STANDARD) / 2), bottom = (float) -BS_A_UP;
    float toBand = (float) seam, toTop = (float) (240 - seam), toBottom = (float) (2 - seam);
    if (bs->turn14 == 3 || bs->turn14 == 5) toBand = band, toTop = top, toBottom = 0.0f;
    bs->place.bandRows = (int16_t) floorf(band + w * (toBand - band) + 0.5f);
    bs->place.topDy = (int16_t) floorf(top + w * (toTop - top) + 0.5f);
    bs->place.bottomDy = (int16_t) floorf(bottom + w * (toBottom - bottom) + 0.5f);
}

// ===[ Muffet above room row 0 (battlesplit14f) ]===
// In her climbs (4, 9 and 15) her Draw (obj_spiderb_body) raises her as the box's top goes above room y 240, by
// 240 - obj_uborder.y, so the PC leaves her head above the screen. The top screen has the place's topDy rows free above
// room row 0 (57 in mode 5's centred and whole places), and maintainer (11-09) asked for her whole body there. Her own sprite
// draws that reach above room row 0 are kept as they come (CtrBattlesplit_drawSpriteTop14, mode 5's drawSprite) and
// drawn again at present (bsPresent14), 240 rows lower, on the half view's last rows over black, which then go on the
// top screen just above the band (14c's strip: CtrRenderer_bsStripClear14 and CtrRenderer_bsStripCopy14). Only her
// object: spiders and bullets that come from above appear as they enter the room, as on PC. Nothing the game reads
// changes. At present no later draw can reach those rows first, and the copy is one more pass on the top screen a frame
// with her above row 0, not one a draw. A draw is drawn again only while its rows stay below the band's on the half
// view (at most 240 - band rows above room row 0), so the band the top screen shows is never reached.
static const char bsBody14s[] __attribute__((section(".aacout.rodata.battlesplit14"))) = "obj_spiderb_body";

// The object index of the object named want (obj_spiderb_body, obj_heatwaver), -1 none.
static BS_CODE14 int16_t bsObject14(DataWin* dw, const char* want) {
    if (dw == nullptr) return -1;
    for (uint32_t i = 0; dw->objt.count > i && 32767 > i; i++) {
        const char* name = dw->objt.objects[i].name;
        if (name != nullptr && bsSame14(name, want)) return (int16_t) i;
    }
    return -1;
}

// Mode 5's drawSprite: as bsDrawSprite, and her draws that reach above room row 0 kept for present. A turned draw's
// reach is taken as its farthest corner's distance from the origin, bounded above (no cosf, sinf or sqrtf from this
// section): a draw kept that does not reach is drawn again below the half view's rows and shows nothing. Extern: see
// the section's head.
void CtrBattlesplit_drawSpriteTop14(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
                                    float xscale, float yscale, float angleDeg, uint32_t color, float alpha);
BS_CODE14 void CtrBattlesplit_drawSpriteTop14(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY,
                                              float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    bsDrawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
    Bs* bs = bsState.bs;
    DataWin* dw = renderer->dataWin;
    VMContext* vm = renderer->runner != nullptr ? renderer->runner->vmContext : nullptr;
    Instance* inst = vm != nullptr ? vm->currentInstance : nullptr;
    if (bs == nullptr || dw == nullptr || inst == nullptr || bs->mode14 != BS_MODE_TURN || 0 > bs->muffet14) return;
    if (inst->objectIndex != bs->muffet14 || vm->currentEventType != EVENT_DRAW || bs->tops14 >= BS_TOPS14) return;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return;
    const TexturePageItem* t = &dw->tpag.items[tpagIndex];
    float y0 = ((float) t->targetY - originY) * yscale, y1 = y0 + (float) t->targetHeight * yscale;
    float top = y0 < y1 ? y0 : y1;
    if (angleDeg != 0.0f) {
        float x0 = ((float) t->targetX - originX) * xscale, x1 = x0 + (float) t->targetWidth * xscale;
        float ax = fabsf(x0) > fabsf(x1) ? fabsf(x0) : fabsf(x1), ay = fabsf(y0) > fabsf(y1) ? fabsf(y0) : fabsf(y1);
        // The farthest corner's distance, at most (a >= b >= 0: sqrt(a*a + b*b) <= a + (sqrt(2) - 1) * b).
        top = -(ax > ay ? ax + 0.41422f * ay : ay + 0.41422f * ax);
    }
    if (y + top >= bs->viewTop) return;
    BsTop14* d = &bs->top14[bs->tops14++];
    d->tpagIndex = tpagIndex;
    d->x = x, d->y = y, d->originX = originX, d->originY = originY;
    d->xscale = xscale, d->yscale = yscale, d->angleDeg = angleDeg, d->alpha = alpha;
    d->color = color;
    d->high = y + top;
}

// In place of bsPresent8 (CtrRenderer_chainEndFrameBattlesplit): her kept draws on the top screen's topDy rows, then
// bsPresent8 as before.
static BS_CODE14 void bsPresent14(Renderer* renderer, int32_t band, int32_t topDy, int32_t bottomDy) {
    Bs* bs = bsState.bs;
    float perPx = bs != nullptr ? (bs->viewBottom - bs->viewTop) / 240.0f : 0.0f;
    int32_t n = bs != nullptr ? bs->tops14 : 0;
    if (bs != nullptr) bs->tops14 = 0;
    bool room = 0 < n && 0 < topDy && 0 < band && 240 > band + topDy && 0.0f < perPx && bs->inner != nullptr && bs->inner->drawSprite != nullptr;
    if (room) room = CtrRenderer_bsStripClear14(renderer, bs->viewLeft, bs->viewTop + perPx * (float) (240 - topDy), bs->viewRight,
                                                bs->viewTop + perPx * 240.0f);
    if (room) {
        float most = bs->viewTop - perPx * (float) (240 - band); // the band's last row, 240 rows up
        for (int32_t k = 0; n > k; k++) {
            const BsTop14* d = &bs->top14[k];
            if (most > d->high) continue;
            bs->inner->drawSprite(renderer, d->tpagIndex, d->x, d->y + perPx * 240.0f, d->originX, d->originY, d->xscale, d->yscale,
                                  d->angleDeg, d->color, d->alpha);
        }
        CtrRenderer_bsStripCopy14(renderer, topDy, topDy);
    }
    bsPresent8(renderer, band, topDy, bottomDy);
}

// ===[ Pyrope's heat wave (battlesplit15) ]===
// After Heat Up, obj_heatwaver (groups 52 and 78) captures the box's inside from the application surface each Draw
// (sprite_create_from_screen_x: sprite_create_from_surface of the application surface, room coordinates) and draws it
// back row by row in waves. On a split frame the box's rows are on the arena layer, so the half view has nothing of
// them: its capture reads the layer (CtrRenderer_bsSpriteFrom9, as Muffet's fake border's), and the wave is a
// bottom-class draw (the scene table), so it lands on the layer it was read from, as on PC. Every other capture goes to
// the inner table as with no hook. Muffet's battle keeps bsSprite9 as its hook. Own constants; extern for the reason in
// the section's head.
static const char bsHeat15s[] __attribute__((section(".aacout.rodata.battlesplit14"))) = "obj_heatwaver";
static const char bsLogHeat15[] __attribute__((section(".aacout.rodata.battlesplit14"))) =
    "battlesplit: room %d group %d: the heat wave's capture (%d,%d %dx%d) reads layer %d (1 the arena layer)\n";
int32_t CtrBattlesplit_sprite15(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h, bool removeback,
                                bool smooth, int32_t xorig, int32_t yorig);
BS_CODE14 int32_t CtrBattlesplit_sprite15(Renderer* renderer, int32_t surfaceID, int32_t x, int32_t y, int32_t w, int32_t h,
                                          bool removeback, bool smooth, int32_t xorig, int32_t yorig) {
    Bs* bs = bsState.bs;
    RendererVtable* inner = bs != nullptr ? bs->inner : nullptr;
    if (inner == nullptr || inner->createSpriteFromSurface == nullptr) return -1;
    Runner* runner = renderer->runner;
    VMContext* vm = runner != nullptr ? runner->vmContext : nullptr;
    Instance* inst = vm != nullptr ? vm->currentInstance : nullptr;
    if (!bsState.armed || inst == nullptr || surfaceID != runner->applicationSurfaceId || bs->viewTop >= bs->viewBottom || 0 >= bs->heat15 ||
        inst->objectIndex != bs->heat15 - 1)
        return inner->createSpriteFromSurface(renderer, surfaceID, x, y, w, h, removeback, smooth, xorig, yorig);
    int32_t layer = 0;
    float split = (bs->splitY - bs->viewTop) / (bs->viewBottom - bs->viewTop);
    int32_t sprite = CtrRenderer_bsSpriteFrom9(renderer, inner, split, surfaceID, x, y, w, h, removeback, smooth, xorig, yorig, &layer);
    if ((bs->capLogged & 4) == 0) {
        bs->capLogged |= 4;
        logInfo(bsLogHeat15, (int) bs->room, (int) bs->group, (int) x, (int) y, (int) w, (int) h, (int) layer);
    }
    return sprite;
}

// In bsClearLayer6, in place of bsClearLayer9 (drawViews: bsInstall has filled the table, and the view is this frame's): a
// following mode's band is the box's top row now (obj_uborder), mode 5's place is bsPlaceTurn14's, and drawViews' rows
// from the place again; a grid mode's drawSprite goes through CtrBattlesplit_drawSprite14, mode 5's through
// CtrBattlesplit_drawSpriteTop14 (her draws above room row 0 kept from none). Then bsClearLayer9, and outside Muffet's
// battle the heat wave's capture hook in its place when the game has obj_heatwaver (battlesplit15). Last, the end
// triggers (battlesplit16): one with a live instance turns the rest of the battle Original. beginFrame reads failed
// before Step, so this frame stays split and the next is Original: the latency bsTriggers' check has for an instance
// Step made. The latch is bsState.failed's: it holds until beginFrame sees the room index change, not per encounter, so
// a restart of the room (the same index) or a retry that never leaves the room stays Original too. For Asgore that strands
// nothing: his trigger is his last cutscene, which leaves room_battle; a death goes to the game-over room first.
// UNDERTALE has one such path: group 91 hands over to group 92 by setting the battle group and restarting the room, so
// an end trigger in 91 would keep 92 Original as well; 91 has none.
static const char bsLogEnd16[] __attribute__((section(".aacout.rodata.battlesplit14"))) =
    "battlesplit: room %d group %d goes Original for the rest of the battle (%s, the scene's end)\n";
// bsLive's test, own copy: a call of bsLive from here could change how GCC builds it (BS_CODE is not noipa).
static BS_CODE14 bool bsAlive16(Runner* runner, int32_t o) {
    if (runner == nullptr || 0 > o || runner->instancesByObject == nullptr) return false;
    Instance** list = runner->instancesByObject[o];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (!list[j]->destroyed) return true;
    }
    return false;
}

static BS_CODE14 void bsClearLayer14(Renderer* renderer) {
    Bs* bs = bsState.bs;
    float perPx = bs != nullptr ? (bs->viewBottom - bs->viewTop) / 240.0f : 0.0f;
    // The guard's test (battle_probe 16 in a bench) lifts every group, where bsPlace14 leaves mode 5's scene lift 0: a
    // lifted place before the battle's first frame turns mode 5 off for the battle (turn14 4); else the split starts at 5
    // (bsPlaceTurn14: armed when the box is the menu box, at once in a split from the battle's start).
    if (bs != nullptr && bs->frames == 0 && bs->turn14 == 0) bs->turn14 = bs->place.bottomDy != 0 ? 4 : 5;
    if (bs != nullptr && bs->mode14 == BS_MODE_TURN && bs->layout == CTR_BATTLE_LAYOUT_A && 0.0f < perPx && bs->turn14 != 4) {
        bsPlaceTurn14(bs, renderer->runner, perPx);
        bs->bandY = bs->viewTop + perPx * (float) bs->place.bandRows;
        bs->hideY = bs->viewTop + perPx * (float) -bs->place.bottomDy;
        if (bs->frames == 0) bs->muffet14 = bsObject14(renderer->dataWin, bsBody14s);
        bs->tops14 = 0;
        bs->outer.drawSprite = CtrBattlesplit_drawSpriteTop14;
    } else if (bsClose14(bs) && 0.0f < perPx) {
        int32_t mode = bs->mode14;
        Instance* edge = mode != 0 && BS_MODE_FOLLOW >= mode ? bsLive(renderer->runner, bsState.border[1]) : nullptr;
        if (edge != nullptr) {
            int32_t seam = (int32_t) floorf((edge->y - bs->viewTop) / perPx);
            bsSeam14(&bs->place, seam > BS_BAND_STANDARD ? BS_BAND_STANDARD : BS_SEAM_MIN > seam ? BS_SEAM_MIN : seam);
            bs->bandY = bs->viewTop + perPx * (float) bs->place.bandRows;
            bs->hideY = bs->viewTop + perPx * (float) -bs->place.bottomDy;
        }
        if (mode == 2 || mode == 4) bs->outer.drawSprite = CtrBattlesplit_drawSprite14;
    }
    bsClearLayer9(renderer);
    if (bs != nullptr && bs->group != BS_MUFFET && bs->inner != nullptr && bs->inner->createSpriteFromSurface != nullptr) {
        if (bs->heat15 == 0) {
            int16_t o = bsObject14(renderer->dataWin, bsHeat15s);
            bs->heat15 = 0 > o ? (int16_t) -1 : (int16_t) (o + 1);
        }
        if (0 < bs->heat15) bs->outer.createSpriteFromSurface = CtrBattlesplit_sprite15;
    }
    for (uint32_t k = 0; bs != nullptr && !bsState.failed && BS_ENDS16 > k && 0 < bs->end16[k]; k++) {
        int32_t o = bs->end16[k] - 1;
        if (!bsAlive16(renderer->runner, o)) continue;
        bsState.failed = true;
        DataWin* dw = renderer->dataWin;
        const char* name = dw != nullptr && dw->objt.count > (uint32_t) o ? dw->objt.objects[o].name : nullptr;
        logInfo(bsLogEnd16, (int) bs->room, (int) bs->group, name != nullptr ? name : bsLogEnd16 + sizeof bsLogEnd16 - 1);
    }
}

// battlesplit17: the band clip. An automatic straddle (bsRouteAt11f's first branch: a BY_CALL draw that starts above
// the band and reaches below it) goes on both layers, and its arena copy put again on the layer's rows above the band
// what the top screen already shows there. Only that copy is cut now: the arena layer keeps its rows from the cut row
// down, the lowest of the standard band (BS_BAND_STANDARD), the encounter's band and this frame's box top (the top
// border's row). Explicit both and cover draws call bsRouteAt themselves, and guard-promoted ones pass bsBoth17
// unmarked: none is cut (group 56, bsRouteAt56: in mode 5 every BS_BOTH copy is, and a climb's draw has none).
// spare14[0]: 0 nothing, 1 the next switch to the arena layer is a cut copy's (BS_AGAIN's), 2 the cut is on. The cut
// is the scissor (CtrRenderer_bsClip17, ctr_renderer.c): it holds the scissor fields, the game's kept aside, until the
// switch back to the top layer has sent the copy's draw; then the game's come back. In battlesplit's AAC tenant
// (.aacout.text.battlesplit14), after its earlier functions.
#define BS_CODE17 BS_CODE14
void CtrRenderer_bsClip17(Renderer* renderer, int32_t row);
void __real_CtrRenderer_bsLayer(Renderer* renderer, int32_t layer);
void __wrap_CtrRenderer_bsLayer(Renderer* renderer, int32_t layer);

// bsBoth17: bsRouteAt for bsRouteAt11f's two BS_BOTH calls (the automatic straddle's, and the guard's own draw's),
// which mark the arena copy for the cut only for the first: the guard's draw is the one whose object's class is
// BS_BOTH now (bsGuard11 set it), an automatic straddle's class is BY_CALL. Both calls take the same arguments, so
// 11f keeps its one shared tail (its size stays). noinline and noclone (a clone for BS_BOTH alone made 11f 4 B
// smaller, which moved late text), but not noipa: noipa changed what the compiler knew about 11f's callers, and every
// routed wrapper in .text_battlesplit moved (120 B of late text).
#define BS_CODE17B __attribute__((noinline, noclone, section(".aacout.text.battlesplit14")))
static BS_CODE17B bool bsBoth17(Renderer* renderer, int32_t want, const float* r) {
    Bs* bs = bsState.bs;
    bool cut = bs->lastWant != BS_BOTH;
    bool both = bsRouteAt(renderer, want, r);
    bs->spare14[0] = 0;
    if (both && cut) bs->spare14[0] = 1;
    return both;
}

// The cut row in layer rows, or 0 for none.
static BS_CODE17 int32_t bsCut17(Bs* bs) {
    int32_t cut = BS_BAND_STANDARD;
    if (cut > bs->place.bandRows) cut = bs->place.bandRows;
    float perPx = (bs->viewBottom - bs->viewTop) / 240.0f;
    if (0.0f >= perPx) return cut;
    Instance* edge = bsLive(bs->renderer->runner, bsState.border[1]);
    if (edge == nullptr) return cut;
    int32_t top = (int32_t) floorf((edge->y - bs->viewTop) / perPx);
    if (cut > top) cut = top;
    if (0 > cut) cut = 0;
    return cut;
}

// Every layer switch (bsplitSwitch's, and the frame's first in bsClearLayer6) comes here first (-Wl,--wrap).
BS_CODE17 void __wrap_CtrRenderer_bsLayer(Renderer* renderer, int32_t layer) {
    __real_CtrRenderer_bsLayer(renderer, layer);
    Bs* bs = bsState.bs;
    if (bs == nullptr) return;
    uint8_t state = bs->spare14[0];
    if (state == 0) return;
    bs->spare14[0] = 0;
    if (layer != BS_BOTTOM) {
        if (state == 2) CtrRenderer_bsClip17(renderer, 0);
        return;
    }
    if (state != 1) return;
    int32_t cut = bsCut17(bs);
    if (0 >= cut) return;
    CtrRenderer_bsClip17(renderer, cut);
    bs->spare14[0] = 2;
}
// ===[ The move up (battlesplit18, 11-10 ruling): the game below goes to the top screen for a battle ]===
// Step 3b's mirror. With the player's game below (CtrScreens_gameBelow), both targets held and Battle layout Split or
// Top screen, a room whose battle would move the game down with it on top (bsTouchRoom10's 2, the same groups:
// bsTouchGroup) moves it up instead (CtrLayout_moveUp, z_layout.c), before this frame's want, so the battle's first
// frame is already split (Split) or whole on the top screen (Top screen, read as Original here: CtrBattlesplit_layout18).
// A room that keeps it (bsTouchRooms' game-over room, or the next battle) keeps it; the first other room moves it back
// below. Layout holds the move's state (CtrLayout_autoUp); this change's st->touch stays step 3b's alone. A Game screen
// change while it lasts is the player picking Top: layout ends the move there. The stylus move (3b) never runs with Top
// screen: the battle is on the top screen whatever the game's screen.
// Same-size edits only (lead 11-10: nothing that exists moves): chainBeginFrameBattlesplit calls bsTouchRoom18 in
// place of bsTouchRoom10 and CtrBattlesplit_layout18 in place of CtrBattlesplit_setting for the frame's layout (the
// same calls, other targets). GCC orders a section's functions by the file's call graph: bsTouchRoom18 is called by
// its own static name and calls bsTouchRoom10 itself, so it takes bsTouchRoom10's place in that graph and no earlier
// function moves (an external second name there moved bsTouchRoom10 below bsRoomNow10); CtrBattlesplit_setting keeps
// other callers, so a second name is enough for it. bsTouchRoom18 runs once per room change, layout18 once a frame in
// a battle room (a call and a compare). In battlesplit's own AAC input (.aacout.text.bsup), the slot's suffix.
#define BS_CODE18 __attribute__((noinline, noipa, used, section(".aacout.text.bsup")))
#define BS_HOOK18 __attribute__((noinline, noipa, section(".aacout.text.bsup")))
#define BS_RO18 __attribute__((section(".aacout.rodata.bsup")))
bool CtrLayout_moveUp(Renderer* renderer, bool up);
bool CtrLayout_autoUp(void);
void CtrRenderer_bsSetUp18(Renderer* renderer);
static const char bsLogUp18[] BS_RO18 = "battlesplit: room %d group %d: the game goes to the top screen for the battle\n";
static const char bsLogDown18[] BS_RO18 = "battlesplit: room %d: the game goes back to the bottom screen\n";

// bsTouchRoom10's answer (0 a room that ends a move, 1 one that keeps it, 2 a battle's first room) for the code after it,
// with the move up made or ended here. While the move up lasts every battle room and the game-over room keep it (1) and
// the first other room ends it (0, with st->touch 0: nothing more). A battle's first room with the game on top: 2 for
// step 3b, 1 with Top screen (no move down). With the game below, no 3b move and Split or Top screen: the move up, then
// 1 (the game is on top now and must not go down again); without it 2, which needs the game on top to do anything.
// The same answer for a Battle layout pick in the room (bsRepick18, LAY-1): Original during the move up ends it here (0,
// the game back below), and Top screen during step 3b ends 3b (0: the caller's bsTouchMove takes the game back up).
static BS_HOOK18 int32_t bsTouchRoom18(Runner* runner, int32_t room) {
    int32_t where = bsTouchRoom10(runner, room);
    Renderer* renderer = runner->renderer;
    if (CtrLayout_autoUp()) {
        if (where != 0 && CtrBattlesplit_setting() != CTR_BATTLE_LAYOUT_ORIGINAL) return 1;
        CtrLayout_moveUp(renderer, false);
        logInfo(bsLogDown18, (int) room);
        return 0;
    }
    BsState* st = &bsState;
    if (st->touch != 0) return where != 0 && CtrBattlesplit_setting() == CTR_BATTLE_LAYOUT_TOP ? 0 : where;
    if (where != 2) return where;
    int32_t setting = CtrBattlesplit_setting();
    if (CtrScreens_game == 0) return setting == CTR_BATTLE_LAYOUT_TOP ? 1 : 2;
    if (setting == CTR_BATTLE_LAYOUT_ORIGINAL || !CtrScreens_gameBelow()) return where;
    int32_t g = st->roomBattle ? bsGroup10(runner) : -1;
    if (st->roomBattle && !bsTouchGroup(g)) return where;
    if (!CtrLayout_moveUp(renderer, true)) return where;
    CtrRenderer_bsSetUp18(renderer); // the screens set up now: bsAllowed reads the top target's size
    logInfo(bsLogUp18, (int) room, (int) g);
    return 1;
}

// LAY-1 (c26rev2): a Battle layout pick applies in the room it was made in. Layout's CtrLayout_chain18 calls this at
// the next frame's start with a pick waiting, before this file's chain: outside the frame, as the room block's move
// at a room's entry. A room with a battle (bsTouchRoom10 nonzero), unchanged since the room block's last look: the
// pick's set-up first (the pair taken for Split or Top screen, or refused: Original back with the row's note, nothing
// moved), then bsTouchRoom18 again for the same room (the move up, the move back below, or step 3b's end) and the room
// block's follow-up for 3b's end. A room change in that frame is the room block's own; the split and st->failed stay
// the frame's (CtrBattlesplit_layout18 reads the setting each frame). A pick doesn't start step 3b (that starts at a
// battle's first room only, as with the Stylus setting).
static const char bsLogPick18[] BS_RO18 = "battlesplit: room %d: a Battle layout pick in the room (%d)\n";
static BS_CODE18 void bsRepick18(Renderer* renderer) {
    BsState* st = &bsState;
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    if (!BS_DRAWVIEWS_WRAPPED || runner == nullptr || runner->renderer != renderer || runner->dataWin == nullptr) return;
    int32_t room = runner->currentRoomIndex;
    if (room != st->lastRoom || bsTouchRoom10(runner, room) == 0) return;
    CtrRenderer_bsSetUp18(renderer);
    int32_t where = bsTouchRoom18(runner, room);
    if (st->touch != 0 && where == 0) bsTouchMove(renderer, room, -1, false);
    logInfo(bsLogPick18, (int) room, (int) where);
}
__asm__(".global CtrBattlesplit_repick18\n.set CtrBattlesplit_repick18, bsRepick18\n");

// The frame's layout: Top screen is the whole battle on the top screen, Original here.
static BS_CODE18 int32_t bsLayout18(void) {
    int32_t v = CtrBattlesplit_setting();
    return v == CTR_BATTLE_LAYOUT_TOP ? CTR_BATTLE_LAYOUT_ORIGINAL : v;
}
__asm__(".global CtrBattlesplit_layout18\n.set CtrBattlesplit_layout18, bsLayout18\n");

// Layout's CtrLayout_canMoveUp (commit D): the groups the move up is for, asked by battlesplit T1 at the SOUL flight's
// start with global.battlegroup.
bool CtrBattlesplit_touchGroup18(int32_t group);
__attribute__((noinline, noipa, used, section(".text_layoutd"))) bool CtrBattlesplit_touchGroup18(int32_t group) {
    return bsTouchGroup(group);
}

// ===[ The SOUL's entry (W1; maintainer 11-10: "the SOUL entry must travel to its real split position") ]===
// An encounter in a room that is not split (obj_battler alive, Split chosen, the group splits): its frames to the room
// change are split already, with the entry's place, the whole half view on the top screen (band 240, no move) and the
// battle's own bottom (its lift and seam), and no class: every draw on the top, as Original. The travelling SOUL
// (obj_transheart) is drawn twice: on the top at the PC's place plus its drop, and on the bottom where the stacked
// screens put that place (the bottom 240 rows below the top, centred). The drop is the PC's travel share (the way gone
// from its start to its own target, 17 moves or 8, 1 once snapped, speed 0) of the way to the battle SOUL's place on the bottom (the target's row,
// lifted unless at or below the seam), so it lands where the battle room's first frame shows the SOUL, and it crosses
// from the top's last row to the bottom's first at an even speed. The PC frames, motion and RNG are the game's: only
// where the two draws go. At the battle room's first frame the entry's split goes on as the battle's (its place and
// classes back, frames from 0), so no remake. Same-size edits: layout's chain18 calls CtrBattlesplit_entryW1 in place
// of the chain, bsClearLayer6 calls bsClearLayerE in place of bsClearLayer14. Bs's free spare14[1]: 1 an entry split,
// 2 a moved fight's (below). Part C (below) moves the SOUL's draw in flight with the game below and Split: the unarmed
// path's draw calls bsDrawViewsC. Appended last in late text (.text_bsentry, .rodata_bsentry); their state (bsMovedE,
// part C's) is late BSS (.bss_bsentry_late).
#define BS_CODEE __attribute__((noinline, noipa, optimize("Os"), section(".text_bsentry"))) // Os: run once a frame or a draw
#define BS_ROE __attribute__((aligned(4), section(".rodata_bsentry")))
#define BS_E_SOUL 142 // obj_transheart in UNDERTALE 1.08 (Steam and GOG); obj_battler follows it (both names checked)
// obj_battlerflowey's (144) is "": it never matches (its room has its own scene).
static const char bsENames[5][17] BS_ROE = { "obj_transheart", "obj_battler", "", "obj_battlerquick", "obj_battlerstory" };
void __wrap_CtrRenderer_chainBeginFrameBattlesplit(Renderer* renderer);
void CtrBattlesplit_entryW1(Renderer* renderer);

static BS_CODEE bool bsEIs(DataWin* dw, uint32_t k) {
    uint32_t o = BS_E_SOUL + k;
    const char* name = dw->objt.count > o ? dw->objt.objects[o].name : nullptr;
    return name != nullptr && strcmp(name, bsENames[k]) == 0;
}

// The travelling SOUL's own target (its Create's mychoicex, mychoicey) in t, or false. Its moves are not always 17: with
// global.flag[16] == 1 (a spear or laser start, the barrier's Asgore, until a reset) it goes back to its start and takes
// 8, and its target is per room (5 and 77, 134-137). The target and the start give the share for any count.
// shgeti's stbds_hmget_key through a declaration of W1's own (same symbol, same code): a first call of the old
// declaration from this section moves bsCut17 after bsPlaceTurn14 in .aacout.text.battlesplit14 (GCC's order follows
// the call graph; arm-none-eabi-gcc -O2, the o3ds flags, 11-12).
extern void* bsHmgetE(void* a, size_t elemsize, void* key, size_t keysize, int mode) __asm__("stbds_hmget_key");
static const char bsEVars[2][10] BS_ROE = { "mychoicex", "mychoicey" };
#undef stbds_hmget_key_wrapper
#define stbds_hmget_key_wrapper bsHmgetE
static BS_CODEE bool bsETarget(VMContext* vm, Instance* inst, float* t) {
    for (uint32_t k = 0; 2 > k; k++) {
        ptrdiff_t slot = shgeti(vm->varNameMap, (char*) bsEVars[k]);
        RValue* v = 0 <= slot ? bsPickFind(&inst->selfVars, (int32_t) vm->varNameMap[slot].value) : nullptr;
        if (v == nullptr || (v->type != RVALUE_REAL && v->type != RVALUE_INT32)) return false;
        t[k] = v->type == RVALUE_REAL ? (float) v->real : (float) v->int32;
    }
    return true;
}
#undef stbds_hmget_key_wrapper
#define stbds_hmget_key_wrapper stbds_hmget_key

// The travelling SOUL's share of its way (0 at its start, 1 once snapped, speed 0), and its target's row in *ty.
static BS_CODEE float bsEShare(VMContext* vm, Instance* inst, float* ty) {
    float frac = 1.0f;
    *ty = inst->y;
    if (inst->speed > 0.0f) {
        // no target read: the end of the 17 moves of its usual Create (its motion is a straight line from the start)
        float tg[2];
        if (!bsETarget(vm, inst, tg)) tg[0] = inst->xstart + 17.0f * inst->hspeed, tg[1] = inst->ystart + 17.0f * inst->vspeed;
        // the share of the way from its start to the target still to go, taken off 1 (0 at the start)
        float ax = tg[0] - inst->xstart, ay = tg[1] - inst->ystart, rx = tg[0] - inst->x, ry = tg[1] - inst->y;
        float all = sqrtf(ax * ax + ay * ay), left = sqrtf(rx * rx + ry * ry);
        frac = left >= all ? 0.0f : 1.0f - left / all;
        *ty = tg[1];
    }
    return frac;
}

// The travelling SOUL's two rows for this draw at y: at[0] on the top (y plus its drop), at[1] on the layer (its row
// on the bottom screen); 2, or 1 for any other draw (one, at y). Apart from the draw, so that no argument of drawSprite
// is live across its calls.
static BS_CODEE int32_t bsEAt(Renderer* renderer, float y, float* at) {
    Bs* bs = bsState.bs;
    VMContext* vm = renderer->runner->vmContext;
    Instance* inst = vm->currentInstance;
    float perPx = (bs->viewBottom - bs->viewTop) / 240.0f;
    if (inst == nullptr || vm->currentEventType != EVENT_DRAW || inst->objectIndex != BS_E_SOUL || !(0.0f < perPx) ||
        !CtrRenderer_bsAppBound(renderer))
        return 1;
    float ty;
    float frac = bsEShare(vm, inst, &ty);
    int32_t seam = CtrBattlesplit_seam();
    float lift = (float) bs->place.bottomDy, row = (ty - bs->viewTop) / perPx;
    float drop = frac * (240.0f + (seam != 0 && row >= (float) seam ? 0.0f : lift));
    float b = (y - bs->viewTop) / perPx + drop - 240.0f; // its row on the bottom screen
    if (seam == 0 || (float) seam > b) b -= lift;        // that row on the layer
    at[0] = y + drop * perPx;
    at[1] = bs->viewTop + b * perPx;
    return 2;
}

// The entry's drawSprite: the travelling SOUL on both screens, every other sprite as Original (the top).
static BS_CODEE void bsDrawSpriteE(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                   float yscale, float angleDeg, uint32_t color, float alpha) {
    float at[2] = { y, y };
    int32_t n = bsEAt(renderer, y, at);
    for (int32_t k = 0; n > k; k++) {
        if (k != 0) CtrRenderer_bsLayer(renderer, BS_BOTTOM);
        bsState.bs->inner->drawSprite(renderer, tpagIndex, x, at[k], originX, originY, xscale, yscale, angleDeg, color, alpha);
    }
    if (n == 2) CtrRenderer_bsLayer(renderer, BS_TOP);
}

static void bsClearLayerF(Renderer* renderer); // the box-follow lift (this file's end): bsClearLayer14, then the lift
static BS_CODEE void bsClearLayerE(Renderer* renderer) {
    Bs* bs = bsState.bs;
    if (bs == nullptr || bs->spare14[1] == 0) {
        bsClearLayerF(renderer);
        return;
    }
    bsClearLayer9(renderer);
    bs->outer.drawSprite = bsDrawSpriteE;
}

// ===[ Moved fights (maintainer 11-12: "yes fly down") ]===
// A fight that step 3b moves whole to the bottom screen at its first room frame (Stylus on, the game on top, the battle
// Original: obj_battlerquick to room_fastbattle, obj_battlerstory to room_storybattle, or obj_battler's room_battle with
// layout Original or a group not admitted, not Asriel's): its entry is a split too, as the ordinary one but with the
// battle's PC place on the bottom (bottomDy 0, no lift), marked spare14[1] 2. The chain makes it through two twins
// (same-size call edits at its layout and admission calls) that answer Split and admitted while bsMovedE says so;
// entryW1 sets bsMovedE every frame before the chain and ends a moved entry before the chain at the room change, so the
// chain's 3b move runs as without it. Only with a good scene table (its every-room row): without one nothing splits.
static uint8_t bsMovedE __attribute__((section(".bss_bsentry_late,\"aw\",%nobits @"))); // 1: this frame's entry is a moved fight's (NOBITS, as BS22_BSS)

#define BS_BSSE __attribute__((section(".bss_bsentry_late,\"aw\",%nobits @")))
bool CtrLayout_canUp18(void);
static int16_t bsUpC[2] BS_BSSE;              // this frame's lift (bottomDy; 0 none) and seam (0 one piece)
static float bsDyC BS_BSSE;                   // this draw's move of the SOUL, in room px
static const RendererVtable* bsRealC BS_BSSE; // the table under the copy
// R5-1 (c26rev5): an entry's flight and the battle's first split frame use one lift, bsFDest's (the box-follow lift's
// first menu frame for a followed group, else the scene's). The split handed over keeps the flight's lift, and its
// first follow frame starts there (its Bs's handF 1) instead of taking the wanted lift at once.
static int32_t bsFDest(const DataWin* dw, int32_t layout, int32_t g, int32_t sceneDy);

// The two latches in a room that is not split: form B's (obj_battler itself alive, a layout chosen) and the moved one,
// true for a moved fight's frame. The cheap tests first (Stylus moves an Original battle from the top screen this
// frame), then the live starter b (obj_battler or a child) and its fight, from this room (not a battle room).
static BS_CODEE bool bsLatchE(Runner* runner, int32_t room) {
    BsState* st = &bsState;
    DataWin* dw = runner->dataWin;
    if (bsMovedE != 0) st->roomBattle = false; // last frame's was a moved fight's: both latches decide again
    int32_t layout = CtrBattlesplit_layout18();
    bool original = layout == CTR_BATTLE_LAYOUT_ORIGINAL, ordinary = !st->roomBattle && !original;
    // The game on top (CtrScreens_game 0; gameBelow needs 1) and no move up: one lasts only through battle rooms and the
    // game-over room, and this room (bsTouchRoom 0, its change made) ended any. Not CtrLayout_autoUp: it clears
    // LY_UP_WAIT, bsTouchRoom18's own business at a room change.
    bool moves = st->touch == 0 && CtrScreens_game == 0 && CtrBattlesplit_setting() != CTR_BATTLE_LAYOUT_TOP && bsTouchOn();
    // part C: the game below, Split (Top screen reads Original: C1, no change for 1.0), the move up's own test
    bool up = st->touch == 0 && !original && CtrLayout_canUp18();
    Instance* b = ordinary || moves || up ? bsLive(runner, BS_E_SOUL + 1) : nullptr;
    if (b == nullptr) return false;
    if (ordinary && b->objectIndex == BS_E_SOUL + 1) st->roomBattle = true; // obj_battler itself: its children start elsewhere
    if (!moves && !up) return false;
    // 1 obj_battler, 3 obj_battlerquick, 4 obj_battlerstory; not obj_battlerflowey (2: its name is "") or another object
    uint32_t k = (uint32_t) b->objectIndex - BS_E_SOUL;
    if (k - 1 > 3 || !bsEIs(dw, k)) return false;
    bool kid = k != 1;
    if (CtrLayout_scene(dw, -1, -1) == nullptr || bsTouchRoom(runner, room) != 0 || (kid && up)) return false;
    if (!kid) {
        int32_t g = bsGroup(runner);
        if (!bsTouchGroup(g)) return false;
        bool admitted = bsGroupSplits10(g) || ((bsProbe() & 2) != 0 && bsHasTableS3(g));
        CtrLayoutPlace p;
        int32_t dy = 0;
        if (up && admitted && bsPlace10(layout, g, &p) && 0 > (dy = bsFDest(dw, layout, g, p.bottomDy))) {
            // the battle's lift, and CtrBattlesplit_seam's answer for that place and group
            bool one = g >= 0 && (int32_t) (sizeof(bsSeamB) * 8u) > g && (bsSeamB[g >> 3] & (1u << (g & 7))) != 0;
#ifdef CTRPORT_BS_PLACESWITCH
            if (bsPlaceC != 0) one = bsPlaceC != 2;
#endif
            bsUpC[0] = (int16_t) dy;
            bsUpC[1] = one ? 0 : BS_C_SEAM;
        }
        if (up || (!original && admitted)) return false;
    }
    st->roomBattle = true;
    return true;
}

// The chain's twins (its calls of CtrBattlesplit_layout18 and bsGroupSplits10 come here).
static BS_CODEE int32_t bsLayoutE(void) {
    return bsMovedE != 0 ? CTR_BATTLE_LAYOUT_A : CtrBattlesplit_layout18();
}

static BS_CODEE bool bsGroupSplitsE(int32_t group) {
    return bsMovedE != 0 || bsGroupSplits10(group);
}

// ===[ Part C: the game below with Split (lead 11-12 C2: the SOUL stays on the bottom) ]===
// With the player's game below and Battle layout Split, the battle's first room moves the game up (bsTouchRoom18) and
// an admitted group's battle splits with its box on the bottom, lifted (bottomDy; unlifted from its seam down). The
// SOUL flies on the bottom as on PC, unsplit: without this it lands at the PC place and the first split frame shows it
// up to 12 rows higher. In flight its draw goes up by its share of that lift, so it lands where the battle shows it.
// The latch is the move up's own test (layout's predicate, the room, the group) and the chain's admission, each frame
// in a room that is not split. The hook is the unarmed draw's: a copy of the renderer's table for that one call, its
// drawSprite W1's, as the split's own outer table around the same call. Late BSS only.
static BS_CODEE void bsDrawSpriteC(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                   float yscale, float angleDeg, uint32_t color, float alpha) {
    VMContext* vm = renderer->runner->vmContext;
    Instance* inst = vm->currentInstance;
    if (inst != nullptr && vm->currentEventType == EVENT_DRAW && inst->objectIndex == BS_E_SOUL) y += bsDyC;
    bsRealC->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

static BS_CODEE void bsDrawViewsC(Runner* runner, int32_t gameW, int32_t gameH, bool debugShowCollisionMasks) {
    Renderer* renderer = runner->renderer;
    Instance* heart = bsUpC[0] != 0 && renderer != nullptr && renderer->vtable != nullptr ? bsLive(runner, BS_E_SOUL) : nullptr;
    if (heart == nullptr || heart->objectIndex != BS_E_SOUL) {
        __real_Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
        return;
    }
    // the view as the split's (__wrap_Runner_drawViews): its top, and room px per screen row
    GMLCamera* camera = Runner_getCameraForView(runner, 0);
    float perPx = camera != nullptr && 0 < camera->viewWidth ? (float) camera->viewWidth / 320.0f : 2.0f;
    float top = camera != nullptr ? camera->viewY : 0.0f, ty;
    float frac = bsEShare(runner->vmContext, heart, &ty);
    int32_t seam = bsUpC[1];
    bsDyC = seam != 0 && (ty - top) / perPx >= (float) seam ? 0.0f : frac * (float) bsUpC[0] * perPx;
    RendererVtable copy = *renderer->vtable;
    bsRealC = renderer->vtable;
    copy.drawSprite = bsDrawSpriteC;
    renderer->vtable = &copy;
    __real_Runner_drawViews(runner, gameW, gameH, debugShowCollisionMasks);
    if (renderer->vtable == &copy) renderer->vtable = (RendererVtable*) bsRealC;
}

// In place of the chain (layout's chain18): the entry's start and its hand-over, around the chain.
BS_CODEE void CtrBattlesplit_entryW1(Renderer* renderer) {
    BsState* st = &bsState;
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    DataWin* dw = runner != nullptr ? runner->dataWin : nullptr;
    int32_t room = -2;
    uint8_t moved = 0;
    int16_t upC = bsUpC[0]; // last frame's part C lift: nonzero on the battle room's first frame after a C flight
    bsUpC[0] = 0;
    if (BS_DRAWVIEWS_WRAPPED && dw != nullptr && runner->renderer == renderer && st->namesFor == dw && 0 <= st->battleRoom) {
        room = runner->currentRoomIndex;
        Bs* bs = st->bs;
        if (room == st->lastRoom && room != st->battleRoom && !st->failed && bsEIs(dw, 0) && bsEIs(dw, 1))
            moved = bsLatchE(runner, room);
        if (bs != nullptr && bs->spare14[1] == 2 && moved == 0) {
            // a moved fight's entry ends before the chain: at the room change (its battle is Original, and the chain's
            // 3b move follows), or once it is no longer a moved one (a pick in flight; the chain may make the ordinary one)
            bsLeave8(renderer, false);
            bsFree(bs);
            st->bs = nullptr;
        } else if (room != st->lastRoom && room == st->battleRoom && bs != nullptr && bs->spare14[1] == 1 && bs->cls != nullptr) {
            int16_t dy = bs->place.bottomDy; // the entry's lift: where its flight landed
            bs->spare14[1] = 0;
            bs->room = room;
            bs->frames = 0;
            bs->count = dw->objt.count;
            bsPlace10(bs->layout, bs->group, &bs->place);
            bs->place.bottomDy = dy;
            bs->handF = 1;
        }
    }
    bsMovedE = moved; // the chain's twins read it this frame
    __wrap_CtrRenderer_chainBeginFrameBattlesplit(renderer);
    // A split made in a room that is not split, for an encounter's group (not a room's own scene): the entry's.
    Bs* bs = st->bs;
    if (bs == nullptr || bs->room != room || bs->frames != 0 || bs->spare14[1] != 0) return;
    if (room == st->battleRoom) {
        // part C's hand-over: the battle's new split (the move up made it this frame) keeps the C flight's lift
        if (upC != 0) bs->place.bottomDy = upC, bs->handF = 1;
    } else if (bs->group != BS_ROOM_SCENE) {
        bs->spare14[1] = moved != 0 ? 2 : 1;
        bs->count = 0;
        bs->place.bandRows = 240;
        bs->place.topDy = 0;
        // the moved battle is Original on the bottom: its PC place, no lift; else the battle's first frame's lift
        bs->place.bottomDy = moved != 0 ? 0 : (int16_t) bsFDest(dw, bs->layout, bs->group, bs->place.bottomDy);
    }
}

// ===[ The box-follow lift (maintainer 11-11: Pyrope, the Royal Guards, the menu text; 11-12 ruling) ]===
// r5, the centred default (maintainer 11-12: "pretty much all of the core encounters are missing the centered battle box",
// "centred"; 11-13 GO, battlesplit-centred-default.md option 1): every followed group (bsFWho below: each mode-0
// group with a scene row but the exceptions: 20 24 45 48 seam modes, 56 Muffet, 22 Toriel, 80 81 Mettaton EX, 57,
// 58; 101 at cap 0 since bsgroup r4) lifts with its box as below, the menu box centred. In the enemy's turn
// (global.mnfight 1, the speech, and 2, the attack; or not known) the lift is at most the group's attack cap: the
// least top row its measured attacks draw on the bottom (battlesplit's bsm sweep), 0 for a group whose shots cross the
// seam (census X1) or that drew above the screen, today's scene lift for a group no sweep measured. The box moves
// between turns: down from the speech's first frame, and if it is not yet at the cap on the attack's first frame it
// goes there at once; in the attack it does not rise. 49, 52 and 78 keep their own rules (the generator class, no
// turn rule).
// Preset A lifts a fight's bottom by its scene's lift, the same for the whole battle, with (c)'s HP line and buttons at
// the foot. A fight whose draws cross the seam or whose box is too tall for the lift had none (lift 0): its box, the
// menu text and the speech sat at the PC place, low on the bottom screen. For the Royal Guards (49) and Pyrope (52,
// 78) the lift follows the box instead, each frame, after bsClearLayer14 (presentation only: bottomDy and hideY, as
// the place's lift; no instance moves; touch reads the place each frame, CtrLayout_bottomSeam):
// - The lift wanted puts the box's centre (obj_uborder's and obj_dborder's y) where BS_A_UP puts the PC's menu box
//   (room 250-385), from 0 to BS_A_UP: BS_A_UP for the menu, the speech and Pyrope's rope box.
// - An attack whose shots reach the seam wants 0: the Royal Guards' (obj_carrotstargen: its first carrot or star is
//   8 frames after it, some at the box's bottom edge) and Pyrope's bombs (obj_bombgen: its first bomb is 3 frames
//   after it; the blasts reach the HP line). The lift falls BS_F_DOWN rows a frame, so it is 0 before the first shot.
//   While a bomb or a blast (obj_bombbul, obj_blastbul) lives it is 0 at once, whatever the frames were.
// - It rises at most BS_F_UP rows a frame and never while a bullet (blt_parent's) lives, so after an attack it comes
//   back up with the box as the game shrinks it. On the battle's first frame (a new split, Split turned on again,
//   or a group change in the same split) it is taken at once (no slide in), but not up while a bullet lives. A split
//   handed over from an entry (R5-1) keeps the lift its SOUL flew to (bsFDest: BS_A_UP, the menu box's) on its first
//   frame, so the flight's last frame and the battle's first show one place, and moves from there as on any later
//   frame. An attack still goes down to its cap at once (a battle that opens in an attack).
// Objects by index, as in UNDERTALE 1.08's Steam and GOG data (the two files the builder takes, by hash: both 1709
// objects, the same indices); data with another count keeps its scene's lift. Own sections, appended after W1's.
#define BS_CODEF __attribute__((noinline, noipa, optimize("Os"), section(".text_bsfollow"))) // Os: once a frame
#define BS_ROF __attribute__((aligned(4), section(".rodata_bsfollow")))
#define BS_BSSF __attribute__((section(".bss_bsfollow_late,\"aw\",%nobits @")))
#define BS_F_MID 118.75f  // the box centre's layer row that takes no lift: the menu box's (158.75) less BS_A_UP
#define BS_F_DOWN 14      // the most rows the lift falls in a frame: BS_A_UP to 0 in the 3 frames before a bomb
#define BS_F_UP 8         // the most rows it rises in a frame (the box's edges move 7.5 rows an update)
#define BS_F_BOMB 345     // obj_blastbul, then obj_bombbul and obj_bombgen
#define BS_F_CARROTS 701  // obj_carrotstargen
#define BS_F_BULLET 680   // blt_parent: every shot of these fights
#define BS_F_OBJECTS 1709 // UNDERTALE 1.08's objects (Steam and GOG)
#define BS_F_GROUPS 141   // bsFWho's groups (0..140)
#define BS_F_CAPS 70      // bsFWho[BS_F_CAPS + class]: the class's cap (classes 1..5; 0 is not followed)
#define BS_F_GEN 0x80     // in a cap: the generator rules only, no turn rule (49, 52, 78)
static const char bsLogF[] BS_ROF = "battlesplit: group %d follows the box (%d)\n";
// r5: a class per group, two to a byte (the even group's in the low nibble), then the caps of classes 1 to 5: 0, 4
// (43), 35 (54), 40, and the generator class's
static const uint8_t bsFWho[76] BS_ROF = { 0x40, 0x44, 0x44, 0x41, 0x44, 0x44, 0x44, 0x44, 0x44, 0x44, 0x40, 0x40,
                                           0x40, 0x44, 0x04, 0x11, 0x14, 0x44, 0x01, 0x00, 0x14, 0x21, 0x01, 0x11,
                                           0x50, 0x41, 0x45, 0x13, 0x00, 0x40, 0x14, 0x11, 0x11, 0x11, 0x04, 0x00,
                                           0x00, 0x00, 0x11, 0x15, 0x00, 0x01, 0x11, 0x04, 0x00, 0x00, 0x44, 0x00,
                                           0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                           0x41, 0x44, 0x14, 0x11, 0x44, 0x14, 0x11, 0x11, 0x00, 0x00, 0x01, 0x00,
                                           0x04, 0x23, 0x28, 0xa8 };
static const Bs* bsFBs BS_BSSF; // the battle and group bsFOk is for
static int16_t bsFGroup BS_BSSF;
static uint8_t bsFOk BS_BSSF;   // 1 follows, first frame to come; 3 follows; else not
static uint8_t bsFCapNow BS_BSSF; // the group's cap (bsFWho)
// global.mnfight through the pick log's bsPickGlobal, called by a pointer GCC cannot see through (volatile): a direct
// call from here changed the order GCC emits .text_battlesplit6's functions in (bsPickGlobal before bsClearLayer6, same
// size), and so that section's bytes. With the pointer only .text_bsfollow, .rodata_bsfollow and bsSeamB change.
static int32_t (*const volatile bsFPick)(Bs*, Runner*, int32_t) BS_ROF = bsPickGlobal;

// Group g's class (bsFWho; 0: not followed, or data without UNDERTALE 1.08's objects).
static BS_CODEF uint32_t bsFClass(const DataWin* dw, int32_t g) {
    uint32_t i = BS_F_GROUPS > (uint32_t) g ? bsFWho[g >> 1] >> (g & 1) * 4 & 15u : 0u;
    return dw != nullptr && dw->objt.count == BS_F_OBJECTS ? i : 0u;
}

// R5-1: the lift (bottomDy) an entry's flight lands at, which the battle's first split frame keeps: a followed group's
// is the menu box's (BS_A_UP: BS_F_MID puts the menu box's centre there), any other the scene's (sceneDy).
static BS_CODEF int32_t bsFDest(const DataWin* dw, int32_t layout, int32_t g, int32_t sceneDy) {
    return layout == CTR_BATTLE_LAYOUT_A && bsFClass(dw, g) != 0 ? -BS_A_UP : sceneDy;
}

static BS_CODEF void bsClearLayerF(Renderer* renderer) {
    bsClearLayer14(renderer);
    Bs* bs = bsState.bs;
    Runner* runner = renderer->runner;
    if (bs == nullptr || runner == nullptr || bs->layout != CTR_BATTLE_LAYOUT_A) return;
    int32_t g = bs->group;
    if (bsFBs != bs || bs->frames == 0 || bsFGroup != g) {
        bsFBs = bs;
        bsFGroup = (int16_t) g;
        uint32_t i = bsFClass(runner->dataWin, g);
        bsFCapNow = bsFWho[BS_F_CAPS + i];
        bsFOk = i != 0;
    }
    float perPx = (bs->viewBottom - bs->viewTop) / 240.0f;
    // bsgroup (z_bsgroup.c): a scene mode past battlesplit's (6, 7) sets its place and line each frame, before the draws
    if (0.0f < perPx) CtrBsgroup_frameF(renderer, bsState.battleRoom, g, &bs->place, bs->viewTop, perPx, &bs->splitY, &bs->outer);
    Instance* u = bsFOk != 0 ? bsLive(runner, bsState.border[1]) : nullptr;
    Instance* d = u != nullptr ? bsLive(runner, bsState.border[3]) : nullptr;
    // bsmex (z_bsmex.c): scene mode 8 (Mettaton EX) sets its place, its lines and the whole flag each frame
    CtrBsmex_frameF(renderer, bsState.battleRoom, g, &bs->place, bs->viewTop, perPx, &bs->splitY, &bs->turn14, bs->frames);
    if (d == nullptr) return;
    // The box measured against the view's top as the room's data places it (room_battle's view 0: 0), not the camera:
    // a view shake (obj_shaker moves view_yview: obj_knighthammer, obj_tsunderplane_body, obj_wordburst,
    // obj_strongpunch, obj_bulletbomb, obj_stormstar_gen) then leaves the lift as it is, so the box shakes with the HP
    // line and the buttons, as on PC (bsgroup, 11-13). hideY below stays on the camera: it is the screen's rows.
    const Room* room = runner->currentRoom;
    float top = room != nullptr && room->views != nullptr ? (float) room->views[0].viewY : bs->viewTop;
    float c = ((u->y + d->y) * 0.5f - top) / perPx - BS_F_MID; // the rows the centre is below its lifted place
    int32_t want = !(c > 0.0f) ? 0 : c >= (float) BS_A_UP ? BS_A_UP : (int32_t) (c + 0.5f); // a NaN: 0
    bool bomb = false;
    for (int32_t k = 0; 4 > k; k++) {
        if (bsLive(runner, 3 > k ? BS_F_BOMB + k : BS_F_CARROTS) == nullptr) continue;
        want = 0;
        bomb |= 2 > k;
    }
    bool attack = false; // global.mnfight 2: no rise, and down at once
    if (BS_F_GEN > bsFCapNow) {
        int32_t mn = bsFPick(bs, runner, 1);
        if (mn != 0 && mn != 3 && want > bsFCapNow) want = bsFCapNow; // the speech, the attack, or not known
        attack = mn == 2;
    }
    int32_t now = -bs->place.bottomDy, step = want - now;
    // the first frame takes the wanted lift at once; a split handed over from an entry keeps its flight's lift that
    // frame (an attack still goes down at once), then moves as on any frame
    if (bs->handF != 0) step = attack && 0 > step ? step : 0;
    else if (bsFOk != 1) step = step > BS_F_UP ? BS_F_UP : -BS_F_DOWN > step && !attack ? -BS_F_DOWN : step;
    if (bsFOk == 1) logInfo(bsLogF, (int) bs->group, (int) want);
    bsFOk = 3;
    bs->handF = 0;
    if (step > 0 && (attack || bsLive(runner, BS_F_BULLET) != nullptr)) step = 0;
    now = bomb ? 0 : now + step;
    bs->place.bottomDy = (int16_t) -now;
    bs->hideY = bs->viewTop + perPx * (float) now;
}

// Group 56 (11-12: each object in one place). In mode 5 every BS_BOTH draw's arena copy is cut at
// battlesplit17's row, as the automatic straddle's is: an object of class both (obj_spiderpour) and one the guard made
// BS_BOTH for the rest of the battle (a bottom draw in the rows a lifted place hides). Their copies put the rows above
// the cut on the arena layer again, which a later centred place shows on the bottom screen under the top screen's copy.
// maintainer's play1 log (11-12, kit-playtest7) has the case: in 3 of his 6 fights the guard promoted the sign spider in a
// close seam, and from then on it was doubled (r4 below keeps it from being promoted at all; the cut stays for the
// pour and any other promoted object). bsRouteAt11f's plain route calls this in place
// of bsRouteAt (same call, only the target differs: battlesplit14 and 11f keep their sizes); BS_AGAIN's switch to the
// arena layer, the next, takes the cut (__wrap_CtrRenderer_bsLayer). The guard's own draw (bsBoth17, unmarked) is
// whole: that frame's lift hides the rows the cut would take. Outside mode 5 nothing changes.
// r3, the climbs (turn14 3, bsPlaceTurn14's bottom whole, kept until the menu box): no arena copy at all. There the cut
// is the box top (room y 90 at its highest: border setup's 22 and 23, yzero - 10, yzero down to 100), so a cut copy
// would show its rows from there to the band on both screens. The case: as climbs 4 and 9 end, the next turn's sign
// spider walks in (obj_spiderb Step at mnfight 3) while the box takes about 11 frames to come back down (obj_uborder,
// 15 px a frame); its body's layer rows 110..119 were on both screens. Bound (11-12): in a climb the
// only BS_BOTH draws are the pour, not drawn (obj_spiderpour Create sets visible 0 once obj_spiderb_body.purple > 80,
// which only grows, by 4 a landed pour, after attack 0), and the sign spider, layer rows 55..119, all above the
// standard band: the top screen shows them whole. A climb BS_BOTH draw reaching row 125 would lose those rows
// (test_battlesplit_g56.py's climb list fails then). A split begun mid-attack (turn14 5) keeps the cut copy.
// r4 (11-13: one copy in one place in every fight): in her fight a draw of obj_signspider is never
// made BS_BOTH. Its want loses BS_GUARD here, so bsRouteAt's guard is not asked: it stays a bottom draw, as in every
// fight where the guard never saw it, and is on the bottom screen in every menu. The cost: while a close seam's narrow
// box hides its rows (attack 0's start, the box narrowing as it walks out; in maintainer's log 0.5 to 0.8 s after the pick),
// those rows are not shown. They are above the bottom screen, and the top screen does not draw a bottom object.
// bsGuard's seam log (dev builds) is not written for it. 11f's check for the guard's own draw reads its own want and
// the class cache, which stays BS_BOTTOM: no both. By index, as bsfollow's objects (BS_F_OBJECTS: UNDERTALE 1.08's
// Steam and GOG data, the same indices); data with another count keeps the guard.
// noinline and noclone but not noipa (BS_CODEF's): with noipa, 11f's callers no longer knew which registers it keeps
// (IPA-RA), and nine routed wrappers in .text_battlesplit grew (184 B of late text).
#define BS_G56_SIGN 358 // obj_signspider
#define BS_CODEF56 __attribute__((noinline, noclone, optimize("Os"), section(".text_bsfollow")))
// Mettaton EX's whole place (z_bsmex.c, scene mode 8: his heart turns): turn14 BS_MEX_WHOLE, and every draw goes on
// the arena layer, whatever its class (so no BS_BOTH copy and no guard). Only mode 5 reads turn14 otherwise (0 to 5),
// and mode 8's mode14 is 0. A BY_CALL straddle never reaches here first: the whole place's splitY is a screen above
// the view (bsRouteAt11f's first branch needs a draw that starts above it).
#define BS_MEX_WHOLE 8
_Static_assert(BS_MEX_WHOLE == CTR_BSMEX_WHOLE, "z_bsmex.c's whole flag");
static BS_CODEF56 bool bsRouteAt56(Renderer* renderer, int32_t want, const float* r) {
    Bs* bs = bsState.bs;
    if (bs->turn14 == BS_MEX_WHOLE) want = BS_BOTTOM; // Mettaton EX's whole place
    if (bs->group == BS_MUFFET && bs->lastObject == BS_G56_SIGN && bs->count == BS_F_OBJECTS) want &= ~BS_GUARD; // r4
    bool both = bsRouteAt(renderer, want, r);
    if (both && bs->mode14 == BS_MODE_TURN) {
        if (bs->turn14 == 3) return false; // a climb: the top screen's copy only (r3)
        bs->spare14[0] = 1;
    }
    return both;
}
#endif // ENABLE_WAD16 (battlesplit14)

#ifdef CTRPORT_STALECARRY
// Development builds only (CMakeLists.txt's stalecarry block): battle_probe's 32 while a bench runs, the death flash
// watch's switch (z_stalecarry.c). Any thread, so settings.ini is not read here: the rows' --ini line sets it at start.
bool CtrBattlesplit_flashProbe(void);
__attribute__((noinline, noipa, optimize("Os"), section(".text_stalecarry"))) bool CtrBattlesplit_flashProbe(void) {
    return (bsState.probe & 32) != 0 && CtrBench_running();
}
#endif
#endif
