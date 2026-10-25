#include "ctr_menu.h"
#include "ctr_menu_text.h"

#include <stdio.h>

#include "ctr_log.h"
#include "ctr_release_branding.h"
#include "ctr_texture_pack.h"

typedef enum {
    ITEM_TOP_MODE,
    ITEM_DRAWING,
    ITEM_STEREO,
    ITEM_SMOOTHING,
    ITEM_BOTTOM_MODE,
    ITEM_DIALOGUE,
    ITEM_CONTROLS,
    ITEM_FRAME_SKIP,
    ITEM_EXACT_COLOURS,
    ITEM_PERF_TEST,
    ITEM_RESUME,
    ITEM_QUIT,
    ITEM_COUNT
} MenuItem;

static const char* const itemLabels[ITEM_COUNT] = {
    "Top screen", "Drawing", "3D mode", "Smoothing", "Bottom screen", "Dialogue box", "Controls", "Frame skip", "Exact colours",
    "Performance test", "Back to the game", "Quit game",
};

// Shown under the list for the selected item; at most two 40-column lines.
static const char* const itemHelp[ITEM_COUNT] = {
    "Standard: the whole frame at 400x240.\nWide: all 800 columns, sharper, no 3D.",
    "Native: drawn at the screen's 320x240,\nfaster. Full: 640x480, shrunk to fit.",
    "", // stereoHelp, by value
    "", // smoothingHelp, by value
    "The magnifier shows small text, like\nspeech bubbles, at full size.",
    "Bottom: dialogue outside battles moves\nthere, leaving the scene clear.",
    "Which PC key each button sends. SELECT\nalways opens this menu.",
    "Auto: a late frame lets the next go\nundrawn so the game keeps its speed.",
    "Unrounded colours; slower, +1/3 on SD.\nApplies at next chapter or restart.",
    "Plays scenes from the game and measures\nthem. Your saves aren't touched.",
    "Also SELECT or B.",
    "Progress since your last save is lost.",
};

static const char* const stereoHelp[STEREO_MODE_COUNT] = {
    "With the 3D slider up, both eyes see\nthe same image.",
    "With the 3D slider up, each eye sees\nhalf the columns of the full image.",
    "HD, and the room sits behind the screen\nwith text on it. The slider sets depth.",
};

static const char* const smoothingHelp[SMOOTHING_COUNT] = {
    "Off: sharp pixels everywhere; small\ntext and fine lines can break up.",
    "Full: every pixel blended. Smooth\ntext, softer pixel art.",
    "Smart: sharp pixel art, smooth text\nand fine detail.",
};

static bool itemAvailable(const CtrMenu* menu, MenuItem item) {
    if (item == ITEM_DRAWING) return menu->settings.topMode == TOP_MODE_STANDARD;
    if (item == ITEM_STEREO) return menu->caps.stereo && menu->settings.topMode == TOP_MODE_STANDARD;
    if (item == ITEM_EXACT_COLOURS) return CTR_PACK_EXACT_AVAILABLE;
    return true;
}

static const char* itemValue(const CtrMenu* menu, MenuItem item) {
    const CtrDisplaySettings* s = &menu->settings;
    switch (item) {
        case ITEM_TOP_MODE:
            if (s->topMode == TOP_MODE_WIDE) return "Wide (800 px)";
            return "Standard";
        case ITEM_DRAWING:
            if (s->topMode != TOP_MODE_STANDARD) return "(full in wide mode)";
            return CtrDisplay_drawingName(s->drawing);
        case ITEM_STEREO:
            if (!menu->caps.stereo) return "(no 3D screen)";
            if (s->topMode != TOP_MODE_STANDARD) return "(off in wide mode)";
            return CtrDisplay_stereoModeName(s->stereoMode);
        case ITEM_SMOOTHING: return CtrDisplay_smoothingName(s->smoothing);
        case ITEM_BOTTOM_MODE: return CtrDisplay_bottomModeName(s->bottomMode);
        case ITEM_DIALOGUE: return s->dialogueBottom ? "Bottom screen" : "Top screen";
        case ITEM_CONTROLS: {
            if (CtrInputMap_isDefault(&s->controls)) return "Default";
            CtrInputMap swapped;
            CtrInputMap_defaults(&swapped);
            CtrInputMap_swapAB(&swapped);
            return CtrInputMap_equal(&s->controls, &swapped) ? "B confirms" : "Custom";
        }
        case ITEM_FRAME_SKIP: return CtrDisplay_frameSkipName(s->frameSkip);
        case ITEM_EXACT_COLOURS:
            if (!CTR_PACK_EXACT_AVAILABLE) return "(New 3DS only)";
            return s->exactColours ? "On (slower)" : "Off";
        default: return "";
    }
}

static void draw(const CtrMenu* menu) {
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    puts(ctrReleaseOptions);
    printf(" ---------------------------------------\n");
    for (int32_t i = 0; ITEM_COUNT > i; i++) {
        bool selected = menu->cursor == i;
        bool available = itemAvailable(menu, (MenuItem) i);
        if (i == ITEM_PERF_TEST) printf("\x1b[K\n");
        const char* colour = !available ? "\x1b[2m" : selected ? "\x1b[1;33m" : "";
        const char* value = itemValue(menu, (MenuItem) i);
        if (value[0] != '\0' && available) {
            printf("%s%s %-14s< %s >\x1b[0m\x1b[K\n", colour, selected ? ">" : " ", itemLabels[i], value);
        } else {
            printf("%s%s %-14s%s\x1b[0m\x1b[K\n", colour, selected ? ">" : " ", itemLabels[i], value);
        }
    }
    printf("\x1b[15;0H\x1b[2m ");
    const char* help = menu->cursor == ITEM_STEREO      ? stereoHelp[menu->settings.stereoMode]
                       : menu->cursor == ITEM_SMOOTHING ? smoothingHelp[menu->settings.smoothing]
                                                        : itemHelp[menu->cursor];
    for (const char* c = help; *c != '\0'; c++) {
        if (*c == '\n') printf("\x1b[K\n ");
        else putchar(*c);
    }
    printf("\x1b[0m\x1b[K\n\x1b[K\n");
    printf("\x1b[26;0H ---------------------------------------\n");
    printf(" Up/Down: choose   Left/Right/A: change\n");
    printf(" SELECT or B: back to the game\x1b[K");
    CtrLog_endConsole();
}

// ===[ Controls page ]===

typedef enum {
    CONTROLS_SWAP = MAP_ROW_COUNT, // after the buttons
    CONTROLS_RESET,
    CONTROLS_BACK,
    CONTROLS_COUNT
} ControlsItem;

static const char* controlsHelp(int32_t item) {
    switch (item) {
#ifdef CTRPORT_PLAYER_SHOTS
        case MAP_ROW_L: return "As a game button, L waits one frame so\nL+SELECT screenshots don't reach it.";
#else // release builds have no L+SELECT screenshot; the same length keeps .rodata in place (trailing spaces)
        case MAP_ROW_L: return "As a game button, L waits one frame\nwhen pressed with SELECT.              ";
#endif
#line 141
        case MAP_ROW_ZL:
        case MAP_ROW_ZR:
        case MAP_ROW_CSTICK: return "New 3DS only.";
        case MAP_ROW_START: return "SELECT+START still quits.";
        case MAP_ROW_DPAD:
        case MAP_ROW_CPAD: return "Arrows: moves. Nothing: off.";
        case CONTROLS_SWAP: return "Swaps what A and B send: B confirms\nand A cancels, as in Japanese games.";
        case CONTROLS_RESET: return "A Z, B X, X and Y C, START Enter,\nD-pad and Circle Pad the arrows.";
        case CONTROLS_BACK: return "Also B.";
        default: return "Z or Enter confirms, X or Shift cancels\n(held: runs), C or Ctrl opens the menu.";
    }
}

static void drawControls(const CtrMenu* menu) {
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    puts(ctrReleaseControls);
    printf(" ---------------------------------------\n");
    for (int32_t i = 0; CONTROLS_COUNT > i; i++) {
        bool selected = menu->controlsCursor == i;
        const char* colour = selected ? "\x1b[1;33m" : "";
        if (i == CONTROLS_SWAP) printf("\x1b[K\n");
        if (MAP_ROW_COUNT > i) {
            const char* value = CtrInputMap_keyName((CtrGameKey) menu->settings.controls.keys[i]);
            printf("%s%s %-14s< %s >\x1b[0m\x1b[K\n", colour, selected ? ">" : " ", CtrInputMap_rowName((CtrMapRow) i), value);
        } else {
            static const char* const labels[] = { "Swap A and B", "Reset to default", "Back" };
            printf("%s%s %s\x1b[0m\x1b[K\n", colour, selected ? ">" : " ", labels[i - CONTROLS_SWAP]);
        }
    }
    printf("\x1b[19;0H\x1b[2m ");
    for (const char* c = controlsHelp(menu->controlsCursor); *c != '\0'; c++) {
        if (*c == '\n') printf("\x1b[K\n ");
        else putchar(*c);
    }
    printf("\x1b[0m\x1b[K\n\x1b[K\n");
    printf("\x1b[26;0H ---------------------------------------\n");
    printf(" Up/Down: choose   Left/Right/A: change\n");
    printf(" B: back   SELECT: back to the game\x1b[K");
    CtrLog_endConsole();
}

static void showPage(CtrMenu* menu, CtrMenuPage page) {
    menu->page = page;
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    consoleClear();
    CtrLog_endConsole();
    if (page == MENU_PAGE_CONTROLS) drawControls(menu);
    else draw(menu);
}

// The Controls page's share of CtrMenu_update; SELECT has been handled.
static CtrMenuAction updateControls(CtrMenu* menu, u32 down) {
    if (down & KEY_B) {
        showPage(menu, MENU_PAGE_OPTIONS);
        return MENU_ACTION_NONE;
    }
    bool redraw = false;
    if (down & (KEY_DUP | KEY_CPAD_UP)) {
        menu->controlsCursor = (menu->controlsCursor + CONTROLS_COUNT - 1) % CONTROLS_COUNT;
        redraw = true;
    }
    if (down & (KEY_DDOWN | KEY_CPAD_DOWN)) {
        menu->controlsCursor = (menu->controlsCursor + 1) % CONTROLS_COUNT;
        redraw = true;
    }
    int32_t dir = 0;
    if (down & (KEY_DRIGHT | KEY_CPAD_RIGHT)) dir = 1;
    if (down & (KEY_DLEFT | KEY_CPAD_LEFT)) dir = -1;
    CtrInputMap* map = &menu->settings.controls;
    bool changed = false;
    if (MAP_ROW_COUNT > menu->controlsCursor) {
        if (down & KEY_A) dir = 1;
        if (dir != 0) {
            CtrMapRow row = (CtrMapRow) menu->controlsCursor;
            map->keys[row] = CtrInputMap_step(row, map->keys[row], dir);
            changed = true;
        }
    } else if (down & KEY_A) {
        if (menu->controlsCursor == CONTROLS_BACK) {
            showPage(menu, MENU_PAGE_OPTIONS);
            return MENU_ACTION_NONE;
        }
        CtrInputMap before = *map;
        if (menu->controlsCursor == CONTROLS_SWAP) CtrInputMap_swapAB(map);
        else CtrInputMap_defaults(map);
        changed = !CtrInputMap_equal(&before, map);
    }
    if (changed) {
        menu->edited = true;
        redraw = true;
    }
    if (redraw) drawControls(menu);
    return changed ? MENU_ACTION_CHANGED : MENU_ACTION_NONE;
}

void CtrMenu_open(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    menu->open = true;
    menu->page = MENU_PAGE_OPTIONS;
    menu->cursor = 0;
    menu->controlsCursor = 0;
    menu->settings = *settings;
    menu->caps = caps;
    menu->edited = false;
    menu->console = console;
    CtrLog_beginConsole();
    consoleSelect(console);
    consoleClear();
    draw(menu);
    CtrLog_endConsole();
}

void CtrMenu_close(CtrMenu* menu) {
    if (!menu->open) return;
    menu->open = false;
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    consoleClear();
    CtrLog_endConsole();
}

// Steps the selected setting forwards (dir 1) or backwards (dir -1).
static bool changeItem(CtrMenu* menu, int32_t dir) {
    CtrDisplaySettings* s = &menu->settings;
    switch ((MenuItem) menu->cursor) {
        case ITEM_TOP_MODE:
            do {
                s->topMode = (CtrTopMode) (((int32_t) s->topMode + dir + TOP_MODE_COUNT) % TOP_MODE_COUNT);
            } while (s->topMode == TOP_MODE_WIDE && !menu->caps.wide);
            return true;
        case ITEM_STEREO: s->stereoMode = (CtrStereoMode) (((int32_t) s->stereoMode + dir + STEREO_MODE_COUNT) % STEREO_MODE_COUNT); return true;
        case ITEM_DRAWING: s->drawing = s->drawing == DRAWING_NATIVE ? DRAWING_FULL : DRAWING_NATIVE; return true;
        case ITEM_SMOOTHING: {
            // Off, Smart, Full: Smart sits between the other two.
            static const CtrSmoothing order[SMOOTHING_COUNT] = { SMOOTHING_OFF, SMOOTHING_SMART, SMOOTHING_FULL };
            int32_t at = 0;
            while (SMOOTHING_COUNT - 1 > at && order[at] != s->smoothing) at++;
            s->smoothing = order[(at + dir + SMOOTHING_COUNT) % SMOOTHING_COUNT];
            return true;
        }
        case ITEM_BOTTOM_MODE: s->bottomMode = (CtrBottomMode) (((int32_t) s->bottomMode + dir + BOTTOM_MODE_COUNT) % BOTTOM_MODE_COUNT); return true;
        case ITEM_DIALOGUE: s->dialogueBottom = !s->dialogueBottom; return true;
        case ITEM_FRAME_SKIP:
            // Off, Auto, 20 fps, 15 fps; 2:1 is set in the file only.
            s->frameSkip = (CtrFrameSkip) (((int32_t) s->frameSkip + dir + FRAME_SKIP_COUNT) % FRAME_SKIP_COUNT);
            if (s->frameSkip == FRAME_SKIP_HALF) s->frameSkip = (CtrFrameSkip) (((int32_t) s->frameSkip + dir + FRAME_SKIP_COUNT) % FRAME_SKIP_COUNT);
            return true;
        case ITEM_EXACT_COLOURS: s->exactColours = !s->exactColours; return true;
        default: return false;
    }
}

CtrMenuAction CtrMenu_update(CtrMenu* menu, u32 down) {
    if (!menu->open) return MENU_ACTION_NONE;
    if (down & KEY_SELECT) return MENU_ACTION_RESUME;
    if (menu->page == MENU_PAGE_CONTROLS) return updateControls(menu, down);
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
    if (menu->cursor == ITEM_CONTROLS && (dir != 0 || (down & KEY_A))) {
        menu->controlsCursor = 0;
        showPage(menu, MENU_PAGE_CONTROLS);
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
    if (redraw) draw(menu);
    return action;
}

// Preserve the former merged literals (GCC lowered printf to puts, omitting the
// newline). Only the retention pointers are new; they live in the late reserve.
static const char* const relprepMenuLegacy[]
    __attribute__((used, section(".rodata.late.relprep"))) = {
        "\x1b[0;0H\x1b[1;36m ctrport options\x1b[0m\x1b[K",
        "\x1b[0;0H\x1b[1;36m ctrport options > Controls\x1b[0m\x1b[K",
    };

// Only a late pointer table is new. Keep the original menu, strings and struct
// unchanged; the legal-menu hooks reuse these texts when the menu is opened.
const CtrMenuText ctrMenuText __attribute__((section(".rodata.late.relprep"))) = {
    itemLabels, itemHelp, stereoHelp, smoothingHelp,
};
