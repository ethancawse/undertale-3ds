// A Draw-created box first exists after the pre-Draw loan seam. Finish the
// current GPU command list without screen transfers, borrow outside that
// native frame, then replay this SAME recorder in the resumed frame. The
// renderer already uses this suspend/resume pair for coherent surface reads.
// No Step, logical frame, input sample, target or frame copy is added.
DLGTOP_CODE static C3D_RenderTarget* dlgtopDrawBirth(CtrRenderer* r) {
    if (!r->inFrame || !dlgtopWants(r) || dlgtopLease.owner != r ||
        !dlgtopLease.drawing || dlgtopHolder() != DLGTOP_FREE ||
        !dlgtopBoxOpen(r) || !r->dlg.armed || arrlen(r->dlg.rec.ops) == 0) return nullptr;
    void originalEndFrameEarly(CtrRenderer*, CtrSuspendedFrame*) __asm__("endFrameEarly");
    void originalResumeFrame(CtrRenderer*, const CtrSuspendedFrame*) __asm__("resumeFrame");
    CtrSuspendedFrame suspended;
    flushBatch(r);
    originalEndFrameEarly(r, &suspended);
    CtrGpu_waitIdle();
    r->inFrame = false;
    dlgtopLease.drawing = false;
    dlgtopTake(r);
    originalResumeFrame(r, &suspended);
    r->inFrame = true;
    dlgtopLease.drawing = true;
    return dlgtopTarget(r);
}
