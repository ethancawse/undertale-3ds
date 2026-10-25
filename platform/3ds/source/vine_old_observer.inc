#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
/* DELTARUNE New 3DS: exact tower face retention only (flowergpu); Old keeps everything below. */
#include "vine_new_retain.inc"
#else
#line 1
/* Stage 2 of X-F1-Full (Correct Rotating Cylindrical Tower on Old 3DS + Byte-Exact Optimizations).
 * - Structural activation at the first safe point after vineDiscoverTowerSurfaces finds the 24 360x360
 *   layer surfaces and 2 240x720 face surfaces in the current room (no frame numbers, no surface IDs,
 *   no scene classifier; bounded retry up to 4 attempts with atomic rollback; automatic restoration
 *   if a mid-room surface promotion temporarily displaces a face from VRAM).
 * - Scene-independent face retention using a full CtrLazyDraw content signature (layer ID, layer
 *   contentSerial version, source rect, destination quad, all 4 vertex colors, all shd_tower uniforms,
 *   target viewport/scissor/projection matrices, and blend/pipeline state) plus target face version.
 * - (a) Vertical scissor/clipping y in [116, 604) on tower_front and tower_back (~32.2% offscreen skip).
 * - (b) Whole-layer empty skip (layers 0, 1 with cut=254) + empty-strip coverage skip ((m & rowsVis) == 0)
 *       on back-face gradient strips (!colorsEqual(ca, cb)), keeping full [y0, y1] endpoints when non-empty.
 * - (c) 5 cooked 512x368 backings in linear FCRAM (layer 0/1 deduped into S_depth_cooked so all 20 depth layers
 *       0..19 share cookedTex[0], plus cookedTex[1..4] for non-depth layers 20..23, saving 376,832 B FCRAM). */
#define STAGE2_CODE __attribute__((noinline, used, optimize("Os", "inline-functions", "no-tree-switch-conversion"), section(".text.late.perfflowery")))
#define STAGE2_COLD __attribute__((noinline, used, optimize("Os", "inline-functions", "no-tree-switch-conversion"), section(".text.late.perfflowery")))
#define STAGE2_RO   __attribute__((section(".rodata.late.perfflowery")))
#define STAGE2_BSS  __attribute__((section(".bss.late.perfflowery")))
#define VI_CALL(name) ({ __typeof__(&(name)) viFn = &(name); __asm__("" : "+r"(viFn)); viFn; })

STAGE2_CODE bool Vine_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    return CtrArgui_override(settings, line, caps);
}
STAGE2_CODE void Vine_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    VI_CALL(CtrArgui_load)(settings, path, caps);
}

#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS)
#define VINE_MAX_ACT_ATTEMPTS 4u

static bool viStage2Enabled STAGE2_BSS;
static bool viStage2InitDone STAGE2_BSS;
static bool viStage2Active STAGE2_BSS;
static bool viFace1SharedTarget STAGE2_BSS;
static uint32_t viLastRoomStart STAGE2_BSS;
static uint32_t viActAttempts STAGE2_BSS;
static uint32_t viActLastTryFrame STAGE2_BSS;
static bool viFound STAGE2_BSS;
static int32_t viLayerIds[24] STAGE2_BSS;
static int32_t viFaceIds[2] STAGE2_BSS;

static bool viFaceHasPrev[2] STAGE2_BSS;
static bool viFaceDeferClear[2] STAGE2_BSS;
static uint32_t viFaceSavedVersion[2] STAGE2_BSS;
static uint32_t viFacePrevCount[2] STAGE2_BSS;
static uint32_t viFaceCallIdx[2] STAGE2_BSS;
static CtrLazyDraw viFacePrevSigs[2][32] STAGE2_BSS;

#ifdef VINE_STAGE2_DIAG_SELFTEST
static const char fmtDemoteVram[] STAGE2_RO =
    "[vine-stage2] demote_vram bank=%c s=%d (%dx%d) f=%u oldVram=%p bytes=%u newLin=%p span=%u->%u sampleOk=%d rebindTarget=%p(scr=%d)\n";
#else
static const char fmtDemoteVram[] STAGE2_RO =
    "[vine-stage2] demote_vram bank=%c s=%d (%dx%d) f=%u oldVram=%p bytes=%u newLin=%p span=%u->%u\n";
#endif
static const char fmtCookBacking[] STAGE2_RO =
    "[vine-stage2] cooked_backing k=%d owner=%d src=%d addr=%p bytes=%u\n";
static const char fmtRetry[] STAGE2_RO =
    "[vine-stage2] retry f=%u attempt=%u/4 spanA=%u spanB=%u linFree=%u\n";
static const char fmtActivated[] STAGE2_RO =
    "[vine-stage2] activated f=%u attempt=%u/4 face0=%d@%p face1=%d@%p shared=%d spanA=%u->%u spanB=%u->%u linFree=%u->%u\n";
static const char fmtRestored[] STAGE2_RO =
    "[vine-stage2] restored_faces f=%u face0=%d@%p face1=%d@%p spanA=%u spanB=%u\n";

static const int32_t viSrcLayerIdx[5] STAGE2_RO = { 3, 20, 21, 22, 23 };
static const int32_t viDstOwnerIdx[5] STAGE2_RO = { 0, 20, 21, 22, 23 };
static const uint8_t viDepthCuts[20] STAGE2_RO = {
    254, 254, 242, 229, 216, 203, 191, 178, 165, 152,
    140, 127, 114, 101,  89,  76,  63,  50,  38,  25
};
static const CtrRect viScissorFaceRect STAGE2_RO = { 0, 116, 240, 488 };
static const CtrColor viClearZeroColor STAGE2_RO = { 0, 0, 0, 0 };
static const Matrix4f viPixels360 STAGE2_RO = { {
    2.0f / 360.0f, 0.0f, 0.0f, 0.0f,
    0.0f, -2.0f / 360.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    -1.0f, 1.0f, 0.0f, 1.0f
} };

static STAGE2_COLD void vineSuspendFrame(CtrRenderer* r, CtrSuspendedFrame* susp) {
    suspendFrame(r, susp);
    CtrGpu_waitIdle();
}

static STAGE2_COLD void vineRawCopyTex(const C3D_Tex* src, C3D_Tex* dst) {
    GSPGPU_FlushDataCache(dst->data, dst->size);
    C3D_SyncTextureCopy((u32*) src->data, textureCopyDim(src),
                        (u32*) dst->data, textureCopyDim(dst),
                        dst->size, GX_TRANSFER_RAW_COPY(1));
    GSPGPU_InvalidateDataCache(dst->data, dst->size);
}

static STAGE2_COLD bool vineTryVramTarget(C3D_Tex* tex, C3D_RenderTarget** outTarget, int32_t w, int32_t h, int32_t usedRows) {
    memset(tex, 0, sizeof(*tex));
    *outTarget = nullptr;
    return tryVramTarget(tex, outTarget, w, h, usedRows);
}

static STAGE2_COLD bool vineInitSurfaceTex(C3D_Tex* tex, int32_t w, int32_t h, int32_t usedRows) {
    return initSurfaceTex(tex, w, h, usedRows, false);
}

static STAGE2_COLD bool vineIsTowerSurface(int32_t id) {
    for (int i = 0; i < 24; i++) {
        if (viLayerIds[i] == id) return true;
    }
    return (viFaceIds[0] == id || viFaceIds[1] == id);
}

static STAGE2_COLD uint32_t vineBankMaxSpan(int32_t bank) {
    static CtrVramBlock blocks[256] STAGE2_BSS;
    int32_t count = CtrMemory_vramBlocks(blocks, 256);
    if (count < 0) return 0;
    uintptr_t bankBytes = (uintptr_t) OS_VRAM_SIZE / 2u;
    uintptr_t lo = (uintptr_t) OS_VRAM_VADDR + (uintptr_t) bank * bankBytes;
    uintptr_t hi = lo + bankBytes;
    uintptr_t start = lo;
    uint32_t maxFree = 0;
    for (int32_t k = 0; k < count; k++) {
        uintptr_t addr = blocks[k].addr;
        if (addr < lo || addr >= hi) continue;
        if (addr > start && (uint32_t) (addr - start) > maxFree) {
            maxFree = (uint32_t) (addr - start);
        }
        start = addr + blocks[k].size;
    }
    if (hi > start && (uint32_t) (hi - start) > maxFree) {
        maxFree = (uint32_t) (hi - start);
    }
    return maxFree;
}

static STAGE2_COLD bool vineDiscoverTowerSurfaces(CtrRenderer* r) {
    int lCount = 0;
    int fCount = 0;
    for (uint32_t i = 1; i < r->surfaceCount; i++) {
        const CtrSurface* s = &r->surfaces[i];
        if (!s->used) continue;
        if (s->width == 360 && s->height == 360 && lCount < 24) {
            viLayerIds[lCount++] = (int32_t) i;
        } else if (s->width == 240 && s->height == 720 && fCount < 2) {
            viFaceIds[fCount++] = (int32_t) i;
        }
    }
    return (lCount == 24 && fCount == 2 && r->surfaces[viLayerIds[3]].recorded &&
            arrlen(r->surfaces[viLayerIds[3]].ops) > 0);
}

static STAGE2_COLD void vineResetLayerSurface(CtrRenderer* r, int32_t id) {
    CtrSurface* s = &r->surfaces[id];
    cancelPark(r, id);
    if (s->parked != nullptr) {
        freeParked(s);
    }
    if (s->target != nullptr && !s->spilled) {
        C3D_RenderTargetDelete(s->target);
    }
    s->target = nullptr;
    if (s->tex.data != nullptr) {
        C3D_TexDelete(&s->tex);
        memset(&s->tex, 0, sizeof(s->tex));
    }
    if (s->spilled) {
        cancelQueuedStore(r, s);
        if (r->scratches != nullptr && s->scratch >= 0 && s->scratch < arrlen(r->scratches) && r->scratches[s->scratch].owner == id) {
            r->scratches[s->scratch].owner = -1;
            r->scratches[s->scratch].pendingStore = false;
        }
        s->spilled = false;
    }
}

static STAGE2_COLD void vineEnsureVramForFaces(CtrRenderer* r, CtrSuspendedFrame* susp) {
    (void) susp;
    if (!oomVineScratch(r)) return;
#line 181
    uintptr_t bankBytes = (uintptr_t) OS_VRAM_SIZE / 2u;
    int32_t appSurfId = r->base.runner != nullptr ? r->base.runner->applicationSurfaceId : 0;
    for (int step = 0; step < 4; step++) {
        uint32_t spanA = vineBankMaxSpan(0);
        uint32_t spanB = vineBankMaxSpan(1);
        int32_t bank = (spanA < 1250000u) ? 0 : ((spanA < 1490944u && spanB < 745472u) ? 1 : -1);
        if (bank < 0) break;
        uintptr_t lo = (uintptr_t) OS_VRAM_VADDR + (uintptr_t) bank * bankBytes;
        uintptr_t hi = lo + bankBytes;
        int32_t bestId = -1;
        uint32_t bestBytes = 0;
        for (uint32_t id = 1; id < r->surfaceCount; id++) {
            if ((int32_t) id == appSurfId || vineIsTowerSurface((int32_t) id)) continue;
            const CtrSurface* cand = &r->surfaces[id];
            if (!cand->used || cand->spilled || cand->recorded || cand->windowed || cand->target == nullptr || cand->tex.data == nullptr) continue;
            if (cand->boundFrame == r->frame || cand->boundFrames >= 2) continue;
            uintptr_t addr = (uintptr_t) cand->tex.data;
            if (addr >= lo && addr < hi && cand->tex.size >= 400000u && cand->tex.size > bestBytes) {
                bestBytes = cand->tex.size;
                bestId = (int32_t) id;
            }
        }
        if (bestId < 0) break;
        CtrSurface* v = &r->surfaces[bestId];
        uint32_t spanBefore = (bank == 0) ? spanA : spanB;
        void* oldVramAddr = v->tex.data;
        uint32_t oldVramSize = (uint32_t) v->tex.size;
        C3D_Tex linTex;
        if (!initLinearTexture(r, &linTex, v->tex.width, v->tex.height, textureRows(&v->tex))) break;
        if (!v->needsClear) {
            vineRawCopyTex(&v->tex, &linTex);
        } else {
            memset(linTex.data, 0, linTex.size);
            GSPGPU_FlushDataCache(linTex.data, linTex.size);
        }
        if (r->clearedTarget == v->target) r->clearedTarget = nullptr;
        C3D_RenderTargetDelete(v->target);
        C3D_TexDelete(&v->tex);
        v->tex = linTex;
        v->target = nullptr;
        v->spilled = true;
        v->scratch = 0;
        v->needsClear = false;
        v->storeQueued = false;
        v->drawnSinceSplit = false;
        v->touchedFrame = r->frame;
        uint32_t spanAfter = vineBankMaxSpan(bank);

#ifdef VINE_STAGE2_DIAG_SELFTEST
        uint32_t savedBF = v->boundFrame;
        uint32_t savedBFC = v->boundFrames;
        resumeFrame(r, susp);
        int sampleOk = (surfaceTexture(r, bestId) == &v->tex) ? 1 : 0;
        bindSurface(r, bestId);
        C3D_RenderTarget* rebTarget = v->target;
        int32_t rebScr = v->scratch;
        r->spillDirty = false;
        r->spillLoadPending = false;
        bindSurface(r, appSurfId);
        releaseIdleScratches(r, 0);
        v->boundFrame = savedBF;
        v->boundFrames = savedBFC;
        vineSuspendFrame(r, susp);

        logInfo(fmtDemoteVram, (int) ('A' + bank), (int) bestId, (int) v->width, (int) v->height, (unsigned) r->frame,
                oldVramAddr, (unsigned) oldVramSize, linTex.data,
                (unsigned) spanBefore, (unsigned) spanAfter, sampleOk, rebTarget, (int) rebScr);
#else
        logInfo(fmtDemoteVram, (int) ('A' + bank), (int) bestId, (int) v->width, (int) v->height, (unsigned) r->frame,
                oldVramAddr, (unsigned) oldVramSize, linTex.data,
                (unsigned) spanBefore, (unsigned) spanAfter);
#endif
    }
}

static STAGE2_COLD void vineRestoreFaceTargets(CtrRenderer* r) {
    CtrSurface* f0 = &r->surfaces[viFaceIds[0]];
    CtrSurface* f1 = &r->surfaces[viFaceIds[1]];
    if (!f0->spilled && f0->target != nullptr && (viFace1SharedTarget || (!f1->spilled && f1->target != nullptr))) return;

    flushBatch(r);
    releaseIdleScratches(r, 0);

    CtrSuspendedFrame susp;
    vineSuspendFrame(r, &susp);
    vineEnsureVramForFaces(r, &susp);

    for (int fi = 0; fi < 2; fi++) {
        if (fi == 1 && viFace1SharedTarget) continue;
        CtrSurface* fs = &r->surfaces[viFaceIds[fi]];
        if (!fs->spilled && fs->target != nullptr) continue;
        C3D_Tex newTex;
        C3D_RenderTarget* newTarget = nullptr;
        if (vineTryVramTarget(&newTex, &newTarget, 256, 1024, 728)) {
            if (!fs->needsClear && fs->tex.data != nullptr) {
                GSPGPU_FlushDataCache(fs->tex.data, fs->tex.size);
                vineRawCopyTex(&fs->tex, &newTex);
            } else {
                C3D_RenderTargetClear(newTarget, C3D_CLEAR_COLOR, 0, 0);
            }
            if (fs->tex.data != nullptr) C3D_TexDelete(&fs->tex);
            fs->tex = newTex;
            fs->target = newTarget;
            fs->spilled = false;
            fs->scratch = 0;
            fs->boundFrames = 2;
            fs->storeQueued = false;
            fs->drawnSinceSplit = false;
            viFaceHasPrev[fi] = false;
            viFaceDeferClear[fi] = false;
        }
    }
    CtrGpu_waitIdle();
    resumeFrame(r, &susp);
    bindSurface(r, r->base.runner != nullptr ? r->base.runner->applicationSurfaceId : 0);
    setViewport(r, 0, 0, r->appW, r->appH, false);
    applyAllState(r);

    logInfo(fmtRestored, (unsigned) r->frame,
            (!f0->spilled && f0->target != nullptr) ? 1 : 0, f0->tex.data,
            (!f1->spilled && f1->target != nullptr) ? 1 : 0, f1->tex.data,
            (unsigned) vineBankMaxSpan(0), (unsigned) vineBankMaxSpan(1));
}

static STAGE2_COLD void vineActivateStage2(CtrRenderer* r) {
    viActAttempts++;
    uint32_t spanBeforeA = vineBankMaxSpan(0);
    uint32_t spanBeforeB = vineBankMaxSpan(1);
    uint32_t linBefore = (uint32_t) linearSpaceFree();
    int32_t appSurfId = r->base.runner != nullptr ? r->base.runner->applicationSurfaceId : 0;

    flushBatch(r);
    releaseIdleScratches(r, 0);

    CtrSuspendedFrame susp;
    vineSuspendFrame(r, &susp);

    // 1. Release any existing VRAM/FCRAM/parked backing on layers 0..2 first so their VRAM is reclaimed.
    for (int i = 0; i < 3; i++) {
        vineResetLayerSurface(r, viLayerIds[i]);
    }

    // 2. Only demote before cookTex if neither bank can hold the temporary 512x368 (753,664 B) target.
    if (vineBankMaxSpan(0) < 753664u && vineBankMaxSpan(1) < 753664u) {
        vineEnsureVramForFaces(r, &susp);
    }

    // 3. Allocate one temporary 512x368 (753,664 B) VRAM render target to cook the 5 distinct 512x368 backings.
    C3D_Tex cookTex;
    C3D_RenderTarget* cookTarget = nullptr;
    bool cookTargetOk = vineTryVramTarget(&cookTex, &cookTarget, 512, 512, 368);
    resumeFrame(r, &susp);

    if (!cookTargetOk || cookTarget == nullptr) {
        logInfo(fmtRetry, (unsigned) r->frame, (unsigned) viActAttempts,
                (unsigned) vineBankMaxSpan(0), (unsigned) vineBankMaxSpan(1), (unsigned) linearSpaceFree());
        return;
    }

    C3D_Tex cookedTex[5];
    memset(cookedTex, 0, sizeof(cookedTex));
    bool allCookedOk = true;

    Matrix4f savedWvp = r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    bool savedSmart = r->smart.active;
    uint8_t savedCut = r->opAlphaCut;
    bool savedFxSusp = r->fxSuspended;
    r->smart.active = false;
    r->fxSuspended = true;
    r->gridHold++;

    for (int k = 0; k < 5; k++) {
        int32_t srcId = viLayerIds[viSrcLayerIdx[k]];
        CtrSurface* srcLayer = &r->surfaces[srcId];
        bindTarget(r, cookTarget, -1);
        C3D_RenderTargetClear(cookTarget, C3D_CLEAR_COLOR, 0, 0);
        r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = viPixels360;
        r->visible.valid = false;
        materializeState(r, 512, 368);
        if (k == 0) {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        } else {
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
        }
        r->opAlphaCut = 0;
        applyTexEnv(r);
        for (ptrdiff_t i = 0; i < arrlen(srcLayer->ops); i++) {
            const CtrRecOp* op = &srcLayer->ops[i];
            if (op->x[0] >= 360.0f || op->y[0] >= 360.0f) continue;
            CtrPage* page = 0 <= op->pageId ? CtrTextures_acquire(&r->textures, (uint32_t) op->pageId) : surfaceSpritePage(r, (int16_t) op->pageId);
            if (page != nullptr) {
                emitPageRegion(r, page, op->sx0, op->sy0, op->sx1, op->sy1, op->x, op->y, op->c);
            }
        }
        flushBatch(r);

        vineSuspendFrame(r, &susp);
        bool okTex = vineInitSurfaceTex(&cookedTex[k], 512, 512, 368);
        if (okTex) {
            vineRawCopyTex(&cookTex, &cookedTex[k]);
            logInfo(fmtCookBacking, k, (int) viLayerIds[viDstOwnerIdx[k]], (int) srcId,
                    cookedTex[k].data, (unsigned) cookedTex[k].size);
        } else {
            allCookedOk = false;
        }
        if (k < 4) {
            resumeFrame(r, &susp);
        }
    }

    C3D_Tex faceTex[2];
    C3D_RenderTarget* faceTarget[2] = { nullptr, nullptr };
    memset(faceTex, 0, sizeof(faceTex));
    bool faceOk[2] = { false, false };
    viFace1SharedTarget = false;

    // Place faceTex[1] while cookTex still holds Bank A so faceTex[1] prefers Bank B's free span.
    if (allCookedOk) {
        faceOk[1] = vineTryVramTarget(&faceTex[1], &faceTarget[1], 256, 1024, 728);
    }

    // Delete the temporary 512x368 VRAM target and demote any unwritten/idle surface before allocating faceTex[0].
    C3D_RenderTargetDelete(cookTarget);
    C3D_TexDelete(&cookTex);
    r->clearedTarget = nullptr;
    vineEnsureVramForFaces(r, &susp);

    if (allCookedOk) {
        faceOk[0] = vineTryVramTarget(&faceTex[0], &faceTarget[0], 256, 1024, 728);
        if (faceOk[0] && !faceOk[1]) {
            faceOk[1] = vineTryVramTarget(&faceTex[1], &faceTarget[1], 256, 1024, 728);
            if (!faceOk[1]) {
                if (vineInitSurfaceTex(&faceTex[1], 256, 1024, 728)) {
                    faceTarget[1] = faceTarget[0];
                    faceOk[1] = true;
                    viFace1SharedTarget = true;
                }
            }
        }
    }

    bool commitOk = allCookedOk && faceOk[0] && faceOk[1];
    uint32_t spanAfterFacesA = 0;
    uint32_t spanAfterFacesB = 0;

    if (!commitOk) {
        if (faceTarget[1] != nullptr && !viFace1SharedTarget) C3D_RenderTargetDelete(faceTarget[1]);
        if (faceTex[1].data != nullptr) C3D_TexDelete(&faceTex[1]);
        if (faceTarget[0] != nullptr) C3D_RenderTargetDelete(faceTarget[0]);
        if (faceTex[0].data != nullptr) C3D_TexDelete(&faceTex[0]);
        for (int k = 0; k < 5; k++) {
            if (cookedTex[k].data != nullptr) C3D_TexDelete(&cookedTex[k]);
        }
        viStage2Active = false;
    } else {
        for (int fi = 0; fi < 2; fi++) {
            int32_t fid = viFaceIds[fi];
            CtrSurface* fs = &r->surfaces[fid];
            if (fs->recorded) {
                oomVineDropOps(fs);
#line 442
            }
            fs->tex = faceTex[fi];
            fs->target = faceTarget[fi];
            fs->recorded = false;
            fs->noVram = false;
            fs->spilled = false;
            fs->wantNative = false;
            fs->shift = 0;
            fs->nativeScale = false;
            fs->needsClear = false;
            fs->drawnSinceSplit = false;
            fs->boundFrames = 2;
            fs->seenFrame = r->frame;
            fs->touchedFrame = r->frame;
            viFaceHasPrev[fi] = false;
            viFaceDeferClear[fi] = false;
            if (fi == 0 || !viFace1SharedTarget) {
                C3D_RenderTargetClear(fs->target, C3D_CLEAR_COLOR, 0, 0);
            } else {
                memset(fs->tex.data, 0, fs->tex.size);
                GSPGPU_FlushDataCache(fs->tex.data, fs->tex.size);
            }
        }
        CtrGpu_waitIdle();
        spanAfterFacesA = vineBankMaxSpan(0);
        spanAfterFacesB = vineBankMaxSpan(1);

        for (int i = 0; i < 24; i++) {
            int32_t lid = viLayerIds[i];
            CtrSurface* ls = &r->surfaces[lid];
            int cookIdx = (i < 20) ? 0 : (i - 19);
            bool isOwner = (i == 0 || i >= 20);
            if (i >= 3) vineResetLayerSurface(r, lid);
            oomVineDropOps(ls);
#line 477
            ls->tex = cookedTex[cookIdx];
            ls->target = nullptr;
            ls->spilled = true;
            ls->scratch = 0;
            ls->storeQueued = false;
            ls->drawnSinceSplit = false;
            ls->needsClear = false;
            ls->noVram = false;
            ls->wantNative = false;
            ls->shift = 0;
            ls->nativeScale = false;
            ls->recorded = !isOwner;
            ls->touchedFrame = r->frame;
            if (ls->version == 0) {
                ls->version = ++r->contentSerial;
            }
            memset(&ls->pattern, 0, sizeof(ls->pattern));
            ls->pattern.version = ls->version;
            if (i < 20) {
                ls->pattern.pageId = 50;
                ls->pattern.sx = 186;
                ls->pattern.sy = 1518;
                ls->pattern.w = 180;
                ls->pattern.h = 180;
                ls->pattern.ox = 0;
                ls->pattern.oy = 0;
                ls->pattern.cut = viDepthCuts[i];
                ls->pattern.cells = 0x70707u;
            } else {
                ls->pattern.pageId = -1;
            }
        }
        viStage2Active = true;
    }

    resumeFrame(r, &susp);
    r->gridHold--;
    r->smart.active = savedSmart;
    r->fxSuspended = savedFxSusp;
    r->opAlphaCut = savedCut;
    applyTexEnv(r);
    r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = savedWvp;
    r->visible.valid = false;
    bindSurface(r, appSurfId);
    setViewport(r, 0, 0, r->appW, r->appH, false);
    applyAllState(r);

    if (!commitOk) {
        logInfo(fmtRetry, (unsigned) r->frame, (unsigned) viActAttempts,
                (unsigned) vineBankMaxSpan(0), (unsigned) vineBankMaxSpan(1), (unsigned) linearSpaceFree());
    } else {
        logInfo(fmtActivated, (unsigned) r->frame, (unsigned) viActAttempts,
                faceOk[0] ? 1 : 0, r->surfaces[viFaceIds[0]].tex.data,
                faceOk[1] ? 1 : 0, r->surfaces[viFaceIds[1]].tex.data,
                viFace1SharedTarget ? 1 : 0,
                (unsigned) spanBeforeA, (unsigned) spanAfterFacesA,
                (unsigned) spanBeforeB, (unsigned) spanAfterFacesB,
                (unsigned) linBefore, (unsigned) linearSpaceFree());
    }
}

static STAGE2_CODE int32_t vineFindLayerIndex(int32_t surfaceId) {
    for (int i = 0; i < 24; i++) {
        if (viLayerIds[i] == surfaceId) return i;
    }
    return -1;
}

static STAGE2_CODE bool vineExecuteLayerCall(CtrRenderer* r, int32_t li, int32_t surfaceId, CtrSurface* s,
                                             int32_t srcLeft, int32_t srcTop, int32_t srcWidth, int32_t srcHeight,
                                             const float* px, const float* py, const CtrColor* cs, const TowerUniforms* uni) {
    r->opAlphaCut = (li < 20) ? s->pattern.cut : 0;
    bool wasRec = s->recorded;
    s->recorded = false;
    bool clipFaceY = (r->boundSurface == viFaceIds[0] || r->boundSurface == viFaceIds[1]) && !r->scissorEnabled &&
                     r->boundTarget != nullptr && r->boundTarget->frameBuf.height == 728;
    if (clipFaceY) {
        r->scissorEnabled = true;
        r->scissor = viScissorFaceRect;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 728u - 604u, 240u, 728u - 116u);
        r->visible.valid = true;
        r->visible.result = true;
        r->visible.x0 = -10.0f;
        r->visible.x1 = 250.0f;
        r->visible.y0 = 116.0f;
        r->visible.y1 = 604.0f;
    }
    bool res = VI_CALL(drawTowerStrips)(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
    if (clipFaceY) {
        r->scissorEnabled = false;
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        r->visible.valid = false;
    }
    r->opAlphaCut = 0;
    s->recorded = wasRec;
    return res;
}

STAGE2_CODE void Vine_wrap_clearBoundTarget(CtrRenderer* r, CtrColor color, int32_t targetW, int32_t targetH) {
    if (viStage2Active && (r->boundSurface == viFaceIds[0] || r->boundSurface == viFaceIds[1])) {
        int fi = (r->boundSurface == viFaceIds[0]) ? 0 : 1;
        if (viFace1SharedTarget && fi == 0 && viFaceHasPrev[1]) {
            CtrSurface* fs0 = &r->surfaces[viFaceIds[0]];
            CtrSurface* fs1 = &r->surfaces[viFaceIds[1]];
            CtrSuspendedFrame susp;
            vineSuspendFrame(r, &susp);
            vineRawCopyTex(&fs0->tex, &fs1->tex);
            resumeFrame(r, &susp);
            viFaceHasPrev[1] = false;
        } else if (!viFace1SharedTarget &&
                   color.r == 0 && color.g == 0 && color.b == 0 && color.a == 0 &&
                   r->colorWrite[0] && r->colorWrite[1] && r->colorWrite[2] && r->colorWrite[3]) {
            CtrSurface* s = &r->surfaces[r->boundSurface];
            viFaceCallIdx[fi] = 0;
            if (!s->spilled && s->target != nullptr && s->lazy == nullptr && s->skipRec == nullptr &&
                !viFaceDeferClear[fi] && viFaceHasPrev[fi] && viFacePrevCount[fi] > 0 && s->version == viFaceSavedVersion[fi]) {
                flushBatch(r);
                s->version = ++r->contentSerial;
                viFaceSavedVersion[fi] = s->version;
                memset(&s->pattern, 0, sizeof(s->pattern));
                s->pattern.version = s->version;
                s->pattern.pageId = -1;
                viFaceDeferClear[fi] = true;
                return;
            }
            viFaceHasPrev[fi] = false;
            viFaceDeferClear[fi] = false;
        }
    }
    VI_CALL(clearBoundTarget)(r, color, targetW, targetH);
}

STAGE2_CODE bool Vine_wrap_drawTowerStrips(CtrRenderer* r, int32_t surfaceId, CtrSurface* s, int32_t srcLeft, int32_t srcTop,
                                           int32_t srcWidth, int32_t srcHeight, const float* px, const float* py,
                                           const CtrColor* cs, const TowerUniforms* uni) {
    if (viStage2Active) {
        int32_t li = vineFindLayerIndex(surfaceId);
        if (li >= 0) {
            int fi = (r->boundSurface == viFaceIds[0]) ? 0 : ((r->boundSurface == viFaceIds[1]) ? 1 : -1);
            if (fi >= 0 && !viFace1SharedTarget && !r->surfaces[r->boundSurface].spilled && viFaceCallIdx[fi] < 32u) {
                CtrSurface* fs = &r->surfaces[r->boundSurface];
                CtrLazyDraw sig;
                memset(&sig, 0, sizeof(sig));
                sig.layer = surfaceId;
                sig.layerVersion = s->version;
                sig.srcLeft = srcLeft;
                sig.srcTop = srcTop;
                sig.srcWidth = srcWidth;
                sig.srcHeight = srcHeight;
                memcpy(sig.px, px, sizeof(sig.px));
                memcpy(sig.py, py, sizeof(sig.py));
                memcpy(sig.cs, cs, sizeof(sig.cs));
                sig.uni = *uni;
                lazyCapture(r, &sig);

                uint32_t idx = viFaceCallIdx[fi];
                if (viFaceDeferClear[fi]) {
                    if (fs->version == viFaceSavedVersion[fi] && idx < viFacePrevCount[fi] &&
                        memcmp(&sig, &viFacePrevSigs[fi][idx], sizeof(sig)) == 0) {
                        viFaceCallIdx[fi] = idx + 1u;
                        if (idx + 1u == viFacePrevCount[fi]) {
                            viFaceDeferClear[fi] = false;
                        }
                        return true;
                    }
                    // Signature differed: materialize the deferred clear and any earlier matched calls for this face.
                    viFaceDeferClear[fi] = false;
                    r->contentSerial--;
                    VI_CALL(clearBoundTarget)(r, viClearZeroColor, 256, 728);
                    for (uint32_t m = 0; m < idx; m++) {
                        const CtrLazyDraw* cur = &viFacePrevSigs[fi][m];
                        int32_t prevLi = vineFindLayerIndex(cur->layer);
                        if (prevLi >= 0) {
                            vineExecuteLayerCall(r, prevLi, cur->layer, &r->surfaces[cur->layer],
                                                 cur->srcLeft, cur->srcTop, cur->srcWidth, cur->srcHeight,
                                                 cur->px, cur->py, cur->cs, &cur->uni);
                        }
                    }
                }
                viFacePrevSigs[fi][idx] = sig;
                viFaceCallIdx[fi] = idx + 1u;
                viFacePrevCount[fi] = idx + 1u;
            }
            bool res = vineExecuteLayerCall(r, li, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
            if (fi >= 0) {
                viFaceHasPrev[fi] = true;
                viFaceSavedVersion[fi] = r->surfaces[viFaceIds[fi]].version;
            }
            return res;
        }
    }
    return VI_CALL(drawTowerStrips)(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
}

STAGE2_COLD void Vine_wrap_CtrRenderer_beginFrame(Renderer* renderer) {
    CtrRenderer* r = (CtrRenderer*) renderer;
    if (!viStage2InitDone) {
        viStage2InitDone = true;
        viStage2Enabled = true;
        viLastRoomStart = 0xffffffffu;
    }

    VI_CALL(CtrRenderer_beginFrame)(renderer);

    uint32_t roomStart = r->textures.roomStartFrame;
    if (roomStart != viLastRoomStart) {
        viLastRoomStart = roomStart;
        viStage2Active = false;
        viFace1SharedTarget = false;
        viFound = false;
        viActAttempts = 0;
        viActLastTryFrame = 0xffffffffu;
        viFaceHasPrev[0] = false;
        viFaceHasPrev[1] = false;
        viFaceDeferClear[0] = false;
        viFaceDeferClear[1] = false;
    }

    if (!viFound) {
        viFound = vineDiscoverTowerSurfaces(r);
    }

    if (viStage2Enabled && viFound && !viStage2Active) {
        if (viActAttempts < VINE_MAX_ACT_ATTEMPTS && viActLastTryFrame != r->frame) {
            viActLastTryFrame = r->frame;
            vineActivateStage2(r);
        }
    } else if (viStage2Active) {
        vineRestoreFaceTargets(r);
    }

    if (viStage2Active) {
        for (int32_t id = 1; id <= viFaceIds[1]; id++) {
            CtrSurface* s = &r->surfaces[id];
            if (s->used && !s->spilled && s->target != nullptr) {
                s->boundFrames = 2;
                s->boundFrame = r->frame;
                s->seenFrame = r->frame;
                s->touchedFrame = r->frame;
            }
        }
        for (int i = 0; i < 24; i++) {
            r->surfaces[viLayerIds[i]].touchedFrame = r->frame;
        }
        for (int32_t k = 0; k < TOWER_MAX_COVERAGES; k++) {
            if (towerCoverages[k].version != 0) {
                towerCoverages[k].usedFrame = r->frame;
            }
        }
    }
}
#else
STAGE2_CODE void Vine_wrap_clearBoundTarget(CtrRenderer* r, CtrColor color, int32_t targetW, int32_t targetH) {
    VI_CALL(clearBoundTarget)(r, color, targetW, targetH);
}
STAGE2_CODE bool Vine_wrap_drawTowerStrips(CtrRenderer* r, int32_t surfaceId, CtrSurface* s, int32_t srcLeft, int32_t srcTop,
                                           int32_t srcWidth, int32_t srcHeight, const float* px, const float* py,
                                           const CtrColor* cs, const TowerUniforms* uni) {
    return VI_CALL(drawTowerStrips)(r, surfaceId, s, srcLeft, srcTop, srcWidth, srcHeight, px, py, cs, uni);
}
STAGE2_COLD void Vine_wrap_CtrRenderer_beginFrame(Renderer* renderer) {
    VI_CALL(CtrRenderer_beginFrame)(renderer);
}
#endif
#endif // CTRPORT_TARGET_N3DS
