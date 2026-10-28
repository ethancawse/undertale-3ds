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
static const char sourceHeading[sizeof("Development source:")] LATE_TEXT = CTR_RELEASE_SOURCE_HEADING;
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
#if defined(ENABLE_WAD17) // UNDERTALE never runs this (argcheck, UT late-text reclaim)
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
#endif
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
_Static_assert(sizeof(CTR_RELEASE_SOURCE_HEADING) <= sizeof(sourceHeading), "Source heading exceeds retained legal heading");

// ===[ legalui (round 20): the Legal notices and Credits pages in the game's own menu font ]===
//
// Drawn like the options page (ctr_optmenu.c): into the menu screen's spare buffer (the menu console's while the menu
// is open, 320 x 240 RGB565, column-major), which optmenu copies to the shown one each frame while a page here is up.
// Glyphs come from optmenu's table through CtrOptmenu_text, so these pages use the menu's font (UNDERTALE's
// fnt_maintext, DELTARUNE's fnt_main) with no texture or SD access of their own.
// - Legal notices: the licence summary and disclaimer, with rows for the licence and the third-party notices. Each
//   opens a reader that scrolls by line (Up/Down, held repeats) and by screen (Left/Right, L/R). Every row of the
//   bundled .pages files is shown as it is (tools/release/prepare_legal.py wraps them at spaces).
// - Credits: romfs:/credits.txt (tools/aot/credits.py: the game's own credits read from the player's data at build
//   time, then this port's), centred, rolling up slowly; Up/Down scroll it by hand.
// B goes back a page (from the Legal page and Credits, to the options page, where its cursor was); SELECT too.
// The menu's sounds play from optKeys, before these keys reach here. Appended at the file's end, in sections named
// outside .text.late.* (late_reserve.ld), so nothing above moves.
#include <stdlib.h>
#include <string.h>

// Size over speed for these pages (UNDERTALE Old's late code reserve is small; they draw on key presses only).
#pragma GCC optimize("Os")
#define LUI_CODE __attribute__((noinline, section(".text.legalui_late")))
#define LUI_RO __attribute__((section(".rodata.legalui_in_text")))
#define LUI_BSS __attribute__((section(".bss.legalui_late")))

void CtrOptmenu_text(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col);
void CtrLegalui_open(CtrMenu* menu, bool credits);
void CtrLegalui_update(CtrMenu* menu, u32 down);

enum { LUI_HUB, LUI_DOC, LUI_CREDITS };
// Lines 14 px apart: the menu font's ink is 12 px tall with its descenders, so 2 px stay clear between lines (as
// the options page's rows). The reader shows 13 rows (title above, the help line below); the Legal page wraps its
// text at 40 columns (x 24 to 304).
enum { LUI_ROWS = 13, LUI_TOP = 30, LUI_PITCH = 14, LUI_COLS = CTR_RELEASE_LEGAL_COLUMNS, LUI_WIDE = 40 };
#define LUI_WHITE 0xFFFFu
#define LUI_YELLOW 0xFFE0u
#define LUI_GREY 0x8410u
#define LUI_LIGHT 0xC618u
#define LUI_RED 0xF800u
#define LUI_UP (KEY_DUP | KEY_CPAD_UP)
#define LUI_DOWN (KEY_DDOWN | KEY_CPAD_DOWN)
#define LUI_PREV (KEY_DLEFT | KEY_CPAD_LEFT)
#define LUI_NEXT (KEY_DRIGHT | KEY_CPAD_RIGHT)
#define LUI_MOVE (LUI_UP | LUI_DOWN | LUI_PREV | LUI_NEXT)

static struct {
    uint8_t view;
    uint8_t row;     // the Legal page's cursor: 0 licence, 1 notices, 2 back
    uint8_t held;    // frames Up or Down has been held
    bool dirty;
    int32_t top;     // reader: first row shown; credits: scroll in pixels
    int32_t rows;    // reader: rows in the document
    int32_t height;  // credits: the text's height in pixels
    int32_t idle;    // credits: frames since a key moved it (the roll waits, then resumes)
    char* credits;   // romfs:/credits.txt, NUL-terminated
} lui LUI_BSS;

// The Legal page's rows (the first two also the reader's titles) and their help lines, NUL-separated (luiNth).
static const char luiRows[] LUI_RO = "Licence (GNU AGPL v3)\0Third-party notices\0Back";
static const char luiHelp[] LUI_RO = "Read the licence.\0Read the notices of the code we use.\0Back to the options.";
static const char luiDocHelp[] LUI_RO = "Up/Down scroll, Left/Right page, L/R ends";
static const char luiCreditsPath[] LUI_RO = "romfs:/credits.txt";
static const char luiHeart[] LUI_RO = "\x01";

// A document (0 licence, 1 notices) or the credits (2), mounting the romfs once if it isn't (as the old viewer does).
// relprep's late rodata is placed in the order its names are first used (section anchors), and this function is
// compiled before readPage: it names fileMode, documentPaths and romfsName in readPage's order (the empty asm keeps
// fileMode first) so that rodata keeps its layout.
static LUI_CODE FILE* luiOpen(int32_t which) {
    const char* mode = fileMode;
    __asm__("" : "+r"(mode));
    const char* path = which < 2 ? documentPaths[which] : luiCreditsPath;
    FILE* file = fopen(path, mode);
    if (file == NULL && !legal.mountTried) {
        legal.mountTried = true;
        if (R_SUCCEEDED(CtrLegal_mountSelf(romfsName))) file = fopen(path, mode);
    }
    return file;
}

// String i of NUL-separated strings.
static LUI_CODE const char* luiNth(const char* s, uint32_t i) {
    for (; i != 0; i--) s += strlen(s) + 1;
    return s;
}

// The page cleared to black, with the options page's frame (border; the rule above the help line at y 214) unless bare.
static LUI_CODE uint16_t* luiClear(CtrMenu* menu, bool bare) {
    uint16_t* fb = (uint16_t*) menu->console->frameBuffer;
#ifdef ENABLE_WAD16
    const uint16_t border = 0xFFFFu;
#else
    const uint16_t border = 0x9D18u;
#endif
    uint16_t* p = fb;
    for (uint32_t x = 0; x < 320u; x++) {
        for (uint32_t y = 0; y < 240u; y++) {
            *p++ = !bare && (x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? border : 0u;
        }
    }
    return fb;
}

static LUI_CODE void luiFill(uint16_t* fb, int32_t x0, int32_t y0, int32_t w, int32_t h, uint16_t col) {
    for (int32_t x = x0; x < x0 + w; x++) {
        for (int32_t y = y0; y < y0 + h; y++) fb[x * 240 + (239 - y)] = col;
    }
}

// One line of at most LUI_COLS + 4 bytes; a control character (a line break joined into a line) shows as a space,
// one outside ASCII (UTF-8, in two names) as '?'.
static LUI_CODE void luiLine(uint16_t* fb, int32_t x, int32_t y, const char* s, int32_t n, uint32_t col) {
    char buf[LUI_COLS + 5];
    int32_t k = 0;
    for (int32_t i = 0; i < n && s[i] != '\0' && k < LUI_COLS + 4; i++) {
        uint8_t c = (uint8_t) s[i];
        if ((c & 0xC0u) == 0x80u) continue; // a continuation byte: its lead byte drew the '?'
        buf[k++] = c < 32u ? ' ' : c < 127u ? (char) c : '?';
    }
    buf[k] = '\0';
    CtrOptmenu_text(fb, x, y, buf, col);
}

// Text from y at x 24, wrapped at LUI_WIDE after a space or '/' (a URL); a single '\n' joins as a space, "\n\n" ends a
// line. Returns the next line's y.
static LUI_CODE int32_t luiBlock(uint16_t* fb, int32_t y, const char* s, uint32_t col) {
    for (;;) {
        while (*s == ' ' || *s == '\n') s++;
        if (*s == '\0') return y;
        int32_t n = 0;
        while (s[n] != '\0' && (s[n] != '\n' || s[n + 1] != '\n') && n < LUI_WIDE) n++;
        if (n == LUI_WIDE && s[n] > ' ') {
            int32_t cut = n;
            while (cut > 0 && s[cut - 1] > ' ' && s[cut - 1] != '/') cut--;
            if (cut > 0) n = cut;
        }
        luiLine(fb, 24, y, s, n, col);
        y += LUI_PITCH;
        s += n;
    }
}

// Appends s to t (n bytes so far, at most 126), returning the new length.
static LUI_CODE int32_t luiCat(char* t, int32_t n, const char* s) {
    while (*s != '\0' && n < 126) t[n++] = *s++;
    t[n] = '\0';
    return n;
}

static LUI_CODE void luiDrawHub(CtrMenu* menu) {
    uint16_t* fb = luiClear(menu, false);
    for (uint32_t i = 0; i < 3u; i++) {
        int32_t y = 8 + LUI_PITCH * (int32_t) i;
        CtrOptmenu_text(fb, 24, y, luiNth(luiRows, i), i == lui.row ? LUI_YELLOW : LUI_WHITE);
        if (i == lui.row) CtrOptmenu_text(fb, 11, y, luiHeart, LUI_RED);
    }
    // Under the rows (Back is the third, as on the options' other pages; a block's 20 px further): the summary, the
    // source and build (light grey), the disclaimer: 11 lines, the last one's ink ending at y 207.
    char t[128];
    int32_t n = luiCat(t, 0, sourceHeading);
    t[n++] = ' ';
    n = luiCat(t, n, sourceUrl);
    t[n++] = '\n';
    t[n++] = '\n';
    n = luiCat(t, n, buildHeading);
    t[n++] = ' ';
    luiCat(t, n, CTRPORT_COMMIT);
    int32_t y = luiBlock(fb, 56, summary, LUI_WHITE);
    luiBlock(fb, luiBlock(fb, y, t, LUI_LIGHT), unofficial, LUI_WHITE);
    CtrOptmenu_text(fb, 12, 221, luiNth(luiHelp, lui.row), LUI_WHITE);
}

// count rows of the open .pages file from row into legal.lines.
static LUI_CODE bool luiRead(FILE* file, int32_t row, int32_t count) {
    return fseek(file, 8 + row * (LUI_COLS + 1), SEEK_SET) == 0 &&
           fread(legal.lines, LUI_COLS + 1, (size_t) count, file) == (size_t) count;
}

// The reader: the title, LUI_ROWS rows from lui.top (CtrLegalui_update keeps it in range), and a scroll bar.
static LUI_CODE void luiDrawDoc(CtrMenu* menu) {
    uint16_t* fb = luiClear(menu, false);
    CtrOptmenu_text(fb, 12, 8, luiNth(luiRows, (uint32_t) legal.document), LUI_YELLOW);
    CtrOptmenu_text(fb, 12, 221, luiDocHelp, LUI_WHITE);
    FILE* file = luiOpen(legal.document);
    bool ok = false;
    if (file != NULL) {
        uint32_t header[2];
        ok = fread(header, 4, 2, file) == 2 && header[0] == 0x314c474c && header[1] - 1u < 65535u;
        if (ok && lui.rows == 0) {
            // Once per opening: the rows up to the last one with text (the last page's padding isn't scrolled to).
            int32_t last = (int32_t) header[1] * CTR_RELEASE_LEGAL_LINES;
            if (luiRead(file, last - CTR_RELEASE_LEGAL_LINES, CTR_RELEASE_LEGAL_LINES)) {
                while (last > 1 && legal.lines[(last - 1) % CTR_RELEASE_LEGAL_LINES][0] == '\0') last--;
            }
            lui.rows = last;
        }
        ok = ok && luiRead(file, lui.top, LUI_ROWS);
        fclose(file);
    }
    if (!ok) {
        luiBlock(fb, LUI_TOP, unavailable, LUI_WHITE);
        return;
    }
    for (int32_t i = 0; i < LUI_ROWS; i++) luiLine(fb, 24, LUI_TOP + LUI_PITCH * i, legal.lines[i], LUI_COLS, LUI_WHITE);
    // The bar: the track grey, the part shown white.
    const int32_t track = LUI_ROWS * LUI_PITCH;
    int32_t span = lui.rows - LUI_ROWS;
    int32_t size = span > 0 ? track * LUI_ROWS / lui.rows : track;
    if (size < 4) size = 4;
    luiFill(fb, 309, LUI_TOP, 2, track, LUI_GREY);
    luiFill(fb, 308, LUI_TOP + (span > 0 ? (track - size) * lui.top / span : 0), 4, size, LUI_WHITE);
}

// Credits: each line of credits.txt starts with its colour (Y yellow, G grey, W white); an empty line is a gap.
static LUI_CODE void luiDrawCredits(CtrMenu* menu) {
    uint16_t* fb = luiClear(menu, true);
    int32_t y = 240 - LUI_PITCH * 8 - lui.top;
    for (const char* s = lui.credits; s != NULL && *s != '\0'; y += LUI_PITCH) {
        int32_t n = 0;
        while (s[n] != '\0' && s[n] != '\n') n++;
        if (n > 1 && y >= 0 && y <= 240 - LUI_PITCH) {
            int32_t w = n - 1 > LUI_COLS + 4 ? LUI_COLS + 4 : n - 1;
            uint32_t col = s[0] == 'Y' ? LUI_YELLOW : s[0] == 'G' ? LUI_GREY : LUI_WHITE;
            int32_t x = (320 - 7 * w) / 2;
            luiLine(fb, x < 0 ? 0 : x, y, s + 1, w, col);
        }
        s += n;
        if (*s == '\n') s++;
    }
    lui.height = y + lui.top - (240 - LUI_PITCH * 8); // the text's height (luiDraw runs before any key is read)
}

static LUI_CODE void luiDraw(CtrMenu* menu) {
    CtrLog_beginConsole();
    if (lui.view == LUI_HUB) luiDrawHub(menu);
    else if (lui.view == LUI_DOC) luiDrawDoc(menu);
    else luiDrawCredits(menu);
    CtrLog_endConsole();
    lui.dirty = false;
}

static LUI_CODE void luiLoadCredits(void) {
    free(lui.credits);
    lui.credits = NULL;
    FILE* file = luiOpen(2);
    if (file == NULL) return;
    if (fseek(file, 0, SEEK_END) == 0) {
        long size = ftell(file);
        char* text = size > 0 && size < 65536 ? malloc((size_t) size + 1u) : NULL;
        if (text != NULL && fseek(file, 0, SEEK_SET) == 0 && fread(text, 1, (size_t) size, file) == (size_t) size) {
            text[size] = '\0';
            lui.credits = text;
        } else free(text);
    }
    fclose(file);
}

// The options page's Legal notices (credits false) or Credits row was chosen.
LUI_CODE void CtrLegalui_open(CtrMenu* menu, bool credits) {
    legal.selected = true;
    legal.active = true;
    legal.document = -1;
    lui.view = credits ? LUI_CREDITS : LUI_HUB;
    lui.row = 0;
    lui.top = 0;
    lui.idle = 0;
    lui.held = 0;
    if (credits) luiLoadCredits();
    luiDraw(menu);
}

// Each frame while a page here is up (down is 0 on frames without a press): keys, held Up/Down, the credits' roll.
LUI_CODE void CtrLegalui_update(CtrMenu* menu, u32 down) {
    if (!legal.active) return;
    u32 held = hidKeysHeld() | down;
    if ((down & KEY_SELECT) || ((down & KEY_B) && lui.view != LUI_DOC)) {
        legal.active = false;
        free(lui.credits);
        lui.credits = NULL;
        return; // the options page draws itself (optKeys marked it dirty)
    }
    // Up/Down a row, Left/Right a screen: one step on the press, then every other frame after 10 frames held.
    int32_t step = 0;
    if (held & LUI_MOVE) {
        lui.held = lui.held < 255u ? (uint8_t) (lui.held + 1u) : lui.held;
        if ((down & LUI_MOVE) || (lui.held > 10u && (lui.held & 1u) == 0)) {
            step = (held & LUI_UP) ? -1 : (held & LUI_DOWN) ? 1 : (held & LUI_PREV) ? -LUI_ROWS : LUI_ROWS;
        }
    } else lui.held = 0;
    if (lui.view == LUI_HUB) {
        if (down & (LUI_UP | LUI_DOWN)) {
            lui.row = (uint8_t) ((lui.row + ((down & LUI_UP) ? 2u : 1u)) % 3u);
        } else if (down & KEY_A) {
            if (lui.row == 2u) {
                legal.active = false;
                return;
            }
            legal.document = lui.row;
            lui.view = LUI_DOC;
            lui.top = 0;
            lui.rows = 0;
        } else return;
        lui.dirty = true;
    } else {
        int32_t before = lui.top;
        int32_t end;
        if (lui.view == LUI_DOC) {
            if (down & KEY_B) {
                lui.view = LUI_HUB;
                lui.dirty = true;
            } else if (step != 0) lui.top += step;
            else if (down & KEY_L) lui.top = 0;
            else if (down & KEY_R) lui.top = lui.rows;
            end = lui.rows - LUI_ROWS;
        } else {
            // The roll: half a pixel a frame after a second untouched; Up/Down move it 3 px a frame while held,
            // Left/Right/L/R a screen.
            if (held & (LUI_UP | LUI_DOWN)) {
                lui.top += (held & LUI_UP) ? -3 : 3;
                lui.idle = 0;
            } else if (down & (LUI_PREV | LUI_NEXT | KEY_L | KEY_R)) {
                lui.top += (down & (LUI_PREV | KEY_L)) ? -192 : 192;
                lui.idle = 0;
            } else if (++lui.idle > 30 && (lui.idle & 1) == 0) lui.top++;
            end = lui.height - LUI_PITCH * 8;
        }
        if (lui.top > end) lui.top = end;
        if (lui.top < 0) lui.top = 0;
        lui.dirty |= lui.top != before;
    }
    if (lui.dirty) luiDraw(menu);
}

// luiDrawHub joins heading, ' ', URL, a blank line, "Build:", ' ' and the build ID in t[128]; luiCat stops at 126
// bytes. The sizes below (NULs included; the build ID's storage is a leading NUL, the ID and its NUL) total at least
// 1 byte more than that text, so this bound keeps it whole for any URL CMake accepts.
_Static_assert(sizeof(CTR_RELEASE_SOURCE_HEADING) + sizeof(CTR_RELEASE_SOURCE_URL) + sizeof("Build:") +
               CTR_RELEASE_BUILD_ID_SIZE <= 126 + 1, "Legal page source and build text would not fit its buffer");
