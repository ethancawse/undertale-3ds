// The options menu's Controls page (change ctrlui, maintainer's pick "2" of three mockups, 2026-11-02), in the menu's own
// look: every 3DS button a row whose value is what it does in the game (left or right, or A, step through the
// choices), then Layout (Default or B confirms; anything else shows Custom) and Back. It replaces ctr_menu.c's
// console-text page, which stays linked but unused (removing it would move main's code).
//
// - Buttons: Nothing, Confirm, Cancel, Menu, Up, Down, Left, Right. The D-pad, Circle Pad and C-stick: Move or
//   Nothing. Confirm is Z (Enter on START, as by default), Cancel X, Menu C; a saved Shift or Ctrl shows as Cancel
//   or Menu, which is what they do. ZL, ZR and the C-stick are grey on an Old 3DS (the cursor passes over them).
// - Layout: right or A step Default, B confirms, Custom, and left steps back; Custom is in the cycle once a custom map
//   has been seen, and comes back unchanged (ctlLayoutCycle). The main page's Controls row does the same on left or
//   right (it did nothing before), and A there opens this page.
// - B or Back: the options page, its cursor on Controls. SELECT: back to the game, as from the menu's other pages.
// - A change that would take the last Confirm, Cancel, Menu or Move (a pad on Move, or all four directions on
//   buttons) off the buttons this model has is refused, the help line saying why (review R117 F1): no layout can
//   leave the game unplayable. A map already short of one (a New 3DS's on an Old) still takes every other change.
//
// Hooked with two call changes in ctr_optmenu.c (optActivate's Controls row calls CtrCtrlui_main, and optKeys hands
// the keys of a page other than its own to CtrCtrlui_update instead of the screens' menu), both functions keeping
// their slots. While the page is up, menu->page is MENU_PAGE_CONTROLS, so optmenu's flush copies the menu screen's
// spare buffer (where this page is drawn, as the legal pages are) to the shown one each frame; a press that changed
// something has the menu's sound (optKeysHeard), and optKeys marks the options page to be drawn again when it comes back.
// Drawn as optmenu's optCompose draws a page: the same frame, rows 14 px apart, label at x 24, value at x 148, the
// heart at x 11, the help line at y 221, yellow for the cursor's row and grey for one that can't change. Glyphs come
// from optmenu's table (CtrOptmenu_text). All code and constants in .text.ctrlui_late / .rodata.ctrlui_in_text and the
// state in .bss.ctrlui_late (late_reserve.ld, last), named outside .text.late.* so no catch-all line takes them.
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ctr_input_map.h"
#include "ctr_log.h"
#include "ctr_menu.h"
#ifdef CTRPORT_PLAYER_SHOTS
#include "log.h"
#endif

#if defined(__3DS__) && defined(__GNUC__)
#pragma GCC optimize("Os")
#endif
#define CTL_CODE __attribute__((noinline, section(".text.ctrlui_late")))
#define CTL_RO __attribute__((section(".rodata.ctrlui_in_text")))
#define CTL_BSS __attribute__((section(".bss.ctrlui_late")))

void CtrOptmenu_text(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col);
CtrMenuAction CtrCtrlui_main(CtrMenu* menu, int32_t dir);
CtrMenuAction CtrCtrlui_update(CtrMenu* menu, u32 down);
static void ctlDrawHeard(const CtrMenu* menu); // the sound, then ctlDraw: at the file's end
static void ctlLayoutCycle(CtrInputMap* map); // Layout's step, Custom kept: at the file's end

enum { CTL_LAYOUT = MAP_ROW_COUNT, CTL_BACK, CTL_ROWS };
#define CTL_WHITE 0xFFFFu
#define CTL_YELLOW 0xFFE0u
#define CTL_GREY 0x8410u
#define CTL_RED 0xF800u

// NUL-separated: each row's label, each job's value, the layouts' values, the help lines.
static const char ctlLabels[] CTL_RO = "A\0B\0X\0Y\0L\0R\0ZL\0ZR\0START\0D-pad\0Circle Pad\0C-stick\0Layout\0Back";
static const char ctlJobs[] CTL_RO =
    "< Nothing >\0< Confirm >\0< Cancel >\0< Menu >\0< Up >\0< Down >\0< Left >\0< Right >\0< Move >";
static const char ctlLayouts[] CTL_RO = "< Default >\0< B confirms >\0< Custom >";
static const char ctlHelp[] CTL_RO = "Left or right: what this button does.\0Left or right: change the layout.    \0"
                                     "Back to the options.\0New 3DS only.\0Keep a Confirm, Cancel, Menu and Move.";
static const char ctlHeart[] CTL_RO = "\x01";
#ifdef CTRPORT_PLAYER_SHOTS
static const char ctlLogDrawn[] CTL_RO = "ctrlui: page drawn off screen in %lu us\n";
#endif
// Each game key's job (ctlJobs' index), and each job's key (Confirm: Z; START's is Enter).
static const uint8_t ctlJob[GAME_KEY_COUNT] CTL_RO = { 0, 1, 2, 3, 1, 4, 5, 6, 7, 2, 3, 8 };
static const uint8_t ctlKey[8] CTL_RO = { GAME_KEY_NONE, GAME_KEY_Z, GAME_KEY_X, GAME_KEY_C,
                                          GAME_KEY_UP, GAME_KEY_DOWN, GAME_KEY_LEFT, GAME_KEY_RIGHT };
// CtrInputMap_defaults' map as a table: the header's inline helpers, emitted out of line here, would land in main's
// text (.text.CtrInputMap_*) and move it.
static const uint8_t ctlDefault[MAP_ROW_COUNT] CTL_RO = { GAME_KEY_Z, GAME_KEY_X, GAME_KEY_C, GAME_KEY_C, 0, 0, 0, 0,
                                                          GAME_KEY_ENTER, GAME_KEY_ARROWS, GAME_KEY_ARROWS, 0 };

#ifdef CTRPORT_PLAYER_SHOTS
// --ini opt_probe=2 (shots builds; optmenu's probe, which opens the menu at frame 300): six ups from the main page's
// first row reach Controls on every target (it is the sixth row from the end, and the cursor wraps), A opens this page,
// four downs reach L, right then left (Confirm, then back), B back, SELECT to the game, which then plays on. Each
// change is undone before the menu closes, so the console's settings.ini keeps what it had. Shots (kind 2) after
// each step; kind 3 after the close. {frames since the open (kind 3: the close), kind 1 key / 2 shot, key}.
typedef struct {
    uint16_t tick;
    uint8_t kind;
    uint8_t key;
} CtlStep;
const CtlStep CtrCtrlui_probe[] CTL_RO = {
    { 5, 2, 0x00 }, { 10, 1, 0x40 }, { 12, 1, 0x40 }, { 14, 1, 0x40 }, { 16, 1, 0x40 }, { 18, 1, 0x40 },
    { 20, 1, 0x40 }, { 22, 2, 0x00 }, { 24, 1, 0x01 }, { 26, 2, 0x00 }, { 28, 1, 0x80 }, { 30, 1, 0x80 },
    { 32, 1, 0x80 }, { 34, 1, 0x80 }, { 36, 2, 0x00 }, { 38, 1, 0x10 }, { 40, 2, 0x00 }, { 42, 1, 0x20 },
    { 44, 2, 0x00 }, { 46, 1, 0x02 }, { 48, 2, 0x00 }, { 50, 1, 0x04 }, { 20, 3, 0x00 }, { 120, 3, 0x00 },
};
const uint8_t CtrCtrlui_probeCount CTL_RO = sizeof(CtrCtrlui_probe) / sizeof(CtrCtrlui_probe[0]);
#endif

static struct {
    uint8_t row;  // the cursor
    bool isNew;   // a New 3DS (ZL, ZR and the C-stick there)
    bool refused; // the last change was refused: the help line says why
} ctl CTL_BSS;

// String i of NUL-separated strings.
CTL_CODE static const char* ctlNth(const char* s, uint32_t i) {
    for (; i != 0; i--) s += strlen(s) + 1;
    return s;
}

CTL_CODE static bool ctlGrey(uint32_t row) {
    return !ctl.isNew && (row == MAP_ROW_ZL || row == MAP_ROW_ZR || row == MAP_ROW_CSTICK);
}

// The jobs no usable button has, of Confirm (bit 1), Cancel (2), Menu (3) and Move (8: a pad on Move, or every
// direction, bits 4 to 7, on some button).
CTL_CODE static uint32_t ctlMissing(const uint8_t* k) {
    uint32_t have = 0;
    for (uint32_t r = 0; MAP_ROW_COUNT > r; r++) {
        if (!ctlGrey(r)) have |= 1u << (GAME_KEY_COUNT > k[r] ? ctlJob[k[r]] : 0u);
    }
    if ((have & 0xF0u) == 0xF0u) have |= 0x100u;
    return ~have & 0x10Eu;
}

// 0 Default, 1 B confirms (A and B swapped), 2 Custom: as the main page's Controls row reads it.
CTL_CODE static uint32_t ctlLayout(const CtrInputMap* map) {
    const uint8_t* k = map->keys;
    if (memcmp(k + 2, ctlDefault + 2, MAP_ROW_COUNT - 2) != 0) return 2u;
    if (k[0] == GAME_KEY_Z && k[1] == GAME_KEY_X) return 0;
    return k[0] == GAME_KEY_X && k[1] == GAME_KEY_Z ? 1u : 2u;
}

// Default to B confirms, anything else to Default. (ctlLayoutCycle's two moves.)
CTL_CODE static void ctlLayoutStep(CtrInputMap* map) {
    bool swap = ctlLayout(map) == 0;
    const uint8_t* from = ctlDefault;
    __asm__("" : "+r"(from)); // else GCC copies the table's bytes into a constant of its own, in main's .rodata
    memcpy(map->keys, from, MAP_ROW_COUNT);
    if (swap) {
        map->keys[MAP_ROW_A] = GAME_KEY_X;
        map->keys[MAP_ROW_B] = GAME_KEY_Z;
    }
}

// A button's next job (dir >= 0) or previous one; a pad's Move or Nothing.
CTL_CODE static void ctlStep(CtrInputMap* map, uint32_t row, int32_t dir) {
    uint32_t key = map->keys[row], job = GAME_KEY_COUNT > key ? ctlJob[key] : 0u;
    if (CtrInputMap_isDirectional((CtrMapRow) row)) {
        map->keys[row] = job == 8u ? GAME_KEY_NONE : GAME_KEY_ARROWS;
        return;
    }
    job = (job + (dir < 0 ? 7u : 1u)) & 7u;
    map->keys[row] = job == 1u && row == MAP_ROW_START ? GAME_KEY_ENTER : ctlKey[job];
}

// The page into the menu console's buffer (the menu screen's spare one while the menu is open: 320 x 240 RGB565,
// column-major), as optCompose draws the options pages.
CTL_CODE static void ctlDraw(const CtrMenu* menu) {
#ifdef CTRPORT_PLAYER_SHOTS
    uint64_t t0 = svcGetSystemTick();
#endif
    uint16_t* fb = (uint16_t*) menu->console->frameBuffer;
#ifdef ENABLE_WAD16
    const uint16_t border = 0xFFFFu;
#else
    const uint16_t border = 0x9D18u;
#endif
    uint16_t* p = fb;
    for (uint32_t x = 0; 320u > x; x++) {
        for (uint32_t y = 0; 240u > y; y++) *p++ = (x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? border : 0u;
    }
    const uint8_t* keys = menu->settings.controls.keys;
    for (uint32_t i = 0; CTL_ROWS > i; i++) {
        int32_t y = MAP_ROW_COUNT > i ? 8 + 14 * (int32_t) i : (i == CTL_LAYOUT ? 182 : 196);
        const char* value;
        const char* help;
        bool grey = MAP_ROW_COUNT > i && ctlGrey(i);
        if (grey) value = help = ctlNth(ctlHelp, 3);
        else if (MAP_ROW_COUNT > i) {
            value = ctlNth(ctlJobs, GAME_KEY_COUNT > keys[i] ? ctlJob[keys[i]] : 0u);
            help = ctlHelp;
        } else if (i == CTL_LAYOUT) {
            value = ctlNth(ctlLayouts, ctlLayout(&menu->settings.controls));
            help = ctlNth(ctlHelp, 1);
        } else {
            value = ctlHeart + 1; // ""
            help = ctlNth(ctlHelp, 2);
        }
        uint32_t col = grey ? CTL_GREY : (i == ctl.row ? CTL_YELLOW : CTL_WHITE);
        CtrOptmenu_text(fb, 24, y, ctlNth(ctlLabels, i), col);
        CtrOptmenu_text(fb, 148, y, value, col);
        if (i == ctl.row) {
            CtrOptmenu_text(fb, 11, y, ctlHeart, CTL_RED);
            CtrOptmenu_text(fb, 12, 221, ctl.refused ? ctlNth(ctlHelp, 4) : help, CTL_WHITE);
        }
    }
#ifdef CTRPORT_PLAYER_SHOTS
    logInfo(ctlLogDrawn, (unsigned long) ((svcGetSystemTick() - t0) / 268u));
#endif
}

// The main page's Controls row (optActivate): A (dir 0) opens the page, left or right switch the layout.
CTL_CODE CtrMenuAction CtrCtrlui_main(CtrMenu* menu, int32_t dir) {
    if (dir != 0) {
        ctlLayoutCycle(&menu->settings.controls);
        menu->edited = true;
        return MENU_ACTION_CHANGED;
    }
    APT_CheckNew3DS(&ctl.isNew);
    ctl.row = 0;
    ctl.refused = false;
    menu->page = MENU_PAGE_CONTROLS;
    ctlDrawHeard(menu);
    return MENU_ACTION_NONE;
}

// The menu's keys while the page is up (optKeys, for any page but its own; down is never 0 here).
CTL_CODE CtrMenuAction CtrCtrlui_update(CtrMenu* menu, u32 down) {
    if (menu->page != MENU_PAGE_CONTROLS) return CtrMenu_update(menu, down); // the screens' menu, as before
    if (down & KEY_SELECT) return MENU_ACTION_RESUME;
    ctl.refused = false;
    if (down & KEY_B) {
        menu->page = MENU_PAGE_OPTIONS; // optmenu draws its page this frame (optKeys marked it)
        return MENU_ACTION_NONE;
    }
    u32 folded = CTR_MENU_FOLD(down); // the Circle Pad and C-stick as the D-pad, as in optKeys
    uint32_t i = ctl.row;
    if (folded & (KEY_DUP | KEY_DDOWN)) {
        do {
            if (folded & KEY_DUP) i = i == 0 ? CTL_ROWS - 1u : i - 1u;
            else i = i + 1u == CTL_ROWS ? 0u : i + 1u;
        } while (MAP_ROW_COUNT > i && ctlGrey(i));
        ctl.row = (uint8_t) i;
        ctlDrawHeard(menu);
        return MENU_ACTION_NONE;
    }
    int32_t dir = (folded & KEY_DRIGHT) ? 1 : ((folded & KEY_DLEFT) ? -1 : 0);
    if (dir == 0 && !(down & KEY_A)) return MENU_ACTION_NONE;
    if (i == CTL_BACK) {
        if (dir == 0) menu->page = MENU_PAGE_OPTIONS;
        return MENU_ACTION_NONE;
    }
    if (MAP_ROW_COUNT > i && ctlGrey(i)) return MENU_ACTION_NONE;
    uint8_t* keys = menu->settings.controls.keys;
    if (i == CTL_LAYOUT) ctlLayoutCycle(&menu->settings.controls);
    else {
        uint32_t was = ctlMissing(keys), old = keys[i];
        ctlStep(&menu->settings.controls, i, dir);
        if (ctlMissing(keys) & ~was) {
            keys[i] = (uint8_t) old; // refused, as optmenu refuses a grey row's change: no sound, no change
            ctl.refused = true;
            ctlDrawHeard(menu);
            return MENU_ACTION_NONE;
        }
    }
    menu->edited = true;
    ctlDrawHeard(menu);
    return MENU_ACTION_CHANGED;
}

// ctr_screens.c's open (__wrap_CtrMenu_open) ends here instead of in its showOptions, which cleared the menu's
// console (libctru's consoleClear prints all 1,200 cells as spaces) and printed the screens' own options page (about
// 900 characters) into the spare buffer, which optmenu's page then covered whole: never shown, and about 30 ms of the
// open's frame on an Old 3DS (o3dsxl, 11-02). What showOptions also did stays: the options page, and stdout on the
// menu's console. (Nothing prints there before clearing it: the legal summary and the close clear it first.)
// Its own section, which late_reserve.ld places after the Controls page's code and constants, so neither moves (75zp
// froze them; c26rev R119).
__attribute__((noinline, section(".text.ctrlui_open"))) void CtrCtrlui_openPage(CtrMenu* menu) {
    menu->page = MENU_PAGE_OPTIONS;
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    CtrLog_endConsole();
}

// Calls retargeted, so that their functions keep their sizes: every draw here but a refusal's follows a change (a new
// row, value, layout or the page itself), and the sound comes before it.
void CtrOptmenu_pressSound(void);
__attribute__((noinline, section(".text_menupress"))) static void ctlDrawHeard(const CtrMenu* menu) {
    if (!ctl.refused) CtrOptmenu_pressSound();
    ctlDraw(menu);
}

// Held Up or Down has a row to go to (optHoldTick): the list's end stops a hold, without a wrap.
bool CtrCtrlui_holdMoves(u32 key);
__attribute__((noinline, section(".text_menuhold"))) bool CtrCtrlui_holdMoves(u32 key) {
    return ctl.row != (key == KEY_DUP ? 0u : CTL_ROWS - 1u);
}

// Layout's step (polish audit, 11-06: as Smoothing does): left or right on Custom used to make it
// Default, and the custom map was gone. Custom now stays in the cycle Default, B confirms, Custom and comes back byte
// for byte. The map is kept here from the moment Layout leaves it, for as long as the game runs: settings.ini holds
// only the map in use, so one left behind on Default or B confirms doesn't outlive a restart. Without a custom map the
// cycle is the two presets, as before. Reached by two retargeted calls, so that neither caller moves (RULES 3adf7ac34).
#if defined(__3DS__) && defined(__GNUC__)
#define CTL_CUSTOM __attribute__((section(".bss.menupress_late")))
#else
#define CTL_CUSTOM
#endif
bool CtrOptmenu_pressLeft(void);
static struct { uint8_t keys[MAP_ROW_COUNT]; bool have; } ctlCustom CTL_CUSTOM;
__attribute__((noinline, section(".text_menupress"))) static void ctlLayoutCycle(CtrInputMap* map) {
    uint32_t now = ctlLayout(map);
    if (now == 2u) {
        memcpy(ctlCustom.keys, map->keys, MAP_ROW_COUNT);
        ctlCustom.have = true;
    }
    uint32_t count = ctlCustom.have ? 3u : 2u;
    uint32_t next = now + (CtrOptmenu_pressLeft() ? count - 1u : 1u);
    if (next >= count) next -= count;
    if (next == 2u) {
        memcpy(map->keys, ctlCustom.keys, MAP_ROW_COUNT);
        return;
    }
    if (next == 1u && now != 0u) ctlLayoutStep(map); // Custom to B confirms goes by way of Default
    ctlLayoutStep(map);
}
