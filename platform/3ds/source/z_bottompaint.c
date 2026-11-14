// Idle LCD painter: shared with the normal bottom compositor, never a second
// GPU pass. The existing spare is painted on the game thread, then copied once
// at VBlank1. The top frame keeps its existing pacer deadline and queue depth.
#if defined(ENABLE_WAD16)
#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_array.h"
#include "ctr_bottom_paint.h"
#include "ctr_bottom_view.h"
#include "ctr_display.h"
#include "ctr_frame_pacer.h"
#include "ctr_renderer.h"
#include "runner.h"
#include "z_bottompaint_layout.h"
#include "log.h"

#define BP_CODE __attribute__((noinline, section(".text.late.paintchg_idle")))
typedef void CtrRecOp;
#define BP_TYPE(name, member, type, offset) typedef type BpT_##name;
#define BP_OFFSET(name, member, type, offset) BpO_##name = offset,
BP_RENDERER_FIELDS(BP_TYPE)
enum { BP_RENDERER_FIELDS(BP_OFFSET) };
#define BP(r, name) (*(BpT_##name*) ((char*) (r) + BpO_##name))
#define BP_BYTES (320u * 240u * 2u)

typedef struct {
    LightLock lock;
    aptHookCookie apt;
    CtrBottomIdlePainter idle;
    uint32_t generation, paintedGeneration;
    uint8_t* shown;
    uint8_t* spare;
    Renderer* ending;
    CtrDisplaySettings settings;
    bool settingsKnown, yielded, suspended, pending, prepared, claim, owed, dirty;
    Result powerFailure;
    bool powerFailureReported;
} BpState;
static BpState* bpRoot __attribute__((section(".bss.late.paintchg_idle"))); _Static_assert(sizeof(bpRoot) == 4 || sizeof(void*) != 4, "z_sdmigrate.c carves these 4 B from smSpare (SM_CARVE_BOTTOMPAINT)");

extern void* const CtrBottomPaint_logState[5];
extern void* const CtrBottomPaint_gpuState[2];
extern void* const CtrBottomPaint_lightState[1];
extern bool CtrScreens_onBottom, CtrScreens_otherOff, CtrScreens_noticeShown;
extern struct { uint8_t cfg[3]; bool menuIsOpen; } CtrArgui_state;
extern bool CtrSave_failed;
bool CtrArgui_keepLit(u32);
bool CtrDimlight_rosalinaHeld(void);
Result CtrDimlight_powerOn(u32);
Result CtrDimlight_powerOff(u32);
static Result bpPainterPower(BpState*, bool);
static void bpReportPowerFailure(BpState*);
static void bpRejectInstall(void);
bool CtrDimlight_lcdScopeActive(void);
void CtrLog_endConsole(void);
void CtrBlank_vblank1Lock(LightLock*);
void __real_CtrBlank_yieldBottom(void*, bool);
void __real_CtrDimlight_setDisplay(Renderer*, const CtrDisplaySettings*);
void __real_CtrDimlight_noticeClear(void);
void __real_CtrScreens_screenshotNotice(bool);
void __real_CtrDimlight_rosalinaSleep(s64);
void __real_CtrRenderer_endFrame(Renderer*);
#if defined(CTRPORT_TARGET_O3DS)
void CtrRenderer_chainEndFrameBoxcache(Renderer*);
void CtrPaintchg_frameEnd(u8);
Result CtrPaintchg_cpuFlush(const void*, u32);
#endif

static BP_CODE BpState* bpReady(void) {
    BpState* p = __atomic_load_n(&bpRoot, __ATOMIC_ACQUIRE);
    return (uintptr_t) p > 1u ? p : NULL;
}
static BP_CODE void bpRevoke(BpState* p) {
    __atomic_add_fetch(&p->generation, 1u, __ATOMIC_RELEASE);
    p->pending = false;
    p->prepared = false;
    p->dirty = true;
    __atomic_store_n(&p->claim, false, __ATOMIC_RELEASE);
}
BP_CODE void CtrBottomPaint_invalidate(void) {
    BpState* p = bpReady();
    if (!p) return;
    LightLock_Lock(&p->lock);
    bpRevoke(p);
    LightLock_Unlock(&p->lock);
}
static BP_CODE void bpApt(APT_HookType event, void* arg) {
    BpState* p = arg;
    LightLock_Lock(&p->lock);
    bpRevoke(p);
    if (event == APTHOOK_ONSUSPEND || event == APTHOOK_ONSLEEP || event == APTHOOK_ONEXIT) p->suspended = true;
    if (event == APTHOOK_ONRESTORE || event == APTHOOK_ONWAKEUP) p->suspended = false;
    LightLock_Unlock(&p->lock);
}
BP_CODE bool CtrBottomPaint_setIdle(const CtrBottomIdlePainter* painter) {
    BpState* p = bpReady();
    if (!p && bpRoot == NULL && painter) {
        p = calloc(1, sizeof(*p));
        __atomic_store_n(&bpRoot, p ? p : (BpState*) 1u, __ATOMIC_RELEASE);
        if (p) { LightLock_Init(&p->lock); aptHook(&p->apt, bpApt, p); }
    }
    if (!p) return painter == NULL;
    LightLock_Lock(&p->lock);
    if (painter && (p->idle.update || p->idle.paint)) {
        LightLock_Unlock(&p->lock);
        bpRejectInstall();
        return false;
    }
    bpRevoke(p);
    if (painter) p->idle = *painter;
    else memset(&p->idle, 0, sizeof(p->idle));
    LightLock_Unlock(&p->lock);
    return true;
}
BP_CODE uint32_t CtrBottomPaint_generation(void) {
    BpState* p = bpReady();
    return p ? __atomic_load_n(&p->generation, __ATOMIC_ACQUIRE) : 0;
}
BP_CODE __attribute__((weak)) bool CtrLayout_bottomIdleAllowed(void) { return !CtrScreens_onBottom; }

static BP_CODE bool bpConsoleIdle(void) {
    return !*(const bool*) CtrBottomPaint_logState[0] &&
        __atomic_load_n((u32*) CtrBottomPaint_logState[1], __ATOMIC_ACQUIRE) == 0 &&
        __atomic_load_n((void**) CtrBottomPaint_logState[4], __ATOMIC_ACQUIRE) == NULL;
}
BP_CODE bool CtrBottomPaint_claimConsole(void) {
    if (!bpConsoleIdle()) return false;
    RecursiveLock* lock = CtrBottomPaint_logState[2];
    if (RecursiveLock_TryLock(lock) != 0) return false;
    if (!bpConsoleIdle()) { RecursiveLock_Unlock(lock); return false; }
    (*(int*) CtrBottomPaint_logState[3])++;
    return true;
}
static BP_CODE bool bpOwnersIdle(BpState* p) {
    return !p->suspended && !p->yielded && CtrScreens_otherOff && !CtrScreens_onBottom &&
        !*(const bool*) CtrBottomPaint_gpuState[1] &&
        CtrLayout_bottomIdleAllowed() && !CtrScreens_noticeShown && !CtrArgui_state.menuIsOpen &&
        !__atomic_load_n(&CtrSave_failed, __ATOMIC_ACQUIRE) && !CtrArgui_keepLit(2) &&
        !CtrDimlight_rosalinaHeld() && bpConsoleIdle();
}
static BP_CODE bool bpEligible(BpState* p, Renderer* r) {
    CtrBottomView view;
    return bpOwnersIdle(p) && BP(r, IN_FRAME) && !BP(r, SKIP) && !BP(r, SKIP_ACTIVE) &&
        (!BP(r, TARGET) || !BP(r, TARGET)->used) &&
        !BP(r, YIELDED) && !BP(r, CONFIG) && !BP(r, SHOWN) && !BP(r, DROPPED) &&
        (!BP(r, ARMED) || arrlen(BP(r, OPS)) == 0) && !BP(r, SHOT_REQUEST) &&
        !BP(r, SHOT) && BP(r, NOTICE) == 0 && !CtrLayout_bottomView(r->runner, &view);
}
// Dimlight's old optimized helper discards its Result on the no-op path.
// Check the actual owed-restore flag after its public power-on instead of
// inventing a second brightness policy or depending on that private ABI.
static BP_CODE bool bpLightRestored(void) {
    uint8_t* light = __atomic_load_n((uint8_t**) CtrBottomPaint_lightState[0], __ATOMIC_ACQUIRE);
    if ((uintptr_t) light <= 2u) return true; // disabled policy never dimmed
    return !__atomic_load_n(light + BP_DIMMED_OFFSET, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(light + BP_SUSPENDED_OFFSET, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(light + BP_RESTORE_PENDING_OFFSET, __ATOMIC_ACQUIRE);
}
BP_CODE bool CtrBottomPaint_keepLit(u32 screen) {
    BpState* p = bpReady();
    return CtrArgui_keepLit(screen) || ((screen & 2u) && p && __atomic_load_n(&p->claim, __ATOMIC_ACQUIRE));
}
static BP_CODE bool bpPages(BpState* p) {
    if (!CtrPacer_screenReady(GFX_BOTTOM) || *(const bool*) CtrBottomPaint_gpuState[0]) return false;
    if (gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return false;
    u16 w = 0, h = 0;
    uint8_t* shown = CtrPacer_shownFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    uint8_t* spare = CtrPacer_backFramebuffer(GFX_BOTTOM, GFX_LEFT);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    if (!shown || !spare || shown == spare || w != 240 || h != 320) return false;
    if (p->shown != shown || p->spare != spare) { bpRevoke(p); p->shown = shown; p->spare = spare; }
    return true;
}
static BP_CODE void bpUpdate(BpState* p, Renderer* r) {
    LightLock_Lock(&p->lock);
    bpReportPowerFailure(p);
    if (p->prepared) { p->prepared = false; p->dirty = true; }
    bool eligible = bpEligible(p, r);
    if (!eligible) {
        if (p->claim || p->pending) bpRevoke(p);
    }
    CtrBottomIdleAction action = p->idle.update ? p->idle.update(p->idle.user, r->runner, eligible, p->generation) : CTR_BOTTOM_IDLE_DIM;
    if (!eligible || !CtrBottomPaint_claimConsole()) { LightLock_Unlock(&p->lock); return; }
    if (action == CTR_BOTTOM_IDLE_DIRTY) p->dirty = true;
    if (action == CTR_BOTTOM_IDLE_DIM || !p->idle.paint) {
        bool wasClaim = p->claim;
        if (wasClaim) bpRevoke(p); // cancels a prepared/queued Frame before Dim
        if ((p->owed || wasClaim) && !p->pending && bpPages(p)) {
            // Black is part of release, including the original 2DS whose LCDs
            // share a backlight. One final CPU publish; dimlight stays owner.
            memset(p->spare, 0, BP_BYTES);
            p->prepared = R_SUCCEEDED(bpPainterPower(p, false));
            p->dirty = !p->prepared; // retain owed release until its service request succeeds
        }
    } else if (!p->pending && (p->dirty || p->paintedGeneration != p->generation || !p->claim) && bpPages(p)) {
        __atomic_store_n(&p->claim, true, __ATOMIC_RELEASE); // holds brightness during restore
        bool lit = R_SUCCEEDED(bpPainterPower(p, true)) && bpLightRestored();
        CtrBottomPaintSurface surface = { (uint16_t*) p->spare };
        if (lit && p->idle.paint(p->idle.user, &surface)) {
            p->prepared = true;
            p->dirty = false;
            p->owed = true;
        } else {
            bpRevoke(p);
            if (p->owed) memset(p->spare, 0, BP_BYTES);
            bool released = R_SUCCEEDED(bpPainterPower(p, false));
            if (p->owed && released) p->prepared = true;
        }
    }
    CtrLog_endConsole();
    LightLock_Unlock(&p->lock);
}
// This is the last endFrame link: the split/redirect owners already recorded.
BP_CODE void CtrBottomPaint_endFrame(Renderer* r) {
    BpState* p = bpReady();
    if (p) { bpUpdate(p, r); p->ending = r; }
#if defined(CTRPORT_TARGET_O3DS)
    CtrRenderer_chainEndFrameBoxcache(r);
#else
    __real_CtrRenderer_endFrame(r);
#endif
    if (p) p->ending = NULL;
}
// Final composition decides: a live bottom target always wins, even if a
// future compositor did not expose its recording through today's predicate.
BP_CODE void CtrBottomPaint_frameEnd(u8 flags) {
    BpState* p = bpReady();
    if (p && p->ending) {
        LightLock_Lock(&p->lock);
        if (BP(p->ending, TARGET) && BP(p->ending, TARGET)->used) {
            if (p->claim || p->pending || p->prepared) bpRevoke(p);
        } else if (p->prepared) { p->prepared = false; p->pending = true; }
        LightLock_Unlock(&p->lock);
    }
#if defined(CTRPORT_TARGET_O3DS)
    CtrPaintchg_frameEnd(flags);
#else
    C3D_FrameEnd(flags);
#endif
}
// Called in place of dlgblank's lock call. It still takes that SAME gate,
// updates dlgblank's clock and returns holding the gate, as its caller expects.
// No wait for the painter/console: a busy owner postpones or cancels publish.
BP_CODE void CtrBottomPaint_vblank1Lock(LightLock* gate) {
    CtrBlank_vblank1Lock(gate);
    BpState* p = bpReady();
    if (!p || LightLock_TryLock(&p->lock) != 0) return;
    if (p->pending) {
        if (!bpOwnersIdle(p)) bpRevoke(p);
        else if (!*(const bool*) CtrBottomPaint_gpuState[0] && CtrPacer_screenReady(GFX_BOTTOM) && CtrBottomPaint_claimConsole()) {
            u32 line = 0;
            if (R_SUCCEEDED(GSPGPU_ReadHWRegs(0x400554, &line, sizeof(line))) && (line < 40u || line >= 403u)) {
                memcpy(p->shown, p->spare, BP_BYTES);
                GSPGPU_FlushDataCache(p->shown, BP_BYTES);
                p->pending = false;
                p->paintedGeneration = p->generation;
                if (!p->claim) p->owed = false;
            }
            CtrLog_endConsole();
        }
    }
    LightLock_Unlock(&p->lock);
}
BP_CODE void __wrap_CtrBlank_yieldBottom(void* r, bool yield) {
    BpState* p = bpReady();
    if (p) {
        LightLock_Lock(&p->lock);
        if (p->yielded != yield) { bpRevoke(p); p->yielded = yield; }
        LightLock_Unlock(&p->lock);
    }
    __real_CtrBlank_yieldBottom(r, yield);
}
BP_CODE void __wrap_CtrDimlight_setDisplay(Renderer* r, const CtrDisplaySettings* settings) {
    BpState* p = bpReady();
    if (p) {
        LightLock_Lock(&p->lock);
        if (!p->settingsKnown || memcmp(&p->settings, settings, sizeof(*settings))) {
            bpRevoke(p); p->settings = *settings; p->settingsKnown = true;
        }
        LightLock_Unlock(&p->lock);
    }
    __real_CtrDimlight_setDisplay(r, settings);
}
BP_CODE void __wrap_CtrDimlight_noticeClear(void) {
    CtrBottomPaint_invalidate();
    __real_CtrDimlight_noticeClear();
}
BP_CODE void __wrap_CtrScreens_screenshotNotice(bool paint) {
    CtrBottomPaint_invalidate();
    __real_CtrScreens_screenshotNotice(paint);
}
BP_CODE void __wrap_CtrDimlight_rosalinaSleep(s64 ns) {
    if (CtrDimlight_rosalinaHeld()) CtrBottomPaint_invalidate();
    __real_CtrDimlight_rosalinaSleep(ns);
    if (CtrDimlight_rosalinaHeld()) CtrBottomPaint_invalidate();
}
BP_CODE Result CtrBottomPaint_cpuFlush(const void* data, u32 bytes) {
    BpState* p = bpReady();
    if (p && p->shown) {
        uintptr_t from = (uintptr_t) data, shown = (uintptr_t) p->shown, spare = (uintptr_t) p->spare;
        if ((from <= shown ? shown - from < bytes : from - shown < BP_BYTES) ||
            (from <= spare ? spare - from < bytes : from - spare < BP_BYTES)) CtrBottomPaint_invalidate();
    }
#if defined(CTRPORT_TARGET_O3DS)
    return CtrPaintchg_cpuFlush(data, bytes);
#else
    return GSPGPU_FlushDataCache(data, bytes);
#endif
}
#endif

// Painter power uses dimlight's existing nonblocking, thread-owned public LCD
// scope, including nested brightness restores. No private service handle or
// second session owner. Log only after releasing the scope (dimlight's rule).
#if defined(ENABLE_WAD16)
static const char bpPowerFailure[] __attribute__((section(".paintchg_lcdscope_const"))) =
    "Bottom painter: LCD error %08lX\n";
static __attribute__((noinline, section(".paintchg_lcdscope_code"))) void bpReportPowerFailure(BpState* p) {
    if (R_FAILED(p->powerFailure) && !p->powerFailureReported && !CtrDimlight_lcdScopeActive()) {
        p->powerFailureReported = true;
        logWarn(bpPowerFailure, (unsigned long) (u32) p->powerFailure);
    }
}
static __attribute__((noinline, section(".paintchg_lcdscope_code"))) Result bpPainterPower(BpState* p, bool on) {
    Result result = gspLcdInit(); // globally wrapped by dimlight, reuses its SDK session
    if (R_SUCCEEDED(result)) {
        result = on ? CtrDimlight_powerOn(2) : CtrDimlight_powerOff(2);
        gspLcdExit(); // retains Rosalina's wrapper and dimlight's release/retry path
    }
    if (R_FAILED(result) && !R_FAILED(p->powerFailure)) p->powerFailure = result;
    bpReportPowerFailure(p);
    return result;
}
static const char bpDuplicatePainter[] __attribute__((section(".paintchg_lcdscope_const"))) =
    "Bottom painter: already installed\n";
static __attribute__((noinline, section(".paintchg_lcdscope_code"))) void bpRejectInstall(void) {
    logWarn(bpDuplicatePainter);
}
#endif
