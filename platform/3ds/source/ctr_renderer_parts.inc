// Generic uniform-colour, unrotated partial sprites. Included last by ctr_renderer.c.
// Keep only mathematical tile metadata: never retain a texture, residency verdict,
// projection, renderer state, colour or destination position between draws.
#define UTPART_LATE __attribute__((noinline, section(".text.utpart_late")))
#if !defined(CTRPORT_RENDER_TIMING)
#if defined(UTPART_SHARE_UTK) && !defined(ENABLE_WAD17)
#define UTPART_CLIPPED_CACHE 1
// main16's whole-sprite path has this identical mathematical cache already.
// It never retains a position, colour, texture or residency verdict either.
#define partTile utkTile
#define partTileRegion utkTilePart
#else
static struct {
    const CtrPage* page;
    float sx0, sy0, sx1, sy1;
    int32_t tileW, tileH, tilesX, tilesY;
    float uv[4];
} partTile __attribute__((section(".bss.utpart_late")));

static void UTPART_LATE partTileRegion(CtrPage* page, float sx0, float sy0, float sx1, float sy1) {
    partTile.page = page;
    partTile.sx0 = sx0, partTile.sy0 = sy0, partTile.sx1 = sx1, partTile.sy1 = sy1;
    partTile.tileW = page->tileW, partTile.tileH = page->tileH;
    partTile.tilesX = page->tilesX, partTile.tilesY = page->tilesY;
    float tw = (float) page->tileW, th = (float) page->tileH;
    partTile.uv[0] = sx0 / tw, partTile.uv[1] = sy0 / th;
    partTile.uv[2] = sx1 / tw, partTile.uv[3] = sy1 / th;
}
#endif

static inline __attribute__((always_inline)) bool partTileDone(const CtrRenderer* r, const CtrTile* tile) {
    // ensureTilesBut would neither record, promote, request nor stamp anything.
    return tile->lastUsedFrame == r->textures.frame && tile->valid &&
           (0 > r->textures.recordRoom || tile->seenVisit == r->textures.recordVisit);
}

static void UTPART_LATE partEmit(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                               const float* px, const float* py, const CtrColor* cs) {
    // A kept pattern must still see every draw and perform its own grid handling.
    if (0 <= r->boundSurface && r->surfaces[r->boundSurface].pattern.version == r->surfaces[r->boundSurface].version) {
        emitPageRegionVisible(r, page, true, sx0, sy0, sx1, sy1, px, py, cs);
        return;
    }
    float gx[4], gy[4];
    if (gridSnapQuad(r, px, py, sx1 - sx0, sy1 - sy0, gx, gy)) {
        px = gx;
        py = gy;
    }
#if !defined(UTPART_CLIPPED_CACHE)
    // Without an existing clipped-tile cache, keep the stock split-page loop.
    // Fast partial-sprite setup still applies, and no extra clip kernel is linked.
    if (page->tilesX != 1 || page->tilesY != 1) {
        REGION_TILES(r, page, sx0, sy0, sx1, sy1, px, py, cs);
        return;
    }
#endif
    // Bit comparisons also distinguish signed zero in the cached arithmetic.
    if (partTile.page != page || floatBits(partTile.sx0) != floatBits(sx0) || floatBits(partTile.sy0) != floatBits(sy0) ||
        floatBits(partTile.sx1) != floatBits(sx1) || floatBits(partTile.sy1) != floatBits(sy1) ||
        partTile.tileW != page->tileW || partTile.tileH != page->tileH ||
        partTile.tilesX != page->tilesX || partTile.tilesY != page->tilesY) {
        partTileRegion(page, sx0, sy0, sx1, sy1);
    }
#if defined(UTPART_CLIPPED_CACHE)
    if (0 > partTile.tile) {
        if (partTile.tile == -2) REGION_TILES(r, page, sx0, sy0, sx1, sy1, px, py, cs);
        return;
    }
    CtrTile* tile = &page->tiles[partTile.tile];
    int32_t tx = partTile.whole ? 0 : partTile.tx0, ty = partTile.whole ? 0 : partTile.ty0;
    // ensureTiles is exactly ensureTilesBut(..., 0), including whole pages.
    if (!partTileDone(r, tile)) CtrTextures_ensureTilesBut(&r->textures, page, tx, ty, tx, ty, 0);
    if (!tile->valid || (!partTile.whole && !partTile.part)) return;
    float qx[4], qy[4];
    if (!partTile.whole) {
        const float* s = partTile.s;
        quadPosition(px, py, s[0], s[1], &qx[0], &qy[0]);
        quadPosition(px, py, s[2], s[1], &qx[1], &qy[1]);
        quadPosition(px, py, s[2], s[3], &qx[2], &qy[2]);
        quadPosition(px, py, s[0], s[3], &qx[3], &qy[3]);
        px = qx;
        py = qy;
    }
#else
    CtrTile* tile = &page->tiles[0];
    if (!partTileDone(r, tile)) CtrTextures_ensureTilesBut(&r->textures, page, 0, 0, 0, 0, 0);
    if (!tile->valid) return;
#endif
    emitQuad(r, CtrTextures_drawTex(tile), px, py, partTile.uv[0], partTile.uv[1], partTile.uv[2], partTile.uv[3], cs);
    r->palNextB = nullptr;
}

#endif

// drawcut is UNDERTALE only (ENABLE_WAD16; review decision, 10-31): DELTARUNE keeps the stock cull and finite test below,
// byte for byte, until its own placement and draw timings are measured (park-drawcut-dr).
#if !defined(CTRPORT_RENDER_TIMING) && defined(ENABLE_WAD16)
#define UTPART_DRAWCUT 1
// quadOutsideView, for partOutside: partOutsideKeep calls it with s0-s11 saved around the call, so partDrawSprite keeps
// its arguments (which its full way passes on) in registers instead of storing and reloading all twelve around it.
static bool UTPART_LATE __attribute__((used, noclone)) partOutsideAsk(CtrRenderer* r, const float* px, const float* py) {
    if (!r->gameProjection) __builtin_unreachable();  // partOutside's own first test
    return quadOutsideView(r, px, py);
}

// Its 8 B tail is never run: a pad that makes .text.utpart_late grow by whole cache lines (128 B), so the late code
// after it keeps its cache-line phase and every phase pad stays as it was. (An alignment would raise the section's own
// alignment and move its start on UNDERTALE New.)
static void __attribute__((naked, noinline, used, section(".text.utpart_late"))) partOutsideKeep(void) {
    __asm__("push {r4, lr}\n\tvpush {d0-d5}\n\tbl partOutsideAsk\n\tvpop {d0-d5}\n\tpop {r4, pc}\n\t.space 8");
}

// quadOutsideView for partDrawSprite's rectangle (corners 0 and 3 share their x, 0 and 1 their y, by construction).
// Under a projection with no rotation or perspective, drawn for one eye, each corner's x depends on its x alone, its y
// on its y alone, and its w is the matrix's own: two x and two y decide it, with quadOutsideView's sums but for the
// sign of a zero term. Both eyes, any other projection and a corner that isn't finite ask quadOutsideView. Every
// tile of a room and every draw_sprite_part comes here; the general test costs about four times this one.
static inline __attribute__((always_inline)) bool partOutside(CtrRenderer* r, const float* px, const float* py) {
    if (!r->gameProjection) return false;
    const C3D_Mtx* m = &r->projection;
    float w = m->r[3].w;
    float x0 = m->r[0].x * px[0] + m->r[0].w, x1 = m->r[0].x * px[1] + m->r[0].w;
    float y0 = m->r[1].y * py[0] + m->r[1].w, y1 = m->r[1].y * py[2] + m->r[1].w;
    // Finite x0..y1 and w mean finite corners and matrix terms, so each zero term is a signed zero.
    float sum = x0 + x1 + y0 + y1 + w;
    if ((floatBits(m->r[0].y) | floatBits(m->r[1].x) | floatBits(m->r[3].x) | floatBits(m->r[3].y)) << 1 != 0 ||
        !(sum - sum == 0.0f) || drawingBothEyes(r)) {
        register uint32_t a0 __asm__("r0") = (uint32_t) r;
        register const float* a1 __asm__("r1") = px;
        register const float* a2 __asm__("r2") = py;
        __asm__ volatile("bl partOutsideKeep" : "+r"(a0), "+r"(a1), "+r"(a2) : : "r3", "ip", "lr", "cc", "memory", "s12", "s13",
                         "s14", "s15");
        return a0 != 0;
    }
    if (0.0f >= w) return false;
    // With w > 0 and all finite, -w > x exactly when x + w rounds to a negative number (never -0), and x > w when
    // w - x does: two sign bits per edge, and no compare.
    uint32_t left = floatBits(x0 + w) & floatBits(x1 + w), right = floatBits(w - x0) & floatBits(w - x1);
    uint32_t top = floatBits(y0 + w) & floatBits(y1 + w), bottom = floatBits(w - y0) & floatBits(w - y1);
    return (int32_t) (left | right | top | bottom) < 0;
}
#endif

static void UTPART_LATE partDrawSprite(Renderer* renderer, int32_t tpagIndex, float srcOffX, float srcOffY,
                                     float srcW, float srcH, float x, float y, float xscale, float yscale,
                                     float angleDeg, float pivotX, float pivotY, uint32_t color, float alpha) {
#if !defined(CTRPORT_RENDER_TIMING)
    CtrRenderer* r = (CtrRenderer*) renderer;
    DataWin* dw = renderer->dataWin;
    if (angleDeg != 0.0f || r->shaderFx != SHADER_FX_NONE || r->palKind != PAL_NONE || r->smart.emitting ||
        r->magnifier.active || recordingSurface(r) != nullptr || (uint32_t) tpagIndex >= r->originalTpagCount ||
        (uint32_t) tpagIndex >= dw->tpag.count) goto full;
    // This is smartSpriteFine's early, side-effect-free negative answer. Unknown
    // items must run the classifier, even when neighbouring draws used the cache.
    uint8_t known = r->smart.itemState != nullptr ? r->smart.itemState[tpagIndex] : 0;
    if (r->smart.active && !(0.0f >= alpha || 0.0f >= srcW || 0.0f >= srcH || (known != 0 && !(known & ITEM_FINE)))) goto full;
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t pageId = tpag->texturePageId;
    if (0 > pageId || (uint32_t) pageId >= r->textures.pageCount) goto full;
    CtrPage* page = &r->textures.pages[pageId];
    float px[4] = { x, x + srcW * xscale, x + srcW * xscale, x };
    float py[4] = { y, y, y + srcH * yscale, y + srcH * yscale };
#if defined(UTPART_DRAWCUT)
    if (partOutside(r, px, py)) {
#else
    if (quadOutsideView(r, px, py)) {
#endif
        page->lastUsedFrame = r->textures.frame;
        return;
    }
    if (!page->layoutKnown) goto full;
    float sx0 = tpag->sourceX + srcOffX, sy0 = tpag->sourceY + srcOffY;
    float sx1 = sx0 + srcW, sy1 = sy0 + srcH;
    // Avoid caching non-finite geometry; the full renderer owns that behaviour.
#if defined(UTPART_DRAWCUT)
    // A finite sum means four finite corners (a sum too large for a float goes the full way too).
    float sum = sx0 + sy0 + sx1 + sy1;
    if (!(sum - sum == 0.0f)) goto full;
#else
    if (!isfinite(sx0) || !isfinite(sy0) || !isfinite(sx1) || !isfinite(sy1)) goto full;
#endif
    page->lastUsedFrame = r->textures.frame;
    CtrColor c = makeColor(color, alpha);
    CtrColor cs[4] = { c, c, c, c };
    partEmit(r, page, sx0, sy0, sx1, sy1, px, py, cs);
    return;
full:
#endif
    E5X_OPAQUE(ctrDrawSpritePart)(renderer, tpagIndex, srcOffX, srcOffY, srcW, srcH, x, y,
                                xscale, yscale, angleDeg, pivotX, pivotY, color, alpha);
}
#undef UTPART_LATE
#undef UTPART_CLIPPED_CACHE
#undef UTPART_DRAWCUT
