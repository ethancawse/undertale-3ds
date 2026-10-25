// pixfix, task C: DELTARUNE's stretched battle box keeps its corners on its edges at native. Late code only
// (platform/3ds/late_reserve.ld): nothing here moves existing code or data.
//
// From Chapter 2 on, obj_growtangle draws the battle box with the nine-slice sprites spr_battlebg_stretch (border 2
// texels, insets 2) and spr_battlebg_stretch_hitbox (the Roaring Knight's: border 4, insets 4), at scales such as 2.24
// x 1.76, from an origin of 37 at x 320: the box's corner is at x 237.12, y 104.88. The nine-slice (renderer.h,
// Renderer_drawSpriteNineSlice) draws the four corners at 1:1 and the edges and centre stretched. The pixel grid
// (ctr_renderer.c, gridSnapQuad) moves a 1:1 draw of an even-sized region onto even pixels, up to one pixel left and
// up, so the corners move (237.12 to 236) while the stretched edges, not 1:1, stay: at native each corner shows one
// output pixel up and left of where its edges meet ("the corner pixel looks offset"). The PC frame,
// sampled at native, has the corners where the edges are.
//
// So the corners of these two sprites are drawn with the grid held, as the renderer holds it for quads placed as a
// group: unmoved, they cover the same output pixels as their edges. The hook is the renderer's drawSpritePart, in an
// outer vtable in front of the renderer's own (as ctr_pixgrid.c does), installed after each CtrRenderer_create by
// wrapping CtrRenderer_setSurfaceReaders (CMakeLists.txt, DELTARUNE only). Only 1:1 upright parts of those sprites'
// frames no bigger than their insets are held: the corners, which only the nine-slice draws at 1:1 (a sprite drawn
// at scale 1 goes through drawSprite). Everything else goes straight to the renderer's own entry, which
// setSkipWrappers keeps current. UNDERTALE builds compile this file empty.

#include <stdint.h>
#include <string.h>

#include "common.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"

#ifdef ENABLE_WAD17

#define PIXNS_TEXT __attribute__((noinline, section(".text.late.pixnslice")))
#define PIXNS_CONST __attribute__((aligned(4), section(".rodata.pixnslice_in_text")))
#define PIXNS_MAX 8

// ctr_renderer.c's end: the renderer's own vtable, and the grid hold's offset in CtrRenderer.
extern const void* const PixNslice_refs[1];
extern const uint32_t PixNslice_layout[1];

#define PIXNS_PREFIX 20 // strlen("spr_battlebg_stretch")
static const char pixnsName[24] PIXNS_CONST = "spr_battlebg_stretch";
static const char pixnsHitbox[8] PIXNS_CONST = "_hitbox";
static const char pixnsLog[] PIXNS_CONST = "Native: battle box corners held on their edges (%u items)\n";

__attribute__((section(".bss.late.pixnslice"))) static struct {
    RendererVtable outer;
    RendererVtable* inner; // the renderer's own table (ctrVtable), whose entries setSkipWrappers swaps
    void (*innerInit)(Renderer* renderer, DataWin* dataWin);
    int32_t lo;   // the lowest tpag index in at[], and
    uint32_t span; // the highest minus the lowest; count 0: nothing to hold
    uint32_t count;
    struct {
        int32_t tpag;
        uint16_t w, h; // the largest corner part: the bigger of the side insets
    } at[PIXNS_MAX];
} pixns;

static PIXNS_TEXT void pixnsDrawSpritePart(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY, float srcW, float srcH, float x, float y,
                                           float xscale, float yscale, float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
    if ((uint32_t) (tpagIndex - pixns.lo) <= pixns.span && xscale == 1.0f && yscale == 1.0f && angleDeg == 0.0f) {
        for (uint32_t k = 0; pixns.count > k; k++) {
            if (pixns.at[k].tpag != tpagIndex || srcW > (float) pixns.at[k].w || srcH > (float) pixns.at[k].h) continue;
            int32_t* hold = (int32_t*) ((uint8_t*) renderer + PixNslice_layout[0]);
            (*hold)++;
            pixns.inner->drawSpritePart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
            (*hold)--;
            return;
        }
    }
    pixns.inner->drawSpritePart(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y, xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
}

// The renderer's init, then the two sprites' frames (by name, nine-slice on).
static PIXNS_TEXT void pixnsInit(Renderer* renderer, DataWin* dataWin) {
    pixns.count = 0;
    pixns.innerInit(renderer, dataWin);
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    for (uint32_t i = 0; dataWin->sprt.count > i; i++) {
        const Sprite* sp = &dataWin->sprt.sprites[i];
        // spr_battlebg_stretch and spr_battlebg_stretch_hitbox
        if (sp->name == nullptr || strncmp(sp->name, pixnsName, PIXNS_PREFIX) != 0 ||
            (sp->name[PIXNS_PREFIX] != '\0' && strcmp(sp->name + PIXNS_PREFIX, pixnsHitbox) != 0) || !sp->nineSliceEnabled) {
            continue;
        }
        int32_t w = sp->nsLeft > sp->nsRight ? sp->nsLeft : sp->nsRight, h = sp->nsTop > sp->nsBottom ? sp->nsTop : sp->nsBottom;
        if (0 >= w || 0 >= h || w > 16 || h > 16) continue;
        for (uint32_t f = 0; sp->textureCount > f && PIXNS_MAX > pixns.count; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 > t || dataWin->tpag.count <= (uint32_t) t) continue;
            pixns.at[pixns.count].tpag = t;
            pixns.at[pixns.count].w = (uint16_t) w;
            pixns.at[pixns.count].h = (uint16_t) h;
            pixns.count++;
            if (lo > t) lo = t;
            if (hi < t) hi = t;
        }
    }
    if (pixns.count == 0) return; // the hook's loop then finds nothing
    pixns.lo = lo;
    pixns.span = (uint32_t) (hi - lo);
    logInfo(pixnsLog, (unsigned) pixns.count);
}

// After each CtrRenderer_create: a renderer still on its own table gets the outer one (a later call changes nothing).
static PIXNS_TEXT void pixnsInstall(Renderer* renderer) {
    RendererVtable* inner = (RendererVtable*) PixNslice_refs[0];
    if (renderer->vtable != inner) return;
    pixns.inner = inner;
    pixns.innerInit = inner->init;
    pixns.count = 0;
    pixns.outer = *inner;
    pixns.outer.init = pixnsInit;
    pixns.outer.drawSpritePart = pixnsDrawSpritePart;
    renderer->vtable = &pixns.outer;
}

void __real_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);
void __wrap_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count);

PIXNS_TEXT void __wrap_CtrRenderer_setSurfaceReaders(Renderer* renderer, bool* readers, uint32_t count) {
    __real_CtrRenderer_setSurfaceReaders(renderer, readers, count);
    pixnsInstall(renderer);
}

#endif // ENABLE_WAD17
