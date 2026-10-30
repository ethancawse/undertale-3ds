// The game screen and other screen settings, as late code: the settings file's game_screen, the
// other screen read from bottom_mode (or other_screen) with the retired magnifier loading as Off, and the options
// menu's Game screen and Other screen rows. The renderer's part (moving the game between the screens) is at the end
// of ctr_renderer.c, the pacer's at the end of ctr_frame_pacer.c, the consoles' at the end of main.c.
//
// Disconnect, don't delete: main's settings and menu code stays compiled where
// it was, at its size, and the link sends its callers here instead (-Wl,--wrap for CtrDisplay_load,
// CtrDisplay_override, CtrDisplay_saveEdits, CtrMenu_open and CtrMenu_update in CMakeLists.txt). The magnifier can no
// longer be chosen: the menu steps Off and Log only, and a saved or given 0 loads as Off. Deleting the dead code is a
// later compaction round with a re-pin, not this change. Everything here is in the file's late sections
// (platform/3ds/late_reserve.ld, "screens"); every string is a named array there, and this file is compiled without
// the printf builtins and switch tables (CMakeLists.txt), which would add constants to .rodata and move it.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "ctr_display.h"
// CTR_SCREENS_SETTINGS_ONLY: the settings alone, for the host test (tools/analysis/test_display_settings.py).
#ifndef CTR_SCREENS_SETTINGS_ONLY
#include "ctr_log.h"
#include "ctr_menu.h"
#include "ctr_release_branding.h"
#include "ctr_texture_pack.h"
#endif

#if defined(__3DS__) && defined(__GNUC__)
#define LATE __attribute__((noinline, section(".text.late.screens")))
// The constants sit in the late code area, after the code (late_reserve.ld): the late constants' reserve is a fixed
// page whose room depends on the target (1458 B left on UT n3ds when this was written), and these are 2 KB.
#define LATE_RO __attribute__((section(".rodata.screens_in_text")))
#define LATE_BSS __attribute__((section(".bss.late.screens")))
#else
#define LATE
#define LATE_RO
#define LATE_BSS
#endif
// A string constant in the late constants, its size rounded up to whole words.
#define LATE_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] LATE_RO = text

// ===[ State, shared with the late code in ctr_renderer.c, ctr_frame_pacer.c and main.c ]===

// Which screen shows the game, as the settings ask: 0 top (the default), 1 bottom (game_screen in the file).
uint8_t CtrScreens_game LATE_BSS;
// CtrScreens_game when the options menu opened, so saving knows whether the menu changed it.
uint8_t CtrScreens_gameAtOpen LATE_BSS;
// The pacer's game screen (gfxScreen_t; GFX_TOP until CtrPacer_setGameScreen).
uint8_t CtrScreens_pacerGame LATE_BSS;
// The renderer: the game shows on the bottom screen; the top backlight is off (Off with the game on the bottom).
bool CtrScreens_onBottom LATE_BSS;
bool CtrScreens_topBacklightOff LATE_BSS;
// The renderer: other_screen is Off, as the caller of CtrRenderer_setDisplay asks.
bool CtrScreens_otherOff LATE_BSS;
// main.c: the stats and log consoles are on the top screen.
bool CtrScreens_consolesOnTop LATE_BSS;
// A --ini game_screen line set CtrScreens_game: a test of the game on the bottom screen (tests otherwise run with the
// game on top, CtrScreens_gameBelow).
bool CtrScreens_gameGiven LATE_BSS;
// The two screens share one backlight, or CFGU couldn't say they don't (the original 2DS): no backlight goes off,
// Off blanks the screen instead (CtrScreens_backlightShared, from CtrRenderer_create).
bool CtrScreens_sharedBacklight LATE_BSS;
// The renderer: "Screenshot saved" is painted on the top screen's consoles (the game on the bottom screen).
bool CtrScreens_noticeShown LATE_BSS;
#ifndef CTR_SCREENS_SETTINGS_ONLY
aptHookCookie CtrScreens_aptCookie LATE_BSS;
#endif

// ===[ Settings ]===

LATE_STR(keyFormat, " %63[^= ] = %d");
LATE_STR(gameKey, "game_screen");
LATE_STR(otherKey, "other_screen");
LATE_STR(bottomKey, "bottom_mode");
LATE_STR(readMode, "r");
LATE_STR(appendMode, "a");
LATE_STR(gameLine, "game_screen = %d\n");

void __real_CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
void __real_CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
bool __real_CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __wrap_CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool __wrap_CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __wrap_CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
// main.c (late): this run was started for a test (--bench, or benchrun.txt on the card).
bool CtrScreens_benchLaunched(void);
bool CtrBench_running(void);
bool CtrScreens_gameBelow(void);
bool CtrScreens_backlightShared(CtrDisplayCaps caps);
const char* __real_CtrDisplay_topModeName(CtrTopMode mode);
const char* __real_CtrDisplay_stereoModeName(CtrStereoMode mode);
const char* __real_CtrDisplay_bottomModeName(CtrBottomMode mode);
const char* __wrap_CtrDisplay_topModeName(CtrTopMode mode);
const char* __wrap_CtrDisplay_stereoModeName(CtrStereoMode mode);
const char* __wrap_CtrDisplay_bottomModeName(CtrBottomMode mode);

enum { LINE_OTHER = 1, LINE_GAME };

// One of this file's settings: game_screen 0-1 (LINE_GAME), or other_screen 0-2 (LINE_OTHER; bottom_mode's numbers,
// 0, the retired magnifier, is Off). 0 if line isn't one of them; *known is false if the value is out of range.
LATE CTR_COLD_OS static int screensLine(CtrDisplaySettings* settings, const char* line, uint8_t* game, bool* known) {
    char key[64];
    int value;
    if (sscanf(line, keyFormat, key, &value) != 2) return 0;
    if (strcmp(key, gameKey) == 0) {
        *known = 0 <= value && 1 >= value;
        if (*known) *game = (uint8_t) value;
        return LINE_GAME;
    }
    if (strcmp(key, otherKey) == 0) {
        *known = 0 <= value && BOTTOM_MODE_COUNT > value;
        if (*known) settings->bottomMode = value == BOTTOM_MODE_MAGNIFIER ? BOTTOM_MODE_OFF : (CtrBottomMode) value;
        return LINE_OTHER;
    }
    return 0;
}

// A bottom_mode line, read again so that the later of it and other_screen wins, as on screens 9a02e086.
LATE CTR_COLD_OS static void screensBottomLine(CtrDisplaySettings* settings, const char* line) {
    char key[64];
    int value;
    if (sscanf(line, keyFormat, key, &value) != 2 || strcmp(key, bottomKey) != 0) return;
    if (0 <= value && BOTTOM_MODE_COUNT > value) settings->bottomMode = (CtrBottomMode) value;
}

// CtrDisplay_load, then the game screen (top unless the file says), the other_screen name (the later of it and
// bottom_mode wins), and the magnifier as Off.
LATE CTR_COLD_OS static void screensLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps, uint8_t* game) {
    void benchfidLoad(CtrDisplaySettings*, const char*, CtrDisplayCaps); benchfidLoad(settings, path, caps);
    *game = 0;
    FILE* f = fopen(path, readMode);
    if (f != nullptr) {
        char line[128];
        bool known;
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (screensLine(settings, line, game, &known) == 0) screensBottomLine(settings, line);
        }
        fclose(f);
    }
    if (settings->bottomMode == BOTTOM_MODE_MAGNIFIER) settings->bottomMode = BOTTOM_MODE_OFF;
}

// A test run (--bench or benchrun.txt; main.c) shows the game on top whatever the file says, so a saved game_screen
// never changes what a test measures; --ini game_screen=1, applied after this, still moves it (a test of that mode).
LATE void __wrap_CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    uint8_t game;
    screensLoad(settings, path, caps, &game);
    if (CtrScreens_benchLaunched()) game = 0;
    CtrScreens_game = game;
}

LATE bool __wrap_CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    uint8_t game = CtrScreens_game;
    bool known = false;
    int kind = screensLine(settings, line, &game, &known);
    if (kind != 0) {
        if (kind == LINE_GAME && known) CtrScreens_gameGiven = true;
        CtrScreens_game = game;
        return known;
    }
    bool benchfidOverride(CtrDisplaySettings*, const char*, CtrDisplayCaps); known = benchfidOverride(settings, line, caps);
    if (settings->bottomMode == BOTTOM_MODE_MAGNIFIER) settings->bottomMode = BOTTOM_MODE_OFF;
    return known;
}

// CtrDisplay_saveEdits with the game screen: what the options menu changed, on top of the file's own settings.
LATE void __wrap_CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    CtrDisplaySettings saved;
    uint8_t game;
    // Main's CtrDisplay_saveEdits stays linked where it was (nothing else calls it, and --gc-sections would drop it
    // and move every function after it): disconnected, not deleted, until a compaction round re-pins.
    __asm__ volatile("" : : "r"(__real_CtrDisplay_saveEdits));
    CtrDisplay_defaults(&saved);
    screensLoad(&saved, path, caps, &game);
    // The settings the options menu can change (CtrMenu_update below).
    if (before->topMode != after->topMode) saved.topMode = after->topMode;
    if (before->stereoMode != after->stereoMode) saved.stereoMode = after->stereoMode;
    if (before->smoothing != after->smoothing) saved.smoothing = after->smoothing;
    if (before->drawing != after->drawing) saved.drawing = after->drawing;
    if (before->bottomMode != after->bottomMode) saved.bottomMode = after->bottomMode;
    if (before->dialogueBottom != after->dialogueBottom) saved.dialogueBottom = after->dialogueBottom;
    for (int32_t row = 0; MAP_ROW_COUNT > row; row++) {
        if (before->controls.keys[row] != after->controls.keys[row]) saved.controls.keys[row] = after->controls.keys[row];
    }
    if (before->frameSkip != after->frameSkip) saved.frameSkip = after->frameSkip;
    if (before->exactColours != after->exactColours) saved.exactColours = after->exactColours;
    if (CtrScreens_gameAtOpen != CtrScreens_game) game = CtrScreens_game;
    CtrDisplay_save(&saved, path);
    FILE* f = fopen(path, appendMode);
    if (f != nullptr) {
        fprintf(f, gameLine, (int) game);
        fclose(f);
    }
}

// The game shows on the bottom screen unless the menu or a test has it: game_screen 1, and no test running, unless
// --ini game_screen=1 asked for one with the game there. A test picked from the menu runs with the game on top too.
LATE bool CtrScreens_gameBelow(void) {
    return CtrScreens_game == 1 && (CtrScreens_gameGiven || !CtrBench_running());
}

// The "Display:" log line (main.c) and a test's "display:" line (ctr_benchmark.c) with the game on the bottom screen:
// each screen named by what it shows, "top" the other screen (Off or Stats and log), "bottom" the game, and 3D
// Normal, as the renderer shows it (CtrRenderer_setDisplay). With the game on top they are main's names.
LATE_STR(nameGame, "the game");

LATE const char* __wrap_CtrDisplay_topModeName(CtrTopMode mode) {
    if (!CtrScreens_gameBelow()) return __real_CtrDisplay_topModeName(mode);
    return __real_CtrDisplay_bottomModeName(CtrScreens_otherOff ? BOTTOM_MODE_OFF : BOTTOM_MODE_CONSOLE);
}

LATE const char* __wrap_CtrDisplay_stereoModeName(CtrStereoMode mode) {
    return __real_CtrDisplay_stereoModeName(CtrScreens_gameBelow() ? STEREO_NORMAL : mode);
}

LATE const char* __wrap_CtrDisplay_bottomModeName(CtrBottomMode mode) {
    return CtrScreens_gameBelow() ? nameGame : __real_CtrDisplay_bottomModeName(mode);
}

// Whether the two screens share one backlight (the original 2DS: switching the bottom one off darkens both,
// docs/3ds-hardware.md): CFGU's model is a 2DS (caps.wide false), or GetModelNintendo2DS says 0, or can't be asked.
// Unknown counts as shared, which keeps the backlight lit and blanks the screen instead (o2black a2c8d0d0).
LATE CTR_COLD_OS bool CtrScreens_backlightShared(CtrDisplayCaps caps) {
    if (!caps.wide) return true;
    u8 not2ds = 0;
    bool known = false;
    if (R_SUCCEEDED(cfguInit())) {
        known = R_SUCCEEDED(CFGU_GetModelNintendo2DS(&not2ds));
        cfguExit();
    }
    return !known || not2ds == 0;
}

#ifndef CTR_SCREENS_SETTINGS_ONLY
// ===[ The options menu ]===
//
// main's menu (ctr_menu.c) with the Game screen row first and Other screen for Bottom screen, as on screens
// 9a02e086, and relprep's Legal notices row after Performance test (ctr_legal.c's viewer, with its label and help).
// The Controls page is main's: CtrMenu_update hands it to the real one.

typedef enum {
    ITEM_GAME_SCREEN,
    ITEM_TOP_MODE,
    ITEM_DRAWING,
    ITEM_STEREO,
    ITEM_SMOOTHING,
    ITEM_OTHER_SCREEN,
    ITEM_DIALOGUE,
    ITEM_CONTROLS,
    ITEM_FRAME_SKIP,
    ITEM_EXACT_COLOURS,
    ITEM_PERF_TEST,
    ITEM_LEGAL,
    ITEM_RESUME,
    ITEM_QUIT,
    ITEM_COUNT
} MenuItem;
// main's MenuItem value for the Controls row (ctr_menu.c), for handing the page to the real CtrMenu_update.
#define MAIN_ITEM_CONTROLS 6

// ctr_legal.c: the Legal notices row's texts, and its viewer, which takes the menu's page while it is shown.
extern const char* const ctrLegalLabel;
extern const char* const ctrLegalHelp;
void CtrLegal_open(CtrMenu* menu);
bool CtrLegal_update(CtrMenu* menu, u32 down);
bool CtrLegal_shown(void);
void CtrLegal_close(void);

LATE_STR(labelGame, "Game screen");
LATE_STR(labelTop, "Top screen");
LATE_STR(labelDrawing, "Drawing");
LATE_STR(labelStereo, "3D mode");
LATE_STR(labelSmoothing, "Smoothing");
LATE_STR(labelOther, "Other screen");
LATE_STR(labelDialogue, "Dialogue box");
LATE_STR(labelControls, "Controls");
LATE_STR(labelFrameSkip, "Frame skip");
LATE_STR(labelExact, "Exact colours");
LATE_STR(labelPerf, "Performance test");
LATE_STR(labelResume, "Back to the game");
LATE_STR(labelQuit, "Quit game");
static const char* const screensItemLabels[ITEM_COUNT] LATE_RO = {
    labelGame, labelTop, labelDrawing, labelStereo, labelSmoothing, labelOther, labelDialogue, labelControls, labelFrameSkip,
    labelExact, labelPerf,
    nullptr, // ctrLegalLabel (screensDraw)
    labelResume, labelQuit,
};

// Shown under the list for the selected item; at most two 40-column lines.
LATE_STR(helpGame, "Top: with 3D and wide mode. Bottom:\nthe frame fills the screen, no 3D.");
LATE_STR(helpTop, "Standard: the whole frame at 400x240.\nWide: all 800 columns, sharper, no 3D.");
LATE_STR(helpDrawing, "Native: drawn at the screen's 320x240,\nfaster. Full: 640x480, shrunk to fit.");
LATE_STR(helpNone, "");
LATE_STR(helpOther, "What the screen without the game shows:\noff, or the frame rate and the log.");
LATE_STR(helpDialogue, "Bottom: dialogue outside battles moves\nthere, leaving the scene clear.");
LATE_STR(helpControls, "Which PC key each button sends. SELECT\nalways opens this menu.");
LATE_STR(helpFrameSkip, "Auto: a late frame lets the next go\nundrawn so the game keeps its speed.");
LATE_STR(helpExact, "Unrounded colours; slower, +1/3 on SD.\nApplies at next chapter or restart.");
LATE_STR(helpPerf, "Plays scenes from the game and measures\nthem. Your saves aren't touched.");
LATE_STR(helpResume, "Also SELECT or B.");
LATE_STR(helpQuit, "Progress since your last save is lost.");
static const char* const screensItemHelp[ITEM_COUNT] LATE_RO = {
    helpGame, helpTop, helpDrawing,
    helpNone, // screensStereoHelp, by value
    helpNone, // screensSmoothingHelp, by value
    helpOther, helpDialogue, helpControls, helpFrameSkip, helpExact, helpPerf,
    nullptr, // ctrLegalHelp (screensDraw)
    helpResume, helpQuit,
};

LATE_STR(stereoNormal, "With the 3D slider up, both eyes see\nthe same image.");
LATE_STR(stereoHd, "With the 3D slider up, each eye sees\nhalf the columns of the full image.");
LATE_STR(stereoDepth, "HD, and the room sits behind the screen\nwith text on it. The slider sets depth.");
static const char* const screensStereoHelp[STEREO_MODE_COUNT] LATE_RO = { stereoNormal, stereoHd, stereoDepth };

LATE_STR(smoothOff, "Off: sharp pixels everywhere; small\ntext and fine lines can break up.");
LATE_STR(smoothFull, "Full: every pixel blended. Smooth\ntext, softer pixel art.");
LATE_STR(smoothSmart, "Smart: sharp pixel art, smooth text\nand fine detail.");
static const char* const screensSmoothingHelp[SMOOTHING_COUNT] LATE_RO = { smoothOff, smoothFull, smoothSmart };

LATE_STR(valueTop, "Top");
LATE_STR(valueBottom, "Bottom");
LATE_STR(valueNeedsTop, "(needs the game on top)");
LATE_STR(valueWide, "Wide (800 px)");
LATE_STR(valueStandard, "Standard");
LATE_STR(valueFullWide, "(full in wide mode)");
LATE_STR(valueNo3d, "(no 3D screen)");
LATE_STR(valueOffWide, "(off in wide mode)");
LATE_STR(valueBlank, "Blank");
LATE_STR(valueBottomScreen, "Bottom screen");
LATE_STR(valueTopScreen, "Top screen");
LATE_STR(valueDefault, "Default");
LATE_STR(valueBConfirms, "B confirms");
LATE_STR(valueCustom, "Custom");
LATE_STR(valueNew3ds, "(New 3DS only)");
LATE_STR(valueOnSlower, "On (slower)");
LATE_STR(valueOff, "Off");

LATE_STR(fmtRule, " ---------------------------------------\n");
LATE_STR(fmtBlankRow, "\x1b[K\n");
LATE_STR(colourDim, "\x1b[2m");
LATE_STR(colourSelected, "\x1b[1;33m");
LATE_STR(markSelected, ">");
LATE_STR(markNone, " ");
LATE_STR(fmtItemValue, "%s%s %-14s< %s >\x1b[0m\x1b[K\n");
LATE_STR(fmtItem, "%s%s %-14s%s\x1b[0m\x1b[K\n");
LATE_STR(fmtHelpAt, "\x1b[%d;0H\x1b[2m ");
LATE_STR(fmtHelpBreak, "\x1b[K\n ");
LATE_STR(fmtHelpEnd, "\x1b[0m\x1b[K\n\x1b[K\n");
LATE_STR(fmtFooter, "\x1b[26;0H ---------------------------------------\n Up/Down: choose   Left/Right/A: change\n"
                    " SELECT or B: back to the game\x1b[K");

// Wide mode, 3D and dialogue on the bottom screen need the game on top.
LATE static bool gameOnTop(void) {
    return CtrScreens_game == 0;
}

LATE static bool itemAvailable(const CtrMenu* menu, MenuItem item) {
    if (item == ITEM_TOP_MODE || item == ITEM_DIALOGUE) return gameOnTop();
    if (item == ITEM_DRAWING) return menu->settings.topMode == TOP_MODE_STANDARD || !gameOnTop();
    if (item == ITEM_STEREO) return menu->caps.stereo && menu->settings.topMode == TOP_MODE_STANDARD && gameOnTop();
    if (item == ITEM_EXACT_COLOURS) return CTR_PACK_EXACT_AVAILABLE;
    return true;
}

LATE static const char* itemValue(const CtrMenu* menu, MenuItem item) {
    const CtrDisplaySettings* s = &menu->settings;
    if (item == ITEM_GAME_SCREEN) return gameOnTop() ? valueTop : valueBottom;
    if (item == ITEM_TOP_MODE) {
        if (!gameOnTop()) return valueNeedsTop;
        return s->topMode == TOP_MODE_WIDE ? valueWide : valueStandard;
    }
    if (item == ITEM_DRAWING) {
        if (s->topMode != TOP_MODE_STANDARD && gameOnTop()) return valueFullWide;
        return CtrDisplay_drawingName(s->drawing);
    }
    if (item == ITEM_STEREO) {
        if (!menu->caps.stereo) return valueNo3d;
        if (!gameOnTop()) return valueNeedsTop;
        if (s->topMode != TOP_MODE_STANDARD) return valueOffWide;
        return __real_CtrDisplay_stereoModeName(s->stereoMode);
    }
    if (item == ITEM_SMOOTHING) return CtrDisplay_smoothingName(s->smoothing);
    if (item == ITEM_OTHER_SCREEN) {
        // The original 2DS has one backlight for both screens: Off blanks the screen instead.
        if (s->bottomMode != BOTTOM_MODE_CONSOLE && CtrScreens_sharedBacklight) return valueBlank;
        return __real_CtrDisplay_bottomModeName(s->bottomMode == BOTTOM_MODE_CONSOLE ? BOTTOM_MODE_CONSOLE : BOTTOM_MODE_OFF);
    }
    if (item == ITEM_DIALOGUE) {
        if (!gameOnTop()) return valueNeedsTop;
        return s->dialogueBottom ? valueBottomScreen : valueTopScreen;
    }
    if (item == ITEM_CONTROLS) {
        if (CtrInputMap_isDefault(&s->controls)) return valueDefault;
        CtrInputMap swapped;
        CtrInputMap_defaults(&swapped);
        CtrInputMap_swapAB(&swapped);
        return CtrInputMap_equal(&s->controls, &swapped) ? valueBConfirms : valueCustom;
    }
    if (item == ITEM_FRAME_SKIP) return CtrDisplay_frameSkipName(s->frameSkip);
    if (item == ITEM_EXACT_COLOURS) {
        if (!CTR_PACK_EXACT_AVAILABLE) return valueNew3ds;
        return s->exactColours ? valueOnSlower : valueOff;
    }
    return helpNone;
}

LATE CTR_COLD_OS static void screensDraw(const CtrMenu* menu) {
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    puts(ctrReleaseOptions); // the release's name, as relprep's menu heading (ctr_release_branding.c)
    printf(fmtRule);
    for (int32_t i = 0; ITEM_COUNT > i; i++) {
        bool selected = menu->cursor == i;
        bool available = itemAvailable(menu, (MenuItem) i);
        if (i == ITEM_PERF_TEST) printf(fmtBlankRow);
        const char* colour = !available ? colourDim : selected ? colourSelected : helpNone;
        const char* value = itemValue(menu, (MenuItem) i);
        const char* mark = selected ? markSelected : markNone;
        const char* label = i == ITEM_LEGAL ? ctrLegalLabel : screensItemLabels[i];
        if (value[0] != '\0' && available) printf(fmtItemValue, colour, mark, label, value);
        else printf(fmtItem, colour, mark, label, value);
    }
    // Rows are 1-based: heading, rule, items and their blank row, then a blank row and the help (as relprep's page).
    printf(fmtHelpAt, (int) ITEM_COUNT + 5);
    const char* help = menu->cursor == ITEM_STEREO      ? screensStereoHelp[menu->settings.stereoMode]
                       : menu->cursor == ITEM_SMOOTHING ? screensSmoothingHelp[menu->settings.smoothing]
                       : menu->cursor == ITEM_LEGAL     ? ctrLegalHelp
                                                        : screensItemHelp[menu->cursor];
    for (const char* c = help; *c != '\0'; c++) {
        if (*c == '\n') printf(fmtHelpBreak);
        else putchar(*c);
    }
    printf(fmtHelpEnd);
    printf(fmtFooter);
    CtrLog_endConsole();
}

LATE static void showOptions(CtrMenu* menu) {
    menu->page = MENU_PAGE_OPTIONS;
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    consoleClear();
    CtrLog_endConsole();
    screensDraw(menu);
}

CtrMenuAction __real_CtrMenu_update(CtrMenu* menu, u32 down);
void __real_CtrMenu_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
void __wrap_CtrMenu_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
CtrMenuAction __wrap_CtrMenu_update(CtrMenu* menu, u32 down);

LATE void __wrap_CtrMenu_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    __asm__ volatile("" : : "r"(__real_CtrMenu_open)); // keeps main's linked where it was (saveEdits says why)
    // As relprep's menu did: the scheduled-menu test hook opens it before main's frame turns the log console off, and
    // the log's lines must not print over this page or the legal one.
    CtrLog_setConsoleEnabled(false);
    CtrLegal_close();
    menu->open = true;
    menu->page = MENU_PAGE_OPTIONS;
    menu->cursor = 0;
    menu->controlsCursor = 0;
    menu->settings = *settings;
    // A saved or given magnifier loaded as Off (screensLoad); this keeps a caller's 0 from showing as a choice.
    if (menu->settings.bottomMode == BOTTOM_MODE_MAGNIFIER) menu->settings.bottomMode = BOTTOM_MODE_OFF;
    menu->caps = caps;
    menu->edited = false;
    menu->console = console;
    CtrScreens_gameAtOpen = CtrScreens_game;
    showOptions(menu);
}

// The real CtrMenu_update's Controls page, with main's cursor value for the Controls row while it runs (its options
// page drawing indexes main's tables by the cursor); this menu's options page is drawn again when it comes back.
LATE CTR_COLD_OS static CtrMenuAction mainControls(CtrMenu* menu, u32 down) {
    int32_t cursor = menu->cursor;
    menu->cursor = MAIN_ITEM_CONTROLS;
    CtrMenuAction action = __real_CtrMenu_update(menu, down);
    menu->cursor = cursor;
    if (menu->open && menu->page == MENU_PAGE_OPTIONS) showOptions(menu);
    return action;
}

// Steps the selected setting forwards (dir 1) or backwards (dir -1).
LATE CTR_COLD_OS static bool changeItem(CtrMenu* menu, int32_t dir) {
    CtrDisplaySettings* s = &menu->settings;
    MenuItem item = (MenuItem) menu->cursor;
    if (item == ITEM_GAME_SCREEN) {
        CtrScreens_game = gameOnTop() ? 1 : 0;
        return true;
    }
    if (item == ITEM_TOP_MODE) {
        do {
            s->topMode = (CtrTopMode) (((int32_t) s->topMode + dir + TOP_MODE_COUNT) % TOP_MODE_COUNT);
        } while (s->topMode == TOP_MODE_WIDE && !menu->caps.wide);
        return true;
    }
    if (item == ITEM_STEREO) {
        s->stereoMode = (CtrStereoMode) (((int32_t) s->stereoMode + dir + STEREO_MODE_COUNT) % STEREO_MODE_COUNT);
        return true;
    }
    if (item == ITEM_DRAWING) {
        s->drawing = s->drawing == DRAWING_NATIVE ? DRAWING_FULL : DRAWING_NATIVE;
        return true;
    }
    if (item == ITEM_SMOOTHING) {
        // Off, Smart, Full: Smart sits between the other two.
        if (s->smoothing == SMOOTHING_OFF) s->smoothing = dir > 0 ? SMOOTHING_SMART : SMOOTHING_FULL;
        else if (s->smoothing == SMOOTHING_SMART) s->smoothing = dir > 0 ? SMOOTHING_FULL : SMOOTHING_OFF;
        else s->smoothing = dir > 0 ? SMOOTHING_OFF : SMOOTHING_SMART;
        return true;
    }
    if (item == ITEM_OTHER_SCREEN) {
        // Off and Log; the magnifier is retired.
        s->bottomMode = s->bottomMode == BOTTOM_MODE_CONSOLE ? BOTTOM_MODE_OFF : BOTTOM_MODE_CONSOLE;
        return true;
    }
    if (item == ITEM_DIALOGUE) {
        s->dialogueBottom = !s->dialogueBottom;
        return true;
    }
    if (item == ITEM_FRAME_SKIP) {
        // Off, Auto, 20 fps, 15 fps; 2:1 is set in the file only.
        s->frameSkip = (CtrFrameSkip) (((int32_t) s->frameSkip + dir + FRAME_SKIP_COUNT) % FRAME_SKIP_COUNT);
        if (s->frameSkip == FRAME_SKIP_HALF) s->frameSkip = (CtrFrameSkip) (((int32_t) s->frameSkip + dir + FRAME_SKIP_COUNT) % FRAME_SKIP_COUNT);
        return true;
    }
    if (item == ITEM_EXACT_COLOURS) {
        s->exactColours = !s->exactColours;
        return true;
    }
    return false;
}

LATE CTR_COLD_OS CtrMenuAction __wrap_CtrMenu_update(CtrMenu* menu, u32 down) {
    if (!menu->open) return MENU_ACTION_NONE;
    // The legal viewer has the page (relprep: SELECT there goes back to this page, not to the game).
    if (CtrLegal_update(menu, down)) {
        if (!CtrLegal_shown()) showOptions(menu);
        return MENU_ACTION_NONE;
    }
    if (down & KEY_SELECT) return MENU_ACTION_RESUME;
    if (menu->page == MENU_PAGE_CONTROLS) return mainControls(menu, down);
    if (down & KEY_B) return MENU_ACTION_RESUME;

    CtrMenuAction action = MENU_ACTION_NONE;
    bool redraw = false;
    if (down & (KEY_DUP | KEY_CPAD_UP)) {
        menu->cursor = (menu->cursor + ITEM_COUNT - 1) % ITEM_COUNT;
        redraw = true;
    }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) {
        menu->cursor = (menu->cursor + 1) % ITEM_COUNT;
        redraw = true;
    }
    int32_t dir = 0;
    if (down & (KEY_DRIGHT | KEY_CPAD_RIGHT)) dir = 1;
    if (down & (KEY_DLEFT | KEY_CPAD_LEFT)) dir = -1;
    if (menu->cursor == ITEM_LEGAL && (dir != 0 || (down & KEY_A))) {
        CtrLegal_open(menu);
        return MENU_ACTION_NONE;
    }
    if (menu->cursor == ITEM_CONTROLS && (dir != 0 || (down & KEY_A))) {
        // The real update opens the Controls page from its own Controls row.
        mainControls(menu, KEY_A);
        return MENU_ACTION_NONE;
    }
    if (down & KEY_A) {
        if (menu->cursor == ITEM_PERF_TEST) return MENU_ACTION_PERF_TEST;
        if (menu->cursor == ITEM_RESUME) return MENU_ACTION_RESUME;
        if (menu->cursor == ITEM_QUIT) return MENU_ACTION_QUIT;
        dir = 1;
    }
    if (dir != 0 && itemAvailable(menu, (MenuItem) menu->cursor) && changeItem(menu, dir)) {
        menu->edited = true;
        action = MENU_ACTION_CHANGED;
        redraw = true;
    }
    if (redraw) screensDraw(menu);
    return action;
}
#endif // CTR_SCREENS_SETTINGS_ONLY

// ===[ Backlights across HOME, sleep and exit (menupause) ]===
// "when i hit the home button when the bottom screen is off, it doesnt come back on": a backlight this app powers off
// (Off for the other screen: ctr_renderer.c's applyBottomBacklight and screensTopBacklight) stays off for whatever
// takes the screens next, the HOME Menu or the Homebrew Launcher. Every power-off and power-on goes through these
// wraps (CMakeLists.txt), which keep which screens are dark; the first power-off adds an APT hook that switches them
// back on as HOME, sleep or closing from HOME takes over, and gfxExit's wrap does the same as the app leaves
// (the menu's Quit, SELECT+START, game_end, the error screens). Coming back, the renderer's own hooks clear its
// "off" flags and mark the screens for configuring, so the next frame applies the player's setting again (and its
// power-off comes back through here). gsp::Lcd is opened only for the call, as applyBottomBacklight does: a session
// held across HOME froze the console.
#ifndef CTR_SCREENS_SETTINGS_ONLY
#include "log.h"

#define BACKLIGHT_CODE __attribute__((noinline, section(".text.late.menupause")))
#define BACKLIGHT_BSS __attribute__((section(".bss.late.menupause")))
#define BACKLIGHT_RO __attribute__((section(".rodata.late.menupause")))

Result __real_GSPLCD_PowerOffBacklight(u32 screen);
Result __real_GSPLCD_PowerOnBacklight(u32 screen);
void __real_gfxExit(void);
Result __wrap_GSPLCD_PowerOffBacklight(u32 screen);
Result __wrap_GSPLCD_PowerOnBacklight(u32 screen);
void __wrap_gfxExit(void);

static u32 backlightsDark BACKLIGHT_BSS; // GSPLCD_SCREEN_TOP and GSPLCD_SCREEN_BOTTOM bits this app powered off
static bool backlightHooked BACKLIGHT_BSS;
static aptHookCookie backlightCookie BACKLIGHT_BSS;
static const char backlightLog[44] BACKLIGHT_RO = "Display: backlights %lu on for %s (%08lX)\n";
static const char backlightHome[8] BACKLIGHT_RO = "HOME";
static const char backlightSleep[8] BACKLIGHT_RO = "sleep";
static const char backlightClose[8] BACKLIGHT_RO = "closing";
static const char backlightExit[8] BACKLIGHT_RO = "exit";

BACKLIGHT_CODE static void backlightsOn(const char* why) {
    u32 dark = backlightsDark;
    if (dark == 0) return;
    Result res = gspLcdInit();
    if (R_SUCCEEDED(res)) {
        res = __real_GSPLCD_PowerOnBacklight(dark); // the bits are GSPLCD_SCREEN_TOP, _BOTTOM or _BOTH
        gspLcdExit();
        if (R_SUCCEEDED(res)) backlightsDark = 0;
    }
    logInfo(backlightLog, (unsigned long) dark, why, (unsigned long) res);
}

BACKLIGHT_CODE static void backlightAptEvent(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND) backlightsOn(backlightHome);
    else if (hook == APTHOOK_ONSLEEP) backlightsOn(backlightSleep);
    else if (hook == APTHOOK_ONEXIT) backlightsOn(backlightClose);
}

#ifdef CTRPORT_BACKLIGHT_PROBE
static void backlightProbe(void);
#endif

BACKLIGHT_CODE Result __wrap_GSPLCD_PowerOffBacklight(u32 screen) {
    bool CtrArgui_keepLit(u32 screen);  // ctr_argui.c: the first-boot hint's or FPS counter's screen stays lit
    if (CtrArgui_keepLit(screen)) return __wrap_GSPLCD_PowerOnBacklight(screen);
    Result res = __real_GSPLCD_PowerOffBacklight(screen);
    if (R_SUCCEEDED(res)) {
        backlightsDark |= screen & GSPLCD_SCREEN_BOTH;
        if (!backlightHooked) {
            backlightHooked = true;
            aptHook(&backlightCookie, backlightAptEvent, nullptr);
        }
#ifdef CTRPORT_BACKLIGHT_PROBE
        backlightProbe();
#endif
    }
    return res;
}

#ifdef CTRPORT_BACKLIGHT_PROBE
// Test builds only (a #define before this block): lab consoles can't press HOME, so the first power-off runs this
// hook as HOME, sleep and closing would, each followed by the power-off the next frame's configureScreens makes on
// the way back, about a second apart (under the watchdog's 12 s). The log shows each step's result.
BACKLIGHT_CODE static void backlightProbe(void) {
    static bool running BACKLIGHT_BSS;
    if (running) return;
    running = true;
    static const APT_HookType hooks[3] = {APTHOOK_ONSUSPEND, APTHOOK_ONSLEEP, APTHOOK_ONEXIT};
    u32 dark = backlightsDark;
    logInfo("Display probe: power-off tracked, dark %lu\n", (unsigned long) dark);
    CtrLog_flush();
    for (int i = 0; i < 3; i++) {
        svcSleepThread(1000000000LL);
        backlightAptEvent(hooks[i], nullptr);
        logInfo("Display probe: after hook %d, dark %lu\n", (int) hooks[i], (unsigned long) backlightsDark);
        CtrLog_flush();
        svcSleepThread(1000000000LL);
        Result res = __wrap_GSPLCD_PowerOffBacklight(dark); // gsp::Lcd is open: the caller holds it for this call
        logInfo("Display probe: back, off again (%08lX), dark %lu\n", (unsigned long) res, (unsigned long) backlightsDark);
        CtrLog_flush();
    }
    logInfo("Display probe: done; gfxExit at the exit switches it on again\n");
    CtrLog_flush();
}
#endif

BACKLIGHT_CODE Result __wrap_GSPLCD_PowerOnBacklight(u32 screen) {
    Result res = __real_GSPLCD_PowerOnBacklight(screen);
    if (R_SUCCEEDED(res)) backlightsDark &= ~(screen & GSPLCD_SCREEN_BOTH);
#ifdef CTRPORT_PLAYER_SHOTS
    static const char onLog[32] BACKLIGHT_RO = "Display: backlight %lu on\n"; // the menu's timing
    logInfo(onLog, (unsigned long) screen);
#endif
    return res;
}

BACKLIGHT_CODE void __wrap_gfxExit(void) {
    backlightsOn(backlightExit);
    __real_gfxExit();
}

// ===[ The player's L+SELECT screenshot: debug and development builds only (menupause) ]===
// On an o3DS test build, Rosalina's L+Down+SELECT also made L+SELECT, and the console froze with a capture in flight
// (docs/3ds-hardware.md). So: no player screenshots in release builds. main.c still asks for one on the combo;
// this wrap (CMakeLists.txt) drops it unless the build has CTRPORT_PLAYER_SHOTS (build.sh ... shots). Test captures
// (--autoshot, bench shots) don't come through here.
struct Renderer;
void __wrap_CtrRenderer_requestPlayerScreenshot(struct Renderer* renderer);
#ifdef CTRPORT_PLAYER_SHOTS
void __real_CtrRenderer_requestPlayerScreenshot(struct Renderer* renderer);
void __real_hidScanInput(void);
void __wrap_hidScanInput(void);

// D-pad Down from this long before the combo to this long after it cancels the capture: Rosalina's L+Down+SELECT can
// make L+SELECT a frame or more before Down arrives, and its menu then pauses the app mid-capture.
#define SHOT_DOWN_GUARD_TICKS (SYSCLOCK_ARM11 / 4) // 250 ms
static struct Renderer* shotRenderer BACKLIGHT_BSS;
static u64 shotDue BACKLIGHT_BSS;  // when the held-back capture is asked for, 0: none waiting
static u64 shotDown BACKLIGHT_BSS; // when D-pad Down was last seen held, 0: never
static const char shotHeldText[52] BACKLIGHT_RO = "Screenshot: not taken, D-pad Down held (Rosalina)\n";

// Once per main-loop scan (main.c and the menus' own loops): notes Down, and asks for a held-back capture when it is due.
BACKLIGHT_CODE void __wrap_hidScanInput(void) {
    __real_hidScanInput();
    u64 now = svcGetSystemTick();
    if (hidKeysHeld() & KEY_DDOWN) {
        shotDown = now;
        if (shotDue != 0) {
            shotDue = 0;
            logInfo(shotHeldText);
        }
    } else if (shotDue != 0 && now >= shotDue) {
        shotDue = 0;
        __real_CtrRenderer_requestPlayerScreenshot(shotRenderer);
    }
}

BACKLIGHT_CODE void __wrap_CtrRenderer_requestPlayerScreenshot(struct Renderer* renderer) {
    u64 now = svcGetSystemTick();
    if (shotDown != 0 && now - shotDown < SHOT_DOWN_GUARD_TICKS) {
        logInfo(shotHeldText);
        return;
    }
    if (shotDue != 0) return;
    shotRenderer = renderer;
    shotDue = now + SHOT_DOWN_GUARD_TICKS;
}
#else
void __real_CtrRenderer_requestPlayerScreenshot(struct Renderer* renderer);

// Never asks for the capture. The call the optimizer can't see past keeps the real function linked: with no reference
// --gc-sections drops it, and every function after it in .text moves 32 B.
BACKLIGHT_CODE void __wrap_CtrRenderer_requestPlayerScreenshot(struct Renderer* renderer) {
    bool never = false;
    __asm__("" : "+r"(never));
    if (never) __real_CtrRenderer_requestPlayerScreenshot(renderer);
}
#endif

// For ctr_rosalina.c's watcher thread (rosalina2): whether this app has the bottom backlight off, read atomically.
bool CtrScreens_bottomDark(void);
__attribute__((noinline, section(".text.rosalina2_late"))) bool CtrScreens_bottomDark(void) {
    return (__atomic_load_n(&backlightsDark, __ATOMIC_ACQUIRE) & GSPLCD_SCREEN_BOTTOM) != 0;
}
#endif // CTR_SCREENS_SETTINGS_ONLY
