// bggrid: DELTARUNE's scrolling battle grid at native drawing, drawn between output pixels (mode 4 by default
// so that the grid matches the PC picture). Late code only (platform/3ds/late_reserve.ld): nothing here moves existing code or data.
//
// The grid is the battle backdrop object's Draw event (every chapter): siner += 0.5 and siner2 += 1 each frame, then
// draw_background_tiled_ext(bg_battleback1, round(siner - 100), round(siner - 100), 1, 1, image_blend, image_alpha / 2)
// and draw_background_tiled_ext(bg_battleback1, round(-200 - siner2), round(-210 - siner2), 1, 1, image_blend,
// image_alpha), over a black rectangle. bg_battleback1 is a 50x50 tile, one line of (66, 0, 66) on texel column 21
// and row 22 (all five chapters' data). So one layer moves 1 room pixel a frame up and left, the other 1 every other
// frame down and right, both at whole room pixels.
//
// At native each output pixel is two room pixels. The renderer places this tile by its phase (itemVerdict's
// ITEM_PHASED), so the lines show at full colour on every frame, but a line moving 1 room pixel a frame moves 1 output
// pixel every other frame: it stands for two frames, then jumps (the shrunk PC picture does the same, at half colour).
// Here, with "bggrid = N" in settings.ini (read at each load; 0 leaves the grid as before), each layer
// is drawn twice with linear filtering, moved by fractions of a room pixel, so that each line lands on two output
// pixels with weights that follow its true position:
//   1  tent: 3/4 on the pixel holding the line's centre and 1/4 on the nearer neighbour (the PC picture shrunk with
//      a 2x tent filter, at the line's full colour): the same brightness on every frame, moving half a pixel a frame;
//   2  half: the line one output pixel wide at its room position: whole on even room columns, half on two pixels
//      on odd ones;
//   3  tent with weights 0.88 and 0.45: about the same light as the full-colour line on the LCD's gamma (over black).
// A tiled draw at native samples texel 2p - X + 0.5 for output pixel p (the half-pixel move of nativeOffset), so with
// linear filtering a line at room column X gives output pixel p the weight max(0, 1 - |2p - X - s|) for a move of s
// room pixels: one draw puts weight g on one pixel and none on any other (they are 2 apart). Two draws give the two
// pixels; their moves come from the line's parity on each axis (bggridMove), on the screen: the view can sit on an
// odd pixel (Chapter 1's Rudinn battle draws from view y -329), so the parity is that of the line's view coordinate. The tile's transparent texels are
// transparent black, so the draws use premultiplied blending (one, inverse source alpha) with the colour scaled by
// the alpha: a filtered texel w * (66, 0, 66, 1) then adds w * alpha * colour over (1 - w * alpha) of what is there.
// The grid hold stops the pixel grid from moving the quads back onto the phase.
//
// Modes 4 and 5 (match the PC picture, at the PC's line weight): no texture at all. Each layer's
// lines are drawn as untextured rectangles, one output pixel across, whose ink is what the PC frame shrunk to the
// output gives (bggridQuads, below). Mode 4 is the PC frame box-filtered (each output pixel the mean of its 2x2 PC
// pixels): a 1-pixel line covers half of one output pixel, so it shows at half its colour on that pixel. Mode 5 is the
// PC frame shrunk with a 2x tent filter (1/8 3/8 3/8 1/8): 3/8 on the line's pixel and 1/8 on the nearer neighbour.
//
// Only draw_background_tiled_ext of that tile at 1:1, tiled both ways, onto a half-resolution target, with normal
// blending, no shader and no filtering, takes this path; anything else goes to the table's own drawSpriteTiled.
// UNDERTALE builds compile this file empty.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "utils.h"

#ifdef ENABLE_WAD17

#define BGGRID_TEXT __attribute__((noinline, section(".text.late.bggrid")))
#define BGGRID_CONST __attribute__((aligned(4), section(".rodata.bggrid_in_text")))

// ctr_renderer.c's end: the renderer's own vtable (PixNslice_refs), the grid hold (PixNslice_layout) and where the
// bound surface's shift is (Cmdlabel_layout: boundSurface, surfaces, sizeof(CtrSurface), CtrSurface.shift).
extern const void* const PixNslice_refs[1];
extern const uint32_t PixNslice_layout[1];
extern const uint32_t Cmdlabel_layout[4];

static const char bggridName[16] BGGRID_CONST = "bg_battleback1";
static const char bggridPath[] BGGRID_CONST = CTRPORT_GAME_DIR "/settings.ini";
static const char bggridRead[4] BGGRID_CONST = "r";
static const char bggridKey[] BGGRID_CONST = " bggrid = %d";
static const char bggridLogMode[] BGGRID_CONST = "Native: battle grid mode %d (item %d, view %d %d)\n";
// Modes 4 and 5: the coverage of a 1-pixel line on its own output pixel and on its neighbour, for a line on an even view
// column, then an odd one (the neighbour is the pixel before for an even column, the one after for an odd one).
static const float bggridQuadWeights[2][4] BGGRID_CONST = {
    { 0.5f, 0.0f, 0.5f, 0.0f },
    { 0.375f, 0.125f, 0.375f, 0.125f },
};
// Per mode: the weights on the line's own pixel and its neighbour, for a line on an even room column, then an odd one.
static const float bggridWeights[4][4] BGGRID_CONST = {
    { 1.0f, 0.0f, 1.0f, 0.0f },
    { 0.75f, 0.25f, 0.75f, 0.25f },
    { 1.0f, 0.0f, 0.5f, 0.5f },
    { 0.88f, 0.45f, 0.88f, 0.45f },
};
// The tile's line: texel column 21 and row 22.
#define BGGRID_LINE_X 21
#define BGGRID_LINE_Y 22
#define BGGRID_SIZE 50
#define BGGRID_MAX_LINES 32
// The mode without a "bggrid" line in settings.ini: 4, the PC frame box-filtered (to
// match the PC). "bggrid = 0" draws the grid as before. A build may set it.
#ifndef BGGRID_DEFAULT_MODE
#define BGGRID_DEFAULT_MODE 4
#endif

typedef void (*BggridDrawSpriteTiled)(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y, float xscale,
                                      float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color, float alpha);

__attribute__((section(".bss.late.bggrid"))) static struct {
    int32_t mode;
    BggridDrawSpriteTiled saved; // the table's drawSpriteTiled when hooked, if it wasn't ctrVtable's own
    const DataWin* dataWin;      // the data item was looked up in
    int32_t item;                // bg_battleback1's frame 0, or -1
    int32_t logged;
    // Modes 4 and 5: the lines of the layer drawn premixed over black just before (the backdrop's first layer), in
    // view pixels, x lines then y lines, and its alpha; used by the next layer's draw, then dropped.
    int32_t stashN[2];
    float stashAlpha;
    int16_t stash[2][BGGRID_MAX_LINES];
} bggrid;

static BGGRID_TEXT bool bggridNative(Renderer* renderer) {
    int32_t bound = *(const int32_t*) ((const char*) renderer + Cmdlabel_layout[0]);
    if (0 > bound) return false;
    const char* surfaces = *(const char* const*) ((const char*) renderer + Cmdlabel_layout[1]);
    return *(const int32_t*) (surfaces + (size_t) bound * Cmdlabel_layout[2] + Cmdlabel_layout[3]) > 0;
}

static BGGRID_TEXT int32_t bggridFind(const DataWin* dw) {
    for (uint32_t s = 0; dw->sprt.count > s; s++) {
        const Sprite* sp = &dw->sprt.sprites[s];
        if (sp->name == nullptr || strcmp(sp->name, bggridName) != 0) continue;
        if (sp->textureCount < 1 || sp->width != BGGRID_SIZE || sp->height != BGGRID_SIZE || sp->originX != 0 || sp->originY != 0) return -1;
        int32_t t = sp->tpagIndices[0];
        if (0 > t || dw->tpag.count <= (uint32_t) t) return -1;
        const TexturePageItem* it = &dw->tpag.items[t];
        if (it->sourceWidth != BGGRID_SIZE || it->sourceHeight != BGGRID_SIZE || it->targetWidth != BGGRID_SIZE || it->targetHeight != BGGRID_SIZE ||
            it->targetX != 0 || it->targetY != 0 || it->boundingWidth != BGGRID_SIZE || it->boundingHeight != BGGRID_SIZE) {
            return -1;
        }
        return t;
    }
    return -1;
}

// The moves (view pixels) of the two draws on one axis, for a line at view coordinate c (whole): the first puts
// weight g1 on output pixel m = floor(c / 2), the second g2 on m - 1 (c even) or m + 1 (c odd), or nothing.
static BGGRID_TEXT void bggridMove(float c, float* s1, float* s2) {
    int32_t ci = (int32_t) (c + 16384.0f) - 16384; // floor for |c| < 16384
    const float* w = bggridWeights[bggrid.mode];
    if ((ci & 1) == 0) {
        *s1 = -(1.0f - w[0]);
        *s2 = -2.0f + (1.0f - w[1]);
    } else {
        *s1 = -1.0f + (1.0f - w[2]);
        *s2 = w[3];
    }
}

// Modes 4 and 5, one axis: the view coordinates of the tile's lines on it (whole pixels; each line covers [v, v + 1)),
// for the tiles that draw_background_tiled_ext puts from start to end (room pixels; as the table's drawSpriteTiled
// tiles), keeping those within two pixels of the visible part of the view, [vis0, vis1). a and m map room to view.
static BGGRID_TEXT int32_t bggridLines(float start, float end, float line, float a, float m, float vis0, float vis1, int16_t* out) {
    int32_t n = 0;
    float first = 0.0f;
    // The first tile whose line can be in view, without stepping through the room's other tiles.
    float lo = a > 0.0f ? vis0 - 2.0f - m : m - vis1 - 2.0f;
    if (lo - line - start > (float) BGGRID_SIZE) first = (float) (int32_t) ((lo - line - start) / (float) BGGRID_SIZE) - 1.0f;
    for (float t = start + first * (float) BGGRID_SIZE; end > t && BGGRID_MAX_LINES > n; t += (float) BGGRID_SIZE) {
        float v = a * (t + line) + m;
        if (0.0f > a) v -= 1.0f;
        if (vis0 - 2.0f > v + 1.0f || v > vis1 + 2.0f) continue;
        int32_t vi = (int32_t) (v + 16384.0f) - 16384; // floor for |v| < 16384
        out[n++] = (int16_t) vi;
    }
    return n;
}

// Modes 4 and 5: one layer's lines as rectangles one output pixel across. A line at view column v covers half of output
// pixel floor(v / 2) (that pixel is view columns 2p and 2p + 1); the rectangle for pixel p is view [2p - 1, 2p + 1),
// whose edges fall on surface pixel edges once nativeOffset has moved it half a surface pixel.
//
// Blending stays the game's normal blending (source alpha, one minus source alpha). Drawn plainly, each rectangle is the
// line's colour at the layer's alpha times its coverage f: that is the box picture exactly wherever the line's pixel
// holds nothing else of the layer, but where a vertical and a horizontal line of the layer cross it gives
// a(fv + fh) - a^2 fv fh instead of a(fv + fh - fv fh), too bright for a < 1. Over black it is exact everywhere when the
// colour is premixed (the line's colour times a, at alpha f): over one colour the rectangles give 1 - (1 - fv)(1 - fh),
// the union of the two lines' coverage, as the box does. The backdrop draws its first layer (x == y, both
// round(siner - 100)) at image_alpha / 2 right after a black rectangle at image_alpha, so at alpha 0.5 or more that
// rectangle was opaque and the layer is premixed; its lines are kept for the second layer. Where a line of the second
// layer (alpha 1) is on the same output pixel as a parallel line of the first, the box picture depends on whether the
// two share a PC pixel (the second hides the first) or not (both show): that rectangle's alpha makes it so.
static BGGRID_TEXT void bggridQuads(Renderer* renderer, float x, float y, float roomW, float roomH, uint32_t color, float alpha, const float* m,
                                    float ax, float ay, const float* vis) {
    const float* w = bggridQuadWeights[bggrid.mode - 4];
    float a = alpha > 1.0f ? 1.0f : alpha;
    // The tile's texels are (66, 0, 66) on the line, times the draw's colour as the texture combiner would.
    uint32_t r8 = (66u * BGR_R(color) + 127u) / 255u, b8 = (66u * BGR_B(color) + 127u) / 255u;
    bool premix = x == y && 0.5f <= alpha && 1.0f > alpha;
    bool fix = !premix && 1.0f <= alpha && bggrid.mode == 4 && 0 < bggrid.stashN[0] + bggrid.stashN[1];
    float qa = a;
    if (premix) {
        r8 = (uint32_t) ((float) r8 * a + 0.5f);
        b8 = (uint32_t) ((float) b8 * a + 0.5f);
        qa = 1.0f;
    }
    uint32_t col = (b8 << 16) | r8;
    float k = bggrid.stashAlpha;
    float aSame = (0.5f - 0.5f * k) / (1.0f - 0.5f * k), aApart = 0.5f / (1.0f - 0.5f * k);
    // Each axis: tiles from start to end (as the table's drawSpriteTiled), the lines, and the view span they cover.
    float span0[2], span1[2];
    float pos[2] = { x, y }, roomEnd[2] = { roomW, roomH }, as[2] = { ax, ay };
    int16_t lines[2][BGGRID_MAX_LINES];
    int32_t n[2];
    for (int32_t i = 0; 2 > i; i++) {
        float start = fmodf(pos[i], (float) BGGRID_SIZE);
        if (start > 0.0f) start -= (float) BGGRID_SIZE;
        float tiles = (float) (int32_t) ((roomEnd[i] - start + (float) BGGRID_SIZE - 1.0f) / (float) BGGRID_SIZE);
        float e0 = as[i] * start + m[12 + i], e1 = as[i] * (start + tiles * (float) BGGRID_SIZE) + m[12 + i];
        if (e0 > e1) {
            float t = e0;
            e0 = e1, e1 = t;
        }
        span0[i] = e0 > vis[2 * i] - 2.0f ? e0 : vis[2 * i] - 2.0f;
        span1[i] = e1 < vis[2 * i + 1] + 2.0f ? e1 : vis[2 * i + 1] + 2.0f;
        n[i] = bggridLines(start, roomEnd[i], i == 0 ? (float) BGGRID_LINE_X : (float) BGGRID_LINE_Y, as[i], m[12 + i], vis[2 * i], vis[2 * i + 1],
                           lines[i]);
    }
    RendererVtable* vt = renderer->vtable;
    for (int32_t i = 0; 2 > i; i++) {
        int32_t o = 1 - i;
        if (span0[o] >= span1[o]) continue;
        float t0 = (span0[o] - m[12 + o]) / as[o], t1 = (span1[o] - m[12 + o]) / as[o];
        for (int32_t j = 0; n[i] > j; j++) {
            int32_t v = lines[i][j];
            int32_t p = v >> 1, odd = v & 1;
            for (int32_t q = 0; 2 > q; q++) {
                float f = w[2 * odd + q];
                if (0.0f >= f) continue;
                int32_t px = q == 0 ? p : (odd ? p + 1 : p - 1);
                float fa = qa * f;
                if (fix && q == 0) {
                    for (int32_t s = 0; bggrid.stashN[i] > s; s++) {
                        if (bggrid.stash[i][s] >> 1 != p) continue;
                        fa = bggrid.stash[i][s] == v ? aSame : aApart;
                        break;
                    }
                }
                // The rectangle in view pixels, then in room pixels (the view is at 1:1, maybe flipped).
                float u0 = ((float) (2 * px - 1) - m[12 + i]) / as[i], u1 = ((float) (2 * px + 1) - m[12 + i]) / as[i];
                float r0[2], r1[2];
                r0[i] = u0 < u1 ? u0 : u1, r1[i] = u0 < u1 ? u1 : u0;
                r0[o] = t0 < t1 ? t0 : t1, r1[o] = t0 < t1 ? t1 : t0;
                // GameMaker's filled rectangles include the far edge (+1).
                vt->drawRectangle(renderer, r0[0], r0[1], r1[0] - 1.0f, r1[1] - 1.0f, col, fa, false);
            }
        }
    }
    bggrid.stashN[0] = bggrid.stashN[1] = 0;
    if (premix && bggrid.mode == 4) {
        for (int32_t i = 0; 2 > i; i++) {
            bggrid.stashN[i] = n[i];
            memcpy(bggrid.stash[i], lines[i], sizeof(lines[i]));
        }
        bggrid.stashAlpha = a;
    }
}

static BGGRID_TEXT void bggridDrawSpriteTiled(Renderer* renderer, int32_t tpagIndex, float originX, float originY, float x, float y,
                                              float xscale, float yscale, bool tileX, bool tileY, float roomW, float roomH, uint32_t color,
                                              float alpha) {
    BggridDrawSpriteTiled inner = bggrid.saved != nullptr ? bggrid.saved : ((const RendererVtable*) PixNslice_refs[0])->drawSpriteTiled;
    if (bggrid.dataWin != renderer->dataWin) {
        bggrid.dataWin = renderer->dataWin;
        bggrid.item = bggridFind(renderer->dataWin);
        bggrid.logged = 0;
    }
    RendererVtable* vt = renderer->vtable;
    if (tpagIndex != bggrid.item || xscale != 1.0f || yscale != 1.0f || !tileX || !tileY || originX != 0.0f || originY != 0.0f ||
        0.0f >= alpha || !bggridNative(renderer) || renderer->texFilter || 0 <= renderer->currentShader || !vt->gpuGetBlendEnable(renderer) ||
        vt->gpuGetBlendMode(renderer) != bm_normal) {
        inner(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha);
        return;
    }
    // The lines' parity is the screen's, so it comes from where the view puts them (rooms scroll their views by
    // single pixels, odd ones too): only a view at 1:1, not turned, takes this path.
    const float* m = renderer->gmlMatrices[MATRIX_WORLD_VIEW].m;
    float ax = m[0], ay = m[5];
    if ((ax != 1.0f && ax != -1.0f) || (ay != 1.0f && ay != -1.0f) || m[1] != 0.0f || m[4] != 0.0f) {
        inner(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha);
        return;
    }
    // The line's first edge in view pixels (texel column 21 covers [x + 21, x + 22) in the room).
    float lx = ax * (x + (float) BGGRID_LINE_X) + m[12], ly = ay * (y + (float) BGGRID_LINE_Y) + m[13];
    if (0.0f > ax) lx -= 1.0f;
    if (0.0f > ay) ly -= 1.0f;
    if (bggrid.logged == 0) {
        bggrid.logged = 1;
        logInfo(bggridLogMode, (int) bggrid.mode, (int) tpagIndex, (int) m[12], (int) m[13]);
    }
    if (4 <= bggrid.mode) {
        // The visible part of the view, from an orthographic projection.
        const float* pm = renderer->gmlMatrices[MATRIX_PROJECTION].m;
        if (pm[0] == 0.0f || pm[5] == 0.0f || pm[1] != 0.0f || pm[4] != 0.0f || pm[3] != 0.0f || pm[7] != 0.0f || pm[15] != 1.0f) {
            inner(renderer, tpagIndex, originX, originY, x, y, xscale, yscale, tileX, tileY, roomW, roomH, color, alpha);
            return;
        }
        float vis[4] = { (-1.0f - pm[12]) / pm[0], (1.0f - pm[12]) / pm[0], (-1.0f - pm[13]) / pm[5], (1.0f - pm[13]) / pm[5] };
        for (int32_t i = 0; 4 > i; i += 2) {
            if (vis[i] > vis[i + 1]) {
                float t = vis[i];
                vis[i] = vis[i + 1], vis[i + 1] = t;
            }
        }
        bggridQuads(renderer, x, y, roomW, roomH, color, alpha, m, ax, ay, vis);
        return;
    }
    float sx1, sx2, sy1, sy2;
    bggridMove(lx, &sx1, &sx2);
    bggridMove(ly, &sy1, &sy2);
    sx1 *= ax, sx2 *= ax, sy1 *= ay, sy2 *= ay; // view pixels to room pixels
    float a = alpha > 1.0f ? 1.0f : alpha;
    uint32_t pre = 0;
    for (int32_t k = 0; 3 > k; k++) pre |= (uint32_t) ((float) ((color >> (8 * k)) & 0xFFu) * a + 0.5f) << (8 * k);
    int32_t* hold = (int32_t*) ((char*) renderer + PixNslice_layout[0]);
    vt->gpuSetBlendModeExt(renderer, bm_one, bm_inv_src_alpha, bm_one, bm_inv_src_alpha);
    vt->gpuSetTexFilter(renderer, true);
    (*hold)++;
    inner(renderer, tpagIndex, 0.0f, 0.0f, x + sx1, y + sy1, 1.0f, 1.0f, true, true, roomW, roomH, pre, alpha);
    inner(renderer, tpagIndex, 0.0f, 0.0f, x + sx2, y + sy2, 1.0f, 1.0f, true, true, roomW, roomH, pre, alpha);
    (*hold)--;
    vt->gpuSetTexFilter(renderer, false);
    vt->gpuSetBlendMode(renderer, bm_normal);
}

void __real_CtrRenderer_setSliderOverride(Renderer* renderer, float position);
void __wrap_CtrRenderer_setSliderOverride(Renderer* renderer, float position);

// main.c calls this once per load with -1, after CtrRenderer_setSurfaceReaders (ctr_pixfix_nslice.c's outer table)
// and CtrRenderer_loadFlickerProbe (ctr_cmdlabel.c's hooks), before the first frame; slider tests also call it each
// frame with 0 or 1, which changes nothing here.
BGGRID_TEXT void __wrap_CtrRenderer_setSliderOverride(Renderer* renderer, float position) {
    __real_CtrRenderer_setSliderOverride(renderer, position);
    if (position >= 0.0f) return;
    int mode = BGGRID_DEFAULT_MODE;
    FILE* f = fopen(bggridPath, bggridRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) sscanf(line, bggridKey, &mode);
        fclose(f);
    }
    bggrid.mode = 0 < mode && 6 > mode ? mode : 0;
    bggrid.stashN[0] = bggrid.stashN[1] = 0;
    bggrid.dataWin = nullptr;
    bggrid.item = -1;
    if (bggrid.mode == 0) return;
    RendererVtable* vt = renderer->vtable;
    const RendererVtable* own = (const RendererVtable*) PixNslice_refs[0];
    // Only in front of the renderer's own table (ctr_pixfix_nslice.c's outer one), never in it: frame skip rewrites
    // ctrVtable's drawSpriteTiled.
    if (vt != own && vt->drawSpriteTiled != bggridDrawSpriteTiled) {
        bggrid.saved = vt->drawSpriteTiled == own->drawSpriteTiled ? nullptr : vt->drawSpriteTiled;
        vt->drawSpriteTiled = bggridDrawSpriteTiled;
    }
}

#endif // ENABLE_WAD17
