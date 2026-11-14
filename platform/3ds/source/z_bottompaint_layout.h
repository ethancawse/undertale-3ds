#ifndef Z_BOTTOMPAINT_LAYOUT_H
#define Z_BOTTOMPAINT_LAYOUT_H
// Identical on the frozen Old/New base; checked at ctr_renderer.c's tail.
#define BP_RENDERER_FIELDS(X) \
    X(TARGET, bottomTarget, C3D_RenderTarget*, 12728) \
    X(DISPLAY, display, CtrDisplaySettings, 12752) \
    X(CONFIG, displayDirty, bool, 12828) \
    X(ARMED, dlg.armed, bool, 15492) \
    X(OPS, dlg.rec.ops, CtrRecOp*, 15612) \
    X(DROPPED, dlg.rec.recDropped, bool, 15622) \
    X(SHOWN, dlg.shown, bool, 15768) \
    X(YIELDED, bottomYielded, bool, 15848) \
    X(IN_FRAME, inFrame, bool, 19964) \
    X(SKIP, skipPresent, bool, 19965) \
    X(SKIP_ACTIVE, skip.active, bool, 19972) \
    X(SHOT_REQUEST, screenshotRequested, bool, 21524) \
    X(SHOT, screenshotTimingAffected, bool, 21535) \
    X(NOTICE, screenshotNoticeUntil, uint64_t, 21536)
#define BP_DIMMED_OFFSET 82
#define BP_SUSPENDED_OFFSET 85
#define BP_RESTORE_PENDING_OFFSET 101
#endif
