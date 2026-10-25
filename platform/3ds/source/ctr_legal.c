// Legal notices are reached only from the paused settings menu, from its Legal notices row (ctr_screens.c's options
// page, which owns the CtrMenu link wrappers): CtrLegal_open and CtrLegal_update at the end of this file.
#include "ctr_menu.h"
#include "ctr_menu_text.h"
#include "ctr_build_id.h"
#include "ctr_log.h"
#include "ctr_release_branding.h"
#include "ctr_release_contract.h"
#include "ctr_texture_pack.h"

#include <stdio.h>

#define LATE_CODE __attribute__((noinline, section(".text.late.relprep")))
#define LATE_TEXT __attribute__((section(".rodata.late.relprep")))
#define LATE_BSS __attribute__((section(".bss.late.relprep")))

enum { OLD_CONTROLS = 6, OLD_PERF = 9, OLD_RESUME = 10, LEGAL_ITEM = 10, MENU_ITEMS = 13 };

static struct {
    bool selected;
    bool active;
    bool mountTried;
    int document; // -1 is the summary, otherwise LICENSE or THIRD_PARTY_NOTICES
    uint32_t page;
    uint32_t pages;
    char lines[CTR_RELEASE_LEGAL_LINES][CTR_RELEASE_LEGAL_COLUMNS + 1];
} legal LATE_BSS;

static const char divider[] LATE_TEXT = " -------------------------------------";
static const char heading[] LATE_TEXT = "\x1b[0;0H\x1b[1;36m%s\x1b[0m\x1b[K\n";
static const char label[] LATE_TEXT = "Legal notices";
static const char help[] LATE_TEXT = "Licence, no warranty and source.\nRead the bundled third-party notices.";
static const char optionValue[] LATE_TEXT = "%s%s %-14s< %s >\x1b[0m\x1b[K\n";
static const char optionAction[] LATE_TEXT = "%s%s %-14s\x1b[0m\x1b[K\n";
static const char yellow[] LATE_TEXT = "\x1b[1;33m";
static const char dim[] LATE_TEXT = "\x1b[2m";
static const char empty[] LATE_TEXT = "";
static const char marker[][2] LATE_TEXT = { " ", ">" };
static const char clearLine[] LATE_TEXT = "\x1b[K\n";
static const char lineFormat[] LATE_TEXT = "%s\x1b[K\n";
static const char helpStart[] LATE_TEXT = "\x1b[18;0H\x1b[2m ";
static const char helpNewline[] LATE_TEXT = "\x1b[K\n ";
static const char helpEnd[] LATE_TEXT = "\x1b[0m\x1b[K\n\x1b[K\n";
static const char optionsFooter[] LATE_TEXT =
    "\x1b[26;0H -------------------------------------\n"
    " Up/Down: choose  Left/Right/A: change\n"
    " SELECT or B: back to the game\x1b[K";
static const char values[][24] LATE_TEXT = {
    "Wide (800 px)", "Standard", "Bottom screen", "Top screen",
    "Default", "B confirms", "Custom", "On (slower)", "Off",
};
static const char summary[] LATE_TEXT =
    "Copyright: Butterscotch contributors\n"
    "and this port's contributors.\n\n"
    "GNU Affero General Public License v3.\n"
    "You may copy, modify and redistribute\n"
    "this runtime under that licence.\n"
    "NO WARRANTY. See the full licence.\n\n";
static const char sourceHeading[sizeof("Development source:")] LATE_TEXT = CTR_RELEASE_LABEL " source";
static const char sourceUrl[] LATE_TEXT = CTR_RELEASE_SOURCE_URL;
static const char buildHeading[] LATE_TEXT = "Build:";
static const char unofficial[] LATE_TEXT =
    "\nUnofficial. Not affiliated with\n"
    "Toby Fox, Fangamer, YoYo Games\n"
    "or Nintendo.";
static const char summaryFooter[] LATE_TEXT =
    "\x1b[26;0H -------------------------------------\n"
    " A: licence and third-party notices\n"
    " B / SELECT: options\x1b[K";
static const char viewerFooter[] LATE_TEXT =
    "\x1b[26;0H -------------------------------------\n"
    " Left/Right: page  Up/Down: document\n"
    " B: legal notices  SELECT: options\x1b[K";
static const char documentNames[][24] LATE_TEXT = { "LICENSE", "THIRD_PARTY_NOTICES" };
static const char documentPaths[][48] LATE_TEXT = {
    "romfs:/legal/LICENSE.pages", "romfs:/legal/THIRD_PARTY_NOTICES.pages",
};
static const char pageHeading[] LATE_TEXT = "\x1b[0;0H\x1b[1;36m%s %lu/%lu\x1b[0m\x1b[K\n";
static const char unavailable[] LATE_TEXT =
    "Bundled legal text is unavailable.\n"
    "Rebuild or reinstall this package.\n"
    "B returns to the legal summary.";
static const char wrappedLine[] LATE_TEXT = "%.*s\n";
static const char fileMode[] LATE_TEXT = "rb";
static const char romfsName[] LATE_TEXT = "romfs";

void __real_CtrMenu_open(CtrMenu*, PrintConsole*, const CtrDisplaySettings*, CtrDisplayCaps);
CtrMenuAction __real_CtrMenu_update(CtrMenu*, u32);
Result CtrLegal_mountSelf(const char*);

static LATE_CODE bool available(const CtrMenu* menu, int item) {
    if (item == 1) return menu->settings.topMode == TOP_MODE_STANDARD;
    if (item == 2) return menu->caps.stereo && menu->settings.topMode == TOP_MODE_STANDARD;
    if (item == 8) return CTR_PACK_EXACT_AVAILABLE;
    return true;
}

static LATE_CODE const char* value(const CtrMenu* menu, int item) {
    const CtrDisplaySettings* s = &menu->settings;
    switch (item) {
        case 0: return values[s->topMode == TOP_MODE_WIDE ? 0 : 1];
        case 1: return CtrDisplay_drawingName(s->drawing);
        case 2: return CtrDisplay_stereoModeName(s->stereoMode);
        case 3: return CtrDisplay_smoothingName(s->smoothing);
        case 4: return CtrDisplay_bottomModeName(s->bottomMode);
        case 5: return values[s->dialogueBottom ? 2 : 3];
        case OLD_CONTROLS: {
            if (CtrInputMap_isDefault(&s->controls)) return values[4];
            CtrInputMap swapped;
            CtrInputMap_defaults(&swapped);
            CtrInputMap_swapAB(&swapped);
            return values[CtrInputMap_equal(&s->controls, &swapped) ? 5 : 6];
        }
        case 7: return CtrDisplay_frameSkipName(s->frameSkip);
        case 8: return values[s->exactColours ? 7 : 8];
        default: return empty;
    }
}

static LATE_CODE int cursor(const CtrMenu* menu) {
    return legal.selected ? LEGAL_ITEM : menu->cursor >= OLD_RESUME ? menu->cursor + 1 : menu->cursor;
}

static LATE_CODE void drawOptions(CtrMenu* menu) {
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    consoleClear();
    puts(ctrReleaseOptions);
    puts(divider);
    int selected = cursor(menu);
    for (int i = 0; i < MENU_ITEMS; i++) {
        if (i == OLD_PERF) printf(clearLine);
        int old = i > LEGAL_ITEM ? i - 1 : i;
        bool enabled = i == LEGAL_ITEM || available(menu, old);
        const char* colour = !enabled ? dim : i == selected ? yellow : empty;
        const char* name = i == LEGAL_ITEM ? label : ctrMenuText.labels[old];
        const char* text = i == LEGAL_ITEM ? empty : value(menu, old);
        if (enabled && text[0]) printf(optionValue, colour, marker[i == selected], name, text);
        else printf(optionAction, colour, marker[i == selected], name);
    }
    printf(helpStart);
    const char* text = legal.selected ? help : menu->cursor == 2 ? ctrMenuText.stereoHelp[menu->settings.stereoMode]
        : menu->cursor == 3 ? ctrMenuText.smoothingHelp[menu->settings.smoothing] : ctrMenuText.help[menu->cursor];
    for (; *text; text++) {
        if (*text == '\n') printf(helpNewline);
        else putchar(*text);
    }
    printf(helpEnd);
    printf(optionsFooter);
    CtrLog_endConsole();
}

static LATE_CODE void printWrapped(const char* text) {
    while (*text) {
        int length = 0;
        while (text[length] && length < CTR_RELEASE_LEGAL_COLUMNS) length++;
        printf(wrappedLine, length, text);
        text += length;
    }
}

static LATE_CODE void drawSummary(CtrMenu* menu) {
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    consoleClear();
    printf(heading, ctrReleaseAppName);
    puts(divider);
    printf(summary);
    puts(sourceHeading);
    printWrapped(sourceUrl);
    puts(buildHeading);
    printWrapped(CTRPORT_COMMIT);
    puts(unofficial);
    printf(summaryFooter);
    CtrLog_endConsole();
}

static LATE_CODE bool readPage(void) {
    FILE* file = fopen(documentPaths[legal.document], fileMode);
    if (file == NULL && !legal.mountTried) {
        legal.mountTried = true;
        if (R_SUCCEEDED(CtrLegal_mountSelf(romfsName))) file = fopen(documentPaths[legal.document], fileMode);
    }
    if (file == NULL) return false;
    uint32_t header[2];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header) && header[0] == 0x314c474c &&
              header[1] > 0 && header[1] < 65536;
    if (ok) {
        legal.pages = header[1];
        if (legal.page >= legal.pages) legal.page = legal.pages - 1;
        ok = fseek(file, 8 + legal.page * sizeof(legal.lines), SEEK_SET) == 0 &&
             fread(legal.lines, 1, sizeof(legal.lines), file) == sizeof(legal.lines);
    }
    fclose(file);
    for (int i = 0; i < CTR_RELEASE_LEGAL_LINES; i++) legal.lines[i][CTR_RELEASE_LEGAL_COLUMNS] = '\0';
    return ok;
}

static LATE_CODE void drawViewer(CtrMenu* menu) {
    bool ok = readPage();
    CtrLog_beginConsole();
    consoleSelect(menu->console);
    consoleClear();
    printf(pageHeading, documentNames[legal.document], (unsigned long) legal.page + 1, (unsigned long) legal.pages);
    puts(divider);
    if (ok) {
        for (int i = 0; i < CTR_RELEASE_LEGAL_LINES; i++) printf(lineFormat, legal.lines[i]);
    } else puts(unavailable);
    printf(viewerFooter);
    CtrLog_endConsole();
}

static LATE_CODE CtrMenuAction updateLegal(CtrMenu* menu, u32 down) {
    if ((down & KEY_SELECT) || ((down & KEY_B) && legal.document < 0)) {
        legal.active = false;
        drawOptions(menu);
    } else if (down & KEY_B) {
        legal.document = -1;
        drawSummary(menu);
    } else if (legal.document < 0) {
        if (down & KEY_A) {
            legal.document = 0;
            legal.page = 0;
            drawViewer(menu);
        }
    } else if (down & (KEY_DUP | KEY_DDOWN | KEY_CPAD_UP | KEY_CPAD_DOWN)) {
        legal.document = 1 - legal.document;
        legal.page = 0;
        drawViewer(menu);
    } else if (down & (KEY_DLEFT | KEY_CPAD_LEFT)) {
        if (legal.page > 0) legal.page--;
        drawViewer(menu);
    } else if (down & (KEY_DRIGHT | KEY_CPAD_RIGHT)) {
        if (legal.page + 1 < legal.pages) legal.page++;
        drawViewer(menu);
    }
    return MENU_ACTION_NONE;
}

// Retired (screens-port round 3b): these were the CtrMenu_open and CtrMenu_update link wrappers. ctr_screens.c's options
// page has those now, and nothing calls these two. They stay, unchanged, so that this file's late code keeps its size
// and every late function after it its address (the old slot stays filled).
void CtrLegal_retiredMenuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps);
CtrMenuAction CtrLegal_retiredMenuUpdate(CtrMenu* menu, u32 down);

LATE_CODE void CtrLegal_retiredMenuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    // Also cover the scheduled-menu test hook, which opens before main updates
    // statsShown. Its log messages must not overwrite a settings/legal page.
    CtrLog_setConsoleEnabled(false);
    __real_CtrMenu_open(menu, console, settings, caps);
    legal.selected = false;
    legal.active = false;
    drawOptions(menu);
}

LATE_CODE CtrMenuAction CtrLegal_retiredMenuUpdate(CtrMenu* menu, u32 down) {
    if (!menu->open) return MENU_ACTION_NONE;
    if (legal.active) return updateLegal(menu, down);
    if (menu->page == MENU_PAGE_CONTROLS) {
        CtrMenuAction action = __real_CtrMenu_update(menu, down);
        if (menu->page == MENU_PAGE_OPTIONS && action == MENU_ACTION_NONE) drawOptions(menu);
        return action;
    }
    if (down & (KEY_SELECT | KEY_B)) return MENU_ACTION_RESUME;
    const u32 previous = KEY_DUP | KEY_CPAD_UP;
    const u32 next = KEY_DDOWN | KEY_CPAD_DOWN;
    if (down & (previous | next)) {
        int at = cursor(menu);
        if (down & previous) at = (at + MENU_ITEMS - 1) % MENU_ITEMS;
        if (down & next) at = (at + 1) % MENU_ITEMS;
        legal.selected = at == LEGAL_ITEM;
        menu->cursor = at > LEGAL_ITEM ? at - 1 : at;
    }
    if (legal.selected && (down & (KEY_A | KEY_DLEFT | KEY_DRIGHT | KEY_CPAD_LEFT | KEY_CPAD_RIGHT))) {
        legal.active = true;
        legal.document = -1;
        drawSummary(menu);
        return MENU_ACTION_NONE;
    }
    CtrMenuAction action = __real_CtrMenu_update(menu, down & ~(previous | next));
    if (down && menu->page == MENU_PAGE_OPTIONS && (action == MENU_ACTION_NONE || action == MENU_ACTION_CHANGED)) drawOptions(menu);
    return action;
}

// ===[ The entry points from the options page (screens-port round 3b) ]===
//
// In screens' late sections (platform/3ds/late_reserve.ld), after every older late input, so nothing above moves.
#define LEGAL_ENTRY __attribute__((noinline, section(".text.late.screens")))
#define LEGAL_ENTRY_RO __attribute__((section(".rodata.late.screens")))

void CtrLegal_open(CtrMenu* menu);
bool CtrLegal_update(CtrMenu* menu, u32 down);
bool CtrLegal_shown(void);
void CtrLegal_close(void);

// The Legal notices row's label and help (the texts above), for the options page.
const char* const ctrLegalLabel LEGAL_ENTRY_RO = label;
const char* const ctrLegalHelp LEGAL_ENTRY_RO = help;

// The options page's Legal notices row was chosen: the summary. (selected as relprep's own row set it, so that
// drawOptions, which updateLegal calls on the way out, reads no list row by the screens page's cursor.)
LEGAL_ENTRY void CtrLegal_open(CtrMenu* menu) {
    legal.selected = true;
    legal.active = true;
    legal.document = -1;
    drawSummary(menu);
}

// The menu's keys while the summary or a document is shown (true), or false when neither is. Leaving it (SELECT, or
// B on the summary) clears legal.active; the options page then draws itself over what updateLegal drew.
LEGAL_ENTRY bool CtrLegal_update(CtrMenu* menu, u32 down) {
    if (!legal.active) return false;
    updateLegal(menu, down);
    return true;
}

LEGAL_ENTRY bool CtrLegal_shown(void) {
    return legal.active;
}

// The menu opened afresh: its options page first.
LEGAL_ENTRY void CtrLegal_close(void) {
    legal.selected = false;
    legal.active = false;
}

// Reuse the original 20 B late slot: every existing instruction and pointer stays fixed.
_Static_assert(sizeof(CTR_RELEASE_LABEL " source") <= sizeof(sourceHeading), "Release label exceeds retained legal heading");
