// S4: the same recorder and policy on the screen without the game.
// Only the helper pass retains this section. Off delegates to the old bodies;
// no new renderer fields, heap allocation or render target are needed.
#ifdef ENABLE_WAD16
#define DLGTOP_CODE __attribute__((section(".aacout.text.dlgtop4"), noinline, noclone, used, optimize("Os")))
bool CtrLayout_otherText(void);
#include "ctr_dlgtop.h"
#include "ctr_dlgtop_lease.inc"
void CtrDimlight_setDisplay(Renderer*, const CtrDisplaySettings*);
void __real_CtrRenderer_beginFrame(Renderer* renderer);
void __real_CtrRenderer_endFrame(Renderer* renderer);

DLGTOP_CODE void CtrDlgtop_display(Renderer* renderer, const CtrDisplaySettings* settings) {
    // The old screens wrapper narrows top text away. Restore the logical
    // Other choice before the lighting/popup/display chain sees it.
    if (dlgtopLease.owner == (CtrRenderer*) renderer && dlgtopLease.battle) {
        CtrDisplaySettings display = *settings;
        display.dialogueBottom = false;
        CtrDimlight_setDisplay(renderer, &display);
    } else if (CtrScreens_gameBelow() && CtrLayout_otherText()) {
        CtrDisplaySettings display = *settings;
        display.dialogueBottom = true;
        CtrDimlight_setDisplay(renderer, &display);
    } else CtrDimlight_setDisplay(renderer, settings);
}

// The held physical target is the route descriptor: its dimensions select the
// presentation width. The recorder, popup anchors and classification are shared.
DLGTOP_CODE static C3D_RenderTarget* dlgtopTarget(CtrRenderer* r) {
    if (!r->display.dialogueBottom || r->bottomYielded) return nullptr;
    return dlgtopBorrowed(r);
}

DLGTOP_CODE void CtrDlgtop_begin(Renderer* renderer) {
    __real_CtrRenderer_beginFrame(renderer);
    CtrRenderer* r = (CtrRenderer*) renderer;
    if (dlgtopLease.owner == r && dlgtopLease.battle) {
        // The move gave the top to the game. Neither direction may redirect
        // text until the first non-battle room returns it to the player.
        r->display.dialogueBottom = r->dlg.armed = false;
    } else if (dlgtopWants(r) && CtrLayout_targetsHeld()) {
        r->display.dialogueBottom = true;
        r->dlg.armed = r->dlg.policy.redirectCount > 0 && dlgtopHolder() != DLGTOP_FLIGHT;
    }
}

// Registration can happen during Draw; never run popup-anchor collection there.
// Menu return uses the full gate, exactly as the bottom-direction repair does.
DLGTOP_CODE void CtrDlgtop_refresh(CtrRenderer* r, bool duringRegistration) {
    if (dlgtopLease.owner == r && dlgtopLease.battle) { r->dlg.armed = false; return; }
    if (!dlgtopWants(r)) { dlgflashRefresh(r, duringRegistration); return; }
    // Registration never requests a loan inside Draw. The presentation seam
    // can suspend the native frame and borrow for this recorder outside it.
    dlgtopTake(r);
    r->dlg.lastInst = nullptr;
    r->dlg.lastAnswer = false;
    bool canRecord = dlgtopBorrowed(r) != nullptr ||
                     (dlgtopLease.drawing && dlgtopHolder() == DLGTOP_FREE);
    r->dlg.armed = r->dlg.policy.redirectCount > 0 && canRecord &&
        (duringRegistration ? __real_CtrDialogue_allowed(&r->dlg.policy, r->base.runner) :
                              CtrDialogue_allowed(&r->dlg.policy, r->base.runner));
}

DLGTOP_CODE static void dlgtopShown(CtrRenderer* r, C3D_RenderTarget* target, bool shown) {
    r->dlg.shown = shown;
    r->magnifier.active = false;
    // Remember an actual blank transfer too; configureScreens resets this
    // when HOME, a settings change or a notice took the screen.
    r->magnifier.blankSent = 2;
    if (!shown) {
        C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0);
        bindTarget(r, target, -1);
    }
    // Physical top lighting; the bottom lighting flag belongs to the game now.
    CtrDimlight_topBacklight(r, !shown && CtrScreens_otherOff && r->screenshotNoticeUntil == 0);
}

// Replay uses the existing bottom dialogue's geometry and GPU primitives,
// parameterized by the borrowed target's width. It never holds an empty frame.
DLGTOP_CODE static bool dlgtopReplay(CtrRenderer* r, C3D_RenderTarget* target, int32_t appW, int32_t appH) {
    float screenW = (float) target->frameBuf.height;
    CtrSurface* rec = &r->dlg.rec;
    ptrdiff_t n = r->dlg.armed ? arrlen(rec->ops) : 0;
    r->dlg.armed = false; // what is drawn from here on is the screens'
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (ptrdiff_t i = 0; n > i; i++) {
        float ox0, oy0, ox1, oy1;
        recOpBounds(rec->ops[i].x, rec->ops[i].y, &ox0, &oy0, &ox1, &oy1);
        x0 = minF(x0, ox0);
        y0 = minF(y0, oy0);
        x1 = maxF(x1, ox1);
        y1 = maxF(y1, oy1);
    }
    x0 = maxF(x0, 0.0f);
    y0 = maxF(y0, 0.0f);
    x1 = minF(x1, (float) appW);
    y1 = minF(y1, (float) appH);
    if (n == 0 || x0 >= x1 || y0 >= y1) {
        if (DLG_GAP_NONE > r->dlg.gapFrames) r->dlg.gapFrames++;
        if (!r->dlg.shown && r->magnifier.blankSent == 2) return false;
        dlgtopShown(r, target, false);
        return false;
    }

    float s = DLG_SCALE;
    if ((x1 - x0) * s > screenW) s = screenW / (x1 - x0);
    if ((y1 - y0) * s > (float) TOP_SCREEN_H) s = (float) TOP_SCREEN_H / (y1 - y0);
    // Across, where the top screen shows it, the frame centred; else centred itself.
    float ox = floorf((screenW - (float) appW * s) * 0.5f);
    if (x0 * s + ox < 0.0f || x1 * s + ox > screenW) ox = floorf((screenW - (x1 - x0) * s) * 0.5f - x0 * s);
    // Down, centred when it appears, and kept starting at that row (whether
    // the game put the box at the top or bottom of its frame) unless it no
    // longer fits.
    float h = (y1 - y0) * s;
    if (!r->dlg.shown || r->dlg.top + h > (float) TOP_SCREEN_H) r->dlg.top = floorf(((float) TOP_SCREEN_H - h) * 0.5f);
    float oy = r->dlg.top - floorf(y0 * s);
    if (!r->dlg.shown) dlgtopShown(r, target, true);
    if (0 < r->dlg.gapFrames && DLG_GAP_NONE > r->dlg.gapFrames) {
        if ((int32_t) (sizeof(r->dlg.gaps) / sizeof(r->dlg.gaps[0])) > r->dlg.gapCount) r->dlg.gaps[r->dlg.gapCount++] = r->dlg.gapFrames;
        r->dlg.gapLongest = r->dlg.gapLongest > r->dlg.gapFrames ? r->dlg.gapLongest : r->dlg.gapFrames;
    }
    r->dlg.gapFrames = 0;
    r->dlg.holdFrames = 0;
    r->dlg.shownFrames++;

    C3D_RenderTargetClear(target, C3D_CLEAR_ALL, 0x000000FF, 0);
    bindTarget(r, target, -1);
    copyState(r);
    screenProjection(r);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA);
    // The quads were recorded without effects (dlgRedirecting); the game's
    // state left from its last draw mustn't apply to them either.
    uint8_t savedFx = r->shaderFx;
    uint8_t savedPal = r->palKind;
    r->shaderFx = SHADER_FX_NONE;
    r->palKind = PAL_NONE;
    r->palNextB = nullptr;
    r->filterOverride = FILTER_NEAREST;
    for (ptrdiff_t i = 0; n > i; i++) {
        const CtrRecOp* op = &rec->ops[i];
        float qx[4], qy[4];
        for (int32_t k = 0; 4 > k; k++) {
            qx[k] = op->x[k] * s + ox;
            qy[k] = op->y[k] * s + oy;
        }
        if (recOpUpright(op)) {
            // Onto whole pixels, keeping whole sizes whole.
            float w = qx[1] - qx[0], hh = qy[3] - qy[0];
            float l = roundf(qx[0]), t = roundf(qy[0]);
            if (fabsf(w - roundf(w)) < 0.01f) w = roundf(w);
            if (fabsf(hh - roundf(hh)) < 0.01f) hh = roundf(hh);
            qx[0] = qx[3] = l;
            qx[1] = qx[2] = l + w;
            qy[0] = qy[1] = t;
            qy[2] = qy[3] = t + hh;
        }
        if (op->pageId == REC_COLOR_PAGE) {
            emitColoredQuad(r, qx, qy, op->c);
            continue;
        }
        if (isRecSurfacePage(op->pageId)) {
            continue;
        }
        CtrPage* page = 0 <= op->pageId ? CtrTextures_acquire(&r->textures, (uint32_t) op->pageId) : surfaceSpritePage(r, (int16_t) op->pageId);
        if (page == nullptr) continue;
        // Sampled 1:1 nearest, as nearly all of it is; finer art (a
        // shrunk sprite) is averaged when smoothing is on.
        float ratio = fabsf((op->sx1 - op->sx0) / (qx[1] - qx[0] != 0.0f ? qx[1] - qx[0] : 1.0f));
        int8_t filter = r->display.smoothing && ratio > 1.01f ? FILTER_LINEAR : FILTER_NEAREST;
        if (filter != r->filterOverride) {
            flushBatch(r);
            r->filterOverride = filter;
        }
        emitPageTiles(r, page, op->sx0, op->sy0, op->sx1, op->sy1, qx, qy, op->c);
    }
    flushBatch(r);
    r->filterOverride = 0;
    r->shaderFx = savedFx;
    r->palKind = savedPal;
    return true;
}

#include "ctr_dlgtop_birth.inc"

DLGTOP_CODE void CtrDlgtop_present(CtrRenderer* r, const CtrSurface* app, C3D_Tex* tex) {
    C3D_RenderTarget* target = dlgtopTarget(r);
    if (target == nullptr) target = dlgtopDrawBirth(r);
    if (target == nullptr) {
        if (dlgtopWants(r)) return; // no loan: never paint over the bottom game target
        void originalBottom(CtrRenderer*, const CtrSurface*, C3D_Tex*) __asm__("presentBottom");
        originalBottom(r, app, tex);
        return;
    }
    // Setup can clear BOTH buffers and force single buffering while lent.
    // Reassert before every transfer and redraw from this frame's recorder;
    // no cached pixels or setup-preserved shown flag are required.
    gfxSetDoubleBuffering(GFX_TOP, true);
    if (!target->linked || target->screen != GFX_TOP || target->transferFlags != BOTTOM_TRANSFER_FLAGS)
        C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, BOTTOM_TRANSFER_FLAGS);
    dlgtopReplay(r, target, app->width, app->height);
}

// A room transition may discard the application surface: present() then
// returns before the replay call. Clear an expired box at the final frame
// seam as well, so that path cannot leave yesterday's text on the LCD.
// No game draw follows this seam. bindTarget flushes the last game's batch
// before selecting the blank other target; the original endFrame submits it.
DLGTOP_CODE void CtrDlgtop_end(Renderer* renderer) {
    CtrRenderer* r = (CtrRenderer*) renderer;
    if (r->inFrame && !r->skip.active) {
        C3D_RenderTarget* target = dlgtopTarget(r);
        if (target != nullptr && (r->dlg.shown || r->magnifier.blankSent == 0) &&
            (!r->dlg.armed || arrlen(r->dlg.rec.ops) == 0)) {
            gfxSetDoubleBuffering(GFX_TOP, true);
            if (!target->linked || target->screen != GFX_TOP || target->transferFlags != BOTTOM_TRANSFER_FLAGS)
                C3D_RenderTargetSetOutput(target, GFX_TOP, GFX_LEFT, BOTTOM_TRANSFER_FLAGS);
            dlgtopShown(r, target, false);
        }
    }
    __real_CtrRenderer_endFrame(renderer);
    // returnTop waits for idle/unlinks. It must run outside the GPU frame,
    // after the final blank transfer, never between Draw and submission.
    if (dlgtopLease.owner == r && (!r->dlg.shown || !dlgtopBoxOpen(r))) dlgtopReturn();
}
#undef DLGTOP_CODE
#endif
