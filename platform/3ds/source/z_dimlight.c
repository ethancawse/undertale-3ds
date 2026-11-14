// Empty-bottom brightness policy (dimlight). UNDERTALE only.
//
// Power call targets change, not their callers' instructions. The existing
// ctr_screens wrappers retain power tracking and the Rosalina hold. Keep-lit's
// pure predicate and renderer conversation hold are unchanged. Raw requests
// reuse libctru's reference-counted LCD session, including error/exit cleanup.
// One approved 4 B late-BSS root owns a process-lifetime heap allocation.
// Allocation failure leaves the original screen policy and player brightness
// untouched; no raw request or dimlight APT hook can run without this state.
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_dimlight_logic.h"
#include "ctr_display.h"
#include "ctr_renderer.h"
#include "ctr_startup_clock.h"
#include "ctr_wrap_chain.h"
#include "log.h"

// These public names are retargeted by CMake for other source files. This unit
// explicitly calls the existing wrappers, avoiding recursion through itself.
#undef GSPLCD_PowerOnBacklight
#undef GSPLCD_PowerOffBacklight
#undef gfxExit
#define DL_CODE __attribute__((noinline, section(".text.late.dimlight")))
#define DL_RO __attribute__((section(".rodata.late.dimlight")))
#define DL_BSS __attribute__((section(".bss.late.dimlight")))
#define DL_DUTY 0x202a40u
#define DL_PWM 0x202a44u

extern bool CtrScreens_sharedBacklight, CtrScreens_otherOff;
extern uint8_t CtrScreens_game;
bool CtrScreens_backlightShared(CtrDisplayCaps caps);
bool CtrArgui_keepLit(u32 screen);
Result __wrap_GSPLCD_PowerOnBacklight(u32 screen);
Result __wrap_GSPLCD_PowerOffBacklight(u32 screen);
void __wrap_gfxExit(void);
void CtrKeeplit_setDisplay(Renderer*, const CtrDisplaySettings*);
void __wrap_CtrArgui_load(CtrDisplaySettings*, const char*, CtrDisplayCaps);
bool __wrap_CtrArgui_override(CtrDisplaySettings*, const char*, CtrDisplayCaps);
bool Loadgame_prepare(void);
void fpsboxFlushConsoles(void);
bool CtrArgcrash_quiet(void);
void CtrScanout_noticeClear(void);
void CtrRosabl_sleep(s64 ns);
bool CtrDimlight_rosalinaHeld(void);
void CtrDimlight_startWatcher(void);
void CtrDimlight_retryWatcher(void);
extern bool CtrScreens_topBacklightOff;

typedef struct {
    struct {
        DimCalibration calibration;
        aptHookCookie hook;
        u32 restoreRaw, savedDuty, savedPwm;
        bool initialized, valid, dimmed, empty, keep;
        bool suspended, stopped, bootApplied;
        bool loadedOff, loadedDialogue;
        bool failureReported;
        Result pendingFailure;
        u32 pendingPower; // pending screen mask (bits 0..1), wanted-on mask (bits 2..3)
        bool retrying, restorePending;
        uintptr_t gameThread;
        uint8_t menu; // 0 idle, 1 Rosalina hold, 2 returned: capture on game thread
    } policy;
    // libctru publishes its reference count before a cold open completes.
    // Serialize the entire public scope; nesting on its owning thread is safe.
    struct {
        uintptr_t owner;
        u32 depth, summaries;
        u32 summaryRaw[4];
    } lcd;
    u32 watcherPending;
#ifdef CTRPORT_DIMLIGHT_DIAG
    struct {
        u32 frame, published, painted;
        u32 requestSerial, raw0Serial, floorStart, floorSerial, floorDuty, floorPwm;
        Renderer* ending;
        CtrDisplaySettings settings;
        bool firstRunning, fps, box, active, settingsKnown, floorValid;
        u32 readback[7];
        bool readbackPending;
    } diagnostic;
#endif
} DimlightHeap;

static DimlightHeap* dlRoot DL_BSS;
#define dl (dlRoot->policy)
#define dlLcd (dlRoot->lcd)
#ifdef CTRPORT_DIMLIGHT_DIAG
#define dd (dlRoot->diagnostic)
#endif

DL_CODE static bool dlReady(void) {
    return (uintptr_t) __atomic_load_n(&dlRoot, __ATOMIC_ACQUIRE) > 2u;
}

DL_CODE static bool dlEnsure(void) {
    DimlightHeap* expected = nullptr;
    // Sentinels 1/2 mean allocating/disabled. Another first caller never spins
    // or retries an allocation, including on the crash path. Publish only
    // complete state; a failed allocation disables this policy for the process.
    if (__atomic_compare_exchange_n(&dlRoot, &expected, (DimlightHeap*) 1,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        DimlightHeap* state = calloc(1, sizeof(*state));
        if (!state) {
            __atomic_store_n(&dlRoot, (DimlightHeap*) 2, __ATOMIC_RELEASE);
            return false;
        }
        state->policy.restoreRaw = UINT32_MAX;
        expected = (DimlightHeap*) 1;
        if (__atomic_compare_exchange_n(&dlRoot, &expected, state, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return true;
        free(state); // crash/exit canceled publication; no hook holds this block
        return false;
    }
    if (expected == (DimlightHeap*) 1) {
        // A concurrent first user must retain the base behavior, not acquire
        // an untracked LCD scope which becomes tracked before its release.
        // Cancel publication and leave dimlight off for this process instead.
        __atomic_compare_exchange_n(&dlRoot, &expected, (DimlightHeap*) 2,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return (uintptr_t) expected > 2u;
}

DL_CODE static bool dlStopReady(void) {
    DimlightHeap* state = __atomic_load_n(&dlRoot, __ATOMIC_ACQUIRE);
    while ((uintptr_t) state <= 2u) {
        // A fault before first use must also prevent later first allocation.
        // Cancel an in-flight allocation without waiting for the heap lock.
        if (__atomic_compare_exchange_n(&dlRoot, &state, (DimlightHeap*) 2,
                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
    }
    return true;
}

// The watcher's retry slot shares this allocation; no second static root or
// allocation is needed. Without it the existing private power request runs
// once, just as it did before dimlight supplied retries.
DL_CODE u32* CtrDimlight_watcherPending(void) {
    return dlReady() ? &dlRoot->watcherPending : nullptr;
}

static const char dlCaptureLog[] DL_RO = "Dimlight: capture valid=%d raw=%lu duty=%lu pwm=%08lX\n";
static const char dlRequestLog[] DL_RO = "Dimlight: raw %lu rc=%08lX at %llu us\n";
static const char dlBootLog[] DL_RO = "Dimlight: boot decision=%d shared=%d owner=%d rc=%08lX at %llu us\n";
static const char dlRestoreFail[] DL_RO = "Dimlight: LCD request failed (%08lX); dimming disabled\n";
static const char dlService[] DL_RO = "gsp::Lcd";
DL_CODE static void dlRetry(bool frame);

DL_CODE static void dlQueuePower(u32 screen, bool on) {
    if (!on && __atomic_load_n(&dl.suspended, __ATOMIC_ACQUIRE)) return;
    screen &= 3u;
    u32 before = __atomic_load_n(&dl.pendingPower, __ATOMIC_ACQUIRE), after;
    do {
        after = (before & ~(screen << 2)) | screen | (on ? screen << 2 : 0);
    } while (!__atomic_compare_exchange_n(&dl.pendingPower, &before, after, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

DL_CODE static void dlRequeuePower(u32 screen, bool on) {
    // A newer request can arrive from a failed acquire on another thread while
    // a retry's IPC is in flight. Requeue only if it has not left newer work.
    u32 before = __atomic_load_n(&dl.pendingPower, __ATOMIC_ACQUIRE), after;
    do {
        if (!on && __atomic_load_n(&dl.suspended, __ATOMIC_ACQUIRE)) return;
        if (before & screen) return;
        after = (before & ~(screen << 2)) | screen | (on ? screen << 2 : 0);
    } while (!__atomic_compare_exchange_n(&dl.pendingPower, &before, after, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

DL_CODE static void dlDropOff(void) {
    u32 before = __atomic_load_n(&dl.pendingPower, __ATOMIC_ACQUIRE), after;
    do {
        u32 off = (before & 3u) & ~(before >> 2);
        after = before & ~(off * 5u);
    } while (!__atomic_compare_exchange_n(&dl.pendingPower, &before, after, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
}

// Context-specific no-argument targets preserve existing callers' instructions
// and slots. A caller which skips its command after a failed open still leaves
// enough information to retry; merely wrapping Init cannot recover that intent.
DL_CODE static Result dlIntentInit(u32 screen, bool on) {
    Result result = gspLcdInit();
    if (R_FAILED(result) && dlReady()) dlQueuePower(screen, on);
    return result;
}
DL_CODE Result CtrDimlight_topOnInit(void) { return dlIntentInit(1, true); }
DL_CODE Result CtrDimlight_bottomOnInit(void) { return dlIntentInit(2, true); }
DL_CODE Result CtrDimlight_topOffInit(void) { return dlIntentInit(1, false); }
DL_CODE Result CtrDimlight_allOnInit(void) { return dlIntentInit(3, true); }
DL_CODE Result CtrDimlight_lightsInit(u32 screen) { return dlIntentInit(screen, true); }

// The renderer supplies the effective request (after the shared-panel/caps
// check) when its configure-only top-screen call could not acquire the scope.
DL_CODE void CtrDimlight_queueTop(bool off) { if (dlEnsure()) dlQueuePower(1, !off); }

DL_CODE static void dlSupersede(u32 screen) {
    // A newer command issued under the public owner supersedes queued work.
    if (__atomic_load_n(&dl.retrying, __ATOMIC_ACQUIRE)) return;
    __atomic_fetch_and(&dl.pendingPower, ~((screen & 3u) * 5u), __ATOMIC_ACQ_REL);
}

DL_CODE static void dlReportFailure(void) {
    // No log lock or filesystem operation inside an LCD scope, including a
    // caller's outer scope. The first failed request is enough for diagnosis;
    // a pending crash painter may retry every frame without logging each time.
    if (__atomic_load_n(&dlLcd.owner, __ATOMIC_ACQUIRE)) return;
    Result result = __atomic_exchange_n(&dl.pendingFailure, 0, __ATOMIC_ACQ_REL);
    if (R_FAILED(result) && !__atomic_exchange_n(&dl.failureReported, true, __ATOMIC_ACQ_REL))
        logWarn(dlRestoreFail, (unsigned long) result);
}

DL_CODE static Result dlRaw(u32 raw) {
    // The renderer may already own gspLcdInit's session. A second independent
    // SRV acquisition can wait for that very session to close. Nest the public
    // refcount instead; never hold a second LCD service session for this call.
    Result result = gspLcdInit();
    if (R_FAILED(result)) {
        __atomic_store_n(&dl.pendingFailure, result, __ATOMIC_RELEASE);
        dlReportFailure();
        return result;
    }
    Handle lcd = *gspLcdGetSessionHandle();
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(0xa, 2, 0);
    cmd[1] = GSPLCD_SCREEN_BOTTOM;
    cmd[2] = raw;
    result = svcSendSyncRequest(lcd);
    if (R_SUCCEEDED(result)) result = (Result) cmd[1];
    if (R_FAILED(result)) __atomic_store_n(&dl.pendingFailure, result, __ATOMIC_RELEASE);
    else if (dlLcd.summaries < 4) dlLcd.summaryRaw[dlLcd.summaries++] = raw;
    gspLcdExit();
    return result;
}

DL_CODE static void dlCapture(void) {
    u32 duty = 0, pwm = 0, raw = 0;
    bool valid = R_SUCCEEDED(GSPGPU_ReadHWRegs(DL_DUTY, &duty, sizeof(duty))) &&
                 R_SUCCEEDED(GSPGPU_ReadHWRegs(DL_PWM, &pwm, sizeof(pwm))) &&
                 (pwm & 0x10000u) && dimRestoreRaw(&dl.calibration, pwm, duty, &raw);
    dl.valid = valid;
    if (valid) { dl.restoreRaw = raw; dl.savedDuty = duty; dl.savedPwm = pwm; }
    logInfo(dlCaptureLog, valid, (unsigned long) raw, (unsigned long) duty, (unsigned long) pwm);
}

DL_CODE static Result dlRestore(void) {
    if (!__atomic_load_n(&dl.dimmed, __ATOMIC_ACQUIRE)) return 0;
    __atomic_store_n(&dl.restorePending, false, __ATOMIC_RELEASE);
    Result result = dlRaw(dl.restoreRaw);
    if (R_SUCCEEDED(result)) __atomic_store_n(&dl.dimmed, false, __ATOMIC_RELEASE);
    else {
        __atomic_store_n(&dl.stopped, true, __ATOMIC_RELEASE);
        __atomic_store_n(&dl.restorePending, true, __ATOMIC_RELEASE);
        // A later owner can retry, but LCD acquisition never waits for capacity.
        // Keep the saved value and owed flag; do not guess a fallback brightness.
    }
    return result;
}

DL_CODE static Result dlDim(void) {
    if (!dl.valid || dl.suspended || __atomic_load_n(&dl.menu, __ATOMIC_ACQUIRE) || __atomic_load_n(&dl.stopped, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&dl.dimmed, __ATOMIC_ACQUIRE)) return 0;
    // Mark first: a fault or HOME inside the call must still owe a restore.
    __atomic_store_n(&dl.dimmed, true, __ATOMIC_RELEASE);
    Result result = dlRaw(0); // supported service applies its own calibrated floor; no PWM writes
    if (R_FAILED(result)) {
        __atomic_store_n(&dl.stopped, true, __ATOMIC_RELEASE);
        dlRestore();
    }
    // A crash on another thread can have restored while this request was in
    // flight. Send the saved value again after it, never leave a late dim last.
    if (__atomic_load_n(&dl.stopped, __ATOMIC_ACQUIRE) || dl.suspended ||
        __atomic_load_n(&dl.menu, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&dl.dimmed, true, __ATOMIC_RELEASE);
        dlRestore();
    }
    return result;
}

DL_CODE static void dlApt(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) {
        __atomic_store_n(&dl.suspended, true, __ATOMIC_RELEASE);
        dlDropOff();
        dlRestore();
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        // An unrestored dim is still our value. Never capture it as the user's.
        if (!__atomic_load_n(&dl.dimmed, __ATOMIC_ACQUIRE)) dlCapture();
        __atomic_store_n(&dl.suspended, false, __ATOMIC_RELEASE);
        dl.empty = false; // renderer's dirty display issues the next ownership request
    }
}

DL_CODE static void dlInit(void) {
    if (dl.initialized) return;
    dl.initialized = true;
    dl.gameThread = (uintptr_t) getThreadLocalStorage();
    // Before the renderer has detected the original 2DS, fail closed on either
    // unknown or shared model. The renderer will query again as before.
    CtrScreens_sharedBacklight = CtrScreens_backlightShared(CtrDisplay_queryCaps());
    aptHook(&dl.hook, dlApt, nullptr);
    if (CtrScreens_sharedBacklight) return;
    if (R_SUCCEEDED(cfguInit())) {
        Result result = CFG_GetConfigInfoBlk8(sizeof(dl.calibration), 0x50002, &dl.calibration);
        cfguExit();
        if (R_SUCCEEDED(result)) dlCapture();
    }
}

// The original hint/FPS/menu predicate, without keep-lit's conversation term.
DL_CODE static bool dlOwner(void) {
    return CtrArgui_keepLit(GSPLCD_SCREEN_BOTTOM) ||
           (CtrScreens_game == 0 && !dl.loadedOff);
}

DL_CODE Result CtrDimlight_powerOn(u32 screen) {
    if (!dlEnsure()) return __wrap_GSPLCD_PowerOnBacklight(screen);
    dlSupersede(screen);
    if (screen & GSPLCD_SCREEN_BOTTOM) {
        dl.empty = false;
        dlRestore();
    }
    Result result = __wrap_GSPLCD_PowerOnBacklight(screen);
    if (R_FAILED(result)) {
        if (dl.retrying) dlRequeuePower(screen, true);
        else dlQueuePower(screen, true);
    }
    return result;
}

DL_CODE Result CtrDimlight_powerOff(u32 screen) {
    if (!dlEnsure()) return __wrap_GSPLCD_PowerOffBacklight(screen);
    dlSupersede(screen);
    if (screen & GSPLCD_SCREEN_BOTTOM) dl.empty = true;
    if (__atomic_load_n(&dl.stopped, __ATOMIC_ACQUIRE))
        return CtrDimlight_powerOn(screen);
    if (screen == GSPLCD_SCREEN_BOTTOM && dl.keep && !CtrScreens_sharedBacklight && !dlOwner() &&
        !dl.suspended && !__atomic_load_n(&dl.stopped, __ATOMIC_ACQUIRE)) {
        // Keep the existing power tracker and Rosalina request up to date. The
        // old on wrapper is called directly, so it cannot undo the following dim.
        Result result = __wrap_GSPLCD_PowerOnBacklight(screen);
        if (R_SUCCEEDED(result)) dlDim();
        else if (dl.retrying) dlRequeuePower(screen, false);
        else dlQueuePower(screen, false); // retry the keep-lit request, including its dim
        return result;
    }
    if (screen & GSPLCD_SCREEN_BOTTOM) dlRestore();
    Result result = __wrap_GSPLCD_PowerOffBacklight(screen);
    if (R_FAILED(result)) {
        if (dl.retrying) dlRequeuePower(screen, false);
        else dlQueuePower(screen, false);
    }
    return result;
}

DL_CODE void CtrDimlight_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    if (!dlEnsure()) { CtrKeeplit_setDisplay(renderer, settings); return; }
    dlInit();
    dl.loadedOff = CtrScreens_otherOff;
    dl.keep = CtrScreens_otherOff && !CtrScreens_sharedBacklight && settings->dialogueBottom;
    if (!dl.keep) dlRestore();
    CtrKeeplit_setDisplay(renderer, settings);
}

DL_CODE void CtrDimlight_flushConsoles(void) {
    if (!dlReady()) { fpsboxFlushConsoles(); return; }
    dlRetry(true);
    dlReportFailure();
    // The watcher restores before a menu can pause the app. Recapture changes
    // made in Rosalina on this game thread, serialized with APT callbacks.
    // Keep the dim gate closed throughout capture. A new hold wins the CAS.
    if (__atomic_load_n(&dl.menu, __ATOMIC_ACQUIRE) == 2 && !dl.suspended &&
        !__atomic_load_n(&dl.stopped, __ATOMIC_ACQUIRE)) {
        if (dl.initialized && !CtrScreens_sharedBacklight &&
            !__atomic_load_n(&dl.dimmed, __ATOMIC_ACQUIRE)) dlCapture();
        uint8_t expected = 2;
        __atomic_compare_exchange_n(&dl.menu, &expected, 0, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    // The hint can expire without applyBottomBacklight changing its cached off
    // flag. Likewise FPS can take over a held-lit blank. Reconcile before paint.
    if (dl.empty && dl.keep && !CtrScreens_sharedBacklight) {
        if (dlOwner()) dlRestore();
        else dlDim();
    }
    fpsboxFlushConsoles();
}

DL_CODE static void dlLoaded(const CtrDisplaySettings* settings) {
    if (!dlEnsure()) return;
    dl.loadedOff = settings->bottomMode == BOTTOM_MODE_OFF;
    dl.loadedDialogue = settings->dialogueBottom;
}

DL_CODE void CtrDimlight_load(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    __wrap_CtrArgui_load(settings, path, caps);
    dlLoaded(settings);
}

DL_CODE bool CtrDimlight_override(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    bool result = __wrap_CtrArgui_override(settings, line, caps);
    dlLoaded(settings);
    return result;
}

DL_CODE bool CtrDimlight_prepare(void) {
    bool result = Loadgame_prepare(); // its settings read, then all ordered CLI overrides
    if (!dlEnsure()) return result;
    if (!dl.bootApplied) {
        dl.bootApplied = true;
        dlInit();
        bool owner = CtrScreens_game != 0 || !dl.loadedOff || dlOwner();
        dl.keep = dl.loadedOff && dl.loadedDialogue && CtrScreens_game == 0 && !CtrScreens_sharedBacklight;
        Result rc = 0;
        int decision = 0; // 0 base timing, 2 early dim/keep-lit (never early real-off)
        if (dl.keep && !owner && !__atomic_load_n(&dl.stopped, __ATOMIC_ACQUIRE)) {
            decision = 2;
            rc = gspLcdInit();
            if (R_SUCCEEDED(rc)) { rc = CtrDimlight_powerOff(GSPLCD_SCREEN_BOTTOM); gspLcdExit(); }
        }
        logInfo(dlBootLog, decision, CtrScreens_sharedBacklight, owner, (unsigned long) rc,
                (unsigned long long) CtrStartup_elapsedUs());
    }
    return result;
}

DL_CODE bool CtrDimlight_crashQuiet(void) {
    if (!dlStopReady()) return CtrArgcrash_quiet();
    __atomic_store_n(&dl.stopped, true, __ATOMIC_RELEASE);
    dlRestore();
    return CtrArgcrash_quiet();
}

DL_CODE void CtrDimlight_noticeClear(void) {
    // setupNotices may run before a renderer exists; its first visible paint
    // must claim the light even after the early empty-screen decision.
    if (R_SUCCEEDED(CtrDimlight_bottomOnInit())) {
        CtrDimlight_powerOn(GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
    CtrScanout_noticeClear();
}

DL_CODE void CtrDimlight_gfxExit(void) {
    if (!dlStopReady()) { __wrap_gfxExit(); return; }
    __atomic_store_n(&dl.stopped, true, __ATOMIC_RELEASE);
    dlRestore();
    __wrap_gfxExit();
}

DL_CODE __attribute__((noreturn)) void CtrDimlight_exit(int status) {
    // z_leavekeep calls this only after it has claimed exit ownership. A second
    // exiting thread can now reach its existing 15 s fallback even if this LCD
    // restore stalls. Never place LCD work ahead of the exitonce claim again.
    void CtrAptTrace_exit(int) __attribute__((noreturn));
    if (dlStopReady()) {
        __atomic_store_n(&dl.stopped, true, __ATOMIC_RELEASE);
        dlRestore();
    }
    CtrAptTrace_exit(status);
}

// Existing watcher cadence, combo filtering and grace are unchanged. Its
// private power-on cannot restore raw brightness, so observe its held state
// even when the bottom is already powered (and bottomDark() is false).
DL_CODE void CtrDimlight_rosalinaSleep(s64 ns) {
    CtrRosabl_sleep(ns);
    if (!dlReady()) return;
    dlRetry(false);
    CtrDimlight_retryWatcher();
    if (CtrDimlight_rosalinaHeld()) {
        __atomic_store_n(&dl.menu, 1, __ATOMIC_RELEASE);
        dlRestore();
    } else {
        uint8_t expected = 1;
        __atomic_compare_exchange_n(&dl.menu, &expected, 2, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
}

DL_CODE static void dlRetry(bool frame) {
    if (__atomic_load_n(&dlLcd.owner, __ATOMIC_ACQUIRE) ||
        __atomic_exchange_n(&dl.retrying, true, __ATOMIC_ACQ_REL)) return;
    // No wait or recursive drain. A lost acquire leaves work for the next
    // release, frame or 5 ms watcher poll. Restore takes priority over power.
    if (__atomic_load_n(&dl.restorePending, __ATOMIC_ACQUIRE)) dlRestore();
    u32 pending = __atomic_exchange_n(&dl.pendingPower, 0, __ATOMIC_ACQ_REL);
    // Newest APT hooks run first: the light-up hook may release its LCD scope
    // before dlApt marks suspension. Neither that release nor the watcher may
    // replay an Off. Only the game's frame drain can do so; suspend drops it.
    bool allowOff = frame && dl.gameThread == (uintptr_t) getThreadLocalStorage();
    for (u32 screen = 1; screen <= 2; screen <<= 1) {
        if ((pending & screen) && !(pending & (screen << 2)) &&
            (!allowOff || __atomic_load_n(&dl.suspended, __ATOMIC_ACQUIRE))) {
            dlRequeuePower(screen, false); // discarded while suspended
            pending &= ~(screen * 5u);
        }
    }
    if (pending & 3u) {
        Result result = gspLcdInit();
        for (u32 screen = 1; screen <= 2; screen <<= 1) {
            if (!(pending & screen)) continue;
            bool on = (pending & (screen << 2)) != 0;
            if (!on && __atomic_load_n(&dl.suspended, __ATOMIC_ACQUIRE)) continue;
            if (R_FAILED(result)) dlRequeuePower(screen, on);
            else {
                Result power = on ? CtrDimlight_powerOn(screen) : CtrDimlight_powerOff(screen);
                if (screen == 1 && R_SUCCEEDED(power)) CtrScreens_topBacklightOff = !on;
            }
        }
        if (R_SUCCEEDED(result)) gspLcdExit();
    }
    __atomic_store_n(&dl.retrying, false, __ATOMIC_RELEASE);
}

#ifdef CTRPORT_DIMLIGHT_DIAG
#include "ctr_dimlight_diag.inc"
#endif

// The public LCD session's cold open must not wait behind another owner,
// including during a restore. Wrap only acquisition of this service; other
// service requests retain libctru's policy and environment-handle behavior.
// This sits after the diagnostic's syscall alias: SRV IPC is not a raw-brightness
// diagnostic sample. No thread's srvSetBlockingPolicy/TLS setting is changed.
#undef svcSendSyncRequest
Result svcSendSyncRequest(Handle session);
Result __real_srvGetServiceHandle(Handle* out, const char* name);
// srvGetServiceHandle already supplies a strong reference to this SDK helper.
// Our additional reference must not extract env.o earlier and move warm text.
#pragma weak envGetHandle

DL_CODE Result __wrap_srvGetServiceHandle(Handle* out, const char* name) {
    if (strcmp(name, dlService) != 0 || !dlReady()) return __real_srvGetServiceHandle(out, name);
    Handle inherited = envGetHandle(name);
    if (inherited) return svcDuplicateHandle(out, inherited);
    Handle srv = *srvGetSessionHandle();
    u32* cmd = getThreadCommandBuffer();
    // SRV GetServiceHandle's nonblocking flag does not cover an unregistered
    // service. Check registration first, then request with zero capacity wait.
    // Public libctru srv.c: IsServiceRegistered (0xe), GetServiceHandle (5).
    cmd[0] = IPC_MakeHeader(0xe, 3, 0);
    memcpy(&cmd[1], dlService, 8);
    cmd[3] = 8;
    Result result = svcSendSyncRequest(srv);
    if (R_FAILED(result)) return result;
    result = (Result) cmd[1];
    if (R_FAILED(result)) return result;
    if (!(cmd[2] & 0xffu)) return (Result) 0xd0401834u;
    cmd[0] = IPC_MakeHeader(5, 4, 0);
    memcpy(&cmd[1], dlService, 8);
    cmd[3] = 8;
    cmd[4] = 1; // nonblocking, including cleanup retries: 0 ms capacity wait
    result = svcSendSyncRequest(srv);
    if (R_SUCCEEDED(result)) {
        result = (Result) cmd[1];
        if (R_SUCCEEDED(result)) *out = cmd[3];
    }
    return result;
}

Result __real_gspLcdInit(void);
void __real_gspLcdExit(void);

DL_CODE Result __wrap_gspLcdInit(void) {
    bool ready = dlEnsure();
    if (ready) {
        uintptr_t self = (uintptr_t) getThreadLocalStorage(), expected = 0;
        if (!__atomic_compare_exchange_n(&dlLcd.owner, &expected, self, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && expected != self)
            return (Result) 0xd0401834u; // another thread owns it: 0 ms wait, no SDK call
    }
    Result result = __real_gspLcdInit();
    if (ready) {
        if (R_SUCCEEDED(result)) dlLcd.depth++;
        else if (!dlLcd.depth) __atomic_store_n(&dlLcd.owner, 0, __ATOMIC_RELEASE);
    }
    return result;
}

// Called by Rosalina's existing __wrap_gspLcdExit. Do not replace that wrapper:
// it must still observe releases and start its watcher when the bottom is dark.
DL_CODE void CtrDimlight_lcdExit(void) {
    bool ready = dlReady();
    uintptr_t self = (uintptr_t) getThreadLocalStorage();
    if (ready && (__atomic_load_n(&dlLcd.owner, __ATOMIC_ACQUIRE) != self || !dlLcd.depth)) return;
    __real_gspLcdExit();
    if (!ready) return;
    if (!--dlLcd.depth) {
        // Copy before dropping ownership: another thread can open a new scope
        // as soon as it is released. Never access that scope's pending array.
        u32 count = dlLcd.summaries, raw[4];
        memcpy(raw, dlLcd.summaryRaw, count * sizeof(*raw));
        dlLcd.summaries = 0;
#ifdef CTRPORT_DIMLIGHT_DIAG
        u32 readback[7];
        bool reportReadback = CtrDimdiag_takeRaw(readback);
#endif
        __atomic_store_n(&dlLcd.owner, 0, __ATOMIC_RELEASE);
#ifdef CTRPORT_DIMLIGHT_DIAG
        if (reportReadback) CtrDimdiag_reportRaw(readback);
#endif
        dlRetry(false);
        if (dl.dimmed && !dl.stopped) CtrDimlight_startWatcher();
        for (u32 i = 0; count > i; ++i)
            logInfo(dlRequestLog, (unsigned long) raw[i], 0UL,
                    (unsigned long long) CtrStartup_elapsedUs());
        dlReportFailure();
    }
}

// Observe dimlight's actual restore outcome; no duplicate brightness policy.
#include "z_bottompaint_layout.h"
_Static_assert(offsetof(DimlightHeap, policy.dimmed) == BP_DIMMED_OFFSET &&
    offsetof(DimlightHeap, policy.suspended) == BP_SUSPENDED_OFFSET &&
    offsetof(DimlightHeap, policy.restorePending) == BP_RESTORE_PENDING_OFFSET, "bottompaint dimlight ABI");
__asm__(
    ".pushsection .rodata.paintchg_idle_late,\"a\",%progbits\n.balign 4\n"
    ".global CtrBottomPaint_lightState\n.type CtrBottomPaint_lightState,%object\n"
    "CtrBottomPaint_lightState:\n.word dlRoot\n"
    ".size CtrBottomPaint_lightState,.-CtrBottomPaint_lightState\n.popsection\n"
);

// Provider diagnostics follow the same no-log-inside-any-LCD-scope rule as
// dlReportFailure. Read only; the existing owner and session remain unchanged.
__attribute__((noinline, section(".paintchg_lcdscope_code"))) bool CtrDimlight_lcdScopeActive(void) {
    return dlReady() && __atomic_load_n(&dlLcd.owner, __ATOMIC_ACQUIRE) != 0;
}

// battlesplit-rework (change battlesplit, 11-10, 4(i); c26rev5 F3): dev builds only (CTRPORT_BSR_LOG: no release
// source URL), so a release build carries none of it. Up to four backlight requests made inside each outermost LCD
// scope this thread owns, printed once that scope is closed (no log inside an LCD scope): the screen (1 top, 2 bottom,
// 3 both), what was asked, the LCD call's result and its caller (a return address in the ELF). Plus a renderer LCD open
// that failed because another thread held the session (z_battlesplitbsr.c, each one). Reached by renamed
// calls (CMakeLists.txt: the global power defines and Rosalina's release continuation), the existing functions by
// their assembler names, so nothing above changes. Code and constants in late text, the records in late .bss (both
// appended, in the .bsrdev sections).
#ifdef CTRPORT_BSR_LOG
#define DL_DEVBSR __attribute__((noinline, noipa, section(".text_bsrdev")))
#define DL_DEVBSR_RO __attribute__((section(".rodata_bsrdev_in_text")))
Result dlPowerOnLinkBsr(u32 screen) __asm__("CtrDimlight_powerOn");
Result dlPowerOffLinkBsr(u32 screen) __asm__("CtrDimlight_powerOff");
void dlLcdExitLinkBsr(void) __asm__("CtrDimlight_lcdExit");
Result CtrDimlight_powerOnBsr(u32 screen);
Result CtrDimlight_powerOffBsr(u32 screen);
void CtrDimlight_lcdExitBsr(void);
void CtrDimlight_openFailedBsr(Result result, uintptr_t caller);
static const char dlPowerLogBsr[] DL_DEVBSR_RO = "Backlight: screen %lu %s rc=%08lX from %08lX\n";
static const char dlOpenLogBsr[] DL_DEVBSR_RO = "Backlight: LCD open failed rc=%08lX from %08lX\n";
static const char dlPowerOnBsr[] DL_DEVBSR_RO = "on";
static const char dlPowerOffBsr[] DL_DEVBSR_RO = "off";
static const char dlPowerKeptBsr[] DL_DEVBSR_RO = "off, kept lit";
// The scope's requests: screen | on << 2 | kept lit << 3, callers, results. Written only by the scope's owner.
static struct {
    u32 count, what[4], caller[4];
    Result rc[4];
} dlBsrPower __attribute__((section(".bss.bsrdev_late")));

// dlReady() and the owner test written out: a call to this file's statics would be a new call-graph edge.
DL_DEVBSR static bool dlOwnedBsr(void) {
    DimlightHeap* root = __atomic_load_n(&dlRoot, __ATOMIC_ACQUIRE);
    return (uintptr_t) root > 2u &&
           __atomic_load_n(&root->lcd.owner, __ATOMIC_ACQUIRE) == (uintptr_t) getThreadLocalStorage();
}

DL_DEVBSR static void dlNoteBsr(u32 what, Result rc, uintptr_t caller) {
    if (!dlOwnedBsr() || dlBsrPower.count >= 4) return;
    u32 i = dlBsrPower.count++;
    dlBsrPower.what[i] = what;
    dlBsrPower.caller[i] = (u32) caller;
    dlBsrPower.rc[i] = rc;
}

DL_DEVBSR Result CtrDimlight_powerOnBsr(u32 screen) {
    Result result = dlPowerOnLinkBsr(screen);
    dlNoteBsr((screen & 3u) | 4u, result, (uintptr_t) __builtin_return_address(0));
    return result;
}

DL_DEVBSR Result CtrDimlight_powerOffBsr(u32 screen) {
    Result result = dlPowerOffLinkBsr(screen);
    DimlightHeap* root = __atomic_load_n(&dlRoot, __ATOMIC_ACQUIRE);
    // Kept lit: dimlight dimmed the bottom in place of the request (its keep-lit policy).
    bool kept = (screen & GSPLCD_SCREEN_BOTTOM) && R_SUCCEEDED(result) && (uintptr_t) root > 2u && root->policy.dimmed;
    dlNoteBsr((screen & 3u) | (kept ? 8u : 0u), result, (uintptr_t) __builtin_return_address(0));
    return result;
}

// Rosalina's release continuation: the outermost scope's requests are taken while it is still owned, printed after.
DL_DEVBSR void CtrDimlight_lcdExitBsr(void) {
    u32 count = 0, what[4], caller[4];
    Result rc[4];
    DimlightHeap* root = __atomic_load_n(&dlRoot, __ATOMIC_ACQUIRE);
    if (dlOwnedBsr() && root->lcd.depth == 1) {
        count = dlBsrPower.count;
        for (u32 i = 0; count > i; ++i) {
            what[i] = dlBsrPower.what[i];
            caller[i] = dlBsrPower.caller[i];
            rc[i] = dlBsrPower.rc[i];
        }
        dlBsrPower.count = 0;
    }
    dlLcdExitLinkBsr();
    for (u32 i = 0; count > i; ++i)
        logInfo(dlPowerLogBsr, (unsigned long) (what[i] & 3u),
                what[i] & 8u ? dlPowerKeptBsr : what[i] & 4u ? dlPowerOnBsr : dlPowerOffBsr, (unsigned long) rc[i],
                (unsigned long) caller[i]);
}

// A renderer LCD open that failed while another thread held the session (no request made, no scope left).
DL_DEVBSR void CtrDimlight_openFailedBsr(Result result, uintptr_t caller) {
    DimlightHeap* root = __atomic_load_n(&dlRoot, __ATOMIC_ACQUIRE);
    if ((uintptr_t) root <= 2u || dlOwnedBsr()) return;
    logInfo(dlOpenLogBsr, (unsigned long) result, (unsigned long) caller);
}
#endif
