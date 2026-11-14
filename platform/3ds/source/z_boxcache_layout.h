// boxcache (z_boxcache.c, UNDERTALE Old): the CtrRenderer fields it reaches, by offset. The renderer's fields are
// private to ctr_renderer.c, whose end checks every line here (offset, size and type) with static asserts, so a change
// to the struct stops the build instead of reading the wrong field.
//
// X(name, member, type, offset)
#ifndef Z_BOXCACHE_LAYOUT_H
#define Z_BOXCACHE_LAYOUT_H

#define BOXCACHE_FIELDS(X)                               \
    X(IN_FRAME, inFrame, bool, 19964)                    \
    X(SKIP_PRESENT, skipPresent, bool, 19965)            \
    X(SKIP_ACTIVE, skip.active, bool, 19972)             \
    X(BOTTOM_TARGET, bottomTarget, C3D_RenderTarget*, 12728) \
    X(BOTTOM_YIELDED, bottomYielded, bool, 15848)        \
    X(DLG_ARMED, dlg.armed, bool, 15492)                 \
    X(DLG_OPS, dlg.rec.ops, CtrRecOp*, 15612)            \
    X(DLG_DROPPED, dlg.rec.recDropped, bool, 15622)      \
    X(DLG_SHOWN, dlg.shown, bool, 15768)                 \
    X(DLG_TOP, dlg.top, float, 15776)                    \
    X(DLG_SHOWN_FRAMES, dlg.shownFrames, uint32_t, 15780) \
    X(DLG_GAP, dlg.gapFrames, int32_t, 15788)            \
    X(DISPLAY, display, CtrDisplaySettings, 12752)       \
    X(TEXTURES, textures, CtrTextureCache, 728)          \
    X(SURFACES, surfaces, CtrSurface*, 19804)            \
    X(SCREENSHOT_AFFECTED, screenshotTimingAffected, bool, 21535) \
    X(SURFACE_COUNT, surfaceCount, uint32_t, 19808)

// CtrSurface: its size and the three fields read of the application surface.
#define BOXCACHE_SURFACE_SIZE 264
#define BOXCACHE_SURFACE_USED 0
#define BOXCACHE_SURFACE_WIDTH 4
#define BOXCACHE_SURFACE_HEIGHT 8

// CtrRecOp, which z_boxcache.c's BxOp copies: its size, and the fields read (X(member, type, offset)). The hash covers
// the bytes up to and including zeroFromZero, the last field; the byte after it is padding.
#define BOXCACHE_OP_SIZE 72
#define BOXCACHE_OP_FIELDS(X) \
    X(pageId, int32_t, 0)     \
    X(sx0, float, 4)          \
    X(sy0, float, 8)          \
    X(sx1, float, 12)         \
    X(sy1, float, 16)         \
    X(x, float[4], 20)        \
    X(y, float[4], 36)        \
    X(zeroFromZero, bool, 70)

// presentDialogue's constants.
#define BOXCACHE_COLOR_PAGE INT32_MIN
// A page presentDialogue's loop skips: not the colour page, not a recorded surface (REC_SURFACE_PAGE and below), and as
// a surface sprite slot -2 - (-1) = -1, which surfaceSpritePage finds no sprite for. Checked where the renderer's
// constants are seen (ctr_renderer.c includes this at its end).
#define BOXCACHE_SKIP_PAGE (-1)
#if defined(REC_SURFACE_PAGE)
_Static_assert(BOXCACHE_SKIP_PAGE != REC_COLOR_PAGE && REC_SURFACE_PAGE < BOXCACHE_SKIP_PAGE && 0 > BOXCACHE_SKIP_PAGE &&
                   0 > -2 - (int16_t) BOXCACHE_SKIP_PAGE,
               "z_boxcache.c: the skipped page");
#endif

#endif
