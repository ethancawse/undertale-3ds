// UNDERTALE's battle grid at native (pixfix, task F; UNDERTALE only). Late code only (platform/3ds/late_reserve.ld):
// nothing here moves existing code or data.
//
// UNDERTALE's battle backdrop, spr_battlebg (drawn by obj_battlebg's default draw, 1:1 in the 640x480 battle view), is
// a grid of lines one texel wide in one opaque colour. Frame 0 (609x236): 7 columns and 3 rows, each the item's whole
// height or width, nothing partly transparent. Frame 1 (639x253, Toriel's and Napstablook's battles set image_index
// 1): 22 runs of 42 to 236 texels, plus 647 faint grey texels at alpha 1 to 11 (UNDERTALE 1.08's data,
// as measured). At native drawing each output pixel shows the even surface pixel of its
// 2x2 block, so only the lines on even pixels show, and another set when the damage shake moves the item by a pixel.
// Smart smoothing leaves items over 96 texels a side alone (SMART_ITEM_MAX).
//
// Frame 0: the renderer draws the item twice more, one surface pixel to the left and one up, before the game's own
// draw: a column on an odd pixel then covers the even pixel of its block, and so does a row, so every 2x2 block with
// ink shows the line's colour and blocks without ink are untouched. That is the shrink keeping ink (the PC game's
// lines, each one output pixel wide, at full colour), the same at every shake offset. Opaque texels of one colour at
// alpha 1 under normal blending draw the same pixel however often they land on it, so the copies change nothing
// else. A block whose only ink is on its odd x and odd y needs a copy one up and one left too: whole-length lines
// leave only the item's corner texel (0, 0) so (where the item's top left lands on odd pixels both ways), drawn as a
// 1x1 part. The copies draw the stored texels themselves, whatever format the texture cache gave each tile.
//
// Frame 1: whole copies would blend the faint texels twice, so each run is drawn again on its own, as a part one
// texel wide, one left, one up and one up-left, after the game's own draw. A run holds only opaque ink, so each
// copy sets its pixels to the ink and touches nothing else; every 2x2 block with ink then shows it at every offset.
// A strip copy is drawn only where the shrink can show it: its first pixel even on the run's thin axis and on each
// axis it moves along (a column's copy one up adds one pixel, above the run). Every pixel of a copy left out is odd
// or already holds the same ink, so the picture is the one all three copies give; about a third of them draw.
//
// The items' shapes are pinned to the data checked: UNDERTALE 1.08's data.win (its size, GEN8 game ID and timestamp,
// the one data.win the builder accepts: docs/data-versions.md) and each item's size. Any other data draws as before.
//
// Only draws that would lose ink go this way: the application surface shown at native (the shrink halving both axes,
// 1:1 from drawing units to surface pixels) under Smart smoothing or Off, the menu's Crisp (pixgridcrisp): the two
// that show one pixel of each 2x2 block, the renderer's grid.pick. Full smoothing averages. With both axes halved,
// grid.pick under Smart is smart.active, which this tested before, so Smart draws as it did. Draws 1:1, upright,
// white, alpha 1 (every draw UNDERTALE makes of the item: nothing writes obj_battlebg's image_alpha, image_blend, scale
// or angle), with the game's normal blending and nothing that changes texels (shader, palette, fog, filtering). The
// copies go through the renderer's drawSprite and drawSpritePart, so recordings and frame skips treat them as any draw.
//
// The hook: an outer vtable (pixgrid.outer) in front of the renderer's own table, a copy of it with init and
// drawSprite replaced. setSkipWrappers swaps the other draw entries in the outer table itself (they still hold the
// normal functions), and its drawSprite in ctrVtable, which pixgridDrawSprite calls. CtrRenderer_setSurfaceReaders is
// wrapped (CMakeLists.txt, UNDERTALE only): main.c calls it once per load right after CtrRenderer_create and before
// the runner's init, and before the display-caps query where view_snap hooks the table through renderer->vtable (so
// its hooks land in the outer table; setSkipWrappers swaps none of view_snap's entries).
//
// Why a file of its own: code added to ctr_renderer.c, even never called, shifts registers and scheduling in existing
// functions there.
// ctr_renderer.c ends with Pixgrid_refs and Pixgrid_layout instead: the addresses and private field offsets used here.
// The constants sit in the late code area (.rodata.pixgrid_in_text, after this file's code in late_reserve.ld), as
// screens' do: UNDERTALE New's late constants page is nearly full.

#include <3ds.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "common.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#if defined(CTRPORT_GOG_COMPAT)
#ifndef ENABLE_WAD17

#define PIXGRID_TEXT __attribute__((noinline, section(".text.late.pixgrid")))
#define PIXGRID_RO __attribute__((aligned(4), section(".rodata.pixgrid_in_text")))

// ctr_renderer.c's end: addresses, then offsets in CtrRenderer and constants.
enum { REF_VTABLE, REF_UPDATE_GRID_MAP, REF_COUNT };
enum {
    L_GRID_PICK, L_GRID_HOLD, L_GRID_MAP, L_BOUND_SURFACE, L_FILTER_OVERRIDE, L_BLEND_ENABLE, L_BLEND_MODE, L_SHADER_FX,
    L_PAL_KIND, L_FOG_ENABLE, L_SHADER_FX_NONE, L_PAL_NONE, L_GRID_MAP_KX, L_GRID_MAP_SIZE, L_FILTER_LINEAR,
    L_ITEM_STATE, L_COUNT
};
extern const void* const Pixgrid_refs[REF_COUNT];
extern const uint32_t Pixgrid_layout[L_COUNT];
#define PG_FIELD(r, index, type) (*(type*) ((char*) (r) + Pixgrid_layout[index]))

// The renderer's gridMap, field for field (pixgridInstall checks its layout).
typedef struct {
    int32_t surface;
    bool on, x, y;
    float kx, ox, invKx;
    float ky, oy, invKy;
} PixgridMap;

typedef void (*PixgridUpdateGridMap)(void* r);

static struct {
    RendererVtable outer;
    const RendererVtable* inner; // the renderer's own table (ctrVtable)
    int32_t slot[3];             // spr_battlebg's frames 0 and 1 at [tpag & 8 ? 2 : 0] (INT32_MIN when none); [1] is
                                 // frame 0's item (slot's place in main, then first's: the same 12 bytes)
} pixgrid __attribute__((section(".bss.late.pixgrid")));

static const char pixgridName[16] PIXGRID_RO = "spr_battlebg";
// UNDERTALE 1.08 (Steam): data.win bytes, GEN8 game ID and timestamp; spr_battlebg[0]'s and [1]'s item sizes
// (width | height << 16).
static const uint32_t pixgridData[5] PIXGRID_RO = { 62921978u, 0x338ADCD9u, 1510935285u, 609u | 236u << 16, 639u | 253u << 16 };
// Runs of opaque ink, x | y << 10 | (length - 1) << 18 | (1 for a row) << 26: spr_battlebg[0]'s corner texel, then
// spr_battlebg[1]'s 22 runs, derived privately by tools/aot/pixgrid.py from the player's data at build time.
static const uint32_t pixgridRuns[23] PIXGRID_RO = {
#include "pixgrid_runs.inc"
};
#line 105
#define PIXGRID_LOG "Smart smoothing: item %d's lines kept\n"
static const char pixgridLog[(sizeof(PIXGRID_LOG) + 3) & ~(size_t) 3] PIXGRID_RO = PIXGRID_LOG;

// The renderer state a draw needs: {Pixgrid_layout index, bytes, value (or PIXGRID_FROM | the index holding it)}.
#define PIXGRID_FROM 0x80
static const uint8_t pixgridState[7][3] PIXGRID_RO = {
    { L_GRID_PICK, 1, 1 },
    { L_GRID_HOLD, 4, 0 },
    { L_BLEND_ENABLE, 1, 1 },
    { L_BLEND_MODE, 4, bm_normal },
    { L_SHADER_FX, 1, PIXGRID_FROM | L_SHADER_FX_NONE },
    { L_PAL_KIND, 1, PIXGRID_FROM | L_PAL_NONE },
    { L_FOG_ENABLE, 1, 0 },
};

// Whether this draw of the item would lose lines (see the top). Smart smoothing's own verdict on the item comes first:
// one that averages it (fine) or places it by phase would change the copies, so such an item draws as before
// (Smart leaves items over 96 texels a side alone today; a check that changes that must not break the lines). The
// verdict comes from the item's texels (itemVerdict), not the setting, so it holds under Off as under Smart.
static PIXGRID_TEXT bool pixgridLoses(Renderer* r, int32_t tpagIndex, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    uint32_t verdict = Pixgrid_layout[L_ITEM_STATE]; // the field's offset, and above bit 24 the bits that move it
    const uint8_t* verdicts = *(const uint8_t* const*) ((const char*) r + (verdict & 0xFFFFFFu));
    if (verdicts != nullptr && (verdicts[tpagIndex] & (verdict >> 24)) != 0) return false;
    if (xscale != 1.0f || yscale != 1.0f || angleDeg != 0.0f || (color & 0xFFFFFFu) != 0xFFFFFFu || 1.0f > alpha) return false;
    for (int32_t i = 0; 7 > i; i++) {
        const uint8_t* e = pixgridState[i];
        const uint8_t* field = (const uint8_t*) r + Pixgrid_layout[e[0]];
        uint32_t value = e[1] == 4 ? *(const uint32_t*) field : *field;
        if (value != ((e[2] & PIXGRID_FROM) != 0 ? Pixgrid_layout[e[2] & ~PIXGRID_FROM] : e[2])) return false;
    }
    const PixgridMap* map = &PG_FIELD(r, L_GRID_MAP, PixgridMap);
    if (map->surface != PG_FIELD(r, L_BOUND_SURFACE, int32_t)) ((PixgridUpdateGridMap) Pixgrid_refs[REF_UPDATE_GRID_MAP])(r); // gridActive's work
    if (!map->on || !map->x || !map->y || fabsf(map->kx - 1.0f) > 0.0001f || fabsf(map->ky - 1.0f) > 0.0001f) return false;
    const BlendFactors* f = &r->blendFactors;
    int8_t filterOverride = PG_FIELD(r, L_FILTER_OVERRIDE, int8_t);
    bool linear = filterOverride != 0 ? filterOverride == (int8_t) Pixgrid_layout[L_FILTER_LINEAR] : r->texFilter;
    return f->src == bm_src_alpha && f->dst == bm_inv_src_alpha && 0 > r->currentShader && !linear;
}

// Whether a copy whose first texel lands at surface coordinate s can show: the shrink keeps even pixels. The first
// pixel is the one whose centre s covers (gridExcess's rounding); a start off whole pixels always draws.
static inline __attribute__((always_inline)) bool pixgridShows(float s) {
    int32_t first = 16384 - (int32_t) (16384.5f - s);
    return (first & 1) == 0 || fabsf(s - (float) first) > 0.25f;
}

// A draw of either frame: frame 0's copies before the game's own draw, frame 1's strips after it.
static PIXGRID_TEXT void pixgridDrawKept(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                         float yscale, float angleDeg, uint32_t color, float alpha) {
    const RendererVtable* inner = pixgrid.inner;
    int32_t frame = tpagIndex == pixgrid.slot[1] ? 0 : 1;
    bool keep = pixgridLoses(renderer, tpagIndex, xscale, yscale, angleDeg, color, alpha);
    for (int32_t k = 0; (keep && frame == 0 ? 2 : 0) > k; k++) { // frame 0: one left, one up
        inner->drawSprite(renderer, tpagIndex, x - (float) (k == 0), y - (float) (k != 0), originX, originY, xscale, yscale, angleDeg, color, alpha);
    }
    inner->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
    if (!keep) return;
    const TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];
    float left = x + (float) tpag->targetX - originX, top = y + (float) tpag->targetY - originY;
    const PixgridMap* map = &PG_FIELD(renderer, L_GRID_MAP, PixgridMap);
    for (int32_t i = frame; (frame != 0 ? 23 : 1) > i; i++) { // frame 0: its corner texel up-left; frame 1: its runs
        uint32_t run = pixgridRuns[i];
        float rx = (float) (run & 1023u), ry = (float) ((run >> 10) & 255u), length = (float) (((run >> 18) & 255u) + 1u);
        bool row = (run >> 26) != 0;
        float w = row ? length : 1.0f, h = row ? 1.0f : length;
        for (int32_t k = frame != 0 ? 0 : 2; 3 > k; k++) { // one left, one up, one up-left
            float cx = left + rx - (float) (k != 1), cy = top + ry - (float) (k != 0);
            if ((!row || k != 1) && !pixgridShows(map->kx * cx + map->ox)) continue;
            if ((row || k != 0) && !pixgridShows(map->ky * cy + map->oy)) continue;
            inner->drawSpritePart(renderer, tpagIndex, rx, ry, w, h, cx, cy, xscale, yscale, angleDeg, 0.0f, 0.0f, color, alpha);
        }
    }
}

// Every drawSprite comes here: both ways on are tail calls with the arguments as they came, so the game's other draws
// pay one look-up and compare and nothing is saved or moved (the copies' work lives in pixgridDrawKept). The two
// items sit in the slot of their index's bit 3: testing both by name made gcc save and move every argument per draw.
static PIXGRID_TEXT void pixgridDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                           float yscale, float angleDeg, uint32_t color, float alpha) {
    if (tpagIndex == pixgrid.slot[(tpagIndex & 8) >> 2]) {
        pixgridDrawKept(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        return;
    }
    pixgrid.inner->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}
// The runner's init with the data: finds spr_battlebg's frames 0 and 1, in the data checked (Windows or macOS).
static bool pixgridKnownData(const DataWin* dataWin);
static bool pixgridKnownGogData(const DataWin* dataWin);
#line 191
static PIXGRID_TEXT void pixgridInit(Renderer* renderer, DataWin* dataWin) {
    __asm__ volatile(".rept 14\n\tnop\n\t.endr\n"); pixgrid.inner->init(renderer, dataWin);
    pixgrid.slot[0] = pixgrid.slot[2] = INT32_MIN;
    if (!pixgridKnownGogData(dataWin)) return;
    for (uint32_t i = 0; dataWin->sprt.count > i; i++) {
        const Sprite* sp = &dataWin->sprt.sprites[i];
        if (sp->name == nullptr || strcmp(sp->name, pixgridName) != 0) continue;
        if (2 > sp->textureCount) return;
        for (int32_t f = 0; 2 > f; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 > t || dataWin->tpag.count <= (uint32_t) t) return;
            const TexturePageItem* item = &dataWin->tpag.items[t];
            uint32_t size = pixgridData[3 + f];
            if (item->sourceWidth != (size & 0xFFFFu) || item->sourceHeight != size >> 16 || item->targetWidth != item->sourceWidth ||
                item->targetHeight != item->sourceHeight) {
                return;
            }
        }
        int32_t t0 = sp->tpagIndices[0], t1 = sp->tpagIndices[1];
        if (((t0 ^ t1) & 8) == 0) return; // bit 3 tells them apart: 39 and 42 (Windows), 49 and 59 (macOS)
        pixgrid.slot[(t0 & 8) >> 2] = pixgrid.slot[1] = t0;
        pixgrid.slot[(t1 & 8) >> 2] = t1;
        logInfo(pixgridLog, (int) t0);
        logInfo(pixgridLog, (int) t1);
        return;
    }
}

// After each CtrRenderer_create: a renderer still on its own table gets the outer one (a later call changes nothing).
static PIXGRID_TEXT void pixgridInstall(Renderer* renderer) {
    RendererVtable* inner = (RendererVtable*) Pixgrid_refs[REF_VTABLE];
    if (renderer->vtable != inner) return;
    if (Pixgrid_layout[L_GRID_MAP_KX] != __builtin_offsetof(PixgridMap, kx) || Pixgrid_layout[L_GRID_MAP_SIZE] != sizeof(PixgridMap)) return;
    pixgrid.inner = inner;
    pixgrid.slot[0] = pixgrid.slot[2] = INT32_MIN;
    pixgrid.outer = *inner;
    pixgrid.outer.init = pixgridInit;
    pixgrid.outer.drawSprite = pixgridDrawSprite;
    renderer->vtable = &pixgrid.outer;
}

void __real_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);
void __wrap_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);

PIXGRID_TEXT void __wrap_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count) {
    __real_CtrRenderer_setSurfaceReaders(renderer, readers, count);
    pixgridInstall(renderer);
}

// ===[ nativemac: UNDERTALE's macOS data (game.ios, on the card as data.win) ]===
//
// The macOS file is a later (2019) build of the same 1.08 game: spr_battlebg's two items have the same sizes and
// texels, but its repacked atlas lists them as items 49 and 59 (both odd, so the slots go by bit 3, above). Its key:
// bytes, GEN8 game ID and timestamp. pixgridInit's nops (run once per load) keep it at main's size (440 B).
static const uint32_t pixgridMacKey[3] __attribute__((aligned(4), section(".rodata.nativemac_in_text"))) = {
    62956178u, 0x338ADCD9u, 1570586084u,
};

static __attribute__((noinline, section(".text.late.nativemac"))) bool pixgridKnownData(const DataWin* dataWin) {
    for (int32_t k = 0; 2 > k; k++) {
        const uint32_t* key = k == 0 ? pixgridData : pixgridMacKey;
        if (dataWin->fileSize == key[0] && dataWin->gen8.gameID == key[1] && dataWin->gen8.timestamp == key[2]) return true;
    }
    return false;
}

// Whether pixgridDrawSprite takes this item (utkill, utkill8): ctr_renderer.c's CtrUtk_plainReady refuses a plain
// run of it. Frame 1's strips follow the game's own draw, which reaches the renderer unchanged, and a run's later
// draws skip the vtable, so they would skip the strips too. Before the data is known, item 0 is refused as well.
__attribute__((noinline, section(".text.late.utkill"))) bool Pixgrid_takes(int32_t tpagIndex) {
    return tpagIndex == pixgrid.slot[(tpagIndex & 8) >> 2];
}

// Both GOG backdrop frames have the same decoded RGBA hashes as Steam's (gogut, 2026-10-22).
// The GOG TPAG indices 50 and 58 are distinguished by bit 3, as pixgridInit requires.
static __attribute__((noinline, section(".text.late.gogut"))) bool pixgridKnownGogData(const DataWin* dataWin) {
    return pixgridKnownData(dataWin) || (dataWin->fileSize == 62951906 && dataWin->gen8.gameID == 0x338ADCD9u &&
                                        dataWin->gen8.timestamp == 1510950063u);
}

#endif
#else
#line 63
#ifndef ENABLE_WAD17

#define PIXGRID_TEXT __attribute__((noinline, section(".text.late.pixgrid")))
#define PIXGRID_RO __attribute__((aligned(4), section(".rodata.pixgrid_in_text")))

// ctr_renderer.c's end: addresses, then offsets in CtrRenderer and constants.
enum { REF_VTABLE, REF_UPDATE_GRID_MAP, REF_COUNT };
enum {
    L_GRID_PICK, L_GRID_HOLD, L_GRID_MAP, L_BOUND_SURFACE, L_FILTER_OVERRIDE, L_BLEND_ENABLE, L_BLEND_MODE, L_SHADER_FX,
    L_PAL_KIND, L_FOG_ENABLE, L_SHADER_FX_NONE, L_PAL_NONE, L_GRID_MAP_KX, L_GRID_MAP_SIZE, L_FILTER_LINEAR,
    L_ITEM_STATE, L_COUNT
};
extern const void* const Pixgrid_refs[REF_COUNT];
extern const uint32_t Pixgrid_layout[L_COUNT];
#define PG_FIELD(r, index, type) (*(type*) ((char*) (r) + Pixgrid_layout[index]))

// The renderer's gridMap, field for field (pixgridInstall checks its layout).
typedef struct {
    int32_t surface;
    bool on, x, y;
    float kx, ox, invKx;
    float ky, oy, invKy;
} PixgridMap;

typedef void (*PixgridUpdateGridMap)(void* r);

static struct {
    RendererVtable outer;
    const RendererVtable* inner; // the renderer's own table (ctrVtable)
    int32_t slot[3];             // spr_battlebg's frames 0 and 1 at [tpag & 8 ? 2 : 0] (INT32_MIN when none); [1] is
                                 // frame 0's item (slot's place in main, then first's: the same 12 bytes)
} pixgrid __attribute__((section(".bss.late.pixgrid")));

static const char pixgridName[16] PIXGRID_RO = "spr_battlebg";
// UNDERTALE 1.08 (Steam): data.win bytes, GEN8 game ID and timestamp; spr_battlebg[0]'s and [1]'s item sizes
// (width | height << 16).
static const uint32_t pixgridData[5] PIXGRID_RO = { 62921978u, 0x338ADCD9u, 1510935285u, 609u | 236u << 16, 639u | 253u << 16 };
// Runs of opaque ink, x | y << 10 | (length - 1) << 18 | (1 for a row) << 26: spr_battlebg[0]'s corner texel, then
// spr_battlebg[1]'s 22 runs, derived privately by tools/aot/pixgrid.py from the player's data at build time.
static const uint32_t pixgridRuns[23] PIXGRID_RO = {
#include "pixgrid_runs.inc"
};
#line 105
#define PIXGRID_LOG "Smart smoothing: item %d's lines kept\n"
static const char pixgridLog[(sizeof(PIXGRID_LOG) + 3) & ~(size_t) 3] PIXGRID_RO = PIXGRID_LOG;

// The renderer state a draw needs: {Pixgrid_layout index, bytes, value (or PIXGRID_FROM | the index holding it)}.
#define PIXGRID_FROM 0x80
static const uint8_t pixgridState[7][3] PIXGRID_RO = {
    { L_GRID_PICK, 1, 1 },
    { L_GRID_HOLD, 4, 0 },
    { L_BLEND_ENABLE, 1, 1 },
    { L_BLEND_MODE, 4, bm_normal },
    { L_SHADER_FX, 1, PIXGRID_FROM | L_SHADER_FX_NONE },
    { L_PAL_KIND, 1, PIXGRID_FROM | L_PAL_NONE },
    { L_FOG_ENABLE, 1, 0 },
};

// Whether this draw of the item would lose lines (see the top). Smart smoothing's own verdict on the item comes first:
// one that averages it (fine) or places it by phase would change the copies, so such an item draws as before
// (Smart leaves items over 96 texels a side alone today; a check that changes that must not break the lines). The
// verdict comes from the item's texels (itemVerdict), not the setting, so it holds under Off as under Smart.
static PIXGRID_TEXT bool pixgridLoses(Renderer* r, int32_t tpagIndex, float xscale, float yscale, float angleDeg, uint32_t color, float alpha) {
    uint32_t verdict = Pixgrid_layout[L_ITEM_STATE]; // the field's offset, and above bit 24 the bits that move it
    const uint8_t* verdicts = *(const uint8_t* const*) ((const char*) r + (verdict & 0xFFFFFFu));
    if (verdicts != nullptr && (verdicts[tpagIndex] & (verdict >> 24)) != 0) return false;
    if (xscale != 1.0f || yscale != 1.0f || angleDeg != 0.0f || (color & 0xFFFFFFu) != 0xFFFFFFu || 1.0f > alpha) return false;
    for (int32_t i = 0; 7 > i; i++) {
        const uint8_t* e = pixgridState[i];
        const uint8_t* field = (const uint8_t*) r + Pixgrid_layout[e[0]];
        uint32_t value = e[1] == 4 ? *(const uint32_t*) field : *field;
        if (value != ((e[2] & PIXGRID_FROM) != 0 ? Pixgrid_layout[e[2] & ~PIXGRID_FROM] : e[2])) return false;
    }
    const PixgridMap* map = &PG_FIELD(r, L_GRID_MAP, PixgridMap);
    if (map->surface != PG_FIELD(r, L_BOUND_SURFACE, int32_t)) ((PixgridUpdateGridMap) Pixgrid_refs[REF_UPDATE_GRID_MAP])(r); // gridActive's work
    if (!map->on || !map->x || !map->y || fabsf(map->kx - 1.0f) > 0.0001f || fabsf(map->ky - 1.0f) > 0.0001f) return false;
    const BlendFactors* f = &r->blendFactors;
    int8_t filterOverride = PG_FIELD(r, L_FILTER_OVERRIDE, int8_t);
    bool linear = filterOverride != 0 ? filterOverride == (int8_t) Pixgrid_layout[L_FILTER_LINEAR] : r->texFilter;
    return f->src == bm_src_alpha && f->dst == bm_inv_src_alpha && 0 > r->currentShader && !linear;
}

// Whether a copy whose first texel lands at surface coordinate s can show: the shrink keeps even pixels. The first
// pixel is the one whose centre s covers (gridExcess's rounding); a start off whole pixels always draws.
static inline __attribute__((always_inline)) bool pixgridShows(float s) {
    int32_t first = 16384 - (int32_t) (16384.5f - s);
    return (first & 1) == 0 || fabsf(s - (float) first) > 0.25f;
}

// A draw of either frame: frame 0's copies before the game's own draw, frame 1's strips after it.
static PIXGRID_TEXT void pixgridDrawKept(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                         float yscale, float angleDeg, uint32_t color, float alpha) {
    const RendererVtable* inner = pixgrid.inner;
    int32_t frame = tpagIndex == pixgrid.slot[1] ? 0 : 1;
    bool keep = pixgridLoses(renderer, tpagIndex, xscale, yscale, angleDeg, color, alpha);
    for (int32_t k = 0; (keep && frame == 0 ? 2 : 0) > k; k++) { // frame 0: one left, one up
        inner->drawSprite(renderer, tpagIndex, x - (float) (k == 0), y - (float) (k != 0), originX, originY, xscale, yscale, angleDeg, color, alpha);
    }
    inner->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
    if (!keep) return;
    const TexturePageItem* tpag = &renderer->dataWin->tpag.items[tpagIndex];
    float left = x + (float) tpag->targetX - originX, top = y + (float) tpag->targetY - originY;
    const PixgridMap* map = &PG_FIELD(renderer, L_GRID_MAP, PixgridMap);
    for (int32_t i = frame; (frame != 0 ? 23 : 1) > i; i++) { // frame 0: its corner texel up-left; frame 1: its runs
        uint32_t run = pixgridRuns[i];
        float rx = (float) (run & 1023u), ry = (float) ((run >> 10) & 255u), length = (float) (((run >> 18) & 255u) + 1u);
        bool row = (run >> 26) != 0;
        float w = row ? length : 1.0f, h = row ? 1.0f : length;
        for (int32_t k = frame != 0 ? 0 : 2; 3 > k; k++) { // one left, one up, one up-left
            float cx = left + rx - (float) (k != 1), cy = top + ry - (float) (k != 0);
            if ((!row || k != 1) && !pixgridShows(map->kx * cx + map->ox)) continue;
            if ((row || k != 0) && !pixgridShows(map->ky * cy + map->oy)) continue;
            inner->drawSpritePart(renderer, tpagIndex, rx, ry, w, h, cx, cy, xscale, yscale, angleDeg, 0.0f, 0.0f, color, alpha);
        }
    }
}

// Every drawSprite comes here: both ways on are tail calls with the arguments as they came, so the game's other draws
// pay one look-up and compare and nothing is saved or moved (the copies' work lives in pixgridDrawKept). The two
// items sit in the slot of their index's bit 3: testing both by name made gcc save and move every argument per draw.
static PIXGRID_TEXT void pixgridDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                           float yscale, float angleDeg, uint32_t color, float alpha) {
    if (tpagIndex == pixgrid.slot[(tpagIndex & 8) >> 2]) {
        pixgridDrawKept(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        return;
    }
    pixgrid.inner->drawSprite(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}
// The runner's init with the data: finds spr_battlebg's frames 0 and 1, in the data checked (Windows or macOS).
static bool pixgridKnownData(const DataWin* dataWin);
static PIXGRID_TEXT void pixgridInit(Renderer* renderer, DataWin* dataWin) {
    __asm__ volatile(".rept 14\n\tnop\n\t.endr\n"); pixgrid.inner->init(renderer, dataWin);
    pixgrid.slot[0] = pixgrid.slot[2] = INT32_MIN;
    if (!pixgridKnownData(dataWin)) return;
    for (uint32_t i = 0; dataWin->sprt.count > i; i++) {
        const Sprite* sp = &dataWin->sprt.sprites[i];
        if (sp->name == nullptr || strcmp(sp->name, pixgridName) != 0) continue;
        if (2 > sp->textureCount) return;
        for (int32_t f = 0; 2 > f; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 > t || dataWin->tpag.count <= (uint32_t) t) return;
            const TexturePageItem* item = &dataWin->tpag.items[t];
            uint32_t size = pixgridData[3 + f];
            if (item->sourceWidth != (size & 0xFFFFu) || item->sourceHeight != size >> 16 || item->targetWidth != item->sourceWidth ||
                item->targetHeight != item->sourceHeight) {
                return;
            }
        }
        int32_t t0 = sp->tpagIndices[0], t1 = sp->tpagIndices[1];
        if (((t0 ^ t1) & 8) == 0) return; // bit 3 tells them apart: 39 and 42 (Windows), 49 and 59 (macOS)
        pixgrid.slot[(t0 & 8) >> 2] = pixgrid.slot[1] = t0;
        pixgrid.slot[(t1 & 8) >> 2] = t1;
        logInfo(pixgridLog, (int) t0);
        logInfo(pixgridLog, (int) t1);
        return;
    }
}

// After each CtrRenderer_create: a renderer still on its own table gets the outer one (a later call changes nothing).
static PIXGRID_TEXT void pixgridInstall(Renderer* renderer) {
    RendererVtable* inner = (RendererVtable*) Pixgrid_refs[REF_VTABLE];
    if (renderer->vtable != inner) return;
    if (Pixgrid_layout[L_GRID_MAP_KX] != __builtin_offsetof(PixgridMap, kx) || Pixgrid_layout[L_GRID_MAP_SIZE] != sizeof(PixgridMap)) return;
    pixgrid.inner = inner;
    pixgrid.slot[0] = pixgrid.slot[2] = INT32_MIN;
    pixgrid.outer = *inner;
    pixgrid.outer.init = pixgridInit;
    pixgrid.outer.drawSprite = pixgridDrawSprite;
    renderer->vtable = &pixgrid.outer;
}

void __real_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);
void __wrap_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);

PIXGRID_TEXT void __wrap_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count) {
    __real_CtrRenderer_setSurfaceReaders(renderer, readers, count);
    pixgridInstall(renderer);
}

// ===[ nativemac: UNDERTALE's macOS data (game.ios, on the card as data.win) ]===
//
// The macOS file is a later (2019) build of the same 1.08 game: spr_battlebg's two items have the same sizes and
// texels, but its repacked atlas lists them as items 49 and 59 (both odd, so the slots go by bit 3, above). Its key:
// bytes, GEN8 game ID and timestamp. pixgridInit's nops (run once per load) keep it at main's size (440 B).
static const uint32_t pixgridMacKey[3] __attribute__((aligned(4), section(".rodata.nativemac_in_text"))) = {
    62956178u, 0x338ADCD9u, 1570586084u,
};

static __attribute__((noinline, section(".text.late.nativemac"))) bool pixgridKnownData(const DataWin* dataWin) {
    for (int32_t k = 0; 2 > k; k++) {
        const uint32_t* key = k == 0 ? pixgridData : pixgridMacKey;
        if (dataWin->fileSize == key[0] && dataWin->gen8.gameID == key[1] && dataWin->gen8.timestamp == key[2]) return true;
    }
    return false;
}

// Whether pixgridDrawSprite takes this item (utkill): ctr_renderer.c's CtrUtk_plainReady refuses a plain
// run of it. Frame 1's strips follow the game's own draw, which reaches the renderer unchanged, and a run's later
// draws skip the vtable, so they would skip the strips too. Before the data is known, item 0 is refused as well.
__attribute__((noinline, section(".text.late.utkill"))) bool Pixgrid_takes(int32_t tpagIndex) {
    return tpagIndex == pixgrid.slot[(tpagIndex & 8) >> 2];
}

#endif
#endif
#line 266
