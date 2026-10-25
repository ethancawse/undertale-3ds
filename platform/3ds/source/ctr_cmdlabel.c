// cmdlabel: DELTARUNE's battle command labels (FIGHT, ACT, MAGIC, ITEM, SPARE, DEFEND and the chapter variants)
// and HP labels at native drawing, in every smoothing mode. Late code only (platform/3ds/late_reserve.ld): nothing here
// moves existing code or data. Option 2b, the label at native size in extended space (mode 3,
// the default); settings.ini's "cmdlabel = N" overrides it. The HP label is redrawn 5 output rows tall ("hplabel").
//
// The label is not text: the game's battle-box script draws each button with draw_sprite (its button sprites,
// 31x32, origin 0, 0) at x = xx + xchunk + 15 + 35 * i, y = yy + 485 - bp, 1:1 in the 640x480 battle view, and the
// selected frames (1, and 2 for the pink "can spare" or "can cast" frames) carry the word in sprite rows 27-31: a 5-row
// pixel font with one-texel strokes, under the box (rows 0-25). At native drawing each output pixel shows one 2x2
// block of surface pixels, so the word is 2.5 output pixels tall: mush, whatever the smoothing.
//
// The switch (read at each load, so a chapter change or a restart rereads it):
//   0  off: the buttons are drawn as before (the mush);
//   1  removed: at native the label rows are left out (option 3; logged, as every mode but the default is);
//   2  the label at native size in today's space (option 2a): the label rows drawn at scale 2 (each texel one output
//      pixel, the game's own glyphs and colour), centred under the box, top at sprite row CMDLABEL_TOP_2A, even if it
//      runs into what is below;
//   3  the default (option 2b): 2a with the space extended: the label at CMDLABEL_TOP_2B, two
//      output pixels lower than 2a, so one black row separates it from the box's bottom edge (which Smart smoothing
//      spreads over two output rows); the box is not moved (one pixel up would touch the panel's bottom line);
//      CMDLABEL_BOX_2B moves it, 0 by default.
// cmdlabel_top, cmdlabel_box and cmdlabel_dx (surface pixels) override modes 2 and 3's placement, for trying layouts.
// Full drawing, other scales and rotations, and draws into a full-size surface take the renderer's own path as before.
//
// The hook: CtrRenderer_loadFlickerProbe runs once per load right after CtrRenderer_setSurfaceReaders (where
// ctr_pixfix_nslice.c puts its outer vtable in front of the renderer's) and before CtrRenderer_getDisplayCaps (where
// ctr_pixfix_hpname.c hooks init), and nothing else wraps it. With the switch on, the current table's init and
// drawSprite point here; init finds the button sprites' frames (by name prefix and 31x32 size) and marks their TPAG
// items in a bitmap. drawSprite then costs one range check and one bit test per draw, and a marked draw at native is
// redrawn as two drawSpritePart calls through the current table (which setSkipWrappers keeps current). Unmarked draws
// go to the renderer's own drawSprite in ctrVtable, read live, so frame skip's swap still applies to them.
//
// The HP label beside each HP bar (its own sprite) goes through the same hook. ctr_pixfix_hpname.c narrows its items to
// the 14x8 core (white strokes on black, columns in 2x pairs, rows one texel each: H's legs in rows 0-7 and its bar in
// rows 3-4; P's top in rows 0-1, its bowl's sides in row 2, its bottom in rows 3-4 and its stem in rows 5-7). At
// native each output row shows one surface row of two, so the word is 4 output pixels tall (rows 0, 2, 4, 6) and,
// whatever the smoothing, too small to read. The switch "hplabel" (1, the default) redraws it at native
// 5 output rows tall instead, the 3x5 shape of the HUD's digits beside it: sprite rows 0, 2, 3, 5 and 7, each drawn 2
// surface rows tall (hplabelRows), so H reads #.#/#.#/###/#.#/#.# and P ###/#.#/###/#../#.., in the game's own
// texels. It grows by one output row downwards, onto the panel's black: its bottom then lines up with the HP bar's.
// Each row goes onto the pixel grid as the whole core does (column 1 on an even pixel of the grid), so the odd columns
// show, as before. 0 leaves it as before. Smart smoothing keeps the label sharp (smartKeepSharp).
//
// The same hook asks ctr_renderer.c's Cmdlabel_smartMix (see there) for a Smart verdict on the first draws of each
// battle panel hero head frame (sprites named with the head prefix: the heads and their command emblems)
// whose item is small (8 to 96 texels a side) and crosses a tile line (a multiple of CTR_TILE_MAX), through drawSprite
// and drawSpritePart, until it has one: an item over tiles of two formats was misjudged, which left DELTARUNE's Chapter
// 4 Susie emblem sharp beside Kris's averaged one. Only the heads: so that the icon change does not
// make some other part softer, this keeps every other item as it was, among them
// Chapter 2's selected (yellow) command buttons, which the same misjudgement leaves sharp; Crisp (part 3) is their
// long-term look. The switch "smartmix" (1, the default; 0 leaves the heads to the renderer as before). The items are
// in a second bitmap, and each bit is cleared once the item is decided, so a draw costs one more range check and bit
// test.
// UNDERTALE builds compile this file empty.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ctr_renderer.h"
#include "ctr_texture_pack.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"

#ifdef ENABLE_WAD17

#define CMDLABEL_TEXT __attribute__((noinline, section(".text.late.cmdlabel")))
#define CMDLABEL_CONST __attribute__((aligned(4), section(".rodata.cmdlabel_in_text")))

// Sprite rows: the box is 0-25, the label 27-31 (every chapter's buttons, Chapters 1-5).
#define CMDLABEL_BOX_ROWS 26
#define CMDLABEL_LABEL_ROW 27
#define CMDLABEL_LABEL_ROWS 5
#define CMDLABEL_W 31
#define CMDLABEL_H 32
// Placement defaults, in surface pixels from the sprite's top-left.
#define CMDLABEL_TOP_2A 26
#define CMDLABEL_TOP_2B 30
#define CMDLABEL_BOX_2B 0
// Option 2b (mode 3: the label at native size, the space extended) ships.
#define CMDLABEL_DEFAULT 3
#define HPLABEL_DEFAULT 1
#define HPLABEL_MAX 4 // HP label sprite frames looked for
#define SMARTMIX_DEFAULT 1
#define SMARTMIX_MIN 8  // ctr_renderer.c's SMART_ITEM_MIN and SMART_ITEM_MAX
#define SMARTMIX_MAX 96

// ctr_renderer.c's end: the renderer's own vtable, and where the bound surface's shift is.
extern const void* const Cmdlabel_refs[1];
extern const uint32_t Cmdlabel_layout[4]; // boundSurface, surfaces, sizeof(CtrSurface), CtrSurface.shift
bool Cmdlabel_smartMix(Renderer* renderer, int32_t tpagIndex);

static const char cmdlabelPrefix[8] CMDLABEL_CONST = "spr_bt";
static const char smartmixPrefix[12] CMDLABEL_CONST = "spr_head";
static const char cmdlabelPath[] CMDLABEL_CONST = CTRPORT_GAME_DIR "/settings.ini";
static const char cmdlabelRead[4] CMDLABEL_CONST = "r";
static const char cmdlabelKey[] CMDLABEL_CONST = " cmdlabel = %d";
static const char cmdlabelKeyTop[] CMDLABEL_CONST = " cmdlabel_top = %d";
static const char cmdlabelKeyBox[] CMDLABEL_CONST = " cmdlabel_box = %d";
static const char cmdlabelKeyDx[] CMDLABEL_CONST = " cmdlabel_dx = %d";
static const char cmdlabelLogMode[] CMDLABEL_CONST = "Native: command labels mode %d (top %d, box %d, dx %d)\n";
static const char cmdlabelLogFound[] CMDLABEL_CONST = "Native: command labels: %u frames of %u button sprites\n";
static const char hplabelName[12] CMDLABEL_CONST = "spr_hpname";
static const char hplabelKey[] CMDLABEL_CONST = " hplabel = %d";
static const char hplabelLog[] CMDLABEL_CONST = "Native: HP label mode %d\n";
// The sprite rows drawn as the 5 output rows (see above).
static const uint8_t hplabelRows[5] CMDLABEL_CONST = {0, 2, 3, 5, 7};
static const char smartmixKey[] CMDLABEL_CONST = " smartmix = %d";
static const char smartmixLog[] CMDLABEL_CONST = "Smart smoothing: items over tiles of two formats %d\n";

typedef void (*CmdlabelDrawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                   float yscale, float angleDeg, uint32_t color, float alpha);
typedef void (*CmdlabelDrawSpritePart)(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x,
                                       float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color,
                                       float alpha);

__attribute__((section(".bss.late.cmdlabel"))) static struct {
    int32_t mode, top, box, dx;
    void (*innerInit)(Renderer* renderer, DataWin* dataWin);
    CmdlabelDrawSprite saved; // the table's drawSprite when hooked, if it wasn't ctrVtable's own
    int32_t lo;
    uint32_t span; // the highest marked index minus lo; bits == nullptr: nothing marked
    uint8_t* bits;
    int32_t hp;                   // the hplabel switch
    uint32_t hpCount;             // HP label sprite items found
    int32_t hpItems[HPLABEL_MAX];
} cmdlabel;
// smartmix's state, in its own section after the later statics so that cmdlabel's keeps its size.
__attribute__((section(".bss.late.cmdlabel.mix"))) static struct {
    int32_t mix;                      // the smartmix switch
    CmdlabelDrawSpritePart savedPart; // the table's drawSpritePart when hooked, if it wasn't ctrVtable's own
    int32_t mixLo;
    uint32_t mixSpan; // the highest marked index minus mixLo; mixBits == nullptr: nothing marked
    uint8_t* mixBits;
} cmdmix;

// An item whose box crosses a tile line, until Smart smoothing has a verdict on it (see above).
static inline __attribute__((always_inline)) void cmdlabelMix(Renderer* renderer, int32_t tpagIndex) {
    uint32_t i = (uint32_t) (tpagIndex - cmdmix.mixLo);
    if (i <= cmdmix.mixSpan && cmdmix.mixBits != nullptr && (cmdmix.mixBits[i >> 3] >> (i & 7) & 1) != 0 &&
        Cmdlabel_smartMix(renderer, tpagIndex)) {
        cmdmix.mixBits[i >> 3] &= (uint8_t) ~(1u << (i & 7));
    }
}

static CMDLABEL_TEXT bool cmdlabelNative(Renderer* renderer) {
    int32_t bound = *(const int32_t*) ((const char*) renderer + Cmdlabel_layout[0]);
    if (0 > bound) return false;
    const char* surfaces = *(const char* const*) ((const char*) renderer + Cmdlabel_layout[1]);
    return *(const int32_t*) (surfaces + (size_t) bound * Cmdlabel_layout[2] + Cmdlabel_layout[3]) > 0;
}

// A marked frame at native: the box (sprite rows 0-25) 1:1, moved by box, then the label rows at scale 2 (mode 2, 3).
static CMDLABEL_TEXT void cmdlabelDraw(Renderer* renderer, const TexturePageItem* item, int32_t tpagIndex, float x, float y, uint32_t color,
                                       float alpha) {
    RendererVtable* vt = renderer->vtable;
    int32_t tx = item->targetX, ty = item->targetY, sw = item->sourceWidth, sh = item->sourceHeight;
    int32_t box = cmdlabel.mode >= 2 ? cmdlabel.box : 0;
    if (CMDLABEL_BOX_ROWS > ty) {
        int32_t h = CMDLABEL_BOX_ROWS - ty < sh ? CMDLABEL_BOX_ROWS - ty : sh;
        vt->drawSpritePart(renderer, tpagIndex, 0.0f, 0.0f, (float) sw, (float) h, x + (float) tx, y + (float) (ty + box), 1.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, color, alpha);
    }
    if (cmdlabel.mode < 2) return;
    int32_t from = CMDLABEL_LABEL_ROW > ty ? CMDLABEL_LABEL_ROW - ty : 0; // item rows
    int32_t to = CMDLABEL_LABEL_ROW + CMDLABEL_LABEL_ROWS - ty < sh ? CMDLABEL_LABEL_ROW + CMDLABEL_LABEL_ROWS - ty : sh;
    if (from >= to) return;
    // Doubled about the box's centre column (15 of 0-30), top at row cmdlabel.top.
    float lx = x + (float) (2 * tx - CMDLABEL_W / 2 + cmdlabel.dx);
    float ly = y + (float) (cmdlabel.top + 2 * (ty + from - CMDLABEL_LABEL_ROW));
    vt->drawSpritePart(renderer, tpagIndex, 0.0f, (float) from, (float) sw, (float) (to - from), lx, ly, 2.0f, 2.0f, 0.0f, 0.0f, 0.0f, color,
                       alpha);
}

// An HP label item at native, laid out as ctr_pixfix_hpname.c leaves it (14x8 at target 1, 0): 5 rows (see above).
// False for any other layout, which is drawn as before.
static CMDLABEL_TEXT bool hplabelDraw(Renderer* renderer, const TexturePageItem* item, int32_t tpagIndex, float x, float y, uint32_t color,
                                      float alpha) {
    if (item->sourceWidth != 14 || item->sourceHeight != 8 || item->targetX != 1 || item->targetY != 0) return false;
    RendererVtable* vt = renderer->vtable;
    // Each row is 14x2 surface pixels for 14x1 texels, which the grid takes as art in 2x pixels: it puts every row
    // where it puts the whole core, column 1 on an even pixel of the grid.
    for (int32_t i = 0; 5 > i; i++) {
        vt->drawSpritePart(renderer, tpagIndex, 0.0f, (float) hplabelRows[i], 14.0f, 1.0f, x + 1.0f, y + (float) (2 * i), 1.0f, 2.0f, 0.0f, 0.0f,
                           0.0f, color, alpha);
    }
    return true;
}

static CMDLABEL_TEXT void cmdlabelDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                             float yscale, float angleDeg, uint32_t color, float alpha) {
    cmdlabelMix(renderer, tpagIndex);
    for (uint32_t k = 0; cmdlabel.hpCount > k; k++) {
        if (cmdlabel.hpItems[k] == tpagIndex && xscale == 1.0f && yscale == 1.0f && angleDeg == 0.0f && cmdlabelNative(renderer) &&
            hplabelDraw(renderer, &renderer->dataWin->tpag.items[tpagIndex], tpagIndex, x - originX, y - originY, color, alpha)) {
            return;
        }
    }
    uint32_t i = (uint32_t) (tpagIndex - cmdlabel.lo);
    if (i <= cmdlabel.span && cmdlabel.bits != nullptr && (cmdlabel.bits[i >> 3] >> (i & 7) & 1) != 0 && xscale == 1.0f && yscale == 1.0f &&
        angleDeg == 0.0f && cmdlabelNative(renderer)) {
        cmdlabelDraw(renderer, &renderer->dataWin->tpag.items[tpagIndex], tpagIndex, x - originX, y - originY, color, alpha);
        return;
    }
    CmdlabelDrawSprite inner = cmdlabel.saved != nullptr ? cmdlabel.saved : ((const RendererVtable*) Cmdlabel_refs[0])->drawSprite;
    inner(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

static CMDLABEL_TEXT void cmdlabelDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH,
                                                 float x, float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY,
                                                 uint32_t color, float alpha) {
    cmdlabelMix(renderer, tpagIndex);
    CmdlabelDrawSpritePart inner = cmdmix.savedPart != nullptr ? cmdmix.savedPart : ((const RendererVtable*) Cmdlabel_refs[0])->drawSpritePart;
    inner(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
}

// With smartmix on, marks the hero heads' small items whose box crosses a tile line (two passes: the range, then the
// bits).
static CMDLABEL_TEXT void cmdlabelMixInit(DataWin* dataWin) {
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int pass = 0; 2 > pass; pass++) {
        uint32_t marked = 0;
        for (uint32_t s = 0; dataWin->sprt.count > s; s++) {
            const Sprite* sp = &dataWin->sprt.sprites[s];
            if (sp->name == nullptr || strncmp(sp->name, smartmixPrefix, 8) != 0) continue;
            for (uint32_t f = 0; sp->textureCount > f; f++) {
                int32_t t = sp->tpagIndices[f];
                if (0 > t || dataWin->tpag.count <= (uint32_t) t) continue;
                const TexturePageItem* item = &dataWin->tpag.items[t];
                int32_t x0 = item->sourceX, y0 = item->sourceY, w = item->sourceWidth, h = item->sourceHeight;
                if (0 > item->texturePageId || SMARTMIX_MIN > w || SMARTMIX_MIN > h || w > SMARTMIX_MAX || h > SMARTMIX_MAX) continue;
                if (x0 / CTR_TILE_MAX == (x0 + w - 1) / CTR_TILE_MAX && y0 / CTR_TILE_MAX == (y0 + h - 1) / CTR_TILE_MAX) continue;
                if (pass == 0) {
                    if (lo > t) lo = t;
                    if (hi < t) hi = t;
                } else {
                    uint32_t i = (uint32_t) (t - lo);
                    cmdmix.mixBits[i >> 3] |= (uint8_t) (1u << (i & 7));
                    marked++;
                }
            }
        }
        if (pass == 0) {
            if (lo > hi) return;
            cmdmix.mixBits = calloc((size_t) (hi - lo) / 8 + 1, 1);
            if (cmdmix.mixBits == nullptr) return;
            cmdmix.mixLo = lo;
            cmdmix.mixSpan = (uint32_t) (hi - lo);
        } else if (cmdmix.mix != SMARTMIX_DEFAULT) {
            logInfo(smartmixLog, (int) marked);
        }
    }
}

// The renderer's init, then the HP label's frames (with hplabel on) and the button frames (with cmdlabel on): sprites
// named with the button prefix, 31x32, every frame whose item lies inside it.
static CMDLABEL_TEXT void cmdlabelInit(Renderer* renderer, DataWin* dataWin) {
    free(cmdlabel.bits);
    cmdlabel.bits = nullptr;
    cmdlabel.hpCount = 0;
    free(cmdmix.mixBits);
    cmdmix.mixBits = nullptr;
    cmdlabel.innerInit(renderer, dataWin);
    if (0 < cmdmix.mix) cmdlabelMixInit(dataWin);
    for (uint32_t s = 0; 0 < cmdlabel.hp && dataWin->sprt.count > s; s++) {
        const Sprite* sp = &dataWin->sprt.sprites[s];
        if (sp->name == nullptr || strcmp(sp->name, hplabelName) != 0) continue;
        for (uint32_t f = 0; sp->textureCount > f && HPLABEL_MAX > cmdlabel.hpCount; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 <= t && dataWin->tpag.count > (uint32_t) t) cmdlabel.hpItems[cmdlabel.hpCount++] = t;
        }
    }
    if (0 >= cmdlabel.mode) return;
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (int pass = 0; 2 > pass; pass++) {
        uint32_t frames = 0, sprites = 0;
        for (uint32_t s = 0; dataWin->sprt.count > s; s++) {
            const Sprite* sp = &dataWin->sprt.sprites[s];
            if (sp->name == nullptr || strncmp(sp->name, cmdlabelPrefix, 6) != 0 || sp->width != CMDLABEL_W || sp->height != CMDLABEL_H) continue;
            sprites++;
            for (uint32_t f = 0; sp->textureCount > f; f++) {
                int32_t t = sp->tpagIndices[f];
                if (0 > t || dataWin->tpag.count <= (uint32_t) t) continue;
                const TexturePageItem* item = &dataWin->tpag.items[t];
                if (item->sourceWidth != item->targetWidth || item->sourceHeight != item->targetHeight ||
                    item->targetX + item->sourceWidth > CMDLABEL_W || item->targetY + item->sourceHeight > CMDLABEL_H) {
                    continue;
                }
                if (pass == 0) {
                    if (lo > t) lo = t;
                    if (hi < t) hi = t;
                } else {
                    uint32_t i = (uint32_t) (t - lo);
                    cmdlabel.bits[i >> 3] |= (uint8_t) (1u << (i & 7));
                    frames++;
                }
            }
        }
        if (pass == 0) {
            if (lo > hi) return;
            cmdlabel.bits = calloc((size_t) (hi - lo) / 8 + 1, 1);
            if (cmdlabel.bits == nullptr) return;
            cmdlabel.lo = lo;
            cmdlabel.span = (uint32_t) (hi - lo);
        } else {
            if (cmdlabel.mode != CMDLABEL_DEFAULT) logInfo(cmdlabelLogFound, (unsigned) frames, (unsigned) sprites);
        }
    }
}

bool __real_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path); bool Cmdlabel_chainPanelgrid(Renderer* renderer, const char* path);
bool __wrap_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path);

CMDLABEL_TEXT bool __wrap_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path) {
    int mode = CMDLABEL_DEFAULT, top = -1000, box = -1000, dx = 0, hp = HPLABEL_DEFAULT, mix = SMARTMIX_DEFAULT;
    FILE* f = fopen(cmdlabelPath, cmdlabelRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) {
            sscanf(line, cmdlabelKey, &mode);
            sscanf(line, cmdlabelKeyTop, &top);
            sscanf(line, cmdlabelKeyBox, &box);
            sscanf(line, cmdlabelKeyDx, &dx);
            sscanf(line, hplabelKey, &hp);
            sscanf(line, smartmixKey, &mix);
        }
        fclose(f);
    }
    cmdlabel.mode = mode;
    cmdlabel.hp = hp;
    cmdmix.mix = mix;
    if (hp != HPLABEL_DEFAULT) logInfo(hplabelLog, hp);
    if (mix != SMARTMIX_DEFAULT) logInfo(smartmixLog, mix);
    if (mode > 0 || hp > 0 || mix > 0) {
        cmdlabel.top = top != -1000 ? top : mode == 3 ? CMDLABEL_TOP_2B : CMDLABEL_TOP_2A;
        cmdlabel.box = box != -1000 ? box : mode == 3 ? CMDLABEL_BOX_2B : 0;
        cmdlabel.dx = dx;
        if (mode != CMDLABEL_DEFAULT && mode > 0) logInfo(cmdlabelLogMode, mode, (int) cmdlabel.top, (int) cmdlabel.box, (int) cmdlabel.dx);
        RendererVtable* vt = renderer->vtable;
        const RendererVtable* own = (const RendererVtable*) Cmdlabel_refs[0];
        // Only in front of the renderer's own table (ctr_pixfix_nslice.c's outer one), never in it: frame skip
        // rewrites ctrVtable's drawSprite.
        if (vt != own && vt->drawSprite != cmdlabelDrawSprite) {
            cmdlabel.saved = vt->drawSprite == own->drawSprite ? nullptr : vt->drawSprite;
            cmdlabel.innerInit = vt->init;
            vt->init = cmdlabelInit;
            vt->drawSprite = cmdlabelDrawSprite;
            cmdmix.savedPart = vt->drawSpritePart == own->drawSpritePart ? nullptr : vt->drawSpritePart;
            vt->drawSpritePart = cmdlabelDrawSpritePart;
        }
    }
    return Cmdlabel_chainPanelgrid(renderer, path); // ctr_panelgrid.c: its hooks, then the real function
}

#endif // ENABLE_WAD17

// crispart reads only the mode to preserve cmdlabel's own box/label split (including the mode-0 fallback).
// An appended reference only: no existing function, constant, state field or source line number changes.
#ifdef ENABLE_WAD17
const int32_t* const Crispart_cmdlabelMode __attribute__((aligned(4), section(".rodata.late.crispart"))) = &cmdlabel.mode;
#endif
