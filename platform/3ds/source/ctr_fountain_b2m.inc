// Generic source-patch acceleration of the fountain shader. The mesh is
// retained for unsupported sampling, geometry, parameters or missing tiles.
#include "ctr_fountain_mask.inc"
static __attribute__((noinline, section(".fountain_patch"))) bool fountainBGpuBuild(CtrRenderer* r, CtrPage* page, const int32_t* rect);

static __attribute__((noinline, section(".fountain_patch"))) bool fountainBEnabled(CtrRenderer* r) {
    CtrFountainB2m* b = &fountainGet(r)->patch;
    if (!b->checked) {
        b->checked = true;
#if defined(CTRPORT_TARGET_N3DS)
        b->enabled = true;
#endif
    }
    return b->enabled;
}

// Only this analytic parameter set has a validated mask. Other shapes
// use the mesh, rather than silently sampling an incompatible lookup table.
static __attribute__((noinline, section(".fountain_patch"))) bool fountainBMask(CtrRenderer* r, float amplitude, float strip) {
    CtrFountainB2m* b = &fountainGet(r)->patch;
    if (amplitude != 0.005f || strip != 0.0101f) return false;
    if (b->maskReady) return true;
    if (!C3D_TexInit(&b->mask, FOUNTAIN_MASK_W, FOUNTAIN_MASK_H, GPU_A8)) return false;
    memcpy(b->mask.data, fountainMaskConstant, sizeof(fountainMaskConstant));
    C3D_TexSetFilter(&b->mask, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&b->mask, GPU_CLAMP_TO_EDGE, GPU_REPEAT);
    C3D_TexFlush(&b->mask);
    b->maskReady = true;
    return true;
}

// Only immutable atlas pages are cached. Surface descriptors and live GPU
// textures fall back. Resident pointers, formats and source checksums are
// revalidated on every hit, including after eviction. This path never
// loads tiles: if any are missing, the normal mesh handles that frame.
static __attribute__((noinline, section(".fountain_patch"))) bool fountainBPatch(CtrRenderer* r, CtrPage* page, const float* origin, const float* span) {
    CtrFountainB2m* b = &fountainGet(r)->patch;
    uintptr_t address = (uintptr_t) page, base = (uintptr_t) r->textures.pages;
    if (address < base || address - base >= r->textures.pageCount * sizeof(CtrPage) ||
        (address - base) % sizeof(CtrPage) != 0) return false;
    if (!page->layoutKnown || page->tiles == nullptr || page->tileW <= 0 || page->tileH <= 0) return false;
    int32_t rect[4] = { (int32_t) origin[0], (int32_t) origin[1], (int32_t) span[0], (int32_t) span[1] };
    if ((float) rect[0] != origin[0] || (float) rect[1] != origin[1] ||
        (float) rect[2] != span[0] || (float) rect[3] != span[1] ||
        rect[2] < 1 || rect[3] < 1 || rect[2] > 128 || rect[3] > 128 ||
        rect[0] < 0 || rect[1] < 0 || rect[0] + rect[2] > page->width || rect[1] + rect[3] > page->height) return false;
    int32_t grid[4] = {rect[0] / page->tileW, rect[1] / page->tileH,
                       (rect[0] + rect[2] - 1) / page->tileW, (rect[1] + rect[3] - 1) / page->tileH};
    int32_t nx = grid[2] - grid[0] + 1, ny = grid[3] - grid[1] + 1;
    if (nx > 2 || ny > 2) return false;
    bool same = b->patchReady && b->page == page && b->tiles == page->tiles &&
                memcmp(b->region, rect, sizeof(rect)) == 0 && memcmp(b->grid, grid, sizeof(grid)) == 0;
    for (int32_t y = 0; ny > y; y++) {
        for (int32_t x = 0; nx > x; x++) {
            CtrTile* tile = &page->tiles[(grid[1] + y) * page->tilesX + grid[0] + x];
            int32_t i = y * nx + x;
            if (!tile->valid && !(page->formatsKnown && tile->format == CTR_TILE_EMPTY)) return false;
            if (tile->valid && tile->tex.fmt != GPU_RGBA8 && tile->tex.fmt != GPU_RGBA5551 &&
                tile->tex.fmt != GPU_RGB8 && tile->tex.fmt != GPU_RGB565) return false;
            same &= b->source[i].data == tile->tex.data && b->source[i].checksum == tile->checksum &&
                    b->source[i].format == tile->tex.fmt && b->source[i].valid == tile->valid;
        }
    }
    if (same) return true;
    if (!fountainBMask(r, FOUNTAIN_EDGE, FOUNTAIN_STRIP)) return false;
    // A prior frame can still read the old patch. Allocate replacement
    // storage rather than overwriting it; ctr_memory defers its old free.
    C3D_Tex replacement;
    if (!C3D_TexInit(&replacement, 256, 256, GPU_RGBA8)) return false;
    if (b->patchReady) C3D_TexDelete(&b->patch);
    b->patch = replacement;
    b->patchReady = true;
    if (!fountainBGpuBuild(r, page, rect)) {
        C3D_TexDelete(&b->patch);
        b->patchReady = false;
        return false;
    }
    C3D_TexSetFilter(&b->patch, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&b->patch, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    b->page = page;
    b->tiles = page->tiles;
    memcpy(b->region, rect, sizeof(rect));
    memcpy(b->grid, grid, sizeof(grid));
    for (int32_t y = 0; ny > y; y++) {
        for (int32_t x = 0; nx > x; x++) {
            CtrTile* tile = &page->tiles[(grid[1] + y) * page->tilesX + grid[0] + x];
            int32_t i = y * nx + x;
            b->source[i].data = tile->tex.data;
            b->source[i].checksum = tile->checksum;
            b->source[i].format = tile->tex.fmt;
            b->source[i].valid = tile->valid;
        }
    }
    b->builds++;
    return true;
}

static __attribute__((noinline, section(".fountain_patch"))) bool fountainBTry(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                          const float* px, const float* py, const CtrColor* colors) {
    CtrFountainB2m* b = &fountainGet(r)->patch;
    b->creationTicks = 0;
    b->vertices = 0;
    b->batches = 0;
    bool linear = r->filterOverride != 0 ? r->filterOverride == FILTER_LINEAR : r->base.texFilter;
    if (linear || r->pairProgram == nullptr || r->pairTc2Loc < 0 ||
        px[0] != px[3] || px[1] != px[2] || py[0] != py[1] || py[2] != py[3] ||
        px[1] <= px[0] || py[3] <= py[0] || memcmp(colors, colors + 1, sizeof(*colors)) != 0 ||
        memcmp(colors, colors + 2, sizeof(*colors)) != 0 || memcmp(colors, colors + 3, sizeof(*colors)) != 0) return false;
    const float* uv = fountainUniformValue(r, "uvs");
    const float* offset = fountainUniformValue(r, "iPosOffset");
    float time = fountainUniformValue(r, "iTime")[0];
    float origin[2] = {uv[0] * page->width, uv[1] * page->height};
    float span[2] = {uv[2] * page->width, uv[3] * page->height};
    if (!(span[0] > 0 && span[1] > 0 && sx1 > sx0 && sy1 > sy0)) return false;
    float u0 = (sx0 - origin[0]) / span[0], u1 = (sx1 - origin[0]) / span[0];
    float v0 = (sy0 - origin[1]) / span[1], v1 = (sy1 - origin[1]) / span[1];
    // The 2x2 source patch covers both fractional phases without wrapping.
    if (v0 < 0 || v1 > 1 || v1 - v0 > 1 || u0 < 0 || u1 > 1) return false;
    if (quadOutsideView(r, px, py)) return true;
    float phaseX[2] = {offset[0] + time * 0.008f, offset[0] - time * 0.009f};
    float phaseY[2] = {offset[1] + time * 0.005f, offset[1] + time * 0.007f};
    for (int32_t k = 0; 2 > k; k++) {
        phaseX[k] -= floorf(phaseX[k]);
        phaseY[k] -= floorf(phaseY[k]);
    }
    float xc[FOUNTAIN_CUTS_MAX];
    int32_t nx = fountainCuts(xc, u0, u1, origin[0], span[0], page->tileW, page->tilesX, phaseX, false);
    flushBatch(r);
    uint64_t creation = svcGetSystemTick();
    if (!fountainBPatch(r, page, origin, span)) return false;
    b->creationTicks = svcGetSystemTick() - creation;
    b->vertices = 0;
    fountainGet(r)->drawing = true;
    b->active = true;
    r->palNextB = &b->patch;
    r->palNextTc1[0] = r->palNextTc1[1] = 1.0f;
    r->palNextTc1[2] = (phaseX[1] - phaseX[0]) * span[0] / 256.0f;
    r->palNextTc1[3] = (phaseY[1] - phaseY[0]) * span[1] / 256.0f;
    const float invTau = 0.15915494309189533577f, strip = FOUNTAIN_STRIP;
    for (int32_t side = 0; 2 > side; side++) {
        b->tc2[0] = (side == 0 ? 1.0f : -1.0f) * 256.0f / (span[0] * strip);
        b->tc2[1] = 256.0f / span[1] * (FOUNTAIN_WAVE_FREQUENCY * invTau);
        b->tc2[2] = (side == 0 ? -phaseX[0] : 1.0f + phaseX[0]) / strip;
        b->tc2[3] = (time - FOUNTAIN_WAVE_FREQUENCY * phaseY[0]) * invTau;
        for (int32_t i = 0; nx - 1 > i; i++) {
            if ((xc[i] >= 0.5f) != (side != 0)) continue;
            CtrVertex* out = reserveVertices(r, &b->patch, 6);
            if (out == nullptr) continue;
            const float us[4] = {xc[i], xc[i + 1], xc[i + 1], xc[i]};
            const float vs[4] = {v0, v0, v1, v1};
            CtrVertex corners[4];
            for (int32_t k = 0; 4 > k; k++) {
                float x, y;
                CtrColor c;
                quadPosition(px, py, (us[k] - u0) / (u1 - u0), (vs[k] - v0) / (v1 - v0), &x, &y);
                c = colors[0];
                c.a = unitToByte(1.0f - fabsf(us[k] - 0.5f) * 0.75f);
                setVertex(&corners[k], x, y, r->vertexZ, (us[k] + phaseX[0]) * span[0] / 256.0f,
                          (vs[k] + phaseY[0]) * span[1] / 256.0f, c);
            }
            *out++ = corners[0]; *out++ = corners[1]; *out++ = corners[2];
            *out++ = corners[0]; *out++ = corners[2]; *out++ = corners[3];
            b->vertices += 6;
        }
        flushBatch(r);
    }
    b->active = false;
    fountainGet(r)->drawing = false;
    r->palNextB = nullptr;
    return true;
}

static __attribute__((noinline, section(".fountain_patch"))) void fountainRegion(CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
                            const float* px, const float* py, const CtrColor* colors) {
    if (!fountainBEnabled(r)) {
        fountainMeshRegion(r, page, sx0, sy0, sx1, sy1, px, py, colors);
        return;
    }
    if (!fountainBTry(r, page, sx0, sy0, sx1, sy1, px, py, colors)) {
        fountainGet(r)->patch.fallbacks++;
        fountainMeshRegion(r, page, sx0, sy0, sx1, sy1, px, py, colors);
    }
}
