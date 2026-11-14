#include "ctr_frame_pacer.h"
#include "ctr_fpsbox.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "common.h"
#include "ctr_gpu_queue.h"
#include "log.h"

// citro3d flips a screen from its GX queue's finish callback, on libctru's GSP
// event thread, and GSP shows the new buffer on the next vblank. That call is
// wrapped (-Wl,--wrap=gfxScreenSwapBuffers, see CMakeLists.txt) so a frame the
// GPU finished early is held until the last vblank of its slot has gone by.
//
// The vblank count is citro3d's frame counter, which counts every vblank while
// citro3d's frame rate stays at 60; C3D_FrameSync is never used.
//
// The GPU only starts a frame once the main thread has stepped and drawn it,
// and it runs that frame while the main thread steps and draws the next
// (ctr_gpu_queue.h), so a frame takes the longer of the two rather than
// both; the main thread waits for the GPU before handing it the next frame.
// It doesn't wait for that frame's flips: the next frame's draws start at
// once, and only its screen transfers wait in the GPU's queue until the
// flips have been shown (and let go here, see CtrPacer_screenReady). Held
// flips are let go from the game screen's vblank callback (pacebot, end).
// A frame is still shown after its CPU and GPU time together, though.
// Low input delay is invariant (maintainer, 2026-10-28). A missed deadline
// drops or extends that picture; it never arms an extra vblank for later
// pictures. Ordinary slots may realign after an actual late flip. Explicit
// frame-skip and Auto display-rate choices keep their existing schedules.










void __real_gfxScreenSwapBuffers(gfxScreen_t screen, bool hasStereo);
#line 46
void __wrap_gfxScreenSwapBuffers(gfxScreen_t screen, bool hasStereo);

typedef struct {
    uint32_t frames;
    // Top screen flips by how many vblanks the previous one stayed up: 1, 2, 3, 4 or more.
    uint32_t shown[4];
    // Flips that replaced one GSP hadn't shown yet.
    uint32_t dropped;
    // Flips held for the last vblank of their slot.
    uint32_t held;
    // Slots the main thread reached after their first vblank had begun, and the furthest behind one was.
    uint32_t late;
    uint32_t worstLate;
    // Slots moved to the vblank the last frame showed on, which the GPU finished too late for its own.
    uint32_t shifted;
    // Retired adaptive-delay counter: retained for layout/log schema, always zero.
    uint32_t delayed;
    // Late frames that went unshown so the next could catch up.
    uint32_t skipped;
    // The main thread's time from a slot's start to waiting for the next.
    uint64_t busyTicks, busyMaxTicks;
    // From a slot's start to the GPU finishing that frame.
    uint64_t doneTicks, doneMaxTicks;
    uint32_t doneCount;
    // C3D_FrameEnd, which also flushes the CPU's data cache.
    uint64_t endTicks, endMaxTicks;
    // The main thread waiting for the GPU to finish the frame before.
    uint64_t gpuWaitTicks, gpuWaitMaxTicks;
    float gpuMs, gpuMaxMs;
    // Top screen flips by how many vblanks the previous one stayed up (CtrFlipStats).
    CtrFlipStats flips;
} PaceStats;

static struct {
    bool ready;
    LightLock lock;
    float vblanksPerFrame;
    float carry;
    bool started;
    // The main thread reached this slot a whole vblank or more after it
    // began: a slow frame (loading a room, say), not one a little long.
    bool slotLate;
    uint32_t slotStart;
    uint32_t slotLength;
    uint64_t slotTick;
    // Late frames keep to the schedule rather than starting it over.
    bool catchUp;
    // This frame goes unshown, and how many before it in a row did too.
    bool skipping;
    int32_t skipsInRow;
    // Frames to go before skipping is tried again, after a skipped frame took
    // a whole slot anyway.
    uint32_t skipBlocked;
    // Set from the frame handed to the GPU last.
    bool holding;
    uint32_t holdUntil;
    uint64_t frameSlotTick;
    bool doneRecorded;
    bool heldTop, heldTopStereo, heldBottom;
    uint32_t lastShown;
    bool lastShownValid;
    uint64_t endTick;
    // pacestress.txt: the most milliseconds the main thread is held up before
    // each frame's end (at random), and the generator's state.
    uint32_t stressMaxMs;
    uint32_t stressSeed;
    // Retired adaptive-delay storage. Kept for layout/log compatibility only;
    // delay is initialized/reset to zero and never becomes nonzero.
    uint32_t delay;
    uint32_t inTimeRun;
    // Frames begun so far; a retired field; and whether this step's frame is of its level's cadence (CtrPacer_frameBegun).
    uint32_t frameIndex;
    uint32_t lateFrame;
    bool stepInBeat;
    // The vblank the last frame handed to the GPU is shown on without the
    // delay, and the vblank count when the GPU finished it.
    uint32_t frameDue;
    uint32_t doneVblank;
    bool doneValid;
    // Fixed 2:1 overlap (CtrPacer_setHalfRateOverlap), and whether a frame went unshown by CtrPacer_skipSlot since the
    // last shown frame.
    bool overlap;
    bool halfSkipped;
    // Auto's levels (CtrPacer_setLevelMode): on, picking the level itself, the level, and the steps since it began.
    bool levelMode, levelAuto;
    int32_t level;
    uint32_t phase;
    // Whether the step under way and the one before it are shown.
    bool stepShown, prevShown;
    bool stepValid;
    // The vblank the last shown frame was due on (without the delay); one still to be checked for lateness.
    uint32_t displayDue;
    bool displayDueValid;
    bool checkPending;
    uint32_t checkDue;
    // The level the frame handed to the GPU last was shown at (for its flip).
    int32_t flipLevel;
    // CtrGpu_waitTotalTicks at the step's start.
    uint64_t waitTotalAt;
    // Costs by kind ([0] shown, [1] unshown), averaged: the main thread's time less its GPU waits, and the GPU's.
    float cpuMs[2], gpuMs[2];
    uint32_t cpuSamples[2], gpuSamples[2];
    // The last steps' busy times (ms), for misses.
    float window[12];
    uint32_t windowCount, windowPos;
    // Steps to go before deciding again (a new room), steps in a row with room to go up, steps before looking down
    // again after nothing helped, all steps, and the step the level was entered on (and whether upward).
    uint32_t settle, upRun, holdOff, steps, enteredStep;
    bool enteredUp;
    // Steps of headroom needed before going up to each level again (after hunting), and steps without hunting.
    uint32_t backoff[CTR_LEVEL_COUNT];
    uint32_t calm;
    CtrLevelTotals levelTotals;
    // pacestress.txt "cpu" and "cpuskip" (ms).
    float stressCpuMs[2];
    PaceStats stats;
    // The flips since CtrPacer_init (CtrPacer_getFlipTotals).
    CtrFlipStats flipTotals;
} pace;

// Adaptive late-flip trigger and recovery thresholds retired. No cushion.




static inline uint32_t vblankCount(void) {
    return C3D_FrameCounter(0);
}

static inline uint64_t maxU64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

static void countFlip(CtrFlipStats* f, uint32_t vblanks) {
    f->count[vblanks > CTR_FLIP_BUCKETS ? CTR_FLIP_BUCKETS - 1 : vblanks - 1]++;
    f->flips++;
    f->sum += vblanks;
    f->sumSq += (uint64_t) vblanks * vblanks;
}

// Called with the lock held.
static void flip(gfxScreen_t screen, bool hasStereo, uint32_t now) {
    if (screen == GFX_TOP) {
        if (gspIsPresentPending(GFX_TOP)) {
            pace.stats.dropped++;
        } else {
            uint32_t shownAt = now + 1;
            if (pace.lastShownValid) {
                uint32_t vblanks = shownAt - pace.lastShown;
                if (vblanks > 0) {
                    pace.stats.shown[vblanks >= 4 ? 3 : vblanks - 1]++;
                    countFlip(&pace.stats.flips, vblanks);
                    countFlip(&pace.flipTotals, vblanks);
                    if (pace.levelMode) {
                        countFlip(&pace.levelTotals.flips[pace.flipLevel], vblanks);
                        if (vblanks > pace.levelTotals.maxVblanks[pace.flipLevel]) pace.levelTotals.maxVblanks[pace.flipLevel] = vblanks;
                    }
                }
            }
            pace.lastShown = shownAt;
            pace.lastShownValid = true;
        }
    }
    __real_gfxScreenSwapBuffers(screen, hasStereo);
}

// Called with the lock held. Flips what was held once its vblank has gone by, or now if forced.
static void releaseHeld(uint32_t now, bool force) {
    if (!pace.holding || (!force && 0 > (int32_t) (now - pace.holdUntil))) return;
    pace.holding = false;
    if (pace.heldTop) flip(GFX_TOP, pace.heldTopStereo, now);
    if (pace.heldBottom) flip(GFX_BOTTOM, false, now);
    pace.heldTop = pace.heldBottom = false;
}

// screens-port: the body is CtrScreens_pacerSwap (late code, at the end of this file), which counts the game
// screen's flips. Main's start and size stay: one branch, then padding that never runs (goes in the compaction round).
void __attribute__((naked, noinline)) __wrap_gfxScreenSwapBuffers(gfxScreen_t screen __attribute__((unused)),
                                                                  bool hasStereo __attribute__((unused))) {
    __asm__("b CtrArgui_pacerSwap\n\t.space 356");
}
#line 251

// Runs before the renderer's and citro3d's hooks (hooks run newest first),
// so nothing is held while the HOME Menu captures the screens. The frame on
// the GPU is then finished here, its held-back transfers let through by the
// next vblanks: citro3d's own hook turns the vblank callbacks off while the
// app is suspended, and the GPU is handed over after the hooks.
static void onAptEvent(APT_HookType hook, void* param) {
    (void) param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        LightLock_Lock(&pace.lock);
        releaseHeld(vblankCount(), true);
        pace.lastShownValid = false;
        LightLock_Unlock(&pace.lock);
        CtrGpu_waitIdle();
    }
}

void CtrPacer_onVBlank(void) {
    if (!pace.ready) return;
    LightLock_Lock(&pace.lock);
    releaseHeld(vblankCount(), false);
    LightLock_Unlock(&pace.lock);
}

bool CtrPacer_screenReady(gfxScreen_t screen) {
    if (!pace.ready) return !gspIsPresentPending(screen);
    LightLock_Lock(&pace.lock);
    // A held flip let go is pending before it stops being held, and both
    // change only under the lock.
    bool heldFlip = pace.holding && (screen == GFX_TOP ? pace.heldTop : pace.heldBottom);
    bool ready = !heldFlip && !gspIsPresentPending(screen);
    LightLock_Unlock(&pace.lock);
    return ready;
}

u8* CtrPacer_backFramebuffer(gfxScreen_t screen, gfx3dSide_t side) {
    if (pace.ready) LightLock_Lock(&pace.lock);
    u8* fb = gfxGetFramebuffer(screen, side, nullptr, nullptr);
    if (pace.ready) LightLock_Unlock(&pace.lock);
    return fb;
}

u8* CtrPacer_shownFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height) {
    // With double buffering off for the call, libctru returns the buffer on
    // screen. A flip meanwhile (flips go through here, under the lock) would
    // swap nothing, and the next transfer would write the shown buffer.
    if (pace.ready) LightLock_Lock(&pace.lock);
    gfxSetDoubleBuffering(screen, false);
    u8* fb = gfxGetFramebuffer(screen, side, width, height);
    gfxSetDoubleBuffering(screen, true);
    if (pace.ready) LightLock_Unlock(&pace.lock);
    return fb;
}

void CtrPacer_loadStress(const char* path) {
    FILE* f = fopen(path, "r");
    if (f == nullptr) return;
    char text[128] = { 0 };
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    unsigned ms = 0;
    const char* jitter = strstr(text, "jitter");
    if (jitter != nullptr) sscanf(jitter + 6, "%u", &ms);
    pace.stressMaxMs = ms;
    pace.stressSeed = 0x2545F491u;
    bool noGate = strstr(text, "nogate") != nullptr;
    CtrGpu_setGateEnabled(!noGate);
    logInfo("Pacer: stress test (%s): frame ends held up 0-%u ms at random, transfers %s\n", path, ms,
            noGate ? "NOT held back (negative control)" : "held back");
    // "gpu<ms>" and "gpuskip<ms>": the GPU's time on frames with screen transfers and without, made up (an emulator's
    // GPU takes none). "gpuskip" first, as "gpu" matches it too.
    float gpuMs = 0.0f, skipMs = 0.0f;
    const char* gpuSkip = strstr(text, "gpuskip");
    if (gpuSkip != nullptr) sscanf(gpuSkip + 7, "%f", &skipMs);
    for (const char* g = strstr(text, "gpu"); g != nullptr; g = strstr(g + 3, "gpu")) {
        if (g != gpuSkip && sscanf(g + 3, "%f", &gpuMs) == 1) break;
    }
    // "cpu<ms>" and "cpuskip<ms>": the main thread held up that long on each shown and unshown step (CtrPacer_stressStep).
    const char* cpuSkip = strstr(text, "cpuskip");
    if (cpuSkip != nullptr) sscanf(cpuSkip + 7, "%f", &pace.stressCpuMs[1]);
    for (const char* c = strstr(text, "cpu"); c != nullptr; c = strstr(c + 3, "cpu")) {
        if (c != cpuSkip && sscanf(c + 3, "%f", &pace.stressCpuMs[0]) == 1) break;
    }
    if (pace.stressCpuMs[0] > 0.0f || pace.stressCpuMs[1] > 0.0f) {
        logInfo("Pacer: stress test: steps held up %.1f ms shown, %.1f ms unshown\n", (double) pace.stressCpuMs[0], (double) pace.stressCpuMs[1]);
    }
    if (gpuMs > 0.0f || skipMs > 0.0f) {
        CtrGpu_setSimulatedGpu(gpuMs, skipMs);
        logInfo("Pacer: stress test: the GPU's time made up to at least %.1f ms a shown frame, %.1f ms an unshown one\n", (double) gpuMs,
                (double) skipMs);
    }
}

void CtrPacer_describeStall(char* out, size_t size) {
    snprintf(out, size,
             "pacer: lock word %ld, vblank %lu, slot %lu+%lu, holding %d (top %d bottom %d) until %lu, delay %lu, last shown %lu (%d)\n",
             (long) pace.lock, (unsigned long) vblankCount(), (unsigned long) pace.slotStart, (unsigned long) pace.slotLength,
             (int) pace.holding, (int) pace.heldTop, (int) pace.heldBottom, (unsigned long) pace.holdUntil, (unsigned long) pace.delay,
             (unsigned long) pace.lastShown, (int) pace.lastShownValid);
}

void CtrPacer_init(void) {
    static aptHookCookie aptCookie;
    if (pace.ready) return;
    LightLock_Init(&pace.lock);
    aptHook(&aptCookie, onAptEvent, nullptr);
    pace.vblanksPerFrame = 1.0f;
    C3D_FrameRate(60.0f);
    pace.ready = true;
}

void CtrPacer_setFrameRate(float fps) {
    pace.vblanksPerFrame = fps > 0.0f ? 60.0f / fps : 1.0f;
    pace.carry = 0.0f;
    pace.delay = 0;
    pace.inTimeRun = 0;
    pace.stepInBeat = false;
}

// A vblank of the 3DS's screens (59.83 Hz) in CPU ticks.
#define VBLANK_TICKS ((uint64_t) SYSCLOCK_ARM11 * 1000 / 59831)
// How long skipping stays off after a skipped frame saved too little.
#define SKIP_RETRY_FRAMES 150

static int32_t skipPolicy;

void CtrPacer_setSkipPolicy(int32_t policy) {
    skipPolicy = policy;
}

void CtrPacer_setHalfRateOverlap(bool on) {
    pace.overlap = on;
}

void CtrPacer_getFlipTotals(CtrFlipStats* out) {
    LightLock_Lock(&pace.lock);
    *out = pace.flipTotals;
    LightLock_Unlock(&pace.lock);
}

void CtrPacer_skipSlot(void) {
    pace.halfSkipped = true;
    if (!pace.started) {
        pace.started = true;
        pace.slotStart = vblankCount();
    } else {
        pace.slotStart += pace.slotLength;
    }
    // Busy time and lateness are measured at the next shown frame's slot, from this slot's beginning (slotTick is kept).
    pace.skipping = true;
    pace.skipsInRow++;
    pace.stats.skipped++;
    pace.carry += pace.vblanksPerFrame;
    uint32_t length = (uint32_t) pace.carry;
    if (1 > length) length = 1;
    pace.carry = pace.carry > (float) length ? pace.carry - (float) length : 0.0f;
    pace.slotLength = length;
    pace.stats.frames++;
    pace.frameIndex++;
}

bool CtrPacer_waitForSlot(bool catchUp, int32_t skipMax) {
    uint64_t entry = svcGetSystemTick();
    uint32_t now = vblankCount();
    bool wasSkipping = pace.skipping;
    pace.skipping = false;
    if (!catchUp) pace.skipBlocked = 0;
    if (pace.skipBlocked > 0) pace.skipBlocked--;
    if (!pace.started) {
        pace.started = true;
        pace.slotStart = now;
    } else {
        uint64_t busy = entry - pace.slotTick;
        pace.stats.busyTicks += busy;
        pace.stats.busyMaxTicks = maxU64(pace.stats.busyMaxTicks, busy);
        // A skipped frame that took most of a slot all the same (its step and
        // draw events alone take that long) wins little or no time back, and
        // costs a shown frame.
        if (wasSkipping && skipPolicy == 0 && 4 * busy > 3 * pace.slotLength * VBLANK_TICKS) pace.skipBlocked = SKIP_RETRY_FRAMES;
        uint32_t target = pace.slotStart + pace.slotLength;
        int32_t behind = (int32_t) (now - target);
        if (behind >= 0) {
            pace.stats.late++;
            pace.slotLate = behind >= 1;
            if ((uint32_t) behind > pace.stats.worstLate) pace.stats.worstLate = (uint32_t) behind;
            if (catchUp && (uint32_t) behind < pace.slotLength) {
                // Less than a frame behind: drawn and shown as soon as it is
                // done, and the slots keep to their schedule, so the time is
                // won back from frames that finish early.
                pace.slotStart = target;
            } else if (catchUp && skipMax > pace.skipsInRow && pace.skipBlocked == 0 &&
                       (uint32_t) behind < (skipPolicy == 0 ? 2u : 4u) * pace.slotLength) {
                // A whole frame behind. Not shown, so it needn't wait for
                // anything, and the next frame gets the time this one saves.
                pace.skipping = true;
                pace.skipsInRow++;
                pace.stats.skipped++;
                pace.slotStart = target;
            } else {
                // The slot's first vblank has begun: start now, and count the
                // slot from the vblank the frame really starts on.
                pace.slotStart = now;
            }
        } else {
            // Held flips are let go from the vblank callback, before the
            // event that wakes this.
            while (0 > (int32_t) (now - target)) {
                // The event stays signalled until waited for, so a vblank
                // between reading the count and waiting still wakes this.
                gspWaitForEvent(GSPGPU_EVENT_VBlank0, false);
                now = vblankCount();
            }
            pace.slotStart = target;
            pace.slotLate = false;
        }
    }
    pace.catchUp = catchUp;
    pace.carry += pace.vblanksPerFrame;
    uint32_t length = (uint32_t) pace.carry;
    if (1 > length) length = 1;
    pace.carry = pace.carry > (float) length ? pace.carry - (float) length : 0.0f;
    pace.slotLength = length;
    pace.slotTick = svcGetSystemTick();
    pace.stats.frames++;
    pace.frameIndex++;
    if (!pace.skipping) pace.skipsInRow = 0;
    return !pace.skipping;
}

static void levelCheckFlip(void); static bool levelShows(int32_t level, uint32_t phase);

void CtrPacer_frameBegun(void) {
    if (pace.levelMode) {
        // The beat holds only level 1's second of two frames shown in a row just after a skipped step (its phase 1), at
        // 30 fps slots, the last shown frame at level 1 too. A readback or the menu shows every step. Shown steps: lateFrame.
        pace.lateFrame = pace.lateFrame << 1 | pace.stepShown; pace.stepInBeat = pace.slotLength == 2 && pace.flipLevel == 1 && pace.level == 1 && (pace.lateFrame & 7) == 3;
        levelCheckFlip();
        return;
    }
    if (pace.skipping) return;
    LightLock_Lock(&pace.lock);
    // A real late flip may move the next slot. It cannot change input delay. Late counts from this frame's own slot,
    // in overlap too (its slot is already past the skipped one), so a done frame is never held past its slot.
    if (pace.lastShownValid) {
        int32_t late = (int32_t) (pace.lastShown - pace.slotStart);
        if (late > 0 && !pace.catchUp) {
            pace.slotStart += (uint32_t) late;
            pace.stats.shifted++;
        }
    }
    pace.doneValid = false;
    LightLock_Unlock(&pace.lock);
}
































void CtrPacer_frameEnding(void) {
    if (pace.stressMaxMs > 0) {
        pace.stressSeed ^= pace.stressSeed << 13;
        pace.stressSeed ^= pace.stressSeed >> 17;
        pace.stressSeed ^= pace.stressSeed << 5;
        svcSleepThread((int64_t) (pace.stressSeed % (pace.stressMaxMs * 1000u + 1u)) * 1000);
    }
    LightLock_Lock(&pace.lock);
    // Anything still held belongs to an earlier frame.
    releaseHeld(vblankCount(), true);
    pace.holding = pace.slotLength >= 2;
    pace.holdUntil = pace.slotStart + pace.slotLength - 1;
    pace.frameDue = pace.slotStart + pace.slotLength;
    // No delayed pictures: the retired counter remains zero.
    pace.frameSlotTick = pace.slotTick;
    pace.doneRecorded = false;
    pace.halfSkipped = false;
    if (pace.levelMode) {
        // Auto's levels: due at the slot's end, or one vblank later for a frame of the beat (CtrPacer_frameBegun) due
        // sooner than the beat after the frame before. Never more than one, so no due carries over to later frames.
        static const uint8_t beat[CTR_LEVEL_COUNT] = { 2, 3, 4, 6 };
        uint32_t due = pace.slotStart + pace.slotLength, period = beat[pace.level];
        if (pace.stepInBeat && pace.displayDueValid && 0 > (int32_t) (due - (pace.displayDue + period))) due++;
        pace.displayDue = due;
        pace.displayDueValid = true;
        pace.frameDue = due;
        pace.holdUntil = due - 1;
        pace.checkPending = true;
        pace.checkDue = due;
        pace.flipLevel = pace.level;
    }
    LightLock_Unlock(&pace.lock);
    // A flip still waiting for its vblank leaves the buffer it replaces on
    // screen until then, and that is the buffer this frame's display transfer
    // writes. The transfers wait for it in the GPU's queue (ctr_gpu_queue.c),
    // each screen for its own flip; the frame's draws don't.
    pace.endTick = svcGetSystemTick();
}

void CtrPacer_frameEnded(float gpuMs) {
    uint64_t end = svcGetSystemTick() - pace.endTick;
    uint64_t gpuWait = CtrGpu_takeWaitTicks();
    LightLock_Lock(&pace.lock);
    pace.stats.endTicks += end;
    pace.stats.endMaxTicks = maxU64(pace.stats.endMaxTicks, end);
    pace.stats.gpuWaitTicks += gpuWait;
    pace.stats.gpuWaitMaxTicks = maxU64(pace.stats.gpuWaitMaxTicks, gpuWait);
    pace.stats.gpuMs += gpuMs;
    if (gpuMs > pace.stats.gpuMaxMs) pace.stats.gpuMaxMs = gpuMs;
    LightLock_Unlock(&pace.lock);
}

void CtrPacer_logStats(void) {
    LightLock_Lock(&pace.lock);
    PaceStats s = pace.stats;
    memset(&pace.stats, 0, sizeof(pace.stats));
    LightLock_Unlock(&pace.lock);
    CtrGpuPaceStats g;
    CtrGpu_takePaceStats(&g);
    CtrGpu_logShownWrites();
    if (s.frames == 0) return;
    double ms = 1.0 / CPU_TICKS_PER_MSEC;
    double frames = (double) s.frames;
    logInfo("[pace] %u frames of %u vblanks: shown for 1/2/3/4+ vblanks %u/%u/%u/%u, %u never shown, %u held for their vblank; "
            "%u late (worst %u vblanks behind), %u skipped, %u moved after a late flip, %u shown a vblank after their slot; "
            "main %.1f ms max %.1f, GPU done %.1f ms into the slot max %.1f, gpu %.1f ms max %.1f, frame end %.2f ms max %.2f, "
            "waited for the GPU %.1f ms max %.1f; "
            "GPU idle %.1f ms (waiting for a flip %.1f, for the next frame %.1f); transfers held back in %u frames, "
            "%u let through by a vblank, draws done first in %u; %u transfers into a shown buffer "
            "(and %u into a single-buffered screen's only buffer), %u dropped\n",
            (unsigned) s.frames, (unsigned) pace.slotLength, (unsigned) s.shown[0], (unsigned) s.shown[1], (unsigned) s.shown[2],
            (unsigned) s.shown[3], (unsigned) s.dropped, (unsigned) s.held, (unsigned) s.late, (unsigned) s.worstLate, (unsigned) s.skipped,
            (unsigned) s.shifted, (unsigned) s.delayed, (double) s.busyTicks * ms / frames, (double) s.busyMaxTicks * ms,
            s.doneCount > 0 ? (double) s.doneTicks * ms / (double) s.doneCount : 0.0, (double) s.doneMaxTicks * ms,
            (double) s.gpuMs / frames, (double) s.gpuMaxMs, (double) s.endTicks * ms / frames, (double) s.endMaxTicks * ms,
            (double) s.gpuWaitTicks * ms / frames, (double) s.gpuWaitMaxTicks * ms,
            (double) (g.flipIdleTicks + g.mainIdleTicks) * ms / frames, (double) g.flipIdleTicks * ms / frames,
            (double) g.mainIdleTicks * ms / frames, (unsigned) g.framesHeldBack, (unsigned) g.transfersWaited, (unsigned) g.framesFlipIdle,
            (unsigned) g.shownWrites, (unsigned) g.singleBufferWrites, (unsigned) g.droppedTransfers);
    // While frames are skipped: how evenly the shown ones came, finer than the 1/2/3/4+ above.
    const CtrFlipStats* f = &s.flips;
    if (s.skipped > 0 && f->flips > 0) {
        double mean = (double) f->sum / f->flips;
        double var = (double) f->sumSq / f->flips - mean * mean;
        logInfo("[flips] %u flips: shown for 1-7/8+ vblanks %u/%u/%u/%u/%u/%u/%u/%u, mean %.2f vblanks, sd %.2f, delay now %u\n",
                (unsigned) f->flips, (unsigned) f->count[0], (unsigned) f->count[1], (unsigned) f->count[2], (unsigned) f->count[3],
                (unsigned) f->count[4], (unsigned) f->count[5], (unsigned) f->count[6], (unsigned) f->count[7], mean,
                var > 0.0 ? sqrt(var) : 0.0, (unsigned) pace.delay);
    }
}

// ===[ Auto's levels (phase 3) ]===

// Steps in the miss window; a step this many slots behind restarts the schedule (and over this long, isn't a sample).
#define LEVEL_WINDOW 12
#define LEVEL_RESET_SLOTS 3
// Steps with headroom before going up; a level entered upward and left within LEVEL_HUNT_STEPS is hunting, and its
// wait doubles from LEVEL_BACKOFF_STEPS up to LEVEL_BACKOFF_MAX (and halves after that many steps without hunting).
#define LEVEL_UP_STEPS 90
#define LEVEL_HUNT_STEPS 300
#define LEVEL_BACKOFF_STEPS 150
#define LEVEL_BACKOFF_MAX 1800
// No decision for this many steps after a room change, nor looking down again after nothing was found to help.
#define LEVEL_SETTLE_STEPS 30
#define LEVEL_HOLD_STEPS 150
// Unshown frames measured before their costs replace the priors (a share of the shown frame's).
#define LEVEL_K_SAMPLES 8

static const uint8_t levelPeriod[CTR_LEVEL_COUNT] __attribute__((no_reorder)) = { 1, 3, 2, 3 };
// Never-read placeholder for the retired Auto delay limits. Keep the same
// object, bytes and alignment so removing the policy does not move constants.
static const uint8_t levelDelayMax[CTR_LEVEL_COUNT] __attribute__((used, no_reorder)) = { 1, 1, 3, 5 };

static bool levelShows(int32_t level, uint32_t phase) {
    switch (level) {
        case 1: return phase % 3 != 2;
        case 2: return phase % 2 == 0;
        case 3: return phase % 3 == 0;
        default: return true;
    }
}

static float levelCpu(int k) {
    return k == 0 || pace.cpuSamples[1] >= LEVEL_K_SAMPLES ? pace.cpuMs[k] : 0.75f * pace.cpuMs[0];
}

static float levelGpuCost(int k) {
    return k == 0 || pace.gpuSamples[1] >= LEVEL_K_SAMPLES ? pace.gpuMs[k] : 0.5f * pace.gpuMs[0];
}

// The projected time of a step at a level: frame i takes max(cpu_i, gpu_{i-1}), as the main thread waits for the
// frame before's GPU work before handing over its own.
static float levelProjection(int32_t level) {
    uint32_t m = levelPeriod[level];
    float sum = 0.0f;
    for (uint32_t i = 0; m > i; i++) {
        float cpu = levelCpu(levelShows(level, i) ? 0 : 1);
        float gpu = levelGpuCost(levelShows(level, (i + m - 1) % m) ? 0 : 1);
        sum += cpu > gpu ? cpu : gpu;
    }
    return sum / (float) m;
}

static void levelAverage(float* avg, uint32_t* samples, float ms) {
    *avg = *samples == 0 ? ms : *avg + (ms - *avg) / 8.0f;
    if (*samples < UINT32_MAX) (*samples)++;
}

static void levelSet(int32_t level, bool up) {
    if (level == pace.level) return;
    // Entered upward and left downward soon after: hunting. Wait longer before trying that level again.
    if (level > pace.level && pace.enteredUp && LEVEL_HUNT_STEPS > pace.steps - pace.enteredStep) {
        uint32_t* b = &pace.backoff[pace.level];
        *b = *b == 0 ? LEVEL_BACKOFF_STEPS : (2 * *b > LEVEL_BACKOFF_MAX ? LEVEL_BACKOFF_MAX : 2 * *b);
        pace.calm = 0;
    }
    logInfo("[level] %d -> %d at step %lu: projected %.1f/%.1f/%.1f/%.1f ms a step (cpu %.1f/%.1f, gpu %.1f/%.1f ms shown/unshown)\n",
            (int) pace.level, (int) level, (unsigned long) pace.steps, (double) levelProjection(0), (double) levelProjection(1),
            (double) levelProjection(2), (double) levelProjection(3), (double) levelCpu(0), (double) levelCpu(1),
            (double) levelGpuCost(0), (double) levelGpuCost(1));
    pace.enteredUp = up;
    pace.enteredStep = pace.steps;
    pace.level = level;
    pace.phase = 0;
    pace.windowCount = pace.windowPos = 0;
    pace.upRun = 0;
    pace.levelTotals.changes++;
}

// The step that just ended took busy ticks; the new one starts behind vblanks after its slot.
static void levelNoteStep(uint64_t busy, int32_t behind) {
    uint64_t waitTotal = CtrGpu_waitTotalTicks();
    uint64_t waited = waitTotal - pace.waitTotalAt;
    pace.waitTotalAt = waitTotal;
    if (!pace.stepValid) return;
    float slotMs = (float) pace.slotLength * 1000.0f / 59.831f;
    float busyMs = (float) ((double) busy / CPU_TICKS_PER_MSEC);
    if ((float) LEVEL_RESET_SLOTS * slotMs >= busyMs) {
        float cpuMs = busy > waited ? (float) ((double) (busy - waited) / CPU_TICKS_PER_MSEC) : 0.0f;
        int k = pace.stepShown ? 0 : 1;
        levelAverage(&pace.cpuMs[k], &pace.cpuSamples[k], cpuMs);
        pace.window[pace.windowPos] = busyMs;
        pace.windowPos = (pace.windowPos + 1) % LEVEL_WINDOW;
        if (LEVEL_WINDOW > pace.windowCount) pace.windowCount++;
    }
    if (++pace.calm >= LEVEL_BACKOFF_MAX) {
        pace.calm = 0;
        for (int i = 0; CTR_LEVEL_COUNT > i; i++) pace.backoff[i] = LEVEL_BACKOFF_STEPS > pace.backoff[i] / 2 ? 0 : pace.backoff[i] / 2;
    }
    if (pace.holdOff > 0) pace.holdOff--;
    if (!pace.levelAuto || pace.slotLength != 2) return;
    if (pace.settle > 0) {
        pace.settle--;
        return;
    }
    if (LEVEL_WINDOW > pace.windowCount) return;
    // Missing: the window's steps less its longest (one long frame never counts) over the slot, and behind.
    float sum = 0.0f, longest = 0.0f;
    for (int i = 0; LEVEL_WINDOW > i; i++) {
        sum += pace.window[i];
        if (pace.window[i] > longest) longest = pace.window[i];
    }
    bool miss = (sum - longest) / (float) (LEVEL_WINDOW - 1) > 1.02f * slotMs && behind >= 1;
    if (miss) {
        pace.upRun = 0;
        if (pace.holdOff > 0) return;
        int32_t target = -1;
        for (int32_t l = pace.level + 1; CTR_LEVEL_COUNT > l && target < 0; l++) {
            if (0.95f * slotMs >= levelProjection(l)) target = l;
        }
        if (target < 0 && pace.level == 3 && levelProjection(3) > slotMs) target = 2; // 10 shown only where it holds
        if (target < 0) {
            // Nothing holds full speed: the fastest of levels 1-2, if 15% faster than this one.
            float best = levelProjection(pace.level) / 1.15f;
            for (int32_t l = pace.level + 1; 2 >= l; l++) {
                float ms = levelProjection(l);
                if (best >= ms) {
                    best = ms;
                    target = l;
                }
            }
        }
        if (target < 0) {
            pace.holdOff = LEVEL_HOLD_STEPS;
            return;
        }
        levelSet(target, false);
        return;
    }
    if (pace.level > 0 && 0.85f * slotMs >= levelProjection(pace.level - 1)) {
        uint32_t need = pace.backoff[pace.level - 1] > LEVEL_UP_STEPS ? pace.backoff[pace.level - 1] : LEVEL_UP_STEPS;
        if (++pace.upRun >= need) levelSet(pace.level - 1, true);
    } else {
        pace.upRun = 0;
    }
}

void CtrPacer_setLevelMode(bool on, bool automatic, int32_t fixedLevel) {
    if (fixedLevel < 0) fixedLevel = 0;
    if (fixedLevel >= CTR_LEVEL_COUNT) fixedLevel = CTR_LEVEL_COUNT - 1;
    if (on && (!pace.levelMode || pace.levelAuto != automatic || (!automatic && pace.level != fixedLevel))) {
        pace.level = automatic ? 0 : fixedLevel;
        pace.phase = 0;
        pace.windowCount = pace.windowPos = pace.upRun = pace.settle = pace.holdOff = 0;
    }
    pace.levelMode = on;
    pace.levelAuto = automatic;
}

void CtrPacer_levelReset(void) {
    if (pace.levelAuto) pace.level = 0;
    pace.phase = 0;
    pace.stepValid = false;
    pace.windowCount = pace.windowPos = pace.upRun = pace.settle = pace.holdOff = pace.calm = 0;
    memset(pace.cpuSamples, 0, sizeof(pace.cpuSamples));
    memset(pace.gpuSamples, 0, sizeof(pace.gpuSamples));
    memset(pace.backoff, 0, sizeof(pace.backoff));
    pace.displayDueValid = false;
}

void CtrPacer_levelRoomChanged(void) {
    pace.settle = LEVEL_SETTLE_STEPS;
    pace.windowCount = pace.windowPos = 0;
    pace.upRun = 0;
}

bool CtrPacer_levelWait(bool mayskip) {
    uint64_t entry = svcGetSystemTick();
    uint32_t now = vblankCount();
    bool shown = true;
    if (!pace.started) {
        pace.started = true;
        pace.slotStart = now;
        pace.waitTotalAt = CtrGpu_waitTotalTicks();
    } else {
        uint64_t busy = entry - pace.slotTick;
        pace.stats.busyTicks += busy;
        pace.stats.busyMaxTicks = maxU64(pace.stats.busyMaxTicks, busy);
        uint32_t target = pace.slotStart + pace.slotLength;
        int32_t behind = (int32_t) (now - target);
        levelNoteStep(busy, behind);
        // After the decision, which may have changed the level (and its phase).
        shown = !mayskip || pace.slotLength != 2 || levelShows(pace.level, pace.phase);
        if (behind >= 0) {
            pace.stats.late++;
            if ((uint32_t) behind > pace.stats.worstLate) pace.stats.worstLate = (uint32_t) behind;
        }
        if (behind >= (int32_t) (LEVEL_RESET_SLOTS * pace.slotLength)) {
            // Too far behind to catch up: the game loses the time once.
            pace.slotStart = now;
            pace.levelTotals.resets++;
        } else if (behind >= 0 || !shown) {
            // Late, or unshown: starts at once, and the slots keep to their schedule.
            pace.slotStart = target;
        } else {
            while (0 > (int32_t) (now - target)) {
                gspWaitForEvent(GSPGPU_EVENT_VBlank0, false);
                now = vblankCount();
            }
            pace.slotStart = target;
        }
    }
    pace.slotLate = false;
    pace.catchUp = true;
    pace.skipping = !shown;
    if (shown) {
        pace.skipsInRow = 0;
    } else {
        pace.skipsInRow++;
        pace.stats.skipped++;
    }
    pace.carry += pace.vblanksPerFrame;
    uint32_t length = (uint32_t) pace.carry;
    if (1 > length) length = 1;
    pace.carry = pace.carry > (float) length ? pace.carry - (float) length : 0.0f;
    pace.slotLength = length;
    pace.slotTick = svcGetSystemTick();
    pace.stats.frames++;
    pace.frameIndex++;
    pace.prevShown = pace.stepShown;
    pace.stepShown = shown;
    pace.stepValid = true;
    pace.phase++;
    pace.steps++;
    pace.levelTotals.steps[pace.level]++;
    return shown;
}

void CtrPacer_levelGpu(float gpuMs) {
    if (!pace.levelMode || !pace.stepValid) return;
    float slotMs = (float) pace.slotLength * 1000.0f / 59.831f;
    if (gpuMs <= 0.0f || gpuMs > (float) LEVEL_RESET_SLOTS * slotMs) return;
    int k = pace.prevShown ? 0 : 1;
    levelAverage(&pace.gpuMs[k], &pace.gpuSamples[k], gpuMs);
}

// Auto's display beat is unchanged. Its adaptive delay is retired too.
static void levelCheckFlip(void) {
    // Constant placement is declared above; the retired delay table is never read.
    LightLock_Lock(&pace.lock);
    if (pace.checkPending && pace.lastShownValid) {
        pace.checkPending = false;
        pace.doneValid = false;
    }
    LightLock_Unlock(&pace.lock);
}






















void CtrPacer_getLevelTotals(CtrLevelTotals* out) {
    LightLock_Lock(&pace.lock);
    *out = pace.levelTotals;
    LightLock_Unlock(&pace.lock);
}

void CtrPacer_logLevelStats(void) {
    if (!pace.levelMode) return;
    const CtrLevelTotals* t = &pace.levelTotals;
    logInfo("[level] level %d (%s), steps at 30/20/15/10 shown %lu/%lu/%lu/%lu, %lu changes, %lu resets, delay %lu; "
            "cpu %.1f/%.1f gpu %.1f/%.1f ms shown/unshown (%lu/%lu unshown samples); projected %.1f/%.1f/%.1f/%.1f ms a step\n",
            (int) pace.level, pace.levelAuto ? "auto" : "fixed", (unsigned long) t->steps[0], (unsigned long) t->steps[1],
            (unsigned long) t->steps[2], (unsigned long) t->steps[3], (unsigned long) t->changes, (unsigned long) t->resets,
            (unsigned long) pace.delay, (double) pace.cpuMs[0], (double) pace.cpuMs[1], (double) pace.gpuMs[0], (double) pace.gpuMs[1],
            (unsigned long) pace.cpuSamples[1], (unsigned long) pace.gpuSamples[1], (double) levelProjection(0), (double) levelProjection(1),
            (double) levelProjection(2), (double) levelProjection(3));
}

void CtrPacer_logLevels(const char* id, const CtrLevelTotals* start, double seconds) {
    if (!pace.levelMode) return;
    CtrLevelTotals now;
    CtrPacer_getLevelTotals(&now);
    const double vblankMs = 1000.0 / 59.831;
    static const char* names[CTR_LEVEL_COUNT] = { "30", "20", "15", "10" };
    char text[512];
    uint32_t changes = now.changes - start->changes;
    int n = snprintf(text, sizeof(text), "Bench: %s: levels: %lu changes (%.1f a minute), %lu resets;", id, (unsigned long) changes,
                     seconds > 0.0 ? 60.0 * (double) changes / seconds : 0.0, (unsigned long) (now.resets - start->resets));
    for (int l = 0; CTR_LEVEL_COUNT > l && n > 0 && (int) sizeof(text) > n; l++) {
        uint32_t steps = now.steps[l] - start->steps[l];
        if (steps == 0) continue;
        const CtrFlipStats* a = &now.flips[l];
        const CtrFlipStats* b = &start->flips[l];
        uint32_t flips = a->flips - b->flips;
        double mean = flips > 0 ? (double) (a->sum - b->sum) / flips : 0.0;
        // p95 from the histogram, in whole vblanks (the last bucket is 8 or more).
        uint32_t p95 = 0, seen = 0;
        for (int i = 0; CTR_FLIP_BUCKETS > i && flips > 0; i++) {
            seen += a->count[i] - b->count[i];
            if (seen * 100u >= flips * 95u) {
                p95 = (uint32_t) i + 1u;
                break;
            }
        }
        // The longest is since the program began: it can't be split by scene.
        n += snprintf(text + n, sizeof(text) - (size_t) n, " %s shown: %lu steps, %lu flips, mean %.1f ms, p95 %.1f ms, max %.1f ms;",
                      names[l], (unsigned long) steps, (unsigned long) flips, mean * vblankMs, (double) p95 * vblankMs,
                      (double) now.maxVblanks[l] * vblankMs);
    }
    logInfo("%s\n", text);
}

void CtrPacer_stressStep(bool shown) {
    float ms = pace.stressCpuMs[shown ? 0 : 1];
    if (ms > 0.0f) svcSleepThread((int64_t) (ms * 1000000.0f));
}

// ===[ The game screen's flips (screens-port, late code) ]===
//
// With game_screen bottom the game's frames flip the bottom screen, so its flips are the frames' (shown, dropped,
// held, when a frame was done) and the top screen's are the consoles'. __wrap_gfxScreenSwapBuffers above branches to
// its copy here, keeping its address and size; with the game on top that copy calls flip as before. flip itself stays
// as it was: releaseHeld inlines its bottom-screen call, which is why the vblank's release of held flips is wrapped
// here too (CtrPacer_onVBlank, link option in CMakeLists.txt) while the game is on the bottom. `pace` is reached by its
// assembler name, so the functions above keep their code (docs/3ds-hardware.md, "A function rewritten as a late
// copy keeps its slot").

#if defined(__3DS__) && defined(__GNUC__)
#define SCREENS_LATE __attribute__((noinline, section(".text.late.screens")))
#else
#define SCREENS_LATE
#endif

extern uint8_t CtrScreens_pacerGame; // the game's gfxScreen_t: GFX_TOP (0) until CtrPacer_setGameScreen (ctr_screens.c)
void CtrScreens_pacerFlip(gfxScreen_t screen, bool hasStereo, uint32_t now);
void CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo);
void CtrPacer_setGameScreen(gfxScreen_t screen);

// Called with the lock held.
SCREENS_LATE void CtrScreens_pacerFlip(gfxScreen_t screen, bool hasStereo, uint32_t now) {
    extern __typeof__(pace) screensPace __asm__("pace");
    if (screen == (gfxScreen_t) CtrScreens_pacerGame) {
        if (gspIsPresentPending(screen)) {
            screensPace.stats.dropped++;
        } else {
            uint32_t shownAt = now + 1;
            if (screensPace.lastShownValid) {
                uint32_t vblanks = shownAt - screensPace.lastShown;
                if (vblanks > 0) {
                    screensPace.stats.shown[vblanks >= 4 ? 3 : vblanks - 1]++;
                    countFlip(&screensPace.stats.flips, vblanks);
                    countFlip(&screensPace.flipTotals, vblanks);
                    if (screensPace.levelMode) {
                        countFlip(&screensPace.levelTotals.flips[screensPace.flipLevel], vblanks);
                        if (vblanks > screensPace.levelTotals.maxVblanks[screensPace.flipLevel]) {
                            screensPace.levelTotals.maxVblanks[screensPace.flipLevel] = vblanks;
                        }
                    }
                }
            }
            screensPace.lastShown = shownAt;
            screensPace.lastShownValid = true;
        }
    }
    __real_gfxScreenSwapBuffers(screen, hasStereo);
}

SCREENS_LATE void CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo) {
    extern __typeof__(pace) screensPace __asm__("pace");
    if (!screensPace.ready) {
        __real_gfxScreenSwapBuffers(screen, hasStereo);
        return;
    }
    uint64_t tick = svcGetSystemTick();
    LightLock_Lock(&screensPace.lock);
    uint32_t now = vblankCount();
    bool game = screen == (gfxScreen_t) CtrScreens_pacerGame;
    if (game && !screensPace.doneRecorded && screensPace.frameSlotTick != 0) {
        screensPace.doneRecorded = true;
        uint64_t done = tick - screensPace.frameSlotTick;
        screensPace.stats.doneTicks += done;
        screensPace.stats.doneMaxTicks = maxU64(screensPace.stats.doneMaxTicks, done);
        screensPace.stats.doneCount++;
        screensPace.doneVblank = now;
        screensPace.doneValid = true;
    }
    if (screensPace.holding && 0 > (int32_t) (now - screensPace.holdUntil)) {
        if (screen == GFX_TOP) {
            screensPace.heldTop = true;
            screensPace.heldTopStereo = hasStereo;
        } else {
            screensPace.heldBottom = true;
        }
        if (game) screensPace.stats.held++;
    } else if (CtrScreens_pacerGame == GFX_TOP) {
        flip(screen, hasStereo, now);
    } else {
        CtrScreens_pacerFlip(screen, hasStereo, now);
    }
    LightLock_Unlock(&screensPace.lock);
}

// releaseHeld with the game on the bottom screen, counting its held flip as flip would the top one's.
void __real_CtrPacer_onVBlank(void);
void __wrap_CtrPacer_onVBlank(void);
SCREENS_LATE void __wrap_CtrPacer_onVBlank(void) {
    extern __typeof__(pace) screensPace __asm__("pace");
    if (CtrScreens_pacerGame == GFX_TOP || !screensPace.ready) {
        __real_CtrPacer_onVBlank();
        return;
    }
    LightLock_Lock(&screensPace.lock);
    uint32_t now = vblankCount();
    if (screensPace.holding && 0 <= (int32_t) (now - screensPace.holdUntil)) {
        screensPace.holding = false;
        if (screensPace.heldTop) CtrScreens_pacerFlip(GFX_TOP, screensPace.heldTopStereo, now);
        if (screensPace.heldBottom) CtrScreens_pacerFlip(GFX_BOTTOM, false, now);
        screensPace.heldTop = screensPace.heldBottom = false;
    }
    LightLock_Unlock(&screensPace.lock);
}

// Shows a screen's other buffer and its format once, outside the frames' flips and their counts: the game's last
// frame on the screen it moved to (ctr_renderer.c's screensMoveGame).
void CtrScreens_pacerShow(gfxScreen_t screen);
SCREENS_LATE void CtrScreens_pacerShow(gfxScreen_t screen) {
    extern __typeof__(pace) screensPace __asm__("pace");
    if (screensPace.ready) LightLock_Lock(&screensPace.lock);
    __real_gfxScreenSwapBuffers(screen, false);
    if (screensPace.ready) LightLock_Unlock(&screensPace.lock);
}

// The screen the game shows on: its flips are counted as the frames' from the next one. Call with nothing held (the
// GPU idle).
SCREENS_LATE void CtrPacer_setGameScreen(gfxScreen_t screen) {
    extern __typeof__(pace) screensPace __asm__("pace");
    LightLock_Lock(&screensPace.lock);
    CtrScreens_pacerGame = (uint8_t) screen;
    screensPace.lastShownValid = false;
    LightLock_Unlock(&screensPace.lock);
}

// Adaptive cushion prototype retired by owner ruling, 2026-10-28.
// Its former wrapper options are rejected in CMake.
// Historical prototype evidence cannot enter a shipping build.

#include "ctr_frame_pacer_slots.inc"

#ifdef CTRPORT_FRAMETRACE
// frametrace (z_frametrace.c; build.sh ... frametrace, test builds only): the pacer's state as a frame's Step begins.
// out: slot start, slot length, delay, the vblank the last flip shows on, flips so far, shifts so far, the frame goes
// unshown (skipping), the slot began a whole vblank late.
__attribute__((section(".text.frametrace_late"))) void CtrPacer_ftState(uint32_t* out) {
    LightLock_Lock(&pace.lock);
    out[0] = pace.slotStart;
    out[1] = pace.slotLength;
    out[2] = pace.delay;
    out[3] = pace.lastShown;
    out[4] = pace.flipTotals.flips;
    out[5] = pace.stats.shifted;
    out[6] = pace.skipping;
    out[7] = pace.slotLate;
    LightLock_Unlock(&pace.lock);
}
#endif

// ===[ pacebot: the game screen's vblanks (change pacer, late code) ]===
//
// GSP shows a screen's present from that screen's own vblank: VBlank0 the top, VBlank1 the bottom (docs/3ds-hardware.md,
// "The bottom screen flips on its own vblank"). With the game on the bottom screen the pacer keeps to VBlank1, so a
// frame held for its slot's last vblank shows at the slot's end on the bottom screen as on the top. With the game on
// top nothing here changes what runs. Link options in CMakeLists.txt; sections last in late_reserve.ld.
// - C3D_FrameCounter(0) counts the game screen's vblanks: the pacer's vblankCount (slots, holds, forced releases at
//   suspend and at the next frame, shown counts) and every other caller's (the FPS counters, the Rosalina probe).
// - The two slot waits (the only gspWaitForEvent(VBlank0, false) calls: every other caller passes true) wait on
//   VBlank1.
// - Held flips are let go from VBlank1's callback, chained ahead of the GPU queue's own (onVBlank1) on the GSP event
//   thread that runs every callback, so it takes the pacer's lock as onVBlank0's release does and lets it go before
//   the queue's callback runs (no lock is held across the two). The queue's callback counts this vblank after it, so
//   the release reads the count it is about to make (count + 1: the pacer sets citro3d's rate to 60 at init, so the
//   queue counts every vblank), as onVBlank0 counts before it lets the pacer flip. The top
//   screen's release (CtrPacer_onVBlank's wrap above) stays as a no-op fallback: it finds nothing held past its vblank.
//   Not the top screen's callback with the bottom count instead: on a New 3DS VBlank1's callback ran 0.03-0.16 ms
//   after VBlank0's (pacer vbphase row, 10-14), so the two interrupts are near-simultaneous and a flip let go from
//   VBlank0's callback could land on either side of VBlank1's (a vblank late or not); from VBlank1's own callback it
//   always shows at the next bottom vblank.
// - A screen change mid-game (the options menu) moves the schedule into the new screen's count: both count every
//   vblank, so they differ by a constant.
// - The next frame's forced release (CtrPacer_frameEnding) counts the game screen's flip, as the vblank's does.
// - The game moved between the screens: its carried frame is waited for on its own screen's vblank.
// - Suspend, sleep, HOME: citro3d's hook turns both callbacks off and on together (the VBlank1 one is chained again as
//   it is set), and both counts stop together, as they did for the top screen's.

#define PACEBOT_TEXT __attribute__((noinline, optimize("Os"), section(".text.pacebot_late")))
#define PACEBOT_BSS __attribute__((section(".bss.pacebot_late")))

static ThreadFunc pacebotQueueVBlank1 PACEBOT_BSS;

u32 __real_C3D_FrameCounter(int id);
u32 __wrap_C3D_FrameCounter(int id);
PACEBOT_TEXT u32 __wrap_C3D_FrameCounter(int id) {
    extern uint8_t CtrScreens_pacerGame;
    return __real_C3D_FrameCounter(id == 0 ? (int) CtrScreens_pacerGame : id);
}

void __real_gspWaitForEvent(GSPGPU_Event id, bool nextEvent);
void __wrap_gspWaitForEvent(GSPGPU_Event id, bool nextEvent);
PACEBOT_TEXT void __wrap_gspWaitForEvent(GSPGPU_Event id, bool nextEvent) {
    extern uint8_t CtrScreens_pacerGame;
    if (id == GSPGPU_EVENT_VBlank0 && !nextEvent && CtrScreens_pacerGame != GFX_TOP) id = GSPGPU_EVENT_VBlank1;
    __real_gspWaitForEvent(id, nextEvent);
}

void CtrScreens_pacerFlip(gfxScreen_t screen, bool hasStereo, uint32_t now);

// releaseHeld with the game on the bottom screen, counting the game screen's flip (CtrScreens_pacerFlip), as the top
// screen's vblank release above does. Called with the lock held.
PACEBOT_TEXT static void pacebotRelease(uint32_t now, bool force) {
    extern __typeof__(pace) pacebotPace __asm__("pace");
    if (!pacebotPace.holding || (!force && 0 > (int32_t) (now - pacebotPace.holdUntil))) return;
    pacebotPace.holding = false;
    if (pacebotPace.heldTop) CtrScreens_pacerFlip(GFX_TOP, pacebotPace.heldTopStereo, now);
    if (pacebotPace.heldBottom) CtrScreens_pacerFlip(GFX_BOTTOM, false, now);
    pacebotPace.heldTop = pacebotPace.heldBottom = false;
}

PACEBOT_TEXT static void pacebotOnVBlank1(void* data) {
    extern uint8_t CtrScreens_pacerGame;
    extern __typeof__(pace) pacebotPace __asm__("pace");
    if (CtrScreens_pacerGame != GFX_TOP && pacebotPace.ready) {
        LightLock_Lock(&pacebotPace.lock);  // a bottom-to-top move may have rebased to the top clock meanwhile
        if (CtrScreens_pacerGame != GFX_TOP) pacebotRelease(__real_C3D_FrameCounter(1) + 1u, false);
        LightLock_Unlock(&pacebotPace.lock);
    }
    pacebotQueueVBlank1(data);
}

// The next frame's forced release of anything still held (CtrPacer_frameEnding's releaseHeld, which counts only the
// top screen's flips) done first with the game on the bottom, so the game screen's forced flip is counted as the
// game's; frameEnding then finds nothing held. (Under pacestress, a test control, it now comes before the stress sleep.)
// The APT hook's forced release stays as it was: it invalidates the shown count right after.
void __real_CtrPacer_frameEnding(void);
void __wrap_CtrPacer_frameEnding(void);
PACEBOT_TEXT void __wrap_CtrPacer_frameEnding(void) {
    extern uint8_t CtrScreens_pacerGame;
    extern __typeof__(pace) pacebotPace __asm__("pace");
    if (CtrScreens_pacerGame != GFX_TOP) {
        LightLock_Lock(&pacebotPace.lock);
        pacebotRelease(__real_C3D_FrameCounter(1), true);
        LightLock_Unlock(&pacebotPace.lock);
    }
    __real_CtrPacer_frameEnding();
}

void __real_gspSetEventCallback(GSPGPU_Event id, ThreadFunc cb, void* data, bool oneShot);
void __wrap_gspSetEventCallback(GSPGPU_Event id, ThreadFunc cb, void* data, bool oneShot);
PACEBOT_TEXT void __wrap_gspSetEventCallback(GSPGPU_Event id, ThreadFunc cb, void* data, bool oneShot) {
    if (id == GSPGPU_EVENT_VBlank1 && cb != nullptr) {
        pacebotQueueVBlank1 = cb;
        cb = pacebotOnVBlank1;
    }
    __real_gspSetEventCallback(id, cb, data, oneShot);
}

// screensMoveGame's wait for the game's carried frame (ctr_renderer.c): that frame's own screen's vblank, where it waited
// for the top screen's (gspWaitForVBlank); the same three words at the call, so screensMoveGame keeps its size.
void CtrScreens_pacerWaitShown(gfxScreen_t screen, bool nextEvent);
PACEBOT_TEXT void CtrScreens_pacerWaitShown(gfxScreen_t screen, bool nextEvent) {
    __real_gspWaitForEvent(screen == GFX_BOTTOM ? GSPGPU_EVENT_VBlank1 : GSPGPU_EVENT_VBlank0, nextEvent);
}

// CtrPacer_setGameScreen (above, kept in its slot) with the schedule moved to the new screen's vblanks. ctr_renderer.c
// calls it with nothing held (the GPU idle).
void __wrap_CtrPacer_setGameScreen(gfxScreen_t screen);
PACEBOT_TEXT void __wrap_CtrPacer_setGameScreen(gfxScreen_t screen) {
    extern uint8_t CtrScreens_pacerGame;
    extern __typeof__(pace) pacebotPace __asm__("pace");
    LightLock_Lock(&pacebotPace.lock);
    uint32_t shift = __real_C3D_FrameCounter((int) screen) - CtrFpsbox_moveOrigin((int) CtrScreens_pacerGame);
    pacebotPace.slotStart += shift;
    pacebotPace.holdUntil += shift;
    pacebotPace.frameDue += shift;
    pacebotPace.doneVblank += shift;
    pacebotPace.displayDue += shift;
    pacebotPace.checkDue += shift;
    CtrScreens_pacerGame = (uint8_t) screen;
    pacebotPace.lastShownValid = false;
    LightLock_Unlock(&pacebotPace.lock);
}

// ===[ utfade: a door's room change shows black (change utfade, late code; UNDERTALE only) ]===
//
// z_utfade.c calls this twice in a door's room change. fill true, before the room load: once the GPU is done and the
// game screen shows the last frame's flip (none held, none pending), the screen's spare buffer is filled black and
// goes through the frames' own swap (gfxScreenSwapBuffers, wrapped: CtrScreens_pacerSwap, which holds a flip while the
// slot's hold lasts and lets the vblank callbacks flip it, on either screen), with this frame's slot held as
// CtrPacer_frameEnding would hold it: the black shows on the vblank the frame would have shown on. fill false, after
// the load: until the hold's last vblank (holdUntil) has begun, so the frame end's forced release can't show the black
// early after a short load; from that vblank on, the vblank callback's release and the forced one both show it on the
// next vblank. Not until it shows: the frame end's own work (the rest of the change, CtrRenderer_endFrame) would then
// wait behind it, where it overlaps the black today (Azahar a712b495d: the next room 1-2 vblanks late). The
// hold is set without the lock: the vblank callbacks only clear it, and only once holdUntil has gone by, when the swap
// then flips at once, which shows the black on the same vblank. The black is this frame's own picture, so this frame
// keeps catchUp set (the next slot wait sets it again; only CtrPacer_frameBegun reads it): frameBegun runs at the frame's
// end, after the black has shown, and would otherwise take it for the frame before's late flip and move the schedule
// by a slot (utfade-ft2 console rows: the next room's first frame 2 vblanks late after short loads). `pace` by its
// assembler name, so the functions above keep their code.
#if defined(ENABLE_WAD16) && defined(__3DS__)
void CtrPacer_utfadeBlack(bool fill);
__attribute__((noinline, optimize("Os"), section(".text_utfade"))) void CtrPacer_utfadeBlack(bool fill) {
    extern uint8_t CtrScreens_pacerGame;
    extern __typeof__(pace) utfadePace __asm__("pace");
    gfxScreen_t screen = (gfxScreen_t) CtrScreens_pacerGame;
    if (!fill) {
        // The game screen's vblanks (pacebot's wraps), as CtrPacer_waitForSlot waits.
        while (0 > (int32_t) (vblankCount() - utfadePace.holdUntil)) gspWaitForEvent(GSPGPU_EVENT_VBlank0, false);
        return;
    }
    CtrGpu_waitIdle();
    while (!CtrPacer_screenReady(screen)) CtrScreens_pacerWaitShown(screen, true);
    u16 w, h;
    u8* fb = gfxGetFramebuffer(screen, GFX_LEFT, &w, &h);
    u32 bytes = (u32) w * h * 3u; // the game screen is BGR8 (ctr_renderer.c's screensMoveGame)
    memset(fb, 0, bytes);
    GSPGPU_FlushDataCache(fb, bytes);
    utfadePace.holdUntil = utfadePace.slotStart + utfadePace.slotLength - 1;
    // holdUntil in memory before holding (regaudit F6): the vblank callbacks run on libctru's GSP event thread, created
    // on the app's own core (gspInit: core -2), so a compiler barrier is the whole ordering they need.
    __asm__ volatile("" ::: "memory");
    utfadePace.holding = true;
    utfadePace.catchUp = true;
    gfxScreenSwapBuffers(screen, false);
}
#endif

// ===[ pacealign: where a bench window's edges cut the pacer's numbers (change auditfixe, late code; UNDERTALE only) ]===
//
// The bench marks a window's opening in its step of the window's first frame A and its end in its step of the frame
// after its last, B+1 (z_pacealign.c). The cut waits for that frame E's end, here: CtrRenderer_endFrame waits for the
// GPU to finish frame E-1, then hands the pacer that frame's GPU time with E's own frame end (CtrPacer_frameEnded,
// wrapped for UNDERTALE in CMakeLists.txt). The report a cut closes has:
//   - the main thread's time of every frame up to E-1, which E's slot wait charged before the bench's step: a window's
//     last is B's, and nothing the bench did after that wait;
//   - the GPU time of every frame up to E-1, taken here after the GPU finished it: B's too, also with --exit-after,
//     whose last frame ends the same way before the game leaves;
//   - the frame end and GPU wait of every frame up to E-1; E's go to the report the cut opens.
// E's slot (its frame count, and its skip if it goes unshown) goes to the report the cut opens too, unless that would
// leave the closing one at 0 frames, for which CtrPacer_logStats prints nothing and drops what it had: then the stats
// block ran in E-1 (or a cut there kept E-1's slot the same way), the slot stays, and the report the cut opens starts
// at 0 frames, which its next slot wait counts before a print can come (main.c prints every 5 s; a cut in E+1 keeps
// the slot the same way). So at such a collision a report's frame count is one off; the sum of each report's frames
// times its averages, and the maxima, are still the window's exactly, and its "Bench: ... measured frames A-B" line
// gives its frame count. GSP's callbacks count flips when they happen (a frame's done time and hold at its GPU finish,
// how long the picture before stayed up at its flip), so a window's last flips can come after its end. A frame that
// goes unshown has no frame end or GPU time (CtrRenderer_endFrame returns before CtrPacer_frameEnded), so its cut is
// in CtrPacer_frameBegun, which every frame calls after the GPU wait. `pace` by its assembler name, so the functions
// above keep their code.
#ifdef CTRPORT_PACEALIGN
void __real_CtrPacer_frameBegun(void);
void __real_CtrPacer_frameEnded(float gpuMs);
void __wrap_CtrPacer_frameBegun(void);
void __wrap_CtrPacer_frameEnded(float gpuMs);

// The cut: the edges' lines and the closing report (z_pacealign.c), then E's frame end and GPU wait if it is shown.
__attribute__((noinline, noclone, noipa, section(".text_pacealign"))) static void paceCut(bool shown, uint64_t end,
                                                                                          uint64_t gpuWait) {
    extern __typeof__(pace) cutPace __asm__("pace");
    LightLock_Lock(&cutPace.lock);
    uint32_t moved = cutPace.stats.frames >= 2 ? 1 : 0;
    uint32_t skipped = moved != 0 && cutPace.skipping && cutPace.stats.skipped > 0 ? 1 : 0;
    cutPace.stats.frames -= moved;
    cutPace.stats.skipped -= skipped;
    LightLock_Unlock(&cutPace.lock);
    CtrBench_paceCutLog();
    LightLock_Lock(&cutPace.lock);
    cutPace.stats.frames += moved;
    cutPace.stats.skipped += skipped;
    if (shown) {
        cutPace.stats.endTicks += end;
        if (end > cutPace.stats.endMaxTicks) cutPace.stats.endMaxTicks = end;
        cutPace.stats.gpuWaitTicks += gpuWait;
        if (gpuWait > cutPace.stats.gpuWaitMaxTicks) cutPace.stats.gpuWaitMaxTicks = gpuWait;
    }
    LightLock_Unlock(&cutPace.lock);
}

// CtrPacer_frameEnded's own numbers, with the cut between frame E-1's GPU time and frame E's frame end.
__attribute__((noinline, noclone, noipa, section(".text_pacealign"))) void __wrap_CtrPacer_frameEnded(float gpuMs) {
    extern __typeof__(pace) cutPace __asm__("pace");
    if (!CtrBench_paceCutDue()) {
        __real_CtrPacer_frameEnded(gpuMs);
        return;
    }
    uint64_t end = svcGetSystemTick() - cutPace.endTick;
    uint64_t gpuWait = CtrGpu_takeWaitTicks();
    LightLock_Lock(&cutPace.lock);
    cutPace.stats.gpuMs += gpuMs;
    if (gpuMs > cutPace.stats.gpuMaxMs) cutPace.stats.gpuMaxMs = gpuMs;
    LightLock_Unlock(&cutPace.lock);
    paceCut(true, end, gpuWait);
}

// An unshown frame's cut (its slot wait set pace.skipping; CtrRenderer_endFrame calls no CtrPacer_frameEnded for it).
__attribute__((noinline, noclone, noipa, section(".text_pacealign"))) void __wrap_CtrPacer_frameBegun(void) {
    extern __typeof__(pace) cutPace __asm__("pace");
    __real_CtrPacer_frameBegun();
    if (cutPace.skipping && CtrBench_paceCutDue()) paceCut(false, 0, 0);
}
#endif

// heldslow (change scanout, logaudit G1, 2026-11-08; UNDERTALE only, CTRPORT_HELDSLOW): a record of each game-screen
// image that stayed up longer than its frames' slots planned, printed by z_heldslow.c every [perf] window.
// - The frame's hand-over to the GPU (CtrHeldslow_transfer: ctr_gpu_queue.c's C3D_FrameEnd calls it by name for each
//   screen's display transfer, on the main thread, after the frame before is done) notes, for the game screen, the
//   frame (main.c's liveRunner, read by a function appended there), its room, and the vblanks its predecessor was
//   planned to stay up: the slot length (the larger of this frame's and the last handed-over one's) times the slots
//   since, counting the unshown frames the pacer meant (CtrPacer_skipSlot's half rate, counted as ctr_renderer.c's
//   call comes here by name; every step at Auto's levels, which show by a beat), and at Auto's levels the vblank a
//   beat moved this frame (CtrPacer_frameEnding's due, one past its slot) less the one it moved the frame before. A
//   frame skip's catch-up, a late frame, a room change's unshown frame and its load are not planned, so they make
//   records. All of it is read then, on the main thread, never from the main thread's state when the GPU is done:
//   by then the main thread may be in the next frame (c26rev7 F1).
// - The frames' swap (CtrScreens_pacerSwap, wrapped: ctr_argui.c's branch to it is the only caller), when the GPU is
//   done with the frame, takes that note for the flip that shows it (the next hand-over comes only after this swap).
//   A swap with no new hand-over since the last has no plan and makes no record.
// - The flips (this file's calls of __real_gfxScreenSwapBuffers, pointed here by name in CMakeLists.txt) compare the
//   game screen's flip totals with the last: one counted flip more, and the vblanks it added (how long the image it
//   replaces stayed up) over plan, is a record. The totals count nothing across a suspend or a screen change, so
//   those make none. The flips run with the lock held.
// State on the heap (allocated at the first game-screen hand-over); one pointer in the late data (4 bytes).
#ifdef CTRPORT_HELDSLOW
#include <stdlib.h>
#include "runner.h"
#include "ctr_startup_clock.h"
#include "z_heldslow.h"

#define HS_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_heldslow")))

typedef struct {
    // The frame swapped to the game screen last (pending its flip) and the one on it, with their rooms.
    uint32_t pendFrame, shownFrame;
    int16_t pendRoom, shownRoom;
    uint32_t pendPlanned;
    // The game screen's flip totals at its last flip.
    uint32_t flips;
    uint64_t sum;
    // The last hand-over's note (main thread; read by the swap after it): its frame, room and plan, and its count.
    uint32_t subFrame, subPlanned, subSeq, takenSeq;
    int16_t subRoom;
    // Main thread only: pace.frameIndex, the slot length, Auto's levels and the vblank its beat moved the frame, at the
    // last hand-over; and CtrPacer_skipSlot's unshown frames since.
    uint32_t subIndex, subLength, subAdj, skips;
    bool subLevel;
    uint32_t heldCount, heldMore;
    // Whether a flip of the game screen has come by since the state was made (the frame on it is known).
    bool primed;
    CtrHeldRecord held[CTR_HELD_RECORDS];
    // G2 (main thread only): the ticks at this frame's step, draw, audio and present marks; the slot start seen at the
    // last wait mark and that mark's tick; the slow frames kept since the last take, and how many more.
    uint64_t tick[4], seenSlot, lastWait;
    uint32_t slowCount, slowMore;
    // The suspends, sleeps and their ends (CtrHeldslow_onApt, the APT thread or the main one) and the count at the last
    // wait mark; the hook's cookie.
    uint32_t pauses, seenPauses;
    aptHookCookie aptCookie;
    CtrSlowRecord slow[CTR_SLOW_RECORDS];
    // The bottom's count (ctr_gpu_queue.c's appended code, either thread, under the GPU queue's lock; atomic for the
    // take): its transfers into the buffer on screen let through outside its blank, dlgblank's forced goes, and
    // whether it gave up holding them for the session.
    uint32_t bottomOutside, bottomForced;
    bool bottomGaveUp;
    // movehold (the end of this file; under the pacer's lock): at the game screen's last counted-from flip, its shown
    // vblank, the flip totals, the suspend/sleep count, which screen it was, and whether these are set.
    bool mhValid;
    uint8_t mhGame;
    uint32_t mhShown, mhFlips, mhPauses;
} CtrHeldslowState;

// NULL until the first game-screen swap; 1 if that allocation failed (nothing is recorded then).
CtrHeldslowState* CtrHeldslow_state __attribute__((section(".data.heldslow_late"))) = NULL;
Runner* CtrHeldslow_liveRunner(void); // main.c's liveRunner (its appended late text)
void CtrHeldslow_realSwap(gfxScreen_t screen, bool hasStereo) __asm__("__real_gfxScreenSwapBuffers");
void __real_CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo);
void __wrap_CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo);
void CtrHeldslow_swap(gfxScreen_t screen, bool hasStereo);
void CtrHeldslow_skipSlot(void);

void CtrHeldslow_transfer(C3D_FrameBuf* frameBuf, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags);
void CtrHeldslow_onApt(APT_HookType hook, void* param);

// The main thread's hand-over of a frame's display transfer (C3D_FrameEnd, after its wait for the frame before): for
// the game screen's (left) one, the frame's note. The state is made at the first.
HS_CODE void CtrHeldslow_transfer(C3D_FrameBuf* frameBuf, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags) {
    extern __typeof__(pace) hsPace __asm__("pace");
    CtrHeldslowState* s = CtrHeldslow_state;
    if (screen == (gfxScreen_t) CtrScreens_pacerGame && side == GFX_LEFT && hsPace.ready &&
        s != (CtrHeldslowState*) 1) {
        uint32_t length = hsPace.slotLength;
        if (s == NULL) {
            s = calloc(1, sizeof(*s));
            if (s != NULL) {
                LightLock_Lock(&hsPace.lock);
                s->flips = hsPace.flipTotals.flips;
                s->sum = hsPace.flipTotals.sum;
                LightLock_Unlock(&hsPace.lock);
                s->subIndex = hsPace.frameIndex - 1;
                s->subLength = length;
                s->shownRoom = s->pendRoom = s->subRoom = -1;
                CtrHeldslow_state = s;
                // As the other hooks made after start-up (z_sleepflush.c's): no parameter; the hook reads the state.
                aptHook(&s->aptCookie, CtrHeldslow_onApt, NULL);
            } else {
                CtrHeldslow_state = (CtrHeldslowState*) 1;
            }
        }
        if (s != NULL) {
            uint32_t slot = length > s->subLength ? length : s->subLength;
            // Auto's levels: the vblank the beat moved this frame's due (CtrPacer_frameEnding), at most one.
            uint32_t adj = hsPace.levelMode && hsPace.frameDue - hsPace.slotStart - length == 1u ? 1u : 0u;
            uint32_t planned = hsPace.levelMode || s->subLevel
                                   ? slot * (hsPace.frameIndex - s->subIndex) + adj - s->subAdj
                                   : slot * (1 + s->skips);
            if (1 > (int32_t) planned) planned = 1;
            Runner* runner = CtrHeldslow_liveRunner();
            s->subPlanned = planned;
            s->subFrame = runner != NULL ? (uint32_t) runner->frameCount : 0;
            s->subRoom = runner != NULL ? (int16_t) runner->currentRoomIndex : -1;
            s->subIndex = hsPace.frameIndex;
            s->subLength = length;
            s->subLevel = hsPace.levelMode;
            s->subAdj = adj;
            s->skips = 0;
            // The GPU queue's lock (taken after this, before the GPU has the frame) hands it to the swap's thread.
            __atomic_store_n(&s->subSeq, s->subSeq + 1u, __ATOMIC_RELEASE);
        }
    }
    C3D_FrameBufTransfer(frameBuf, screen, side, transferFlags);
}

// When the GPU is done with a frame (its queue's finish, the GSP thread): the game screen's swap takes its note.
HS_CODE void __wrap_CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo) {
    extern __typeof__(pace) hsPace __asm__("pace");
    CtrHeldslowState* s = CtrHeldslow_state;
    if (screen == (gfxScreen_t) CtrScreens_pacerGame && hsPace.ready && s != NULL && s != (CtrHeldslowState*) 1) {
        uint32_t seq = __atomic_load_n(&s->subSeq, __ATOMIC_ACQUIRE);
        // Read by the flip under the pacer's lock, which the swap takes before any flip of this frame.
        if (seq != s->takenSeq) {
            s->takenSeq = seq;
            s->pendPlanned = s->subPlanned;
            s->pendFrame = s->subFrame;
            s->pendRoom = s->subRoom;
        } else {
            s->pendPlanned = UINT32_MAX;
        }
    }
    __real_CtrScreens_pacerSwap(screen, hasStereo);
}

// APT's suspends (the HOME Menu, from aptMainLoop on the main thread), sleeps (APT's own thread) and their ends: G2
// counts no frame they fall in (CtrHeldslow_mark).
HS_CODE void CtrHeldslow_onApt(APT_HookType hook, void* param) {
    (void) param;
    CtrHeldslowState* s = CtrHeldslow_state;
    if (hook != APTHOOK_ONEXIT && s != NULL && s != (CtrHeldslowState*) 1) {
        __atomic_fetch_add(&s->pauses, 1u, __ATOMIC_RELEASE);
    }
}

HS_CODE void CtrHeldslow_skipSlot(void) {
    CtrPacer_skipSlot();
    CtrHeldslowState* s = CtrHeldslow_state;
    if (s != NULL && s != (CtrHeldslowState*) 1) s->skips++;
}

// The flips' way in (CtrHeldslow_swap, the name CMake gives this file's __real_gfxScreenSwapBuffers calls) is one
// assembler branch to the body: to the compiler this file's flips still call an outside function, as before. A call to
// a function defined here would change the order the compiler emits this file's own functions in, and move them.
#ifdef __3DS__
__asm__(".pushsection .text_heldslow,\"ax\",%progbits\n.global CtrHeldslow_swap\n.type CtrHeldslow_swap,%function\n"
        "CtrHeldslow_swap:\n\tb CtrHeldslow_swapBody\n.size CtrHeldslow_swap,4\n.popsection\n");
#endif
void CtrHeldslow_swapBody(gfxScreen_t screen, bool hasStereo);

// Called with the lock held, but for the swaps before the pacer is ready (which count no flips).
HS_CODE void CtrHeldslow_swapBody(gfxScreen_t screen, bool hasStereo) {
    extern __typeof__(pace) hsPace __asm__("pace");
    CtrHeldslowState* s = CtrHeldslow_state;
    if (s != NULL && s != (CtrHeldslowState*) 1 && screen == (gfxScreen_t) CtrScreens_pacerGame) {
        uint32_t flips = hsPace.flipTotals.flips;
        uint64_t sum = hsPace.flipTotals.sum;
        uint32_t vblanks = (uint32_t) (sum - s->sum);
        if (s->primed && flips == s->flips + 1 && vblanks > s->pendPlanned) {
            if (CTR_HELD_RECORDS > s->heldCount) {
                CtrHeldRecord* r = &s->held[s->heldCount++];
                r->frame = s->pendFrame;
                r->room = s->pendRoom;
                r->shownFrame = s->shownFrame;
                r->shownRoom = s->shownRoom;
                r->fromVblank = hsPace.lastShown - vblanks;
                r->vblanks = vblanks > 0xFFFFu ? 0xFFFFu : (uint16_t) vblanks;
                r->planned = s->pendPlanned > 0xFFFFu ? 0xFFFFu : (uint16_t) s->pendPlanned;
                r->ms = (uint32_t) (CtrStartup_elapsedUs() / 1000u);
            } else {
                s->heldMore++;
            }
        }
        s->flips = flips;
        s->sum = sum;
        s->shownFrame = s->pendFrame;
        s->shownRoom = s->pendRoom;
        s->primed = true;
    }
    CtrHeldslow_realSwap(screen, hasStereo);
}

HS_CODE uint32_t CtrHeldslow_takeHeld(CtrHeldRecord* out, uint32_t* more) {
    extern __typeof__(pace) hsPace __asm__("pace");
    CtrHeldslowState* s = CtrHeldslow_state;
    *more = 0;
    if (s == NULL || s == (CtrHeldslowState*) 1) return 0;
    LightLock_Lock(&hsPace.lock);
    uint32_t count = s->heldCount;
    memcpy(out, s->held, count * sizeof(*out));
    *more = s->heldMore;
    s->heldCount = s->heldMore = 0;
    LightLock_Unlock(&hsPace.lock);
    return count;
}

// heldslow G2 (change scanout, logaudit G2, 2026-11-08): a record of each frame whose main thread took over 33.4 ms, from
// its slot's start (pace.slotTick, the pacer's own "main" time) to the next frame's wait, split at main.c's watchdog
// marks (its calls of CtrWatchdog_mark come here by name, CMakeLists.txt): before the step, the step, the draw, audio
// and the room change, present and the frame's end. A frame that waited for no slot (CtrPacer_skipSlot's half rate
// keeps slotTick) counts from the last frame's wait. A frame a suspend (the HOME Menu) or a sleep falls in makes no
// record: an APT hook (registered with the state) counts them. Printed with the held images (z_heldslow.c); kept in
// the same state. Four ticks a frame at the step, draw, audio and present marks and one at the wait;
// CTRPORT_SLOWLOG_ONETICK keeps only the wait's (the frame's total, no parts).
#include "ctr_watchdog.h"

void CtrHeldslow_mark(const char* phase);

static HS_CODE uint16_t slowTenths(uint64_t ticks) {
    uint64_t tenths = ticks * 10u / (uint64_t) CPU_TICKS_PER_MSEC;
    return tenths >= CTR_SLOW_UNKNOWN ? (uint16_t) (CTR_SLOW_UNKNOWN - 1u) : (uint16_t) tenths;
}

HS_CODE void CtrHeldslow_mark(const char* phase) {
    extern __typeof__(pace) hsPace __asm__("pace");
    CtrWatchdog_mark(phase);
    CtrHeldslowState* s = CtrHeldslow_state;
    if (s == NULL || s == (CtrHeldslowState*) 1) return;
    char c = phase[0];
#ifndef CTRPORT_SLOWLOG_ONETICK
    if (c == 's' && phase[1] == 't') { s->tick[0] = svcGetSystemTick(); return; } // "step"
    if (c == 'd') { s->tick[1] = svcGetSystemTick(); return; }                    // "draw"
    if (c == 'a') { s->tick[2] = svcGetSystemTick(); return; }                    // "audio update"
    if (c == 'p') { s->tick[3] = svcGetSystemTick(); return; }                    // "present"
#endif
    if (c != 'w') return;                                                          // "wait for frame"
    uint64_t now = svcGetSystemTick();
    uint64_t slot = hsPace.slotTick;
    uint64_t start = slot != s->seenSlot ? slot : s->lastWait;
    s->seenSlot = slot;
    s->lastWait = now;
    // A suspend (the HOME Menu), a sleep or their end since the last wait: the time blocked in them is no frame's
    // work, so this frame makes no record (c26rev7 F3). The next counts from its own slot or this wait, after them.
    uint32_t pauses = __atomic_load_n(&s->pauses, __ATOMIC_ACQUIRE);
    if (pauses != s->seenPauses) {
        s->seenPauses = pauses;
        return;
    }
    if (start == 0) return;
    uint64_t busy = now - start;
    if ((uint64_t) (33.4 * CPU_TICKS_PER_MSEC) >= busy) return;
    if (CTR_SLOW_RECORDS > s->slowCount) {
        CtrSlowRecord* r = &s->slow[s->slowCount++];
        Runner* runner = CtrHeldslow_liveRunner();
        r->frame = runner != NULL ? (uint32_t) runner->frameCount : 0;
        r->room = runner != NULL ? (int16_t) runner->currentRoomIndex : -1;
        r->ms = (uint32_t) (CtrStartup_elapsedUs() / 1000u);
        r->tenths[0] = slowTenths(busy);
#ifndef CTRPORT_SLOWLOG_ONETICK
        // Each part only if its marks came in order this frame (a tick before the start is an earlier frame's).
        uint64_t at[6] = { start, s->tick[0], s->tick[1], s->tick[2], s->tick[3], now };
        for (int i = 0; 5 > i; i++) {
            r->tenths[1 + i] = at[i] >= start && at[i + 1] >= at[i] ? slowTenths(at[i + 1] - at[i]) : CTR_SLOW_UNKNOWN;
        }
#else
        for (int i = 0; 5 > i; i++) r->tenths[1 + i] = CTR_SLOW_UNKNOWN;
#endif
    } else {
        s->slowMore++;
    }
}

HS_CODE uint32_t CtrHeldslow_takeSlow(CtrSlowRecord* out, uint32_t* more) {
    CtrHeldslowState* s = CtrHeldslow_state;
    *more = 0;
    if (s == NULL || s == (CtrHeldslowState*) 1) return 0;
    uint32_t count = s->slowCount;
    memcpy(out, s->slow, count * sizeof(*out));
    *more = s->slowMore;
    s->slowCount = s->slowMore = 0;
    return count;
}

// The bottom's count: from ctr_gpu_queue.c's appended code, after dlgblank let a transfer into the bottom's shown buffer
// through without its blank (a forced go, or any go once it gave up), and for z_heldslow.c's line.
HS_CODE void CtrHeldslow_noteBottom(bool forced, bool outside, bool gaveUp) {
    CtrHeldslowState* s = CtrHeldslow_state;
    if (s == NULL || s == (CtrHeldslowState*) 1) return;
    if (outside) __atomic_fetch_add(&s->bottomOutside, 1u, __ATOMIC_RELAXED);
    if (forced) __atomic_fetch_add(&s->bottomForced, 1u, __ATOMIC_RELAXED);
    if (gaveUp) __atomic_store_n(&s->bottomGaveUp, true, __ATOMIC_RELAXED);
}

HS_CODE void CtrHeldslow_takeBottom(uint32_t* outside, uint32_t* forced, bool* gaveUp) {
    CtrHeldslowState* s = CtrHeldslow_state;
    *outside = *forced = 0;
    *gaveUp = false;
    if (s == NULL || s == (CtrHeldslowState*) 1) return;
    *outside = __atomic_exchange_n(&s->bottomOutside, 0u, __ATOMIC_RELAXED);
    *forced = __atomic_exchange_n(&s->bottomForced, 0u, __ATOMIC_RELAXED);
    *gaveUp = __atomic_load_n(&s->bottomGaveUp, __ATOMIC_RELAXED);
}
#endif // CTRPORT_HELDSLOW

// ===[ movehold: the game picture a screen move kept up, counted as its frame's (change bonepulse, 11-09) ]===
//
// (UNDERTALE only, late code.) A screen move (ctr_renderer.c's two moves, at an Options toggle) rebases the pacer to the
// new game screen's vblanks (__wrap_CtrPacer_setGameScreen), which clears lastShownValid. The first frame flipped on
// the new screen then counted nothing. So the picture that stayed up across the move, from its last flip on the old
// screen to that first one (CtrScreens_pacerShow's handover presents of it in between), was in no [pace] bucket and
// made no [held] record (bstoggle: frame 900, 75 ms, 0 counted flips at 901).
// - Every swap from this file's flips comes here first: CMake points this file's __real_gfxScreenSwapBuffers here, and
//   this goes on to heldslow's CtrHeldslow_swap. The flips count before they swap, with the lock held once the pacer
//   is ready.
// - At each game-screen swap while lastShownValid holds, heldslow's state keeps the screen, its shown vblank
//   (lastShown), the flip totals and heldslow's suspend/sleep count.
// - The first game flip after a rebase has lastShownValid again, unchanged totals, a different game screen and no
//   suspend or sleep since. Its interval runs from the kept vblank, moved to the new screen's count by the two screens'
//   frame counters now (as the rebase moves the schedule), to this flip's. It goes into the window's shown buckets and
//   the flip totals as flip() would count it, before heldslow's swap reads them: so a move that kept the picture up
//   past its slot makes a [held] record, and [pace] shows it in its buckets.
// - pacerShow's handover presents are part of that interval, not frames: lastShownValid is clear while they run.
// - Counting only: lastShown, lastShownValid and the schedule are untouched. The totals' readers (bench, fpsbox,
//   spikerec, rosalina's log, heldslow) only report. A move with no heldslow state (before the first game hand-over,
//   or a failed allocation) counts nothing, as before.
// State: 16 bytes at the end of heldslow's heap state; no late data.
#ifdef CTRPORT_HELDSLOW
#define MH_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text.movehold_late")))
void CtrMovehold_swapBody(gfxScreen_t screen, bool hasStereo);
// The flips' way in, as heldslow's: one assembler branch, so to the compiler this file's flips still call an outside
// function and its own functions keep their order.
#ifdef __3DS__
__asm__(".pushsection .text.movehold_late,\"ax\",%progbits\n.global CtrMovehold_swap\n.type CtrMovehold_swap,%function\n"
        "CtrMovehold_swap:\n\tb CtrMovehold_swapBody\n.size CtrMovehold_swap,4\n.popsection\n");
#endif

// countFlip's sums, kept here so the flips' own calls of it compile as before.
static MH_CODE void moveHoldCount(CtrFlipStats* f, uint32_t vblanks) {
    f->count[vblanks > CTR_FLIP_BUCKETS ? CTR_FLIP_BUCKETS - 1 : vblanks - 1]++;
    f->flips++;
    f->sum += vblanks;
    f->sumSq += (uint64_t) vblanks * vblanks;
}

// Called with the lock held, but for the swaps before the pacer is ready (which count nothing).
MH_CODE void CtrMovehold_swapBody(gfxScreen_t screen, bool hasStereo) {
    extern __typeof__(pace) mhPace __asm__("pace");
    CtrHeldslowState* s = CtrHeldslow_state;
    uint8_t game = CtrScreens_pacerGame;
    if (s != NULL && s != (CtrHeldslowState*) 1 && mhPace.ready && mhPace.lastShownValid &&
        screen == (gfxScreen_t) game) {
        uint32_t pauses = __atomic_load_n(&s->pauses, __ATOMIC_ACQUIRE);
        uint32_t flips = mhPace.flipTotals.flips;
        if (s->mhValid && game != s->mhGame && flips == s->mhFlips && pauses == s->mhPauses) {
            uint32_t from = s->mhShown + (__real_C3D_FrameCounter((int) game) - __real_C3D_FrameCounter((int) s->mhGame));
            uint32_t vblanks = mhPace.lastShown - from;
            if (vblanks > 0u && 0x10000u > vblanks) {
                mhPace.stats.shown[vblanks >= 4u ? 3u : vblanks - 1u]++;
                moveHoldCount(&mhPace.stats.flips, vblanks);
                moveHoldCount(&mhPace.flipTotals, vblanks);
                if (mhPace.levelMode) {
                    moveHoldCount(&mhPace.levelTotals.flips[mhPace.flipLevel], vblanks);
                    if (vblanks > mhPace.levelTotals.maxVblanks[mhPace.flipLevel]) {
                        mhPace.levelTotals.maxVblanks[mhPace.flipLevel] = vblanks;
                    }
                }
                flips++;
            }
        }
        s->mhShown = mhPace.lastShown;
        s->mhFlips = flips;
        s->mhPauses = pauses;
        s->mhGame = game;
        s->mhValid = true;
    }
    CtrHeldslow_swap(screen, hasStereo);
}
#endif // CTRPORT_HELDSLOW (movehold)

// ===[ bottomflip: the second screen's held flip on its own vblank, and both screens' real shown counts ]===
//
// (change bonepulse, 11-09; UNDERTALE only, late code; docs/3ds-hardware-2.md, "The bottom screen's held flip".)
// With the game on top, a frame that also flips a double-buffered bottom screen has both flips held for the slot's
// last vblank when it is done before it, and the top's vblank callback let both go
// (releaseHeld). GSP applies a present at its own screen's vblank, and VBlank1 comes 0.02-1.36 ms after VBlank0
// (docs/3ds-hardware.md, "Both screens' vblanks fire together"). So a held bottom flip mostly beat GSP's VBlank1 and
// showed one vblank early, now and then lost and showed on time, while a frame done after that vblank (not held)
// showed on time too: whenever frames moved between held and not held, the bottom screen showed frames for 3 then 1
// vblanks while the top kept 2/2/2. The pacer counts only the game screen's flips, so no [pace] or [held] line saw it.
// - A held bottom flip let go from the top's vblank callback (the GSP event thread, the game on top) is kept here and
//   presented from the bottom's own vblank callback, which runs right after: it shows a vblank later, with the top's.
//   Until then the bottom reads as present-pending, so nothing writes its buffers (the GPU queue's transfers,
//   bottompaint's copy and every screen-ready wait ask gspIsPresentPending, through the pacer or directly). Forced
//   releases on other threads (the next frame's frame end, suspend) present at once, as before. A newer present
//   for the bottom replaces a kept one.
// - Each screen's presents are counted, and at each bottom vblank a present no longer pending has been shown: the
//   vblanks it stayed up (1/2/3/4+), judder (an interval 2 or more from the one before, a 3-then-1 pair) and presents
//   replaced before they showed. One [screens] line per [pace] window that showed anything on the bottom screen. The
//   top's counts are the real ones that the pacer's inferred shown counts stand for.
// - State on the heap, made on the main thread when the renderer starts the pacer (CtrPacer_init), before the first
//   frame, so every frame is covered; one pointer in the late data. A failed allocation leaves it off (old presents).
//   (Before 11-09's review: made at the first [pace] window, so the first window's frames were not covered.)
// Hooks are link wraps: gspPresentBuffer and gspIsPresentPending (libctru), the bottom's vblank callback's lock call
// (CtrBottomPaint_vblank1Lock, ctr_gpu_queue.c's onVBlank1, after it counts the vblank), CtrGpu_takePaceStats
// (CtrPacer_logStats's call, for the line) and CtrPacer_init (ctr_renderer.c's call, for the state; DELTARUNE's
// pixfix wraps it there, and this is UNDERTALE only). No existing body changes.
#if defined(CTRPORT_BOTTOMFLIP) && defined(__3DS__)
#include <stdlib.h>
#define BF_CODE __attribute__((noinline, optimize("Os"), section(".text.bottomflip_late")))
#define BF_CONST __attribute__((aligned(4), section(".rodata.bottomflip_in_text")))

typedef struct {
    uint32_t shown[4], judder, replaced, same;
} CtrBfCounts;

typedef struct {
    // Presents per screen (any thread, atomic) and the count seen at the last bottom vblank.
    uint32_t seq[2], seen[2];
    // Bottom vblanks counted here; each screen's last shown one and the interval before.
    uint32_t vblanks, lastShown[2], prevInterval[2];
    bool lastShownValid[2];
    // The kept bottom present (pace's lock held to change it).
    bool kept;
    unsigned keptSwap;
    const void* keptA;
    const void* keptB;
    u32 keptStride, keptMode;
    // Each screen's last presented buffer: a present of the same one is a single buffer's (the transfer, not the
    // present, changes the picture).
    const void* lastFb[2];
    // The GSP event thread (seen at the bottom's vblank callback).
    Thread gspThread;
    // This window's counts (pace's lock held), and the held flips moved to the bottom's vblank.
    CtrBfCounts counts[2];
    uint32_t moved;
} CtrBfState;

// NULL until the pacer starts; 1 if that allocation failed.
CtrBfState* CtrBottomflip_state __attribute__((section(".data.bottomflip_late"))) = NULL;

static const char bfLine[] BF_CONST =
    "[screens] top shown for 1/2/3/4+ vblanks %lu/%lu/%lu/%lu, judder %lu, replaced %lu, single buffer %lu; bottom shown "
    "for 1/2/3/4+ vblanks %lu/%lu/%lu/%lu, judder %lu, replaced %lu, single buffer %lu; %lu held bottom flips moved to its vblank\n";

static inline CtrBfState* bfReady(void) {
    CtrBfState* s = CtrBottomflip_state;
    return s == NULL || s == (CtrBfState*) 1 ? NULL : s;
}

bool __real_gspIsPresentPending(unsigned screen);
bool __wrap_gspIsPresentPending(unsigned screen);
BF_CODE bool __wrap_gspIsPresentPending(unsigned screen) {
    CtrBfState* s = bfReady();
    if (s != NULL && screen == GFX_BOTTOM && __atomic_load_n(&s->kept, __ATOMIC_ACQUIRE)) return true;
    return __real_gspIsPresentPending(screen);
}

bool __real_gspPresentBuffer(unsigned screen, unsigned swap, const void* fb_a, const void* fb_b, u32 stride, u32 mode);
bool __wrap_gspPresentBuffer(unsigned screen, unsigned swap, const void* fb_a, const void* fb_b, u32 stride, u32 mode);
BF_CODE bool __wrap_gspPresentBuffer(unsigned screen, unsigned swap, const void* fb_a, const void* fb_b, u32 stride, u32 mode) {
    extern uint8_t CtrScreens_pacerGame;
    extern __typeof__(pace) bfPace __asm__("pace");
    CtrBfState* s = bfReady();
    if (s == NULL || screen > GFX_BOTTOM) return __real_gspPresentBuffer(screen, swap, fb_a, fb_b, stride, mode);
    if (fb_a == s->lastFb[screen]) __atomic_fetch_add(&s->counts[screen].same, 1u, __ATOMIC_RELAXED);
    s->lastFb[screen] = fb_a;
    if (screen == GFX_BOTTOM && CtrScreens_pacerGame == GFX_TOP && bfPace.ready && bfPace.heldBottom && !bfPace.holding &&
        s->gspThread != nullptr && threadGetCurrent() == s->gspThread) {
        // releaseHeld from the top's vblank callback, pace's lock held: keep it for the bottom's vblank.
        if (s->kept) s->counts[GFX_BOTTOM].replaced++;
        s->keptSwap = swap;
        s->keptA = fb_a;
        s->keptB = fb_b;
        s->keptStride = stride;
        s->keptMode = mode;
        __atomic_store_n(&s->kept, true, __ATOMIC_RELEASE);
        s->moved++;
        return false;
    }
    // A newer bottom present replaces a kept one; callers without pace's lock (screen setup) count atomically.
    bool replaced = screen == GFX_BOTTOM && __atomic_exchange_n(&s->kept, false, __ATOMIC_ACQ_REL);
    if (replaced || __real_gspIsPresentPending(screen)) __atomic_fetch_add(&s->counts[screen].replaced, 1u, __ATOMIC_RELAXED);
    bool r = __real_gspPresentBuffer(screen, swap, fb_a, fb_b, stride, mode);
    __atomic_add_fetch(&s->seq[screen], 1u, __ATOMIC_RELEASE);
    return r;
}

void __real_CtrBottomPaint_vblank1Lock(LightLock* gate);
void __wrap_CtrBottomPaint_vblank1Lock(LightLock* gate);
BF_CODE void __wrap_CtrBottomPaint_vblank1Lock(LightLock* gate) {
    extern __typeof__(pace) bfPace __asm__("pace");
    CtrBfState* s = bfReady();
    if (s != NULL) {
        s->gspThread = threadGetCurrent();
        LightLock_Lock(&bfPace.lock);
        s->vblanks++;
        for (unsigned i = 0; 2 > i; i++) {
            uint32_t q = __atomic_load_n(&s->seq[i], __ATOMIC_ACQUIRE);
            if (q == s->seen[i] || __real_gspIsPresentPending(i)) continue;
            s->seen[i] = q;
            if (s->lastShownValid[i]) {
                uint32_t iv = s->vblanks - s->lastShown[i];
                if (iv > 0) {
                    CtrBfCounts* c = &s->counts[i];
                    c->shown[iv >= 4u ? 3u : iv - 1u]++;
                    if (s->prevInterval[i] != 0 && (iv >= s->prevInterval[i] + 2u || s->prevInterval[i] >= iv + 2u)) c->judder++;
                    s->prevInterval[i] = iv;
                }
            }
            s->lastShown[i] = s->vblanks;
            s->lastShownValid[i] = true;
        }
        if (s->kept) {
            // Shown at the next bottom vblank, with the top's flip of the same frame.
            __atomic_store_n(&s->kept, false, __ATOMIC_RELEASE);
            __real_gspPresentBuffer(GFX_BOTTOM, s->keptSwap, s->keptA, s->keptB, s->keptStride, s->keptMode);
            __atomic_add_fetch(&s->seq[GFX_BOTTOM], 1u, __ATOMIC_RELEASE);
        }
        LightLock_Unlock(&bfPace.lock);
    }
    __real_CtrBottomPaint_vblank1Lock(gate);
}

void __real_CtrPacer_init(void);
void __wrap_CtrPacer_init(void);
BF_CODE void __wrap_CtrPacer_init(void) {
    if (CtrBottomflip_state == NULL) {
        CtrBfState* made = (CtrBfState*) calloc(1, sizeof(CtrBfState));
        __atomic_store_n(&CtrBottomflip_state, made != NULL ? made : (CtrBfState*) 1, __ATOMIC_RELEASE);
    }
    __real_CtrPacer_init();
}

void __real_CtrGpu_takePaceStats(CtrGpuPaceStats* out);
void __wrap_CtrGpu_takePaceStats(CtrGpuPaceStats* out);
BF_CODE void __wrap_CtrGpu_takePaceStats(CtrGpuPaceStats* out) {
    extern __typeof__(pace) bfPace __asm__("pace");
    __real_CtrGpu_takePaceStats(out);
    CtrBfState* s = bfReady();
    if (s == NULL) return;
    CtrBfCounts c[2];
    LightLock_Lock(&bfPace.lock);
    memcpy(c, s->counts, sizeof(c));
    memset(s->counts, 0, sizeof(s->counts));
    uint32_t moved = s->moved;
    s->moved = 0;
    LightLock_Unlock(&bfPace.lock);
    const CtrBfCounts* b = &c[GFX_BOTTOM];
    if (b->shown[0] + b->shown[1] + b->shown[2] + b->shown[3] + b->replaced + b->same + moved == 0) return;
    logInfo(bfLine, (unsigned long) c[0].shown[0], (unsigned long) c[0].shown[1], (unsigned long) c[0].shown[2],
            (unsigned long) c[0].shown[3], (unsigned long) c[0].judder, (unsigned long) c[0].replaced, (unsigned long) c[0].same, (unsigned long) b->shown[0],
            (unsigned long) b->shown[1], (unsigned long) b->shown[2], (unsigned long) b->shown[3], (unsigned long) b->judder,
            (unsigned long) b->replaced, (unsigned long) b->same, (unsigned long) moved);
}
#endif // CTRPORT_BOTTOMFLIP

// A port menu has already presented this iteration by copying at its screen's
// vblank. Its schedule must not then follow the paused game's late GPU flip.
// Use the existing per-frame keep-schedule policy, as utfadeBlack does above;
// waitForSlot/levelWait overwrites it before the next frame, including resume.
// Only the qualified final Options/Legal/Credits and setup-notice copies call
// here. No GPU, event or extra vblank wait; keep the copy's scan-safe wait.
// The assembler alias keeps every pre-existing pacer body byte-identical.
#if defined(ENABLE_WAD16) && defined(__3DS__)
void* CtrPortmenu_copy(void* to, const void* from, size_t bytes);
__attribute__((noinline, noipa, optimize("Os"), section(".text.late.noticescroll_menucopy")))
void* CtrPortmenu_copy(void* to, const void* from, size_t bytes) {
    extern __typeof__(pace) menuCopyPace __asm__("pace");
    menuCopyPace.catchUp = true;
    return memcpy(to, from, bytes);
}
#endif
