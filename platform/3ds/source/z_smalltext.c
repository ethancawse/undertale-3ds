// Crisp small text (smalltext, a test build for an on-console verdict), and UNDERTALE's release
// Smoothing choices (smoothux; below). Late code only (platform/3ds/late_reserve.ld,
// "smalltext"): nothing here moves existing code or data. With every key off (the --ini test keys) nothing here draws.
//
// The options menu's Smoothing row (ctr_optmenu.c) is Recommended or Custom; Custom opens a page that sets four parts,
// Smooth or Crisp, kept in settings.ini as smoothing_custom and crisp_text, crisp_bones, crisp_numbers, crisp_soul
// (1 Crisp; the parts' lines are the Custom choices, kept while Recommended is on). Recommended = Text Crisp, Bones
// Crisp, Numbers Smooth, Soul Smooth. What each part switches:
//   Text     crisp_ut_dialogue + crisp_sans + crisp_papyrus + crisp_fix_w (the remakes; Sans's bubbles, sansfont).
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
// The Crisp cards (round 18, playtest 10-25: Mettaton EX's "HAPPY BREAKTIME" card "looked broken"): with the Text part
// Crisp (key crisp_ut_dialogue), each frame of a card in the menu data (stmenu.py: its letters derived from the
// player's data.win as half-size glyphs, no hand-drawn pixel) is drawn instead of the sprite, one texel per screen
// pixel at the screen pixel the sprite's top left is in, opaque black where no letter is, in the draw's colour and
// alpha. With Text Smooth the card is the sprite as before (Smart's averaged card, ctr_krsharp.c).
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
//
// DELTARUNE on New 3DS (sansfont round 3, playtest 10-26: "all new fonts approved for crisp mode"): the same code,
// compiled through z_smalltext_dr.c (ST_DR_UNIT) with sections named smalltext_dr, linked last (late_reserve.ld), so
// nothing merged before it moves. DELTARUNE has no Smoothing parts menu: the Text part is on (Recommended's choice) and
// adds crisp_dr_dialogue (fnt_dotumche, the enemies' bubbles); fnt_comicsans (Sans) is the UNDERTALE remake, the same
// font in every chapter. The --ini test keys and a settings.ini crisp_text = 0 line turn it off as in UNDERTALE. No
// soul, cards, KR label or menu data. Each chapter has its own pack (romfs:/fonts/chapterN_windows.dtst) and renderer:
// the outer table's destroy frees the atlases and the pack before the renderer's own, and the next chapter's first room
// reads its pack. The drawing goes through crispart's Crispart_drawTexture (ctr_renderer.c, DELTARUNE), with the
// glyph's top left put on the grid's even surface pixel here, as Smalltext_drawTexture's grid snap does for UNDERTALE.
// DELTARUNE Old builds compile both files empty: its late code reserve can't hold this.

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

#ifdef ST_DR_UNIT
#define ST_LATE __attribute__((noinline, section(".text.late.smalltext_dr")))
#define ST_CONST __attribute__((aligned(4), section(".rodata.smalltext_dr_in_text")))
#define ST_BSS __attribute__((section(".bss.late.smalltext_dr")))
#else
#define ST_LATE __attribute__((noinline, section(".text.late.smalltext")))
#define ST_CONST __attribute__((aligned(4), section(".rodata.smalltext_in_text")))
#define ST_BSS __attribute__((section(".bss.late.smalltext")))
#endif
// The UNDERTALE Old layout event (integrate23, ruling 2026-10-27): there cullcrisp lives inside stDraw (about
// 60-200 B) instead of in its out-of-line entries at the file's end (1,944 B), which moves the late code after stDraw
// once. Every other target keeps the out-of-line entries and its layout.
#if defined(CTRPORT_TARGET_O3DS) && !defined(ST_DR_UNIT)
#define ST_CULL_INLINE 1
#endif

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
#define ST_CARD_REC 28  // a card frame: sprite name[24], frame, entries, rows, 0; then its atlas entries
#define ST_CARDS 2      // card frames drawn crisp (Mettaton EX's break card: the card and its blink)
#define ST_CARD_PARTS 4 // atlas entries across a card, 16 pixels each

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
#if defined(ENABLE_WAD16) && !defined(ST_DR_UNIT)
void Asrtext_load(const uint8_t* data, size_t size);
const uint8_t* Asrtext_match(const char* text, size_t len, int32_t font);
int Asrtext_motion(const char* text, size_t len, int32_t font);
#endif
#line 180
void __real_CtrRenderer_loadFontVerdicts(Renderer* renderer);
void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer);
#if defined(ENABLE_WAD16) || defined(ST_DR_UNIT)
void CTR_FONTVERDICTS_AFTER_SMALLTEXT(Renderer* renderer);
#endif
#ifdef ST_DR_UNIT
// DELTARUNE New: the argui chain's next links are crispart's (ctr_wrap_chain.h: optmenu -> smalltext -> crispart ->
// argui); crispart's glyph draw (ctr_renderer.c, DELTARUNE).
#include "ctr_crispart.h"
#define __real_CtrArgui_override CtrArgui_chainOverrideCrispart
#define __real_CtrArgui_saveEdits CtrArgui_chainSaveEditsCrispart
void Crispart_drawTexture(Renderer* renderer, C3D_Tex* tex, const CaFrame* frame, float x, float y, uint32_t color, float alpha);
#endif
bool __real_CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
bool CtrArgui_chainOverrideSmalltext(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __real_CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);
void CtrArgui_chainSaveEditsSmalltext(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);

#if !defined(ENABLE_WAD16) && !defined(ST_DR_UNIT)
// DELTARUNE compiled as itself: nothing (New 3DS builds compile this file through z_smalltext_dr.c instead, see the
// top). The whole of it (about 11 KB of late code) does not fit in DELTARUNE Old's late code reserve (7487 B left at
// cand20), and growing a reserve is a layout move. DELTARUNE Old links nothing from this file: ctr_optmenu.c maps the
// two argui link names to crispart's links instead.
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
#ifndef ST_DR_UNIT
static const char stMenuFmt[] ST_CONST = "romfs:/fonts/%s.dtsm";
static const char stSoulName[] ST_CONST = "soul";
static const char stCardName[] ST_CONST = "card";
static const char stLogMenu[] ST_CONST = "Smoothing: %u souls (%u items), %u sharp items from %s\n";
static const char stLogNoMenu[] ST_CONST = "Smoothing: no menu data for this data (%s)\n";
#endif
static const char stLogKeys[] ST_CONST = "Crisp text: keys %02x; %u fonts from %s\n";
static const char stLogNoPack[] ST_CONST = "Crisp text: keys %02x, but no glyph pack for this data (%s)\n";
static const char stLogScale[] ST_CONST = "Crisp text: font %d first drawn at %d/1000 x %d/1000 surface px per glyph px\n";
static const char stLogAtlas[] ST_CONST = "Crisp text: %s atlas %dx%d in a %ux%u texture\n";
#ifndef ST_DR_UNIT
static const char stKrName[] ST_CONST = "spr_krmeter";
#endif
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
#ifndef ST_DR_UNIT
// The Crisp cards (see the top), defined before the other state so that the compiler puts them after it in the bss
// (GCC emits a file's statics last first) and the older state keeps its place.
static struct {
    StFont font;   // ST_CARD_PARTS entries per frame; stAtlas makes its pixels without ink opaque black
    uint8_t* entries;
    int32_t tpag[ST_CARDS]; // each card frame's item + 1, 0 for none
} stCard ST_BSS;
CtrSmoothSharp CtrSmooth_sharp ST_BSS;
#endif

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
#ifdef ST_DR_UNIT
    if ((parts & 1u) != 0) keys |= 1u << 1 | 1u << 2 | 1u << 4 | 1u << 5 | 1u << ST_KEY_FIX_W; // and DELTARUNE's bubbles
#else
    if ((parts & 1u) != 0) keys |= 1u << 1 | 1u << 4 | 1u << 5 | 1u << ST_KEY_FIX_W;
#endif
    for (int32_t i = 0; ST_KEYS > i; i++) {
        if (st.given[i] != 0) keys = st.given[i] == 2 ? keys | 1u << (i + 1) : keys & ~(1u << (i + 1));
    }
    st.keys = keys;
    st.soulOn = (parts & 8u) != 0;
#ifndef ST_DR_UNIT
    CtrSmooth_sharp.on = (uint8_t) (parts & 6u); // Bones: group 1's bit, Numbers: group 2's
#endif
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

#ifndef ST_DR_UNIT
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
#endif

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

#ifndef ST_DR_UNIT
static int32_t stBubbleClip(Runner* runner, const StFont* f, float pen); // bubbleclip (the file's end)
static int32_t asrPcDy(Renderer* renderer, const StFont* f, float y, float py, int32_t dy);
#line 474
#endif
#ifdef CTRPORT_BUBBLE_CHECK
// out of line, so none of it lands in stDraw's late text
static __attribute__((noinline)) void bckGlyph(Renderer* renderer, const StFont* f, const uint8_t* g, float pen, float y, float gx, float gy, float ox, float oy);
#endif
// The native x of a whole-pixel glyph (entry g, pen in 640 space, nominal native x nx0) with carry spacing.
static ST_LATE int32_t stCarry(Renderer* renderer, const StFont* f, const uint8_t* g, float pen, float y, int32_t nx0, bool spaceBefore) {
#ifndef ST_DR_UNIT
    int32_t bubbleShift = stBubbleClip(renderer->runner, f, pen);
#endif
#line 481
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
#ifndef ST_DR_UNIT
    dyPc = asrPcDy(renderer, f, y, line->py, dyPc);
#endif
#line 512
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
#ifdef ST_DR_UNIT
    return nx;
#else
    return nx + bubbleShift; // a writer's line in its bubble may give pixels back (the file's end)
#endif
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
#ifndef ST_DR_UNIT
    Asrtext_load(nullptr, 0);
#endif
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
#ifdef ST_DR_UNIT
    // Kept only as long as the pack (a chapter's is about 12 KB, not the 64 KiB read buffer). The entries move with it.
    size_t off[ST_FONTS];
    for (uint32_t i = 0; st.fontCount > i; i++) off[i] = (size_t) (st.fonts[i].entries - buf);
    uint8_t* kept = realloc(buf, n);
    if (kept != nullptr) {
        buf = kept;
        for (uint32_t i = 0; st.fontCount > i; i++) st.fonts[i].entries = buf + off[i];
    }
#endif
#ifndef ST_DR_UNIT
    Asrtext_load(buf + at, n - at);
#endif
    st.file = buf;
    return true;
}
#line 632

#ifndef ST_DR_UNIT
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
    free(stCard.entries);
    stCard.entries = nullptr;
    memset(&stCard.font, 0, sizeof(stCard.font));
    memset(stCard.tpag, 0, sizeof(stCard.tpag));
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
    // The cards, after the previews (the header's last count; 0 in a file from before them): ST_CARD_PARTS atlas
    // entries per frame, every frame with the same rows.
    size_t at = ST_MENU_HEADER + (size_t) souls * ST_SOUL_REC + (size_t) sharp * ST_SHARP_REC;
    for (uint32_t i = 0; previews > i && n >= at + 8; i++)
        at += 8 + ((((size_t) buf[at + 2] | (size_t) buf[at + 3] << 8) * ((size_t) buf[at + 4] | (size_t) buf[at + 5] << 8) * 2u + 3u) & ~(size_t) 3u);
    uint32_t cards = (uint32_t) buf[30] | (uint32_t) buf[31] << 8, rows = n >= at + ST_CARD_REC ? buf[at + 26] : 0u;
    size_t bytes = ST_CARD_PARTS * (6u + 2u * rows), rec = (ST_CARD_REC + bytes + 3u) & ~(size_t) 3u;
    if (cards != 0 && ST_CARDS >= cards && rows != 0 && n >= at + cards * rec && (stCard.entries = malloc(cards * bytes)) != nullptr) {
        for (uint32_t c = 0; cards > c; c++) {
            const uint8_t* r = buf + at + c * rec;
            memcpy(stCard.entries + c * bytes, r + ST_CARD_REC, bytes);
            char name[25];
            memcpy(name, r, 24);
            name[24] = '\0';
            for (uint32_t s = 0; r[25] == ST_CARD_PARTS && r[26] == rows && sprt->count > s; s++) {
                const Sprite* sp = &sprt->sprites[s];
                if (sp->name != nullptr && sp->textureCount > r[24] && strcmp(sp->name, name) == 0) stCard.tpag[c] = sp->tpagIndices[r[24]] + 1;
            }
        }
        stCard.font.font = -1;
        stCard.font.rows = (uint8_t) rows;
        stCard.font.cell = 16;
        stCard.font.count = (uint8_t) (cards * ST_CARD_PARTS);
        stCard.font.entries = stCard.entries;
        memcpy(stCard.font.name, stCardName, sizeof(stCardName));
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
#endif // !ST_DR_UNIT (the menu data)

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
#ifdef ST_DR_UNIT
    const bool every = false;
#else
    const bool every = f == &stCard.font; // the card's atlas writes its pixels without ink too (opaque black)
#endif
    for (uint32_t e = 0; f->count > e; e++) {
        const uint8_t* g = stEntry(f, e);
        uint32_t cx = (e % ST_ATLAS_COLS) * (uint32_t) pitchX, cy = (e / ST_ATLAS_COLS) * (uint32_t) pitchY;
        // statlas (aotprof): a row's pixels to write as bits (x below the glyph's width; only those with ink unless
        // every), taken lowest first: the same texels as a walk of every x, without the walk (Photoshop Flowey's first
        // text, 12/0, made an atlas in its Draw: about 295k instructions). Columns go 32 at a time; a row's mask has 16
        // bits, so from x 32 on there are only the card's black pixels (main's mask >> x was 0 there on the ARM).
        for (uint32_t y = 0; f->rows > y; y++) {
            uint32_t mask = (uint32_t) g[6 + 2 * y] | ((uint32_t) g[7 + 2 * y] << 8);
            for (uint32_t x0 = 0; g[2] > x0; x0 += 32u, mask = 0u) {
                uint32_t left = g[2] - x0, span = left >= 32u ? 0xFFFFFFFFu : (1u << left) - 1u;
                for (uint32_t todo = every ? span : mask & span; todo != 0u; todo &= todo - 1u) {
                    uint32_t bit = (uint32_t) __builtin_ctz(todo);
                    uint32_t tx = cx + x0 + bit, ty = cy + y, mx = tx & 7u, my = ty & 7u;
                    uint32_t morton = (mx & 1u) | ((my & 1u) << 1) | ((mx & 2u) << 1) | ((my & 2u) << 2) | ((mx & 4u) << 2) | ((my & 4u) << 3);
                    texels[(ty >> 3) * (tw / 8u) * 64u + (tx >> 3) * 64u + morton] = (mask >> bit & 1u) != 0u ? 0xFFFFFFFFu : 0x000000FFu; // opaque black: A is the low byte
                }
            }
        }
    }
#line 839 // statlas: main's numbering
    C3D_TexFlush(&f->tex);
    C3D_TexSetFilter(&f->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&f->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    f->made = 1;
    logInfo(stLogAtlas, f->name, (int) w, (int) h, (unsigned) tw, (unsigned) th);
    return true;
}

// Glyph e of atlas f at (x, y) in drawing units, two per texel, as the renderer draws a sprite part there (its pixel
// grid included).
#ifdef ST_DR_UNIT
static ST_LATE float stSurfaceOx(Renderer* renderer);
static ST_LATE float stSurfaceOy(Renderer* renderer);
static ST_LATE int32_t stGridPixel(float s);
// DELTARUNE: the top left on the even surface pixel at or before the first one whose centre it covers, as the grid
// snaps a quad (ctr_renderer.c gridDelta: (ceil(s - 0.5) & ~1) in each axis), then crispart's draw (its frame skip and
// application surface checks are Smalltext_drawTexture's).
static ST_LATE void stBlit(Renderer* renderer, StFont* f, uint32_t e, uint32_t w, float x, float y, uint32_t color, float alpha) {
    CaFrame frame;
    frame.x = (uint16_t) ((e % ST_ATLAS_COLS) * (uint32_t) (f->cell + 1));
    frame.y = (uint16_t) ((e / ST_ATLAS_COLS) * (uint32_t) (f->rows + 1));
    frame.w = (uint16_t) w;
    frame.h = f->rows;
    float ox = stSurfaceOx(renderer), oy = stSurfaceOy(renderer);
    x = (float) (stGridPixel(x + ox) & ~1) - ox;
    y = (float) (stGridPixel(y + oy) & ~1) - oy;
    Crispart_drawTexture(renderer, &f->tex, &frame, x, y, color, alpha);
}
#else
static ST_LATE void stBlit(Renderer* renderer, StFont* f, uint32_t e, uint32_t w, float x, float y, uint32_t color, float alpha) {
    float sx = (float) ((e % ST_ATLAS_COLS) * (uint32_t) (f->cell + 1)), sy = (float) ((e / ST_ATLAS_COLS) * (uint32_t) (f->rows + 1));
    Smalltext_drawTexture(renderer, &f->tex, sx, sy, (float) w, (float) f->rows, x, y, color, alpha);
}
#endif

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

#ifndef ST_DR_UNIT
// floor(v / 2) for a 640-space place: the screen pixel it is in.
static ST_LATE int32_t stHalfFloor(float v) {
    int32_t i = (int32_t) v;
    if ((float) i > v) i--;
    return i >> 1;
}
static int32_t stHalfFloorView(float v); // soulshake: the Crisp soul's and cards' places (the file's end)
#endif

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

#ifndef ST_DR_UNIT
    const uint8_t* rowRule = Asrtext_match(text, (size_t) textLen, fontIndex);
#endif
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
#ifdef ST_CULL_INLINE
    // cullcrisp (credfix, 2026-10-27): a string wholly above or below the view draws nothing, so its glyphs are
    // neither carried nor blitted, as drawTextImpl skips such strings (the credits dodger draws ~190 names a frame,
    // most of them off screen). Its glyphs still take their shake slots, so later text in the frame shakes as before.
    bool out = false;
    {
        const uint8_t* rp = (const uint8_t*) renderer;
        const C3D_Mtx* m = (const C3D_Mtx*) (rp + Smalltext_layout[L_PROJECTION]);
        const StRect* vp = (const StRect*) (rp + Smalltext_layout[L_VIEWPORT]);
        float span = (float) (lineCount - 1) * lineStride, top = y + cursorY + oy;
        float bottom = top + (span > 0.0f ? span : 0.0f) + 2.0f * (float) f->rows + 32.0f;
        top += (0.0f > span ? span : 0.0f) - 32.0f;
        out = 0.0f > m->r[1].y && ((float) vp->y > bottom || top > (float) (vp->y + vp->h));
    }
#else
    const bool out = false;
#endif
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
#ifndef ST_DR_UNIT
        const uint8_t* localRows = rowRule != nullptr && lineIdx >= rowRule[130] ? rowRule : nullptr;
#endif
        for (int32_t i = 0; lineLen > i; i++) {
            uint8_t ch = (uint8_t) text[lineStart + i];
            FontGlyph* glyph = TextUtils_findGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (ch == ' ' || glyph->sourceWidth == 0 || glyph->sourceHeight == 0) space = true;
            else {
                uint32_t e = (uint32_t) f->lut[ch] - 1u;
                if (ch == 'W' && fixW) e = (uint32_t) f->fixW - 1u;
                else if (ch == '*' && fixStar) e = (uint32_t) f->fixStar - 1u;
#ifndef ST_DR_UNIT
                bool localGlyph = localRows != nullptr && localRows[ch] != 0 && localRows[ch] <= f->count;
                if (localGlyph) e = (uint32_t) localRows[ch] - 1u;
#endif
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
                    if (!shakeX && !out) {
                        // the PC phase: 2 * floor(surface x / 2) against the PC ink's first column, in 640 px
                        int32_t p = stGridPixel(pen + ox), off = -(p & 1) - (int32_t) (int8_t) g[5];
                        if (-2 >= off) gx += 2.0f * (float) (-off / 2);
                        else if (off >= 2) gx -= 2.0f * (float) (off / 2);
                        int32_t parity = stGridPixel(gy + oy) & 1;
                        int32_t rowShift = f->rowShift[parity];
#ifndef ST_DR_UNIT
                        if (localGlyph) rowShift = (int8_t) localRows[128 + parity];
#endif
                        gy += 2.0f * (float) rowShift; // the font's rows against PC's
                        if (carry) {
                            int32_t nx0 = stGridPixel(gx + ox) >> 1; // the screen pixel the grid puts it on
                            gx += 2.0f * (float) (stCarry(renderer, f, g, pen, gy, nx0, space) - nx0);
                        }
                    }
                    space = false;
#ifdef CTRPORT_BUBBLE_CHECK
                    bckGlyph(renderer, f, g, pen, y + cursorY, gx, gy, ox, oy);
#endif
                    if (!out) stBlit(renderer, f, e, g[2], gx, gy, color, alpha);
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

#line 1057
#ifndef ST_DR_UNIT
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
        stBlit(renderer, &st.soul, e, 8u, (float) (2 * stHalfFloorView(x - originX + ox)) - ox, (float) (2 * stHalfFloorView(y - originY + oy)) - oy, mixed,
               alpha);
        return;
    }
    // A Crisp card (Text Crisp): its frame's ST_CARD_PARTS entries side by side, placed as the soul.
    for (uint32_t c = 0; ST_CARDS > c; c++) {
        if (stCard.tpag[c] == 0 || tpagIndex != stCard.tpag[c] - 1) continue;
        if ((st.keys & 2u) == 0 || xscale != 1.0f || yscale != 1.0f || angleDeg != 0.0f || !stOneToOne(renderer, -1) || !stAtlas(&stCard.font)) break;
        float ox = stSurfaceOx(renderer), oy = stSurfaceOy(renderer);
        float px = (float) (2 * stHalfFloorView(x - originX + ox)) - ox, py = (float) (2 * stHalfFloorView(y - originY + oy)) - oy;
        for (uint32_t p = 0; ST_CARD_PARTS > p; p++) {
            uint32_t e = c * ST_CARD_PARTS + p, w = stEntry(&stCard.font, e)[2];
            if (w != 0) stBlit(renderer, &stCard.font, e, w, px + (float) (32u * p), py, color, alpha);
        }
        return;
    }
    st.prev->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}
#else
// DELTARUNE: a chapter's end (ctr_chapters.c destroys the renderer, then the next chapter makes its own): the atlases
// once the GPU is done with them (as crispart's own), the pack and the line records, then the table's own destroy.
static ST_LATE void stDestroy(Renderer* renderer) {
    bool made = false;
    for (uint32_t i = 0; ST_FONTS > i; i++) made = made || st.fonts[i].made > 0;
    if (made) C3D_FrameSync();
    for (uint32_t i = 0; ST_FONTS > i; i++) {
        if (st.fonts[i].made > 0) C3D_TexDelete(&st.fonts[i].tex);
    }
    memset(st.fonts, 0, sizeof(st.fonts));
    memset(st.lines, 0, sizeof(st.lines));
    st.fontCount = 0;
    free(st.file);
    st.file = nullptr;
    st.prev->destroy(renderer);
}
#endif

static ST_LATE void stDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    if (stDraw(renderer, text, x, y, xscale, yscale, angleDeg, renderer->drawColor, renderer->drawAlpha, lineSeparation)) return;
    st.text->drawText(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation);
}

static ST_LATE void stDrawTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1,
                                    int32_t c2, int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    if (c1 == c2 && c2 == c3 && c3 == c4 && stDraw(renderer, text, x, y, xscale, yscale, angleDeg, (uint32_t) c1, alpha, lineSeparation)) return;
    st.textColor->drawTextColor(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
}

#ifndef ST_CULL_INLINE
// cullcrisp (the file's end): the outer table's text entries, which skip Crisp text wholly out of view.
static void stCullText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation);
static void stCullTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2,
                            int32_t c3, int32_t c4, float alpha, float lineSeparation);
#endif

// At each room change: a renderer not yet on the outer table gets it when a Crisp key is on and the pack has glyphs.
ST_LATE void __wrap_CtrRenderer_loadFontVerdicts(Renderer* renderer) {
    CTR_FONTVERDICTS_AFTER_SMALLTEXT(renderer);
    if (renderer == nullptr || renderer->vtable == &st.outer || renderer->dataWin == nullptr) return;
    // The menu's choices and keys, once per run (settings.ini, then --ini), and the menu data once per data.win: with
    // no font key on and the soul Smooth, nothing more happens (a later change in the menu then takes the next room).
    stEnsure();
#ifndef ST_DR_UNIT
    if (st.menuDw != renderer->dataWin) stLoadMenu(renderer->dataWin);
#endif
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
#ifdef ST_CULL_INLINE
    st.outer.drawText = stDrawText; // cullcrisp is inside stDraw here (the layout event at the top)
    st.outer.drawTextColor = stDrawTextColor;
#else
    st.outer.drawText = stCullText;
    st.outer.drawTextColor = stCullTextColor;
#endif
#ifdef ST_DR_UNIT
    st.outer.destroy = stDestroy;
#else
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
    if (st.soulSlots != 0 || stCard.tpag[0] != 0) st.outer.drawSprite = stDrawSprite;
#endif
    renderer->vtable = &st.outer;
}
#ifndef ST_DR_UNIT
// ===[ soulshake: the Crisp soul's place tolerates the view offset's float error ]===
// The soul and the cards add the view offset (stSurfaceOx/Oy), a float from the projection: Photoshop Flowey's intro
// scrolls the view 478 -> 0 with the SOUL riding it, and oy came out as -478.00003 or -477.00006 on some frames, so a
// SOUL on a whole place (900 - 478 = 422) floored to 421 and the Crisp SOUL hopped up 1 px on those frames (playtest,
// 2026-10-26). A 1/64 place allowance takes whole places back; a real fraction only ever moves by less than 1/64.
__attribute__((noinline, section(".text_soulshake_crisp"))) static int32_t stHalfFloorView(float v) {
    return stHalfFloor(v + 1.0f / 64.0f);
}
#endif

#ifndef ST_DR_UNIT // bubbleclip is UNDERTALE only: its bubble table never compiles into DELTARUNE New's unit
// ===[ bubbleclip: a carried line stays inside its bubble ]===
// Carry moves every later glyph of a line right (fnt_plain's 4 px glyphs at a 4.5 px advance gain about half a pixel
// a letter), so a long line can end several pixels right of PC's: Alphys's date bubble "chance of a successful" ran
// into the bubble's edge and tail (playtest, 2026-10-26). Option B: such a line gives the pixels back at its word
// gaps, and every letter keeps its gap. For a writer's glyph (the current instance has writingxend and spacing, and
// vtext is off), the writer's lines are worked out once (per writer, string, place and surface x; a few writers kept)
// from originalstring as obj_base_writer lays it out: each letter at myx, which moves on by spacing plus the font's
// kerning for that letter (the writer's own table for fnt_papyrus and fnt_comicsans: an "I" gives back 6 px), and a
// new line at '&' or when myx has passed writingxend. Each line's glyphs are placed with carry as they will be drawn
// (at myx plus the offset the writer draws the first glyph at: shaking text such as Vegetoid's is drawn 10 px right
// of myx), with its PC ink, and the bubble the writer starts in (an instance whose sprite the build's bubble table
// has, romfs:/fonts/<data.win folder>.dtsb from tools/aot/stmenu.py: each row's inner right edge; the table also
// holds the writer's kerning). A glyph's line and place are told by the writer's myy and myx as it draws the glyph,
// so shaking text moves with its line. A line whose Crisp ink comes closer to the bubble's inner edge, on the line's rows,
// than PC's ink does (up to BC_MARGIN px of margin: less when PC leaves less; where PC's ink is past the edge, no further past than PC's) moves left: its word gaps give back a
// pixel at a time, round robin from the line's last gap, each keeping BC_MINGAP px; only if that isn't enough, letter
// gaps of 2 px or more give back down to 1 px, and last, to keep the ink off the edge, down to touching (never
// overlapping; not for a writer that draws away from myx, as its letters wobble apart and would overlap). Every other line, and text with no bubble, is drawn as before (tools/analysis/smalltext_carry.py,
// bubble_plan, is the reference model; test builds with CTRPORT_BUBBLE_CHECK check every Crisp line drawn, below).
#include "vm.h"
// flatten: the body takes its own copies of the touch tests, the selfVars lookup and the rest, so the code in
// z_smalltext.o's shared line (and its clones) stays as it was; no __builtin_popcount (libgcc's helper would land in
// the early text).
#define BC_LATE __attribute__((noinline, flatten, section(".text_bubbleclip_late")))
#define BC_CONST __attribute__((aligned(4), section(".rodata_bubbleclip_late")))
// Zero-initialised, outside .bss.* so z_smalltext.o's line in late_reserve.ld leaves it to its own line. GCC marks only
// .bss.* names as nobits, so the flags are spelled out ('@' starts a comment for the assembler, ending GCC's own).
#define BC_BSS __attribute__((section(".bss_bubbleclip_late,\"aw\",%nobits @")))
#define BC_LINES 8
#define BC_COLS 32
#define BC_GIVES 8   // the places a line gives pixels back at, at most (a line that needs more is left as it is)
#define BC_SLOTS 3
#define BC_MARGIN 2  // native px of margin from the bubble's edge, at most (PC's own when it leaves less)
#define BC_MINGAP 3  // native px a word gap keeps
#define BC_FILE_MAX 4096u
#define BC_MAGIC 0x42535444u // "DTSB"
#define BC_KERN 112u         // a kerning entry: the font's name (16 chars, NUL padded), then int8 px for chars 32..127
static const char bcNames[] BC_CONST = "writingxend\0writingx\0spacing\0myx\0myy\0writingy\0vspacing\0vtext\0originalstring";
// The writer's \ codes that draw nothing: colours and C, then M, E, F and S, which take one more character.
static const char bcCodes[] BC_CONST = "RGWYXBOLPpCMEFS";
static const char bcFmt[] BC_CONST = "romfs:/fonts/%s.dtsb";
enum { BC_XEND, BC_X0, BC_SP, BC_MYX, BC_MYY, BC_Y0, BC_VSP, BC_VTEXT, BC_STR, BC_VARS };
typedef struct {
    uint32_t id, hash; // the writer (instance id), its string's hash, writingx, writingy and the surface x it is for
    float x0, y0, ox;
    uint8_t ok;        // bit 0: bubble plan; bit 1: exact-string PC-y contact correction
    // line i's places: a glyph drawn with myx - writingx at least at[i][k] moves px[i][k] native px left (summed)
    uint8_t gives[BC_LINES];
    int16_t at[BC_LINES][BC_GIVES];
    int8_t px[BC_LINES][BC_GIVES];
} BcPlan;
static struct {
    const VMContext* vm; // the context the ids are from
    int32_t id[BC_VARS]; // varNameMap ids, -1 for none
    const uint8_t* edges; // the bubble table (nullptr: none), read once, and its kerning entries
    uint32_t edgeCount, kernCount;
    const uint8_t* kern;
    bool read;
    uint32_t next;
    BcPlan plan[BC_SLOTS];
    int32_t frame;      // the last writer's frame and instance id, and its plan (nullptr: none, this frame)
    uint32_t inst;
    const BcPlan* last;
    float v[BC_VARS];   // its vars (myx and myy read per glyph)
} bc BC_BSS;

// The bubble table's rows of sprite `sprite` (*rows of them: each the inner right edge's column, 0 for none), or nullptr.
// The table is read at the first lookup (its own read: stReadRomfs's callers stay as they were).
static BC_LATE const uint8_t* bcEdges(Runner* runner, int32_t sprite, uint32_t* rows) {
    if (!bc.read) {
        bc.read = true;
        char folder[64], file[96];
        size_t n = 0;
        uint8_t* buf = nullptr;
        if (CtrPack_romfsName(folder, sizeof(folder)) && R_SUCCEEDED(CtrRomfs_init(stRomfs))) {
            snprintf(file, sizeof(file), bcFmt, folder);
            FILE* fp = fopen(file, stReadBin);
            if (fp != nullptr) {
                buf = malloc(BC_FILE_MAX);
                if (buf != nullptr) n = fread(buf, 1, BC_FILE_MAX, fp);
                fclose(fp);
            }
        }
        if (buf != nullptr && n >= ST_HEADER) {
            CtrPackKey key = CtrPack_keyFor(runner->dataWin);
            uint32_t head[6] = {BC_MAGIC, 2, key.dataWinSize, key.gameId, (uint32_t) key.timestamp, (uint32_t) (key.timestamp >> 32)}, count, fonts = 0;
            memcpy(&count, buf + 24, 4);
            size_t at = ST_HEADER;
            uint32_t i = 0;
            for (; count > i && n >= at + 4; i++) at += 4 + ((size_t) buf[at + 2] | (size_t) buf[at + 3] << 8);
            if (n >= at + 4) memcpy(&fonts, buf + at, 4);
            if (memcmp(buf, head, 24) == 0 && i == count && n >= at + 4 && BC_FILE_MAX / BC_KERN > fonts && n >= at + 4 + fonts * BC_KERN) {
                bc.edges = buf;
                bc.edgeCount = count;
                bc.kern = buf + at + 4;
                bc.kernCount = fonts;
                buf = nullptr;
            }
        }
        free(buf);
    }
    const uint8_t* p = bc.edges + ST_HEADER;
    for (uint32_t i = 0; bc.edges != nullptr && bc.edgeCount > i; i++) {
        uint32_t s = (uint32_t) p[0] | (uint32_t) p[1] << 8, h = (uint32_t) p[2] | (uint32_t) p[3] << 8;
        if (s == (uint32_t) sprite) {
            *rows = h;
            return p + 4;
        }
        p += 4 + h;
    }
    return nullptr;
}

// Do glyphs a (native x ax) and b (bx, to its right, on the same row) touch? (one copy of stTouchNative for bcMake)
static BC_LATE bool bcTouch(const StFont* f, const uint8_t* a, int32_t ax, const uint8_t* b, int32_t bx) {
    return stTouchNative(f, a, ax, b, bx, 0);
}

// The writer's kerning for font f (int8 px for chars 32..127), or nullptr (none: such a font moves by spacing alone).
static BC_LATE const int8_t* bcKern(const StFont* f) {
    for (uint32_t i = 0; bc.kernCount > i; i++) {
        const uint8_t* k = bc.kern + i * BC_KERN;
        if (strncmp((const char*) k, f->name, 16) == 0) return (const int8_t*) (k + 16);
    }
    return nullptr;
}

// The plan of a writer (vars v, its string text, surface x ox; its first glyph drawn d px right of myx) into p; see
// the top.
static BC_LATE void bcMake(Runner* runner, const StFont* f, BcPlan* p, const float* v, const char* text, float d) {
    memset(p->gives, 0, sizeof(p->gives));
    p->ok = false;
    float x0 = v[BC_X0], y0 = v[BC_Y0], xend = v[BC_XEND], myx = x0;
    int32_t s = (int32_t) v[BC_SP], vsp = (int32_t) v[BC_VSP];
    // the lines as the writer lays them out: each glyph's char, myx - writingx and whether a space comes before it
    struct {
        uint8_t c, word;
        int16_t at;
    } gl[BC_LINES][BC_COLS];
    uint8_t count[BC_LINES];
    memset(count, 0, sizeof(count));
    const int8_t* kern = bcKern(f);
    int32_t li = 0;
    bool space = false;
    for (const char* c = text; *c != '\0'; c++) {
        if (*c == '^' && c[1] != '0' && c[1] != '\0') c++; // ^n: a pause
        else if (*c == '\\') {
            const char* k = c[1] != '\0' ? strchr(bcCodes, c[1]) : nullptr;
            if (k == nullptr) return;
            c += k >= bcCodes + 11 && c[2] != '\0' ? 2 : 1;
        } else if (*c == '&') {
            li++;
            myx = x0;
            space = false;
        } else if (*c == '/' || *c == '%') break;
        else if (*c == '^') return;
        else {
            if (myx > xend) {
                li++;
                myx = x0;
                space = false;
            }
            if (li >= BC_LINES) return;
            uint8_t ch = (uint8_t) *c;
            if (ch == ' ') space = true;
            else {
                if (count[li] >= BC_COLS) return;
                gl[li][count[li]].c = ch;
                gl[li][count[li]].word = space;
                gl[li][count[li]++].at = (int16_t) (myx - x0);
                space = false;
            }
            myx += (float) s + (kern != nullptr && ch >= 32u && 128u > ch ? (float) kern[ch - 32u] : 0.0f);
        }
    }
    // the bubble the writer starts in (of those it is in, the one whose top left corner is nearest)
    DataWin* dw = runner->dataWin;
    const uint8_t* edge = nullptr;
    uint32_t rows = 0;
    float left = 0.0f, top = 0.0f;
    for (ptrdiff_t i = 0; arrlen(runner->instances) > i; i++) {
        const Instance* b = runner->instances[i];
        uint32_t h;
        const uint8_t* e;
        if (b->destroyed || !b->active || b->imageXscale != 1.0f || b->imageYscale != 1.0f || 0 > b->spriteIndex ||
            (e = bcEdges(runner, b->spriteIndex, &h)) == nullptr)
            continue;
        const Sprite* sp = &dw->sprt.sprites[b->spriteIndex];
        float l = b->x - (float) sp->originX, t = b->y - (float) sp->originY;
        if (l > x0 || t > y0 || x0 >= l + (float) sp->width || y0 >= t + (float) h || (edge != nullptr && left + top > l + t)) continue;
        edge = e;
        rows = h;
        left = l;
        top = t;
    }
    if (edge == nullptr) return;
    p->ok = 1u | (Asrtext_motion(text, strlen(text), f->font) ? 2u : 0u);
    bool fixW = (st.keys >> ST_KEY_FIX_W & 1u) != 0 && f->fixW != 0, fixStar = (st.keys >> ST_KEY_FIX_STAR & 1u) != 0 && f->fixStar != 0;
    for (li = 0; BC_LINES > li; li++) {
        // The selected random-shake line must not cache a weaker plan merely
        // because its first typed glyph happened to draw one PC pixel left.
        float planD = (p->ok & 2u) != 0 && li == 0 ? 0.0f : d;
#line 1395
        // the line's glyphs placed as stDraw and stCarry place them: native ink left and right
        int32_t lo[BC_COLS], hi[BC_COLS], at[BC_COLS], n = 0, carry = 0, prevX = 0, pcRight = -100000;
        bool word[BC_COLS];
        const uint8_t* prev = nullptr;
        float prevPen = 0.0f;
        uint32_t inked = 0; // PC rows with ink
        for (int32_t j = 0; count[li] > j; j++) {
            uint32_t c = gl[li][j].c, e = c < 128u ? f->lut[c] : 0u;
            if (e-- == 0) continue;
            if (c == 'W' && fixW) e = (uint32_t) f->fixW - 1u;
            else if (c == '*' && fixStar) e = (uint32_t) f->fixStar - 1u;
            const uint8_t* g = stEntry(f, e);
            if (g[2] == 0) continue;
            float pen = x0 + planD + (float) gl[li][j].at, gx = pen + 2.0f * (float) (int8_t) g[3];
            int32_t q = stGridPixel(pen + p->ox), off = -(q & 1) - (int32_t) (int8_t) g[5];
            if (-2 >= off) gx += 2.0f * (float) (-off / 2);
            else if (off >= 2) gx -= 2.0f * (float) (off / 2);
            int32_t nx0 = stGridPixel(gx + p->ox) >> 1;
            if (prev != nullptr && (prevPen >= pen || pen - prevPen > 4.0f * (float) f->adv)) {
                prev = nullptr;
                carry = 0;
            }
            int32_t nx = nx0 + carry;
            if (prev != nullptr && (float) f->adv + 1.0f >= pen - prevPen && !stTouchPc(f, prev, (int32_t) prevPen, g, (int32_t) pen, 0)) {
                while (carry > 0 && !bcTouch(f, prev, prevX, g, nx - 1)) {
                    carry--;
                    nx--;
                }
                for (int32_t k = 0; 2 > k && bcTouch(f, prev, prevX, g, nx); k++) {
                    carry++;
                    nx++;
                }
            }
            int32_t a = 99, z = -99;
            for (int32_t r = 0; f->rows > r; r++) {
                uint32_t m = (uint32_t) g[6 + 2 * r] | ((uint32_t) g[7 + 2 * r] << 8);
                if (m == 0) continue;
                if (a > __builtin_ctz(m)) a = __builtin_ctz(m);
                if (31 - __builtin_clz(m) > z) z = 31 - __builtin_clz(m);
            }
            const int8_t* ext = (const int8_t*) (g + 6 + 2 * f->rows);
            for (int32_t r = 0; f->pcRows > r && 32 > r; r++) {
                if (ext[2 * r] > ext[2 * r + 1]) continue;
                inked |= 1u << r;
                if ((int32_t) pen + ext[2 * r + 1] > pcRight) pcRight = (int32_t) pen + ext[2 * r + 1];
            }
            word[n] = gl[li][j].word != 0;
            at[n] = gl[li][j].at;
            lo[n] = nx + a;
            hi[n++] = nx + z;
            prev = g;
            prevX = nx;
            prevPen = pen;
        }
        if (n == 0) continue;
        // the bubble's inner edge on the line's rows (640 px, room x); a row outside the bubble: no limit
        int32_t in = 1000, right = -100000;
        for (int32_t r = 0; 32 > r; r++) {
            int32_t row = (int32_t) (y0 + (float) (li * vsp) - top) + r;
            if ((inked >> r & 1u) == 0) continue;
            if (0 > row || row >= (int32_t) rows || edge[row] == 0) in = -1;
            else if (in > edge[row]) in = edge[row];
        }
        if (0 > in) continue;
        for (int32_t k = 0; n > k; k++)
            if (hi[k] > right) right = hi[k];
        float x = left + (float) in;
        int32_t pc = (int32_t) x - 1 - pcRight, req = pc / 2; // PC past the edge: no further past than PC (native, toward 0)
        if (req > BC_MARGIN) req = BC_MARGIN;
        int32_t gap = (stGridPixel(x + p->ox) >> 1) - 1 - right, need = req - gap;
        // the passes: word gaps to BC_MINGAP, letter gaps to 1, then (only to keep off the edge; steady text) to 0
        int8_t give[BC_COLS];
        memset(give, 0, sizeof(give));
        for (int32_t pass = 0; (planD == 0.0f ? 3 : 2) > pass; pass++) {
            if (pass == 2 && req > 1) need--;
            int32_t floor = pass == 0 ? BC_MINGAP : 2 - pass;
            for (bool moved = true; need > 0 && moved;) {
                moved = false;
                for (int32_t k = n - 1; k > 0 && need > 0; k--) {
                    if (word[k] == (pass == 0) && lo[k] - hi[k - 1] - 1 - give[k] > floor) {
                        give[k]++;
                        need--;
                        moved = true;
                    }
                }
            }
        }
        int32_t acc = 0, places = 0;
        for (int32_t k = 0; n > k; k++) {
            if (give[k] == 0) continue;
            if (BC_GIVES > places) {
                p->at[li][places] = (int16_t) at[k];
                p->px[li][places] = give[k];
            }
            places++;
            acc += give[k];
        }
        if (places > BC_GIVES) places = acc = 0; // more places than a plan keeps: the line as it was
        p->gives[li] = (uint8_t) places;
#ifdef CTRPORT_BUBBLE_CHECK
        logInfo("bubblecheck: plan room %d frame %d font %s line %d x %d y %d edge %d pc %d req %d gap %d after %d d %d\n", (int) runner->currentRoomIndex,
                (int) runner->frameCount, f->name, (int) li, (int) x0, (int) (y0 + (float) (li * vsp)), (int) x, (int) pc, (int) req, (int) gap,
                (int) (gap + acc), (int) d);
#endif
    }
}

// The slot of writer var i (bcNames) of instance in, or nullptr.
static BC_LATE RValue* bcSlot(Instance* in, int32_t i) {
    return 0 > bc.id[i] ? nullptr : IntRValueHashMap_findSlot(&in->selfVars, bc.id[i]);
}

// Writer var i of instance in into v[i]; false (v[i] 0) when it isn't a number.
static BC_LATE bool bcNum(Instance* in, int32_t i, float* v) {
    RValue* r = bcSlot(in, i);
    v[i] = 0.0f;
    if (r != nullptr && r->type == RVALUE_REAL) v[i] = (float) r->real;
    else if (r != nullptr && r->type == RVALUE_INT32) v[i] = (float) r->int32;
    else return false;
    return true;
}

// The native px (0 or less) a writer's glyph at pen `pen` (font f) moves; 0 for any other glyph. The writer draws a
// glyph while its myx and myy are the glyph's place (obj_base_writer's Draw moves them on after each letter).
static BC_LATE int32_t stBubbleClip(Runner* runner, const StFont* f, float pen) {
    if (runner == nullptr || runner->vmContext == nullptr || runner->vmContext->currentInstance == nullptr) return 0;
    VMContext* vm = runner->vmContext;
    Instance* in = vm->currentInstance;
    if (bc.vm != vm) {
        const char* s = bcNames;
        for (int32_t i = 0; BC_VARS > i; i++, s += strlen(s) + 1) {
            ptrdiff_t k = shgeti(vm->varNameMap, s);
            bc.id[i] = 0 > k ? -1 : vm->varNameMap[k].value;
        }
        bc.vm = vm;
        memset(bc.plan, 0, sizeof(bc.plan));
        bc.frame = runner->frameCount - 1;
    }
    float* v = bc.v;
    // once a frame per writer: its vars and its plan (made once per writer, string and place)
    if (bc.frame != runner->frameCount || bc.inst != in->instanceId) {
        bc.frame = runner->frameCount;
        bc.inst = in->instanceId;
        bc.last = nullptr;
        for (int32_t i = 0; BC_STR > i; i++)
            if (!bcNum(in, i, v) && BC_VTEXT > i) return 0; // not a writer
        int32_t s = (int32_t) v[BC_SP];
        RValue* str = bcSlot(in, BC_STR);
        if (v[BC_VTEXT] != 0.0f || 1 > s || v[BC_SP] != (float) s || 0.0f >= v[BC_VSP] || str == nullptr || str->type != RVALUE_STRING ||
            str->string == nullptr)
            return 0;
        uint32_t hash = 2166136261u;
        for (const char* c = str->string; *c != '\0'; c++) hash = (hash ^ (uint8_t) *c) * 16777619u;
        float ox = stSurfaceOx(runner->renderer);
        BcPlan* p = nullptr;
        for (int32_t i = 0; BC_SLOTS > i; i++) {
            BcPlan* q = &bc.plan[i];
            if (q->id == in->instanceId && q->hash == hash && q->x0 == v[BC_X0] && q->y0 == v[BC_Y0] && q->ox == ox) p = q;
        }
        if (p == nullptr) {
            p = &bc.plan[bc.next++ % BC_SLOTS];
            p->id = in->instanceId;
            p->hash = hash;
            p->x0 = v[BC_X0];
            p->y0 = v[BC_Y0];
            p->ox = ox;
            bcMake(runner, f, p, v, str->string, pen - v[BC_MYX]);
        }
        if (p->ok) bc.last = p;
    }
    if (bc.last == nullptr) return 0;
    if (!bcNum(in, BC_MYX, v) || !bcNum(in, BC_MYY, v)) return 0;
    float fl = (v[BC_MYY] - v[BC_Y0]) / v[BC_VSP], rel = v[BC_MYX] - v[BC_X0];
    int32_t li = (int32_t) fl, shift = 0;
    if (fl != (float) li || 0 > li || li >= BC_LINES) return 0;
    for (uint32_t k = 0; bc.last->gives[li] > k; k++)
        if (rel >= (float) bc.last->at[li][k]) shift -= bc.last->px[li][k];
    return shift;
}
#ifdef CTRPORT_BUBBLE_CHECK
// ===[ bubblecheck: test builds only (build.sh bubblecheck; release builds have none of it) ]===
// Every Crisp glyph stDraw draws (any font the pack has, so a font added to the pack is checked with no change here;
// shaking text included) is grouped into lines: the glyphs of a frame in one font at about one y, each at most 4
// advances right of the last. Each line is checked three ways, against PC's own ink (the pack's PC extents at the
// glyph's pen), and logged once per room visit (font, place, text):
//   - its holder's right edge: a bubble of the bubble table that its first glyph is in (the edge on the line's PC rows;
//     bubbleclip's rule), else the battle box when the line is inside it (obj_rborder's x is the black interior's last
//     column). The line's Crisp ink must keep min(PC's margin / 2, BC_MARGIN) native px from the edge, PC's margin
//     being the 640 px between PC's ink and the edge: "fail" when closer, "pc" when PC's ink crosses the edge as well.
//     A line with no holder is "free" when its Crisp ink ends right of PC's.
//   - the screen: Crisp ink outside the application surface (320 x 240 native) where PC's is inside (640 x 480):
//     "screen" ("screenpc" when PC's ink is outside too).
//   - the lines below it in the same frame (any font): where a glyph's ink is over or beside another line's glyph, the
//     native rows between them must be at least 1 where PC leaves 2 or more (touching counts), and 0 (no overlap)
//     where PC leaves 0 or 1: "overlap" when closer ("overlappc" when PC's ink overlaps as well).
// Each room visit's counts are logged every 300 frames and at its end (the last is its total). Writers' plans are
// logged as "plan". tools/analysis/bubblecheck_report.py makes the table from the log.
#define BCK_LINES 24
#define BCK_TEXT 64
#define BCK_GLYPHS 48
#define BCK_SEEN 4096
typedef struct {
    const uint8_t* g;
    int16_t nx, ny, pen, pcy; // native x and y of its cell (surface px), its pen and PC cell top (640)
} BckGlyph;
typedef struct {
    const StFont* f;
    float y, pen0, pen;                  // the PC cell's top (640), first and last pen
    int32_t obj, right, pcRight, ox, oy; // the drawing instance's object, native right ink (surface px), PC's (640)
    uint32_t rows;                       // the PC rows with ink
    int32_t n, glyphs;
    // the bubbles its first glyph was in as it was drawn (a bubble's instance can be gone by the time the line is
    // checked, at the next frame's first glyph: a monster's turn ends in that frame's step)
    int32_t holders;
    struct {
        const uint8_t* edge;
        const char* name;
        float left, top;
        uint32_t rows;
    } holder[4];
    char text[BCK_TEXT];
    BckGlyph glyph[BCK_GLYPHS];
} BckLine;
static struct {
    BckLine line[BCK_LINES];
    uint32_t count;
    int32_t frame, room, summary; // summary: the frame of the visit's last counts line
    uint32_t visit, seen[BCK_SEEN];
    uint32_t lines, fail, pc, free, bubble, box, screen, overlap;
    int32_t border[4]; // obj_lborder, obj_rborder, obj_uborder, obj_dborder (-1 none)
    const DataWin* dw;
} bck = {.frame = -1, .room = -1};
static const char bckBorders[] = "obj_lborder\0obj_rborder\0obj_uborder\0obj_dborder";

static const char* bckObjName(const DataWin* dw, int32_t obj) {
    return 0 > obj || obj >= (int32_t) dw->objt.count || dw->objt.objects[obj].name == nullptr ? "-" : dw->objt.objects[obj].name;
}

static const Instance* bckFirst(Runner* runner, int32_t obj) {
    for (ptrdiff_t i = 0; 0 <= obj && arrlen(runner->instances) > i; i++) {
        const Instance* b = runner->instances[i];
        if (!b->destroyed && b->active && b->objectIndex == obj) return b;
    }
    return nullptr;
}

// The room visit's counts so far (each visit's last such line is its total).
static __attribute__((noinline)) void bckCounts(Runner* runner) {
    const DataWin* dw = runner->dataWin;
    const char* name = bck.room < (int32_t) dw->room.count && dw->room.rooms[bck.room].name != nullptr ? dw->room.rooms[bck.room].name : "-";
    logInfo("bubblecheck: room %d %s visit %u lines %u bubble %u box %u fail %u pc %u free %u screen %u overlap %u\n", (int) bck.room, name,
            (unsigned) bck.visit, (unsigned) bck.lines, (unsigned) bck.bubble, (unsigned) bck.box, (unsigned) bck.fail, (unsigned) bck.pc,
            (unsigned) bck.free, (unsigned) bck.screen, (unsigned) bck.overlap);
    bck.summary = runner->frameCount;
}

static __attribute__((noinline)) void bckRoomEnd(Runner* runner) {
    if (0 <= bck.room && bck.lines != 0) bckCounts(runner);
    bck.visit++;
    bck.summary = runner->frameCount;
    memset(bck.seen, 0, sizeof(bck.seen));
    bck.lines = bck.fail = bck.pc = bck.free = bck.bubble = bck.box = bck.screen = bck.overlap = 0;
    bck.room = runner->currentRoomIndex;
}

// True the first time (this room visit) hash h is seen.
static bool bckFirstSeen(uint32_t h) {
    h |= 1u;
    uint32_t slot = h % BCK_SEEN;
    for (uint32_t probe = 0; bck.seen[slot] != 0 && bck.seen[slot] != h && BCK_SEEN > probe; probe++) slot = (slot + 1) % BCK_SEEN;
    if (bck.seen[slot] == h) return false;
    bck.seen[slot] = h;
    return true;
}

static uint32_t bckHash(uint32_t h, const void* p, size_t n) {
    for (size_t i = 0; n > i; i++) h = (h ^ ((const uint8_t*) p)[i]) * 16777619u;
    return h;
}

// A glyph's native ink mask on row r.
static uint32_t bckMask(const uint8_t* g, int32_t r) {
    return (uint32_t) g[6 + 2 * r] | ((uint32_t) g[7 + 2 * r] << 8);
}

static __attribute__((noinline)) void bckCheck(Runner* runner, const BckLine* l) {
    if (!bckFirstSeen(bckHash(bckHash(2166136261u, l, offsetof(BckLine, obj)), l->text, (size_t) l->n))) return;
    bck.lines++;
    DataWin* dw = runner->dataWin;
    const StFont* f = l->f;
    // the screen: Crisp ink outside the surface where PC's is inside
    int32_t nl = 100000, nr = -100000, nt = 100000, nb = -100000, pl = 100000, pr = -100000, pt = 100000, pb = -100000;
    for (int32_t k = 0; l->glyphs > k; k++) {
        const BckGlyph* q = &l->glyph[k];
        for (int32_t r = 0; f->rows > r; r++) {
            uint32_t m = bckMask(q->g, r);
            if (m == 0) continue;
            if (q->nx + __builtin_ctz(m) < nl) nl = q->nx + __builtin_ctz(m);
            if (q->nx + 31 - __builtin_clz(m) > nr) nr = q->nx + 31 - __builtin_clz(m);
            if (q->ny + r < nt) nt = q->ny + r;
            if (q->ny + r > nb) nb = q->ny + r;
        }
        const int8_t* ext = (const int8_t*) (q->g + 6 + 2 * f->rows);
        for (int32_t r = 0; f->pcRows > r; r++) {
            if (ext[2 * r] > ext[2 * r + 1]) continue;
            if (q->pen + ext[2 * r] + l->ox < pl) pl = q->pen + ext[2 * r] + l->ox;
            if (q->pen + ext[2 * r + 1] + l->ox > pr) pr = q->pen + ext[2 * r + 1] + l->ox;
            if (q->pcy + r + l->oy < pt) pt = q->pcy + r + l->oy;
            if (q->pcy + r + l->oy > pb) pb = q->pcy + r + l->oy;
        }
    }
    int32_t out = 0 > nl ? -nl : 0, pcOut = 0 > pl ? -pl : 0;
    if (nr - 319 > out) out = nr - 319;
    if (0 > nt && -nt > out) out = -nt;
    if (nb - 239 > out) out = nb - 239;
    if (pr - 639 > pcOut) pcOut = pr - 639;
    if (0 > pt && -pt > pcOut) pcOut = -pt;
    if (pb - 479 > pcOut) pcOut = pb - 479;
    if (out > 0 && l->glyphs > 0) {
        bck.screen++;
        logInfo("bubblecheck: %s room %d frame %d obj %s font %s x %d y %d out %d pcout %d '%.*s'\n", pcOut > 0 ? "screenpc" : "screen",
                (int) bck.room, (int) bck.frame, bckObjName(dw, l->obj), f->name, (int) l->pen0, (int) l->y, (int) out, (int) pcOut, (int) l->n,
                l->text);
    }
    // the holder: a bubble, else the battle box
    const char* kind = "free";
    int32_t edge = 0;
    float bl = 0.0f, bt = 0.0f;
    bool found = false;
    for (int32_t i = 0; l->holders > i; i++) {
        const uint8_t* e = l->holder[i].edge;
        uint32_t rows = l->holder[i].rows;
        float left = l->holder[i].left, top = l->holder[i].top;
        if (found && bl + bt > left + top) continue;
        int32_t in = 1000;
        for (int32_t r = 0; 32 > r; r++) {
            int32_t row = (int32_t) (l->y - top) + r;
            if ((l->rows >> r & 1u) == 0) continue;
            if (0 > row || row >= (int32_t) rows || e[row] == 0) in = -1;
            else if (in > e[row]) in = e[row];
        }
        if (0 > in) continue;
        found = true;
        bl = left;
        bt = top;
        edge = (int32_t) left + in;
        kind = l->holder[i].name;
    }
    if (!found) {
        if (bck.dw != dw) {
            bck.dw = dw;
            const char* s = bckBorders;
            for (int32_t k = 0; 4 > k; k++, s += strlen(s) + 1) {
                bck.border[k] = -1;
                for (uint32_t o = 0; dw->objt.count > o; o++)
                    if (dw->objt.objects[o].name != nullptr && strcmp(dw->objt.objects[o].name, s) == 0) bck.border[k] = (int32_t) o;
            }
        }
        const Instance* b[4];
        for (int32_t k = 0; 4 > k; k++) b[k] = bckFirst(runner, bck.border[k]);
        if (b[0] != nullptr && b[1] != nullptr && b[2] != nullptr && b[3] != nullptr && l->pen0 > b[0]->x && b[1]->x > l->pen0 &&
            l->y > b[2]->y && b[3]->y > l->y) {
            found = true;
            edge = (int32_t) b[1]->x + 1;
            kind = "box";
        }
    }
    int32_t pcNative = stGridPixel((float) l->pcRight + (float) l->ox) >> 1, drift = l->right - pcNative;
    const char* status;
    int32_t pc = 0, req = 0, gap = 0;
    if (found) {
        pc = edge - 1 - l->pcRight;
        req = pc / 2; // bubbleclip's rule: PC past the edge, no further past than PC
        if (req > BC_MARGIN) req = BC_MARGIN;
        gap = (stGridPixel((float) edge + (float) l->ox) >> 1) - 1 - l->right;
        if (strcmp(kind, "box") == 0) bck.box++;
        else bck.bubble++;
        if (gap >= req) return;
        status = 0 > pc ? "pc" : "fail";
        if (0 > pc) bck.pc++;
        else bck.fail++;
    } else {
        if (0 >= drift) return;
        status = "free";
        bck.free++;
    }
    logInfo("bubblecheck: %s room %d frame %d obj %s font %s holder %s x %d y %d edge %d pc %d req %d gap %d drift %d '%.*s'\n", status,
            (int) bck.room, (int) bck.frame, bckObjName(dw, l->obj), f->name, kind, (int) l->pen0, (int) l->y, (int) edge, (int) pc,
            (int) req, (int) gap, (int) drift, (int) l->n, l->text);
}

// Line a (above) against line b: the fewest native rows between a glyph of a and one of b in the same or a
// neighbouring column, and the same for PC's ink (640 rows; columns within 2 px, a native column); 1000 when no glyphs are near.
static __attribute__((noinline)) void bckRows(const BckLine* a, const BckLine* b, int32_t* native, int32_t* pc) {
    *native = *pc = 1000;
    for (int32_t i = 0; a->glyphs > i; i++) {
        const BckGlyph* p = &a->glyph[i];
        for (int32_t j = 0; b->glyphs > j; j++) {
            const BckGlyph* q = &b->glyph[j];
            if (p->nx > q->nx + 17 || q->nx > p->nx + 17) continue;
            int8_t lowest[16], highest[16]; // a's lowest ink row and b's highest per column (-1 none)
            memset(lowest, -1, sizeof(lowest));
            memset(highest, -1, sizeof(highest));
            for (int32_t r = 0; a->f->rows > r; r++)
                for (uint32_t m = bckMask(p->g, r); m != 0; m &= m - 1) lowest[__builtin_ctz(m) & 15] = (int8_t) r;
            for (int32_t r = b->f->rows - 1; r >= 0; r--)
                for (uint32_t m = bckMask(q->g, r); m != 0; m &= m - 1) highest[__builtin_ctz(m) & 15] = (int8_t) r;
            for (int32_t c = 0; 16 > c; c++) {
                if (0 > lowest[c]) continue;
                for (int32_t dc = -1; 1 >= dc; dc++) {
                    int32_t cb = p->nx + c + dc - q->nx;
                    if (0 > cb || cb >= 16 || 0 > highest[cb]) continue;
                    int32_t rows = (q->ny + highest[cb]) - (p->ny + lowest[c]) - 1;
                    if (*native > rows) *native = rows;
                }
            }
            const int8_t* ea = (const int8_t*) (p->g + 6 + 2 * a->f->rows);
            const int8_t* eb = (const int8_t*) (q->g + 6 + 2 * b->f->rows);
            for (int32_t r = 0; a->f->pcRows > r; r++) {
                if (ea[2 * r] > ea[2 * r + 1]) continue;
                for (int32_t s = 0; b->f->pcRows > s; s++) {
                    if (eb[2 * s] > eb[2 * s + 1] || p->pen + ea[2 * r] > q->pen + eb[2 * s + 1] + 2 || q->pen + eb[2 * s] > p->pen + ea[2 * r + 1] + 2)
                        continue;
                    int32_t rows = (q->pcy + s) - (p->pcy + r) - 1;
                    if (*pc > rows) *pc = rows;
                }
            }
        }
    }
}

static __attribute__((noinline)) void bckFlush(Runner* runner) {
    for (uint32_t i = 0; bck.count > i; i++) bckCheck(runner, &bck.line[i]);
    // lines over or under each other
    for (uint32_t i = 0; bck.count > i; i++)
        for (uint32_t j = 0; bck.count > j; j++) {
            const BckLine *a = &bck.line[i], *b = &bck.line[j];
            if (i == j || a->y > b->y || (a->y == b->y && i > j) || b->y - a->y > 64.0f) continue;
            int32_t native, pc;
            bckRows(a, b, &native, &pc);
            if (native == 1000 || pc == 1000) continue;
            int32_t req = pc >= 2 ? 1 : 0;
            if (native >= req || (0 > pc && 2 * native >= pc)) continue; // PC overlapping: only worse than PC counts (640 px)
            uint32_t h = bckHash(bckHash(bckHash(bckHash(2166136261u, a, offsetof(BckLine, obj)), a->text, (size_t) a->n), b,
                                         offsetof(BckLine, obj)), b->text, (size_t) b->n);
            if (!bckFirstSeen(h ^ 0x5bd1e995u)) continue;
            bck.overlap++;
            logInfo("bubblecheck: %s room %d frame %d obj %s font %s y %d font2 %s y2 %d rows %d pc %d req %d '%.*s' / '%.*s'\n",
                    0 > pc ? "overlappc" : "overlap", (int) bck.room, (int) bck.frame, bckObjName(runner->dataWin, a->obj), a->f->name, (int) a->y,
                    b->f->name, (int) b->y, (int) native, (int) pc, (int) req, (int) a->n, a->text, (int) b->n, b->text);
        }
    bck.count = 0;
}

// A Crisp glyph (entry g of font f, pen in 640 space, PC cell top y) drawn at gx, gy drawing units on surface x ox, y oy.
static __attribute__((noinline)) void bckGlyph(Renderer* renderer, const StFont* f, const uint8_t* g, float pen, float y, float gx, float gy, float ox, float oy) {
    Runner* runner = renderer->runner;
    if (runner == nullptr) return;
    if (bck.frame != runner->frameCount) {
        bckFlush(runner);
        bck.frame = runner->frameCount;
    }
    if (bck.room != runner->currentRoomIndex) bckRoomEnd(runner);
    else if (runner->frameCount - bck.summary >= 300 && bck.lines != 0) bckCounts(runner); // a run can end in the room
    BckLine* l = nullptr;
    for (uint32_t i = 0; bck.count > i; i++)
        if (bck.line[i].f == f && 2.0f >= (bck.line[i].y > y ? bck.line[i].y - y : y - bck.line[i].y) && pen > bck.line[i].pen &&
            4.0f * (float) f->adv >= pen - bck.line[i].pen)
            l = &bck.line[i]; // shaking text: a pixel or two up or down
    if (l == nullptr) {
        if (bck.count >= BCK_LINES) return;
        l = &bck.line[bck.count++];
        memset(l, 0, offsetof(BckLine, glyph));
        l->f = f;
        l->y = y;
        l->pen0 = pen;
        l->pen = pen;
        l->right = -100000;
        l->pcRight = -100000;
        l->ox = (int32_t) ox;
        l->oy = (int32_t) oy;
        const VMContext* vm = runner->vmContext;
        l->obj = vm != nullptr && vm->currentInstance != nullptr ? vm->currentInstance->objectIndex : -1;
        for (ptrdiff_t i = 0; arrlen(runner->instances) > i && 4 > l->holders; i++) {
            const Instance* b = runner->instances[i];
            uint32_t rows;
            const uint8_t* e;
            if (b->destroyed || !b->active || b->imageXscale != 1.0f || b->imageYscale != 1.0f || 0 > b->spriteIndex ||
                (e = bcEdges(runner, b->spriteIndex, &rows)) == nullptr)
                continue;
            const Sprite* sp = &runner->dataWin->sprt.sprites[b->spriteIndex];
            float left = b->x - (float) sp->originX, top = b->y - (float) sp->originY;
            if (left > pen || top > y || pen >= left + (float) sp->width || y >= top + (float) rows) continue;
            l->holder[l->holders].edge = e;
            l->holder[l->holders].name = sp->name;
            l->holder[l->holders].left = left;
            l->holder[l->holders].top = top;
            l->holder[l->holders++].rows = rows;
        }
    }
    if (pen - l->pen > 1.5f * (float) f->adv && BCK_TEXT > l->n) l->text[l->n++] = ' ';
    if (BCK_TEXT > l->n) l->text[l->n++] = (char) g[0];
    l->pen = pen;
    int32_t nx = stGridPixel(gx + ox) >> 1, z = -99;
    for (int32_t r = 0; f->rows > r; r++) {
        uint32_t m = bckMask(g, r);
        if (m != 0 && 31 - __builtin_clz(m) > z) z = 31 - __builtin_clz(m);
    }
    if (nx + z > l->right) l->right = nx + z;
    if (BCK_GLYPHS > l->glyphs)
        l->glyph[l->glyphs++] = (BckGlyph) {g, (int16_t) nx, (int16_t) (stGridPixel(gy + oy) >> 1), (int16_t) pen, (int16_t) y};
    const int8_t* ext = (const int8_t*) (g + 6 + 2 * f->rows);
    for (int32_t r = 0; f->pcRows > r && 32 > r; r++) {
        if (ext[2 * r] > ext[2 * r + 1]) continue;
        l->rows |= 1u << r;
        if ((int32_t) pen + ext[2 * r + 1] > l->pcRight) l->pcRight = (int32_t) pen + ext[2 * r + 1];
    }
}
#endif // CTRPORT_BUBBLE_CHECK
#endif // !ST_DR_UNIT (bubbleclip)

#ifndef ST_CULL_INLINE // UNDERTALE Old has cullcrisp inside stDraw (the layout event at the top)
// ===[ cullcrisp (credfix, 2026-10-27): Crisp text wholly out of view draws nothing ]===
// The renderer's drawTextImpl skips a string wholly above or below the view (the true ending's credits dodger draws
// some 190 names a frame, most of them off screen), but stDraw carried and blitted every glyph of every string: in
// Azahar Old that cost the dodger ~2.6 ms of CPU a frame against Smooth text. The outer table's text entries now ask
// stCullOut first. A string stDraw would draw crisp (its own checks, its atlas already made) that lies wholly above or
// below the view takes its glyphs' shake slots as stDraw takes them, so later text shakes as before, and draws
// nothing; any other string goes to stDraw as before. Linked last, outside .text.* (late_reserve.ld), and stDraw,
// stDrawText and stDrawTextColor are unchanged, so nothing merged before it moves.
#ifdef ST_DR_UNIT
#define ST_CULL __attribute__((noinline, section(".text_cullcrisp_dr_late")))
#else
#define ST_CULL __attribute__((noinline, section(".text_cullcrisp_late")))
#endif
// fn's address through an empty asm, so the call stays an out-of-line call to the function as compiled before
#define ST_CULL_NEXT(fn) ({ __typeof__(&fn) next_ = fn; __asm__("" : "+r"(next_)); next_; })

// text_utils.h's helpers for the ASCII text stDraw takes, as copies: stDraw's own calls stay inlined as before (a
// second caller would make the compiler put them out of line and move stDraw's code).
#define ST_CULL_INL __attribute__((always_inline)) static inline
ST_CULL_INL bool stCullNl(char c) {
    return c == '\n' || c == '\r';
}
ST_CULL_INL FontGlyph* stCullGlyph(Font* font, uint8_t ch) {
    return 128 > ch ? font->glyphLUT[ch] : nullptr;
}
ST_CULL_INL float stCullKern(const FontGlyph* glyph, uint8_t next) {
    for (uint32_t k = 0; glyph->kerningCount > k; k++) {
        if (glyph->kerning[k].character == (int16_t) next) return glyph->kerning[k].shiftModifier;
    }
    return 0;
}

static ST_CULL bool stCullOut(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
#ifdef CTRPORT_BUBBLE_CHECK
    if (renderer != nullptr) return false; // the bubble check sees every Crisp glyph
#endif
    DataWin* dw = renderer->dataWin;
    int32_t fontIndex = renderer->drawFont;
    if (text == nullptr || angleDeg != 0.0f || 0 > fontIndex || dw == nullptr || dw->font.count <= (uint32_t) fontIndex) return false;
    StFont* f = nullptr;
    for (uint32_t i = 0; st.fontCount > i; i++) {
        if (st.fonts[i].font == fontIndex && (st.keys >> st.fonts[i].key & 1u) != 0) f = &st.fonts[i];
    }
    if (f == nullptr || 0 >= f->made) return false; // an atlas not yet made: stDraw makes it
    Font* font = &dw->font.fonts[fontIndex];
    if (xscale * font->scaleX != 1.0f || yscale * font->scaleY != 1.0f) return false;
    const uint8_t* rp = (const uint8_t*) renderer;
    const C3D_Mtx* m = (const C3D_Mtx*) (rp + Smalltext_layout[L_PROJECTION]);
    const StRect* vp = (const StRect*) (rp + Smalltext_layout[L_VIEWPORT]);
    if (m->r[1].y >= 0.0f) return false; // a mirrored y axis: stDraw's surface y (stSurfaceOy) is not the view's
    // The view first (most text is in it, and goes to stDraw at once): the lines' tops from stDraw's cursor, with a
    // margin past the glyphs' rows, the font's row shift and a shake. Lines as TextUtils_countLines counts them.
    int32_t textLen = 0, lineCount = 1;
    for (; text[textLen] != '\0'; textLen++) {
        if (!stCullNl(text[textLen])) continue;
        lineCount++;
        if (stCullNl(text[textLen + 1]) && text[textLen] != text[textLen + 1]) textLen++;
    }
    if (textLen == 0) return false;
    float lineStride = 0.0f > lineSeparation ? (font->maxGlyphHeight > 0 ? (float) font->maxGlyphHeight : font->emSize)
                                             : (lineSeparation / (font->scaleY != 0.0f ? font->scaleY : 1.0f));
    float valignOffset = 0.0f;
    if (renderer->drawValign == 1) valignOffset = -(float) lineCount * lineStride / 2.0f;
    else if (renderer->drawValign == 2) valignOffset = -(float) lineCount * lineStride;
    float cursorY = valignOffset - (float) font->ascenderOffset;
    float span = (float) (lineCount - 1) * lineStride, top = y + cursorY + stSurfaceOy(renderer);
    float bottom = top + (span > 0.0f ? span : 0.0f) + 2.0f * (float) f->rows + 32.0f;
    top += (0.0f > span ? span : 0.0f) - 32.0f;
    if (bottom >= (float) vp->y && (float) (vp->y + vp->h) >= top) return false;
    for (int32_t i = 0; textLen > i; i++) {
        uint8_t c = (uint8_t) text[i];
        if (c >= 128) return false;
        if (c == ' ' || stCullNl((char) c)) continue;
        if (f->lut[c] == 0 || stCullGlyph(font, c) == nullptr) return false;
    }
    if (!stOneToOne(renderer, fontIndex)) return false;
    // Out of view: stDraw's glyph walk (all ASCII now), taking a shake slot wherever stDraw takes one.
    bool fixW = (st.keys >> ST_KEY_FIX_W & 1u) != 0 && f->fixW != 0;
    bool fixStar = (st.keys >> ST_KEY_FIX_STAR & 1u) != 0 && f->fixStar != 0;
    int32_t lineStart = 0;
    for (int32_t lineIdx = 0; lineCount > lineIdx; lineIdx++) {
        int32_t lineEnd = lineStart;
        while (textLen > lineEnd && !stCullNl(text[lineEnd])) lineEnd++;
        int32_t lineLen = lineEnd - lineStart;
        const uint8_t* line = (const uint8_t*) text + lineStart;
        float cursorX = 0.0f;
        if (renderer->drawHalign != 0) { // TextUtils_measureLineWidth
            float lineWidth = 0.0f;
            for (int32_t i = 0; lineLen > i; i++) {
                FontGlyph* glyph = stCullGlyph(font, line[i]);
                if (glyph == nullptr) continue;
                lineWidth += glyph->shift;
                if (lineLen > i + 1) lineWidth += stCullKern(glyph, line[i + 1]);
            }
            cursorX = renderer->drawHalign == 1 ? -lineWidth / 2.0f : -lineWidth;
        }
        for (int32_t i = 0; lineLen > i; i++) {
            uint8_t ch = line[i];
            FontGlyph* glyph = stCullGlyph(font, ch);
            if (glyph == nullptr) continue;
            if (ch != ' ' && glyph->sourceWidth != 0 && glyph->sourceHeight != 0) {
                uint32_t e = (uint32_t) f->lut[ch] - 1u;
                if (ch == 'W' && fixW) e = (uint32_t) f->fixW - 1u;
                else if (ch == '*' && fixStar) e = (uint32_t) f->fixStar - 1u;
                const uint8_t* g = stEntry(f, e);
                if (g[2] != 0) {
                    float gx = x + cursorX + 2.0f * (float) (int8_t) g[3], gy = y + cursorY;
                    if (gx != (float) (int32_t) gx || gy != (float) (int32_t) gy) (void) stSlotHash(renderer);
                }
            }
            cursorX += glyph->shift;
            if (lineLen > i + 1) cursorX += stCullKern(glyph, line[i + 1]);
        }
        cursorY += lineStride;
        lineStart = lineEnd + 1; // TextUtils_skipNewline
        if (textLen > lineStart && stCullNl(text[lineStart]) && text[lineEnd] != text[lineStart]) lineStart++;
    }
    return true;
}

static ST_CULL void stCullText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, float lineSeparation) {
    if (stCullOut(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation)) return;
    ST_CULL_NEXT(stDrawText)(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation);
}

static ST_CULL void stCullTextColor(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg, int32_t c1, int32_t c2,
                                    int32_t c3, int32_t c4, float alpha, float lineSeparation) {
    if (c1 == c2 && c2 == c3 && c3 == c4 && stCullOut(renderer, text, x, y, xscale, yscale, angleDeg, lineSeparation)) return;
    ST_CULL_NEXT(stDrawTextColor)(renderer, text, x, y, xscale, yscale, angleDeg, c1, c2, c3, c4, alpha, lineSeparation);
}
#endif // !ST_CULL_INLINE (cullcrisp)
#endif // ENABLE_WAD16 || ST_DR_UNIT

#if defined(ENABLE_WAD16) && !defined(ST_DR_UNIT)
// UT-026: PC extents have no native row-phase shift. Undo only that shift
// for the PC contact test, on the reproduced writer's first line. Native
// contact tests, row placement and glyphs stay intact.
static __attribute__((noinline, section(".text.late.asrtext"))) int32_t asrPcDy(Renderer* renderer, const StFont* f, float y, float py, int32_t dy) {
    if (bc.last == nullptr || (bc.last->ok & 2u) == 0 || bc.v[BC_MYY] != bc.v[BC_Y0]) return dy;
    float oy = stSurfaceOy(renderer);
    return dy - 2 * ((int32_t) f->rowShift[stGridPixel(y + oy) & 1] - (int32_t) f->rowShift[stGridPixel(py + oy) & 1]);
}
#endif
