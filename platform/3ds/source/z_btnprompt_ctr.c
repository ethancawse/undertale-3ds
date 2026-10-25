// The 3DS side of UNDERTALE's console prompts (btnprompt; engine/src/z_btnprompt.c has the game side): which
// button the prompts name, from the live button map, and the button glyphs (assets/btnprompt_glyphs.txt in the
// RomFS, our own art) that the Tough Glove's prompt draws.
//
// main.c calls CtrBtnprompt_setDisplay where it applied the loaded settings (CtrRenderer_setDisplay) and
// CtrBtnprompt_saveEdits where it saved the options menu's edits (CtrArgui_saveEdits, which calls CtrDisplay_saveEdits): the labels change at load
// and when the menu closes, never in a frame. Late code only (late_reserve.ld, "btnprompt"); the strings are named
// arrays in the late code area, and the file is compiled without the printf builtins and switch tables
// (CMakeLists.txt), which would add constants to .rodata.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "ctr_benchromfs.h"
#include "ctr_display.h"
#include "ctr_input_map.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#define BTN_LATE __attribute__((noinline, section(".text.late.btnprompt")))
#define BTN_CONST __attribute__((section(".rodata.btnprompt_in_text")))
#define BTN_BSS __attribute__((section(".bss.late.btnprompt")))

// engine/src/z_btnprompt.c.
void Btnprompt_setLabels(const char letters[3], const char* title);

#if defined(ENABLE_WAD16) // UNDERTALE's builds; DELTARUNE's have only the two wrappers at the end, which call through.
#define GLYPH_W 12
#define GLYPH_H 14
#define GLYPHS 6
#define TITLE_MAX 18 // the title prompt slot of UNDERTALE 1.08 (z_btnprompt.c writes at most the slot length)

// The buttons with a glyph, in map row order (MAP_ROW_A to MAP_ROW_R), and the names the title uses for the rows
// that can confirm (MAP_ROW_A to MAP_ROW_START).
static const char btnButtons[8] BTN_CONST = "ABXYLR";
static const char btnRowNames[MAP_ROW_START + 1][8] BTN_CONST = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "START" };
// The glyph file's pixels: r rim, f face, s shadow, w letter ('.' is clear), as 0xRRGGBB.
static const char btnPaletteKeys[8] BTN_CONST = "rfsw";
static const uint32_t btnPalette[4] BTN_CONST = { 0xD8D8D8u, 0x383838u, 0x787878u, 0xFFFFFFu };

static const char btnRomfs[] BTN_CONST = "romfs";
static const char btnGlyphPath[] BTN_CONST = "romfs:/btnprompt_glyphs.txt";
static const char btnReadMode[] BTN_CONST = "rb";
static const char btnGlyphWord[] BTN_CONST = "glyph ";
static const char btnTitleStart[] BTN_CONST = "[PRESS ";
static const char btnTitleOr[] BTN_CONST = " OR ";
static const char btnTitleEnd[] BTN_CONST = "]";
static const char btnLogGlyphs[] BTN_CONST = "Buttons: no glyphs in %s, so the Tough Glove keeps its PC prompt\n";
static const char btnLogLabels[] BTN_CONST = "Buttons: confirm %s, cancel %s, menu %s, title %s\n";
static const char btnLogSprite[] BTN_CONST = "Buttons: the %c glyph's sprite couldn't be made\n";
static const char btnKeep[] BTN_CONST = "(the key)";

// The glyphs as read (the palette letters), whether the file was read, and the sprites made from them in this VM's
// run (sprite index + 1; 0 until the game first asks).
static char btnArt[GLYPHS][GLYPH_H][GLYPH_W] BTN_BSS;
static bool btnGlyphsRead BTN_BSS;
static bool btnGlyphsOk BTN_BSS;
static int32_t btnSprite[GLYPHS] BTN_BSS;
// The button each prompt names: Z (confirm), X (cancel), C (menu); 0 keeps the key's letter.
static char btnLetters[3] BTN_BSS;

static BTN_LATE int32_t btnGlyphIndex(char button) {
    for (int32_t i = 0; GLYPHS > i; i++) {
        if (btnButtons[i] == button) return i;
    }
    return -1;
}

static BTN_LATE int32_t btnPaletteIndex(char c) {
    for (int32_t i = 0; 4 > i; i++) {
        if (btnPaletteKeys[i] == c) return i;
    }
    return -1;
}

static BTN_LATE bool btnReadGlyphs(void) {
    if (R_FAILED(CtrRomfs_init(btnRomfs))) return false;
    FILE* f = fopen(btnGlyphPath, btnReadMode);
    if (f == nullptr) return false;
    char line[160];
    int32_t glyph = -1, row = 0;
    uint32_t complete = 0;
    bool ok = true;
    bool lineStart = true;
    while (ok && fgets(line, sizeof(line), f) != nullptr) {
        size_t n = strlen(line);
        bool start = lineStart;
        lineStart = n != 0 && line[n - 1] == '\n';
        // A comment, or the rest of a line longer than the buffer.
        if (!start || line[0] == '#') continue;
        if (strncmp(line, btnGlyphWord, sizeof(btnGlyphWord) - 1) == 0) {
            glyph = btnGlyphIndex(line[sizeof(btnGlyphWord) - 1]);
            row = 0;
            ok = glyph >= 0;
            continue;
        }
        while (n != 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        if (n == 0) continue;
        ok = glyph >= 0 && GLYPH_H > row && n == GLYPH_W;
        for (int32_t x = 0; ok && GLYPH_W > x; x++) ok = line[x] == '.' || btnPaletteIndex(line[x]) >= 0;
        if (!ok) break;
        memcpy(btnArt[glyph][row], line, GLYPH_W);
        row++;
        if (row == GLYPH_H) complete |= 1u << glyph;
    }
    fclose(f);
    return ok && complete == (1u << GLYPHS) - 1u;
}

// Whether the glyphs could be read (once per app), and forgets the sprites of the last VM's run.
BTN_LATE bool CtrBtnprompt_begin(void) {
    memset(btnSprite, 0, sizeof(btnSprite));
    if (!btnGlyphsRead) {
        btnGlyphsRead = true;
        btnGlyphsOk = btnReadGlyphs();
        if (!btnGlyphsOk) logInfo(btnLogGlyphs, btnGlyphPath);
    }
    return btnGlyphsOk;
}

// The sprite of the glyph of the button that sends control's key ('Z', 'X' or 'C'), made the first time it is asked
// for (a Draw event: the renderer makes sprites from surfaces only in a frame), or -4, as scr_getbuttonsprite returns
// for a button without a sprite.
BTN_LATE int32_t CtrBtnprompt_buttonSprite(Renderer* renderer, char control) {
    int32_t which = control == 'Z' ? 0 : control == 'X' ? 1 : control == 'C' ? 2 : -1;
    if (0 > which || !btnGlyphsOk || renderer == nullptr || renderer->dataWin == nullptr) return -4;
    int32_t glyph = btnLetters[which] != 0 ? btnGlyphIndex(btnLetters[which]) : -1;
    if (0 > glyph) return -4;
    Sprt* sprt = &renderer->dataWin->sprt;
    int32_t made = btnSprite[glyph] - 1;
    if (made >= 0 && sprt->count > (uint32_t) made && sprt->sprites[made].present) return made;
    btnSprite[glyph] = 0;

    // RGBA rows, bottom row first (the renderer's surfaceSetPixels).
    uint8_t rgba[GLYPH_W * GLYPH_H * 4];
    for (int32_t y = 0; GLYPH_H > y; y++) {
        uint8_t* out = rgba + (size_t) (GLYPH_H - 1 - y) * GLYPH_W * 4;
        for (int32_t x = 0; GLYPH_W > x; x++) {
            int32_t c = btnPaletteIndex(btnArt[glyph][y][x]);
            uint32_t rgb = c >= 0 ? btnPalette[c] : 0u;
            out[x * 4 + 0] = (uint8_t) (rgb >> 16);
            out[x * 4 + 1] = (uint8_t) (rgb >> 8);
            out[x * 4 + 2] = (uint8_t) rgb;
            out[x * 4 + 3] = c >= 0 ? 255 : 0;
        }
    }
    const RendererVtable* vt = renderer->vtable;
    int32_t sprite = -1;
    int32_t surface = vt->createSurface != nullptr ? vt->createSurface(renderer, GLYPH_W, GLYPH_H) : -1;
    if (surface >= 0) {
        if (vt->surfaceSetPixels != nullptr && vt->surfaceSetPixels(renderer, surface, rgba) && vt->createSpriteFromSurface != nullptr)
            sprite = vt->createSpriteFromSurface(renderer, surface, 0, 0, GLYPH_W, GLYPH_H, false, false, 0, 0);
        if (vt->surfaceFree != nullptr) vt->surfaceFree(renderer, surface);
    }
    if (0 > sprite) {
        logInfo(btnLogSprite, btnButtons[glyph]);
        return -4;
    }
    btnSprite[glyph] = sprite + 1;
    return sprite;
}

// The first non-directional row from A to R that sends key, else one that sends alt; 0 if none does.
static BTN_LATE char btnLetterFor(const CtrInputMap* map, uint8_t key, uint8_t alt) {
    for (int32_t pass = 0; 2 > pass; pass++) {
        uint8_t want = pass == 0 ? key : alt;
        for (int32_t row = MAP_ROW_A; MAP_ROW_R >= row; row++) {
            if (map->keys[row] == want) return btnButtons[row];
        }
    }
    return 0;
}

static BTN_LATE size_t btnAppend(char* out, size_t at, const char* text) {
    size_t n = strlen(text);
    if (TITLE_MAX >= at + n) memcpy(out + at, text, n);
    return at + n;
}

// The title's prompt: the first two buttons (A to START) that confirm, "[PRESS A OR START]", else only the first if
// both don't fit; false if no button confirms.
static BTN_LATE bool btnTitle(const CtrInputMap* map, char out[TITLE_MAX + 1]) {
    int32_t first = -1, second = -1;
    for (int32_t row = MAP_ROW_A; MAP_ROW_START >= row; row++) {
        if (map->keys[row] != GAME_KEY_Z && map->keys[row] != GAME_KEY_ENTER) continue;
        if (0 > first) first = row;
        else if (0 > second) second = row;
    }
    if (0 > first) return false;
    for (int32_t both = 1; both >= 0; both--) {
        if (both && 0 > second) continue;
        size_t at = btnAppend(out, 0, btnTitleStart);
        at = btnAppend(out, at, btnRowNames[first]);
        if (both) {
            at = btnAppend(out, at, btnTitleOr);
            at = btnAppend(out, at, btnRowNames[second]);
        }
        at = btnAppend(out, at, btnTitleEnd);
        if (TITLE_MAX >= at) {
            out[at] = '\0';
            return true;
        }
    }
    return false;
}

static BTN_LATE void btnLabels(const CtrInputMap* map) {
    char letters[3] = {
        btnLetterFor(map, GAME_KEY_Z, GAME_KEY_ENTER),
        btnLetterFor(map, GAME_KEY_X, GAME_KEY_SHIFT),
        btnLetterFor(map, GAME_KEY_C, GAME_KEY_CTRL),
    };
    char title[TITLE_MAX + 1];
    bool haveTitle = btnTitle(map, title);
    memcpy(btnLetters, letters, sizeof(btnLetters));
    Btnprompt_setLabels(letters, haveTitle ? title : nullptr);
    char names[3][2];
    for (int32_t i = 0; 3 > i; i++) {
        names[i][0] = letters[i];
        names[i][1] = '\0';
    }
    logInfo(btnLogLabels, letters[0] != 0 ? names[0] : btnKeep, letters[1] != 0 ? names[1] : btnKeep,
            letters[2] != 0 ? names[2] : btnKeep, haveTitle ? title : btnKeep);
}

#endif // ENABLE_WAD16

// main.c, where the loaded settings are applied: then the prompts name the buttons of the loaded map.
BTN_LATE void CtrBtnprompt_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    CtrRenderer_setDisplay(renderer, settings);
#if defined(ENABLE_WAD16)
    btnLabels(&settings->controls);
#endif
}

// main.c, where the options menu's edits are saved as it closes: then the prompts name the buttons of the new map.
BTN_LATE void CtrBtnprompt_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
    CtrArgui_saveEdits(before, after, path, caps);  // argui's settings, then the display's
#if defined(ENABLE_WAD16)
    btnLabels(&after->controls);
#endif
}
