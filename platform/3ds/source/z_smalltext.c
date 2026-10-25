// Crisp small text (smalltext, a test build for an on-console verdict), and UNDERTALE's release
// Smoothing choices (smoothux; below). Late code only (platform/3ds/late_reserve.ld,
// "smalltext"): nothing here moves existing code or data. With every key off (the --ini test keys) nothing here draws.
//
// The options menu's Smoothing row (ctr_optmenu.c) is Recommended or Custom; Custom opens a page that sets four parts,
// Smooth or Crisp, kept in settings.ini as smoothing_custom and crisp_text, crisp_bones, crisp_numbers, crisp_soul
// (1 Crisp; the parts' lines are the Custom choices, kept while Recommended is on). Recommended = Text Crisp, Bones
// Crisp, Numbers Smooth, Soul Smooth. What each part switches:
//   Text     crisp_ut_dialogue + crisp_papyrus + crisp_fix_w (the approved remakes; Sans stays as the PC font).
//   Bones    Sans's and Papyrus's bone sprites drawn sharp under Smart (ctr_krsharp.c gives their items the verdict
//            Smart keeps sharp; Smooth gives them back to the classifier).
//   Numbers  the damage numbers, MISS and the last hit's big 9s, the same way.
//   Soul     the player's soul drawn as an 8x8 Crisp soul in its own colour, one texel per screen pixel
//            at the screen pixel its top left is in, like the KR label below; Smooth draws the sprite as before.
// The battle HUD's remake (crisp_ut_hud, KR label included) is on in both. A part changed in the menu applies at once.
// The --ini test keys still win over all of it, so a test can turn any key off (every key off: the frames of a build
// without this file). Lines for the other Crisp keys in settings.ini are ignored and dropped at the menu's next save,
// but for crisp_carry (a release with test lines left from earlier test builds draws what the menu says). The menu's data
// comes from the build (tools/aot/stmenu.py, romfs:/fonts/<data.win folder>.dtsm, beside the glyph pack): the souls'
// names with their 8x8 and colours, the sprites Bones and Numbers keep sharp, and the Custom page's previews.
//
// The 3DS top screen shows a 640x480 room at half scale, so a font drawn 1:1 in 640-space lands at half size and
// smoothing averages it. With a Crisp key on, text in that font drawn at that size
// is drawn instead from glyphs derived from the player's own font by a fixed algorithm (tools/analysis/
// smalltext_derive.py), one texel per screen pixel: the build writes them to romfs:/fonts/<data.win folder>.dtst
// (tools/analysis/smalltext_pack.py, build.sh's "smalltext" word). Keys (settings.ini lines "<key> = 1", or --ini):
//   crisp_ut_dialogue  UNDERTALE's battle bubbles (fnt_plain)       crisp_dr_dialogue  DELTARUNE's (fnt_dotumche)
//   crisp_ut_hud       UNDERTALE's battle HUD (fnt_curs, and the KR label)  crisp_sans   Sans (fnt_comicsans)
//   crisp_papyrus      Papyrus (fnt_papyrus)
//   crisp_fix_w        the proposed W correction (fnt_plain, fnt_dotumche): its bolded band opened
//   crisp_fix_star     the proposed `*` correction: the feet lifted onto the stem row
//   crisp_carry        "carry" spacing: letters that have a gap on PC never touch (below);
//                      on unless set to 0, so it applies whenever a font key is on
//
// Which draws: the font has glyphs in the pack and its key is on; regular font, upright, scale 1, one colour, every
// character ASCII with a glyph; drawn on the application surface at one surface pixel per glyph pixel (the 640x480
// views; the surface goes to the screen at half size). Everything else, and any text when the pack is missing or
// stale, goes to the renderer's own text draw. Layout is the renderer's (lines, alignment, kerning). Each glyph is a
// part of one atlas texture per font (this file's own, made at the first draw; never a game sprite or surface), drawn
// at scale 2 as drawSpritePart draws a surface sprite's part (Smalltext_drawTexture), on whole screen pixels: a whole
// 640-space position lands on the screen pixel it is in. A fractional one (a shaken glyph) is moved by 2h - 0.5 first,
// h a fixed hash of the glyph's order in the frame, so the grid's rounding gives floor(x / 2 + h) and the jiggle keeps
// its mean size. Only a fractional x counts as shaken: a glyph at a whole x and a
// fractional y (Susie's bubbles sit at y 169.5) keeps its x and the grid's own rounding of y, so a steady line keeps
// one row, so a steady line doesn't jiggle. At a whole x the glyph's
// place is floor(x / 2) + left, moved a pixel toward the PC ink when that is a whole pixel off it (as
// for Jevil's T against the bubble, Susie's "Your head"): the pack's phase is the PC ink's offset from the pen
// minus 2 * left, and an odd pen with phase 1 (most letters of fnt_plain and fnt_dotumche, whose advance is 9) put
// the glyph a whole pixel left of the PC ink (smalltext_carry.nominal_x). Parity and screen pixels are the surface's:
// drawing x plus the view's offset (ctr_renderer.c updateGridMap's ox; Berdly's battle view is at x 119, so drawing
// units have the other parity there), rounded as the grid rounds, ceil(s - 0.5). Rows the same way for the whole
// font (Jevil's T sat 3 px above PC's): the pack's row shift for a line whose surface row (y
// plus the view's oy) is even or odd moves every glyph of a steady line down or up whole native rows, chosen on the
// font's letters so the baseline stays within half a native pixel of PC's and the tops come as close as they can
// (smalltext_carry.row_shifts; fnt_plain, fnt_dotumche and fnt_papyrus move down a row on an odd row).
//
// Carry (crisp_carry; tools/analysis/smalltext_carry.py is the reference model the checks use): each glyph is
// one fixed bitmap at floor(x / 2) + left, so letters with a gap on PC can touch. A line is the glyphs drawn in one
// frame, in one font, at one y, left to right (writers draw one character per call, so a few line records are kept
// across calls); a glyph left of the line's last one, or more than 4 advances right of it, starts a new line. With
// no line at its y, a glyph joins the nearest line whose last glyph is at most a quarter of the PC cell above or
// below and at most 4 advances to its left (wavy text: Snowdrake's letters bob one by one). Next
// to the last glyph (no space between them in one string, its pen at most the widest advance + 1 to the right) and
// where the PC glyphs have a gap (no 8-neighbour contact, from the pack's per-row PC extents, rows offset by the two
// glyphs' y), the line's carry is
// given back a pixel at a time while the glyph a pixel further left still clears the last one, then the glyph moves
// right a pixel while it touches (at most 2), and the carry keeps that. Every glyph of the line is drawn carry pixels
// right of its place. Only whole-x places take part; a glyph shaken in x keeps its jiggle and no carry.
//
// The KR label (crisp_ut_hud): Sans's HUD draws spr_krmeter, white letters 23x10 texels, at x 265 + maxhp * 1.2,
// so at a fractional place that smoothing averages to grey (blurry). The pack has
// its texels at even rows and columns as a 12x5 bitmap (fnt_curs variant 3); with the key on that bitmap is drawn
// instead, one texel per screen pixel, at the screen pixel the sprite's top left is in, in the sprite's colour.
//
// Side effects while a key is on (none with every key off): none the game can see. The atlases are textures of this
// file's own (an earlier build made them atlas sprites, which took sprite and surface indices the game's own later
// sprite_create_from_surface then didn't get, and UNDERTALE's state split at Muffet), kept until the program ends.
//
// The hook: an outer vtable in front of whatever table the renderer has (ctr_pixgrid.c's or ctr_pixfix_nslice.c's),
// with drawText and drawTextColor replaced (and drawSprite for the KR label); everything else is a copy, which
// setSkipWrappers keeps current as it keeps the state hash's. Every other draw goes to the table it was in front of
// (UNDERTALE: ctr_pixgrid.c's, whose drawSprite keeps the battle grid's ink; round 4b called ctrVtable's directly and
// lost it with any Crisp key on, caught by the capture diff in round 4c). Installed at each room change once the renderer is not on it yet: CtrRenderer_loadFontVerdicts
// (z_roomload.c's only call into the renderer at a room change) is wrapped (CMakeLists.txt), when a font key is on or
// the soul is Crisp (the HUD's key is on in both choices, so in play it always is; with every key off by --ini nothing
// is installed, and a menu change then takes effect from the next room). The keys given with --ini come through
// CtrArgui_override's chain (ctr_wrap_chain.h: optmenu -> smalltext -> argui). The options menu's save rewrites
// settings.ini with only the keys the display knows, so CtrArgui_saveEdits' chain writes crisp_carry back (if the file
// had it) and the menu's lines after.

#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_benchromfs.h"
#include "ctr_display.h"
#include "ctr_renderer.h"
#include "ctr_texture_pack.h"
#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "text_utils.h"

#define ST_LATE __attribute__((noinline, section(".text.late.smalltext")))
#define ST_CONST __attribute__((aligned(4), section(".rodata.smalltext_in_text")))
#define ST_BSS __attribute__((section(".bss.late.smalltext")))

#define ST_MAGIC 0x54535444u // "DTST"
#define ST_HEADER 28
#define ST_FONT_HEAD 32
#define ST_FONTS 6
#define ST_KEYS 8 // five fonts, the two corrections, carry
#define ST_KEY_FIX_W 6
#define ST_KEY_FIX_STAR 7
#define ST_KEY_CARRY 8
#define ST_LINES 6
#define ST_ATLAS_COLS 16
#define ST_FILE_MAX 65536
#define ST_MENU 5 // smoothing_custom, then the four parts' Custom choices
#define ST_ALL (ST_KEYS + ST_MENU)
#define ST_REC_PARTS 0x3u // Recommended: Text and Bones Crisp, Numbers and Soul Smooth
#define ST_MENU_MAGIC 0x4D535444u // "DTSM"
#define ST_MENU_HEADER 32
#define ST_SOUL_REC 52
#define ST_SHARP_REC 28
#define ST_SOULS 24
#define ST_SOUL_SLOTS 64
#define ST_SHARP_MAX 96
#define ST_SOUL_ENTRY 22 // an atlas entry like the glyph pack's: 6 bytes, then 8 u16 row masks
#define ST_MENU_MAX 65536

// ctr_renderer.c's end: the renderer's own vtable, and the offsets of the private fields read here.
extern const void* const Smalltext_refs[1];
extern const void* const Smalltext_textDraws[4]; // drawText's two forms (shown frame, frame skip), then drawTextColor's
extern const uint32_t Smalltext_layout[3];
enum { L_PROJECTION, L_VIEWPORT, L_BOUND_SURFACE };
typedef struct {
    int32_t x, y, w, h;
} StRect; // ctr_renderer.c's CtrRect

bool CtrPack_romfsName(char* name, size_t size);
// ctr_renderer.c's end (UNDERTALE): a sw x sh texel part of tex at (sx, sy) drawn at (x, y), two drawing units a texel.
void Smalltext_drawTexture(Renderer* renderer, C3D_Tex* tex, float sx, float sy, float sw, float sh, float x, float y, uint32_t color, float alpha);
void __real_CtrRenderer_loadFontVerdicts(Renderer* renderer);
void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer);
#ifdef ENABLE_WAD16
void CTR_FONTVERDICTS_AFTER_SMALLTEXT(Renderer* renderer);
#endif
bool __real_CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
bool CtrArgui_chainOverrideSmalltext(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __real_CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
void CtrArgui_chainSaveEditsSmalltext(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);

#ifndef ENABLE_WAD16
// DELTARUNE: Crisp text is UNDERTALE-only for now. The whole of it (about 8.7 KB of late code) does not fit
// in DELTARUNE Old's late code reserve (6631 B left at main 3befd6c21), and growing a reserve is a layout move.
// DELTARUNE links nothing from this file: ctr_optmenu.c maps the two argui link names to crispart's links
// instead, and CMakeLists.txt wraps CtrRenderer_loadFontVerdicts for UNDERTALE only.
#else

static const char stKeyFmt[ST_ALL][24] ST_CONST = {
    " crisp_ut_dialogue = %d", " crisp_dr_dialogue = %d", " crisp_ut_hud = %d", " crisp_sans = %d",
    " crisp_papyrus = %d",     " crisp_fix_w = %d",       " crisp_fix_star = %d",   " crisp_carry = %d",
    " smoothing_custom = %d",  " crisp_text = %d",        " crisp_bones = %d",      " crisp_numbers = %d",
    " crisp_soul = %d",
};
// The parts' names in the first build with the menu (1 = Crisp as now), read as the names above; a save writes
// the new names.
static const char stOldPartFmt[4][24] ST_CONST = { " smooth_text = %d", " smooth_bones = %d", " smooth_numbers = %d", " smooth_soul = %d" };
static const char stIniPath[] ST_CONST = CTRPORT_GAME_DIR "/settings.ini";
static const char stRead[] ST_CONST = "r";
static const char stAppend[] ST_CONST = "a";
static const char stReadBin[] ST_CONST = "rb";
static const char stRomfs[] ST_CONST = "romfs";
static const char stPackFmt[] ST_CONST = "romfs:/fonts/%s.dtst";
static const char stMenuFmt[] ST_CONST = "romfs:/fonts/%s.dtsm";
static const char stSoulName[] ST_CONST = "soul";
static const char stLogMenu[] ST_CONST = "Smoothing: %u souls (%u items), %u sharp items from %s\n";
static const char stLogNoMenu[] ST_CONST = "Smoothing: no menu data for this data (%s)\n";
static const char stLogKeys[] ST_CONST = "Crisp text: keys %02x; %u fonts from %s\n";
static const char stLogNoPack[] ST_CONST = "Crisp text: keys %02x, but no glyph pack for this data (%s)\n";
static const char stLogScale[] ST_CONST = "Crisp text: font %d first drawn at %d/1000 x %d/1000 surface px per glyph px\n";
static const char stLogAtlas[] ST_CONST = "Crisp text: %s atlas %dx%d in a %ux%u texture\n";
static const char stKrName[] ST_CONST = "spr_krmeter";
static const char stLogAtlasFail[] ST_CONST = "Crisp text: %s atlas %dx%d couldn't be made; the font draws as before\n";

typedef struct {
    int32_t font;       // data.win font index, -1 for none
    uint8_t key, rows, cell, count;
    uint8_t pcRows, adv;    // the PC cell's rows and widest advance (640 space)
    int8_t rowShift[2];     // native rows added on an even / odd surface row (round 4d)
    const uint8_t* entries; // count entries of 6 + 2 * rows + 2 * pcRows bytes (smalltext_pack.py; byte 5 the phase)
    uint8_t lut[128];   // entry + 1 of each ASCII character's derived glyph, 0 for none
    uint8_t fixW, fixStar; // entry + 1 of the corrections
    uint8_t kr;            // entry + 1 of the KR label (UNDERTALE's fnt_curs)
    int8_t made;        // the atlas: 1 made, 0 not yet, -1 failed
    C3D_Tex tex;        // the atlas, a texture of this file's own (never a game sprite or surface)
    char name[24];
} StFont;

// One line of carry spacing (see the top).
typedef struct {
    const StFont* font;
    const uint8_t* prev;     // the line's last glyph (its pack entry), nullptr: none
    float y, pen, py;        // the line's y, the last glyph's pen and y (640 space)
    int32_t frame, x, carry; // x: the last glyph's native x as drawn
    bool live;
} StLine;

// The sprites the Bones and Numbers parts keep sharp, read by ctr_krsharp.c once per frame: the items of a group whose
// bit (1 << group) is in `on` get the verdict Smart keeps sharp; a group turned off gives its items back once.
typedef struct {
    const DataWin* dataWin;
    uint32_t count;
    uint8_t on, applied;
    uint8_t group[ST_SHARP_MAX];
    int32_t item[ST_SHARP_MAX];
} CtrSmoothSharp;
CtrSmoothSharp CtrSmooth_sharp ST_BSS;

static struct {
    RendererVtable outer;
    RendererVtable* inner; // the renderer's own table (ctrVtable), whose entries setSkipWrappers swaps
    RendererVtable* prev;  // the table the outer one was put in front of (its drawSprite takes every other sprite)
    RendererVtable* text;  // the table that draws text not drawn crisp: prev when it wraps drawText (the state hash's),
    RendererVtable* textColor; // else inner (prev's entry is then a copy setSkipWrappers may have left); drawTextColor's
    int8_t given[ST_ALL];  // --ini values + 1 (0: not given), for this run only
    int8_t fileAt[ST_ALL]; // settings.ini's own lines when the keys were first read (values + 1, 0: no line)
    uint8_t touched, orig; // the menu's keys it changed this run (bit 0 Custom, bit k + 1 part k), and their values first
    uint32_t keys;         // bit k: key k (1..8) on
    uint8_t* file;
    uint32_t fontCount;
    StFont fonts[ST_FONTS];
    int32_t frame;  // runner->frameCount of the last crisp draw, and
    uint32_t slot;  // glyphs drawn crisp in that frame
    StLine lines[ST_LINES];
    uint32_t lineNext;
    int32_t krTpag; // spr_krmeter's frame 0 item + 1, 0 for none
    bool scaleLogged;
    bool keysRead;
    bool carryOff;         // the file's crisp_carry = 0
    uint8_t custom, parts; // the menu: Custom on, and the Custom choices (bit 0 Text, 1 Bones, 2 Numbers, 3 Soul: Crisp)
    bool soulOn;           // the Soul part in effect is Crisp
    const DataWin* menuDw; // the data the menu data was read for
    StFont soul;           // the Crisp souls as one atlas, an 8x8 entry per soul sprite
    uint8_t* soulEntries;
    uint8_t* soulMap;      // per data item: its soul slot + 1, 0 for none
    uint32_t soulMapCount;
    uint32_t soulSlots;
    uint8_t slotEntry[ST_SOUL_SLOTS];
    uint32_t slotColour[ST_SOUL_SLOTS]; // 0x00BBGGRR
    uint8_t* preview;      // the menu data while the options menu is open (CtrSmooth_menuOpen)
    uint32_t previewSize;
} st ST_BSS;

// A settings line: true if it is one of the keys (value 0 or 1), into values (value + 1).
static ST_LATE bool stParse(const char* line, int8_t* values) {
    int value;
    for (int32_t i = 0; ST_ALL + 4 > i; i++) {
        if (sscanf(line, ST_ALL > i ? stKeyFmt[i] : stOldPartFmt[i - ST_ALL], &value) == 1 && (uint32_t) value <= 1u) {
            values[ST_ALL > i ? i : i - 4] = (int8_t) (value + 1);
            return true;
        }
    }
    return false;
}

static ST_LATE void stResolve(void);

// --ini lines (main.c applies them after settings.ini, and after the options' boot migration may already have read
// the file): kept for this run only (a save never writes them), and in effect at once if the keys were read.
ST_LATE bool CtrArgui_chainOverrideSmalltext(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    if (!stParse(line, st.given)) return __real_CtrArgui_override(settings, line, caps);
    if (st.keysRead) stResolve();
    return true;
}

// The file's own key lines (values + 1, 0: no line).
static ST_LATE void stReadFile(const char* path, int8_t* values) {
    memset(values, 0, ST_ALL);
    FILE* f = fopen(path, stRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) stParse(line, values);
        fclose(f);
    }
}

// The keys in effect, from the menu's choices and the --ini test keys (see the top), at once: the text keys, the
// soul, and the sharp groups ctr_krsharp.c reads. Carry is on unless the file or --ini says crisp_carry = 0 (it was
// approved by eye); alone it draws nothing.
static ST_LATE void stApply(void) {
    uint32_t parts = st.custom ? st.parts : ST_REC_PARTS;
    uint32_t keys = 1u << 3 | (st.carryOff ? 0u : 1u << ST_KEY_CARRY); // the HUD's remake in both
    if ((parts & 1u) != 0) keys |= 1u << 1 | 1u << 5 | 1u << ST_KEY_FIX_W;
    for (int32_t i = 0; ST_KEYS > i; i++) {
        if (st.given[i] != 0) keys = st.given[i] == 2 ? keys | 1u << (i + 1) : keys & ~(1u << (i + 1));
    }
    st.keys = keys;
    st.soulOn = (parts & 8u) != 0;
    CtrSmooth_sharp.on = (uint8_t) (parts & 6u); // Bones: group 1's bit, Numbers: group 2's
}

// The menu's choices and carry from the file's lines read at the start, the --ini values over them, then what the menu
// changed this run over both. A part with no line takes Recommended's choice.
static ST_LATE void stResolve(void) {
    int8_t values[ST_ALL];
    for (int32_t i = 0; ST_ALL > i; i++) values[i] = st.given[i] != 0 ? st.given[i] : st.fileAt[i];
    st.carryOff = values[ST_KEY_CARRY - 1] == 1;
    uint32_t menu = values[ST_KEYS] == 2 ? 1u : 0u;
    for (uint32_t k = 0; 4u > k; k++) {
        int8_t v = values[ST_KEYS + 1 + k];
        if (v == 2 || (v == 0 && (ST_REC_PARTS >> k & 1u) != 0)) menu |= 2u << k;
    }
    uint32_t now = (uint32_t) st.custom | (uint32_t) st.parts << 1;
    menu = (menu & ~(uint32_t) st.touched) | (now & st.touched);
    st.custom = (uint8_t) (menu & 1u);
    st.parts = (uint8_t) (menu >> 1);
    stApply();
}

// Once per run (the first room change, the menu, or an --ini line after the boot's read), settings.ini's lines.
static ST_LATE void stEnsure(void) {
    if (st.keysRead) return;
    st.keysRead = true;
    stReadFile(stIniPath, st.fileAt);
    stResolve();
}

// The options menu's save (the display's rewrite drops the lines it doesn't know), then crisp_carry and the menu's
// five lines: a key the menu changed this run (to other than its value at the start) as the menu shows it, every other
// key as settings.ini had it (its line, or none), so --ini values never reach the file. The file's other Crisp lines are
// dropped (see the top); the parts' old names come back under the new ones.
ST_LATE void CtrArgui_chainSaveEditsSmalltext(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    int8_t values[ST_ALL];
    stReadFile(path, values);
    if (st.keysRead) memcpy(values, st.fileAt, sizeof(values));
    uint32_t now = (uint32_t) st.custom | (uint32_t) st.parts << 1, changed = (now ^ st.orig) & st.touched;
    __real_CtrArgui_saveEdits(before, after, path, caps);
    FILE* f = fopen(path, stAppend);
    if (f == nullptr) return;
    for (int32_t i = ST_KEY_CARRY - 1; ST_ALL > i; i++) {
        uint32_t bit = i >= ST_KEYS ? 1u << (i - ST_KEYS) : 0u;
        int32_t v = (changed & bit) != 0 ? (int32_t) (now & bit ? 1 : 0) : values[i] - 1;
        if (0 > v) continue;
        fprintf(f, stKeyFmt[i] + 1, (int) v); // the key's format without its leading space
        fputc('\n', f);
    }
    fclose(f);
}

// The options menu (ctr_optmenu.c, UNDERTALE): bit 7 Custom, bits 0..3 the Custom choices (Text, Bones, Numbers, Soul;
// set: Crisp). A change applies at once; the menu's save writes it.
ST_LATE uint32_t CtrSmooth_get(void) {
    stEnsure();
    return (uint32_t) st.custom << 7 | st.parts;
}

ST_LATE void CtrSmooth_set(uint32_t v) {
    stEnsure();
    uint32_t now = (uint32_t) st.custom | (uint32_t) st.parts << 1, next = (v >> 7 & 1u) | (v & 15u) << 1;
    uint32_t fresh = (now ^ next) & ~(uint32_t) st.touched;
    st.orig = (uint8_t) ((st.orig & ~fresh) | (now & fresh));
    st.touched |= (uint8_t) fresh;
    st.custom = (uint8_t) (next & 1u);
    st.parts = (uint8_t) (next >> 1);
    stApply();
}

static ST_LATE const uint8_t* stEntry(const StFont* f, uint32_t index) {
    return f->entries + (size_t) index * (6u + 2u * f->rows + 2u * f->pcRows);
}

// Do glyphs a (native x ax) and b (bx, to the right, its top dy native rows below a's) touch, on native rows at most
// one apart?
static ST_LATE bool stTouchNative(const StFont* f, const uint8_t* a, int32_t ax, const uint8_t* b, int32_t bx, int32_t dy) {
    for (int32_t r = 0; f->rows > r; r++) {
        uint32_t ma = (uint32_t) a[6 + 2 * r] | ((uint32_t) a[7 + 2 * r] << 8);
        if (ma == 0) continue;
        int32_t right = ax + 31 - __builtin_clz(ma);
        for (int32_t q = r - dy - 1; r - dy + 1 >= q; q++) {
            if (0 > q || q >= f->rows) continue;
            uint32_t mb = (uint32_t) b[6 + 2 * q] | ((uint32_t) b[7 + 2 * q] << 8);
            if (mb != 0 && 1 >= bx + __builtin_ctz(mb) - right) return true;
        }
    }
    return false;
}

// The same on PC: glyphs a (pen ax) and b (pen bx, top dy rows below a's) in 640 space, from the pack's per-row
// extents.
static ST_LATE bool stTouchPc(const StFont* f, const uint8_t* a, int32_t ax, const uint8_t* b, int32_t bx, int32_t dy) {
    const int8_t* ea = (const int8_t*) (a + 6 + 2 * f->rows);
    const int8_t* eb = (const int8_t*) (b + 6 + 2 * f->rows);
    for (int32_t r = 0; f->pcRows > r; r++) {
        if (ea[2 * r] > ea[2 * r + 1]) continue;
        int32_t right = ax + ea[2 * r + 1];
        for (int32_t q = r - dy - 1; r - dy + 1 >= q; q++) {
            if (0 > q || q >= f->pcRows || eb[2 * q] > eb[2 * q + 1]) continue;
            if (1 >= bx + eb[2 * q] - right) return true;
        }
    }
    return false;
}

// The native x of a whole-pixel glyph (entry g, pen in 640 space, nominal native x nx0) with carry spacing.
static ST_LATE int32_t stCarry(Renderer* renderer, const StFont* f, const uint8_t* g, float pen, float y, int32_t nx0, bool spaceBefore) {
    int32_t frame = renderer->runner != nullptr ? renderer->runner->frameCount : 0;
    StLine* line = nullptr;
    StLine* near = nullptr;
    float best = (float) (f->pcRows / 4); // wavy text: a glyph a little above or below its line's last one
    for (uint32_t i = 0; ST_LINES > i; i++) {
        StLine* l = &st.lines[i];
        if (!l->live || l->frame != frame || l->font != f) continue;
        if (l->y == y) line = l;
        float d = l->py > y ? l->py - y : y - l->py;
        // at least half an advance on: a shadow copy one pixel off never joins its text's line
        if (best >= d && pen - l->pen >= 0.5f * (float) f->adv && 4.0f * (float) f->adv >= pen - l->pen) {
            best = d;
            near = l;
        }
    }
    if (line == nullptr) line = near;
    if (line == nullptr) {
        line = &st.lines[st.lineNext++ % ST_LINES];
        line->live = true;
        line->frame = frame;
        line->font = f;
        line->y = y;
        line->py = y;
        line->prev = nullptr;
        line->carry = 0;
    } else if (line->pen >= pen || pen - line->pen > 4.0f * (float) f->adv) {
        line->prev = nullptr;
        line->carry = 0;
    }
    int32_t carry = line->carry, nx = nx0 + carry;
    int32_t dyPc = (int32_t) y - (int32_t) line->py, dy = ((int32_t) y >> 1) - ((int32_t) line->py >> 1);
    if (line->prev != nullptr && !spaceBefore && (float) f->adv + 1.0f >= pen - line->pen &&
        !stTouchPc(f, line->prev, (int32_t) line->pen, g, (int32_t) pen, dyPc)) {
        while (carry > 0 && !stTouchNative(f, line->prev, line->x, g, nx - 1, dy)) {
            carry--;
            nx--;
        }
        for (int32_t k = 0; 2 > k && stTouchNative(f, line->prev, line->x, g, nx, dy); k++) {
            carry++;
            nx++;
        }
    }
    line->carry = carry;
    line->pen = pen;
    line->py = y;
    line->prev = g;
    line->x = nx;
    return nx;
}

// A build file of this data.win (fmt: romfs:/fonts/<data.win folder>.<ext>), read whole into a new buffer (*n bytes).
static ST_LATE uint8_t* stReadRomfs(const char* fmt, char* file, size_t fileSize, size_t max, size_t* n) {
    *n = 0;
    char folder[64];
    if (!CtrPack_romfsName(folder, sizeof(folder))) return nullptr;
    snprintf(file, fileSize, fmt, folder);
    if (R_FAILED(CtrRomfs_init(stRomfs))) return nullptr;
    FILE* f = fopen(file, stReadBin);
    if (f == nullptr) return nullptr;
    uint8_t* buf = malloc(max);
    if (buf != nullptr) *n = fread(buf, 1, max, f);
    fclose(f);
    return buf;
}

// The pack of this data.win (romfs:/fonts/<data.win folder>.dtst), its fonts matched to the data's by name.
// Read only until the outer table is in (the renderer's first install), so before any atlas is made: the fonts' atlas
// fields are still empty when this clears them.
static ST_LATE bool stLoadPack(DataWin* dw, char* file, size_t fileSize) {
    free(st.file);
    st.file = nullptr;
    st.fontCount = 0;
    size_t n;
    uint8_t* buf = stReadRomfs(stPackFmt, file, fileSize, ST_FILE_MAX, &n);
    if (buf == nullptr) return false;
    uint32_t magic = 0, version = 0, size = 0, gameId = 0, fonts = 0;
    uint64_t timestamp = 0;
    bool ok = n >= ST_HEADER;
    if (ok) {
        memcpy(&magic, buf, 4);
        memcpy(&version, buf + 4, 4);
        memcpy(&size, buf + 8, 4);
        memcpy(&gameId, buf + 12, 4);
        memcpy(&timestamp, buf + 16, 8);
        memcpy(&fonts, buf + 24, 4);
        CtrPackKey key = CtrPack_keyFor(dw);
        ok = magic == ST_MAGIC && version == 2 && size == key.dataWinSize && gameId == key.gameId && timestamp == key.timestamp &&
             fonts <= ST_FONTS;
    }
    size_t at = ST_HEADER;
    for (uint32_t i = 0; ok && fonts > i; i++) {
        StFont* sf = &st.fonts[st.fontCount];
        if (at + ST_FONT_HEAD > n) {
            ok = false;
            break;
        }
        memset(sf, 0, sizeof(*sf));
        memcpy(sf->name, buf + at, 23);
        sf->key = buf[at + 24];
        sf->rows = buf[at + 25];
        sf->cell = buf[at + 26];
        sf->count = buf[at + 27];
        sf->pcRows = buf[at + 28];
        sf->adv = buf[at + 29];
        sf->rowShift[0] = (int8_t) buf[at + 30];
        sf->rowShift[1] = (int8_t) buf[at + 31];
        sf->entries = buf + at + ST_FONT_HEAD;
        sf->font = -1;
        at += ST_FONT_HEAD + (size_t) sf->count * (6u + 2u * sf->rows + 2u * sf->pcRows);
        if (at > n || sf->rows == 0 || sf->rows > 16 || sf->cell == 0 || sf->cell > 16 || sf->key == 0 || sf->key > 5 || sf->pcRows > 32) {
            ok = false;
            break;
        }
        for (uint32_t k = 0; dw->font.count > k; k++) {
            const Font* font = &dw->font.fonts[k];
            if (font->present && font->name != nullptr && strcmp(font->name, sf->name) == 0 && !font->isSpriteFont) sf->font = (int32_t) k;
        }
        if (0 > sf->font) continue;
        for (uint32_t e = 0; sf->count > e; e++) {
            const uint8_t* g = stEntry(sf, e);
            uint32_t ch = (uint32_t) g[0] | ((uint32_t) g[1] << 8);
            if (ch >= 128 || g[2] > sf->cell) continue;
            if (g[4] == 0) sf->lut[ch] = (uint8_t) (e + 1);
            else if (g[4] == 1 && ch == 'W') sf->fixW = (uint8_t) (e + 1);
            else if (g[4] == 2 && ch == '*') sf->fixStar = (uint8_t) (e + 1);
            else if (g[4] == 3) sf->kr = (uint8_t) (e + 1);
        }
        st.fontCount++;
    }
    if (!ok) {
        st.fontCount = 0;
        free(buf);
        return false;
    }
    st.file = buf;
    return true;
}

// The menu data's header against this data.win (tools/aot/stmenu.py): the soul, sharp and preview counts, or false.
static ST_LATE bool stMenuHeader(const uint8_t* buf, size_t n, const DataWin* dw, uint32_t* souls, uint32_t* sharp, uint32_t* previews) {
    if (buf == nullptr || ST_MENU_HEADER > n) return false;
    uint32_t magic, version, size, gameId;
    uint64_t timestamp;
    uint16_t c[3];
    memcpy(&magic, buf, 4);
    memcpy(&version, buf + 4, 4);
    memcpy(&size, buf + 8, 4);
    memcpy(&gameId, buf + 12, 4);
    memcpy(&timestamp, buf + 16, 8);
    memcpy(c, buf + 24, 6);
    CtrPackKey key = CtrPack_keyFor(dw);
    *souls = c[0];
    *sharp = c[1];
    *previews = c[2];
    return magic == ST_MENU_MAGIC && version == 1 && size == key.dataWinSize && gameId == key.gameId && timestamp == key.timestamp &&
           ST_SOULS >= c[0] && n >= ST_MENU_HEADER + (size_t) c[0] * ST_SOUL_REC + (size_t) c[1] * ST_SHARP_REC;
}

// The menu data, once per data.win: the Crisp souls (an atlas entry per soul sprite, each frame's item mapped to a
// slot with its colour) and the items Bones and Numbers keep sharp. Missing or stale data: no soul, no sharp groups.
static ST_LATE void stLoadMenu(const DataWin* dw) {
    st.menuDw = dw;
    free(st.soulMap);
    free(st.soulEntries);
    st.soulMap = nullptr;
    st.soulEntries = nullptr;
    st.soulSlots = 0;
    memset(&st.soul, 0, sizeof(st.soul)); // UNDERTALE has one data.win, read before the soul's atlas is made
    CtrSmooth_sharp.count = 0;
    CtrSmooth_sharp.applied = 0;
    CtrSmooth_sharp.dataWin = dw;
    char file[96];
    file[0] = '\0';
    size_t n;
    uint8_t* buf = stReadRomfs(stMenuFmt, file, sizeof(file), ST_MENU_MAX, &n);
    uint32_t souls, sharp, previews;
    if (!stMenuHeader(buf, n, dw, &souls, &sharp, &previews)) {
        free(buf);
        logInfo(stLogNoMenu, file);
        return;
    }
    st.soulEntries = calloc(souls != 0 ? souls : 1u, ST_SOUL_ENTRY);
    st.soulMap = calloc(dw->tpag.count != 0 ? dw->tpag.count : 1u, 1);
    st.soulMapCount = st.soulMap != nullptr ? dw->tpag.count : 0;
    const Sprt* sprt = &dw->sprt;
    for (uint32_t i = 0; st.soulEntries != nullptr && st.soulMap != nullptr && souls > i; i++) {
        const uint8_t* r = buf + ST_MENU_HEADER + (size_t) i * ST_SOUL_REC;
        uint8_t* e = st.soulEntries + (size_t) i * ST_SOUL_ENTRY;
        e[2] = 8; // width; char, left, variant and phase 0
        for (uint32_t y = 0; 8u > y; y++) e[6 + 2 * y] = r[44 + y];
        char name[25];
        memcpy(name, r, 24);
        name[24] = '\0';
        for (uint32_t s = 0; sprt->count > s; s++) {
            const Sprite* sp = &sprt->sprites[s];
            if (sp->name == nullptr || strcmp(sp->name, name) != 0) continue;
            for (uint32_t k = 0; sp->textureCount > k && r[24] > k && 4u > k && ST_SOUL_SLOTS > st.soulSlots; k++) {
                int32_t t = sp->tpagIndices[k];
                if (0 > t || st.soulMapCount <= (uint32_t) t) continue;
                st.slotEntry[st.soulSlots] = (uint8_t) i;
                memcpy(&st.slotColour[st.soulSlots], r + 28 + 4 * k, 4);
                st.soulMap[t] = (uint8_t) ++st.soulSlots;
            }
            break;
        }
    }
    if (st.soulEntries != nullptr) {
        st.soul.font = -1;
        st.soul.rows = 8;
        st.soul.cell = 8;
        st.soul.count = (uint8_t) souls;
        st.soul.entries = st.soulEntries;
        memcpy(st.soul.name, stSoulName, sizeof(stSoulName));
    }
    for (uint32_t i = 0; sharp > i; i++) {
        const uint8_t* r = buf + ST_MENU_HEADER + (size_t) souls * ST_SOUL_REC + (size_t) i * ST_SHARP_REC;
        char name[25];
        memcpy(name, r, 24);
        name[24] = '\0';
        for (uint32_t s = 0; sprt->count > s; s++) {
            const Sprite* sp = &sprt->sprites[s];
            if (sp->name == nullptr || strcmp(sp->name, name) != 0) continue;
            for (uint32_t k = 0; sp->textureCount > k && ST_SHARP_MAX > CtrSmooth_sharp.count; k++) {
                int32_t t = sp->tpagIndices[k];
                if (0 > t || dw->tpag.count <= (uint32_t) t) continue;
                CtrSmooth_sharp.group[CtrSmooth_sharp.count] = r[24];
                CtrSmooth_sharp.item[CtrSmooth_sharp.count++] = t;
            }
            break;
        }
    }
    free(buf);
    logInfo(stLogMenu, (unsigned) souls, (unsigned) st.soulSlots, (unsigned) CtrSmooth_sharp.count, file);
}

// The Custom page's previews (ctr_optmenu.c): the menu data is read when the options menu opens and freed when it
// closes. Part 0 Text, 1 Bones, 2 Numbers, 3 Soul; crisp 0 Smooth, 1 Crisp. RGB565 rows, top first; nullptr: none.
ST_LATE void CtrSmooth_menuOpen(void) {
    if (st.preview != nullptr || st.menuDw == nullptr) return;
    char file[96];
    size_t n;
    uint8_t* buf = stReadRomfs(stMenuFmt, file, sizeof(file), ST_MENU_MAX, &n);
    uint32_t souls, sharp, previews;
    if (!stMenuHeader(buf, n, st.menuDw, &souls, &sharp, &previews)) {
        free(buf);
        return;
    }
    st.preview = buf;
    st.previewSize = (uint32_t) n;
}

ST_LATE void CtrSmooth_menuClose(void) {
    free(st.preview);
    st.preview = nullptr;
}

ST_LATE const uint16_t* CtrSmooth_preview(uint32_t part, uint32_t crisp, uint32_t* w, uint32_t* h) {
    uint32_t souls, sharp, previews;
    if (st.preview == nullptr || !stMenuHeader(st.preview, st.previewSize, st.menuDw, &souls, &sharp, &previews)) return nullptr;
    size_t at = ST_MENU_HEADER + (size_t) souls * ST_SOUL_REC + (size_t) sharp * ST_SHARP_REC;
    for (uint32_t i = 0; previews > i && st.previewSize >= at + 8; i++) {
        const uint8_t* p = st.preview + at;
        uint32_t pw = (uint32_t) p[2] | (uint32_t) p[3] << 8, ph = (uint32_t) p[4] | (uint32_t) p[5] << 8;
        size_t bytes = ((size_t) pw * ph * 2u + 3u) & ~(size_t) 3u;
        if (at + 8 + bytes > st.previewSize) return nullptr;
        if (p[0] == part && p[1] == crisp) {
            *w = pw;
            *h = ph;
            return (const uint16_t*) (p + 8);
        }
        at += 8 + bytes;
    }
    return nullptr;
}

// The font's atlas, made the first time: a texture of this file's own, drawn by Smalltext_drawTexture
// (ctr_renderer.c's end). Never a game sprite or surface: the game's own sprite_create_from_surface and surface_create
// calls get the same indices with Crisp parts on or off (an earlier build made the atlases with
// createSpriteFromSurface, which moved those indices and with them the game's state). Texels top row first, as the
// renderer's tiles; RGBA8 white where the glyph has ink.
static ST_LATE bool stAtlas(StFont* f) {
    if (f->made != 0) return f->made > 0;
    f->made = -1;
    int32_t pitchX = f->cell + 1, pitchY = f->rows + 1;
    int32_t w = ST_ATLAS_COLS * pitchX, h = ((f->count + ST_ATLAS_COLS - 1) / ST_ATLAS_COLS) * pitchY;
    uint32_t tw = 8, th = 8;
    while ((int32_t) tw < w) tw <<= 1;
    while ((int32_t) th < h) th <<= 1;
    if (tw > 1024u || th > 1024u || !C3D_TexInit(&f->tex, (u16) tw, (u16) th, GPU_RGBA8)) {
        logInfo(stLogAtlasFail, f->name, (int) w, (int) h);
        return false;
    }
    memset(f->tex.data, 0, f->tex.size);
    uint32_t* texels = (uint32_t*) f->tex.data;
    for (uint32_t e = 0; f->count > e; e++) {
        const uint8_t* g = stEntry(f, e);
        uint32_t cx = (e % ST_ATLAS_COLS) * (uint32_t) pitchX, cy = (e / ST_ATLAS_COLS) * (uint32_t) pitchY;
        for (uint32_t y = 0; f->rows > y; y++) {
            uint32_t mask = (uint32_t) g[6 + 2 * y] | ((uint32_t) g[7 + 2 * y] << 8);
            for (uint32_t x = 0; g[2] > x; x++) {
                if ((mask >> x & 1u) == 0) continue;
                uint32_t tx = cx + x, ty = cy + y, mx = tx & 7u, my = ty & 7u;
                uint32_t morton = (mx & 1u) | ((my & 1u) << 1) | ((mx & 2u) << 1) | ((my & 2u) << 2) | ((mx & 4u) << 2) | ((my & 4u) << 3);
                texels[(ty >> 3) * (tw / 8u) * 64u + (tx >> 3) * 64u + morton] = 0xFFFFFFFFu;
            }
        }
    }
    C3D_TexFlush(&f->tex);
    C3D_TexSetFilter(&f->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&f->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    f->made = 1;
    logInfo(stLogAtlas, f->name, (int) w, (int) h, (unsigned) tw, (unsigned) th);
    return true;
}

// Glyph e of atlas f at (x, y) in drawing units, two per texel, as the renderer draws a sprite part there (its pixel
// grid included).
static ST_LATE void stBlit(Renderer* renderer, StFont* f, uint32_t e, uint32_t w, float x, float y, uint32_t color, float alpha) {
    float sx = (float) ((e % ST_ATLAS_COLS) * (uint32_t) (f->cell + 1)), sy = (float) ((e / ST_ATLAS_COLS) * (uint32_t) (f->rows + 1));
    Smalltext_drawTexture(renderer, &f->tex, sx, sy, (float) w, (float) f->rows, x, y, color, alpha);
}

// A fixed hash of the glyph's order in the frame, in [0, 1).
static ST_LATE float stSlotHash(Renderer* renderer) {
    int32_t frame = renderer->runner != nullptr ? renderer->runner->frameCount : 0;
    if (frame != st.frame) {
        st.frame = frame;
        st.slot = 0;
    }
    uint32_t v = (st.slot++ + 1u) * 2654435761u;
    v ^= v >> 15;
    v *= 2246822519u;
    v ^= v >> 13;
    return (float) (v >> 8) * (1.0f / 16777216.0f);
}

// Whether the renderer draws to the application surface at one surface pixel per 640-space pixel (the 640x480
// views); fontIndex is for the first time's log line.
static ST_LATE bool stOneToOne(Renderer* renderer, int32_t fontIndex) {
    const uint8_t* rp = (const uint8_t*) renderer;
    if (renderer->runner == nullptr || *(const int32_t*) (rp + Smalltext_layout[L_BOUND_SURFACE]) != renderer->runner->applicationSurfaceId) return false;
    const C3D_Mtx* m = (const C3D_Mtx*) (rp + Smalltext_layout[L_PROJECTION]);
    const StRect* vp = (const StRect*) (rp + Smalltext_layout[L_VIEWPORT]);
    if (m->r[0].y != 0.0f || m->r[1].x != 0.0f) return false;
    float kx = m->r[0].x * 0.5f * (float) vp->w, ky = m->r[1].y * 0.5f * (float) vp->h;
    if (0.0f > kx) kx = -kx;
    if (0.0f > ky) ky = -ky;
    if (!st.scaleLogged && fontIndex >= 0) { // the soul (-1) is no font
        st.scaleLogged = true;
        logInfo(stLogScale, (int) fontIndex, (int) (kx * 1000.0f), (int) (ky * 1000.0f));
    }
    return kx >= 0.99f && kx <= 1.01f && ky >= 0.99f && ky <= 1.01f;
}

// The surface x of drawing x 0 (ctr_renderer.c updateGridMap's ox), 0 when the view's x axis is mirrored.
static ST_LATE float stSurfaceOx(Renderer* renderer) {
    const uint8_t* rp = (const uint8_t*) renderer;
    const C3D_Mtx* m = (const C3D_Mtx*) (rp + Smalltext_layout[L_PROJECTION]);
    const StRect* vp = (const StRect*) (rp + Smalltext_layout[L_VIEWPORT]);
    if (0.0f >= m->r[0].x) return 0.0f;
    return (m->r[0].w + 1.0f) * 0.5f * (float) vp->w + (float) vp->x;
}

// The surface y of drawing y 0 (ctr_renderer.c updateGridMap's oy), 0 when the view's y axis is mirrored.
static ST_LATE float stSurfaceOy(Renderer* renderer) {
    const uint8_t* rp = (const uint8_t*) renderer;
    const C3D_Mtx* m = (const C3D_Mtx*) (rp + Smalltext_layout[L_PROJECTION]);
    const StRect* vp = (const StRect*) (rp + Smalltext_layout[L_VIEWPORT]);
    if (m->r[1].y >= 0.0f) return 0.0f;
    return (1.0f - m->r[1].w) * 0.5f * (float) vp->h + (float) vp->y;
}

// ceil(s - 0.5): the surface pixel the grid puts surface place s on (whole places stay; a view offset's float error
// rounds away).
static ST_LATE int32_t stGridPixel(float s) {
    float t = s - 0.5f;
    int32_t i = (int32_t) t;
    if (t > (float) i) i++;
    return i;
}

// floor(v / 2) for a 640-space place: the screen pixel it is in.
static ST_LATE int32_t stHalfFloor(float v) {
    int32_t i = (int32_t) v;
    if ((float) i > v) i--;
    return i >> 1;
}

// Text drawn crisp if it can be; false: the caller draws it as before.
static ST_LATE bool stDraw(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, uint32_t color,
                           float alpha, float lineSeparation) {
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (text == nullptr || angleDeg != 0.0f || 0 > fontIndex || dw == nullptr || dw->font.count <= (uint32_t) fontIndex) return false;
    StFont* f = nullptr;
    for (uint32_t i = 0; st.fontCount > i; i++) {
        if (st.fonts[i].font == fontIndex && (st.keys >> st.fonts[i].key & 1u) != 0) f = &st.fonts[i];
    }
    if (f == nullptr || 0 > f->made) return false;
    Font* font = &dw->font.fonts[fontIndex];
    float sx = xscale * font->scaleX, sy = yscale * font->scaleY;
    if (sx != 1.0f || sy != 1.0f || !stOneToOne(renderer, fontIndex)) return false;

    int32_t textLen = (int32_t) strlen(text);
    if (textLen == 0) return true;
    for (int32_t i = 0; textLen > i; i++) {
        uint8_t c = (uint8_t) text[i];
        if (c >= 128) return false;
        if (c == ' ' || TextUtils_isNewlineChar((char) c)) continue;
        if (f->lut[c] == 0 || TextUtils_findGlyph(font, (uint16_t) c) == nullptr) return false;
    }
    if (!stAtlas(f)) return false;

    bool fixW = (st.keys >> ST_KEY_FIX_W & 1u) != 0 && f->fixW != 0;
    bool fixStar = (st.keys >> ST_KEY_FIX_STAR & 1u) != 0 && f->fixStar != 0;
    bool carry = (st.keys >> ST_KEY_CARRY & 1u) != 0;
    float ox = stSurfaceOx(renderer), oy = stSurfaceOy(renderer);
    int32_t lineCount = TextUtils_countLines(text, textLen);
    float lineStride = (0.0f > lineSeparation) ? TextUtils_lineStride(font) : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -(float) lineCount * lineStride / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -(float) lineCount * lineStride;
    float cursorY = valignOffset - (float) font->ascenderOffset;
    int32_t lineStart = 0;
    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !TextUtils_isNewlineChar(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;
        float halignOffset = 0.0f;
        if (renderer->drawHalign != 0) {
            float lineWidth = TextUtils_measureLineWidth(font, text + lineStart, lineLen);
            halignOffset = renderer->drawHalign == 1 ? -lineWidth / 2.0f : -lineWidth;
        }
        float cursorX = halignOffset;
        bool space = false;
        for (int32_t i = 0; lineLen > i; i++) {
            uint8_t ch = (uint8_t) text[lineStart + i];
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (ch == ' ' || glyph->sourceWidth == 0 || glyph->sourceHeight == 0) space = true;
            else {
                uint32_t e = (uint32_t) f->lut[ch] - 1u;
                if (ch == 'W' && fixW) e = (uint32_t) f->fixW - 1u;
                else if (ch == '*' && fixStar) e = (uint32_t) f->fixStar - 1u;
                const uint8_t* g = stEntry(f, e);
                if (g[2] != 0) {
                    float pen = x + cursorX, gx = pen + 2.0f * (float) (int8_t) g[3], gy = y + cursorY;
                    bool shakeX = gx != (float) (int32_t) gx, shakeY = gy != (float) (int32_t) gy;
                    // A fractional x (a shaken glyph): floor(v / 2 + h) on the screen after the grid's rounding, for x
                    // and a fractional y. A whole x and a fractional y (Susie's bubbles at y 169.5) is steady text: y
                    // keeps the grid's rounding, so the line's glyphs share a row. The slot is taken as before either way.
                    if (shakeX || shakeY) {
                        float d = 2.0f * stSlotHash(renderer) - 0.5f;
                        if (shakeX) {
                            gx += d;
                            if (shakeY) gy += d;
                        }
                    }
                    if (!shakeX) {
                        // the PC phase: 2 * floor(surface x / 2) against the PC ink's first column, in 640 px
                        int32_t p = stGridPixel(pen + ox), off = -(p & 1) - (int32_t) (int8_t) g[5];
                        if (-2 >= off) gx += 2.0f * (float) (-off / 2);
                        else if (off >= 2) gx -= 2.0f * (float) (off / 2);
                        gy += 2.0f * (float) f->rowShift[stGridPixel(gy + oy) & 1]; // the font's rows against PC's
                        if (carry) {
                            int32_t nx0 = stGridPixel(gx + ox) >> 1; // the screen pixel the grid puts it on
                            gx += 2.0f * (float) (stCarry(renderer, f, g, pen, gy, nx0, space) - nx0);
                        }
                    }
                    space = false;
                    stBlit(renderer, f, e, g[2], gx, gy, color, alpha);
                }
            }
            cursorX += glyph->shift;
            if (lineLen > i + 1) cursorX += TextUtils_getKerningOffset(glyph, (uint8_t) text[lineStart + i + 1]);
        }
        cursorY += lineStride;
        lineStart = textLen > lineEnd ? TextUtils_skipNewline(text, lineEnd, textLen) : lineEnd;
    }
    return true;
}

// The KR label crisp (see the top); every other sprite as before.
static ST_LATE void stDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale,
                                 float angleDeg, uint32_t color, float alpha) {
    if (st.krTpag != 0 && tpagIndex == st.krTpag - 1 && xscale == 1.0f && yscale == 1.0f && angleDeg == 0.0f) {
        for (uint32_t i = 0; st.fontCount > i; i++) {
            StFont* f = &st.fonts[i];
            if (f->kr == 0 || (st.keys >> f->key & 1u) == 0 || 0 > f->font) continue;
            if (!stOneToOne(renderer, f->font) || !stAtlas(f)) break;
            uint32_t e = (uint32_t) f->kr - 1u;
            stBlit(renderer, f, e, stEntry(f, e)[2], (float) (2 * stHalfFloor(x - originX)), (float) (2 * stHalfFloor(y - originY)), color, alpha);
            return;
        }
    }
    // The Crisp soul: its 8x8 in the frame's colour times the draw's, one texel per screen pixel at the screen pixel the
    // sprite's top left is in (surface places, as the text's).
    if (st.soulOn && st.soulMapCount > (uint32_t) tpagIndex && st.soulMap[tpagIndex] != 0 && xscale == 1.0f && yscale == 1.0f &&
        angleDeg == 0.0f && stOneToOne(renderer, -1) && stAtlas(&st.soul)) {
        uint32_t slot = st.soulMap[tpagIndex] - 1u, e = st.slotEntry[slot], c = st.slotColour[slot], mixed = 0;
        for (uint32_t sh = 0; 24u > sh; sh += 8) mixed |= (((c >> sh & 255u) * (color >> sh & 255u) + 127u) / 255u) << sh;
        float ox = stSurfaceOx(renderer), oy = stSurfaceOy(renderer);
        stBlit(renderer, &st.soul, e, 8u, (float) (2 * stHalfFloor(x - originX + ox)) - ox, (float) (2 * stHalfFloor(y - originY + oy)) - oy, mixed,
               alpha);
        return;
    }
    st.prev->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

static ST_LATE void stDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    if (stDraw(renderer, text, x, y, xscale, yscale, angleDeg, renderer->drawColor, renderer->drawAlpha, lineSeparation)) return;
    st.text->drawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation);
}

static ST_LATE void stDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1,
                                    int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    if (c1 == c2 && c2 == c3 && c3 == c4 && stDraw(renderer, text, x, y, xscale, yscale, angleDeg, (uint32_t) c1, alpha, lineSeparation)) return;
    st.textColor->drawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
}

// At each room change: a renderer not yet on the outer table gets it when a Crisp key is on and the pack has glyphs.
ST_LATE void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer) {
    CTR_FONTVERDICTS_AFTER_SMALLTEXT(renderer);
    if (renderer == nullptr || renderer->vtable == &st.outer || renderer->dataWin == nullptr) return;
    // The menu's choices and keys, once per run (settings.ini, then --ini), and the menu data once per data.win: with
    // no font key on and the soul Smooth, nothing more happens (a later change in the menu then takes the next room).
    stEnsure();
    if (st.menuDw != renderer->dataWin) stLoadMenu(renderer->dataWin);
    uint32_t keys = st.keys;
    if ((keys & 0x3Eu) == 0 && !st.soulOn) return;
    st.inner = (RendererVtable*) Smalltext_refs[0];
    char file[96];
    file[0] = '\0';
    if (!stLoadPack(renderer->dataWin, file, sizeof(file))) {
        logInfo(stLogNoPack, (unsigned) keys, file);
        return;
    }
    logInfo(stLogKeys, (unsigned) keys, (unsigned) st.fontCount, file);
    st.scaleLogged = false;
    st.prev = renderer->vtable;
    const void* t = (const void*) st.prev->drawText;
    const void* tc = (const void*) st.prev->drawTextColor;
    st.text = t == Smalltext_textDraws[0] || t == Smalltext_textDraws[1] ? st.inner : st.prev;
    st.textColor = tc == Smalltext_textDraws[2] || tc == Smalltext_textDraws[3] ? st.inner : st.prev;
    st.outer = *renderer->vtable;
    st.outer.drawText = stDrawText;
    st.outer.drawTextColor = stDrawTextColor;
    st.krTpag = 0;
    for (uint32_t i = 0; st.fontCount > i; i++) {
        if (st.fonts[i].kr == 0) continue;
        const Sprt* sprt = &renderer->dataWin->sprt;
        for (uint32_t k = 0; sprt->count > k; k++) {
            const Sprite* sp = &sprt->sprites[k];
            if (sp->name != nullptr && sp->textureCount > 0 && strcmp(sp->name, stKrName) == 0) st.krTpag = sp->tpagIndices[0] + 1;
        }
        if (st.krTpag != 0) st.outer.drawSprite = stDrawSprite;
    }
    if (st.soulSlots != 0) st.outer.drawSprite = stDrawSprite;
    renderer->vtable = &st.outer;
}
#endif // ENABLE_WAD16
