// The element layout model (change layout; the dual-screen plan, Part E). Each element (the Game, the Text, the
// battle Box) has a place: the Game is on the Top or Bottom screen, and the Text and the Box are "with the game" (drawn
// as on PC) or on the "other screen". A preset names one set of places; the three places are the truth and the preset is
// only a label (dsplan E4).
//
// Stage S1 (this file's first version) expresses today's settings as places and changes nothing on screen:
//   Game  = game_screen (CtrScreens_game).
//   Text  = text_place when settings.ini has it, else dialogue_bottom read as "other screen" only with the game on top
//           (E4's migration). In memory, CtrDisplaySettings.dialogueBottom now means "Text: other screen"; the
//           renderer routes it to the non-game screen in either direction (S4 borrows S2's held top target).
//   Box   = battle_layout (battlesplit): 0 with the game, 1 (and test-only 2) on the other screen; 3 (Top screen) on
//           the other screen while the player's game is below, else with it. 0 where battlesplit isn't linked.
// UNDERTALE only for now (z_layout.c), like battlesplit.
#ifndef CTR_LAYOUT_H
#define CTR_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "ctr_display.h"

// Game: which screen shows the room view.
enum { CTR_LAYOUT_GAME_TOP = 0, CTR_LAYOUT_GAME_BOTTOM = 1 };
// Text and Box: with the game, or on the screen without it.
enum { CTR_LAYOUT_WITH_GAME = 0, CTR_LAYOUT_OTHER = 1 };
// settings.ini layout_preset. Custom is 0, so an unknown or missing label reads as "work it out from the places".
enum {
    CTR_LAYOUT_CUSTOM = 0,
    CTR_LAYOUT_CLASSIC = 1,  // L1: game on top, everything with it, the bottom screen unused
    CTR_LAYOUT_3DS = 2,      // L4: game on top, text and box below
    CTR_LAYOUT_HANDHELD = 3, // L5: the whole game on the bottom (touch) screen
    CTR_LAYOUT_FLIPPED = 4,  // L6: game below, text on top (S4 borrows the held top target)
    CTR_LAYOUT_PRESETS = 5
};

typedef struct {
    uint8_t game;   // CTR_LAYOUT_GAME_*
    uint8_t text;   // CTR_LAYOUT_WITH_GAME / CTR_LAYOUT_OTHER
    uint8_t box;    // CTR_LAYOUT_WITH_GAME / CTR_LAYOUT_OTHER
    uint8_t preset; // CTR_LAYOUT_* label (CTR_LAYOUT_CUSTOM when the places match no preset)
} CtrLayoutPlaces;

// The places the player chose (settings.ini, the options menu's edits, --ini), from the settings in hand. preset is the
// stored label when it still matches the places (or is Custom), else the preset the places match.
void CtrLayout_places(const CtrDisplaySettings* settings, CtrLayoutPlaces* out);
// The preset whose places these are, or CTR_LAYOUT_CUSTOM.
int32_t CtrLayout_presetOf(const CtrLayoutPlaces* places);

// ===[ Fixed screen targets (S2; dsplan R10) ]===
//
// True while both screens' targets are held for the session (a two-target layout, L2-L6: a 400x240 target for the top
// screen and a 320x240 one for the bottom), so moving the game between the screens for one scene is a swap of the two
// and never allocates. False in Classic and Handheld (one target). A change that links without z_layout.c may define a
// weak copy that returns false.
//
// The targets are taken at the first screen set-up in a two-target layout (boot), or at the set-up after the player
// picks one in the options menu (the menu still open). Tiles' copies are given up first, seam copies only if the two
// still don't fit; if they don't, the layout stays as it was (the Text and Box places go back to "with the game") and
// the options menu's row says so (CtrLayout_menuNoteFor). They are let go only when the player picks a one-target
// layout. The renderer's topTarget/bottomTarget point at them in their roles (the game's, the bottom dialogue's); one
// not in a role is kept unlinked. A one-target layout's Game screen change that can't have its new target is refused
// the same way (the game stays where it was).
//
// Ownership (who frees a held target, and when):
// - Layout owns both held targets. It deletes one only when a one-target layout lets the pair go, and then only the
//   one in no role; the one in a role (topTarget, bottomTarget) is the renderer's, or the borrower's, from then on.
// - A borrower never deletes a held target. A split battle borrows the bottom one through CtrLayout_lendBottom (in
//   place of making its own) and gives it back through CtrLayout_returnBottom (in place of deleting it), which deletes
//   only a target layout no longer holds. Between the two it may link it (C3D_RenderTargetSetOutput) and keep it in
//   the renderer's bottomTarget.
// - Layout never makes a target for a borrower: CtrLayout_lendBottom gives nullptr without the held pair (Classic,
//   Handheld, a refused pair) or with the game on the bottom screen, and the borrower then does without (the battle
//   stays Original).
bool CtrLayout_targetsHeld(void);
// C3D_RenderTargetCreate's arguments (240, 320, GPU_RB_RGBA8, -1): the held 320x240 target, lent, while the game is on
// the top screen (in the bottom dialogue's role or unlinked); nullptr otherwise.
struct C3D_RenderTarget_tag* CtrLayout_lendBottom(int width, int height, int colorFmt, int depthFmt);
// Gives back what CtrLayout_lendBottom lent (nullptr is fine): a held target stays layout's; any other is deleted.
void CtrLayout_returnBottom(struct C3D_RenderTarget_tag* target);
// A per-scene move of the game (battlesplit's step 3b, the stylus moving an Original battle to the bottom screen), in
// place of CtrRenderer_bsMoveGame with its arguments and result. Going below needs the held pair: without it
// CtrScreens_game goes back to 0 (the game stays on top, the caller's next frame drops the move) and nothing changes.
// With it the player's places stay theirs through the move, so the pair is kept and the move is a swap.
struct Renderer;
uint32_t CtrLayout_moveGame(struct Renderer* renderer, bool below, uint32_t saved);
// battle_layout's Top screen value (ctr_battlesplit.h's CTR_BATTLE_LAYOUT_TOP): the whole battle on the top screen.
enum { CTR_LAYOUT_BATTLE_TOP = 3 };
// The move up (maintainer 11-10: every screen everywhere): with the player's game below and the pair held,
// the game goes to the top screen for a battle (battlesplit calls it at a battle's first room when Battle layout is
// Split or Top screen) and comes back below after it (up false). A swap of the held targets, as the Game screen row's:
// the display settings are the player's throughout, so the battle looks as it does with the game on top in the
// matching layout. The player's places, the Game screen row and settings.ini keep Bottom while it lasts. False (and
// nothing changes) without the pair. A Game screen change while it lasts is the player picking Top: the game stays up
// and the move ends there (CtrLayout_autoUp false from then on; battlesplit has no way back to make).
bool CtrLayout_moveUp(struct Renderer* renderer, bool up);
// The move up is in force. battlesplit asks it first at each room change (bsTouchRoom18, its only caller): D2 drops
// there the wait for a move up the top dialogue held off in the room left behind, so ask it nowhere else.
bool CtrLayout_autoUp(void);
// battletouch's Battle layout pick: with the player's game below, a set-up at the next frame's start, which takes the
// pair for Split or Top screen (or refuses it, with the menu's note) and lets it go for Original. Inside a battle the
// pick applies there (LAY-1): at the next frame's start, outside the frame, battlesplit's CtrBattlesplit_repick18 makes
// that set-up and then the room's battle decision again, so the game moves up for Split or Top screen and back below
// for Original in the same room. The Game screen row stays the player's.
void CtrLayout_pick18(struct Renderer* renderer);
// The beginFrame link after inputlate's (CMake points inputlate's call here): a waiting pick's repick, then
// battlesplit's chain (through shopsplit's wrap). D2: also the repick of a move up refused because the top dialogue
// held the top target, after the dialogue's give-back (returnTop), when the refusal is still the latest word: no Game
// screen change, Battle layout pick or room change since. The repick decides for the room current at that frame
// (battlesplit's last room seen), which the room-change clear makes the room of the refusal; no other identity of the
// refused encounter is kept. Each frame: two byte tests and a byte store (the flight carry's end, LAYD-2).
void CtrLayout_chain18(struct Renderer* renderer);
// battlesplit's half of a pick (z_battlesplit.c): the pick's set-up, then the room's move decision in the same room.
void CtrBattlesplit_repick18(struct Renderer* renderer);
// The line for the options menu after a refused layout change ("not enough video memory"), for the row that made the
// change, until the places change again; nullptr otherwise (and for the other rows): CTR_LAYOUT_ROW_TEXT for a refused
// pair (the Dialogue box row; battletouch's Battle layout row may show it too), CTR_LAYOUT_ROW_GAME for a refused Game
// screen change. CtrLayout_optRow is the options menu's main-page row with that note (battletouch's obRow calls it in
// place of optRow: same arguments, row an OptRow); CtrLayout_menuRefused puts a refusal's revert into the open menu's
// copy of the settings (z_layout.c calls it).
enum { CTR_LAYOUT_ROW_GAME = 1, CTR_LAYOUT_ROW_TEXT = 2 };
const char* CtrLayout_menuNoteFor(int32_t row);
void CtrLayout_optRow(const void* menu, uint32_t page, uint32_t i, void* row);
void CtrLayout_menuRefused(bool dialogueBottom);

// ===[ The held top target's lend (layout D, dv8t): the SOUL flight (battlesplit T1) and the top dialogue (dlgtop S4) ]===
// With the game on the bottom screen and both targets held (CtrLayout_targetsHeld), the 400x240 top target is in no
// role. Layout lends it to one holder at a time. While it is lent:
// - the holder may link it to GFX_TOP (C3D_RenderTargetSetOutput) and draw into it each frame. Layout's set-ups
//   (lyStage/lyPlace, at any HOME/sleep return, settings change, notice ending or dirty display) never DETACH it, and
//   never let the pair go (a one-target layout picked meanwhile lets it go at the first set-up after the return);
// - the top's framebuffers are NOT protected. With Other screen Off, the UT configure body (ctr_renderer.c, main text,
//   byte-fixed) still clears both top buffers and sets the top single buffered at every set-up, lent or not. Other
//   paths can also set the top single: the options menu, quitblack. Each set-up first waits for the GPU
//   (CtrGpu_waitIdle) and runs between frames. So a holder must, after any set-up:
//     FLIGHT (T1): call gfxSetDoubleBuffering(GFX_TOP, true) at the start of every flight frame, before its FrameEnd.
//       A flight frame blanked by a set-up logs one dev line.
//     DIALOGUE (dlgtop): redraw its box and re-assert the buffering mode it needs on the next frame after any set-up
//       (HOME/sleep return, settings change, notice ending, dirty display). It must never assume the top kept its
//       picture or its mode across one;
// - the top screen is kept lit (CtrLayout_keepLit(GSPLCD_SCREEN_TOP) answers true) when the lend asked for light;
// - the format stays the consoles' RGB565 (nobody calls gfxSetScreenFormat on the top while it is lent).
// Any move of the game to the top screen ends the lend there (the target is the game's from then on): the battle's
// move up (which takes a FLIGHT lend over, below), the Game screen row, the options menu or a notice borrowing the
// bottom screen. topHolder() is FREE from that set-up on and the holder's returnTop is a no-op, so a holder asks
// topHolder() at each frame's start and draws nothing into the target once it isn't its own (DIALOGUE lends again
// when the game is back below).
// Never a new target: nullptr without the held pair, with the game on top, during a move (autoMove), or while lent.
typedef enum {
    CTR_LAYOUT_TOP_FREE = 0,      // not lent
    CTR_LAYOUT_TOP_FLIGHT = 1,    // battlesplit T1: the battle-entry SOUL flying to its real start on the top screen
    CTR_LAYOUT_TOP_DIALOGUE = 2,  // dlgtop S4: a dialogue box on the top screen with the game below
} CtrLayoutTopHolder;

// Lends the held top target to `holder` (FLIGHT or DIALOGUE), lit now (CtrDimlight_topBacklight) when `light`, or
// returns nullptr. The holder logs one dev line on nullptr and keeps to today's path. Call outside the renderer's
// frame or at a frame's start, never between a frame's draw and its end.
struct C3D_RenderTarget_tag* CtrLayout_lendTop(struct Renderer* renderer, int holder, bool light);

// Gives the lend back (`holder` must be the one holding; anything else, a lend the move already took included, is a
// no-op). Layout unlinks the target if it is still linked, after the GPU is idle. If light was asked and the player's
// Other screen is Off, the next set-up powers the top off through dimlight, as F8's give-back does. The holder restores
// what it changed itself (T1: single buffering and the console, B5/C2(d)).
void CtrLayout_returnTop(struct Renderer* renderer, int holder);

// Who holds the top target now: CTR_LAYOUT_TOP_FREE, _FLIGHT or _DIALOGUE. This is the busy query.
// - T1 asks it at flight start: DIALOGUE means a top dialogue box is showing, so no flight lend (contract (ii)).
// - dlgtop asks it before opening a box: FLIGHT means queue the box or draw it with the game.
// dlgtop holds the lend exactly while a top box is shown: lend at the box's open, return at its close. So DIALOGUE is
// "a top dialogue box is open", and no separate box query exists.
int CtrLayout_topHolder(void);

// True when the battle-entry move up will happen for battle group `group`. All of these must hold: the pair is held,
// the game is below (CtrScreens_gameBelow), no move is in progress, Battle layout is Split or Top screen,
// bsTouchGroup(group) passes, and the top is FREE or lent to FLIGHT. T1 asks it at flight start with
// global.battlegroup (the room isn't the battle room yet) and lends only on true. CtrLayout_moveUp asserts the same
// predicate at the battle room's first frame (all but the group, which battlesplit's caller checks before it), so a
// started flight is followed by the move as long as nothing it tests changes in between. It answers for the state now
// and reserves nothing: a Game screen change to Top, a Battle layout pick or a set-up's refusal before the battle room
// makes moveUp refuse, and the holder keeps its own way out (topHolder FREE after a move it didn't expect, or its own
// returnTop at its end).
bool CtrLayout_canMoveUp(int32_t group);

// Existing (F8), unchanged in shape: ctr_screens.c's power-off wrapper asks it. It is also true for GSPLCD_SCREEN_TOP
// while the top is lent with light.
bool CtrLayout_keepLit(uint32_t screen);

// ===[ The scene table (S3; dsplan B5, E5) ]===
// romfs:/layout/undertale.lst (written by tools/layout/build_scenes.py at build time from the names-only spec
// tools/layout/undertale-scenes.json and the player's data.win). Numbers only: room and object indices, classes and
// rows. Little-endian, 4-byte aligned:
//
//   CtrLayoutTableHeader, sceneCount x CtrLayoutScene (sorted by room, then group), entryCount x CtrLayoutEntry
//
// A scene is a battle room and battle group (an encounter), a room alone (group CTR_LAYOUT_ANY: the Flowey
// tutorial's kind, no battle group), or every room (room and group CTR_LAYOUT_ANY: the name classes every split
// battle starts from). Its entries are its objects' classes (battlesplit's class byte: BS_TOP..BS_COVER, BS_SOUL),
// prefixes expanded at build time, first match first. The file is checked whole at its first use (magic, version,
// the data.win's CtrPackKey, room and object counts, sizes, FNV-1a of the body, every index in range); a file that
// fails any check is dropped whole and every battle is Original (F7): never a partly read table.
#define CTR_LAYOUT_TABLE_MAGIC 0x594C5444u // "DTLY"
#define CTR_LAYOUT_TABLE_VERSION 1u
#define CTR_LAYOUT_TABLE_MAX 65536u
#define CTR_LAYOUT_ANY 0xFFFFu
// CtrLayoutScene.flags.
enum { CTR_LAYOUT_SCENE_SPLIT = 1 }; // admitted: the scene splits (else only its entries, for a bench run's probe)
// CtrLayoutScene.band 0: the standard band; .lift CTR_LAYOUT_LIFT_DEFAULT: the preset's standard lift.
#define CTR_LAYOUT_LIFT_DEFAULT 0xFFu
typedef struct {
    uint32_t magic, version;
    uint32_t dataWinSize, gameId; // CtrPackKey of the data.win the table was resolved on
    uint64_t timestamp;
    uint32_t roomCount, objectCount; // that data.win's
    uint16_t sceneCount, entryCount;
    uint32_t bodySize;               // what follows the header
    uint32_t checksum;               // FNV-1a of the body (RoomPlan_fnv1a)
    uint32_t reserved;               // 0
} CtrLayoutTableHeader;
typedef struct {
    uint16_t room, group;   // CTR_LAYOUT_ANY: every room / no battle group
    uint16_t first, count;  // its entries
    uint8_t band;           // native rows the top screen shows (0: standard)
    uint8_t lift;           // rows preset A moves the bottom up (CTR_LAYOUT_LIFT_DEFAULT: standard)
    uint8_t mode;           // placement mode: 0 standard, 1-4 the close seam (z_battlesplit.c, battlesplit14)
    uint8_t flags;          // CTR_LAYOUT_SCENE_*
} CtrLayoutScene;
typedef struct {
    uint16_t object;
    uint8_t cls;            // battlesplit's class byte
    uint8_t flags;          // 0 (reserved)
} CtrLayoutEntry;
_Static_assert(sizeof(CtrLayoutTableHeader) == 48, "layout table header");
_Static_assert(sizeof(CtrLayoutScene) == 12, "layout scene");
_Static_assert(sizeof(CtrLayoutEntry) == 4, "layout entry");
// The scene for (room, group), room or group -1 for CTR_LAYOUT_ANY; nullptr without one or without a good table
// (loaded at the first call for that data.win). A room's own scene is (room, -1), the every-room row (-1, -1).
struct DataWin;
const CtrLayoutScene* CtrLayout_scene(struct DataWin* dataWin, int32_t room, int32_t group);
// The scene's entries (scene->count of them).
const CtrLayoutEntry* CtrLayout_entries(const CtrLayoutScene* scene);

// ===[ The renderer's side (internal: ctr_renderer.c fills it, z_layout.c reads it) ]===
typedef struct {
    // Offsets into CtrRenderer.
    uint32_t bottomYielded, topTarget, bottomTarget, topW, inFrame, displayDirty, textures, display;
    uint32_t displayFlags;                              // DISPLAY_TRANSFER_FLAGS
    void (*keepBottom)(void* renderer);                 // screensConfigureKeepBottom
    bool (*scanoutMove)(void* renderer, bool onBottom); // scanoutMove (deletes topTarget: z_layout clears it first)
} CtrLayoutRendererLinks;
extern const CtrLayoutRendererLinks Layout_renderer;

#endif
