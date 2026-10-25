static __attribute__((noinline, section(".fountain_patch"))) void fountainPageRegion(
    CtrRenderer* r, CtrPage* page, float sx0, float sy0, float sx1, float sy1,
    const float* px, const float* py, const CtrColor* c) {
    if (recordingSurface(r) != nullptr) {
        windowRecordedSurface(r);
        if (recordingSurface(r) != nullptr) {
            LOG_ONCE("Renderer: no target for fountain shader draw\n");
            return;
        }
    }
    fountainRegion(r, page, sx0, sy0, sx1, sy1, px, py, c);
}

static __attribute__((noinline, section(".fountain_patch"))) bool fountainBGpuBuild(CtrRenderer* r, CtrPage* page, const int32_t* rect) {
    CtrFountainB2m* b = &fountainGet(r)->patch;
    C3D_RenderTarget* target = CtrGpu_renderTargetAnywhere(&b->patch);
    if (target == nullptr) {
        return false;
    }
    CtrTargetState state;
    saveState(r, &state);
    C3D_RenderTarget* previousTarget = r->boundTarget;
    int32_t previous = r->boundSurface;
    C3D_Mtx projection = r->projection;
    BlendFactors factors = r->base.blendFactors;
    bool blendEnable = r->blendEnable, blendOverridden = r->blendOverridden;
    int32_t blendMode = r->blendMode;
    bool alpha = r->alphaTestEnable, fog = r->fogEnable;
    bool suspended = r->fxSuspended, plain = r->plainTexEnv;
    int8_t filtering = r->filterOverride;
    uint8_t cut = r->opAlphaCut, layer = r->layer.mode;
    bool writes[4];
    memcpy(writes, r->colorWrite, sizeof(writes));
    bindTarget(r, target, -1);
    r->viewport = (CtrRect){0, 0, 256, 256};
    r->scissorEnabled = false;
    r->hostPass = false;
    pixelProjection(&r->projection, 256, 256);
    Matrix4f pixels = {{2.0f/256,0,0,0, 0,-2.0f/256,0,0,
                       0,0,1,0, -1,1,0,1}};
    r->base.gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION] = pixels;
    r->visible.valid = false;
    r->blendEnable = false;
    r->blendOverridden = true;
    r->alphaTestEnable = false;
    r->fogEnable = false;
    r->fxSuspended = true;
    r->plainTexEnv = true;
    r->filterOverride = FILTER_NEAREST;
    r->opAlphaCut = 0;
    r->layer.mode = 0;
    memset(r->colorWrite, true, sizeof(r->colorWrite));
    r->gridHold++;
    applyAllState(r);
    emitRect(r, 0, 0, 256, 256, (CtrColor){0,0,0,0});
    flushBatch(r);
    CtrColor white[4] = {{255,255,255,255},{255,255,255,255},
                         {255,255,255,255},{255,255,255,255}};
    for (int iy = 0; 2 > iy; iy++) {
        for (int ix = 0; 2 > ix; ix++) {
            float x = (float)(ix * rect[2]), y = (float)(iy * rect[3]);
            float px[4] = {x, x+rect[2], x+rect[2], x};
            float py[4] = {y, y, y+rect[3], y+rect[3]};
            emitPageRegionDirect(r, page, true, (float)rect[0], (float)rect[1],
                (float)(rect[0]+rect[2]), (float)(rect[1]+rect[3]), px, py, white);
        }
    }
    flushBatch(r);
    splitFrame(r);
    r->gridHold--;
    r->projection = projection;
    r->base.blendFactors = factors;
    r->blendEnable = blendEnable;
    r->blendOverridden = blendOverridden;
    r->blendMode = blendMode;
    r->alphaTestEnable = alpha;
    r->fogEnable = fog;
    r->fxSuspended = suspended;
    r->plainTexEnv = plain;
    r->filterOverride = filtering;
    r->opAlphaCut = cut;
    r->layer.mode = layer;
    memcpy(r->colorWrite, writes, sizeof(writes));
    if (0 <= previous) bindSurface(r, previous);
    else bindTarget(r, previousTarget, -1);
    restoreState(r, &state);
    applyAllState(r);
    // Ordering is explicit: FrameSplit alone does not submit the GX queue.
    // Complete this one-time build before sampling it; charge the entire wait
    // (including preceding frame work) to this draw. No CPU readback/transfer.
    CtrSuspendedFrame frame;
    endFrameEarly(r, &frame);
    CtrGpu_waitIdle();
    // Target deletion is only legal between citro3d frames. It owns neither
    // colour nor depth storage, so this deletes only its small descriptor.
    C3D_RenderTargetDelete(target);
    resumeFrame(r, &frame);
    return true;
}
