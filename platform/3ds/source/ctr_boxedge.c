// boxedge: thin bars at half resolution, both games. Late code only (platform/3ds/late_reserve.ld): nothing here
// moves existing code or data.
//
// A bar drawn as a stretched sprite (UNDERTALE's battle box: its border sprite, 5x5, stretched along the box and 5 drawing
// units across) is 2.5 texels across on a half-resolution target, so it covers 2 or 3 sample points depending on where
// it lands: the box showed sides of 2 and 3 pixels, and a side that moves changed between them. Such a bar is drawn N
// texels across instead, N its width rounded half up (2.5 to 3, as a widened thin line is): its far edge (the larger
// surface coordinate) moves by N less its width, and an interval of exactly N texels holds N sample points wherever it
// starts, whatever the pixel grid then moves it by. Its other axis's far end moves by as much, so the bars of a box
// still meet: UNDERTALE's bottom bar ends at the right bar's far edge. Where a bar's far end meets another's near edge
// instead (the top bar's end at the right bar), the two then overlap by up to a texel, which an opaque bar drawn with
// plain alpha blending doesn't show; so only such draws are changed (Undyne's translucent box stays as it was).
//
// Bars are opaque (alpha 1, bm_normal's blend factors) upright sprites on a half-resolution target under the game's upright projection, 1 to 3.5 texels across
// and at least 8 and 4 times that long, stretched (scale beyond 2.5) along their length. Art magnified across (a
// texel of it a pixel or more, more than one texel across) is left as it is, and so is a bar already a whole number of
// texels across.
//
// The hook is the outermost drawSprite, installed where main.c applies the loaded settings (CtrBtnprompt_setDisplay,
// wrapped in CMakeLists.txt), after every other hook (ctr_pixgrid.c, ctr_pixfix_nslice.c, ctr_cmdlabel.c) is in place.
// It calls the entry it replaced, or the renderer's own one (which setSkipWrappers keeps current) when it replaced
// that. ctr_renderer.c's end has Boxedge_refs and Boxedge_layout: the renderer's own table and the private field
// offsets read here. Code added to ctr_renderer.c itself changes how GCC inlines the rest of that file.

#include <stdint.h>

#include <3ds.h>
#include <citro3d.h>

#include "common.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#define BOXEDGE_TEXT __attribute__((noinline, section(".text.late.boxedge")))
#define BOXEDGE_CONST __attribute__((aligned(4), section(".rodata.boxedge_in_text")))

enum {
    L_BOUND_SURFACE, // int32_t
    L_BOUND_TARGET,  // C3D_RenderTarget*
    L_SURFACES,      // CtrSurface*
    L_SURFACE_COUNT, // uint32_t
    L_SURFACE_SIZE,  // sizeof(CtrSurface)
    L_SURFACE_USED,  // CtrSurface.used, bool
    L_SURFACE_SHIFT, // CtrSurface.shift, int32_t
    L_GAME_PROJECTION, // bool
    L_PROJECTION,    // C3D_Mtx
    L_VIEWPORT_W,    // CtrRect viewport.w, int32_t (h follows)
    L_COUNT
};
extern const void* const Boxedge_refs[1];
extern const uint32_t Boxedge_layout[L_COUNT];
#define BX_FIELD(base, index, type) (*(const type*) ((const char*) (base) + Boxedge_layout[index]))

typedef void (*BoxedgeDrawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale, float yscale,
                                  float angleDeg, uint32_t color, float alpha);

__attribute__((section(".bss.late.boxedge"))) static BoxedgeDrawSprite boxedgeSaved;
static const char boxedgeLog[] BOXEDGE_CONST = "Native: thin bars drawn whole texels across (after %d)\n";

static inline __attribute__((always_inline)) float boxedgeAbs(float v) {
    return 0.0f > v ? -v : v;
}

// Moves the draw's x, y and scales so that a thin bar is a whole number of texels across; false if it isn't one.
static BOXEDGE_TEXT bool boxedgeFit(Renderer* renderer, int32_t tpagIndex, float* x, float* y, float originX, float originY, float* xscale,
                                    float* yscale) {
    DataWin* dw = renderer->dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return false;
    const TexturePageItem* tp = &dw->tpag.items[tpagIndex];
    int32_t id = BX_FIELD(renderer, L_BOUND_SURFACE, int32_t);
    if (BX_FIELD(renderer, L_BOUND_TARGET, void*) == nullptr || 0 > id || BX_FIELD(renderer, L_SURFACE_COUNT, uint32_t) <= (uint32_t) id) return false;
    const char* s = BX_FIELD(renderer, L_SURFACES, const char*) + (size_t) id * Boxedge_layout[L_SURFACE_SIZE];
    int32_t shift = BX_FIELD(s, L_SURFACE_SHIFT, int32_t);
    const C3D_Mtx* m = &BX_FIELD(renderer, L_PROJECTION, C3D_Mtx);
    if (!BX_FIELD(s, L_SURFACE_USED, bool) || 0 >= shift || shift > 4 || !BX_FIELD(renderer, L_GAME_PROJECTION, bool) || m->r[0].y != 0.0f ||
        m->r[1].x != 0.0f || m->r[3].x != 0.0f || m->r[3].y != 0.0f || m->r[3].w != 1.0f || 0 >= tp->targetWidth || 0 >= tp->targetHeight ||
        0 >= tp->sourceWidth || 0 >= tp->sourceHeight) {
        return false;
    }
    // As updateGridMap: surface pixel = k * unit + o.
    const int32_t* vp = &BX_FIELD(renderer, L_VIEWPORT_W, int32_t);
    float kx = m->r[0].x * 0.5f * (float) vp[0];
    float ky = -m->r[1].y * 0.5f * (float) vp[1];
    if (0.0f >= kx || 0.0f >= ky) return false;
    float t = (float) (1 << shift);
    float lx0 = (float) tp->targetX - originX, lx1 = lx0 + (float) tp->targetWidth;
    float ly0 = (float) tp->targetY - originY, ly1 = ly0 + (float) tp->targetHeight;
    float ex = boxedgeAbs((lx1 - lx0) * *xscale * kx / t);
    float ey = boxedgeAbs((ly1 - ly0) * *yscale * ky / t);
    // Its thin axis: extent across in texels, along, the scale along and the source texels across.
    bool vert = ex > ey;
    float across = vert ? ey : ex, along = vert ? ex : ey;
    float scaleAlong = boxedgeAbs(vert ? *xscale : *yscale);
    int32_t srcAcross = vert ? tp->sourceHeight : tp->sourceWidth;
    if (1.0f > across || across >= 3.5f || 8.0f > along || 4.0f * across > along || 2.5f >= scaleAlong) return false;
    if (srcAcross > 1 && across >= 0.99f * (float) srcAcross) return false;
    float grow = (float) (int32_t) (across + 0.5f + 1.0f / 64.0f) - across;
    if (1.0f / 64.0f > grow && grow > -1.0f / 64.0f) return false;
    // Each axis's far end (the larger room coordinate: k > 0) moves by grow texels.
    float x0 = *x + lx0 * *xscale, x1 = *x + lx1 * *xscale;
    float y0 = *y + ly0 * *yscale, y1 = *y + ly1 * *yscale;
    float gx = grow * t / kx, gy = grow * t / ky;
    if (x1 >= x0) x1 += gx;
    else x0 += gx;
    if (y1 >= y0) y1 += gy;
    else y0 += gy;
    *xscale = (x1 - x0) / (float) tp->targetWidth;
    *yscale = (y1 - y0) / (float) tp->targetHeight;
    *x = x0 - lx0 * *xscale;
    *y = y0 - ly0 * *yscale;
    return true;
}

static BOXEDGE_TEXT void boxedgeDrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                           float yscale, float angleDeg, uint32_t color, float alpha) {
    BoxedgeDrawSprite next = boxedgeSaved != nullptr ? boxedgeSaved : ((const RendererVtable*) Boxedge_refs[0])->drawSprite;
    const BlendFactors* f = &renderer->blendFactors;
    if (angleDeg == 0.0f && alpha >= 1.0f && (boxedgeAbs(xscale) > 2.5f || boxedgeAbs(yscale) > 2.5f) && f->src == bm_src_alpha &&
        f->dst == bm_inv_src_alpha) {
        boxedgeFit(renderer, tpagIndex, &x, &y, originX, originY, &xscale, &yscale);
    }
    next(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
}

void __real_CtrBtnprompt_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
#ifdef ENABLE_WAD17
// DELTARUNE: natsurf6 wraps CtrBtnprompt_setDisplay too, and a symbol can be wrapped once: its wrapper puts its table in
// front and then calls this, so the drawSprite hook still goes in last, on natsurf6's table (integrate12).
#define BOXEDGE_SET_DISPLAY Boxedge_setDisplay
#else
#define BOXEDGE_SET_DISPLAY __wrap_CtrBtnprompt_setDisplay
#endif
void BOXEDGE_SET_DISPLAY(Renderer* renderer, const CtrDisplaySettings* settings);
BOXEDGE_TEXT void BOXEDGE_SET_DISPLAY(Renderer* renderer, const CtrDisplaySettings* settings) {
    __real_CtrBtnprompt_setDisplay(renderer, settings);
    RendererVtable* vt = renderer->vtable;
    const RendererVtable* own = (const RendererVtable*) Boxedge_refs[0];
    // Only in front of the renderer's own table, never in it: frame skip rewrites ctrVtable's drawSprite.
    if ((const RendererVtable*) vt == own || vt->drawSprite == boxedgeDrawSprite) return;
    boxedgeSaved = vt->drawSprite == own->drawSprite ? nullptr : vt->drawSprite;
    vt->drawSprite = boxedgeDrawSprite;
    logInfo(boxedgeLog, boxedgeSaved != nullptr);
}
