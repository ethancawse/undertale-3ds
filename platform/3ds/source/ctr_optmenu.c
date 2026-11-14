// The release options menu (optmenu) for UNDERTALE / DELTARUNE 1.0.0-beta.1:
// argui's menu (ctr_argui.c) reworked, with an FPS counter page and a Debug page, on either screen.
//
// All late code (.text.late.optmenu, constants after it, statics in .bss.late.optmenu), in a file of its own so that
// ctr_argui.c compiles as on main (code added to a file reorders the functions GCC emits there). The menu's open,
// update and close, and the settings' load, override, save and keepLit, are wrapped (CMakeLists.txt). argui's own
// painter still draws the first-boot hint and the FPS counter (fps_counter, fps_size), so nothing here runs per frame
// while the menu is closed. The one per-frame addition is in the screens' yield wrapper (ctr_renderer.c): a test of
// CtrOptmenu_onTop, a byte, and no call.
//
// Removed choices: 3D (always off), Top screen (Standard), Drawing (Native), Full smoothing, Frame skip (Off). A saved
// removed value loads as the allowed one and the file is written again; a value given with --ini (a lab test) is
// honoured for that run. Every page is drawn off screen, in the menu screen's spare buffer, and copied to the shown one
// in one go only when it changed: drawing straight into the shown buffer (a clear to black, then slow glyph sampling)
// was the flicker. With the game on the bottom screen the menu is on the top one and the game stays where it is.
//
// The main page in blocks with even gaps (screens; Smoothing, Exact colours, FPS counter; Controls; Debug, Legal
// notices and Credits; Back to the game and Quit game at the bottom), and up and down pass over grey
// rows. UNDERTALE's Smoothing row is Recommended or Custom (left or right switch them, the Custom choices are kept; A on
// Custom opens a page that sets Text, Bones, Numbers and Soul to Smooth or Crisp, with both previews of the row's part
// from the build's menu data, z_smalltext.c); Off is gone from UNDERTALE's menu (a saved Off loads as Smart; --ini
// smoothing=0, or the file's smoothing = 0 in a bench launch, still works for a test run, and the row then shows Off
// until a press turns smoothing back on). DELTARUNE keeps Smart / Off.
// Stats and log (other_screen 2: frame rate, timing, memory and the log on the screen without the game) is the Debug
// page's first row (maintainer 10-26, layout C); the main page's Debug row reads "Log on" while it is on. The setting's
// values and meaning are unchanged (only its row moved), so settings.ini needs no new settings_version.
//
// Opening takes no extra frames: the menu opens on the frame SELECT is
// pressed, page drawn and screen lit in that frame. Before, the open waited for SELECT's release (main.c's
// CtrInputCombo, kept for the L+SELECT and SELECT+START chords), then for the audio hold's fade (menupause, 35 ms of
// sleeps on the game thread) before drawing the page, which sampled the game's font texel by texel through the texture
// cache (13-14 ms on an Old 3DS, and the font's page read from the SD card if it wasn't resident), and the bottom
// screen's backlight (off with Other screen Off) came on only at the next frame's screen set-up. Now:
// - hidKeysUp, which only main's combo calls, reports SELECT as released on the frame it is pressed alone (no L, R, ZL
//   or ZR held then or the frame before, so Rosalina's L+Down+SELECT and the L/R chords never open the menu), and
//   never at the real release, which the combo then never sees (a release after Rosalina's menu opens nothing either).
//   SELECT+START still quits from either order: the combo checks it first, menu open or not.
// - The menu's glyphs (the menu font's, and the heart) are read into a bitmap at rooms' loads (argui's prefetch wrap
//   calls CtrOptmenu_prepareGlyphs) from the tiles resident then, leaving the texture cache as it was: the page is
//   drawn from it, no texture or SD access. A glyph not resident at any load is sampled at the page's first draw.
// - The page is drawn and shown, and the bottom screen lit, before the screens' open (which holds the audio).

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "ctr_wrap_chain.h"
#include "ctr_fpsbox.h"
#ifndef ENABLE_WAD16
#define quitblackKeys optKeysHeard
#define quitblackFlush fpsboxMenuFlush
#endif
#line 22
#include "ctr_display.h"
#include "audio_system.h"
#include "ctr_benchmark.h"
#include "ctr_frame_pacer.h"
#include "ctr_link_files.h"
#include "ctr_log.h"
#include "ctr_menu.h"
#include "ctr_renderer.h"
#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "data_win.h"
#include "log.h"
#include "runner.h"

#if defined(__3DS__) && defined(__GNUC__)
#pragma GCC optimize("Os")
#endif

#if defined(__3DS__) && defined(__GNUC__)
#define OPT_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.late.optmenu")))
#define OPT_STATIC __attribute__((noinline, optimize("Os"), section(".text.late.optmenu"))) static
#define OPT_RO __attribute__((section(".rodata.late.optmenu")))
#define OPT_BSS __attribute__((section(".bss.late.optmenu")))
#else
#define OPT_CODE
#define OPT_STATIC static
#define OPT_RO
#define OPT_BSS
#endif
#define OPT_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] OPT_RO = text

// argui's state (ctr_argui.c, a global): the same layout.
typedef struct {
    uint8_t cfg[3]; // [0] = fps_counter (kept 0 here: argui's painter then draws the hint only), [1] = fps_size, [2] = menu_hint_seen
    bool menuIsOpen;
    uint8_t sx;
    uint8_t sy;
    uint8_t adv;
    bool rgb565;
    char fpsBuf[3];
    uint16_t hintFrames;
    uint16_t fpsFrames;
    int16_t assetIds[5]; // sndMove, sndSelect, fntSmall, fntTheme, sprHeartSmall
    uint32_t fpsVb;
    void* fb;
    CtrTextureCache* texCache;
    AudioSystem* audio;
} ArguiState;

extern ArguiState CtrArgui_state;
#define g CtrArgui_state
#define gFpsScreen g.cfg[0]
#define gFpsSize g.cfg[1]
#define gHintSeen g.cfg[2]

extern uint8_t CtrScreens_game;
extern bool CtrScreens_otherOff;
extern bool CtrScreens_topBacklightOff;
bool CtrScreens_benchLaunched(void);
bool CtrBench_running(void);


extern bool CtrScreens_topBacklightOff;
extern bool CtrScreens_onBottom;
extern bool CtrScreens_sharedBacklight;
extern bool CtrSave_failed;
bool CtrScreens_benchLaunched(void);
bool CtrScreens_gameBelow(void);
bool CtrLegal_shown(void);
void CtrLegal_open(CtrMenu* menu);
bool CtrLegal_update(CtrMenu* menu, u32 down);
void CtrLegalui_open(CtrMenu* menu, bool credits); // the Legal notices and Credits pages (ctr_legal.c, legalui)
void CtrLegalui_update(CtrMenu* menu, u32 down);

// ===[ Strings ]===

OPT_STR(omArguiReadMode, "r");
OPT_STR(omArguiAppendMode, "a");
OPT_STR(omLblGame, "Game screen");
OPT_STR(omLblDialogue, "Dialogue box");
OPT_STR(lblStats, "Stats and log");
OPT_STR(omLblFps, "FPS counter");
#if CTR_PACK_EXACT_AVAILABLE
OPT_STR(omLblSmoothing, "Smoothing");
OPT_STR(omLblExact, "Exact colours");
#else
// GCC emits this pool in first-use order. Keep the removed label's 16 B as
// the preceding string's zero tail, with the original symbol sizes/addresses.
static const char omLblSmoothing[28] __asm__("oldexactSmoothSlot") OPT_RO = "Smoothing";
__asm__(".local omLblSmoothing\n.set omLblSmoothing,oldexactSmoothSlot\n.type omLblSmoothing,%object\n.size omLblSmoothing,12\n"
        ".local omLblExact\n.set omLblExact,oldexactSmoothSlot+12\n.type omLblExact,%object\n.size omLblExact,16");
#endif
OPT_STR(omLblControls, "Controls");
OPT_STR(lblDebug, "Debug");
OPT_STR(omLblPerf, "Performance test");
OPT_STR(omLblLegal, "Legal notices");
OPT_STR(lblCredits, "Credits");
OPT_STR(omLblResume, "Back to the game");
OPT_STR(omLblQuit, "Quit game");
OPT_STR(lblScreen, "Screen");
OPT_STR(lblSize, "Size");
OPT_STR(lblBack, "Back");
OPT_STR(omStrEmpty, "");
OPT_STR(omStrHeart, "\x01");
OPT_STR(omValTop, "< Top >");
OPT_STR(omValBottom, "< Bottom >");
OPT_STR(omValOff, "< Off >");
OPT_STR(omValOn, "< On >");
OPT_STR(omValSmall, "< Small >");
OPT_STR(omValMedium, "< Medium >");
OPT_STR(omValLarge, "< Large >");
#ifndef ENABLE_WAD16
OPT_STR(omValSmart, "< Smart >");
#endif
OPT_STR(omValDefault, "< Default >");
OPT_STR(omValBConfirms, "< B confirms >");
OPT_STR(omValCustom, "< Custom >");
OPT_STR(valDlgTop, "< Default (top) >");
OPT_STR(valDlgBot, "< Bottom screen >");
#if CTR_PACK_EXACT_AVAILABLE
OPT_STR(valOpen, ">");
#else
static const char valOpen[20] __asm__("oldexactOpenSlot") OPT_RO = ">";
__asm__(".local valOpen\n.set valOpen,oldexactOpenSlot\n.type valOpen,%object\n.size valOpen,4\n"
        ".local omDisNew3ds\n.set omDisNew3ds,oldexactOpenSlot+4\n.type omDisNew3ds,%object\n.size omDisNew3ds,16");
#endif
OPT_STR(valLogOpen, "Log on  >");
OPT_STR(valOffOpen, "Off  >");
OPT_STR(valTopOpen, "Top  >");
OPT_STR(valBotOpen, "Bottom  >");
OPT_STR(omDisNeedsTop, "Needs the game on top.");
#if CTR_PACK_EXACT_AVAILABLE
OPT_STR(omDisNew3ds, "New 3DS only.");
#endif
OPT_STR(omHlpGameTop, "Play on the top screen.");
OPT_STR(omHlpGameBot, "Play on the bottom screen.");
OPT_STR(omHlpDlgTop, "Dialogue stays in the game.");
#if defined(ENABLE_WAD16) // popbot: the overworld's popups follow it; the same 40 B, and 38 x 7 px fits the line
OPT_STR(omHlpDlgBot, "Dialogue and menus move to the bottom.");
#else
OPT_STR(omHlpDlgBot, "Dialogue moves to the bottom screen.");
#endif
OPT_STR(omHlpOtherOff, "Other screen off.");
OPT_STR(hlpOtherBlank, "Other screen blank.");
OPT_STR(omHlpOtherLog, "Frame rate and log on the other screen.");
#ifndef ENABLE_WAD16
OPT_STR(omHlpSmoothSmart, "Sharp art, smooth text.");
#endif
OPT_STR(omHlpSmoothOff, "No smoothing.");
#if CTR_PACK_EXACT_AVAILABLE
OPT_STR(omHlpExactOn, "Truer colours, slower. Applies on restart.");
#endif
OPT_STR(hlpFps, "Frame rate overlay. A for its options.");
OPT_STR(omHlpControls, "A to change buttons.");
OPT_STR(hlpDebug, "Tools for testing.");
OPT_STR(hlpLegal, "Licence and source code.");
OPT_STR(hlpCredits, "Who made the game and this port.");
OPT_STR(hlpResume, "Close this menu.");
OPT_STR(omHlpQuit, "Unsaved progress is lost.");
OPT_STR(omHlpFpsOn, "Shows the frame rate.");
OPT_STR(hlpFpsOff, "No frame rate overlay.");
OPT_STR(hlpScreen, "The screen it shows on.");
OPT_STR(hlpSize, "Text size.");
OPT_STR(hlpBack, "Back to the options.");
OPT_STR(omHlpPerf, "Measures speed. Saves aren't touched.");
#ifdef ENABLE_WAD16
OPT_STR(omValRecommended, "< Recommended >");
OPT_STR(hlpSmoothRec, "Our picks for text, bones, numbers, soul.");
OPT_STR(hlpSmoothCustom, "A to choose each part.");
OPT_STR(lblText, "Text");
OPT_STR(lblBones, "Bones");
OPT_STR(lblNumbers, "Numbers");
OPT_STR(lblSoul, "Soul");
OPT_STR(valSmooth, "< Smooth >");
OPT_STR(valCrisp, "< Crisp >");
OPT_STR(capSmooth, "Smooth");
OPT_STR(capCrisp, "Crisp");
OPT_STR(hlpTextSmooth, "The PC text, smoothed.");
OPT_STR(hlpTextCrisp, "Remade pixel text. Recommended.");
// smoothdef (11-06): Recommended moved to Bones Smooth and Numbers Crisp. This block can't grow and its symbols stay at their
// addresses and sizes, so the two lines naming it are late strings; hlpBonesSmooth and hlpNumCrisp stay unread.
static const char hlpBonesSmooth[16] __attribute__((used)) OPT_RO = "Smoothed bones.";
static const char hlpBonesCrisp[28] OPT_RO = "Sharp bones.";
static const char hlpNumSmooth[40] OPT_RO = "Smoothed damage numbers.";
static const char hlpNumCrisp[24] __attribute__((used)) OPT_RO = "Sharp damage numbers.";
static const char hlpBonesSmoothRec[32] __attribute__((section(".text_smoothdef_str"))) = "Smoothed bones. Recommended.";
static const char hlpNumCrispRec[36] __attribute__((section(".text_smoothdef_str"))) = "Sharp damage numbers. Recommended.";
OPT_STR(hlpSoulSmooth, "The PC soul, smoothed. Recommended.");
OPT_STR(hlpSoulCrisp, "Remade pixel soul.");
#endif
OPT_STR(optFmtScreen, " fps_screen = %d");
OPT_STR(optSaveFmt, "fps_screen = %d\n");
OPT_STR(optMigrated, "Display: removed settings in %s loaded as allowed ones, file written again\n");
#ifdef CTRPORT_PLAYER_SHOTS
OPT_STR(optLogCompose, "optmenu: page drawn off screen in %lu us\n");
OPT_STR(optLogShot, "optprobe: shot %u (open %u, closed %u)\n");
OPT_STR(optLogKey, "optprobe: key %lx (open %u)\n");
OPT_STR(optLogProbes, "optprobe: %u steps from %s\n");
OPT_STR(optFmtPath, "%s%s");
OPT_STR(optProbeName, "optprobe.txt");
OPT_STR(optFmtShot, "%soptshot_%02u_%s.bmp");
OPT_STR(optTopName, "top");
OPT_STR(optBotName, "bot");
OPT_STR(optWriteMode, "wb");
OPT_STR(optFmtKey, "open %u key %lx");
OPT_STR(optFmtOpenShot, "open %u shot");
OPT_STR(optFmtCloseShot, "close %u shot");
OPT_STR(optFmtProbeKey, " opt_probe = %d");
OPT_STR(optAutopauseName, "autopause.txt");
OPT_STR(optAutopauseText, "300 3000\n");
OPT_STR(optLogBuiltin, "optprobe: built-in steps (--ini opt_probe=1), menu at frame 300\n");
// One key per press, " = %d" like the other --ini keys, so the lab queue's lint knows them and none is "given twice"
OPT_STR(optFmtPressAt, " opt_press_at = %d");
OPT_STR(optFmtPressAt2, " opt_press_at2 = %d");
OPT_STR(optFmtPressAt3, " opt_press_at3 = %d");
OPT_STR(optFmtLegacy, " opt_legacy_open = %d");
OPT_STR(optLogPress, "optprobe: SELECT %s at frame %u%s\n");
OPT_STR(optLogDown, "down");
OPT_STR(optLogUp, "up");
OPT_STR(optLogLegacy, " (as before: open on release, font sampled, no early backlight)");
OPT_STR(optLogOpened, "optmenu: open at frame %u: page shown after %lu us (placed %d), opened in %lu us\n");
OPT_STR(optLogGlyphs, "optmenu: %u glyphs read from resident tiles (%u of 96) in %lu us\n");
#endif

static const uint8_t omArguiMorton[8] OPT_RO = { 0, 1, 4, 5, 16, 17, 20, 21 };
static const uint8_t omArguiDefKeys[10] OPT_RO = { 3, 3, 0, 0, 0, 0, 4, 11, 11, 0 };

#ifdef ENABLE_WAD16
enum { OPT_PAGE_MAIN, OPT_PAGE_FPS, OPT_PAGE_DEBUG, OPT_PAGE_SMOOTH, OPT_PAGES };
#else
enum { OPT_PAGE_MAIN, OPT_PAGE_FPS, OPT_PAGE_DEBUG, OPT_PAGES };
#endif
enum { OPT_MAIN_GAME, OPT_MAIN_DIALOGUE, OPT_MAIN_SMOOTHING,
#if CTR_PACK_EXACT_AVAILABLE
       OPT_MAIN_EXACT,
#endif
       OPT_MAIN_FPS,
       OPT_MAIN_CONTROLS, OPT_MAIN_DEBUG, OPT_MAIN_LEGAL, OPT_MAIN_CREDITS, OPT_MAIN_RESUME, OPT_MAIN_QUIT,
       OPT_MAIN_COUNT };
enum { OPT_FPS_ON, OPT_FPS_SCREEN, OPT_FPS_SIZE, OPT_FPS_BACK, OPT_FPS_COUNT };
#ifdef CTRPORT_PUBLIC_MENU
// A public build (CMakeLists: CTRPORT_RELEASE_SOURCE_URL, every builder build) has no Performance test (maintainer 11-04):
// its Debug page is Stats and log, then Back where the test was. The test's index is past the page's rows, so no row
// reaches it, and the code keeps its shape (only compared constants change).
enum { OPT_DEBUG_LOG, OPT_DEBUG_BACK, OPT_DEBUG_COUNT, OPT_DEBUG_PERF };
#else
enum { OPT_DEBUG_LOG, OPT_DEBUG_PERF, OPT_DEBUG_BACK, OPT_DEBUG_COUNT };
#endif
#ifdef ENABLE_WAD16
enum { OPT_SMOOTH_TEXT, OPT_SMOOTH_BONES, OPT_SMOOTH_NUMBERS, OPT_SMOOTH_SOUL, OPT_SMOOTH_BACK, OPT_SMOOTH_COUNT };
#endif
#define OPT_SCREENS_CONTROLS 7 // ctr_screens.c's row number for Controls (argui's arguiToScreens)
#define OPT_PAGE_BYTES (320u * 240u * 2u)
#define OPT_TOP_SIDE (40u * 240u) // the top screen's left side bar, in pixels

// Each row's y, per page (logical blocks, even gaps). Rows 14 px apart (the font's 12 px of ink, descenders included,
// and 2 px clear, so no row's ink touches the next's), blocks 6 px further; the main page's Back to the game and Quit
// game sit at the bottom, Quit's lowest ink 6 px above the help line (y 214) as the first row's top is 6 px below the
// frame. Debug: Stats and log (a setting), Performance test (a tool), Back.
// Keep the data slot's width on Old; the unused final column is never drawn.
#if CTR_PACK_EXACT_AVAILABLE
#define OPT_MAIN_Y { 8, 22, 42, 56, 70, 90, 110, 124, 138, 182, 196 }
#else
#define OPT_MAIN_Y { 8, 22, 42, 56, 76, 96, 110, 124, 182, 196 }
#endif
#ifdef ENABLE_WAD16
static const uint8_t optY[OPT_PAGES][11] OPT_RO = {
    OPT_MAIN_Y, { 8, 28, 42, 62 }, { 8, 28, 48 }, { 8, 22, 36, 50, 70 } };
static const uint8_t optCounts[OPT_PAGES] OPT_RO = { OPT_MAIN_COUNT, OPT_FPS_COUNT, OPT_DEBUG_COUNT, OPT_SMOOTH_COUNT };
// The main page's row each page goes back to.
static const uint8_t optParent[OPT_PAGES] OPT_RO = { 0, OPT_MAIN_FPS, OPT_MAIN_DEBUG, OPT_MAIN_SMOOTHING };
#else
static const uint8_t optY[OPT_PAGES][11] OPT_RO = {
    OPT_MAIN_Y, { 8, 28, 42, 62 }, { 8, 28, 48 } };
static const uint8_t optCounts[OPT_PAGES] OPT_RO = { OPT_MAIN_COUNT, OPT_FPS_COUNT, OPT_DEBUG_COUNT };
// The main page's row each page goes back to.
static const uint8_t optParent[OPT_PAGES] OPT_RO = { 0, OPT_MAIN_FPS, OPT_MAIN_DEBUG };
#endif
static const char* const optSizeVals[3] OPT_RO = { omValSmall, omValMedium, omValLarge };

#ifdef CTRPORT_PLAYER_SHOTS
typedef struct {
    uint16_t tick;
    uint8_t kind; // 1 key while open, 2 shot while open, 3 shot after the menu closed
    uint8_t done;
    uint32_t arg;
} OptProbe;
#endif

typedef struct {
    CtrMenu* menu;
    uint16_t* spareBase; // the page as drawn, off screen: the menu console's buffer while the menu is open
    uint16_t* shownBase;
    void* restore;       // the console's buffer to give back at close
    uint8_t page;
    uint8_t cur[OPT_PAGES];
    uint8_t scr;         // the screen the menu is placed on
    uint8_t pref;        // fps_screen: the counter's screen, kept while it is Off (0 top, 1 bottom)
    bool placed;
    bool dirty;
#ifdef ENABLE_WAD16
    uint8_t part;        // the Smoothing page's part last shown (its preview stays on the Back row)
#endif
#ifdef CTRPORT_PLAYER_SHOTS
    uint16_t sinceOpen;
    uint16_t sinceClose;
    uint8_t probes;
    uint8_t shots;
    bool wroteAutopause;
    uint32_t pendingKeys;
    char dir[64];
    OptProbe probe[48];
    uint16_t frame;      // hidKeysUp's calls: main's frames
    uint16_t pressAt[4]; // opt_press_at: frames at which SELECT is pressed (and released 4 frames later)
    uint8_t presses;
    bool legacy;         // opt_legacy_open: open as before, for the comparison
#endif
    uint32_t keysHeld;   // HID keys held at hidKeysUp's last call (with the frame's presses)
    bool selectBlocked;  // SELECT held with L, R, ZL or ZR: no open until it is released
} OptState;

static OptState opt OPT_BSS;

// The menu on the top screen while it is open with the game on the bottom one: read once a frame by the screens' yield
// wrapper (ctr_renderer.c), which then keeps the bottom screen for the game and Other screen Off from blanking the top.
uint8_t CtrOptmenu_onTop OPT_BSS;

// The menu on the top screen: open, with the game on the bottom screen as game_screen says, and no failed save
// (whose message main prints on the bottom screen, the game then moving up as before). Set at each menu call, before
// the frame's yield; the top screen is lit when the menu takes it.
OPT_STATIC void optLive(void) {
    bool top = g.menuIsOpen && CtrScreens_game == 1 && CtrScreens_gameBelow() && !__atomic_load_n(&CtrSave_failed, __ATOMIC_ACQUIRE);
    if (top && !CtrOptmenu_onTop && CtrScreens_topBacklightOff && R_SUCCEEDED(CtrDimlight_topOnInit())) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP);
        gspLcdExit();
        CtrScreens_topBacklightOff = false;
    }
    CtrOptmenu_onTop = top;
}

// ===[ Settings ]===

OPT_STATIC bool optParse(const char* line) {
    int value;
    if (sscanf(line, optFmtScreen, &value) != 1 || (uint32_t) value > 1u) return false;
    opt.pref = (uint8_t) value;
    return true;
}

// The choices the menu no longer offers, as the nearest one it does: 3D off, Standard, Native, Smart for Full
// smoothing, no frame skip. True if anything changed.
OPT_STATIC bool optClamp(CtrDisplaySettings* s) {
    CtrDisplaySettings before = *s;
    s->topMode = TOP_MODE_STANDARD;
    s->stereoMode = STEREO_NORMAL;
    s->drawing = DRAWING_NATIVE;
    s->frameSkip = FRAME_SKIP_OFF;
    if (s->smoothing != SMOOTHING_OFF) s->smoothing = SMOOTHING_SMART;
#ifdef ENABLE_WAD16
    // UNDERTALE's Smoothing is Recommended or Custom (z_smalltext.c), over Smart; a saved Off stays only for a test
    // (a bench launch: Azahar runs and console rows set smoothing in the file).
    if (!CtrScreens_benchLaunched()) s->smoothing = SMOOTHING_SMART;
#endif
    return memcmp(&before, s, sizeof(before)) != 0;
}

#if defined(ENABLE_WAD17) && !defined(CTRPORT_TARGET_N3DS) // DELTARUNE New: z_smalltext_dr.c's links, then crispart's
#define CtrArgui_chainOverrideSmalltext CtrArgui_chainOverrideCrispart
#define CtrArgui_chainSaveEditsSmalltext CtrArgui_chainSaveEditsCrispart
#endif
#line 271
void __real_CtrArgui_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool CtrArgui_chainOverrideSmalltext(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps); // ctr_wrap_chain.h
void CtrArgui_chainSaveEditsSmalltext(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps); // ctr_wrap_chain.h
void __real_CtrArgui_menuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
CtrMenuAction __real_CtrArgui_menuUpdate(CtrMenu* menu, u32 down);
void __real_CtrArgui_menuClose(CtrMenu* menu);
bool __real_CtrArgui_keepLit(u32 screen);
void __wrap_CtrArgui_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool __wrap_CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __wrap_CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
void __wrap_CtrArgui_menuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
CtrMenuAction __wrap_CtrArgui_menuUpdate(CtrMenu* menu, u32 down);
void __wrap_CtrArgui_menuClose(CtrMenu* menu);
bool __wrap_CtrArgui_keepLit(u32 screen);
#ifdef CTRPORT_PLAYER_SHOTS
OPT_STATIC void optProbeLoad(const char* path);
OPT_STATIC bool optProbeBuiltin(const char* line);
OPT_STATIC bool optPressParse(const char* line);
#endif
#ifdef ENABLE_WAD16
// UNDERTALE's Smoothing choices (z_smalltext.c): bit 7 Custom, bits 0..3 Text, Bones, Numbers, Soul Crisp.
uint32_t CtrSmooth_get(void);
void CtrSmooth_set(uint32_t v);
void CtrSmooth_menuOpen(void);
void CtrSmooth_menuClose(void);
const uint16_t* CtrSmooth_preview(uint32_t part, uint32_t crisp, uint32_t* w, uint32_t* h);
static const char* const optPartLabel[4] OPT_RO = { lblText, lblBones, lblNumbers, lblSoul };
static const char* const optPartHelp[4][2] OPT_RO = {
    { hlpTextSmooth, hlpTextCrisp }, { hlpBonesSmoothRec, hlpBonesCrisp }, { hlpNumSmooth, hlpNumCrispRec }, { hlpSoulSmooth, hlpSoulCrisp } };
#endif

// argui's save (the display's settings, game_screen, fps_counter, fps_size, menu_hint_seen), then fps_screen.
OPT_CODE void __wrap_CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    CtrArgui_chainSaveEditsSmalltext(before, after, path, caps);
    FILE* f = fopen(path, omArguiAppendMode);
    if (f != nullptr) {
        fprintf(f, optSaveFmt, (int) opt.pref);
        fclose(f);
    }
}

// The first-boot hint's and the counter's screen stays lit (argui), and the top screen while the menu is there.
OPT_CODE bool __wrap_CtrArgui_keepLit(u32 screen) {
    return __real_CtrArgui_keepLit(screen) | (CtrOptmenu_onTop && screen == GSPLCD_SCREEN_TOP);
}

OPT_CODE void __wrap_CtrArgui_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    // argui's menu entry points stay linked where they were (--gc-sections would drop them and move what follows).
    WAD17_ONLY(__asm__ volatile("" : : "r"(__real_CtrArgui_menuOpen), "r"(__real_CtrArgui_menuUpdate), "r"(__real_CtrArgui_menuClose));)
    __real_CtrArgui_load(settings, path, caps);
    opt.pref = 0;
    FILE* f = fopen(path, omArguiReadMode);
    bool exists = f != nullptr;
    if (exists) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) {
            optParse(line);
#ifdef CTRPORT_PLAYER_SHOTS
            optPressParse(line);
#endif
        }
        fclose(f);
    }
    if (gFpsScreen != 0) opt.pref = (uint8_t) (gFpsScreen - 1u);
    CtrDisplaySettings before = *settings;
    if (optClamp(settings) && exists && !CtrScreens_benchLaunched()) {
        __wrap_CtrArgui_saveEdits(&before, settings, path, caps);
        logInfo(optMigrated, path);
    }
#ifdef CTRPORT_PLAYER_SHOTS
    optProbeLoad(path);
#endif
}

// A line given with --ini: fps_screen here, the rest as argui and the display read them (removed values included,
// for that run only).
OPT_CODE bool __wrap_CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
#ifdef CTRPORT_PLAYER_SHOTS
    if (optProbeBuiltin(line) || optPressParse(line)) return true;
#endif
    if (optParse(line)) return true;
    bool done = CtrArgui_chainOverrideSmalltext(settings, line, caps);
    if (gFpsScreen != 0) opt.pref = (uint8_t) (gFpsScreen - 1u);
    return done;
}

// ===[ Drawing ]===

// arguiSampleGlyph's body (inlined there, so a copy): the menu font (theme) or the overlay's (fnt_small). Returns the
// texel's bit (0 or 1), or -1 if its tile isn't resident: a peek then gives up, a sample loads it through the texture
// cache. A peek reads resident tiles only and changes nothing in the cache (no page or tile use, load, room record or
// packed copy), so a room's load takes the glyphs without moving the game's memory (sampling
// through the cache at each data.win's first room load kept the font page's tiles and packed copy, and DELTARUNE Old's
// ch4_titan heap peak went 34.0 -> 37.0 MB).
OPT_STATIC int32_t optTexel(uint8_t ch, int32_t gx, int32_t gy, uint32_t theme, bool peek) {
    CtrTextureCache* cache = g.texCache;
    const DataWin* dw = cache->dataWin;
    int32_t tpagIndex = -1;
    uint32_t lx = (uint32_t) gx, ly = (uint32_t) gy;
    int32_t fid = g.assetIds[2u + theme];
    if (ch == 1u) {
        if (ly < 9u && g.assetIds[4] >= 0) tpagIndex = dw->sprt.sprites[g.assetIds[4]].tpagIndices[0];
        ch = '>';
    } else if (fid >= 0 && dw->font.fonts[fid].glyphLUT[ch] != nullptr) {
        const Font* f = &dw->font.fonts[fid];
        const FontGlyph* gl = f->glyphLUT[ch];
        lx = (uint32_t) (gx - gl->offset);
        ly = (uint32_t) (gy + (int32_t) (theme << 2));
        if (lx >= gl->sourceWidth || ly >= gl->sourceHeight) return 0;
        tpagIndex = f->tpagIndex;
        lx += gl->sourceX;
        ly += gl->sourceY;
    }
    if (tpagIndex < 0) {
        return (uint32_t) (gx | gy) < 8u && (consoleGetDefault()->font.gfx[(uint32_t) ch * 8u + (uint32_t) gy] & (0x80u >> gx)) != 0;
    }
    const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    uint32_t pageId = (uint32_t) tpag->texturePageId;
    CtrPage* page = !peek ? CtrTextures_acquire(cache, pageId)
                    : pageId < cache->pageCount && cache->pages[pageId].layoutKnown ? &cache->pages[pageId] : nullptr;
    if (page == nullptr) return -1;
    uint32_t px = tpag->sourceX + lx, py = tpag->sourceY + ly;
    int32_t tx = (int32_t) (px >> 8), ty = (int32_t) (py >> 8);
    const CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
    if (!tile->valid && !peek) CtrTextures_ensureTiles(cache, page, tx, ty, tx, ty);
    // A fully transparent tile is never loaded: its texels are 0.
    if (!tile->valid) return page->formatsKnown && tile->format == CTR_TILE_EMPTY ? 0 : -1;
    uint32_t idx = ((py & 0xF8u) << 8) | ((px & 0xF8u) << 3) |
                   omArguiMorton[px & 7u] | ((uint32_t) omArguiMorton[py & 7u] << 1);
    return ((const uint16_t*) tile->tex.data)[idx << (tile->tex.fmt == GPU_RGBA8)] & 1u;
}

// The menu font's glyphs (ch 1, the heart, and 32 to 126 in slots 0 to 95) as optTexel has them, a row of 9 bits per
// line, for the data.win and font optGlyphsFor names; bit 15 of a slot's first row (OPT_GLYPH_SET) once all its texels
// were read. Peeked at each room's load (CtrOptmenu_prepareGlyphs, from argui's prefetch wrap), so the tiles the last
// room drew give them with no texture or SD access; a glyph that wasn't resident then is sampled at the page's first
// draw. (No new variables: one would move CtrOptmenu_onTop in the object's bss, and the code that reads it.)
static uint16_t optGlyphs[96][12] OPT_BSS;
static const DataWin* optGlyphsDw OPT_BSS;
static int32_t optGlyphsFont OPT_BSS;
#define OPT_GLYPH_SET 0x8000u

// Whether there is a font to take glyphs from; the table is emptied when its data.win or font isn't the current one.
// The data.win's texture item count is mixed into the font's key, so a chapter's data.win parsed where the last one was
// doesn't keep that one's glyphs.
OPT_STATIC bool optGlyphsFor(void) {
    CtrTextureCache* cache = g.texCache;
    if (cache == nullptr || cache->dataWin == nullptr) return false;
    int32_t font = (g.assetIds[3] * 65536 + g.assetIds[4]) ^ (int32_t) (cache->dataWin->tpag.count * 0x9E3779B1u);
    if (optGlyphsDw != cache->dataWin || optGlyphsFont != font) {
        memset(optGlyphs, 0, sizeof(optGlyphs));
        optGlyphsDw = cache->dataWin;
        optGlyphsFont = font;
    }
    return true;
}

// Reads a slot's glyph into the table unless it is there (a peek stops at the first texel not resident).
OPT_STATIC void optGlyph(uint32_t slot, bool peek) {
    if ((optGlyphs[slot][0] & OPT_GLYPH_SET) != 0) return;
    uint8_t ch = slot == 0 ? 1u : (uint8_t) (slot + 31u);
    bool whole = true;
    for (int32_t gy = 0; 12 > gy; gy++) {
        uint32_t bits = 0;
        for (int32_t gx = 0; 9 > gx; gx++) {
            int32_t t = optTexel(ch, gx, gy, 1u, peek);
            if (t < 0 && peek) return;
            whole = whole && t >= 0;
            bits |= (uint32_t) (t > 0) << gx;
        }
        optGlyphs[slot][gy] = (uint16_t) bits;
    }
    if (whole) optGlyphs[slot][0] |= OPT_GLYPH_SET;
}

#ifdef CTRPORT_PLAYER_SHOTS
OPT_STATIC uint32_t optGlyphsHeld(void) {
    uint32_t n = 0;
    for (uint32_t slot = 0; 96u > slot; slot++) n += optGlyphs[slot][0] >> 15;
    return n;
}
#endif

void CtrOptmenu_prepareGlyphs(void);
OPT_CODE void CtrOptmenu_prepareGlyphs(void) {
    if (!optGlyphsFor()) return;
#ifdef CTRPORT_PLAYER_SHOTS
    uint64_t t0 = svcGetSystemTick();
    uint32_t before = optGlyphsHeld();
#endif
    for (uint32_t slot = 0; 96u > slot; slot++) optGlyph(slot, true);
#ifdef CTRPORT_PLAYER_SHOTS
    uint32_t after = optGlyphsHeld();
    if (after != before) logInfo(optLogGlyphs, (unsigned) (after - before), (unsigned) after, (unsigned long) ((svcGetSystemTick() - t0) / 268u));
#endif
}

// Menu text into a 320 x 240 RGB565 page (column-major, as the screens), the glyphs' set pixels only.
OPT_STATIC void optText(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col) {
    bool table = optGlyphsFor();
#ifdef CTRPORT_PLAYER_SHOTS
    if (opt.legacy) table = false;
#endif
    for (; *s != '\0'; s++, x += 7) {
        uint32_t ch = (uint8_t) *s;
        uint32_t slot = ch == 1u ? 0u : ch - 31u;
        if (table && (ch == 1u || ch - 32u < 95u)) {
            optGlyph(slot, false);
            for (int32_t gy = 0; 12 > gy; gy++) {
                uint16_t* col0 = fb + x * 240 + (239 - y - gy);
                for (uint32_t bits = optGlyphs[slot][gy] & 0x1FFu, gx = 0; bits != 0; bits >>= 1, gx++) {
                    if (bits & 1u) col0[gx * 240u] = (uint16_t) col;
                }
            }
            continue;
        }
        for (int32_t gy = 0; 12 > gy; gy++) {
            for (int32_t gx = 0; 9 > gx; gx++) {
                if (optTexel((uint8_t) *s, gx, gy, 1u, false) > 0) fb[(x + gx) * 240 + (239 - y - gy)] = (uint16_t) col;
            }
        }
    }
}

typedef struct {
    const char* label;
    const char* value;
    const char* help;
    bool dis;
} OptRow;

OPT_STATIC void optRowMain(const CtrMenu* menu, uint32_t i, OptRow* r) {
#if defined(__3DS__) && !CTR_PACK_EXACT_AVAILABLE
    // Retain this cold menu function's footprint: removing the row saves 40 B.
    __asm__ volatile(".rept 10\n nop\n .endr");
#endif
    const CtrDisplaySettings* s = &menu->settings;
    bool top = CtrScreens_game == 0;
    if (i == OPT_MAIN_GAME) {
        r->label = omLblGame;
        r->value = top ? omValTop : omValBottom;
        r->help = top ? omHlpGameTop : omHlpGameBot;
    } else if (i == OPT_MAIN_DIALOGUE) {
        r->label = omLblDialogue;
        r->dis = !top;
        r->value = r->dis ? omDisNeedsTop : (s->dialogueBottom ? valDlgBot : valDlgTop);
        r->help = r->dis ? omDisNeedsTop : (s->dialogueBottom ? omHlpDlgBot : omHlpDlgTop);
    } else if (i == OPT_MAIN_SMOOTHING) {
        r->label = omLblSmoothing;
#ifdef ENABLE_WAD16
        // Off only from a test (--ini smoothing=0, or a bench launch's file; optClamp): shown as it is, until changed.
        bool custom = (CtrSmooth_get() & 0x80u) != 0, off = s->smoothing == SMOOTHING_OFF;
        r->value = off ? omValOff : (custom ? omValCustom : omValRecommended);
        r->help = off ? omHlpSmoothOff : (custom ? hlpSmoothCustom : hlpSmoothRec);
#else
        bool off = s->smoothing == SMOOTHING_OFF;
        r->value = off ? omValOff : omValSmart;
        r->help = off ? omHlpSmoothOff : omHlpSmoothSmart;
#endif
#if CTR_PACK_EXACT_AVAILABLE
    } else if (i == OPT_MAIN_EXACT) {
        r->label = omLblExact;
        r->dis = !CTR_PACK_EXACT_AVAILABLE;
        r->value = r->dis ? omDisNew3ds : (s->exactColours ? omValOn : omValOff);
        r->help = r->dis ? omDisNew3ds : (s->exactColours ? omHlpExactOn : omStrEmpty);
#endif
    } else if (i == OPT_MAIN_FPS) {
        r->label = omLblFps;
        r->value = gFpsScreen == 0 ? valOffOpen : (gFpsScreen == 1 ? valTopOpen : valBotOpen);
        r->help = hlpFps;
    } else if (i == OPT_MAIN_CONTROLS) {
        const uint8_t* k = s->controls.keys;
        uint32_t layout = ((k[0] * k[1] == 2u) & (memcmp(k + 2, omArguiDefKeys, 10) == 0)) ? (uint32_t) (k[0] - 1u) : 2u;
        r->label = omLblControls;
        r->value = layout == 0 ? omValDefault : (layout == 1 ? omValBConfirms : omValCustom);
        r->help = omHlpControls;
    } else if (i == OPT_MAIN_DEBUG) {
        r->label = lblDebug;
        r->value = s->bottomMode == BOTTOM_MODE_CONSOLE ? valLogOpen : valOpen;
        r->help = hlpDebug;
    } else if (i == OPT_MAIN_LEGAL) {
        r->label = omLblLegal;
        r->help = hlpLegal;
    } else if (i == OPT_MAIN_CREDITS) {
        r->label = lblCredits;
        r->help = hlpCredits;
    } else if (i == OPT_MAIN_RESUME) {
        r->label = omLblResume;
        r->help = hlpResume;
    } else {
        r->label = omLblQuit;
        r->help = omHlpQuit;
    }
}

// The FPS counter's page: every row but the first and Back grey while the counter is Off (the cursor passes over them,
// so they show the counter's settings for when it is on and need no help line).
OPT_STATIC void optRowFps(uint32_t i, OptRow* r) {
    bool on = gFpsScreen != 0;
    r->dis = !on && i != OPT_FPS_ON && i != OPT_FPS_BACK;
    if (i == OPT_FPS_ON) {
        r->label = omLblFps;
        r->value = on ? omValOn : omValOff;
        r->help = on ? omHlpFpsOn : hlpFpsOff;
    } else if (i == OPT_FPS_SCREEN) {
        r->label = lblScreen;
        r->value = opt.pref ? omValBottom : omValTop;
        r->help = hlpScreen;
    } else if (i == OPT_FPS_SIZE) {
        r->label = lblSize;
        r->value = optSizeVals[gFpsSize < 3u ? gFpsSize : 0u];
        r->help = hlpSize;
    } else {
        r->label = lblBack;
        r->help = hlpBack;
    }
}

// The Debug page: Stats and log On or Off (Off: the other screen's backlight off, or blank on the original 2DS).
OPT_STATIC void optRowDebug(const CtrMenu* menu, uint32_t i, OptRow* r) {
    if (i == OPT_DEBUG_LOG) {
        bool console = menu->settings.bottomMode == BOTTOM_MODE_CONSOLE;
        r->label = lblStats;
        r->value = console ? omValOn : omValOff;
        r->help = console ? omHlpOtherLog : (CtrScreens_sharedBacklight ? hlpOtherBlank : omHlpOtherOff);
        return;
    }
    r->label = i == OPT_DEBUG_PERF ? omLblPerf : lblBack;
    r->help = i == OPT_DEBUG_PERF ? omHlpPerf : hlpBack;
}

#ifdef ENABLE_WAD16
// UNDERTALE's Smoothing page: each part Smooth or Crisp (the Custom choices), then Back.
OPT_STATIC void optRowSmooth(uint32_t i, OptRow* r) {
    if (i == OPT_SMOOTH_BACK) {
        r->label = lblBack;
        r->help = hlpBack;
        return;
    }
    uint32_t crisp = CtrSmooth_get() >> i & 1u;
    r->label = optPartLabel[i];
    r->value = crisp ? valCrisp : valSmooth;
    r->help = optPartHelp[i][crisp];
}

// A preview at (x0, y0), k screen pixels per pixel (RGB565 rows, top first).
OPT_STATIC void optBlit(uint16_t* fb, const uint16_t* img, uint32_t w, uint32_t h, int32_t x0, int32_t y0, uint32_t k) {
    for (uint32_t y = 0; h * k > y; y++) {
        for (uint32_t x = 0; w * k > x; x++) fb[(x0 + (int32_t) x) * 240 + (239 - y0 - (int32_t) y)] = img[(y / k) * w + x / k];
    }
}

// The part's previews in a box under the rows: Smooth on the left, Crisp on the right, each at native size (as the
// battle shows it) and, when it is small, again magnified below; the caption of the current choice yellow.
OPT_STATIC void optPreview(uint16_t* fb, uint32_t part) {
    for (int32_t x = 11; 308 >= x; x++) fb[x * 240 + (239 - 84)] = fb[x * 240 + (239 - 207)] = 0xFFFFu;
    for (int32_t y = 84; 207 >= y; y++) fb[11 * 240 + (239 - y)] = fb[308 * 240 + (239 - y)] = 0xFFFFu;
    for (int32_t y = 90; 201 >= y; y++) fb[160 * 240 + (239 - y)] = 0x8410u;
    uint32_t crispNow = CtrSmooth_get() >> part & 1u;
    for (uint32_t c = 0; 2u > c; c++) {
        int32_t cx = c != 0 ? 234 : 85;
        uint32_t w = 0, h = 0;
        const uint16_t* img = CtrSmooth_preview(part, c, &w, &h);
        if (img != nullptr && w != 0 && 140u >= w && 99u >= h) {
            uint32_t k = 1;
            while (4u > k && 140u >= w * (k + 1u) && 99u >= h + 6u + h * (k + 1u)) k++;
            uint32_t total = k > 1u ? h + 6u + k * h : h;
            int32_t top = 89 + (int32_t) (99u - total) / 2;
            optBlit(fb, img, w, h, cx - (int32_t) w / 2, top, 1u);
            if (k > 1u) optBlit(fb, img, w, h, cx - (int32_t) (w * k) / 2, top + (int32_t) h + 6, k);
        }
        const char* cap = c != 0 ? capCrisp : capSmooth;
        optText(fb, cx - (int32_t) strlen(cap) * 7 / 2, 192, cap, c == crispNow ? 0xFFE0u : 0x8410u);
    }
}
#endif

// Row i of the page.
static void optRowMainHeard(const CtrMenu* menu, uint32_t i, OptRow* r); // optRowMain, then the appended text (below)
static void optRowMainShown(const CtrMenu* menu, uint32_t i, OptRow* r); // then this polish's value (below)
OPT_STATIC void optRow(const CtrMenu* menu, uint32_t page, uint32_t i, OptRow* r) {
    r->label = r->value = r->help = omStrEmpty;
    r->dis = false;
    if (page == OPT_PAGE_MAIN) optRowMainShown(menu, i, r);
    else if (page == OPT_PAGE_FPS) optRowFps(i, r);
#ifdef ENABLE_WAD16
    else if (page == OPT_PAGE_SMOOTH) optRowSmooth(i, r);
#endif
    else optRowDebug(menu, i, r);
}

// The page, drawn off screen: argui's frame (border, the line above the help) and rows.
OPT_STATIC void optCompose(const CtrMenu* menu, uint16_t* fb) {
#ifdef ENABLE_WAD16
    const uint32_t border = 0xFFFFu;
#else
    const uint32_t border = 0x9D18u;
#endif
    uint16_t* p = fb;
    for (uint32_t x = 0; 320u > x; x++) {
        for (uint32_t y = 0; 240u > y; y++) *p++ = (uint16_t) ((x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? border : 0u);
    }
    uint32_t page = opt.page, count = optCounts[page], sel = opt.cur[page];
    for (uint32_t i = 0; count > i; i++) {
        OptRow r;
        optRow(menu, page, i, &r);
        int32_t y = optY[page][i];
        uint32_t col = r.dis ? 0x8410u : (i == sel ? 0xFFE0u : 0xFFFFu);
        optText(fb, 24, y, r.label, col);
        optText(fb, 148, y, r.value, col);
        if (i == sel) {
            optText(fb, 11, y, omStrHeart, 0xF800u);
            optText(fb, 12, 221, r.help, 0xFFFFu);
        }
    }
#ifdef ENABLE_WAD16
    if (page == OPT_PAGE_SMOOTH) {
        if (OPT_SMOOTH_BACK > sel) opt.part = (uint8_t) sel;
        optPreview(fb, opt.part);
    }
#endif
}

// The menu's buffers on the screen without the game: the page is drawn in that screen's spare buffer (the menu
// console's while the menu is open, so the Controls and legal pages print there too) and copied to the shown one,
// in the middle 320 columns of the top screen. Done again whenever the game has moved (the renderer's move points the
// console at its own buffer, which is then what the console gets back at close).
#if defined(ENABLE_WAD16)
u8* CtrBlank_settledShown(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height); // ctr_gpu_queue.c, dlgblank
#define OPT_SHOWN CtrBlank_settledShown
#else
#define OPT_SHOWN CtrPacer_shownFramebuffer
#endif
OPT_STATIC void optPlace(CtrMenu* menu) {
    gfxScreen_t s = CtrScreens_onBottom ? GFX_TOP : GFX_BOTTOM;
    opt.placed = false;
    if (gfxGetScreenFormat(s) != GSP_RGB565_OES) return;
    u16 w = 0, h = 0;
    uint16_t* shown = (uint16_t*) OPT_SHOWN(s, GFX_LEFT, &w, &h);
    uint16_t* spare = (uint16_t*) CtrPacer_backFramebuffer(s, GFX_LEFT);
    gfxSetDoubleBuffering(s, false); // the consoles' single buffer, as the renderer's move leaves it
    if (shown == nullptr || spare == nullptr || shown == spare || w != 240u) return;
    uint32_t side = s == GFX_TOP ? OPT_TOP_SIDE : 0u;
    if (menu->console->frameBuffer != opt.spareBase) opt.restore = menu->console->frameBuffer;
    opt.spareBase = spare + side;
    opt.shownBase = shown + side;
    menu->console->frameBuffer = opt.spareBase;
    if (side != 0u) {
        CtrLog_beginConsole();
        memset(shown, 0, (size_t) w * h * 2u);
        GSPGPU_FlushDataCache(shown, (u32) w * h * 2u);
        CtrLog_endConsole();
    }
    opt.scr = (uint8_t) s;
    opt.placed = true;
    opt.dirty = true;
}

// At each menu call (once a frame while it is open, after the frame's start and before its yield): the page drawn and
// shown if it changed, placed again first if the game has moved since. The Controls and legal pages print through the
// console, and are copied every frame.
#if defined(ENABLE_WAD16) // menufirst (UNDERTALE): composed off the LCD, then copied at the screen's vblank (file's end)
static void optComposeShown(const CtrMenu* menu, uint16_t* fb);
static void* optCopyShown(void* to, const void* from, size_t bytes);
static void optFlushFrame(CtrMenu* menu);
#define OPT_COMPOSE_PAGE optComposeShown
#define OPT_FLUSH_FRAME optFlushFrame
#define OPT_COPY_SHOWN optCopyShown
#else
#define OPT_COMPOSE_PAGE optCompose
#define OPT_COPY_SHOWN memcpy
#define OPT_FLUSH_FRAME optFlush
#endif
#line 879 // menufirst: the numbering before it (the __LINE__ trap)
OPT_STATIC void optFlush(CtrMenu* menu) {
    if (!menu->open) return;
    if (!opt.placed || CtrScreens_onBottom != (opt.scr == GFX_TOP) || menu->console->frameBuffer != opt.spareBase) {
        optPlace(menu);
    }
    if (!opt.placed) return;
    bool consolePage = menu->page != MENU_PAGE_OPTIONS || CtrLegal_shown();
    if (!opt.dirty && !consolePage) return;
    CtrLog_beginConsole();
    if (!consolePage) {
#ifdef CTRPORT_PLAYER_SHOTS
        uint64_t t0 = svcGetSystemTick();
        OPT_COMPOSE_PAGE(menu, opt.spareBase);
        logInfo(optLogCompose, (unsigned long) ((svcGetSystemTick() - t0) / 268u));
#else
        OPT_COMPOSE_PAGE(menu, opt.spareBase);
#endif
    }
    OPT_COPY_SHOWN(opt.shownBase, opt.spareBase, OPT_PAGE_BYTES);
    GSPGPU_FlushDataCache(opt.shownBase, OPT_PAGE_BYTES);
    CtrLog_endConsole();
    opt.dirty = false;
}
static void fpsboxMenuFlush(CtrMenu* menu); static void quitblackFlush(CtrMenu*); static CtrMenuAction quitblackKeys(CtrMenu*, u32); void CtrFpsbox_menuNoErase(uint16_t*, const uint16_t*, bool); CtrMenuAction CtrCtrlui_main(CtrMenu* menu, int32_t dir); CtrMenuAction CtrCtrlui_update(CtrMenu* menu, u32 down);
// ===[ The menu ]===

// The menu's sounds: the game's own cursor sound (argui's sndMove) for the open and each move, its choice sound
// (sndSelect) for a change or choice; back and close are silent. Played through the audio's menu hold
// (ctr_audio_system.c's CtrAudio_menuSound), so they sound while the game's sounds are held. A press has its sound only
// if it changed something (optKeysHeard).
void CtrAudio_menuSound(AudioSystem* audio, int32_t id);
#ifdef CTR_FPSGHOST_DIAG
static void fpsghostOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
static void fpsghostClose(CtrMenu* menu);
#endif
#line 910
OPT_STATIC void optSound(bool selectSound);

OPT_CODE void __wrap_CtrArgui_menuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    opt.menu = menu;
    opt.spareBase = nullptr;
    opt.restore = console->frameBuffer;
    menu->console = console;
    optPlace(menu); // before the screens' open prints its page, so that it prints off screen
#ifdef CTRPORT_PLAYER_SHOTS
    uint64_t t0 = svcGetSystemTick(), shownAt = 0;
    bool early = !opt.legacy && !CtrLegal_shown(); if (early) {
#else
    bool early = !CtrLegal_shown(); if (early) { // (legal page left open last time: the screens' open closes it first)
#endif
    // The page shown first (before the screens' open, whose CtrLegal_close is menupause's hook that holds the audio,
    // once a 35 ms fade on this thread): the settings as the screens' open takes them, the main page, drawn and
    // copied to the shown buffer; then that screen lit, if Other screen Off had darkened it (the renderer's next screen
    // set-up would light it a frame later; the top screen's is optLive's).
    CtrLog_setConsoleEnabled(false); // as the screens' open does first: no log line over the page
    menu->settings = *settings;
    if (menu->settings.bottomMode == BOTTOM_MODE_MAGNIFIER) menu->settings.bottomMode = BOTTOM_MODE_OFF;
    menu->caps = caps;
    menu->open = true;
    menu->page = MENU_PAGE_OPTIONS;
    *(uint8_t*) &menu->edited = gHintSeen ^ 1u;
    g.menuIsOpen = 1;
    opt.page = OPT_PAGE_MAIN;
    memset(opt.cur, 0, sizeof(opt.cur));
    opt.dirty = true;
    optLive();
    fpsboxMenuFlush(menu);
    if (opt.placed && opt.scr == GFX_BOTTOM && settings->bottomMode != BOTTOM_MODE_CONSOLE && R_SUCCEEDED(CtrDimlight_bottomOnInit())) {
        GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
#ifdef CTRPORT_PLAYER_SHOTS
    shownAt = svcGetSystemTick();
#endif
    }
    CTR_FPSDIAG_OPEN(menu, console, settings, caps);
    *(uint8_t*) &menu->edited = gHintSeen ^ 1u;
    gHintSeen = g.menuIsOpen = 1;
    opt.page = OPT_PAGE_MAIN;
    memset(opt.cur, 0, sizeof(opt.cur));
    opt.dirty = !early; __asm__ volatile("nop"); // drawn above, and not printed over now (nop: 75zo's size)
    optLive();
    fpsboxMenuFlush(menu);
    optSound(false);
#ifdef CTRPORT_PLAYER_SHOTS
    opt.sinceOpen = 0;
    uint64_t t1 = svcGetSystemTick();
    if (shownAt == 0) shownAt = t1;
    logInfo(optLogOpened, (unsigned) opt.frame, (unsigned long) ((shownAt - t0) / 268u), (int) opt.placed, (unsigned long) ((t1 - t0) / 268u));
#endif
}

// The menu leaves the top screen (closed, or the game moved up): cleared, then dark again if Other screen is Off, as
// the renderer's screensConfigure leaves it (screensTopBacklight).
OPT_STATIC void optLeaveTop(const CtrMenu* menu) {
    if (!CtrScreens_onBottom || gfxGetScreenFormat(GFX_TOP) != GSP_RGB565_OES) return;
    u16 w = 0, h = 0;
    CtrLog_beginConsole();
    gfxSetDoubleBuffering(GFX_TOP, false);
    for (uint32_t i = 0; 2u > i; i++) {
        u8* fb = i == 0 ? CtrPacer_shownFramebuffer(GFX_TOP, GFX_LEFT, &w, &h) : CtrPacer_backFramebuffer(GFX_TOP, GFX_LEFT);
        if (fb == nullptr) continue;
        memset(fb, 0, 400u * 240u * 2u);
        GSPGPU_FlushDataCache(fb, 400u * 240u * 2u);
    }
    // CtrPacer_shownFramebuffer turned double buffering on, and the counter's paint (gfxGetFramebuffer) would then go
    // to the buffer off screen: the consoles' single buffer again, as screensConfigure leaves it.
    gfxSetDoubleBuffering(GFX_TOP, false);
    CtrLog_endConsole();
    bool off = menu->settings.bottomMode != BOTTOM_MODE_CONSOLE && !CtrScreens_sharedBacklight && menu->caps.wide;
    if (off && !CtrScreens_topBacklightOff && R_SUCCEEDED(CtrDimlight_topOffInit())) {
        GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_TOP); // the screens' wrap keeps it lit for the hint or the counter
        gspLcdExit();
        CtrScreens_topBacklightOff = true;
    }
}

OPT_CODE void __wrap_CtrArgui_menuClose(CtrMenu* menu) {
    if (opt.menu == menu && opt.spareBase != nullptr && menu->console->frameBuffer == opt.spareBase) {
        menu->console->frameBuffer = opt.restore;
    }
    bool wasTop = CtrOptmenu_onTop;
    opt.menu = nullptr;
    opt.spareBase = nullptr;
    opt.placed = false;
    g.menuIsOpen = false;
    CtrOptmenu_onTop = 0;
    if (wasTop) optLeaveTop(menu);
#ifdef ENABLE_WAD16
    CtrSmooth_menuClose();
#endif
#ifdef CTRPORT_PLAYER_SHOTS
    opt.sinceClose = 1;
#endif
    CTR_FPSDIAG_CLOSE(menu);
}

OPT_STATIC void optSound(bool selectSound) {
    if (g.audio != nullptr) CtrAudio_menuSound(g.audio, g.assetIds[selectSound]);
}

// A page's row chosen or changed (dir -1, 1, or 0 for A).
OPT_STATIC CtrMenuAction optActivate(CtrMenu* menu, uint32_t i, int32_t dir) {
#if defined(__3DS__) && !CTR_PACK_EXACT_AVAILABLE && defined(ENABLE_WAD16)
    // Retain the original slot so no later function changes its cache phase (each count +4: ctrlui's Controls row).
    __asm__ volatile(".rept 9\n nop\n .endr");
#elif defined(__3DS__) && !CTR_PACK_EXACT_AVAILABLE
    __asm__ volatile(".rept 6\n nop\n .endr");
#elif defined(__3DS__)
    __asm__ volatile(".rept 4\n nop\n .endr");
#endif
    CtrDisplaySettings* s = &menu->settings;
    if (opt.page == OPT_PAGE_FPS) {
        bool on = gFpsScreen != 0;
        if (i == OPT_FPS_BACK) {
            if (dir == 0) opt.page = OPT_PAGE_MAIN;
            return MENU_ACTION_NONE;
        }
        if (i == OPT_FPS_ON) gFpsScreen = on ? 0u : (uint8_t) (opt.pref + 1u);
        else if (!on) return MENU_ACTION_NONE;
        else if (i == OPT_FPS_SCREEN) gFpsScreen = (uint8_t) ((opt.pref ^= 1u) + 1u);
        else gFpsSize = (uint8_t) ((gFpsSize + (uint32_t) (dir < 0 ? 2 : 1)) % 3u);
        menu->edited = true;
        return MENU_ACTION_CHANGED;
    }
    if (opt.page == OPT_PAGE_DEBUG) {
        if (i == OPT_DEBUG_LOG) {
            s->bottomMode = s->bottomMode == BOTTOM_MODE_CONSOLE ? BOTTOM_MODE_OFF : BOTTOM_MODE_CONSOLE;
            menu->edited = true;
            return MENU_ACTION_CHANGED;
        }
        if (dir != 0) return MENU_ACTION_NONE;
        if (i == OPT_DEBUG_PERF) return MENU_ACTION_PERF_TEST;
        opt.page = OPT_PAGE_MAIN;
        return MENU_ACTION_NONE;
    }
#ifdef ENABLE_WAD16
    // UNDERTALE's Smoothing: left, right or A switch Recommended and Custom (the Custom choices are kept); A on Custom
    // opens the page of parts, where each row switches its part and applies it at once.
    if (opt.page == OPT_PAGE_SMOOTH) {
        if (i == OPT_SMOOTH_BACK) {
            if (dir == 0) opt.page = OPT_PAGE_MAIN;
            return MENU_ACTION_NONE;
        }
        CtrSmooth_set(CtrSmooth_get() ^ 1u << i);
        menu->edited = true;
        return MENU_ACTION_CHANGED;
    }
    if (i == OPT_MAIN_SMOOTHING) {
        uint32_t v = CtrSmooth_get();
        if (s->smoothing == SMOOTHING_OFF) {
            // A test's Off: any press turns smoothing back on, on the choice kept (Recommended or Custom).
            s->smoothing = SMOOTHING_SMART;
            menu->edited = true;
            return MENU_ACTION_CHANGED;
        }
        if (dir == 0 && (v & 0x80u) != 0) {
            CtrSmooth_menuOpen();
            opt.page = OPT_PAGE_SMOOTH;
            opt.cur[OPT_PAGE_SMOOTH] = 0;
            return MENU_ACTION_NONE;
        }
        CtrSmooth_set(v ^ 0x80u);
        menu->edited = true;
        return MENU_ACTION_CHANGED;
    }
#endif
    if (i == OPT_MAIN_FPS || i == OPT_MAIN_DEBUG) {
        if (dir >= 0) opt.page = i == OPT_MAIN_FPS ? OPT_PAGE_FPS : OPT_PAGE_DEBUG;
        return MENU_ACTION_NONE;
    }
    if (i == OPT_MAIN_CONTROLS) {
        // A opens the Controls page (ctr_ctrlui.c); left or right switch Default and B confirms, as its Layout row
        // does. (Before: A opened ctr_menu.c's console page through the screens' menu, and left or right did nothing.)
        // The page then has the menu's keys while menu->page is MENU_PAGE_CONTROLS (optKeys, below).
        return CtrCtrlui_main(menu, dir);
    }
    if (i == OPT_MAIN_LEGAL || i == OPT_MAIN_CREDITS) {
        if (dir >= 0) CtrLegalui_open(menu, i == OPT_MAIN_CREDITS);
        return MENU_ACTION_NONE;
    }
    if (i >= OPT_MAIN_RESUME) {
        if (dir != 0) return MENU_ACTION_NONE;
        return i == OPT_MAIN_RESUME ? MENU_ACTION_RESUME : MENU_ACTION_QUIT;
    }
    if (i == OPT_MAIN_GAME) CtrScreens_game ^= 1u;
    else if (i == OPT_MAIN_DIALOGUE) {
        if (CtrScreens_game != 0) return MENU_ACTION_NONE;
        s->dialogueBottom = !s->dialogueBottom;
    } else if (i == OPT_MAIN_SMOOTHING) s->smoothing = s->smoothing == SMOOTHING_OFF ? SMOOTHING_SMART : SMOOTHING_OFF;
    else {
#if CTR_PACK_EXACT_AVAILABLE
        if (!CTR_PACK_EXACT_AVAILABLE) return MENU_ACTION_NONE;
        s->exactColours = !s->exactColours;
#else
        return MENU_ACTION_NONE;
#endif
    }
    menu->edited = true;
    return MENU_ACTION_CHANGED;
}
static CtrMenuAction fpsghostActivate(CtrMenu*, uint32_t, int32_t); void CtrMenuquiet_beforeKeys(CtrMenu* menu, u32 down) __asm__("CtrMenuquiet_press");
OPT_STATIC CtrMenuAction optKeys(CtrMenu* menu, u32 down) {
    u32 folded = CTR_MENU_FOLD(down);
    CtrMenuquiet_beforeKeys(menu, down); __asm__ volatile(".rept 6\n nop\n .endr"); // spend the retained sound-call slot
    opt.dirty = true;
    if (CtrLegal_shown()) {
        CtrLegalui_update(menu, down);
        return MENU_ACTION_NONE;
    }
    if (menu->page != MENU_PAGE_OPTIONS) {
        menu->cursor = OPT_SCREENS_CONTROLS;
        return CtrCtrlui_update(menu, down); // the Controls page (ctr_ctrlui.c; SELECT: back to the game)
    }
    if (down & KEY_SELECT) return MENU_ACTION_RESUME;
    uint32_t page = opt.page, count = optCounts[page], i = opt.cur[page];
    if (down & KEY_B) {
        if (page == OPT_PAGE_MAIN) return MENU_ACTION_RESUME;
        opt.page = OPT_PAGE_MAIN;
        opt.cur[OPT_PAGE_MAIN] = optParent[page];
        return MENU_ACTION_NONE;
    }
    // Up and down pass over grey rows (Dialogue box says "Needs the game on top.").
    for (uint32_t n = 0; count > n; n++) {
        if (folded & KEY_DUP) i = i == 0 ? count - 1u : i - 1u;
        if (folded & KEY_DDOWN) i = i + 1u == count ? 0u : i + 1u;
        OptRow r;
        optRow(menu, page, i, &r);
        if (!r.dis || !(folded & (KEY_DUP | KEY_DDOWN))) break;
    }
    opt.cur[page] = (uint8_t) i;
    if (folded & (KEY_DUP | KEY_DDOWN)) return MENU_ACTION_NONE;
    int32_t dir = (folded & KEY_DRIGHT) ? 1 : ((folded & KEY_DLEFT) ? -1 : 0);
    if (dir == 0 && !(down & KEY_A)) return MENU_ACTION_NONE;
    CtrMenuAction action = fpsghostActivate(menu, i, dir);
    if (page != OPT_PAGE_MAIN && opt.page == OPT_PAGE_MAIN) opt.cur[OPT_PAGE_MAIN] = optParent[page];
    return action;
}

// A press's sound only if the press changed something (maintainer 11-06: Right on Legal notices sounded, and nothing
// happened): an action for main.c, or a new page, cursor or position; a value's change always returns an action. The
// same sound, as early as the change is known: after the keys, or just before the page that changed draws or reads a
// file (its hooks: CtrOptmenu_pressSound and CtrOptmenu_pressLegal), so neither delays it and the Credits' vblank wait
// is still followed by the copy. Appended (late_reserve.ld).
#if defined(__3DS__) && defined(__GNUC__)
#define OPT_PRESS_GLOBAL __attribute__((noinline, optimize("Os"), section(".text_menupress")))
#else
#define OPT_PRESS_GLOBAL
#endif
#define OPT_PRESS OPT_PRESS_GLOBAL static
// The menu's hold, its rows' ">" and the counter's corner, after every other late function: growth or a new neighbour here would
// move the snow floor, HOME and CtrBlank code that follows .text_menupress (review R379 F2). So this code calls
// .text_menupress, and only retargeted calls reach it. Appended (late_reserve.ld).
#if defined(__3DS__) && defined(__GNUC__)
#define OPT_HOLD_GLOBAL __attribute__((noinline, optimize("Os"), section(".text_menuhold")))
#define OPT_HOLD_RO __attribute__((section(".rodata.menuhold_in_text")))
#else
#define OPT_HOLD_GLOBAL
#define OPT_HOLD_RO
#endif
#define OPT_HOLD OPT_HOLD_GLOBAL static
u32 CtrLegalui_mark(u32 folded);
bool CtrOptmenu_pressLeft(void);
#if defined(__3DS__) && defined(__GNUC__)
__attribute__((section(".bss.menupress_late")))
#endif
static struct { u32 keys, legalMark; bool quitArmed; uint8_t holdKey, holdFrames; } optPress; // keys: the press's menu keys until its sound has played or it ends

OPT_PRESS u32 optPressMark(const CtrMenu* menu) {
    return (u32) opt.page | (u32) opt.cur[opt.page] << 8 | (u32) menu->page << 16 | (u32) optPress.quitArmed << 24;
}

// The press changed something: its sound, once. Nothing while no press is being handled (a held frame's draw).
void CtrOptmenu_pressSound(void);
void CtrOptmenu_pressLegal(void);
OPT_PRESS_GLOBAL void CtrOptmenu_pressSound(void) {
    u32 keys = optPress.keys;
    optPress.keys = 0;
    if (keys != 0) optSound((keys & 0xC0u) == 0);
}

// The press being handled has Left in it (the Circle Pad and C-stick folded in): the Controls page's Layout steps back.
OPT_PRESS_GLOBAL bool CtrOptmenu_pressLeft(void) {
    return (optPress.keys & KEY_DLEFT) != 0;
}

// A Legal or Credits page is about to draw or read. A draw alone says nothing: the roll and a held key's repeat draw too.
OPT_PRESS_GLOBAL void CtrOptmenu_pressLegal(void) {
    if (optPress.keys != 0 && optPress.legalMark != CtrLegalui_mark(optPress.keys)) CtrOptmenu_pressSound();
}

// The rows' appended text, on the page's own lines. (polish audit, 11-06.) Quit game: while its first A
// is waiting for the second, the row says so. Exact colours Off had a blank help line; the row's other state has one.
#if defined(__3DS__) && defined(__GNUC__)
#define OPT_PRESS_RO __attribute__((section(".rodata.menupress_in_text")))
#else
#define OPT_PRESS_RO
#endif
static const char omQuitArmed[] OPT_PRESS_RO = "Press A again to quit";
#if CTR_PACK_EXACT_AVAILABLE
static const char omHlpExactOff[] OPT_PRESS_RO = "Faster, less exact. Applies on restart.";
#endif
OPT_PRESS void optRowMainHeard(const CtrMenu* menu, uint32_t i, OptRow* r) {
    optRowMain(menu, i, r);
    if (i == OPT_MAIN_QUIT && optPress.quitArmed) r->label = omQuitArmed;
#if CTR_PACK_EXACT_AVAILABLE
    if (i == OPT_MAIN_EXACT && !r->dis && !menu->settings.exactColours) r->help = omHlpExactOff;
#endif
}

// A on the main page's Quit game row, with no direction, B or SELECT in the press (those win over A in optKeys).
OPT_PRESS bool optQuitPress(const CtrMenu* menu, u32 down, u32 folded) {
    return (down & KEY_A) != 0 && (folded & 0xF0u) == 0 && (down & (KEY_B | KEY_SELECT)) == 0 && !CtrLegal_shown() &&
           menu->page == MENU_PAGE_OPTIONS && opt.page == OPT_PAGE_MAIN && opt.cur[OPT_PAGE_MAIN] == OPT_MAIN_QUIT;
}

OPT_PRESS CtrMenuAction optKeysHeard(CtrMenu* menu, u32 down) {
    u32 folded = CTR_MENU_FOLD(down);
    u32 mark = optPressMark(menu);
    // The menu's keys; on a Legal or Credits page L and R too, which move it (their ends: silent by the same mark).
    optPress.keys = folded & (CtrLegal_shown() ? 0x3FFu : 0xFFu);
    optPress.legalMark = CtrLegalui_mark(optPress.keys);
    // Quit game takes two A presses, and any other key in between cancels (polish audit assistant 4: one A on the last row,
    // a wrap of Up from the top, and the game was gone). Only this row's A: no other way of quitting changes.
    bool armed = optQuitPress(menu, down, folded) && !optPress.quitArmed;
    if (armed != optPress.quitArmed) opt.dirty = true;
    // A cancel changes the row, so it sounds whichever key made it: L, R, X and Y do nothing else off a Legal page. No up
    // or down can be among them here (the mask above keeps those), so this is START's sound.
    if (optPress.keys == 0 && optPress.quitArmed && !armed) optPress.keys = down;
    optPress.quitArmed = armed;
    CtrMenuAction action = armed ? MENU_ACTION_NONE : optKeys(menu, down);
    if (action != MENU_ACTION_NONE || mark != optPressMark(menu)) CtrOptmenu_pressSound();
    CtrOptmenu_pressLegal();
    optPress.keys = 0;
    return action;
}

// Rows that open a page all show ">" and take Right as A (polish audit assistant 15): these two now do, as Debug and FPS counter
// did (optActivate).
OPT_HOLD void optRowMainShown(const CtrMenu* menu, uint32_t i, OptRow* r) {
    optRowMainHeard(menu, i, r);
    if (i == OPT_MAIN_LEGAL || i == OPT_MAIN_CREDITS) r->value = valOpen;
}

// Held Up or Down on the menu's lists (polish audit assistant 6): a step on the press, then every other frame after 10 frames
// held, the reader's timing (CtrLegalui_update). A repeat is a press of that key, with a press's sound and draw. It never
// wraps: at the list's end the hold is over, with no sound and no redraw, and only a fresh press wraps. Each list's first
// and last rows are never grey (test_menu_press_sound.py), so a list's end is its last index. Left and Right stay single
// presses; the reader and the Credits have their own repeat. A key held since before the menu saw a press does nothing.
bool CtrCtrlui_holdMoves(u32 key);
bool CtrLegalui_holdMoves(u32 key);
OPT_HOLD bool optHoldMoves(const CtrMenu* menu, u32 key) {
    if (CtrLegal_shown()) return CtrLegalui_holdMoves(key);
    if (menu->page == MENU_PAGE_CONTROLS) return CtrCtrlui_holdMoves(key);
    // Not optCounts: a reference from here changes where the compiler lays out this file's frozen strings (its section
    // anchors), and optRowMain's code with it.
#ifdef ENABLE_WAD16
    static const uint8_t last[OPT_PAGES] OPT_HOLD_RO = { OPT_MAIN_COUNT - 1, OPT_FPS_COUNT - 1, OPT_DEBUG_COUNT - 1, OPT_SMOOTH_COUNT - 1 };
#else
    static const uint8_t last[OPT_PAGES] OPT_HOLD_RO = { OPT_MAIN_COUNT - 1, OPT_FPS_COUNT - 1, OPT_DEBUG_COUNT - 1 };
#endif
    return menu->page == MENU_PAGE_OPTIONS && opt.cur[opt.page] != (key == KEY_DUP ? 0u : last[opt.page]);
}

// A hold starts from a press of Up or Down alone. Any other of the menu's keys ends one, so a repeat never follows a page's
// change, and never cancels the Quit game row's first A. In optKeysHeard's call in __wrap_CtrArgui_menuUpdate, so that function
// keeps its size. A repeat calls optKeysHeard itself, so the hold's frames run on.
OPT_HOLD CtrMenuAction optKeysHold(CtrMenu* menu, u32 down) {
    u32 alone = CTR_MENU_FOLD(down) & 0xFFFu;
    if (alone != 0) {
        optPress.holdKey = alone == KEY_DUP || alone == KEY_DDOWN ? (uint8_t) alone : 0u;
        optPress.holdFrames = 0;
    }
    return quitblackKeys(menu, down);
}

// In optLive's call in __wrap_CtrArgui_menuUpdate, so that function keeps its size: after any press, ahead of the flush.
OPT_HOLD void optHoldTick(void) {
    CtrMenu* menu = opt.menu;
    u32 key = optPress.holdKey;
    if (key != 0 && menu) {
        uint8_t frames = (uint8_t) (optPress.holdFrames + 1u); // can't wrap: every list ends the hold before 255 frames
        optPress.holdFrames = frames;
        if ((CTR_MENU_FOLD(hidKeysHeld()) & key) == 0) optPress.holdKey = 0;
        else if (frames > 10u && (frames & 1u) == 0) {
            if (optHoldMoves(menu, key)) optKeysHeard(menu, key);
            else optPress.holdKey = 0;
        }
    }
    optLive();
}

// Once a frame while the menu is open (main.c), after the frame's start and before its CtrRenderer_setDisplay: Other
// screen Off as the settings say again (the yield wrapper held it off for the frame's screensConfigure while the menu
// was on top), the keys, then where the menu goes and its page.
OPT_CODE CtrMenuAction __wrap_CtrArgui_menuUpdate(CtrMenu* menu, u32 down) {
#ifdef CTRPORT_PLAYER_SHOTS
    down |= opt.pendingKeys;
    opt.pendingKeys = 0;
#endif
    if (!menu->open) return MENU_ACTION_NONE;
    bool wasTop = CtrOptmenu_onTop;
    if (wasTop) CtrScreens_otherOff = menu->settings.bottomMode != BOTTOM_MODE_CONSOLE;
    CtrMenuAction action = down != 0 ? optKeysHold(menu, down) : MENU_ACTION_NONE;
    if (down == 0 && CtrLegal_shown()) CtrLegalui_update(menu, 0); // held keys and the credits' roll
    optHoldTick(); // then optLive: a move (Game screen) happens at this frame's yield; the next call places the menu again
    quitblackFlush(menu);
    return action;
}

#ifdef CTRPORT_PLAYER_SHOTS
// ===[ Test probe (shots builds only): optprobe.txt beside settings.ini scripts menu keys and captures ]===
// Lines: "open <frame> key <hex mask>", "open <frame> shot", "close <frame> shot", frames counted from the menu's
// open or close. A shot writes both screens' shown buffers as optshot_NN_top.bmp and optshot_NN_bot.bmp.

OPT_STATIC void optProbeLoad(const char* path) {
    const char* slash = strrchr(path, '/');
    size_t n = slash == nullptr ? 0 : (size_t) (slash - path) + 1u;
    if (n >= sizeof(opt.dir)) n = 0;
    memcpy(opt.dir, path, n);
    opt.dir[n] = '\0';
    char name[96], line[64];
    snprintf(name, sizeof(name), optFmtPath, opt.dir, optProbeName);
    FILE* f = fopen(name, omArguiReadMode);
    opt.probes = 0;
    if (f == nullptr) return;
    while (opt.probes < 48u && fgets(line, sizeof(line), f) != nullptr) {
        unsigned tick = 0;
        unsigned long key = 0;
        OptProbe* p = &opt.probe[opt.probes];
        p->done = 0;
        if (sscanf(line, optFmtKey, &tick, &key) == 2) p->kind = 1;
        else if (sscanf(line, optFmtOpenShot, &tick) == 1) p->kind = 2;
        else if (sscanf(line, optFmtCloseShot, &tick) == 1) p->kind = 3;
        else continue;
        p->tick = (uint16_t) tick;
        p->arg = (uint32_t) key;
        opt.probes++;
    }
    fclose(f);
    logInfo(optLogProbes, (unsigned) opt.probes, name);
}

OPT_STATIC void optPut32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

OPT_STATIC void optShotScreen(gfxScreen_t s, uint32_t n) {
    GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(s);
    u16 w = 0, h = 0;
    uint8_t* fb = CtrPacer_shownFramebuffer(s, GFX_LEFT, &w, &h);
    if (fmt == GSP_RGB565_OES) gfxSetDoubleBuffering(s, false);
    if (fb == nullptr || w != 240u || h > 400u) return;
    uint32_t bpp = fmt == GSP_RGB565_OES ? 2u : 3u;
    GSPGPU_FlushDataCache(fb, (u32) w * h * bpp);
    GSPGPU_InvalidateDataCache(fb, (u32) w * h * bpp);
    char name[96];
    snprintf(name, sizeof(name), optFmtShot, opt.dir, (unsigned) n, s == GFX_TOP ? optTopName : optBotName);
    FILE* f = fopen(name, optWriteMode);
    if (f == nullptr) return;
    uint8_t hd[54];
    memset(hd, 0, sizeof(hd));
    hd[0] = 'B';
    hd[1] = 'M';
    optPut32(hd + 2, 54u + (uint32_t) h * 720u);
    hd[10] = 54;
    hd[14] = 40;
    optPut32(hd + 18, h);
    optPut32(hd + 22, 240u);
    hd[26] = 1;
    hd[28] = 24;
    optPut32(hd + 34, (uint32_t) h * 720u);
    fwrite(hd, 1, sizeof(hd), f);
    uint8_t row[400 * 3];
    for (int32_t y = 239; y >= 0; y--) {
        for (uint32_t x = 0; h > x; x++) {
            uint32_t idx = x * 240u + 239u - (uint32_t) y;
            uint8_t* o = row + x * 3u;
            if (bpp == 3u) {
                memcpy(o, fb + idx * 3u, 3);
            } else {
                uint32_t v = ((const uint16_t*) fb)[idx];
                o[0] = (uint8_t) ((v & 31u) << 3);
                o[1] = (uint8_t) (((v >> 5) & 63u) << 2);
                o[2] = (uint8_t) ((v >> 11) << 3);
            }
        }
        fwrite(row, 1, (size_t) h * 3u, f);
    }
    fclose(f);
    CtrLinkFiles_note(name); // to the PC at the end with --send-files (console rows)
}

OPT_STATIC void optProbeTick(void) {
    if (opt.wroteAutopause) { // main has read it before its loop: gone, so that later runs on the card don't open the menu
        char name[96];
        snprintf(name, sizeof(name), optFmtPath, opt.dir, optAutopauseName);
        remove(name);
        opt.wroteAutopause = false;
    }
    if (g.menuIsOpen) opt.sinceOpen++;
    else if (opt.sinceClose != 0) opt.sinceClose++;
    for (uint32_t i = 0; opt.probes > i; i++) {
        OptProbe* p = &opt.probe[i];
        if (p->done) continue;
        bool open = p->kind != 3u;
        if (open != g.menuIsOpen || p->tick != (open ? opt.sinceOpen : opt.sinceClose)) continue;
        p->done = 1;
        if (p->kind == 1u) {
            opt.pendingKeys |= p->arg;
            logInfo(optLogKey, (unsigned long) p->arg, (unsigned) opt.sinceOpen);
            continue;
        }
        optShotScreen(GFX_TOP, opt.shots);
        optShotScreen(GFX_BOTTOM, opt.shots);
        logInfo(optLogShot, (unsigned) opt.shots, (unsigned) opt.sinceOpen, (unsigned) opt.sinceClose);
        opt.shots++;
    }
}

// Shots builds only (CMakeLists.txt): main.c asks this once a frame, right after the screens' yield wrapper, which
// moves the game. The probe counts frames and takes its captures here.
bool __real_CtrRenderer_dialogueShown(Renderer* renderer);
bool __wrap_CtrRenderer_dialogueShown(Renderer* renderer);

OPT_CODE bool __wrap_CtrRenderer_dialogueShown(Renderer* renderer) {
    optProbeTick();
    return __real_CtrRenderer_dialogueShown(renderer);
}

// Console runs can't put optprobe.txt on the card: --ini opt_probe=1 gives these steps instead (the p2.txt probe
// steps), with the menu scheduled at frame 300 (autopause.txt, written here and removed once main has read
// it). Captures go to the PC with --send-files.
typedef struct {
    uint16_t tick;
    uint8_t kind;
    uint8_t key;
} OptStep;
extern const OptStep CtrCtrlui_probe[]; extern const uint8_t CtrCtrlui_probeCount; // opt_probe=2: ctr_ctrlui.c's
static const OptStep optSteps[] OPT_RO = {
    { 5, 2, 0x00 }, { 10, 1, 0x80 }, { 11, 2, 0x00 }, { 12, 2, 0x00 }, { 13, 1, 0x80 }, { 14, 2, 0x00 }, { 15, 2, 0x00 },
    { 20, 1, 0x80 }, { 22, 1, 0x80 }, { 24, 1, 0x00 }, { 26, 1, 0x01 }, { 28, 2, 0x00 }, { 30, 1, 0x80 }, { 32, 2, 0x00 },
    { 34, 1, 0x40 }, { 36, 1, 0x01 }, { 38, 2, 0x00 }, { 40, 1, 0x80 }, { 42, 1, 0x80 }, { 44, 1, 0x10 }, { 46, 2, 0x00 },
    { 48, 1, 0x02 }, { 50, 2, 0x00 }, { 52, 1, 0x40 }, { 54, 1, 0x40 }, { 56, 1, 0x40 }, { 58, 1, 0x40 }, { 60, 1, 0x00 },
    { 62, 1, 0x10 }, { 64, 2, 0x00 }, { 70, 2, 0x00 }, { 74, 1, 0x10 }, { 78, 2, 0x00 }, { 84, 2, 0x00 }, { 88, 1, 0x40 },
    { 90, 1, 0x40 }, { 92, 1, 0x40 }, { 94, 1, 0x40 }, { 96, 1, 0x01 }, { 98, 2, 0x00 }, { 100, 1, 0x02 }, { 104, 1, 0x04 },
    { 20, 3, 0x00 }, { 120, 3, 0x00 },
};

OPT_STATIC bool optProbeBuiltin(const char* line) {
    int value = 0;
    if (sscanf(line, optFmtProbeKey, &value) != 1 || (value != 1 && value != 2)) return false;
    if (opt.probes != 0) return true; // applyIniOverrides runs twice
    const OptStep* steps = value == 2 ? CtrCtrlui_probe : optSteps; // 2: the Controls page's steps (change ctrlui)
    uint32_t count = value == 2 ? CtrCtrlui_probeCount : sizeof(optSteps) / sizeof(optSteps[0]);
    for (uint32_t i = 0; count > i; i++) {
        OptProbe* p = &opt.probe[i];
        p->tick = steps[i].tick; p->kind = steps[i].kind; p->done = 0; p->arg = steps[i].key;
    }
    opt.probes = (uint8_t) count;
    char name[96];
    snprintf(name, sizeof(name), optFmtPath, opt.dir, optAutopauseName);
    FILE* f = fopen(name, optWriteMode);
    if (f != nullptr) {
        fputs(optAutopauseText, f);
        fclose(f);
        opt.wroteAutopause = true;
    }
    logInfo(optLogBuiltin);
    return true;
}
#endif

// ===[ Open on SELECT's press ]===
//
// main.c's combo (ctr_input_combo.h) opens the menu when hidKeysUp has SELECT and SELECT was pressed alone (no button
// but directions with it since its press). hidKeysUp is called by nothing else (CMakeLists.txt wraps it), so this
// gives the combo SELECT's release on the frame of its press, when nothing rules the menu out, and never the real
// release. Kept from opening: L, R, ZL or ZR held on this frame or the last one (Rosalina's L+Down+SELECT, the
// screenshot's L+SELECT in shots builds), and any SELECT press while held with one of them until SELECT is released.
// The combo itself still rules out SELECT with any other button (START: SELECT+START quits, checked before the menu).
u32 __real_hidKeysUp(void);
u32 __wrap_hidKeysUp(void);

OPT_CODE u32 __wrap_hidKeysUp(void) {
    u32 up = __real_hidKeysUp(), down = hidKeysDown();
#ifdef CTRPORT_PLAYER_SHOTS
    for (uint32_t i = 0; opt.presses > i; i++) {
        if ((uint16_t) (opt.pressAt[i] + 4u) != opt.frame) continue;
        up |= KEY_SELECT; // the probe's tap: released 4 frames after its press (133 ms)
        logInfo(optLogPress, optLogUp, (unsigned) opt.frame, opt.legacy ? optLogLegacy : omStrEmpty);
    }
    opt.frame++;
    if (opt.legacy) return up;
#endif
    const u32 lr = KEY_L | KEY_R | KEY_ZL | KEY_ZR;
    u32 before = opt.keysHeld, held = hidKeysHeld() | down;
    opt.keysHeld = held;
    if (((held | before) & lr) != 0 && (held & KEY_SELECT) != 0) opt.selectBlocked = true;
    if ((held & KEY_SELECT) == 0 && (down & KEY_SELECT) == 0) opt.selectBlocked = false;
    up &= ~(u32) KEY_SELECT;
    if ((down & KEY_SELECT) != 0 && !opt.selectBlocked) up |= KEY_SELECT;
    return up;
}

#ifdef CTRPORT_PLAYER_SHOTS
// The probe's press (opt_press_at = <frame>, frames counted by hidKeysUp's calls): SELECT down on that frame, as
// libctru would report a press, for every caller that frame (shots builds wrap hidKeysDown).
u32 __real_hidKeysDown(void);
u32 __wrap_hidKeysDown(void);

OPT_CODE u32 __wrap_hidKeysDown(void) {
    u32 down = __real_hidKeysDown();
    for (uint32_t i = 0; opt.presses > i; i++) {
        if (opt.pressAt[i] != opt.frame) continue;
        if ((down & KEY_SELECT) == 0) logInfo(optLogPress, optLogDown, (unsigned) opt.frame, opt.legacy ? optLogLegacy : omStrEmpty);
        down |= KEY_SELECT;
    }
    return down;
}

OPT_STATIC bool optPressParse(const char* line) {
    int frame = 0;
    int value = 0;
    if (sscanf(line, optFmtPressAt, &frame) == 1 || sscanf(line, optFmtPressAt2, &frame) == 1 ||
        sscanf(line, optFmtPressAt3, &frame) == 1) {
        bool known = false; // the file is read more than once, and --ini lines are applied twice
        for (uint32_t i = 0; opt.presses > i; i++) known |= opt.pressAt[i] == (uint16_t) frame;
        if (!known && opt.presses < 4u) opt.pressAt[opt.presses++] = (uint16_t) frame;
        return true;
    }
    if (sscanf(line, optFmtLegacy, &value) == 1) {
        opt.legacy = value != 0;
        return true;
    }
    return false;
}
#endif

// optText for the legal and credits pages (ctr_legal.c): the menu font's glyphs into the page being drawn.
void CtrOptmenu_text(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col);
OPT_CODE void CtrOptmenu_text(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col) {
    optText(fb, x, y, s, col);
}

// All added menu work is at the tail; optFlush keeps its original body/slot.
__attribute__((noinline, noipa, optimize("Os"), section(".text.fpsbox_late")))
static void fpsboxMenuFlush(CtrMenu* menu) {
    OPT_FLUSH_FRAME(menu);
    if (menu->open && opt.placed)
        CtrFpsbox_menuNoErase(opt.shownBase, opt.spareBase, opt.scr == GFX_TOP);
}

// Keep the flush addition outside ctr_argui.c: a same-TU callee makes GCC
// postpone the original wrapper past CtrArgui_keepLit, swapping their slots.
void __real_CtrScreens_flushConsoles(void);
void fpsboxFlushConsoles(void);
__attribute__((noinline, noipa, optimize("Os"), section(".text.fpsbox_late")))
void fpsboxFlushConsoles(void) {
    __real_CtrScreens_flushConsoles();
    if (gFpsScreen != 0) CTR_FPSBOX_UPDATE(g.fpsBuf);
}

// A counter is composed to its final pixels before writing the live buffer.
// No black intermediate frame and no full-screen clear over bottom dialogue.
// All new input sections follow every stage75zm input, including its statics.
#define FPS75_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.fpsbox75zm_late")))
static uint8_t fps75Rect[2][4] __attribute__((aligned(4), section(".bss.fpsbox75zm_late")));
// A separate tail table avoids changing GCC's anchor choices for the menu's
// existing constant pool when this sampler adds two more Morton references.
static const uint8_t fps75Morton[8] __attribute__((section(".rodata.fpsbox75zm_late"))) = {0,1,4,5,16,17,20,21};

// The overlay font, sampled exactly as arguiSampleGlyph does. Kept separate
// from optTexel so its existing theme-only constprop body stays byte-exact.
FPS75_CODE static bool fps75Texel(uint8_t ch, int32_t x, int32_t y) {
    const DataWin* dw = g.texCache->dataWin;
    int32_t fid = g.assetIds[2];
    const FontGlyph* gl = fid >= 0 ? dw->font.fonts[fid].glyphLUT[ch] : nullptr;
    if (gl == nullptr)
        return (uint32_t) (x | y) < 8u && (consoleGetDefault()->font.gfx[ch * 8u + (uint32_t) y] & (0x80u >> x)) != 0;
    uint32_t lx = (uint32_t) (x - gl->offset), ly = (uint32_t) y;
    if (lx >= gl->sourceWidth || ly >= gl->sourceHeight) return false;
    const TexturePageItem* item = &dw->tpag.items[dw->font.fonts[fid].tpagIndex];
    CtrPage* page = CtrTextures_acquire(g.texCache, (uint32_t) item->texturePageId);
    uint32_t px = item->sourceX + gl->sourceX + lx, py = item->sourceY + gl->sourceY + ly;
    int32_t tx = (int32_t) (px >> 8), ty = (int32_t) (py >> 8);
    const CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
    if (!tile->valid) CtrTextures_ensureTiles(g.texCache, page, tx, ty, tx, ty);
    uint32_t idx = ((py & 0xF8u) << 8) | ((px & 0xF8u) << 3) |
                   fps75Morton[px & 7u] | ((uint32_t) fps75Morton[py & 7u] << 1);
    return (((const uint16_t*) tile->tex.data)[idx << (tile->tex.fmt == GPU_RGBA8)] & 1u) != 0;
}

FPS75_CODE static void fps75Pixel(uint32_t index, uint16_t colour) {
    if (g.rgb565) ((uint16_t*) g.fb)[index] = colour;
    else {
        uint8_t* p = (uint8_t*) g.fb + index * 3u;
        p[0] = p[1] = p[2] = (uint8_t) colour;
    }
}

FPS75_CODE void CtrFpsbox_drawDigits(int32_t x, int32_t y, const char* text, uint32_t colour) {
    // Preserve the sampler's two-digit display, including 00 through 09.
    uint16_t rows[12] = {0};
    int32_t l = 11, t = 12, r = -1, b = -1;
    for (uint32_t i = 0; text[i] != '\0'; i++) {
        for (int32_t gy = 0; gy < 12; gy++) for (int32_t gx = 0; gx < 6; gx++) {
            if (!fps75Texel((uint8_t) text[i], gx, gy)) continue;
            int32_t xx = gx + (int32_t) i * 5;
            rows[gy] |= (uint16_t) (1u << xx);
            if (xx < l) l = xx;
            if (xx > r) r = xx;
            if (gy < t) t = gy;
            if (gy > b) b = gy;
        }
    }
    if (r < l) return;
    uint32_t left = (uint32_t) (x + l * g.sx), right = (uint32_t) (x + (r + 1) * g.sx);
    uint32_t top = (uint32_t) (y + t * g.sy), bottom = (uint32_t) (y + (b + 1) * g.sy);
    uint32_t screen = CtrScreens_game ^ g.rgb565;
    uint8_t* old = fps75Rect[screen];
    const uint16_t* backdrop = g.rgb565 && opt.menu != nullptr && opt.menu->open &&
                               opt.placed && opt.scr == GFX_BOTTOM ? opt.spareBase : nullptr;
    if (g.rgb565) {
        // Only pixels outside the new bounds can belong to an old digit.
        // Never blank a current stroke before replacing it with its final value.
        for (uint32_t xx = old[0]; xx < old[2]; xx++) for (uint32_t yy = old[1]; yy < old[3]; yy++) {
            if (xx >= left && xx < right && yy >= top && yy < bottom) continue;
            uint32_t at = xx * 240u + 239u - yy;
            fps75Pixel(at, backdrop != nullptr ? backdrop[at] : 0u);
        }
        old[0] = (uint8_t) left; old[1] = (uint8_t) top;
        old[2] = (uint8_t) right; old[3] = (uint8_t) bottom;
    } else old[2] = old[3] = 0;
    for (int32_t gy = t; gy <= b; gy++) for (int32_t gx = l; gx <= r; gx++) {
        bool on = (rows[gy] & (1u << gx)) != 0;
        for (uint32_t dx = 0; dx < g.sx; dx++) for (uint32_t dy = 0; dy < g.sy; dy++) {
            uint32_t at = (uint32_t) (x + gx * g.sx) * 240u + dx * 240u + 239u -
                          (uint32_t) (y + gy * g.sy) - dy;
            fps75Pixel(at, on ? (uint16_t) colour : backdrop != nullptr ? backdrop[at] : 0u);
        }
    }
}

// The counter over a menu on the bottom screen (polish audit assistant 15: it sat on row 0's heart and the first letter of the
// reader's titles): in the top right corner, which every page leaves empty, with the same two pixels' margin. It stays
// live there, so that FPS counter's rows still show what they do. Nothing changes during play or over a menu on top,
// where the counter is in the black border. CtrFpsbox_drawDigits keeps its rectangle in bytes (x past 255 doesn't fit), so
// it draws as ever, at x, with the screen and the page it reads both moved under it. g.rgb565: this screen's paint, on the
// main thread with the menu's own copies; the other screen's paints don't touch either pointer. The close clears the page,
// the corner with it, and the counter's next paint is back at x.
// The rectangle's coordinates are the unmoved ones, and a smaller size's corner is farther right. So a rectangle that reaches
// past what this size can draw comes from a larger size (the Size row, over this menu or the other screen), and at this
// origin it would run off the screen and the page. It's forgotten, as the page's redraw has already covered those digits.
// Any rectangle kept ends at x + 11 * sx or before, so at column 318 or before. Same-size readings keep theirs.
void __wrap_CtrFpsbox_drawDigits(int32_t x, int32_t y, const char* text, uint32_t colour);
void CtrFpsbox_drawPlaced(int32_t x, int32_t y, const char* text, uint32_t colour);
OPT_HOLD_GLOBAL void CtrFpsbox_drawPlaced(int32_t x, int32_t y, const char* text, uint32_t colour) {
    int32_t by = 0;
    void* screen = g.fb;
    uint16_t* page = opt.spareBase;
    if (g.rgb565 && opt.menu != nullptr && opt.menu->open && opt.placed && opt.scr == GFX_BOTTOM) {
        by = (318 - 11 * (int32_t) g.sx - x) * 240;
        uint8_t* old = fps75Rect[CtrScreens_game ^ g.rgb565];
        if (old[2] > x + 11 * (int32_t) g.sx) old[2] = old[3] = 0;
        g.fb = (uint16_t*) screen + by;
        opt.spareBase = page + by;
    }
    __wrap_CtrFpsbox_drawDigits(x, y, text, colour);
    if (by != 0) {
        g.fb = screen;
        opt.spareBase = page;
    }
}

// main.c's SAVE FAILED line (its saveFailureWarning, row 24 of the menu console), printed only while the menu is
// closed. With the menu open that console is the page's: the main page redraws it whole before each copy (optCompose,
// so the line never showed there), but the Controls and legal pages print through it and are shown every frame
// (optFlush), the line then over one of their rows (chg-savefail-optpage). Late text, last, so no earlier late code
// moves.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
__attribute__((noinline, noipa, used, section(".text.savewarn_late"))) void CtrOptmenu_saveWarn(const char* line) {
    if (opt.menu == nullptr) printf(line);
}
#pragma GCC diagnostic pop

#if defined(ENABLE_WAD16)
// ===[ menufirst (change menufirst, 11-07; UNDERTALE): the options menu's first open ]===
// maintainer's o3DS, 11-07: the first open after boot sounded about 0.5 s late, and on the bottom screen parts of the page
// near its top showed a moment before the rest. Late code, last in late text (.text_menufirst), so nothing before it
// moves; reached through one retargeted call each (optFlush's compose and copy here; argui's glyph call and
// credwait's vblank wait by CMake's renames).
#define MENUFIRST_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_menufirst")))
// A vblank wait for the menu's screen came just now, ahead of optFlush's copy (optCopyShown).
static bool optSettled __attribute__((section(".bss.menufirst_late")));

// The open's sound (argui's sndMove) and the menu's choice sound (sndSelect) were not in the SFX cache until their first
// play: that play queued a read of the sound on the audio loader thread, whose first read opens the file (54-262 ms an
// open on maintainer's card, its 11-07 log), and the voice started one audio update after the read, held, and was let go
// at the next. Here, at each room load (argui's prefetch wrap, which the first room's load runs behind the loading
// bar), both are asked of the sound plan's copy worker, after the room's own sounds, as the plan asks for the sounds
// it has no time for: the worker copies them in the background as their first play would, and the copies go in only
// if they fit under the cache's cap with the linear floor kept. Nothing is pushed out for them (R420: the preload's
// copy, buildSfx, evicts cached sounds when a fragmented linear allocation fails, a room's kept 1 MB sound among
// them). No room, or a read already on its way: nothing done, and the first open loads its sound as it did before.
// Cached already: nothing read. Its keep list (flowend) then holds them for the room when it has room after the plan.
int CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib, uint32_t floorBytes);
#define MENUFIRST_SOUND_KIB 64u                    // the worker's upper bound for one sound (both are a few KB)
#define MENUFIRST_FLOOR_BYTES (3u * 1024u * 1024u) // linear memory left free: the plan's worker floor (one shared value)
// R420b's review: a first request starts the worker, whose 32 KiB read buffer (UTSFX_PIECE) is taken before any floor
// check. So these optional requests go only while that buffer fits above the floor too, whether or not the worker is
// already running (from here that can't be told; with it running, this is only a little stricter than needed).
#define MENUFIRST_PIECE_BYTES (32u * 1024u)
void CtrOptmenu_prepareRoom(void); void* CtrMenuborder_copy(void* to, const void* from, size_t bytes);
MENUFIRST_CODE void CtrOptmenu_prepareRoom(void) {
    CtrOptmenu_prepareGlyphs();
    if (MENUFIRST_FLOOR_BYTES + MENUFIRST_PIECE_BYTES > linearSpaceFree()) return;
    for (uint32_t i = 0; 2u > i; i++) {
        if (g.assetIds[i] >= 0) (void) CtrAudioSystem_queueSfx(g.assetIds[i], MENUFIRST_SOUND_KIB, MENUFIRST_FLOOR_BYTES);
    }
}

// Whether a buffer is the one the menu's screen scans now: that screen's framebuffer registers (0x68 buffer A, 0x6C
// buffer B, 0x78 bit 0 which one shows; the bottom's at 0x400568, the top's left eye at 0x400468), as dlgblank reads
// them. Unreadable: no.
MENUFIRST_CODE static bool optScanned(const uint16_t* buffer) {
    u32 regs[5];
    if (R_FAILED(GSPGPU_ReadHWRegs(opt.scr == GFX_TOP ? 0x400468u : 0x400568u, regs, sizeof(regs)))) return false;
    return osConvertVirtToPhys(buffer) == ((regs[4] & 1u) != 0 ? regs[1] : regs[0]);
}

// optFlush's compose. The page is drawn in the screen's spare buffer, then copied onto the one shown. At the open, the
// game's last frame may have just flipped the screen (the dialogue box's transfer: the GPU's finish callback swaps, and
// GSP shows the swap at that screen's next vblank), so the spare buffer is still on the LCD and the 10+ ms compose was
// scanned there: the frame and the first rows first. One vblank's wait lets the flip land first. Then the copy: once
// the compose is done, wait for the screen's next vblank, so that optFlush's memcpy, its next work, runs from column 0
// ahead of the scan (crispgap's rule, docs/3ds-hardware.md; credwait does the same for a moved Legal or Credits page),
// not over a scan already part way across. The page shows at the same refresh as before, now whole.
MENUFIRST_CODE static void optComposeShown(const CtrMenu* menu, uint16_t* fb) {
    GSPGPU_Event vblank = opt.scr == GFX_TOP ? GSPGPU_EVENT_VBlank0 : GSPGPU_EVENT_VBlank1;
    if (optScanned(fb - (opt.scr == GFX_TOP ? OPT_TOP_SIDE : 0u))) gspWaitForEvent(vblank, true);
    optCompose(menu, fb);
    gspWaitForEvent(vblank, true);
    optSettled = true;
}

// The Controls, Legal and Credits pages (R420): printed through the console into the spare buffer and copied by
// optFlush every frame, over a scan part way across when the page has changed (a press, a held key's repeat, the
// Credits' roll). Copied here: at the menu screen's vblank, as the Options page is, unless a wait for it came just
// before (optComposeShown's, or credwait's for a moved Legal or Credits page), so no frame waits twice. One wait a
// frame at most, under the console's lock as the Options page's; a paused frame's slot is two vblanks, as credwait
// counts on for its own wait every frame of a held scroll.
MENUFIRST_CODE static void* optCopyShown(void* to, const void* from, size_t bytes) {
    if (!optSettled) gspWaitForEvent(opt.scr == GFX_TOP ? GSPGPU_EVENT_VBlank0 : GSPGPU_EVENT_VBlank1, true);
    optSettled = false;
    return CtrMenuborder_copy(to, from, bytes);
}

// The frame's flush (fpsboxMenuFlush's call, the menu's one flush a frame, at the open too): the note lives only from
// a wait to the end of its own frame's flush (s8bxrev L1). A frame whose flush copies nothing (the menu closing, a
// screen not placed) would otherwise leave credwait's note for a later frame's copy, which then would not wait.
MENUFIRST_CODE static void optFlushFrame(CtrMenu* menu) {
    optFlush(menu);
    optSettled = false;
}

// credwait's wait (z_credclip.c's one call, retargeted by CMake's rename): the wait, then a note that it came.
void CtrOptmenu_credWait(GSPGPU_Event id, bool nextEvent);
MENUFIRST_CODE void CtrOptmenu_credWait(GSPGPU_Event id, bool nextEvent) {
    gspWaitForEvent(id, nextEvent);
    optSettled = true;
}
#endif

#if defined(ENABLE_WAD16)
#define MENUBORDER_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_menuborder")))
// Physical-bottom menu counters retain their (-1,+1) offset. Play and top counters stay at y 2.
void CtrMenuborder_fps(int32_t x, int32_t y, const char* text, uint32_t colour);
MENUBORDER_CODE void CtrMenuborder_fps(int32_t x, int32_t y, const char* text, uint32_t colour) {
    bool bottom = (CtrScreens_game ^ g.rgb565) == GFX_BOTTOM;
    int32_t by = 0;
    void* screen = g.fb;
    uint16_t* page = opt.spareBase;
    if (bottom && g.rgb565 && opt.menu != nullptr && opt.menu->open && opt.placed && opt.scr == GFX_BOTTOM) {
        by = (317 - 11 * (int32_t) g.sx - x) * 240;
        uint8_t* old = fps75Rect[CtrScreens_game ^ g.rgb565];
        if (old[2] > x + 11 * (int32_t) g.sx) old[2] = old[3] = 0;
        g.fb = (uint16_t*) screen + by;
        opt.spareBase = page + by;
    }
    __wrap_CtrFpsbox_drawDigits(x, y + (by != 0), text, colour);
    if (by != 0) {
        g.fb = screen;
        opt.spareBase = page;
    }
}
#endif

// maintainer 11-08: leaving a page or closing the menu is silent. Decide before a page's early draw/read hook can sound
// the press. Only the pending sound is cleared: keys, actions, settings, drawing and the menu-open sound stay intact.
// Appended after every existing late input; optKeys spends its original sound-call padding instead of growing.
// The assembler-name declaration above hides the new call-graph edge from GCC's function ordering; otherwise
// optKeys moves to the end of its existing section despite keeping its size. The release byte audit pins this.
bool CtrCtrlui_holdMoves(u32 key);
__attribute__((noinline, noipa, optimize("Os"), section(".text_menuquiet")))
void CtrMenuquiet_press(CtrMenu* menu, u32 down) {
    u32 folded = CTR_MENU_FOLD(down);
    bool back = (down & (KEY_B | KEY_SELECT)) != 0;
    if (!back && (folded & (KEY_A | KEY_DUP | KEY_DDOWN)) == KEY_A) {
        if (CtrLegal_shown()) {
            // CtrLegalui_mark: low two bits are the view (0 hub), next two the row (2 Back).
            back = (optPress.legalMark & 0xFu) == 8u;
        } else if (folded & (KEY_DLEFT | KEY_DRIGHT)) {
            return; // outside the Legal hub a direction wins over A
        } else if (menu->page == MENU_PAGE_CONTROLS) {
            // Back is the Controls list's last row: Down cannot repeat from it.
            back = !CtrCtrlui_holdMoves(KEY_DDOWN);
        } else if (opt.page == OPT_PAGE_MAIN) {
            back = opt.cur[OPT_PAGE_MAIN] == OPT_MAIN_RESUME;
        } else {
            back = opt.cur[opt.page] == optCounts[opt.page] - 1u;
        }
    }
    if (back) optPress.keys = 0;
}

// Quit owns the next picture, rather than repainting its disarmed row and
// clearing only the menu. The two old callers keep their instructions/slots;
// these adapters are at the late tail. No new state: cursor is unused by this
// menu's main page, and -1 is reserved only after confirmed Quit (never Resume).
#ifdef ENABLE_WAD16
#define QUITBLACK_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_quitblack")))
QUITBLACK_CODE static CtrMenuAction quitblackKeys(CtrMenu* menu, u32 down) {
    CtrMenuAction action = optKeysHeard(menu, down);
    if (action == MENU_ACTION_QUIT) menu->cursor = -1;
    return action;
}

u8* CtrQuitblack_present(CtrTextureCache* cache);
QUITBLACK_CODE static void quitblackFlush(CtrMenu* menu) {
    if (menu->cursor == -1) {
        u8* page = CtrQuitblack_present(g.texCache);
        if (page != nullptr) {
            // Close must clear only the black spare we just queued, never the
            // still-scanned old menu. Its RGB565 console remains single-buffered.
            opt.restore = (u16*) page + (opt.scr == GFX_TOP ? OPT_TOP_SIDE : 0u);
            menu->console->frameBuffer = opt.restore;
            CtrOptmenu_onTop = 0; // skip optLeaveTop's early scanned-buffer clear
            opt.dirty = false;
            optSettled = false;
            return;
        }
    }
    fpsboxMenuFlush(menu);
}
#endif

#if defined(ENABLE_WAD16)
// menutime (change scanout, ethanlog5 row 13, 11-08; UNDERTALE only): whether this press closes the menu with Resume, as
// optKeysHeard, optKeys and ctr_ctrlui.c decide it: SELECT on the Options or Controls page, B on the Options page's
// main list, or A alone on its Resume row (with no Legal or Credits page shown). z_menutime.c asks before main.c's update, and then skips
// the page's redraw for that press: optKeys marks the page changed at every press, so the press that closed the menu
// drew the page again (and waited for two vblanks) just before the close cleared it. Reads only; its own section,
// appended last (late_reserve.ld), so no other code moves.
#if defined(__3DS__) && defined(__GNUC__)
#define MENUTIME_OPT_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_menutime_opt")))
#else
#define MENUTIME_OPT_CODE
#endif
bool CtrOptmenu_pressResumes(const CtrMenu* menu, u32 down);
MENUTIME_OPT_CODE bool CtrOptmenu_pressResumes(const CtrMenu* menu, u32 down) {
#ifdef CTRPORT_PLAYER_SHOTS
    down |= opt.pendingKeys; // the test probe's keys, merged at the update's start
#endif
    if (down == 0 || !menu->open || CtrLegal_shown()) return false;
    if (down & KEY_SELECT) return true; // both pages (CtrCtrlui_update's first test on the Controls page)
    if (menu->page != MENU_PAGE_OPTIONS) return false;
    if (opt.page != OPT_PAGE_MAIN) return false;
    if (down & KEY_B) return true;
    return (down & KEY_A) != 0 && (CTR_MENU_FOLD(down) & 0xF0u) == 0 && opt.cur[OPT_PAGE_MAIN] == OPT_MAIN_RESUME;
}

// z_menutime.c's recovery when that guess was wrong (the press did not close the menu): the update's flush alone, as
// __wrap_CtrArgui_menuUpdate ends, with no keys and no held-key or Credits tick, so the frame's page is drawn once.
void CtrOptmenu_flushPage(CtrMenu* menu);
MENUTIME_OPT_CODE void CtrOptmenu_flushPage(CtrMenu* menu) {
    quitblackFlush(menu);
}
#endif

#if defined(ENABLE_WAD16) && defined(__3DS__)
// battletouch's main-page rows (z_battletouch.c, "Options, Battle layout and Battle stylus"): reached through
// builderx_compile.py's routing, they read this file's OptRow and the first fields of OptState at these offsets, and
// place their rows among these indices. Checks only, at the file's end: no code or line number before them changes.
_Static_assert(__builtin_offsetof(OptRow, dis) == 12 && __builtin_offsetof(OptState, page) == 16 &&
               __builtin_offsetof(OptState, cur) == 17 && __builtin_offsetof(OptState, dirty) == 24 &&
               OPT_PAGE_MAIN == 0 && OPT_PAGES == 4, "z_battletouch.c's ObRow and ObState");
#ifdef CTRPORT_TARGET_O3DS
_Static_assert(OPT_MAIN_CONTROLS == 4 && OPT_MAIN_COUNT == 10, "z_battletouch.c's obOrder (Old)");
#else
_Static_assert(OPT_MAIN_CONTROLS == 5 && OPT_MAIN_COUNT == 11, "z_battletouch.c's obOrder (New)");
#endif
#endif

// bottomframe: a virtual main-page row after Dialogue box. Compose with
// battletouch's released rows, retaining its navigation and setting handlers.
#ifdef ENABLE_WAD16
#ifndef CTRPORT_NO_BOTTOMFRAME
#include "ctr_bottomframe.h"
#define BF_MENU __attribute__((noinline, noipa, optimize("Os"), section(".aacout.text.bottomframe")))
#define BF_MENU_RO __attribute__((section(".aacout.rodata.bottomframe")))
static const char bfMenuLabel[] BF_MENU_RO = "Free bottom screen";
static const char bfMenuFrame[] BF_MENU_RO = "Frame";
static const char bfMenuDim[] BF_MENU_RO = "Dim";
static const char bfMenuHelp[] BF_MENU_RO = "Area frame or dim when unused.";
#ifdef CTRPORT_TARGET_O3DS
static const uint8_t bfMenuOldY[] BF_MENU_RO = {8,22,42,56,76,90,104,124,138,152};
#else
static const uint8_t bfMenuOldY[] BF_MENU_RO = {8,22,41,55,69,88,102,116,135,149,163};
#endif

CtrMenuAction CtrOptbattle_keys(CtrMenu*, u32);
void CtrOptbattle_compose(const CtrMenu*, uint16_t*);
// Opaque original-row/text aliases keep GCC's existing local graph intact.
void bfMenuOriginalRow(const CtrMenu*, uint32_t, uint32_t, OptRow*) __asm__("optRow");
void bfMenuOriginalText(uint16_t*, int32_t, int32_t, const char*, uint32_t) __asm__("optText");

BF_MENU CtrMenuAction CtrBottomframe_menuKeys(CtrMenu* menu, u32 down) {
    if (CtrLegal_shown() || menu->page != MENU_PAGE_OPTIONS || opt.page != OPT_PAGE_MAIN ||
        (down & (KEY_B | KEY_SELECT))) return CtrOptbattle_keys(menu, down);
    if (CtrScreens_game != 0 && opt.cur[OPT_PAGE_MAIN] == OPT_MAIN_COUNT)
        opt.cur[OPT_PAGE_MAIN] = OPT_MAIN_GAME;
    u32 folded = CTR_MENU_FOLD(down);
    uint32_t i = opt.cur[OPT_PAGE_MAIN];
    if (folded & (KEY_DUP | KEY_DDOWN)) {
        if (i == OPT_MAIN_COUNT) {
            // Start beside ours, then let battletouch skip disabled rows.
            opt.cur[OPT_PAGE_MAIN] = (folded & KEY_DUP) ? OPT_MAIN_SMOOTHING : OPT_MAIN_DIALOGUE;
            return CtrOptbattle_keys(menu, down);
        }
        bool enter = (folded & KEY_DUP) ? i == OPT_MAIN_SMOOTHING : i == OPT_MAIN_DIALOGUE;
        if (!(folded & KEY_DUP) && i == OPT_MAIN_GAME) {
            OptRow row;
            bfMenuOriginalRow(menu, OPT_PAGE_MAIN, OPT_MAIN_DIALOGUE, &row);
            enter = row.dis;
        }
        if (!enter || CtrScreens_game != 0) return CtrOptbattle_keys(menu, down);
        CtrMenuquiet_beforeKeys(menu, down);
        opt.cur[OPT_PAGE_MAIN] = OPT_MAIN_COUNT;
        opt.dirty = true;
        return MENU_ACTION_NONE;
    }
    if (i != OPT_MAIN_COUNT) return CtrOptbattle_keys(menu, down);
    CtrMenuquiet_beforeKeys(menu, down);
    opt.dirty = true;
    if (CtrScreens_game != 0 || (!(folded & (KEY_DLEFT | KEY_DRIGHT)) && !(down & KEY_A))) return MENU_ACTION_NONE;
    CtrBottomframe_setEnabled(!CtrBottomframe_enabled());
    menu->edited = true;
    return MENU_ACTION_CHANGED;
}
BF_MENU void CtrBottomframe_menuCompose(const CtrMenu* menu, uint16_t* fb) {
    if (opt.page == OPT_PAGE_MAIN && CtrScreens_game != 0 && opt.cur[OPT_PAGE_MAIN] == OPT_MAIN_COUNT)
        opt.cur[OPT_PAGE_MAIN] = OPT_MAIN_GAME;
    CtrOptbattle_compose(menu, fb);
    if (opt.page != OPT_PAGE_MAIN) return;
    bool selected = CtrScreens_game == 0 && opt.cur[OPT_PAGE_MAIN] == OPT_MAIN_COUNT;
    // Repack the already composed 12 px rows into a 14 px list. One column
    // on the paused menu's stack avoids another full-screen buffer. Resume,
    // Quit and the help line keep their released positions. Battle row text,
    // choices, disabled colours and selection remain battletouch's own.
    for (uint32_t x = 3; x < 317; ++x) {
        uint16_t column[174];
        for (uint32_t y = 8; y < 182; ++y) {
            column[y - 8] = fb[x * 240 + 239 - y];
            fb[x * 240 + 239 - y] = 0;
        }
        for (uint32_t k = 0; k < sizeof(bfMenuOldY); ++k) {
            uint32_t y = 8 + 14 * (k + (k >= 2));
            for (uint32_t row = 0; row < 12; ++row) {
                uint16_t colour = column[bfMenuOldY[k] + row - 8];
                // battletouch falls back to Game for our virtual cursor.
                if (selected && k == 0) {
                    if (x < 24) colour = 0;
                    else if (colour == 0xffe0u) colour = 0xffffu;
                }
                fb[x * 240 + 239 - y - row] = colour;
            }
        }
        if (selected)
            for (uint32_t y = 221; y < 235; ++y) fb[x * 240 + 239 - y] = 0;
    }
    uint32_t colour = CtrScreens_game != 0 ? 0x8410u : (selected ? 0xffe0u : 0xffffu);
    bfMenuOriginalText(fb, 24, 36, bfMenuLabel, colour);
    bfMenuOriginalText(fb, 164, 36, CtrBottomframe_enabled() ? bfMenuFrame : bfMenuDim,
                       colour);
    if (selected) {
        bfMenuOriginalText(fb, 11, 36, omStrHeart, 0xf800u);
        bfMenuOriginalText(fb, 12, 221, bfMenuHelp, 0xffffu);
    }
}
#endif

#endif // CTRPORT_NO_BOTTOMFRAME
#if defined(ENABLE_WAD16) && defined(__3DS__)
// battletouch's first-battle hint (z_battletouch.c) draws from the glyph table: a second name for optGlyphs, no code.
__asm__(".global CtrOptmenu_glyphs\n.set CtrOptmenu_glyphs, optGlyphs\n");
#endif

// Accepted menu actions only: the composed page does not cover the top
// menu's left margin. Retire its counter when it stops being painted there.
// The game gets a clean transfer; the bottom menu gets a clean page copy.
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
static CtrMenuAction fpsghostActivate(CtrMenu* menu, uint32_t row, int32_t dir) {
    uint32_t before = gFpsScreen;
    CtrMenuAction action = optActivate(menu, row, dir);
    if ((before == 0u) != (gFpsScreen == 0u)) CtrFpsbox_reset();
    if (before == 1u && gFpsScreen != 1u && opt.placed && opt.scr == GFX_TOP) {
        CtrLog_beginConsole();
        CtrFpsbox_restoreMenu(opt.shownBase, opt.spareBase, true);
        // Include the whole left margin in one cache operation, never the
        // page or hint. This LCD remains in the menu's single-buffer mode.
        GSPGPU_FlushDataCache(opt.shownBase - OPT_TOP_SIDE, OPT_TOP_SIDE * 2u);
        CtrLog_endConsole();
    }
    return action;
}

#ifdef CTR_FPSGHOST_DIAG
// Keep the old menu bodies and their embedded constants in their old slots.
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
static void fpsghostOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    CTR_MENUOPEN_AFTER_OPTMENU(menu, console, settings, caps);
    CtrFpsdiag_event(1);
}

__attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
static void fpsghostClose(CtrMenu* menu) {
    CTR_MENUCLOSE_AFTER_OPTMENU(menu);
    CtrFpsdiag_event(2);
}

// A separate TU keeps argui's original function emission order intact.
void __wrap_arguiDrawText(int32_t x, int32_t y, const char* text, uint32_t colour);
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.fpsghost")))
void CtrFpsdiag_draw(int32_t x, int32_t y, const char* text, uint32_t colour) {
    CtrFpsdiag_paint((uint32_t) (CtrScreens_game ^ g.rgb565), g.fb, text);
    __wrap_arguiDrawText(x, y, text, colour);
}
#endif
