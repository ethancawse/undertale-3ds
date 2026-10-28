// UNDERTALE's waving sprite rows, drawn in one call (utkill; included at the end of ctr_renderer.c, UNDERTALE
// only). engine/src/z_utkill_waver.c works out every row of one waving-rows script call (top, x, y) and hands them here.
// Each row is drawn as draw_sprite_part_ext would draw it: Renderer_drawSpritePartExt's clip, e5xDrawSpritePart's test,
// then ctrDrawSpritePartColor's work for an unturned sprite on a surface's page. What is the same for every row of the
// call is done once: the vtable's entry, the sprite's page item, e5xDrawSpritePart's test, the colour, and Smart's
// verdict (below). All of it is late code (.text.late.utkill.waverbatch), so nothing that existed moves; nothing here
// is inlined into an existing function.
//
// Answers false, drawing nothing, when the rows must go one by one through the vtable: the draw-hash wrapper (or any
// other) is in it, or the sprite is on a data page (ctrDrawSpritePartColor's direct path, which the rows don't take).
//
// ctrDrawSpritePartColor for one row, angle 0, the item on a surface's page (texturePageId < 0), and what is left of it:
// - its direct test: angle 0 and an unshaded draw call recordingSurface (kept, per row: dlgRedirecting caches its
//   answer by instance); the page id is negative, so not direct.
// - acquireTpagPage, per row (utkbAcquire, below); none: the row draws nothing.
// - the four colours: one colour and alpha for the call, so worked out once.
// - smartSpriteFine: false for every row, with nothing changed. Past its first tests it answers false when
//   smartTexelSide (no side effect) is 0.99 or more, else when smartItemState is not SMART_FINE; smartItemState is
//   itemVerdict's, which answers 0 at once for a negative texturePageId (before its calloc). With linear false there is
//   no asgBegin/asgEnd.
// - emitPageRegion with the same arguments (kept, per row).

#define UTKW_BATCH_LATE __attribute__((noinline, section(".text.late.utkill.waverbatch")))

#if defined(UTKB_PROF)
// Measurement builds only (never committed on): per-row ticks round the acquire and the emit.
static uint64_t utkbAcq, utkbEmit, utkbTick;
static uint32_t utkbCalls, utkbRows, utkbRecorded, utkbOutside, utkbNullPage;
static void utkbReport(CtrRenderer* r, int32_t tpagIndex) UTKW_BATCH_LATE;
static void utkbReport(CtrRenderer* r, int32_t tpagIndex) {
    if (utkbTick == 0) {
        uint64_t a = svcGetSystemTick();
        utkbTick = svcGetSystemTick() - a + 1;
    }
    if (++utkbCalls % 300 != 0) return;
    double us = 1e6 / 268111856.0, n = (double) utkbRows, tk = (double) utkbTick;
    TexturePageItem* tpag = &r->base.dataWin->tpag.items[tpagIndex];
    CtrPage* page = acquireTpagPage(r, tpagIndex, &tpag);
    int32_t slot = -2 - tpag->texturePageId;
    int32_t sid = 0 <= slot && arrlen(r->surfaceSprites) > slot && r->surfaceSprites[slot] != nullptr ? r->surfaceSprites[slot]->surfaceId : -99;
    logInfo("utkill waverbatch prof: %u calls, %u rows | per row: acquire %.2f us, emit %.2f us (tick read %.2f us) | recorded %u, "
            "outside %u, null page %u | fx %d pal %d tiles %dx%d grid %d eyes %d smart %d copy surface %d\n",
            (unsigned) utkbCalls, (unsigned) utkbRows, ((double) utkbAcq / n - tk) * us, ((double) utkbEmit / n - tk) * us, tk * us,
            (unsigned) utkbRecorded, (unsigned) utkbOutside, (unsigned) utkbNullPage, (int) r->shaderFx, (int) r->palKind,
            page != nullptr ? (int) page->tilesX : -1, page != nullptr ? (int) page->tilesY : -1, (int) gridActive(r),
            (int) drawingBothEyes(r), (int) r->smart.active, (int) sid);
}
#endif

#if defined(UTKB_SHADOW)
// Test builds only (never committed on).
static uint32_t utkbsCalls, utkbsRows, utkbsBad;
static float utkbsRec[6];
static int32_t utkbsRecTpag, utkbsRecCount;
static void utkbsRecord(Renderer* renderer, int32_t tpagIndex, float l, float t, float w, float h, float x, float y, float xs,
                        float ys, float a, float px, float py, uint32_t c, float al) {
    (void) renderer, (void) xs, (void) ys, (void) a, (void) px, (void) py, (void) c, (void) al;
    utkbsRecTpag = tpagIndex;
    utkbsRec[0] = l, utkbsRec[1] = t, utkbsRec[2] = w, utkbsRec[3] = h, utkbsRec[4] = x, utkbsRec[5] = y;
    utkbsRecCount++;
}
#endif

// acquireTpagPage for a row, its work done here when nothing in surfaceTexture would act: the surface sprite's surface
// is in use, neither recorded nor windowed, has no skipped draws (skipRec), no lazy draws, isn't parked, spilled or
// the one being parked (cancelPark acts only on r->park.id), was touched this frame (touchSurface's write is the same
// value), doesn't need clearing in the frame, and needs no split (drawn since one while another is bound). Then
// surfaceTexture only hands back &s->tex, and what is left is surfaceSpritePage's own writes and the item. Otherwise
// the real acquireTpagPage. The surface is read here directly (getSurface gains no caller).
static CtrPage* utkbAcquire(CtrRenderer* r, int32_t tpagIndex, TexturePageItem** outTpag) UTKW_BATCH_LATE;
static CtrPage* utkbAcquire(CtrRenderer* r, int32_t tpagIndex, TexturePageItem** outTpag) {
    DataWin* dw = r->base.dataWin;
    if (0 > tpagIndex || dw->tpag.count <= (uint32_t) tpagIndex) return acquireTpagPage(r, tpagIndex, outTpag);
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    int32_t slot = -2 - tpag->texturePageId;
    if (0 <= tpag->texturePageId || 0 > slot || arrlen(r->surfaceSprites) <= slot || r->surfaceSprites[slot] == nullptr)
        return acquireTpagPage(r, tpagIndex, outTpag);
    CtrSurfaceSprite* ss = r->surfaceSprites[slot];
    int32_t id = ss->surfaceId;
    if (0 > id || r->surfaceCount <= (uint32_t) id) return acquireTpagPage(r, tpagIndex, outTpag);
    CtrSurface* sf = &r->surfaces[id];
    if (!sf->used || sf->recorded || sf->windowed || sf->skipRec != nullptr || sf->lazy != nullptr || sf->parked != nullptr ||
        sf->spilled || r->park.id == id || sf->touchedFrame != r->frame || (sf->needsClear && r->inFrame) ||
        (sf->drawnSinceSplit && r->boundSurface != id))
        return acquireTpagPage(r, tpagIndex, outTpag);
    // surfaceSpritePage's writes, from surfaceTexture's answer &sf->tex.
    C3D_Tex* tex = &sf->tex;
    ss->tile.tex = *tex;
    ss->tile.valid = true;
    ss->page.width = tex->width << ss->shift;
    ss->page.height = tex->height << ss->shift;
    ss->page.tileW = ss->page.width;
    ss->page.tileH = ss->page.height;
    ss->page.tilesX = 1;
    ss->page.tilesY = 1;
    ss->page.tiles = &ss->tile;
    *outTpag = tpag;
#if defined(UTKB_SHADOW)
    {
        // The real acquire after it changes nothing more and answers the same.
        static uint32_t fast;
        static unsigned char s0[sizeof(CtrSurface)], ss0[sizeof(CtrSurfaceSprite)], park0[sizeof(r->park)];
        memcpy(s0, sf, sizeof s0);
        memcpy(ss0, ss, sizeof ss0);
        memcpy(park0, &r->park, sizeof park0);
        int32_t bound0 = r->boundSurface;
        TexturePageItem* t2 = nullptr;
        CtrPage* p2 = acquireTpagPage(r, tpagIndex, &t2);
        bool same = p2 == &ss->page && t2 == tpag && memcmp(s0, sf, sizeof s0) == 0 && memcmp(ss0, ss, sizeof ss0) == 0 &&
                    memcmp(park0, &r->park, sizeof park0) == 0 && bound0 == r->boundSurface;
        if (!same) logInfo("utkill waverbatch shadow: ACQUIRE MISMATCH (fast %u)\n", (unsigned) fast);
        if (++fast % 100000 == 0) logInfo("utkill waverbatch shadow: %u fast acquires\n", (unsigned) fast);
    }
#endif
    return &ss->page;
}

bool CtrUtk_waverRows(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float left, float width, uint32_t colour,
                      float alpha, int32_t n, const float* tops, const float* xs, const float* ys) UTKW_BATCH_LATE;
bool CtrUtk_waverRows(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float left, float width, uint32_t colour,
                      float alpha, int32_t n, const float* tops, const float* xs, const float* ys) {
    if (renderer->vtable->drawSpritePart != e5xDrawSpritePart) return false;
    DataWin* dw = renderer->dataWin;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, subimg);
    if (0 > tpagIndex) return true;   // every row's draw returns there
    TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    if (0 <= tpag->texturePageId) return false;
    if (makeColor(0, alpha).a == 0 && e5xNoop(renderer)) return true;
    CtrRenderer* r = (CtrRenderer*) renderer;
    RENDER_TIMED(r, TIME_DRAW);
    CtrColor c = makeColor(colour, alpha);
    CtrColor cs[4] = { c, c, c, c };
    for (int32_t k = 0; n > k; k++) {
        // Renderer_drawSpritePartExt's clip, with xscale = yscale = 1 and height 1.
        float l = left, t = tops[k], w = width, h = 1.0f, x = xs[k], y = ys[k];
        const float xscale = 1.0f, yscale = 1.0f;
        if (tpag->targetX > l) {
            float off = tpag->targetX - l;
            x += (float) off * xscale;
            w -= off;
            l = 0;
        } else {
            l -= tpag->targetX;
        }
        if (tpag->targetY > t) {
            float off = tpag->targetY - t;
            y += (float) off * yscale;
            h -= off;
            t = 0;
        } else {
            t -= tpag->targetY;
        }
        if (w > tpag->sourceWidth - l) w = tpag->sourceWidth - l;
        if (h > tpag->sourceHeight - t) h = tpag->sourceHeight - t;
#if defined(UTKB_SHADOW)
        {
            // The one-by-one path's own code (renderer.h's inline) into a recorder in a vtable copy.
            RendererVtable copy = *renderer->vtable;
            const RendererVtable* real = renderer->vtable;
            copy.drawSpritePart = utkbsRecord;
            renderer->vtable = &copy;
            utkbsRecCount = 0;
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, left, tops[k], width, 1.0f, xs[k], ys[k], 1.0f, 1.0f, 0.0f,
                                       0.0f, 0.0f, colour, alpha);
            renderer->vtable = (RendererVtable*) real;
            bool drawn = 0 < w && 0 < h;
            float mine[6] = { l, t, w, h, x, y };
            bool same = real->drawSpritePart == e5xDrawSpritePart && utkbsRecCount == (drawn ? 1 : 0) &&
                        (!drawn || (utkbsRecTpag == tpagIndex && memcmp(mine, utkbsRec, sizeof mine) == 0)) &&
                        Renderer_resolveTPAGIndex(dw, spriteIndex, subimg) == tpagIndex && !(makeColor(0, alpha).a == 0 && e5xNoop(renderer));
            if (drawn) {
                // Smart's verdict as ctrDrawSpritePartColor would ask it, and the colours.
                float qx[4] = { x, x + w * xscale, x + w * xscale, x };
                float qy[4] = { y, y, y + h * yscale, y + h * yscale };
                TexturePageItem* qt;
                CtrPage* qp = acquireTpagPage(r, tpagIndex, &qt);
                CtrColor qc = makeColor(colour, alpha);
                same = same && 0 > qt->texturePageId && memcmp(&qc, &c, sizeof c) == 0 &&
                       (qp == nullptr || !smartSpriteFine(r, tpagIndex, qt, qp, qx, qy, l, t, w, h, alpha));
            }
            utkbsRows++;
            if (!same) {
                utkbsBad++;
                logInfo("utkill waverbatch shadow: MISMATCH (call %u, row %d)\n", (unsigned) utkbsCalls, (int) k);
            }
        }
#endif
        if (0 >= w || 0 >= h) continue;
        // ctrDrawSpritePartColor (see the top of this file).
        float px[4] = { x, x + w * xscale, x + w * xscale, x };
        float py[4] = { y, y, y + h * yscale, y + h * yscale };
        if (r->shaderFx == SHADER_FX_NONE || r->fxSuspended) (void) recordingSurface(r);
#if defined(UTKB_PROF)
        uint64_t t0 = svcGetSystemTick();
#endif
        TexturePageItem* rowTpag;
        CtrPage* page = utkbAcquire(r, tpagIndex, &rowTpag);
#if defined(UTKB_PROF)
        uint64_t t1 = svcGetSystemTick();
        utkbAcq += t1 - t0;
        utkbRows++;
        if (page == nullptr) utkbNullPage++;
        else if (recordedPageSurface(r, page) != nullptr) utkbRecorded++;
        if (quadOutsideView(r, px, py)) utkbOutside++;
#endif
        if (page == nullptr) continue;
        float sx0 = rowTpag->sourceX + l;
        float sy0 = rowTpag->sourceY + t;
#if defined(UTKB_PROF)
        uint64_t t2 = svcGetSystemTick();
#endif
        emitPageRegion(r, page, sx0, sy0, sx0 + w, sy0 + h, px, py, cs);
#if defined(UTKB_PROF)
        utkbEmit += svcGetSystemTick() - t2;
#endif
    }
#if defined(UTKB_PROF)
    utkbReport(r, tpagIndex);
#endif
#if defined(UTKB_SHADOW)
    if (++utkbsCalls % 300 == 0)
        logInfo("utkill waverbatch shadow: %u calls, %u rows, %u mismatches\n", (unsigned) utkbsCalls, (unsigned) utkbsRows,
                (unsigned) utkbsBad);
#endif
    return true;
}
