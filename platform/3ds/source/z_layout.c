// The element layout model (change layout, UNDERTALE): Game, Text and Box places, the presets, the text_place and
// layout_preset settings keys, and the fixed screen targets (S2). ctr_layout.h has the model;
// the dual-screen plan, Part E, the design.
//
// Stage S1 changes nothing on screen. The settings links sit between argui and the screens' settings (ctr_argui.c's
// three CtrDisplay_* calls are pointed here by name in CMakeLists.txt, the same size): load reads the two keys after
// the screens' load, --ini lines may give them for one run, and the options menu's save writes them back. All code,
// constants and state are late (late_reserve.ld, "layout"): no earlier address moves, no BSS. The state is one small
// root in late data; the held targets' state is on the heap, made at the first two-target layout.
//
// Size (relready's reserve forecast): release builds log only a refusal; the other notes (targets taken, swaps, let
// go, lent) are test builds' (CTRPORT_LAYOUT_TEST), where the transition rows read them.
//
// CTR_LAYOUT_SETTINGS_ONLY: the model and the settings links alone, for the host test (tools/analysis/test_layout.py).
#if (defined(ENABLE_WAD16) && defined(__3DS__)) || defined(CTR_LAYOUT_SETTINGS_ONLY)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#ifndef CTR_LAYOUT_SETTINGS_ONLY
#include <3ds.h>
#include <citro3d.h>

#include "ctr_textures.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#endif

#include "ctr_display.h"
#include "ctr_layout.h"
#ifndef CTR_LAYOUT_SETTINGS_ONLY
#include "ctr_menu.h"
#endif

#if defined(CTRPORT_LAYOUT_TEST) && !defined(CTR_LAYOUT_SETTINGS_ONLY)
static void lyTestLoad(const char* path);
#define LY_TEST_LOAD(path) lyTestLoad(path)
#else
#define LY_TEST_LOAD(path) ((void) 0)
#endif

#ifndef CTR_LAYOUT_SETTINGS_ONLY
#define LY_CODE __attribute__((noinline, section(".text_layout")))
#define LY_CONST __attribute__((section(".rodata_layout_in_text")))
#define LY_STATE __attribute__((section(".data_layout")))
// The appended pair (change layout F8, F8b and S3 on dv8d): code changed or added after .text_layout went into dev, in
// sections of their own after paintchg's provider tail (late_reserve.ld), so .text_layout and everything after it
// keep their addresses. noinline, not noipa (RULES: a noipa callee of old code changes the old caller).
#define LY_CODE2 __attribute__((noinline, section(".text_layout2")))
#define LY_CONST2 __attribute__((section(".rodata_layout2_in_text")))
#define LY_STATE2 __attribute__((section(".data_layout2")))
#else
#define LY_CODE
#define LY_CONST
#define LY_STATE
#define LY_CODE2
#define LY_CONST2
#define LY_STATE2
#endif

extern uint8_t CtrScreens_game;
extern uint8_t CtrScreens_gameAtOpen;
extern bool CtrScreens_onBottom;
bool CtrScreens_gameBelow(void);

// battlesplit's setting (the Box place) and its answer for a split frame, where battlesplit is linked (weak
// references: 0 without it).
int32_t CtrBattlesplit_setting(void);
#pragma weak CtrBattlesplit_setting

// The settings links (CMakeLists.txt points ctr_argui.c's calls here) and the screens' links they call.
void __wrap_CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool __wrap_CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __wrap_CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path,
                                 CtrDisplayCaps caps);
void CtrLayout_chainLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool CtrLayout_chainOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void CtrLayout_chainSaveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path,
                              CtrDisplayCaps caps);

// ===[ State ]===

typedef struct {
    int8_t fileText; // text_place as settings.ini has it (load, then the last save): -1 none, 0 with the game, 1 other
    int8_t given;    // a --ini text_place line set the Text place for this run
    int8_t preset;   // the layout_preset label in effect (settings.ini or --ini): -1 none
    uint8_t refused; // S2: the places after the last refused change (lyWantTwo's key, LY_MOVE_REFUSED for a Game screen
                     // change), 0 for none; here, not on the heap, so a refusal needs no allocation
    CtrDisplaySettings* settings;   // main.c's display settings, as loaded (the places the player chose)
    struct LyTargets* targets;      // heap: the held screen targets (made at the first two-target layout)
} LyRoot;
static LyRoot lyRoot LY_STATE = { -1, 0, -1, 0, nullptr, nullptr };
// State added after lyRoot went into dev (F8, S3), its own object in the appended data section (.data_layout2, last in
// late data), so lyRoot keeps its size.
typedef struct {
    uint8_t kept;         // F8: a borrow of the bottom screen was refused; the game stays there until it's given back
    uint8_t up18;         // LY_UP_ON the move up in force, LY_UP_PICKED a look at the next frame's start, LY_UP_WAIT
                          // and LY_UP_PICK what it is for (the end of this file): in kept's padding
    uint8_t lent;         // D: the top target's holder (CTR_LAYOUT_TOP_*) and LY_LENT_LIGHT: in the padding too
    uint8_t carry18;      // D: the move up took a FLIGHT lend: its scanout carry is t->top's picture (padding)
    void* table;          // S3: heap, the scene table as checked (nullptr: none, every battle Original)
    const void* tableFor; // S3: the data.win it was loaded (or tried) for
} LyRoot2;
#ifndef CTR_LAYOUT_SETTINGS_ONLY
static LyRoot2 lyRoot2 LY_STATE2 = { 0, 0, 0, 0, nullptr, nullptr };
#endif

// ===[ Constants ]===

// Game, Text, Box for Classic, 3DS mode, Handheld, Flipped (CTR_LAYOUT_CLASSIC.. order).
static const uint8_t lyPresets[CTR_LAYOUT_PRESETS - 1][3] LY_CONST = {
    { CTR_LAYOUT_GAME_TOP, CTR_LAYOUT_WITH_GAME, CTR_LAYOUT_WITH_GAME },
    { CTR_LAYOUT_GAME_TOP, CTR_LAYOUT_OTHER, CTR_LAYOUT_OTHER },
    { CTR_LAYOUT_GAME_BOTTOM, CTR_LAYOUT_WITH_GAME, CTR_LAYOUT_WITH_GAME },
    { CTR_LAYOUT_GAME_BOTTOM, CTR_LAYOUT_OTHER, CTR_LAYOUT_WITH_GAME },
};
static const char lyKeyFormat[] LY_CONST = " %63[^= ] = %d";
static const char lyTextKey[] LY_CONST = "text_place";
static const char lyPresetKey[] LY_CONST = "layout_preset";
static const char lyGameKey[] LY_CONST = "game_screen";
static const char lyReadMode[] LY_CONST = "r";
static const char lyAppendMode[] LY_CONST = "a";
static const char lyTextLine[] LY_CONST = "text_place = %d\n";
static const char lyPresetLine[] LY_CONST = "layout_preset = %d\n";

// ===[ The model ]===

// Box: on the screen without the game where battlesplit is linked and lyBoxSetting18 says so (this file's end: Split,
// and Top screen while the player's game is below).
static int32_t lyBoxSetting18(void);
static LY_CODE uint8_t lyBox(void) {
    return CtrBattlesplit_setting != nullptr && lyBoxSetting18() != 0 ? CTR_LAYOUT_OTHER : CTR_LAYOUT_WITH_GAME;
}

LY_CODE int32_t CtrLayout_presetOf(const CtrLayoutPlaces* places) {
    for (int32_t i = 0; CTR_LAYOUT_PRESETS - 1 > i; i++) {
        const uint8_t* p = lyPresets[i];
        if (places->game == p[0] && places->text == p[1] && places->box == p[2]) return CTR_LAYOUT_CLASSIC + i;
    }
    return CTR_LAYOUT_CUSTOM;
}

// The label for these places: the stored one while it is Custom or still names them, else the preset they match.
static LY_CODE uint8_t lyLabel(const CtrLayoutPlaces* places, int32_t stored) {
    int32_t match = CtrLayout_presetOf(places);
    if (stored == CTR_LAYOUT_CUSTOM || (stored > CTR_LAYOUT_CUSTOM && stored == match)) return (uint8_t) stored;
    return (uint8_t) match;
}

// S2: whether the game is on the bottom screen only for a battle's automatic move (battlesplit's step 3b): the player's
// Game place is still the top one.
#ifndef CTR_LAYOUT_SETTINGS_ONLY
static LY_CODE bool lyAuto(void);
#else
#define lyAuto() false
#endif

// Text: settings.dialogueBottom is the "other screen" choice. Without a text_place line (settings.ini or --ini), the
// old dialogue_bottom counts only with the game on top (dsplan E4), so a bit set long ago and hidden while the game was
// below never becomes "text on top" once that layout exists. Game: where the player put it (a battle's automatic move
// to the bottom screen leaves it on top).
LY_CODE void CtrLayout_places(const CtrDisplaySettings* settings, CtrLayoutPlaces* out) {
    uint8_t game = CtrScreens_game != 0 && !lyAuto() ? CTR_LAYOUT_GAME_BOTTOM : CTR_LAYOUT_GAME_TOP;
    bool known = lyRoot.fileText >= 0 || lyRoot.given != 0;
    out->game = game;
    out->text = settings != nullptr && settings->dialogueBottom && (known || game == CTR_LAYOUT_GAME_TOP) ? CTR_LAYOUT_OTHER
                                                                                                     : CTR_LAYOUT_WITH_GAME;
    out->box = lyBox();
    out->preset = lyLabel(out, lyRoot.preset);
}

// ===[ Settings ]===

typedef struct {
    int8_t text, preset, game;
} LyFile;

// One of the keys this file reads: its LY_* kind, with the value in range stored in *f (else -LY_* and nothing
// stored); 0 for another line. text_place 0-1, layout_preset 0..CTR_LAYOUT_PRESETS-1, game_screen 0-1.
enum { LY_TEXT = 1, LY_PRESET, LY_GAME };
static LY_CODE int32_t lyLine(const char* line, LyFile* f) {
    char key[64];
    int value;
    if (sscanf(line, lyKeyFormat, key, &value) != 2) return 0;
    int32_t kind = strcmp(key, lyTextKey) == 0 ? LY_TEXT : strcmp(key, lyPresetKey) == 0 ? LY_PRESET
                                                       : strcmp(key, lyGameKey) == 0     ? LY_GAME
                                                                                         : 0;
    if (kind == 0) return 0;
    if (0 > value || value >= (kind == LY_PRESET ? CTR_LAYOUT_PRESETS : 2)) return -kind;
    int8_t* slot = kind == LY_TEXT ? &f->text : kind == LY_PRESET ? &f->preset : &f->game;
    *slot = (int8_t) value;
    return kind;
}

static LY_CODE LyFile lyRead(const char* path) {
    LyFile f = { -1, -1, -1 };
    FILE* file = fopen(path, lyReadMode);
    if (file == nullptr) return f;
    char line[128];
    while (fgets(line, sizeof(line), file) != nullptr) lyLine(line, &f);
    fclose(file);
    return f;
}

// The screens' load (which sets CtrScreens_game, on top for a test run), then the keys. A text_place line is the Text
// place; without one, dialogueBottom stays as dialogue_bottom set it, so every setting combination loads as before.
LY_CODE void CtrLayout_chainLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    __wrap_CtrDisplay_load(settings, path, caps);
    LyFile f = lyRead(path);
    lyRoot.fileText = f.text;
    lyRoot.given = 0;
    lyRoot.preset = f.preset;
    lyRoot.settings = settings;
    if (f.text >= 0) settings->dialogueBottom = f.text != 0;
    LY_TEST_LOAD(path);
}

// --ini text_place=N or layout_preset=N, for this run only (a save never writes them); every other line, game_screen
// included, goes on to the screens.
LY_CODE bool CtrLayout_chainOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    LyFile f = { -1, -1, -1 };
    int32_t kind = lyLine(line, &f);
    if (kind == 0 || kind == LY_GAME || kind == -LY_GAME) return __wrap_CtrDisplay_override(settings, line, caps);
    if (kind < 0) return false;
    if (kind == LY_TEXT) {
        settings->dialogueBottom = f.text != 0;
        lyRoot.given = 1;
    } else {
        lyRoot.preset = f.preset;
    }
    return true;
}

// The screens' save (the display's lines and game_screen: the file's own, with what the menu changed), then the keys.
// text_place is written from the first save in which the menu changed the Text place (the options menu's Dialogue box
// row, shown only with the game on top: E4's "dialogue_bottom=1 is other only when game_screen=0"), and kept from
// then on. layout_preset goes with it (or stays, if the file has one). A run's --ini values are never written back.
LY_CODE void CtrLayout_chainSaveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path,
                                      CtrDisplayCaps caps) {
    LyFile f = lyRead(path);
    __wrap_CtrDisplay_saveEdits(before, after, path, caps);
    bool changed = before->dialogueBottom != after->dialogueBottom;
    int32_t text = changed ? (after->dialogueBottom ? 1 : 0) : f.text;
    if (0 > text && 0 > f.preset) return;
    // The places as written: game_screen as the screens' save wrote it; without text_place, dialogue_bottom's reading.
    uint8_t game = CtrScreens_gameAtOpen != CtrScreens_game ? CtrScreens_game : (uint8_t) (f.game > 0 ? 1 : 0);
    bool other = text >= 0 ? text > 0 : after->dialogueBottom && game == CTR_LAYOUT_GAME_TOP;
    CtrLayoutPlaces written = { game, other ? CTR_LAYOUT_OTHER : CTR_LAYOUT_WITH_GAME, lyBox(), 0 };
    uint8_t label = lyLabel(&written, f.preset);
    FILE* file = fopen(path, lyAppendMode);
    if (file == nullptr) return;
    if (text >= 0) fprintf(file, lyTextLine, (int) text);
    fprintf(file, lyPresetLine, (int) label);
    fclose(file);
    lyRoot.fileText = (int8_t) text;
    if (lyRoot.preset == f.preset) lyRoot.preset = (int8_t) label;
}

#ifndef CTR_LAYOUT_SETTINGS_ONLY
// ===[ Fixed screen targets (S2, dsplan R10) ]===
//
// In a two-target layout (L2-L6: the text or the box on the screen without the game, or the game below with the text on
// top) both screens' targets are held for the session: a 400x240 one shown on the top screen and a 320x240 one shown
// on the bottom (691,200 B, the pair today's bottom dialogue already holds). The renderer keeps using topTarget for the
// game's and bottomTarget for the bottom dialogue's, and these two fill those roles: a move of the game between the
// screens (the options menu's Game screen row, a test borrowing the bottom screen, a stylus battle) is then a swap of
// roles, with no target deleted or made. Classic and Handheld (L1, L5) keep today's one target.
//
// Every screen set-up passes here first (ctr_renderer.c names the entry point in asm: configureScreens' first call, at
// the start of CtrRenderer_beginFrame, outside a frame, whenever the display changed). lyStage takes the pair when a
// two-target layout is first in effect, adopting the targets the renderer already has (at boot: the game's, made by the
// set-up before the settings arrive) and making the rest, in the order screensConfigure made them; it lets the unused one
// go when the player picks a one-target layout; and with the pair held it makes the game's move itself (scanoutMove, the
// move UNDERTALE's set-up makes, with the renderer's two target pointers cleared so the move deletes nothing) and then
// puts each held target in its role, so screensConfigureKeepBottom finds the targets it wants and makes none. A target
// out of a role is unlinked from its screen: nothing is transferred from it, and the consoles or the menu draw there as
// before.
//
// Ownership (who frees a target, and when; c26rev4 F1):
// - Both held targets are layout's until a one-target layout lets one go (lyLetGo: the one in no role is deleted, the
//   one in a role is the renderer's again) or a new renderer starts (the old one's went with it). Nothing else deletes
//   a held target.
// - A split battle borrows the held bottom target (CtrLayout_lendBottom, called where battlesplit made its own) and
//   gives it back with CtrLayout_returnBottom (where it deleted its own), which deletes only a target layout doesn't
//   hold. Layout never makes a target for a split: without the held pair the battle stays Original (R10: per-scene
//   moves never allocate), so there is no third 307,200 B target (F2) and no battlesplit target for layout to adopt.
// - The renderer's own targets (one-target layouts) stay the renderer's; layout adopts them when it takes the pair.
//
// Allocation happens only at a layout change: the options menu's rows change the display, and the next frame's set-up
// (the menu still open) takes the targets: tiles' copies first given up (as the set-up does), then, only if the two
// still don't fit, seam copies. If they still don't, the layout stays as it was (the Text place back with the game), the
// log says so and the options menu's row says so (CtrLayout_menuNoteFor; ctr_optmenu.c). A refused set of places isn't
// tried again until they change. A one-target layout's Game screen change (which deletes the game's target and makes
// the other screen's) is made sure of the same way first; if the new target can't be had, the game stays where it was
// (lyGuardMove8, F5). Moves of the game (the menu or a test borrowing the bottom screen and giving it back, the Game
// screen row) reach the same set-up: the yield wrapper and setDisplay mark the display changed. main's loop calls the
// yield wrapper inside a frame (after CtrRenderer_beginFrame), where no target may be made or deleted; the one call
// outside a frame (a setup notice at boot) is handled in CtrLayout_yield8.
//
// A battle's automatic move (battlesplit's step 3b: the stylus with an Original battle moves the game to the bottom
// screen) goes through CtrLayout_moveGame: only with the pair held (F4; Classic keeps the game where it is), and the
// player's places stay what they chose (CtrLayout_places), so the move is a swap and the pair is kept (F3).

void CtrGpu_waitIdle(void);
uint32_t CtrSeam_dropAll(void);
#pragma weak CtrSeam_dropAll
void CtrGsfix_preConfigure(void* r);
void CtrGsfix_yield(Renderer* renderer, bool yield);
extern bool CtrScreens_gameGiven;
void CtrDimlight_topBacklight(void* renderer, bool off);
extern bool CtrScreens_topBacklightOff; // the renderer's top-light state (screensTopBacklight's cache)
// battlesplit's move of the game (ctr_renderer.c) and the options menu's copy of the settings (the file's end).
uint32_t CtrRenderer_bsMoveGame(Renderer* renderer, bool below, uint32_t saved);
void CtrLayout_menuRefused(bool dialogueBottom);
#pragma weak CtrLayout_menuRefused
void CtrBattlesplit_setSetting(int32_t value);
#pragma weak CtrBattlesplit_setSetting

typedef struct LyTargets {
    void* owner;              // the renderer they are for
    C3D_RenderTarget* top;    // 400x240, shown on the top screen when in a role
    C3D_RenderTarget* bottom; // 320x240, shown on the bottom screen when in a role
    uint32_t taken, swaps, refused, lost;
    uint8_t autoMove; // the game is on the bottom screen for a battle's automatic move (CtrLayout_moveGame)
    uint8_t pad[3];
#ifdef CTRPORT_LAYOUT_TEST
    uint32_t highWater; // screen targets' bytes at most (held and the renderer's own), for the transition tests
    uint8_t* ops;  // layout_ops: frame and letter pairs, run by the yield hook
    int32_t opAt;  // the next op
    void* filler;  // the 'f' op's VRAM blocks (a linked list through their first word)
    bool squeeze;  // the 's' op: the next take's first try counts as not fitting, so seam copies are given up
#endif
} LyTargets;
#ifdef CTRPORT_LAYOUT_TEST
static const char lySqueezeLog[] LY_CONST = "Layout test: squeezed: %lu B of seam copies given up (%.0f KB VRAM free)\n";
#endif

#define LY_R(r, field, type) (*(type*) ((uint8_t*) (r) + Layout_renderer.field))
#define LY_TOP_W 400
#define LY_BOTTOM_W 320
#define LY_TOP_BYTES (240u * LY_TOP_W * 4u)
#define LY_BOTTOM_BYTES (240u * LY_BOTTOM_W * 4u)
#define LY_MOVE_REFUSED 0x10u // lyRoot.refused: a Game screen change was refused (lyWantTwo's keys leave this bit clear)

// The one release log line, a refused change: 'L' the layout's pair, 'M' a one-target Game screen change, 'H' no heap for
// the pair's state, 'B' a borrow of the bottom screen (a failed save, a test's end); VRAM free; seam copies given up.
static const char lyRefusedLog[] LY_CONST = "Layout: %c refused, %.0f KB VRAM free, %lu B seam dropped\n";
#ifdef CTRPORT_LAYOUT_TEST
#define LY_NOTE(...) logInfo(__VA_ARGS__)
static const char lyTookLog[] LY_CONST = "Layout: both screen targets held (%s; %lu B of screen targets, %.0f KB VRAM free)\n";
static const char lyAdopted[] LY_CONST = "the game's kept, one made";
static const char lyBothKept[] LY_CONST = "both kept";
static const char lyBothMade[] LY_CONST = "both made";
static const char lySwapLog[] LY_CONST = "Layout: the game to the %s screen by a swap of the held targets (%lu so far)\n";
static const char lyLetGoLog[] LY_CONST = "Layout: one screen target let go (%.0f KB VRAM free)\n";
static const char lyScanoutLog[] LY_CONST = "Layout: the game's move couldn't be made in place; the held targets are let go\n";
static const char lyLostLog[] LY_CONST = "Layout: the renderer's %s target isn't a held one (%lu so far)\n";
static const char lyLentLog[] LY_CONST = "Layout: the held bottom target lent to the split battle\n";
static const char lyAutoLog[] LY_CONST = "Layout: the battle's move of the game to the bottom screen needs both screen targets held; the game stays on top\n";
static const char lyTopName[] LY_CONST = "top";
static const char lyBottomName[] LY_CONST = "bottom";
static const char lyGameName[] LY_CONST = "game's";
#else
#define LY_NOTE(...) ((void) 0)
#endif
static const char lyMenuNote[] LY_CONST = "Not enough video memory for this layout.";
static const char lyMoveNote[] LY_CONST = "Not enough video memory to move the game.";

static LY_CODE bool lyHeld(const LyTargets* t) {
    return t != nullptr && t->top != nullptr;
}

static LY_CODE bool lyAuto(void) {
    const LyTargets* t = lyRoot.targets;
    return t != nullptr && t->autoMove != 0 && CtrScreens_game == 1;
}

// Whether the layout in effect wants both targets, and a key for its places. With the game on top that is the
// renderer's own display (bottom dialogue, the box below), as screensConfigure reads it, so the pair is taken by the
// set-up that would have made the bottom target; during a battle's automatic move (which narrows that display as the
// game goes below) the player's settings instead. With the game below, the player's places (the renderer's display then
// has the dialogue off): the text on top (L6). Wide (a test only since R3) is left to the renderer.
// The places and the game-below test go through lyWantPlaces18 and lyWantBelow18 (the move up's section: the move up
// and a Box on the other screen with the game below want the pair too).
static void lyWantPlaces18(const CtrDisplaySettings* settings, CtrLayoutPlaces* out);
static bool lyWantBelow18(void);
static LY_CODE bool lyWantTwo(void* r, uint8_t* key) {
    const CtrDisplaySettings* d = &LY_R(r, display, CtrDisplaySettings);
    if (lyAuto() && lyRoot.settings != nullptr) d = lyRoot.settings;
    CtrLayoutPlaces p;
    lyWantPlaces18(lyRoot.settings != nullptr ? lyRoot.settings : d, &p);
    *key = (uint8_t) (0x80u | p.game | (uint32_t) p.text << 1 | (uint32_t) p.box << 2 | (d->dialogueBottom ? 8u : 0u));
    if (p.game == CTR_LAYOUT_GAME_TOP || !lyWantBelow18())
        return d->topMode == TOP_MODE_STANDARD && (d->dialogueBottom || p.box == CTR_LAYOUT_OTHER);
    return p.text == CTR_LAYOUT_OTHER;
}

static LY_CODE C3D_RenderTarget* lyMake(int32_t width) {
    return C3D_RenderTargetCreate(240, width, GPU_RB_RGBA8, -1);
}

#ifdef CTRPORT_LAYOUT_TEST
// Screen target bytes held now: the pair's and any of the renderer's own.
static LY_CODE uint32_t lyBytes(void* r, const LyTargets* t) {
    C3D_RenderTarget* game = LY_R(r, topTarget, C3D_RenderTarget*);
    C3D_RenderTarget* below = LY_R(r, bottomTarget, C3D_RenderTarget*);
    uint32_t n = 0;
    if (lyHeld(t)) n = LY_TOP_BYTES + LY_BOTTOM_BYTES;
    bool ours = lyHeld(t) && (game == t->top || game == t->bottom);
    if (game != nullptr && !ours) n += 240u * (uint32_t) LY_R(r, topW, int32_t) * 4u;
    if (below != nullptr && !(lyHeld(t) && below == t->bottom)) n += LY_BOTTOM_BYTES;
    return n;
}

#include "ctr_memory.h"
static const char lyGapLog[] LY_CONST =
    "Layout test: %s: screen targets %lu B (high-water %lu B), largest VRAM gap %lu B (bank A %lu, bank B %lu)\n";
// The largest free gap in each 3 MB bank (blocks start at 128-byte boundaries), and the high-water, after each step.
static LY_CODE void lyTestRecord(void* r, LyTargets* t, const char* what) {
    if (t == nullptr) return;
    uint32_t bytes = lyBytes(r, t);
    if (bytes > t->highWater) t->highWater = bytes;
    CtrVramBlock* blocks = malloc(256 * sizeof(CtrVramBlock));
    uint32_t gap[2] = { 0, 0 };
    int32_t n = blocks != nullptr ? CtrMemory_vramBlocks(blocks, 256) : -1;
    uintptr_t bank = (uintptr_t) OS_VRAM_SIZE / 2u;
    for (uintptr_t b = 0; 2u > b && n >= 0; b++) {
        uintptr_t lo = (uintptr_t) OS_VRAM_VADDR + b * bank, hi = lo + bank, start = lo;
        for (int32_t k = 0; n > k; k++) {
            uintptr_t at = blocks[k].addr;
            if (lo > at || at >= hi) continue;
            uintptr_t from = (start + 127u) & ~(uintptr_t) 127u;
            if (at > from && at - from > gap[b]) gap[b] = (uint32_t) (at - from);
            if (at + blocks[k].size > start) start = at + blocks[k].size;
        }
        uintptr_t from = (start + 127u) & ~(uintptr_t) 127u;
        if (hi > from && hi - from > gap[b]) gap[b] = (uint32_t) (hi - from);
    }
    free(blocks);
    logInfo(lyGapLog, what, (unsigned long) bytes, (unsigned long) t->highWater,
            (unsigned long) (gap[0] > gap[1] ? gap[0] : gap[1]), (unsigned long) gap[0], (unsigned long) gap[1]);
}
#define LY_RECORD(r, t, what) lyTestRecord(r, t, what)
#else
#define LY_RECORD(r, t, what) ((void) 0)
#endif

// A refused layout change: the player's choice goes back to what is in effect (the Text and Box places with the game,
// or the game on the screen it is on), the refusal is kept for the options menu's note until the places change again,
// and the menu's own copy of the settings (and its page) follow. No allocation: a failed allocation of LyTargets ends
// here too. The Box place goes back too, or a split battle would find no bottom target at every frame of the fight.
static LY_CODE void lyRefuse(void* r, LyTargets* t, bool move) {
    if (t != nullptr) t->refused++;
    if (move) {
        CtrScreens_game = CtrScreens_onBottom ? 1 : 0;
    } else {
        if (lyRoot.settings != nullptr) lyRoot.settings->dialogueBottom = false;
        LY_R(r, display, CtrDisplaySettings).dialogueBottom = false;
        if (CtrBattlesplit_setSetting != nullptr && lyBox() == CTR_LAYOUT_OTHER) CtrBattlesplit_setSetting(0);
    }
    uint8_t key;
    lyWantTwo(r, &key);
    lyRoot.refused = (uint8_t) (key | (move ? LY_MOVE_REFUSED : 0u));
    if (CtrLayout_menuRefused != nullptr)
        CtrLayout_menuRefused(lyRoot.settings != nullptr ? lyRoot.settings->dialogueBottom
                                                         : LY_R(r, display, CtrDisplaySettings).dialogueBottom);
}

// Seam copies given up when a target doesn't fit (tiles' copies are already gone): the bytes given up.
static LY_CODE uint32_t lySqueeze(void) {
    CtrGpu_waitIdle();
    uint32_t dropped = CtrSeam_dropAll != nullptr ? CtrSeam_dropAll() : 0;
#ifdef CTRPORT_LAYOUT_TEST
    logInfo(lySqueezeLog, (unsigned long) dropped, (double) vramSpaceFree() / 1024.0);
#endif
    return dropped;
}

// Takes the pair: the renderer's targets kept where they are the right size, the rest made (400 first, as
// screensConfigure makes the game's before the bottom one). Tiles' copies go first, as at every set-up; seam copies only
// when the two don't fit. Not in roles yet (lyPlace). False when they still don't fit: nothing new is kept, and the
// Text place goes back with the game.
static LY_CODE bool lyTake(void* r, LyTargets* t) {
    C3D_RenderTarget* game = LY_R(r, topTarget, C3D_RenderTarget*);
    C3D_RenderTarget* below = LY_R(r, bottomTarget, C3D_RenderTarget*);
    int32_t topW = LY_R(r, topW, int32_t);
    C3D_RenderTarget* have4 = game != nullptr && topW == LY_TOP_W ? game : nullptr;
    C3D_RenderTarget* have3 = game != nullptr && topW == LY_BOTTOM_W ? game : below;
    CtrTextures_releaseMirrors(&LY_R(r, textures, CtrTextureCache), true);
    C3D_RenderTarget* made4 = nullptr;
    C3D_RenderTarget* made3 = nullptr;
    uint32_t dropped = 0;
    for (int32_t pass = 0; 2 > pass; pass++) {
        if (have4 == nullptr) made4 = lyMake(LY_TOP_W);
        if (have3 == nullptr) made3 = lyMake(LY_BOTTOM_W);
        bool fit = (have4 != nullptr || made4 != nullptr) && (have3 != nullptr || made3 != nullptr);
#ifdef CTRPORT_LAYOUT_TEST
        if (fit && pass == 0 && t->squeeze) fit = t->squeeze = false;
#endif
        if (fit) break;
        if (made4 != nullptr) C3D_RenderTargetDelete(made4);
        if (made3 != nullptr) C3D_RenderTargetDelete(made3);
        made4 = made3 = nullptr;
        if (pass == 0) {
            dropped = lySqueeze();
            continue;
        }
        lyRefuse(r, t, false);
        logWarn(lyRefusedLog, 'L', (double) vramSpaceFree() / 1024.0, (unsigned long) dropped);
        return false;
    }
    t->owner = r;
    t->top = have4 != nullptr ? have4 : made4;
    t->bottom = have3 != nullptr ? have3 : made3;
    t->taken++;
    LY_NOTE(lyTookLog, have4 != nullptr && have3 != nullptr ? lyBothKept : have4 != nullptr || have3 != nullptr ? lyAdopted : lyBothMade,
            (unsigned long) lyBytes(r, t), (double) vramSpaceFree() / 1024.0);
    return true;
}

// Lets the pair go: the one in no role is deleted, the renderer's own (its topTarget, its bottomTarget) are its again.
static LY_CODE void lyLetGo(void* r, LyTargets* t) {
    C3D_RenderTarget* game = LY_R(r, topTarget, C3D_RenderTarget*);
    C3D_RenderTarget* below = LY_R(r, bottomTarget, C3D_RenderTarget*);
    C3D_RenderTarget* held[2] = { t->top, t->bottom };
    t->top = t->bottom = nullptr;
    t->autoMove = 0;
    for (int32_t i = 0; 2 > i; i++) {
        if (held[i] != game && held[i] != below) C3D_RenderTargetDelete(held[i]);
    }
    LY_NOTE(lyLetGoLog, (double) vramSpaceFree() / 1024.0);
}

// Each held target in its role for the game's screen: the game's linked to that screen (as createScreenTarget links
// it), the bottom one in bottomTarget when the bottom dialogue (or magnifier) wants it there, where the set-up links it
// (or unlinks it while the screen is borrowed). The one in no role is unlinked (lyDetach18, C3D_RenderTargetDetachOutput
// spelled out as citro3d's inline has it, so the call's arguments are set up as before: but not the top one while lent,
// the end of this file).
static void lyDetach18(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags);
static LY_CODE void lyPlace(void* r, LyTargets* t, bool onBottom) {
    const CtrDisplaySettings* d = &LY_R(r, display, CtrDisplaySettings);
    bool bottomWanted = !onBottom && (d->bottomMode == BOTTOM_MODE_MAGNIFIER || d->dialogueBottom);
    C3D_RenderTarget* game = onBottom ? t->bottom : t->top;
    C3D_RenderTarget* other = onBottom ? t->top : t->bottom;
    gfxScreen_t screen = onBottom ? GFX_BOTTOM : GFX_TOP;
    if (other->linked) lyDetach18(NULL, other->screen, other->side, 0);
    if (!game->linked || game->screen != screen || game->side != GFX_LEFT || game->transferFlags != Layout_renderer.displayFlags)
        C3D_RenderTargetSetOutput(game, screen, GFX_LEFT, Layout_renderer.displayFlags);
    LY_R(r, topTarget, C3D_RenderTarget*) = game;
    LY_R(r, topW, int32_t) = onBottom ? LY_BOTTOM_W : LY_TOP_W;
    LY_R(r, bottomTarget, C3D_RenderTarget*) = bottomWanted ? other : nullptr;
#ifdef CTRPORT_LAYOUT_TEST
    uint32_t bytes = lyBytes(r, t);
    if (bytes > t->highWater) t->highWater = bytes;
#endif
}

// The guard (lyGuardMove8, the appended section).
static void lyGuardMove8(void* r, LyTargets* t);
// lyPlace and the pair let go after a swap that no longer wants it; lyStage for the yield hook (the move up's section, U4).
static void lyPlace18(void* r, LyTargets* t, bool onBottom);
static void lyStage18(void* r);

// Before every screen set-up, outside a frame (see the section's comment).
static LY_CODE void lyStage(void* r) {
    LyTargets* t = lyRoot.targets;
    C3D_RenderTarget** game = &LY_R(r, topTarget, C3D_RenderTarget*);
    C3D_RenderTarget** below = &LY_R(r, bottomTarget, C3D_RenderTarget*);
    // A new renderer (no game target yet): the pair was the old one's, and went with it.
    if (lyHeld(t) && (t->owner != r || *game == nullptr)) {
        t->top = t->bottom = nullptr;
        t->autoMove = 0;
    }
    // A battle's move given up without its way back (the menu's Game screen row put the game on top: battlesplit's drop).
    if (t != nullptr && t->autoMove != 0 && CtrScreens_game != 1) t->autoMove = 0;
    // Something else put its own target in a role: counted and logged, for the transition tests (none expected: a split
    // battle borrows the held bottom target). One in the bottom role is deleted, as the renderer's set-up deletes a bottom
    // target it doesn't want; lyPlace puts the held one there.
    if (lyHeld(t) && *game != t->top && *game != t->bottom) {
        t->lost++;
        LY_NOTE(lyLostLog, lyGameName, (unsigned long) t->lost);
    }
    if (lyHeld(t) && *below != nullptr && *below != t->bottom && *below != t->top) {
        t->lost++;
        LY_NOTE(lyLostLog, lyBottomName, (unsigned long) t->lost);
        C3D_RenderTargetDelete(*below);
        *below = nullptr;
    }
    uint8_t key;
    bool two = lyWantTwo(r, &key);
    if (lyRoot.refused != 0 && (lyRoot.refused & ~LY_MOVE_REFUSED) != key) lyRoot.refused = 0;
    if (lyHeld(t) && !two) {
        lyLetGo(r, t);
        LY_RECORD(r, t, "let go");
    } else if (!lyHeld(t) && two && lyRoot.refused != key) {
        if (t == nullptr) {
            t = calloc(1, sizeof(LyTargets));
            if (t == nullptr) {
                lyRefuse(r, nullptr, false);
                logWarn(lyRefusedLog, 'H', (double) vramSpaceFree() / 1024.0, 0ul); // 'H': no heap for the state
            } else {
                lyRoot.targets = t;
            }
        }
        if (t != nullptr && lyTake(r, t)) {
            LY_RECORD(r, t, "taken");
        } else {
            LY_RECORD(r, t, "refused");
        }
    }
    if (!lyHeld(t)) {
        lyGuardMove8(r, t);
        return;
    }
    bool onBottom = CtrScreens_gameBelow() && !LY_R(r, bottomYielded, bool);
    if (onBottom != CtrScreens_onBottom) {
        C3D_RenderTarget* was0 = *game;
        C3D_RenderTarget* was1 = *below;
        *game = nullptr;
        *below = nullptr;
        if (!Layout_renderer.scanoutMove(r, onBottom)) {
            *game = was0;
            *below = was1;
            LY_NOTE(lyScanoutLog);
            lyLetGo(r, t);
            return;
        }
        t->swaps++;
        LY_NOTE(lySwapLog, onBottom ? lyBottomName : lyTopName, (unsigned long) t->swaps);
        lyPlace18(r, t, onBottom);
        LY_RECORD(r, t, onBottom ? "swap to bottom" : "swap to top");
        return;
    }
    lyPlace18(r, t, onBottom);
}

#ifdef CTRPORT_LAYOUT_TEST
static LY_CODE void lyTestOps(Renderer* renderer, LyTargets* t);
#define LY_TEST_OPS(renderer) lyTestOps(renderer, lyRoot.targets)
#else
#define LY_TEST_OPS(renderer) ((void) 0)
#endif

// configureScreens' first call (r0 = the renderer), before fps75Configure and screensConfigureKeepBottom.
LY_CODE void CtrLayout_preConfigure(void* r) {
    CtrGsfix_preConfigure(r);
    lyStage(r);
}

LY_CODE bool CtrLayout_targetsHeld(void) {
    return lyHeld(lyRoot.targets);
}

// A split battle's bottom screen target (CtrRenderer_bsPrepare calls this where it made its own, with the same
// arguments): the held bottom one, lent, while the game is on the top screen; else none, and the battle stays Original
// (layout logged why when it refused the pair).
LY_CODE struct C3D_RenderTarget_tag* CtrLayout_lendBottom(int width, int height, int colorFmt, int depthFmt) {
    (void) colorFmt;
    (void) depthFmt;
    LyTargets* t = lyRoot.targets;
    if (width != 240 || height != LY_BOTTOM_W || !lyHeld(t) || CtrScreens_onBottom) return nullptr;
    LY_NOTE(lyLentLog);
    return t->bottom;
}

// The split's bottom target given back (where CtrRenderer_bsPrepare deleted its own): a held one stays layout's; any
// other (one a let-go left in the bottom role during the split) is deleted, as battlesplit did.
LY_CODE void CtrLayout_returnBottom(struct C3D_RenderTarget_tag* target) {
    LyTargets* t = lyRoot.targets;
    if (target == nullptr || (lyHeld(t) && (target == t->top || target == t->bottom))) return;
    C3D_RenderTargetDelete(target);
}

// battlesplit's move of the game for a stylus Original battle (bsTouchMove's CtrRenderer_bsMoveGame calls). Going below
// needs the held pair (R10; c26rev4 F4): without it the game stays on top (battlesplit set CtrScreens_game, which is put
// back: its next frame sees the change and drops the move) and nothing else changes. With it, the move is marked so the
// player's places stay theirs (CtrLayout_places, lyWantTwo) and the set-up swaps the held targets (F3).
LY_CODE uint32_t CtrLayout_moveGame(Renderer* renderer, bool below, uint32_t saved) {
    LyTargets* t = lyRoot.targets;
    if (below && !lyHeld(t)) {
        CtrScreens_game = 0;
        LY_NOTE(lyAutoLog);
        return saved;
    }
    if (t != nullptr) t->autoMove = below ? 1 : 0;
    return CtrRenderer_bsMoveGame(renderer, below, saved);
}

LY_CODE const char* CtrLayout_menuNoteFor(int32_t row) {
    uint8_t refused = lyRoot.refused;
    if (refused == 0) return nullptr;
    bool move = (refused & LY_MOVE_REFUSED) != 0;
    if (row == CTR_LAYOUT_ROW_GAME) return move ? lyMoveNote : nullptr;
    if (row == CTR_LAYOUT_ROW_TEXT) return move ? nullptr : lyMenuNote;
    return nullptr;
}

#if defined(ENABLE_WAD16) && defined(__3DS__)
// The options menu's side (c26rev4 F6). Its main page is drawn and keyed through battletouch's rows (z_battletouch.c's
// obRow, reached through builderx_compile.py's routing of ctr_optmenu.c), whose call for optmenu's own rows comes here
// (one call retargeted, the same arguments): after a refusal the row that asked shows the note in place of its help
// line (drawn for the cursor's row) until the places change again. ctr_optmenu.c itself is unchanged: code added there
// reorders what GCC already emits in it (docs/3ds-hardware.md). optRow and the menu's state are reached by the names
// that routing gives them; ctr_optmenu.c checks these offsets for z_battletouch.c.
typedef struct {
    CtrMenu* menu;
    uint32_t pointers[3];
    uint8_t page, cur[4], scr, pref;
    bool placed, dirty;
} LyOptState;
typedef struct {
    const char *label, *value, *help;
    bool dis;
} LyOptRow;
_Static_assert(sizeof(void*) != 4 || (offsetof(LyOptState, page) == 16 && offsetof(LyOptState, dirty) == 24 &&
                                      offsetof(LyOptRow, help) == 8 && offsetof(LyOptRow, dis) == 12),
               "ctr_optmenu.c's OptState and OptRow");
extern LyOptState CtrOptbattle_state;
void CtrOptbattle_row(const CtrMenu* menu, uint32_t page, uint32_t i, LyOptRow* r);
enum { LY_OPT_GAME = 0, LY_OPT_DIALOGUE = 1 }; // ctr_optmenu.c's OPT_MAIN_GAME, OPT_MAIN_DIALOGUE

// The row through lyOptRow18 (the move up's section: the Game screen row shows the player's Bottom while it lasts).
static void lyOptRow18(const CtrMenu* menu, uint32_t page, uint32_t i, LyOptRow* r);
LY_CODE void CtrLayout_optRow(const void* menu, uint32_t page, uint32_t i, void* row) {
    LyOptRow* r = row;
    lyOptRow18((const CtrMenu*) menu, page, i, r);
    const char* note = nullptr;
    if (page == 0 && i == LY_OPT_GAME) note = CtrLayout_menuNoteFor(CTR_LAYOUT_ROW_GAME);
    if (page == 0 && i == LY_OPT_DIALOGUE) note = CtrLayout_menuNoteFor(CTR_LAYOUT_ROW_TEXT);
    if (note != nullptr) r->help = note;
}

// A refusal while the menu is open: its own copy of the settings (main.c copies it to the game's at each change) gets
// the revert too, so the row shows what is in effect and the next change doesn't ask again; the page is drawn again.
LY_CODE void CtrLayout_menuRefused(bool dialogueBottom) {
    CtrMenu* menu = CtrOptbattle_state.menu;
    if (menu == nullptr || !menu->open) return;
    menu->settings.dialogueBottom = dialogueBottom;
    CtrOptbattle_state.dirty = true;
}
#endif

#ifdef CTRPORT_LAYOUT_TEST
// ===[ Transition test driver (test builds: CTRPORT_LAYOUT_TEST) ]===
//
// settings.ini's layout_ops = <frame><letter>[,<frame><letter>...] runs each step once the game reaches that frame (at
// the yield, inside the frame):
//   m  the game to the other screen (the options menu's Game screen row; a test run's game_screen given)
//   d  the Text place to the other one (the options menu's Dialogue row: one target to two or back)
//   f  VRAM filled with 64 KB blocks until none fits (the layout change after it must be refused, and play go on)
//   F  those blocks freed
//   s  the next take's first try counts as not fitting (the seam copies given up, then the second try)
//   h  a HOME Menu or sleep return's set-up (CtrRenderer's APT hook: displayDirty)
//   r  a record line (targets, high-water, largest gaps) with no step
//   v  a failed save (main's warning then borrows the bottom screen from the next frame: F8's other borrow)
//   o  Battle layout Original picked in the options menu (btLayoutStep18's two calls: the setting, then pick18)
//   a  Battle layout 3DS Mode (Split) picked the same way
//   u  Battle layout Top screen picked the same way
//   T  the top target lent to FLIGHT with light (commit D's forced mid-flight proof: a stand-in for battlesplit T1),
//      linked to the top screen RGB565 and filled magenta each frame, double buffered, while the lend lasts
//   U  FLIGHT gives it back (a dropped move)
//   D  the top target lent to DIALOGUE with light (a stand-in for dlgtop's box), filled cyan each frame
//   E  DIALOGUE gives it back
static const char lyOpsKey[] LY_CONST = "layout_ops";
static const char lyTestLendLog[] LY_CONST = "Layout test: lend '%c': %s\n";
static const char lyOpsLog[] LY_CONST = "Layout test: frame %d step '%c'\n";
static const char lyFillLog[] LY_CONST = "Layout test: %lu blocks of VRAM taken (%.0f KB free)\n";
static const char lyLoadLog[] LY_CONST = "Layout test: %d steps from %s\n";

static LY_CODE void lyTestLoad(const char* path) {
    FILE* file = fopen(path, lyReadMode);
    if (file == nullptr) return;
    char line[256];
    while (fgets(line, sizeof(line), file) != nullptr) {
        size_t n = strlen(lyOpsKey);
        const char* p = line;
        while (*p == ' ') p++;
        if (strncmp(p, lyOpsKey, n) != 0) continue;
        p += n;
        while (*p == ' ' || *p == '=') p++;
        LyTargets* t = lyRoot.targets;
        if (t == nullptr) t = lyRoot.targets = calloc(1, sizeof(LyTargets));
        if (t == nullptr) break;
        free(t->ops);
        t->ops = calloc(strlen(p) + 2, 5); // a record per letter at most, and a zero one to end
        t->opAt = 0;
        if (t->ops == nullptr) break;
        // Kept as "frame letter" records: a 4-byte frame then the letter.
        size_t k = 0;
        while (*p >= '0' && *p <= '9') {
            int32_t frame = 0;
            while (*p >= '0' && *p <= '9') frame = frame * 10 + (*p++ - '0');
            char op = *p != '\0' ? *p++ : 'r';
            memcpy(t->ops + k, &frame, 4);
            t->ops[k + 4] = (uint8_t) op;
            k += 5;
            while (*p == ',' || *p == ' ') p++;
        }
        logInfo(lyLoadLog, (int) (k / 5), path);
        break;
    }
    fclose(file);
}

static LY_CODE void lyTestOps(Renderer* renderer, LyTargets* t) {
    if (t == nullptr || t->ops == nullptr || renderer->runner == nullptr) return;
    void* r = renderer;
    int32_t frame = renderer->runner->frameCount;
    // A test lend's frame (T, D): its colour into the lent target, as the holder's draw would be, inside the frame.
    int holder = CtrLayout_topHolder();
    if (holder != CTR_LAYOUT_TOP_FREE && t->top->linked && LY_R(r, inFrame, bool)) {
        gfxSetDoubleBuffering(GFX_TOP, true);
        C3D_FrameBufClear(&t->top->frameBuf, C3D_CLEAR_COLOR, holder == CTR_LAYOUT_TOP_FLIGHT ? 0xFF00FFFFu : 0x00FFFFFFu, 0);
        t->top->used = true;
    }
    for (;;) {
        uint8_t* at = t->ops + t->opAt * 5;
        char op = (char) at[4];
        int32_t when;
        memcpy(&when, at, 4);
        if (op == '\0' || when > frame) return;
        t->opAt++;
        logInfo(lyOpsLog, (int) frame, op);
        if (op == 'm') {
            CtrScreens_game ^= 1u;
            CtrScreens_gameGiven = true;
        } else if (op == 'd' && lyRoot.settings != nullptr) {
            lyRoot.settings->dialogueBottom = !lyRoot.settings->dialogueBottom;
            lyRoot.given = 1;
        } else if (op == 'f') {
            uint32_t n = 0;
            for (void* b; (b = vramAlloc(64u * 1024u)) != nullptr; n++) {
                *(void**) b = t->filler;
                t->filler = b;
            }
            logInfo(lyFillLog, (unsigned long) n, (double) vramSpaceFree() / 1024.0);
        } else if (op == 'F') {
            while (t->filler != nullptr) {
                void* next = *(void**) t->filler;
                vramFree(t->filler);
                t->filler = next;
            }
        } else if (op == 's') {
            t->squeeze = true;
        } else if (op == 'h') {
            LY_R(r, displayDirty, bool) = true;
        } else if (op == 'v') {
            void CtrSafeSave_failed(void);
            CtrSafeSave_failed();
        } else if ((op == 'o' || op == 'a' || op == 'u') && CtrBattlesplit_setSetting != nullptr) {
            void CtrLayout_pick18(Renderer* renderer);
            CtrBattlesplit_setSetting(op == 'o' ? 0 : op == 'a' ? 1 : 3);
            CtrLayout_pick18(renderer);
        } else if (op == 'T' || op == 'D') {
            C3D_RenderTarget* top = CtrLayout_lendTop(renderer, op == 'T' ? CTR_LAYOUT_TOP_FLIGHT : CTR_LAYOUT_TOP_DIALOGUE, true);
            if (top != nullptr) {
                uint32_t flags = (Layout_renderer.displayFlags & ~GX_TRANSFER_OUT_FORMAT(7u)) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565);
                C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, flags);
            }
            logInfo(lyTestLendLog, op, top != nullptr ? "lent and linked" : "refused");
        } else if (op == 'U' || op == 'E') {
            int was = CtrLayout_topHolder();
            CtrLayout_returnTop(renderer, op == 'U' ? CTR_LAYOUT_TOP_FLIGHT : CTR_LAYOUT_TOP_DIALOGUE);
            logInfo(lyTestLendLog, op, was != CtrLayout_topHolder() ? "given back" : "no-op");
        }
        LY_RECORD(r, t, "step");
    }
}
#endif

// ===[ F8's guard and yield (the appended pair) ]===
// lyStage's call and ctr_renderer.c's yield hook (CtrScreens_yieldHook) name these (F5, F8 and F8b; the versions
// before F8 went in the post-G8 move commit).

// A one-target layout's Game screen change to the top screen (the options menu's row, which lends the menu the bottom
// screen as the game goes up; a test's), before the set-up makes it: the set-up deletes the game's 320x240 target and
// must make a 400x240 one, and aborts if it can't (c26rev4 F5). (A change to the bottom screen always fits: its target
// is smaller than the one it replaces.) Made sure of here first: the new target made beside the old one (tiles' copies
// given up, then seam copies) and given back at once, so the set-up's own make fits; if it doesn't fit beside it, the
// old one is given up and the new one tried in its place (the set-up then makes it there). If it fits neither way, a
// target of the old size goes back on the bottom screen (where the old one was), the game stays there with the menu on
// top (the screen it lent the menu taken back, as at the menu's next call) and the menu's Game screen row says so. A
// borrow's move (game_screen still the bottom; a failed save or a test's end borrows the screen) is made sure of the same
// way (F8: the set-up aborted there on Old with VRAM fragmented, 736 KB free and no 384,000 B gap); refused, the game
// stays below, the borrow is held off (CtrLayout_yield8) and no menu note is set: game_screen didn't change.
static LY_CODE2 void lyGuardMove8(void* r, LyTargets* t) {
    C3D_RenderTarget** game = &LY_R(r, topTarget, C3D_RenderTarget*);
    bool* yielded = &LY_R(r, bottomYielded, bool);
    bool borrow = CtrScreens_gameBelow();
    if (*game == nullptr || !CtrScreens_onBottom || (borrow && !*yielded)) return;
    int32_t oldW = LY_R(r, topW, int32_t), needW = LY_TOP_W;
    if (needW <= oldW) return;
    CtrTextures_releaseMirrors(&LY_R(r, textures, CtrTextureCache), true);
    uint32_t dropped = 0;
    C3D_RenderTarget* probe = lyMake(needW);
    if (probe == nullptr) {
        dropped = lySqueeze();
        probe = lyMake(needW);
    }
    if (probe == nullptr) {
        // lySqueeze waited for the GPU: the old target isn't in use.
        C3D_RenderTargetDelete(*game);
        *game = nullptr;
        probe = lyMake(needW);
        if (probe == nullptr) {
            *game = lyMake(oldW);
            if (*game != nullptr) C3D_RenderTargetSetOutput(*game, GFX_BOTTOM, GFX_LEFT, Layout_renderer.displayFlags);
            *yielded = false;
            if (borrow) {
                // Held, and the top screen lit for the borrower's console now: a setup notice yields only once.
                lyRoot2.kept = 1;
                CtrDimlight_topBacklight(r, false);
            } else {
                lyRefuse(r, t, true);
            }
            logWarn(lyRefusedLog, borrow ? 'B' : 'M', (double) vramSpaceFree() / 1024.0, (unsigned long) dropped);
            return;
        }
    }
    C3D_RenderTargetDelete(probe);
}

// The yield wrapper's real call (main's CtrRenderer_yieldBottomScreen, through gsfix). main's loop calls it inside a
// frame, and the move it asks for waits for the next set-up. Outside a frame (a setup notice taking the bottom screen)
// the wrapper moves the game at once; with the pair held that move is made here first, as a swap with the set-up the
// wrapper would have run, so the wrapper finds the game where it wants it and deletes nothing. With one target, a move
// up is made sure of first (lyGuardMove8), as before the set-up: the options menu closing after its Game screen row asked
// for the top screen is such a call (Handheld, the menu on top; layout's run N2 aborted there when the 400x240 target
// couldn't be made). Refused, the game stays below and the wrapper finds it where it wants it. Otherwise a load and a
// return. A refused borrow (lyGuardMove8, F8) is held off until the screen is given back: no yield, so no set-up and no
// new try each frame, and the top screen lit, where the menu console (a failed save's warning, a test's end) prints with
// the game below; given back, the set-up puts the top backlight back as the settings say.
LY_CODE2 void CtrLayout_yield8(Renderer* renderer, bool yield) {
    void* r = renderer;
    if (lyRoot2.kept && yield && CtrScreens_gameBelow()) {
        CtrDimlight_topBacklight(r, false);
    } else if (lyRoot2.kept) {
        // Given back: a set-up while held asked for Off and keep-lit lit the screen instead, so the cached state (Off)
        // is wrong. Put it in step without a power call that could fail (c26rev4 F8b F1): the light is On as far as
        // anyone asked; the set-up marked here then powers it off when Off is in force, through dimlight, whose newer
        // request supersedes any queued one and is retried if it fails.
        lyRoot2.kept = 0;
        CtrScreens_topBacklightOff = false;
        LY_R(r, displayDirty, bool) = true;
        CtrGsfix_yield(renderer, yield);
    } else {
        CtrGsfix_yield(renderer, yield);
    }
    LY_TEST_OPS(renderer);
    LyTargets* t = lyRoot.targets;
    if (t != nullptr && t->autoMove != 0 && CtrScreens_game != 1) t->autoMove = 0;
    if (LY_R(r, inFrame, bool)) return;
    if (!lyHeld(t)) {
        lyGuardMove8(r, t);
        return;
    }
    bool onBottom = CtrScreens_gameBelow() && !LY_R(r, bottomYielded, bool);
    if (onBottom == CtrScreens_onBottom) return;
    lyStage18(r);
    if (lyHeld(lyRoot.targets)) Layout_renderer.keepBottom(r);
}

// ===[ Keep-lit while a borrow is refused (F8) ]===
// CMake points ctr_screens.c's power-off wrapper's keep-lit question here (the same call, another target): while a
// refused borrow holds the game below, the top screen, where the borrower's console shows (a setup notice, the failed
// save's warning), stays lit through any set-up's Off (HOME, sleep, a dirty display) until it is given back.
// The keep-lit question goes through lyKeepLit18 (commit D's section: the top lent with light too).
bool CtrKeeplit_keepLit(u32 screen);
static bool lyKeepLit18(u32 screen);
LY_CODE2 bool CtrLayout_keepLit(u32 screen) {
    return lyKeepLit18(screen) | (lyRoot2.kept != 0 && screen == GSPLCD_SCREEN_TOP);
}

// ===[ The scene table (S3; dsplan B5, E5) ]===
// ctr_layout.h has the format; tools/layout/build_scenes.py writes it at build time. Read whole at the first lookup
// for a data.win (battlesplit's first split battle), checked whole, then kept for the session on the heap (about
// 1.5 KB). A file that fails a check is freed at once (one log line); every battle is Original (F7).

uint32_t RoomPlan_fnv1a(const uint8_t* data, size_t size);
static const char lyTablePath[] LY_CONST2 = "romfs:/layout/undertale.lst";
static const char lyTableRead[] LY_CONST2 = "rb";
static const char lyTableLog[] LY_CONST2 = "Layout: no scene table (%d)";

// Read whole and checked whole; why it can't be used: 1 no file, 2 its size, 3 its header (format, or made for other
// data), 4 its checksum, 5 an index out of range.
static LY_CODE2 void lyTableLoad(DataWin* dw) {
    free(lyRoot2.table);
    lyRoot2.tableFor = dw;
    uint8_t* data = nullptr;
    int32_t why = 1;
    long size = -1;
    FILE* f = fopen(lyTablePath, lyTableRead);
    if (f != nullptr) {
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        why = 2;
        if (size >= (long) sizeof(CtrLayoutTableHeader) && CTR_LAYOUT_TABLE_MAX >= (unsigned long) size &&
            fseek(f, 0, SEEK_SET) == 0 && (data = (uint8_t*) malloc((size_t) size)) != nullptr &&
            fread(data, 1, (size_t) size, f) == (size_t) size)
            why = 0;
        fclose(f);
    }
    const CtrLayoutTableHeader* h = (const CtrLayoutTableHeader*) data;
    do {
        if (why != 0) break;
        // The header's first 32 bytes as they must be: magic, version, the data.win's key, its room and object counts.
        const struct { uint32_t magic, version; CtrPackKey key; uint32_t rooms, objects; } want = {
            CTR_LAYOUT_TABLE_MAGIC, CTR_LAYOUT_TABLE_VERSION, CtrPack_keyFor(dw), dw->room.count, dw->objt.count };
        _Static_assert(sizeof(want) == 32 && sizeof(CtrPackKey) == 16, "the header's first 32 bytes");
        why = 3;
        if (memcmp(h, &want, sizeof(want)) != 0) break;
        uint32_t scenes = h->sceneCount, entries = h->entryCount;
        why = 2;
        if (h->bodySize != (uint32_t) size - sizeof(*h) ||
            h->bodySize != scenes * sizeof(CtrLayoutScene) + entries * sizeof(CtrLayoutEntry))
            break;
        why = 4;
        if (h->checksum != RoomPlan_fnv1a(data + sizeof(*h), h->bodySize)) break;
        why = 5;
        const CtrLayoutScene* s = (const CtrLayoutScene*) (h + 1);
        const CtrLayoutEntry* e = (const CtrLayoutEntry*) (s + scenes);
        uint32_t i = 0;
        while (scenes > i && (s[i].room == CTR_LAYOUT_ANY || h->roomCount > s[i].room) && entries >= (uint32_t) s[i].first + s[i].count)
            i++;
        if (scenes > i) break;
        for (i = 0; entries > i && h->objectCount > e[i].object; i++) {
        }
        if (entries > i) break;
        why = 0;
    } while (false);
    if (why != 0) {
        free(data);
        data = nullptr;
        logWarn(lyTableLog, (int) why);
    }
    lyRoot2.table = data;
}

// room and group as uint16_t: -1 is CTR_LAYOUT_ANY; one past 65534 has no scene.
LY_CODE2 const CtrLayoutScene* CtrLayout_scene(DataWin* dataWin, int32_t room, int32_t group) {
    if (dataWin == nullptr || (uint32_t) room + 1u > CTR_LAYOUT_ANY || (uint32_t) group + 1u > CTR_LAYOUT_ANY) return nullptr;
    if (lyRoot2.tableFor != dataWin) lyTableLoad(dataWin);
    const CtrLayoutTableHeader* h = (const CtrLayoutTableHeader*) lyRoot2.table;
    if (h == nullptr) return nullptr;
    const CtrLayoutScene* s = (const CtrLayoutScene*) (h + 1);
    for (uint32_t i = 0; h->sceneCount > i; i++) {
        if (s[i].room == (uint16_t) room && s[i].group == (uint16_t) group) return &s[i];
    }
    return nullptr;
}

LY_CODE2 const CtrLayoutEntry* CtrLayout_entries(const CtrLayoutScene* scene) {
    const CtrLayoutTableHeader* h = (const CtrLayoutTableHeader*) lyRoot2.table;
    return (const CtrLayoutEntry*) ((const CtrLayoutScene*) (h + 1) + h->sceneCount) + scene->first;
}

// ===[ The move up (maintainer 11-10; rulings of 11-10) ]===
// "Every screen should be able to go everywhere": with the player's game on the bottom screen, Battle layout Split
// (shown as "3DS Mode") or Top screen moves the game to the top screen for the battle and back below after it
// (battlesplit's bsTouchRoom18 calls CtrLayout_moveUp at the room changes). The mirror of battlesplit's step 3b
// (CtrLayout_moveGame): a swap of the held targets (R10: never an allocation). The pair is held from the moment the
// player picks either value with the game below (CtrLayout_pick18 asks for a set-up, whose lyWantTwo then wants it) and
// while the move lasts. lyRoot2.up18's LY_UP_ON marks the move (autoMove stays step 3b's alone), so the player's
// places, the Game screen row and settings.ini keep Bottom; the display settings stay the player's (the battle is drawn
// as with the game on top). Every change to earlier code keeps its size and place (lead 11-10; check_layout --full against dv8r): lyBox,
// lyWantTwo and optRow call lyBoxSetting18, lyWantPlaces18, lyWantBelow18 and lyOptRow18 in place of the calls of
// CtrBattlesplit_setting, CtrLayout_places, CtrScreens_gameBelow and CtrOptbattle_row (the same calls, other targets).
// Each calls the function it stands in for, so it takes that call's place in the file's call graph, by which GCC orders
// a section's functions: no earlier function moves. In layout's AAC input (.aacout.text.layoutup), the slot's suffix.
// Per frame only CtrLayout_chain18's byte test (LAY-1); the rest: CtrLayout_autoUp at battlesplit's room changes and a
// pick's frame, lyWantPlaces18 and lyWantBelow18 at a set-up, lyBoxSetting18 in lyBox (a set-up, a save, a refusal),
// lyOptRow18 for each menu row drawn.
#if defined(ENABLE_WAD16)
#define LY_CODE18 __attribute__((noinline, noipa, section(".aacout.text.layoutup")))
#define LY_IPA18 __attribute__((noinline, noclone, section(".aacout.text.layoutup")))
#define LY_CONST18 __attribute__((section(".aacout.rodata.layoutup")))
// Commit D's new code and constants: one appended late-text pair (late_reserve.ld, last among the appended sections),
// named outside .text.* and .rodata.* (the late object catch-all for z_layout.o would take those); nothing pins its
// address.
#define LY_CODED __attribute__((noinline, noipa, section(".text_layoutd")))
#define LY_CONSTD __attribute__((section(".rodata_layoutd")))
#else
#define LY_CODE18 __attribute__((noinline, noipa))
#define LY_IPA18 __attribute__((noinline, noclone))
#define LY_CONST18
#define LY_CODED __attribute__((noinline, noipa))
#define LY_CONSTD
#endif
#ifdef CTRPORT_LAYOUT_TEST
static const char lyUpLog[] LY_CONST18 = "Layout: the battle's move of the game to the %s screen (%s)\n";
static const char lyUpMade[] LY_CONST18 = "a swap of the held targets";
static const char lyUpRefused[] LY_CONST18 = "refused: the pair isn't held, the game stays on the bottom screen";
static const char lyUpPicked[] LY_CONST18 = "Layout: a Game screen change during the move up: the player picked Top\n";
static const char lyUpDialogue[] LY_CONSTD = "refused: the top dialogue holds the top target, the battle stays on the bottom screen";
static const char lyUpTook[] LY_CONSTD = "the flight's lend taken over, its last frame carried";
static const char lyLendLog[] LY_CONSTD = "Layout: the top target %s %d (light %d)\n";
static const char lyLendMade[] LY_CONSTD = "lent to";
static const char lyLendRefused[] LY_CONSTD = "refused to";
static const char lyLendBack[] LY_CONSTD = "given back by";
static const char lyLendEnded[] LY_CONSTD = "lend ended by the game's move to the top screen, holder";
static const char lyCarryLog[] LY_CONSTD = "Layout: the move up's carry from the top target (the flight's last frame)\n";
#endif

// The move up in force (LY_UP18, CtrLayout_autoUp), and the player's Game place the bottom screen (LY_BELOW18: the game
// below and not for step 3b, or the move up in force). Data only, written out where they're read: GCC orders a
// section's functions by the call graph, walked from each called function up to its callers, and a call from these
// stand-ins to a function new here would walk up to lyBox's or lyWantTwo's callers sooner than before and move them.
// lyRoot2.up18's bits: the move up in force; a look at the next frame's start (LAY-1: CtrLayout_chain18), asked by a
// Battle layout pick or by the top dialogue's give-back (D); D2 (lead 11-10): which of the two the look is for. A pick
// (LY_UP_PICK) always makes the repick. A give-back makes it only after a move up refused because the dialogue held
// the top, in this room (LY_UP_WAIT: set at that refusal, cleared at the look and at each room change).
#define LY_UP_ON 1u
#define LY_UP_PICKED 2u
#define LY_UP_WAIT 4u
#define LY_UP_PICK 8u
#define LY_UP18() ((lyRoot2.up18 & LY_UP_ON) != 0 && CtrScreens_game == 0)
#define LY_BELOW18() (CtrScreens_game != 0 ? lyRoot.targets == nullptr || lyRoot.targets->autoMove == 0 : LY_UP18())
// D: the top target's holder now (CTR_LAYOUT_TOP_*; FREE once the pair is gone, whatever lyRoot2.lent still says).
#define LY_LENT_HOLDER 3u
#define LY_LENT_LIGHT 4u
#define LY_HOLDER18() \
    (lyRoot.targets != nullptr && lyRoot.targets->top != nullptr ? (uint32_t) (lyRoot2.lent & LY_LENT_HOLDER) : 0u)
// D2 (LAYD-2): lyRoot2.carry18 is for the move up that set it, in that frame-start chain (bsTouchRoom18 -> bsSetUp18);
// CtrLayout_chain18 ends it at the next frame's start. The rule: no second carry-consuming move of the game (scanout's
// carry copy, the menu page's restore) before then. A refused scanout lets the pair go (t->top nullptr) and the
// renderer's own move carries nothing (both users test the pair). A later set-up in the chain (a screenshot's) may take
// the pair again with carry18 still set, but the game is on top already: no Game screen change, so nothing uses it.

// battlesplit's bsTouchRoom18 asks this first at each room change (its only caller), so a move up the dialogue held off
// in the room left behind is not retried (D2: LY_UP_WAIT is cleared here). A repick asks it too, in the same room: its
// decision then refuses again (WAIT again) or wants no move.
LY_CODE18 bool CtrLayout_autoUp(void) {
    lyRoot2.up18 &= (uint8_t) ~LY_UP_WAIT;
    return LY_UP18();
}

// lyBox's call (battlesplit linked): nonzero for the Box on the screen without the game. Split always; Top screen with
// the player's game below (the whole battle on the top screen, which with the game on top is the game's own screen).
static LY_CODE18 int32_t lyBoxSetting18(void) {
    int32_t v = CtrBattlesplit_setting();
    return v != 0 && (v != CTR_LAYOUT_BATTLE_TOP || LY_BELOW18());
}

// A flip of the game while the move up lasts (the options menu's Game screen row, which shows the player's Bottom then;
// a test's 'm') is the player picking Top: the game stays up, the move ends (battlesplit has no way back to make then),
// and the menu's save writes Top (game_screen is written when it differs from its value at the menu's opening, which
// was the top screen). Made at the first look after the flip: the row's next drawing (the menu draws the page after
// each change), or the next set-up.
static inline __attribute__((always_inline)) void lyUpFlip18(void) {
    if (CtrScreens_game != 1) lyRoot2.up18 &= (uint8_t) ~LY_UP_WAIT; // D2 (LAYD-1): a Game screen change ends the wait
    if ((lyRoot2.up18 & LY_UP_ON) == 0 || CtrScreens_game == 0) return;
    CtrScreens_game = 0;
    CtrScreens_gameAtOpen = 1;
    lyRoot2.up18 &= (uint8_t) ~LY_UP_ON;
    LY_NOTE(lyUpPicked);
}

// lyWantTwo's places: the player's (CtrLayout_places), with the game below while the move up lasts (the player's
// Bottom), and the text on the other screen then and with the game below and the Box on the other screen (Split or Top
// screen picked), so both want the pair: the move up is a swap. While the top target is lent (D) the text and the Box
// count as on the other screen whatever the player picks, so no set-up lets the pair go under the holder (a one-target
// layout picked meanwhile lets it go at the first set-up after the return).
// The pin (U4, LAY-1): a held pair stays wanted while a battle's automatic move below is in force (battlesplit's stylus
// move, step 3b: Battle layout and the Dialogue box are live in its menu, and a one-target choice there must not let
// the pair go under the move) and while the game changes screens (a move's end, its drop, a live Battle layout pick's
// move, any flip with the pair held): that set-up swaps the held targets, and only then does lyPlace18 ask again and
// let the pair go if the places now want one target. The game so lands in the held target of its size: nothing is made
// at a move's end (a new 400x240 or 320x240 one can be refused, leaving the game on the wrong screen). The move up pins
// through LY_UP18 the same way.
static inline __attribute__((always_inline)) bool lyPin18(void) {
    const LyTargets* t = lyRoot.targets;
    if (t == nullptr || t->top == nullptr || t->owner == nullptr) return false;
    if (t->autoMove != 0 && CtrScreens_game == 1) return true;
    return (CtrScreens_gameBelow() && !LY_R(t->owner, bottomYielded, bool)) != CtrScreens_onBottom;
}

static LY_IPA18 void lyWantPlaces18(const CtrDisplaySettings* settings, CtrLayoutPlaces* out) {
    lyUpFlip18();
    CtrLayout_places(settings, out);
    bool up = LY_UP18() || lyPin18();
    if (up) out->game = CTR_LAYOUT_GAME_BOTTOM;
    if (up || (out->game == CTR_LAYOUT_GAME_BOTTOM && out->box == CTR_LAYOUT_OTHER)) out->text = CTR_LAYOUT_OTHER;
    if (LY_HOLDER18() != 0) out->text = out->box = CTR_LAYOUT_OTHER;
}

// lyWantTwo's CtrScreens_gameBelow: the player's game is below while the move up lasts too (and pinned, U4).
static LY_CODE18 bool lyWantBelow18(void) {
    return CtrScreens_gameBelow() || LY_UP18() || lyPin18();
}

// lyStage's lyPlace (its one tail call, both paths): the held targets in their roles, then the pair let go if the
// places want one target now that the game is on its screen (the pin above held it for the swap). With no swap the
// answer is the one lyStage had (it let go already when the pair wasn't wanted).
static LY_CODE18 void lyPlace18(void* r, LyTargets* t, bool onBottom) {
    lyPlace(r, t, onBottom);
    uint8_t key;
    if (!lyWantTwo(r, &key)) {
        lyLetGo(r, t);
        LY_RECORD(r, t, "let go");
    }
}

// CtrLayout_yield8's lyStage (a swap outside a frame): a pair let go after its swap (lyPlace18) gets the set-up the
// yield hook gives a held one (keepBottom); the wrapper's own then finds the game where it wants it.
static LY_CODE18 void lyStage18(void* r) {
    bool was = lyHeld(lyRoot.targets);
    lyStage(r);
    if (was && !lyHeld(lyRoot.targets) &&
        (CtrScreens_gameBelow() && !LY_R(r, bottomYielded, bool)) == CtrScreens_onBottom)
        Layout_renderer.keepBottom(r);
}

// battlesplit's move at a battle's first room (up) and at the first room after it (down). Up needs the pair held, the
// game below where the player put it and no move in force; down, the move up in force. False otherwise, nothing changed.
// Up asserts CtrLayout_canMoveUp's predicate (all but the group: battlesplit's caller checks bsTouchGroup just before):
// with the top lent to DIALOGUE (a top box still open at the battle's first room) the battle stays below, one dev line.
// A FLIGHT lend is taken over (D): the lend ends here (FREE; T1's later returnTop is a no-op), and this frame's
// scanout move carries t->top's last flight frame to the top screen in place of the bottom's (CtrLayout_carry18).
// The predicate (the group aside; D's CtrLayout_canMoveUp adds it): the pair held, the game below where the player put
// it, no move in force, Battle layout Split or Top screen, the top not lent to DIALOGUE.
static inline __attribute__((always_inline)) bool lyCanUp18(void) {
    const LyTargets* t = lyRoot.targets;
    int32_t v = CtrBattlesplit_setting != nullptr ? CtrBattlesplit_setting() : 0;
    return t != nullptr && t->top != nullptr && CtrScreens_game == 1 && CtrScreens_gameBelow() && t->autoMove == 0 &&
           (lyRoot2.up18 & LY_UP_ON) == 0 && v != 0 && LY_HOLDER18() != CTR_LAYOUT_TOP_DIALOGUE;
}

// battlesplit part C's latch (W1, each frame of a SOUL flight with the game below): would the move up be made now (the
// group aside)? The predicate only: no WAIT, no state. Late text, with W1's.
__attribute__((noinline, noipa, optimize("Os"), section(".text_bsentry"))) bool CtrLayout_canUp18(void) {
    return lyCanUp18();
}

LY_CODE18 bool CtrLayout_moveUp(Renderer* renderer, bool up) {
    if (up) {
        if (!lyCanUp18()) {
            bool held = LY_HOLDER18() == CTR_LAYOUT_TOP_DIALOGUE;
            if (held) lyRoot2.up18 |= LY_UP_WAIT; // D2: the give-back retries this move (returnTop, chain18)
            LY_NOTE(lyUpLog, lyTopName, held ? lyUpDialogue : lyUpRefused);
            return false;
        }
        lyRoot2.carry18 = LY_HOLDER18() == CTR_LAYOUT_TOP_FLIGHT ? 1 : 0;
        if (lyRoot2.carry18 != 0) {
            lyRoot2.lent = 0;
            LY_NOTE(lyUpLog, lyTopName, lyUpTook);
        }
        lyRoot2.up18 |= LY_UP_ON;
        CtrScreens_game = 0;
    } else {
        if (!LY_UP18()) return false;
        lyRoot2.up18 &= (uint8_t) ~LY_UP_ON;
        CtrScreens_game = 1;
    }
    // The renderer's display as main's setDisplay later in this frame would make it for the game's new screen (the
    // player's settings on top; narrowed below), so this frame's set-up, the split check and the backlights follow them.
    void CtrRenderer_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
    if (lyRoot.settings != nullptr) CtrRenderer_setDisplay(renderer, lyRoot.settings);
    LY_R(renderer, displayDirty, bool) = true;
    LY_NOTE(lyUpLog, up ? lyTopName : lyBottomName, lyUpMade);
    return true;
}

// The display settings' chain (A2), as linked for UNDERTALE: main's setDisplay -> menutime's timer -> the screens wrap
// (narrows) -> bottompaint's wrap (forgets a bottom paint on a change) -> dimlight (keep/restore) -> keep-lit (the
// renderer's cached bottom flag and displayDirty on a change) -> popbot -> popbot2 (close a popup with the dialogue gone
// from the bottom) -> errtext's wrap (the controls' prompt rows) -> this -> inputlate's link (the real setDisplay, then
// the frame's input take) -> the real setDisplay. CMake points errtext's continuation here (the same call, another
// target). Inside a frame this hands on to inputlate's link. Before the renderer's frame (its inFrame false) it makes
// the real setDisplay only, which is all inputlate's link makes there too, except in beginFrame's own chain after
// inputlate's beginFrame link and before the real beginFrame: there inputlate would take the frame's input and block
// the frame's later keyboard events (a test's script keys, applied after this). CtrLayout_moveUp is the only caller of
// the chain there, so its call makes every link's state as main's would (the backlight's cached flag, dimlight's keep,
// the bottom paint, popbot's session) before this frame's set-up, and the frame keeps its one input take, main's.
// test_layout_chain18.py fails when a link joins or leaves the chain.
void __real_CtrRenderer_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
void __real_CtrRenderer_chainDisplayInputlate(Renderer* renderer, const CtrDisplaySettings* settings);
void CtrLayout_display18(Renderer* renderer, const CtrDisplaySettings* settings);
LY_CODE18 void CtrLayout_display18(Renderer* renderer, const CtrDisplaySettings* settings) {
    if (LY_R(renderer, inFrame, bool)) __real_CtrRenderer_chainDisplayInputlate(renderer, settings);
    else __real_CtrRenderer_setDisplay(renderer, settings);
}

// battletouch's Battle layout pick (btLayoutStep18): with the player's game below, a set-up at the next frame's start,
// the options menu still open, as a Dialogue box change makes one. Its lyWantTwo then wants the pair for Split or Top
// screen and takes it, or refuses it (Battle layout back to Original, the row's note: lyRefuse), and lets it go for
// Original (unless the text on top wants it). Any pick also marks LY_UP_PICKED, for the next frame's start (below).
LY_CODE18 void CtrLayout_pick18(Renderer* renderer) {
    if (renderer == nullptr) return;
    lyRoot2.up18 |= LY_UP_PICKED | LY_UP_PICK;
    if (CtrScreens_game == 1 && LY_BELOW18()) LY_R(renderer, displayDirty, bool) = true;
}

// LAY-1 (c26rev2): a pick inside a battle applies there, not at the next room. The beginFrame chain as linked for
// UNDERTALE: inputlate's link -> this -> shopsplit's wrap -> battlesplit's chain (its room block, then the frame's
// split) -> the real beginFrame (CMake points inputlate's call here: the same call, another target). Outside the
// frame, the GPU's last frame settled by the set-up as at a room's entry: with a pick waiting, battlesplit's
// CtrBattlesplit_repick18 makes the pick's set-up (the pair taken, or refused: Original back with the row's note) and
// then the room's decision again in the same room (the move up, the move down, or step 3b's end with Top screen), as
// its room block makes it at the entry. Per frame: one byte test. The Game screen row stays the player's: a flip during
// the move ended it (lyUpFlip18), and battlesplit moves nothing with the game where the player put it.
void CtrRenderer_chainBeginFrameBattlesplit(Renderer* renderer);
// D2: the look made for a give-back with no refused move up waiting clears the bits and repicks nothing (no set-up, no
// pick line); the wait also ends here with the game off the bottom (a Game screen change since the refusal), and the
// flight carry of the last frame's move up ends (LAYD-2).
LY_CODE18 void CtrLayout_chain18(Renderer* renderer) {
    lyRoot2.carry18 = 0;
    uint32_t u = lyRoot2.up18;
    if ((u & (LY_UP_PICKED | LY_UP_WAIT)) != 0) {
        if (CtrScreens_game != 1) u &= ~LY_UP_WAIT;
        lyRoot2.up18 = (uint8_t) ((u & LY_UP_PICKED) != 0 ? u & ~(LY_UP_PICKED | LY_UP_PICK | LY_UP_WAIT) : u);
        if ((u & LY_UP_PICKED) != 0 && (u & (LY_UP_PICK | LY_UP_WAIT)) != 0) CtrBattlesplit_repick18(renderer);
    }
    void CtrBattlesplit_entryW1(Renderer* renderer); // battlesplit's SOUL entry, then the chain (z_battlesplit.c's end)
    CtrBattlesplit_entryW1(renderer);
}

// ===[ The held top target's lend (commit D; ctr_layout.h has the contract) ]===
// lyRoot2.lent: the holder and LY_LENT_LIGHT, in LyRoot2's padding; lyRoot2.carry18: the move up took a FLIGHT lend.
// Stand-ins for earlier code's calls (the same calls, other targets; each calls the function it stands in for, so it
// takes that call's place in the file's call graph and nothing earlier moves): lyPlace's detach (lyDetach18), the
// keep-lit question (lyKeepLit18) and scanoutMove's carry copy (CtrLayout_carry18, called from ctr_renderer.c). They
// read the state through macros only (LY_HOLDER18): a call from them to a function new here would walk the call graph
// up to their callers sooner and move them. Per frame: lyKeepLit18 at a power request only, and the holders' own
// topHolder calls; the rest runs at a lend, a give-back or a set-up.
bool CtrBattlesplit_touchGroup18(int32_t group);
#pragma weak CtrBattlesplit_touchGroup18

LY_CODED bool CtrLayout_canMoveUp(int32_t group) {
    return lyCanUp18() && CtrBattlesplit_touchGroup18 != nullptr && CtrBattlesplit_touchGroup18(group);
}

LY_CODED int CtrLayout_topHolder(void) {
    return (int) LY_HOLDER18();
}

LY_CODED struct C3D_RenderTarget_tag* CtrLayout_lendTop(struct Renderer* renderer, int holder, bool light) {
    LyTargets* t = lyRoot.targets;
    bool ok = (holder == CTR_LAYOUT_TOP_FLIGHT || holder == CTR_LAYOUT_TOP_DIALOGUE) && t != nullptr &&
              t->top != nullptr && t->owner == (void*) renderer && CtrScreens_gameBelow() && CtrScreens_onBottom &&
              t->autoMove == 0 && (lyRoot2.up18 & LY_UP_ON) == 0 && LY_HOLDER18() == 0;
    LY_NOTE(lyLendLog, ok ? lyLendMade : lyLendRefused, holder, (int) light);
    if (!ok) return nullptr;
    lyRoot2.lent = (uint8_t) ((uint32_t) holder | (light ? LY_LENT_LIGHT : 0u));
    lyRoot2.carry18 = 0;
    if (light) CtrDimlight_topBacklight(renderer, false);
    return t->top;
}

LY_CODED void CtrLayout_returnTop(struct Renderer* renderer, int holder) {
    LyTargets* t = lyRoot.targets;
    if (holder == CTR_LAYOUT_TOP_FREE || LY_HOLDER18() != (uint32_t) holder) return;
    bool light = (lyRoot2.lent & LY_LENT_LIGHT) != 0;
    lyRoot2.lent = 0;
    lyRoot2.carry18 = 0;
    // LAY-2/6 (9f.2): a move up refused while the dialogue held the top is retried at the next frame's start, the
    // boundary a Battle layout pick uses (CtrLayout_chain18 -> battlesplit's repick: the same battle room only). D2: the
    // look made here repicks only with LY_UP_WAIT set (chain18 decides, so this function's bytes stay D's).
    if (holder == CTR_LAYOUT_TOP_DIALOGUE) lyRoot2.up18 |= LY_UP_PICKED;
    if (t->top->linked) {
        CtrGpu_waitIdle();
        C3D_RenderTargetDetachOutput(t->top);
    }
    if (light) {
        // As F8's give-back: set-ups while lent asked for Off and keep-lit lit the screen instead, so the cached state
        // is put back to On and the set-up marked here powers it off when Off is in force, through dimlight.
        CtrScreens_topBacklightOff = false;
        LY_R(renderer, displayDirty, bool) = true;
    }
    LY_NOTE(lyLendLog, lyLendBack, holder, (int) light);
}

// lyPlace's unlink of the target in no role (C3D_RenderTargetSetOutput's arguments: no target, that one's screen and
// side): never the lent top one (the holder's link); the bottom one in no role means the game is on the top screen,
// whose target t->top is now, which ends any lend. The target linked at (screen, side) is the one lyPlace unlinks: a
// slot holds one target.
static LY_CODED void lyDetach18(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags) {
    const LyTargets* t = lyRoot.targets;
    if (t != nullptr && target == nullptr) {
        const C3D_RenderTarget* top = t->top;
        const C3D_RenderTarget* bottom = t->bottom;
        if (top != nullptr && top->linked && top->screen == screen && top->side == side) {
            if (LY_HOLDER18() != 0) return;
        } else if (bottom != nullptr && bottom->linked && bottom->screen == screen && bottom->side == side) {
            if (lyRoot2.lent != 0)
                LY_NOTE(lyLendLog, lyLendEnded, (int) (lyRoot2.lent & LY_LENT_HOLDER), (int) (lyRoot2.lent >> 2));
            lyRoot2.lent = 0;
            lyRoot2.carry18 = 0;
        }
    }
    C3D_RenderTargetSetOutput(target, screen, side, transferFlags);
}

// The power-off wrapper's keep-lit question: F8's, and the top lent with light.
static LY_CODED bool lyKeepLit18(u32 screen) {
    return CtrKeeplit_keepLit(screen) ||
           (screen == GSPLCD_SCREEN_TOP && LY_HOLDER18() != 0 && (lyRoot2.lent & LY_LENT_LIGHT) != 0);
}

// scanoutMove's copy of the game's last frame into the game screen's spare (memcpy's arguments and result). The move up
// that took a FLIGHT lend carries t->top's picture instead, the flight's last frame, as the frame path would show it
// (the display transfer, BGR8, all 400 columns), so the top shows no bottom frame for a refresh before the battle's
// first (the blink). `out` is the spare plus the top's 40 black columns (ctr_renderer.c's SCREENS_CARRY_AT, BGR8). The
// spare's cleared sides are written out before the transfer (no dirty line lands on it after) and the lines dropped
// after it (scanoutMove copies the spare to the other buffer next, by CPU).
#define LY_CARRY_AT (40u * 240u * 3u)
LY_CODED void* CtrLayout_carry18(void* out, const void* in, size_t n) {
    const LyTargets* t = lyRoot.targets;
    if (lyRoot2.carry18 == 0 || CtrScreens_onBottom || t == nullptr || t->top == nullptr) return memcpy(out, in, n);
    lyRoot2.carry18 = 0;
    u8* spare = (u8*) out - LY_CARRY_AT;
    u32 bytes = 240u * LY_TOP_W * 3u;
    GSPGPU_FlushDataCache(spare, bytes);
    C3D_SyncDisplayTransfer((u32*) t->top->frameBuf.colorBuf, GX_BUFFER_DIM(240, LY_TOP_W), (u32*) spare,
                            GX_BUFFER_DIM(240, LY_TOP_W), Layout_renderer.displayFlags);
    GSPGPU_InvalidateDataCache(spare, bytes);
    LY_NOTE(lyCarryLog);
    return out;
}

#if defined(ENABLE_WAD16) && defined(__3DS__)
// CtrScreens_moveConsoles' copy of the menu console's page to the bottom screen (main.c, UNDERTALE; Loadgame_restoreMenu's
// arguments and result). With the game below, that page is the top screen's shown buffer. While the top was lent to the
// FLIGHT, that buffer held the flight's frames, not the page (T1 stops the consoles for the flight, B5). So the move up
// that took the lend gives the bottom screen a blank page: the flight's last frame had shown behind the log lines there
// until they scrolled over it, and on the whole screen with Other screen Off (Azahar dq66 T3/T3off, 11-10). Every other
// move copies the page as before.
void* Loadgame_restoreMenu(void* dst, const void* src, size_t bytes);
LY_CODED void* CtrLayout_restoreMenu18(void* dst, const void* src, size_t n) {
    const LyTargets* t = lyRoot.targets; // D2 (LAYD-2): the pair still held, as carry18 tests it
    if (lyRoot2.carry18 != 0 && !CtrScreens_onBottom && t != nullptr && t->top != nullptr) return memset(dst, 0, n);
    return Loadgame_restoreMenu(dst, src, n);
}
#endif

#if defined(ENABLE_WAD16) && defined(__3DS__)
static const char lyValBottom[] LY_CONST18 = "< Bottom >";
static const char lyHlpBottom[] LY_CONST18 = "Play on the bottom screen.";
static const char lyNeedsTop[] LY_CONST18 = "Needs the game on top.";
// optRow's row call: a flip during the move up made first (the row shows what is in force), then the menu's row; while
// the move up lasts the Game screen row shows the player's Bottom and the Dialogue box row is grey, as with the game
// below (ctr_optmenu.c's own strings).
static LY_CODE18 void lyOptRow18(const CtrMenu* menu, uint32_t page, uint32_t i, LyOptRow* r) {
    if (page == 0 && i == LY_OPT_GAME) lyUpFlip18();
    CtrOptbattle_row(menu, page, i, r);
    if (page != 0 || i > LY_OPT_DIALOGUE || !LY_UP18()) return;
    if (i == LY_OPT_GAME) {
        r->value = lyValBottom;
        r->help = lyHlpBottom;
    } else {
        r->dis = true;
        r->value = r->help = lyNeedsTop;
    }
}
#endif
#endif // CTR_LAYOUT_SETTINGS_ONLY

#ifdef CTR_LAYOUT_SETTINGS_ONLY
// The host model's lyBox call: the game where CtrScreens_game says (no move up, no step 3b there).
static int32_t lyBoxSetting18(void) {
    int32_t v = CtrBattlesplit_setting();
    return v != 0 && (v != CTR_LAYOUT_BATTLE_TOP || CtrScreens_game != 0);
}
#endif

#endif

// touchui: read the player's text place before screens removes the renderer's
// dialogue flag for a bottom-screen game. Appended in touchui's own late input;
// none of layout's existing bodies, state or settings writes change.
#if (defined(ENABLE_WAD16) && defined(__3DS__)) || defined(CTR_LAYOUT_SETTINGS_ONLY)
__attribute__((noinline, section(".touchui_text")))
bool CtrTouchUi_textOther(void) {
    CtrLayoutPlaces places;
    CtrLayout_places(lyRoot.settings, &places);
    return places.text == CTR_LAYOUT_OTHER;
}
#endif

// S4 reads the player's logical text placement. Target ownership stays in
// layout's exclusive lendTop/returnTop API (commit D), never a private getter.
#ifdef ENABLE_WAD16
#ifndef CTR_LAYOUT_SETTINGS_ONLY
__attribute__((noinline, noipa, used, section(".aacout.text.dlgtop4"), optimize("Os")))
bool CtrLayout_otherText(void) {
    CtrLayoutPlaces places;
    CtrLayout_places(lyRoot.settings, &places);
    return places.text == CTR_LAYOUT_OTHER;
}
#endif
#endif
