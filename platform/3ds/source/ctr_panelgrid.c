// cmdlabel: DELTARUNE's battle panel slots on the pixel grid at native drawing ("panelgrid", 0 by default: a
// visual approximation, on only once it is approved by eye). Late code only (platform/3ds/late_reserve.ld):
// nothing here moves existing code or data.
//
// scr_charbox draws each hero's slot of the battle panel from its origin xx + xchunk, 1:1 in the 640x480 view. xchunk
// is 0, 213 or 426 (three heroes), 108 or 322 (two) or 213 (one) in Chapters 2-5, and 0, 212 or 424, 106 or 326, or
// 212 in Chapter 1 (gmlaot output of every chapter's scr_charbox). At native each output pixel shows a 2x2 block of
// surface pixels, so a slot at an odd origin (213) shows every sprite in it at the other phase from the same sprite in
// the slots beside it: Susie's "HP", digits and "/" differ from Kris's and Ralsei's. And Chapter 1 draws
// its command buttons at xchunk + 15 + 35 i where Chapters 2-5 draw them at xchunk + 20 + 35 i, so each button shows at
// the other phase in Chapter 1.
//
// With panelgrid 1 or 2, at native: every draw scr_charbox makes (draw_sprite, draw_sprite_ext, draw_text,
// draw_rectangle, draw_line_width (the slot's side lines under its box), and those of a script it calls directly, as Chapter 4's ossafe_fill_rectangle) moves one surface
// pixel left while its xchunk is odd: the whole slot as one piece, 213 to Chapter 1's 212. With panelgrid 3 (C') a
// slot whose xchunk is even moves one pixel right instead (0 and 426 to 1 and 427, Chapter 1's to odd origins), so
// every name, "HP" and number lands at the phase Susie's middle slot draws them at, the crisp one for the names. The command buttons (31x32
// frames, drawn at y = yy + 485 - bp, an odd row while the panel is up) have their boxes drawn one pixel lower while
// y is odd, so each box shows whole (rows 167-179 at native, one black row under the panel's line as on PC, one above
// the label), and the label (ctr_cmdlabel.c's 2b, drawn through the table) stays where it is (rows 181-185). That
// needs cmdlabel's native redraw (its box part is the scale-1 part draw a moved button makes; cmdlabel 0 draws the
// sprite whole and the box isn't moved). Across, the buttons then go one of two ways (a design fork,
// picked by eye):
//   1  as a group (B): Chapter 1's buttons (a local x of 15 mod 35) one pixel left, to Chapters 2-5's phase. Nothing moves against anything else in its slot or row, so the gaps
//      that moving each item by its own phase broke (ctr_renderer.c, Pixel grid: the HP slash against the digits, the
//      buttons' gaps, the TP label) stay as the game draws them; ACT and SPARE stay on odd columns (35 apart).
//   2  each button on its own (C): each button to the even column at or before where it lands, so every button
//      shows at the same phase, and the gaps between them alternate; 3 (C') the same.
// Only the picture moves: xchunk, xx, yy and every other game value are read, never written.
//
// Exact, no new engine hook: a draw is scr_charbox's when the VM's current code is scr_charbox, or the code that called
// the current one is (the call frame keeps the caller's code index); xchunk and xx are the drawing instance's own
// variables, found by name in the VM's variable map at the first such draw of each load.
//
// The hook: ctr_cmdlabel.c's wrap of CtrRenderer_loadFlickerProbe hands on to Cmdlabel_chainPanelgrid instead of the
// real function (its only change: the call's target). That runs after cmdlabel has put its hooks in the current table,
// so these go in front of them: init, drawSprite, drawSpritePart, drawRectangle, drawLine and drawText. main.c creates a new
// renderer for each load and ctr_pixfix_nslice.c gives it a fresh copy of the outer table, so the chain is built anew
// each time. A moved draw holds the move for the draws it makes through the table (cmdlabel's button and HP label
// redraws), so nothing moves twice. With panelgrid 0 nothing is hooked.
// UNDERTALE builds compile this file empty.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "data_win.h"
#include "instance.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"

#ifdef ENABLE_WAD17

#define PANELGRID_TEXT __attribute__((noinline, section(".text.late.panelgrid")))
#define PANELGRID_CONST __attribute__((aligned(4), section(".rodata.panelgrid_in_text")))
#define PANELGRID_BUTTON_W 31 // the command buttons' frames (ctr_cmdlabel.c)
#define PANELGRID_BUTTON_H 32

// ctr_renderer.c's end: where the bound surface's shift is (see ctr_cmdlabel.c).
extern const uint32_t Cmdlabel_layout[4]; // boundSurface, surfaces, sizeof(CtrSurface), CtrSurface.shift

static const char gridPath[] PANELGRID_CONST = CTRPORT_GAME_DIR "/settings.ini";
static const char gridRead[4] PANELGRID_CONST = "r";
static const char gridKey[] PANELGRID_CONST = " panelgrid = %d";
static const char gridCodeName[] PANELGRID_CONST = "gml_Script_scr_charbox";
static const char gridChunkName[] PANELGRID_CONST = "xchunk";
static const char gridXxName[] PANELGRID_CONST = "xx";
static const char gridYyName[] PANELGRID_CONST = "yy";
static const char gridLog[] PANELGRID_CONST = "Native: battle panel grid %d (scr_charbox code %d)\n";
static const char gridLogVars[] PANELGRID_CONST = "Native: battle panel grid: xchunk var %d, xx var %d, yy var %d\n";

typedef void (*GridDrawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale,
                               float angleDeg, uint32_t color, float alpha);
typedef void (*GridDrawSpritePart)(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y,
                                   float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha);
typedef void (*GridDrawRectangle)(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline);
typedef void (*GridDrawLine)(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha);
typedef void (*GridDrawText)(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                             float lineSeparation);

__attribute__((section(".bss.late.panelgrid"))) static struct {
    int32_t on;    // the panelgrid switch
    int32_t code;  // scr_charbox's code index, -1 if none
    int32_t chunk; // xchunk's, xx's and yy's variable ids: -2 until looked up, -1 if not found
    int32_t xx;
    int32_t yy;
    int32_t held; // inside a moved draw: the draws it makes through the table move with it
    int32_t boxDy; // inside a moved button: how far its box (the scale-1 part draws) goes down
    int32_t edge;  // the last moved draw starts at its slot's left edge (x = xx + xchunk)
    void (*innerInit)(Renderer* renderer, DataWin* dataWin);
    GridDrawSprite sprite;
    GridDrawSpritePart part;
    GridDrawRectangle rect;
    GridDrawText text;
    GridDrawLine line;
} grid;

// Whether the bound surface is drawn at native (halved): ctr_cmdlabel.c's cmdlabelNative.
static PANELGRID_TEXT bool gridNative(Renderer* renderer) {
    int32_t bound = *(const int32_t*) ((const char*) renderer + Cmdlabel_layout[0]);
    if (0 > bound) return false;
    const char* surfaces = *(const char* const*) ((const char*) renderer + Cmdlabel_layout[1]);
    return *(const int32_t*) (surfaces + (size_t) bound * Cmdlabel_layout[2] + Cmdlabel_layout[3]) > 0;
}

// A self variable's value as a whole number (rounded), into *out; false if the instance hasn't it or it isn't a number.
// IntRValueHashMap_findSlot's lookup, written out so no header's inline function is emitted outside the late code.
static PANELGRID_TEXT bool gridVar(Instance* inst, int32_t varId, int32_t* out) {
    const IntRValueHashMap* map = &inst->selfVars;
    if (0 > varId || map->capacity == 0) return false;
    uint32_t idx = ((uint32_t) varId * 0x9E3779B9u) & map->mask;
    while (map->entries[idx].key != varId) {
        if (map->entries[idx].key == INT_RVALUE_HASHMAP_EMPTY_KEY) return false;
        idx = (idx + 1) & map->mask;
    }
    const RValue* v = &map->entries[idx].value;
    if (v->type == RVALUE_INT32) {
        *out = v->int32;
        return true;
    }
    if (v->type != RVALUE_REAL) return false;
    *out = (int32_t) (v->real + (GMLReal) 16384.5) - 16384;
    return true;
}

// A name's variable id, -1 if the VM has none (none is made).
static PANELGRID_TEXT int32_t gridVarId(VMContext* ctx, const char* name) {
    for (ptrdiff_t j = 0; shlen(ctx->varNameMap) > j; j++) {
        if (strcmp(ctx->varNameMap[j].key, name) == 0) return ctx->varNameMap[j].value;
    }
    return -1;
}

// How far (surface pixels) a draw whose top-left is at x, y (view units) moves: *dx -1, 0 or 1, and for a button *dy 0
// or 1, how far its box goes down; tpagIndex -1 for anything but a sprite (see above).
static PANELGRID_TEXT void gridMove(Renderer* renderer, int32_t tpagIndex, float x, float y, int32_t* dx, int32_t* dy) {
    *dx = *dy = 0;
    grid.edge = 0;
    if (grid.held != 0 || 0 > grid.code) return;
    VMContext* ctx = renderer->runner->vmContext;
    if (ctx->currentCodeIndex != grid.code && (ctx->callStack == nullptr || ctx->callStack->savedCurrentCodeIndex != grid.code)) return;
    if (ctx->currentInstance == nullptr || !gridNative(renderer)) return;
    if (grid.chunk == -2) {
        grid.chunk = gridVarId(ctx, gridChunkName);
        grid.xx = gridVarId(ctx, gridXxName);
        grid.yy = gridVarId(ctx, gridYyName);
        logInfo(gridLogVars, (int) grid.chunk, (int) grid.xx, (int) grid.yy);
    }
    int32_t chunk, xx, yy;
    if (!gridVar(ctx->currentInstance, grid.chunk, &chunk)) return;
    int32_t slot = grid.on == 3 ? ((chunk & 1) == 0 ? 1 : 0) : ((chunk & 1) != 0 ? -1 : 0);
    *dx = slot;
    if (!gridVar(ctx->currentInstance, grid.xx, &xx)) return;
    int32_t left = (int32_t) (x + 16384.5f) - 16384 - xx; // from the view's left edge
    grid.edge = slot != 0 && left == chunk ? 1 : 0;
    if (0 > tpagIndex) return;
    const TexturePageItem* item = &renderer->dataWin->tpag.items[tpagIndex];
    if (item->boundingWidth != PANELGRID_BUTTON_W || item->boundingHeight != PANELGRID_BUTTON_H) return;
    if (grid.on >= 2) {
        *dx = (left & 1) != 0 ? -1 : 0;
    } else if ((((left - chunk) % 35) + 35) % 35 == 15) {
        *dx = slot - 1;
    }
    if (gridVar(ctx->currentInstance, grid.yy, &yy) && (((int32_t) (y + 16384.5f) - 16384 - yy) & 1) != 0) *dy = 1;
}

static PANELGRID_TEXT void gridDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                          float yscale, float angleDeg, uint32_t color, float alpha) {
    int32_t dx, dy;
    gridMove(renderer, tpagIndex, x - originX, y - originY, &dx, &dy);
    int32_t hold = (dx | dy) != 0 ? 1 : 0;
    grid.held += hold;
    grid.boxDy = dy;
    grid.sprite(renderer, tpagIndex, x + (float) dx, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
    grid.boxDy = 0;
    grid.held -= hold;
}

static PANELGRID_TEXT void gridDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x,
                                              float y, float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color,
                                              float alpha) {
    int32_t dx, dy;
    gridMove(renderer, -1, x, y, &dx, &dy);
    if (grid.held != 0 && grid.boxDy != 0 && xscale == 1.0f && yscale == 1.0f) y += (float) grid.boxDy;
    grid.held += dx != 0 ? 1 : 0;
    grid.part(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x + (float) dx, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
    grid.held -= dx != 0 ? 1 : 0;
}

static PANELGRID_TEXT void gridDrawRectangle(Renderer* renderer, float x1, float y1, float x2, float y2, uint32_t color, float alpha, bool outline) {
    int32_t dx, dy;
    gridMove(renderer, -1, x1, y1, &dx, &dy);
    // A moved slot's flat full-width rectangles (its strip of the panel's top line, its box's top edge) start where
    // the slot before it ends. Moved left they cover that slot's right line: a dim purple pixel in Kris's cyan
    // outline (cl_ch3 186, mode 2). They keep their left end and lose one surface px instead. Tall ones still move:
    // the raised box is a tall full-width rectangle under a black fill, and its left column is the box's left line.
    float left = 0 > dx && grid.edge != 0 && x2 - x1 >= 200.0f && y2 - y1 <= 4.5f ? x1 : x1 + (float) dx;
    grid.rect(renderer, left, y1, x2 + (float) dx, y2, color, alpha, outline);
}

static PANELGRID_TEXT void gridDrawLine(Renderer* renderer, float x1, float y1, float x2, float y2, float width, uint32_t color, float alpha) {
    int32_t dx, dy;
    gridMove(renderer, -1, x1, y1, &dx, &dy);
    grid.line(renderer, x1 + (float) dx, y1, x2 + (float) dx, y2, width, color, alpha);
}

static PANELGRID_TEXT void gridDrawText(Renderer* renderer, const char* text, float x, float y, float xscale, float yscale, float angleDeg,
                                        float lineSeparation) {
    int32_t dx, dy;
    gridMove(renderer, -1, x, y, &dx, &dy);
    grid.held += dx != 0 ? 1 : 0;
    grid.text(renderer, text, x + (float) dx, y, xscale, yscale, angleDeg, lineSeparation);
    grid.held -= dx != 0 ? 1 : 0;
}

// The table's init (cmdlabel's, and the renderer's under it), then scr_charbox's code index.
static PANELGRID_TEXT void gridInit(Renderer* renderer, DataWin* dataWin) {
    grid.innerInit(renderer, dataWin);
    grid.code = -1;
    grid.chunk = grid.xx = grid.yy = -2;
    grid.held = grid.boxDy = 0;
    for (uint32_t i = 0; dataWin->code.count > i; i++) {
        if (dataWin->code.entries[i].name != nullptr && strcmp(dataWin->code.entries[i].name, gridCodeName) == 0) {
            grid.code = (int32_t) i;
            break;
        }
    }
    logInfo(gridLog, (int) grid.on, (int) grid.code);
}

bool __real_CtrRenderer_loadFlickerProbe(Renderer* renderer, const char* path);
bool Cmdlabel_chainPanelgrid(Renderer* renderer, const char* path);

// ctr_cmdlabel.c's wrap of CtrRenderer_loadFlickerProbe calls this after putting its own hooks in (see above).
PANELGRID_TEXT bool Cmdlabel_chainPanelgrid(Renderer* renderer, const char* path) {
    int on = 0;
    FILE* f = fopen(gridPath, gridRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) sscanf(line, gridKey, &on);
        fclose(f);
    }
    grid.on = on;
    RendererVtable* vt = renderer->vtable;
    if (on > 0 && vt->drawSprite != gridDrawSprite) {
        grid.innerInit = vt->init;
        vt->init = gridInit;
        grid.sprite = vt->drawSprite;
        vt->drawSprite = gridDrawSprite;
        grid.part = vt->drawSpritePart;
        vt->drawSpritePart = gridDrawSpritePart;
        grid.rect = vt->drawRectangle;
        vt->drawRectangle = gridDrawRectangle;
        grid.text = vt->drawText;
        vt->drawText = gridDrawText;
        grid.line = vt->drawLine;
        vt->drawLine = gridDrawLine;
    }
    return __real_CtrRenderer_loadFlickerProbe(renderer, path);
}

#endif // ENABLE_WAD17
