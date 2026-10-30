// In-game UI for UNDERTALE / DELTARUNE 1.0.0-beta.1 (argui):
// 1. First-boot options menu hint ("SELECT: Options"), shown on the screen without the game when free, or small in a
//    corner of the game screen for the first ~5 seconds before fading out; dismissed permanently once SELECT opens the menu.
// 2. Configurable FPS counter (fps_counter: Off / Top / Bottom; fps_size: Small / Medium / Large), drawn in the game's
//    font (fnt_maintext / fnt_main) when available or whole-pixel fallback digits, inside the 40-px side bar in Standard top mode.
// 3. Game-themed options menu (black box with white border, white/yellow text, runtime font fnt_maintext/fnt_main with
//    libctru console-font fallback, runtime red SOUL cursor spr_heartsmall, and runtime move/select sounds
//    snd_squeak/snd_menumove + snd_select from the player's own data.win) and concise one-line player help.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "ctr_display.h"
#include "ctr_fps_clock.h"
#include "ctr_fpsbox.h"
#line 16
#ifndef CTR_SCREENS_SETTINGS_ONLY
#include "audio_system.h"
#include "ctr_benchmark.h"
#include "ctr_frame_pacer.h"
#include "ctr_log.h"
#include "ctr_menu.h"
#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "data_win.h"
#include "runner.h"
#include "stb_ds.h"
#endif

#if defined(__3DS__) && defined(__GNUC__)
#pragma GCC optimize("Os")
#define ARGUI_CODE __attribute__((noinline, optimize("Os"), section(".text.late.argui")))
#define ARGUI_STATIC __attribute__((optimize("Os"), section(".text.late.argui"))) static
#define ARGUI_NOINLINE __attribute__((noinline, optimize("Os"), section(".text.late.argui"))) static
#define ARGUI_RO __attribute__((section(".rodata.late.argui")))
#define ARGUI_BSS __attribute__((section(".bss.late.argui")))
#else
#define ARGUI_CODE
#define ARGUI_STATIC static
#define ARGUI_NOINLINE static
#define ARGUI_RO
#define ARGUI_BSS
#endif

#define ARGUI_STR(name, text) static const char name[(sizeof(text) + 3u) & ~3u] ARGUI_RO = text

// ===[ State ]===

typedef struct {
    uint8_t cfg[3]; // [0] = fps_counter (0..2), [1] = fps_size (0..2), [2] = menu_hint_seen (0..1)
    bool menuIsOpen;
#ifndef CTR_SCREENS_SETTINGS_ONLY
    uint8_t sx;
    uint8_t sy;
    uint8_t adv;
    bool rgb565;
    char fpsBuf[3];
    uint16_t hintFrames;
    uint16_t fpsFrames;
    int16_t assetIds[5]; // [0] = sndMove, [1] = sndSelect, [2] = fntSmall, [3] = fntTheme, [4] = sprHeartSmall
    uint32_t fpsVb;
    void* fb;
    CtrTextureCache* texCache;
    AudioSystem* audio;
#endif
} ArguiState;

ArguiState CtrArgui_state ARGUI_BSS;
#define g CtrArgui_state

#define gFpsScreen g.cfg[0]
#define gFpsSize   g.cfg[1]
#define gHintSeen  g.cfg[2]

extern uint8_t CtrScreens_game;
extern uint8_t CtrScreens_pacerGame;
extern bool CtrScreens_otherOff;
extern bool CtrScreens_onBottom;
extern bool CtrScreens_consolesOnTop;
extern bool CtrScreens_sharedBacklight;
bool CtrScreens_benchLaunched(void);
bool CtrBench_running(void);
void __wrap_CtrDisplay_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool __wrap_CtrDisplay_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __wrap_CtrDisplay_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps);

// ===[ Settings (fps_counter, fps_size, menu_hint_seen) ]===

ARGUI_STR(arguiFmtFps, " fps_counter = %d");
ARGUI_STR(arguiFmtSize, " fps_size = %d");
ARGUI_STR(arguiFmtHint, " menu_hint_seen = %d");
ARGUI_STR(arguiReadMode, "r");
ARGUI_STR(arguiAppendMode, "a");
ARGUI_STR(arguiSaveFmt, "fps_counter = %d\nfps_size = %d\nmenu_hint_seen = %d\n");

static const char* const arguiCfgFmts[3] ARGUI_RO = { arguiFmtFps, arguiFmtSize, arguiFmtHint };

ARGUI_NOINLINE bool arguiParseLine(const char* line) {
    int value;
    for (uint32_t i = 0; 3u > i; i++) {
        if (sscanf(line, arguiCfgFmts[i], &value) == 1 && (uint32_t) value <= 2u - (i >> 1)) {
            g.cfg[i] = (uint8_t) value;
            return true;
        }
    }
    return false;
}

ARGUI_CODE void CtrArgui_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
#ifdef CTR_SCREENS_SETTINGS_ONLY
    __wrap_CtrDisplay_load(settings, path, caps);
#else
    CtrDisplay_load(settings, path, caps);
    g.fpsVb = 30u << 16;
    memcpy(&g.hintFrames, &g.fpsVb, 4);
#endif
    *(uint32_t*) g.cfg = 0;
    FILE* f = fopen(path, arguiReadMode);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) arguiParseLine(line);
        fclose(f);
    }
    if (CtrScreens_benchLaunched()) gHintSeen = 1;
}

ARGUI_CODE bool CtrArgui_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    if (arguiParseLine(line)) return true;
#ifdef CTR_SCREENS_SETTINGS_ONLY
    return __wrap_CtrDisplay_override(settings, line, caps);
#else
    return CtrDisplay_override(settings, line, caps);
#endif
}

ARGUI_CODE void CtrArgui_saveEdits(const CtrDisplaySettings* before, const CtrDisplaySettings* after, const char* path, CtrDisplayCaps caps) {
#ifdef CTR_SCREENS_SETTINGS_ONLY
    __wrap_CtrDisplay_saveEdits(before, after, path, caps);
#else
    CtrDisplay_saveEdits(before, after, path, caps);
#endif
    FILE* f = fopen(path, arguiAppendMode);
    if (f != nullptr) {
        fprintf(f, arguiSaveFmt, (int) g.cfg[0], (int) g.cfg[1], (int) g.cfg[2]);
        fclose(f);
    }
}

#ifndef CTR_SCREENS_SETTINGS_ONLY

// ===[ Runtime theme assets from the player's own data.win ]===

#ifdef ENABLE_WAD16
ARGUI_STR(nameFntTheme, "fnt_maintext");
ARGUI_STR(nameSndMove, "snd_squeak");
#else
ARGUI_STR(nameFntTheme, "fnt_main");
ARGUI_STR(nameSndMove, "snd_menumove");
#endif
ARGUI_STR(nameFntSmall, "fnt_small");
ARGUI_STR(nameSprHeartSmall, "spr_heartsmall");
ARGUI_STR(nameSndSelect, "snd_select");

static const char* const arguiAssetNames[5] ARGUI_RO = {
    nameSndMove, nameSndSelect, nameFntSmall, nameFntTheme, nameSprHeartSmall
};
static const uint8_t arguiMorton[8] ARGUI_RO = { 0, 1, 4, 5, 16, 17, 20, 21 };

void CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner); // z_roomload.c; it calls __real_
void CtrOptmenu_prepareGlyphs(void);
void __wrap_CtrTextures_prefetchRoom(CtrTextureCache* cache, Runner* runner);

ARGUI_CODE void __wrap_CtrTextures_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    g.texCache = cache;
    g.audio = runner->audioSystem;
    for (uint32_t i = 0; 5u > i; i++) {
        g.assetIds[i] = (int16_t) shget(runner->assetsByName, arguiAssetNames[i]);
    }
    CtrOptmenu_prepareGlyphs(); // ctr_optmenu.c: the menu's glyphs, from the tiles resident now
    CtrRoomload_prefetchRoom(cache, runner);
}

ARGUI_STATIC bool arguiSampleGlyph(uint8_t ch, int32_t gx, int32_t gy) {
    CtrTextureCache* cache = g.texCache;
    const DataWin* dw = cache->dataWin;
    int32_t tpagIndex = -1;
    uint32_t lx = (uint32_t) gx, ly = (uint32_t) gy;
    int32_t fid = g.assetIds[2u + g.menuIsOpen];
    if (ch == 1u) {
        if (ly < 9u && g.assetIds[4] >= 0) tpagIndex = dw->sprt.sprites[g.assetIds[4]].tpagIndices[0];
        ch = '>';
    } else if (fid >= 0 && dw->font.fonts[fid].glyphLUT[ch] != nullptr) {
        const Font* f = &dw->font.fonts[fid];
        const FontGlyph* gl = f->glyphLUT[ch];
        lx = (uint32_t) (gx - gl->offset);
        ly = (uint32_t) (gy + (g.menuIsOpen << 2));
        if (lx >= gl->sourceWidth || ly >= gl->sourceHeight) return false;
        tpagIndex = f->tpagIndex;
        lx += gl->sourceX;
        ly += gl->sourceY;
    }
    if (tpagIndex < 0) {
        return (uint32_t) (gx | gy) < 8u && (consoleGetDefault()->font.gfx[(uint32_t) ch * 8u + (uint32_t) gy] & (0x80u >> gx)) != 0;
    }
    const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    CtrPage* page = CtrTextures_acquire(cache, (uint32_t) tpag->texturePageId);
    uint32_t px = tpag->sourceX + lx, py = tpag->sourceY + ly;
    int32_t tx = (int32_t) (px >> 8), ty = (int32_t) (py >> 8);
    const CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
    if (!tile->valid) CtrTextures_ensureTiles(cache, page, tx, ty, tx, ty);
    uint32_t idx = ((py & 0xF8u) << 8) | ((px & 0xF8u) << 3) |
                   arguiMorton[px & 7u] | ((uint32_t) arguiMorton[py & 7u] << 1);
    return (((const uint16_t*) tile->tex.data)[idx << (tile->tex.fmt == GPU_RGBA8)] & 1u) != 0;
}
#if defined(ENABLE_WAD17) // UNDERTALE never runs this (argcheck, UT late-text reclaim)
ARGUI_STATIC void arguiPlaySound(bool selectSound) {
    int32_t id = g.assetIds[selectSound];
    if (id >= 0) g.audio->vtable->playSound(g.audio, id, 100, false);
}
#endif
// ===[ Unified text renderer ]===

WAD17_ONLY(ARGUI_NOINLINE) WAD17_NONE(ARGUI_CODE) void arguiDrawText(int32_t x, int32_t y, const char* s, uint32_t col) { // UNDERTALE: global (CtrFps_drawText)
    for (; *s != '\0'; s++, x += g.adv * g.sx) {
        for (int32_t gy = 0; 12 > gy; gy++) {
            for (int32_t gx = 0; (g.menuIsOpen ? 9 : 6) > gx; gx++) {
                bool on = arguiSampleGlyph((uint8_t) *s, gx, gy);
                if (!on) continue;
                uint16_t c565 = (uint16_t) (on ? col : 0u);
                for (int32_t dx = 0; g.sx > dx; dx++) {
                    size_t idx = (size_t) (x + gx * g.sx + dx) * 240u + (size_t) (239 - y - gy * g.sy);
                    for (int32_t dy = 0; g.sy > dy; dy++, idx--) {
                        if (g.rgb565) {
                            ((uint16_t*) g.fb)[idx] = c565;
                        } else {
                            uint8_t* p = (uint8_t*) g.fb + idx * 3u;
                            p[0] = p[1] = p[2] = (uint8_t) c565;
                        }
                    }
                }
            }
        }
    }
}

// ===[ Themed options menu (16 items in 3 groups) ]===

typedef enum {
    ITEM_GAME_SCREEN,
    ITEM_TOP_MODE,
    ITEM_STEREO,
    ITEM_DIALOGUE,
    ITEM_OTHER_SCREEN,
    ITEM_FPS_COUNTER,
    ITEM_FPS_SIZE,
    ITEM_SMOOTHING,
    ITEM_DRAWING,
    ITEM_FRAME_SKIP,
    ITEM_EXACT_COLOURS,
    ITEM_CONTROLS,
    ITEM_PERF_TEST,
    ITEM_LEGAL,
    ITEM_RESUME,
    ITEM_QUIT,
    ITEM_COUNT
} MenuItem;

bool CtrLegal_shown(void);
#if defined(ENABLE_WAD17) // UNDERTALE never runs this (argcheck, UT late-text reclaim): the menu rows, values and help
ARGUI_STR(lblGame, "Game screen");
ARGUI_STR(lblTop, "Top screen");
ARGUI_STR(lblStereo, "3D mode");
ARGUI_STR(lblDialogue, "Dialogue box");
ARGUI_STR(lblOther, "Other screen");
ARGUI_STR(lblFps, "FPS counter");
ARGUI_STR(lblFpsSize, "FPS size");
ARGUI_STR(lblSmoothing, "Smoothing");
ARGUI_STR(lblDrawing, "Drawing");
ARGUI_STR(lblFrameSkip, "Frame skip");
ARGUI_STR(lblExact, "Exact colours");
ARGUI_STR(lblControls, "Controls");
ARGUI_STR(lblPerf, "Performance test");
ARGUI_STR(lblLegal, "Legal notices");
ARGUI_STR(lblResume, "Back to the game");
ARGUI_STR(lblQuit, "Quit game");

static const int8_t arguiToScreens[ITEM_COUNT] ARGUI_RO = {
    0, 1, 3, 6, 5, -1, -1, 4, 2, 8, 9, 7, 10, 11, 12, 13
};

ARGUI_STR(strEmpty, "");
ARGUI_STR(strHeart, "\x01");
ARGUI_STR(valTop, "< Top >");
ARGUI_STR(valBottom, "< Bottom >");
ARGUI_STR(valStandard, "< Standard >");
ARGUI_STR(valStdPlain, "Standard");
ARGUI_STR(valWide, "< Wide >");
ARGUI_STR(valHd, "< HD >");
ARGUI_STR(valNormal, "< Normal >");
ARGUI_STR(valOff, "< Off >");
ARGUI_STR(valOn, "< On >");
ARGUI_STR(valBlank, "< Blank >");
ARGUI_STR(valStatsLog, "< Stats and log >");
ARGUI_STR(valSmall, "< Small >");
ARGUI_STR(valMedium, "< Medium >");
ARGUI_STR(valLarge, "< Large >");
ARGUI_STR(valNative, "< Native >");
ARGUI_STR(valFull, "< Full >");
ARGUI_STR(valSmart, "< Smart >");
ARGUI_STR(valAuto, "< Auto >");
ARGUI_STR(val20Fps, "< 20 fps >");
ARGUI_STR(val15Fps, "< 15 fps >");
ARGUI_STR(valDefault, "< Default >");
ARGUI_STR(valBConfirms, "< B confirms >");
ARGUI_STR(valCustom, "< Custom >");

ARGUI_STR(disNo3d, "No 3D screen.");
ARGUI_STR(disNeedsTop, "Needs the game on top.");
ARGUI_STR(disNotWide, "Not in wide mode.");
ARGUI_STR(disNew3ds, "New 3DS only.");

ARGUI_STR(hlpGameTop, "Play on the top screen.");
ARGUI_STR(hlpGameBot, "Play on the bottom screen.");
ARGUI_STR(hlpTopStd, "Normal size, works with 3D.");
ARGUI_STR(hlpTopWide, "Sharper, without 3D.");
ARGUI_STR(hlp3dHd, "Sharper 3D.");
ARGUI_STR(hlp3dNormal, "Simple 3D.");
ARGUI_STR(hlpDlgTop, "Dialogue stays in the game.");
ARGUI_STR(hlpDlgBot, "Dialogue moves to the bottom screen.");
ARGUI_STR(hlpOtherOff, "Screen off.");
ARGUI_STR(hlpOtherLog, "Frame rate and log.");
ARGUI_STR(hlpFpsOn, "Shows the frame rate.");
ARGUI_STR(hlpSmoothSmart, "Sharp art, smooth text.");
ARGUI_STR(hlpSmoothFull, "Everything smoothed.");
ARGUI_STR(hlpSmoothOff, "No smoothing.");
ARGUI_STR(hlpDrawNative, "Faster.");
ARGUI_STR(hlpDrawFull, "Exact, slower.");
ARGUI_STR(hlpSkipAuto, "Skips drawing a late frame.");
ARGUI_STR(hlpSkipFixed, "Lower frame rate.");
ARGUI_STR(hlpExactOn, "Truer colours, slower. Applies on restart.");
ARGUI_STR(hlpControls, "A to change buttons.");
ARGUI_STR(hlpPerf, "Measures speed. Saves aren't touched.");
ARGUI_STR(hlpQuit, "Unsaved progress is lost.");

static const uint8_t arguiDefKeys[10] ARGUI_RO = { 3, 3, 0, 0, 0, 0, 4, 11, 11, 0 };

typedef struct {
    const char* label;
    uint8_t dis;
    uint8_t base;
    uint8_t off;
    uint8_t step;
} ArguiRow;

static const ArguiRow arguiRows[ITEM_COUNT] ARGUI_RO = {
    { lblGame,      0,  0,  31, 8 },
    { lblTop,       9,  2,  12, 20 },
    { lblStereo,    28, 4,  13, 32 },
    { lblDialogue,  8,  6,  17, 44 },
    { lblOther,     0,  8,  34, 56 },
    { lblFps,       0,  11, 32, 68 },
    { lblFpsSize,   0,  14, 33, 80 },
    { lblSmoothing, 0,  17, 14, 96 },
    { lblDrawing,   4,  20, 15, 108 },
    { lblFrameSkip, 0,  22, 52, 120 },
    { lblExact,     2,  27, 80, 132 },
    { lblControls,  0,  29, 35, 148 },
    { lblPerf,      0,  32, 1,  160 },
    { lblLegal,     0,  33, 1,  172 },
    { lblResume,    0,  34, 1,  184 },
    { lblQuit,      0,  35, 1,  196 },
};

static const char* const arguiVals[41] ARGUI_RO = {
    valTop, valBottom,
    valStandard, valWide,
    valNormal, valHd,
    valTop, valBottom,
    valOff, valStatsLog, valBlank,
    valOff, valTop, valBottom,
    valSmall, valMedium, valLarge,
    valOff, valFull, valSmart,
    valNative, valFull,
    valOff, valAuto, valOff, val20Fps, val15Fps,
    valOff, valOn,
    valDefault, valBConfirms, valCustom,
    strEmpty, strEmpty, strEmpty, strEmpty,
    valStdPlain, disNew3ds, disNotWide, disNeedsTop, disNo3d
};

static const char* const arguiHelps[41] ARGUI_RO = {
    hlpGameTop, hlpGameBot,
    hlpTopStd, hlpTopWide,
    hlp3dNormal, hlp3dHd,
    hlpDlgTop, hlpDlgBot,
    hlpOtherOff, hlpOtherLog, hlpOtherOff,
    strEmpty, hlpFpsOn, hlpFpsOn,
    strEmpty, strEmpty, strEmpty,
    hlpSmoothOff, hlpSmoothFull, hlpSmoothSmart,
    hlpDrawNative, hlpDrawFull,
    strEmpty, hlpSkipAuto, strEmpty, hlpSkipFixed, hlpSkipFixed,
    strEmpty, hlpExactOn,
    hlpControls, hlpControls, hlpControls,
    hlpPerf, strEmpty, strEmpty, hlpQuit,
    hlpTopStd, disNew3ds, disNotWide, disNeedsTop, disNo3d
};
// (the #if above still holds: arguiDrawOptions)
ARGUI_NOINLINE void arguiDrawOptions(CtrMenu* menu) {
    CtrLog_beginConsole();
    uint16_t* p = (uint16_t*) menu->console->frameBuffer;
    g.fb = p;
    g.rgb565 = true;
    g.sx = g.sy = 1;
    g.adv = 7;
    for (uint32_t x = 0, y = 0; 320u > x;) {
#ifdef ENABLE_WAD16
        *p++ = (x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? 0xFFFFu : 0;
#else
        *p++ = (x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? 0x9D18u : 0;
#endif
        if (++y == 240u) {
            y = 0;
            x++;
        }
    }

    const CtrDisplaySettings* s = &menu->settings;
    uint8_t* m = (uint8_t*) menu;
    uint8_t below = CtrScreens_game;
    const uint8_t* k = s->controls.keys;
    uint32_t disMask = (!menu->caps.wide) | (!CTR_PACK_EXACT_AVAILABLE ? 2u : 0u) |
                       ((uint32_t) (s->topMode > below) << 2) | ((uint32_t) below << 3) | ((uint32_t) !menu->caps.stereo << 4);
    m[31] = below;
    *(uint16_t*) (m + 32) = *(uint16_t*) g.cfg;
    m[34] = s->bottomMode == BOTTOM_MODE_CONSOLE ? 1u : (CtrScreens_sharedBacklight ? 2u : 0u);
    m[35] = ((k[0] * k[1] == 2u) & (memcmp(k + 2, arguiDefKeys, 10) == 0)) ? (uint8_t) (k[0] - 1u) : 2u;

    const ArguiRow* selRow = &arguiRows[menu->cursor];
    for (const ArguiRow* r = arguiRows; r < arguiRows + ITEM_COUNT; r++) {
        int32_t ry = r->step;
        uint32_t d = r->dis & disMask;
        uint32_t idx = d ? 67u - (uint32_t) __builtin_clz(d) : (uint32_t) r->base + m[r->off];
        bool sel = r == selRow;
        uint32_t col = d ? 0x8410u : (sel ? 0xFFE0u : 0xFFFFu);
        arguiDrawText(24, ry, r->label, col);
        arguiDrawText(148, ry, arguiVals[idx], col);
        if (sel) {
            arguiDrawText(11, ry, strHeart, 0xF800u);
            arguiDrawText(12, 221, arguiHelps[idx], 0xFFFFu);
        }
    }
    GSPGPU_FlushDataCache(g.fb, 320u * 240u * 2u);
    CtrLog_endConsole();
}
#endif
#undef CtrMenu_open
#undef CtrMenu_update
#undef CtrMenu_close
#if defined(ENABLE_WAD17) // UNDERTALE never runs this (argcheck, UT late-text reclaim)
ARGUI_CODE void CtrArgui_menuOpen(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    CtrMenu_open(menu, console, settings, caps);
    *(uint8_t*) &menu->edited = gHintSeen ^ 1u;
    gHintSeen = g.menuIsOpen = 1;
    CtrArgui_menuUpdate(menu, KEY_START);
}

ARGUI_CODE void CtrArgui_menuClose(CtrMenu* menu) {
    g.menuIsOpen = false;
    CtrMenu_close(menu);
}

ARGUI_CODE CtrMenuAction CtrArgui_menuUpdate(CtrMenu* menu, u32 down) {
    if (!menu->open || down == 0) return MENU_ACTION_NONE;
    u32 folded = down | ((down >> 24) & 0xF0u);
    int32_t cur = menu->cursor;
    bool onOpts = menu->page == MENU_PAGE_OPTIONS && !CtrLegal_shown();
    CtrMenuAction action = MENU_ACTION_NONE;

    if (folded & 0xFFu) arguiPlaySound((folded & 0xC0u) == 0);
    if ((folded & 0xC0u) && onOpts) {
        cur = (int32_t) ((uint32_t) (cur + ((folded & KEY_DDOWN) ? 1 : -1)) & 15u);
        down &= ~0xC00000C0u;
    }

    int8_t sCur = arguiToScreens[cur];
    if (onOpts && sCur < 0) {
        uint32_t step = ((folded & (KEY_DRIGHT | KEY_A)) ? 1u : 0u) | ((folded & KEY_DLEFT) ? 2u : 0u);
        if (step != 0) {
            uint32_t v = g.cfg[cur - ITEM_FPS_COUNTER] + step;
            if (v >= 3u) v -= 3u;
            g.cfg[cur - ITEM_FPS_COUNTER] = (uint8_t) v;
            menu->edited = true;
            action = MENU_ACTION_CHANGED;
        }
        down &= (KEY_SELECT | KEY_B);
    }
    menu->cursor = sCur;
    action |= CtrMenu_update(menu, down);
    if (menu->settings.stereoMode == STEREO_DEPTH) {
        menu->settings.stereoMode = (folded & KEY_DLEFT) ? STEREO_HD : STEREO_NORMAL;
    }
    menu->cursor = cur;
    if (menu->page == MENU_PAGE_OPTIONS && !CtrLegal_shown()) arguiDrawOptions(menu);
    return action;
}
#endif
// ===[ In-game overlays: first-boot hint ("SELECT: Options") and FPS counter ]===

ARGUI_STR(strHint, "SELECT: Options"); void CtrHint_draw(int32_t x, int32_t y, const char* s, uint32_t col); // hintpos: ctr_hintpos.c

// Both screens keep the live counter while Options is open. The menu restores its own pixels under the digits
// before this transparent paint; the game screen gets a fresh transfer as usual. Measurement is sampled once per
// main-thread iteration by the consoles' flush wrapper below, including paused iterations.
ARGUI_NOINLINE void arguiPaintScreen(void* fb, bool rgb565) {
    bool open = g.menuIsOpen;
    if (gFpsScreen < gHintSeen || g.texCache == nullptr || CtrBench_running()) return;
    // Retired fpsFrames/fpsVb fields retain their layout; the pacer supplies the reading.
    uint32_t game = CtrScreens_game;
    gfxScreen_t screen = (gfxScreen_t) (game ^ rgb565);
    bool other565 = gfxGetScreenFormat((gfxScreen_t) (game ^ 1u)) == GSP_RGB565_OES;
    if (rgb565 && !other565) return;
    bool otherFree = CtrScreens_otherOff && other565;
    uint32_t draw = (uint32_t) (!gHintSeen && rgb565 == otherFree && (rgb565 || g.hintFrames < 180u)) |
                    ((uint32_t) (gFpsScreen == (uint32_t) screen + 1u) << 1);
    if (!draw) return;
    if (fb == nullptr && (fb = CtrPacer_backFramebuffer(screen, GFX_LEFT)) == nullptr) return;

    uint32_t xMul = (!rgb565 && gfxIsWide()) ? 2u : 1u;
    g.rgb565 = rgb565;
    g.adv = 5;
    // The main-thread flush below samples the pacer, including paused frames.
#line 539
    if (!rgb565) GSPGPU_InvalidateDataCache(fb, 320u * 240u * 2u);
    else if (otherFree && !open) memset(fb, 0, 320u * 240u * 2u);
    g.fb = fb;
    g.menuIsOpen = false; // the counter's own font and cells, not the menu's
    if (draw & 1u) {
        g.sx = (uint8_t) xMul;
        g.sy = 1;
        uint32_t col = rgb565 ? 0x9CF3u : (g.hintFrames++ < 150u ? 0xA8u : (181u - g.hintFrames) * 5u);
        CtrHint_draw(2, 228, strHint, col);
    }
    if (draw & 2u) {
        uint32_t s = (uint32_t) gFpsSize + 1u;
        g.sx = (uint8_t) (s * xMul);
        g.sy = (uint8_t) s;
        __wrap_arguiDrawText(2, 2, g.fpsBuf, 0xFFFFu);
    }
    g.menuIsOpen = open;
    GSPGPU_FlushDataCache(fb, 320u * 240u * 2u);
}

void CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo);
void CtrArgui_pacerSwap(gfxScreen_t screen, bool hasStereo);

ARGUI_CODE void CtrArgui_pacerSwap(gfxScreen_t screen, bool hasStereo) {
    arguiPaintScreen(nullptr, false);
    CtrScreens_pacerSwap(screen, hasStereo);
}

void __real_C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags);
void __wrap_C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags);

ARGUI_CODE void __wrap_C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags) {
    __real_C3D_SyncDisplayTransfer(inadr, indim, outadr, outdim, flags);
    if (((flags >> 12) & 7u) == 1u) arguiPaintScreen(outadr, false);
}

void __real_CtrScreens_flushConsoles(void);
void __wrap_CtrScreens_flushConsoles(void); void fpsboxFlushConsoles(void);

#ifdef CTRPORT_PLAYER_SHOTS
extern bool CtrBootlog_fpsLegacy; // main.c's tail: fps_overlap_legacy = 1, the counter as before (the proof's before)
#endif

// The counter stays off the stats and log consoles' screen while they show (otherwise the stats' top line flickers
// and clears whenever the log is turned on or off): it painted its cells over the stats'
// first line every frame, and the stats print that line again once a second. The stats' fourth line has the frame
// rate. With Other screen Off (or the original 2DS's Blank) the counter shows there as before.
ARGUI_CODE void __wrap_CtrScreens_flushConsoles(void) {
    fpsboxFlushConsoles();
    uint8_t fps = gFpsScreen;
#ifdef CTRPORT_PLAYER_SHOTS
    if (!CtrBootlog_fpsLegacy && CtrLog_consoleEnabled()) gFpsScreen = 0;
#else
    if (CtrLog_consoleEnabled()) gFpsScreen = 0;
#endif
    arguiPaintScreen(nullptr, true);
    gFpsScreen = fps;
}

// The first-boot hint's and the FPS counter's screen stays lit. Since menupause,
// __wrap_GSPLCD_PowerOffBacklight (ctr_screens.c) asks this first and powers the screen on instead.
bool CtrArgui_keepLit(u32 screen);

ARGUI_CODE bool CtrArgui_keepLit(u32 screen) {
    return (!gHintSeen) | (gFpsScreen == screen);
}
#endif // CTR_SCREENS_SETTINGS_ONLY

#if defined(__3DS__) && !defined(CTR_SCREENS_SETTINGS_ONLY)
// Export aliases without adding C functions (which reorders argui's old code).
__asm__(".global CtrFps_drawText\n.set CtrFps_drawText,arguiDrawText\n"
        ".global CtrFps_counterSize\n.type CtrFps_counterSize,%object\n"
        ".set CtrFps_counterSize,CtrArgui_state+1\n.size CtrFps_counterSize,1\n");
#endif

#if defined(__3DS__) && !defined(CTR_SCREENS_SETTINGS_ONLY)
// hintpos (ctr_hintpos.c draws the first-boot hint for arguiPaintScreen): argui's rgb565 flag, the free other screen's
// pass, as an exact alias like CtrFps_counterSize, so this file's functions keep their order and bytes.
_Static_assert(__builtin_offsetof(ArguiState, rgb565) == 7, "CtrHint_rgb565 is CtrArgui_state.rgb565");
__asm__(".global CtrHint_rgb565\n.type CtrHint_rgb565,%object\n"
        ".set CtrHint_rgb565,CtrArgui_state+7\n.size CtrHint_rgb565,1\n");
#endif
