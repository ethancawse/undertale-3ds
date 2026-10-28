// pixfix, task D: DELTARUNE's "HP" label (spr_hpname) shows one shape in every battle at native drawing. Late
// code only (platform/3ds/late_reserve.ld): nothing here moves existing code or data, and nothing runs per draw.
//
// The label (16x11 in Chapters 1, 2 and 5, 16x8 in 3 and 4, the same texels) is white strokes on a black box: the
// strokes are in 2x columns starting at column 1 (1-2, 5-6, 9-14), except the P's bowl, whose right edge is one texel
// (column 13 in rows 0 and 4, column 14 in rows 1-3). Columns 0 and 15 are black, and the 16x11 copies end in three
// transparent rows. It is drawn 1:1 in the 640x480 battle view, so at native drawing each output pixel shows the first
// texel of its 2x2 block of surface pixels. Which texel columns those are depends on where the label lands:
// - the 16x8 copies (even both ways) go onto the pixel grid (ctr_renderer.c, gridSnapQuad: art in 2x pixels goes to
//   even pixels) at x 322 in Chapters 3 and 4: even columns show, and the P loses its top-right corner (the
//   "notched P");
// - the 16x11 copies (odd height) stay where they are drawn, x 321 in Chapters 1 and 2: odd columns, a whole P.
// (scr_charbox draws the labels at xx + 109 + xchunk, and xchunk differs between chapters.)
//
// The fix gives every copy the same even size with its first column odd: each frame's item is narrowed to the
// label's 14x8 core (columns 1-14, rows 0-7), and its target moved one column right to match, so every texel stays
// where it was drawn. The core is even both ways, so it always goes onto the grid, with column 1 on an even pixel: the
// odd columns show (the whole P), wherever the label is drawn. The rows are even ones, as the 16x8 copies already
// showed. What goes is the box's black edge columns and the empty rows; the label is drawn on the battle panel's
// black, so they showed black on black (the HP bar starts two output pixels further right). Every other mode draws
// the same texels at the same places, less those.
//
// An item is narrowed only if it is laid out as the shipped data has it (16 wide, 8 or 11 tall, at the target's
// origin, unscaled); anything else stays as it is. Smart smoothing still keeps the label sharp (smartKeepSharp knows
// it by its item index). The items are changed in the loaded data, once, when the renderer is initialised with it:
// CtrRenderer_getDisplayCaps is wrapped (CMakeLists.txt, DELTARUNE only; UNDERTALE's builds wrap it for view_snap,
// ctr_uthud_snap.c), and main.c calls it right after each CtrRenderer_create, before the runner initialises the
// renderer with the data; the wrapper points the vtable's init at a hook that runs the original and then narrows the
// items. UNDERTALE's label (its own spr_hpname, drawn at a fixed (220, 400)) never changes position, so UNDERTALE
// builds leave this file empty.

#include <stdint.h>
#include <string.h>

#include "common.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "renderer.h"
#include "ctr_wrap_chain.h"
#ifdef ENABLE_WAD17

#define PIXFIX_TEXT __attribute__((noinline, section(".text.late.pixfix")))

static const char pixfixHpname[12] __attribute__((aligned(4), section(".rodata.late.pixfix"))) = "spr_hpname";

__attribute__((section(".bss.late.pixfix"))) static void (*pixfixInit)(Renderer* renderer, DataWin* dataWin);

static PIXFIX_TEXT void pixfixInitHook(Renderer* renderer, DataWin* dataWin) {
    pixfixInit(renderer, dataWin);
    for (uint32_t i = 0; dataWin->sprt.count > i; i++) {
        const Sprite* sp = &dataWin->sprt.sprites[i];
        if (sp->name == nullptr || strcmp(sp->name, pixfixHpname) != 0) continue;
        for (uint32_t f = 0; sp->textureCount > f; f++) {
            int32_t t = sp->tpagIndices[f];
            if (0 > t || dataWin->tpag.count <= (uint32_t) t) continue;
            TexturePageItem* item = &dataWin->tpag.items[t];
            if (item->sourceWidth != 16 || (item->sourceHeight != 8 && item->sourceHeight != 11) || item->targetX != 0 || item->targetY != 0 ||
                item->targetWidth != 16 || item->targetHeight != item->sourceHeight || 0 > item->texturePageId) {
                continue;
            }
            item->sourceX += 1;
            item->targetX = 1;
            item->sourceWidth = item->targetWidth = 14;
            item->sourceHeight = item->targetHeight = 8;
        }
    }
}

uint32_t CTR_DISPLAYCAPS_AFTER_PIXFIX(Renderer* renderer);
uint32_t __wrap_CtrRenderer_getDisplayCaps(Renderer* renderer);

// CtrRenderer_create fills its static vtable again each time it runs; the hook goes back in whenever it isn't there.
PIXFIX_TEXT uint32_t __wrap_CtrRenderer_getDisplayCaps(Renderer* renderer) {
    RendererVtable* vtable = renderer->vtable;
    if (vtable->init != pixfixInitHook) {
        pixfixInit = vtable->init;
        vtable->init = pixfixInitHook;
    }
    return CTR_DISPLAYCAPS_AFTER_PIXFIX(renderer);
}

#endif // ENABLE_WAD17
