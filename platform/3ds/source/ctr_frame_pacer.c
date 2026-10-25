#include "ctr_frame_pacer.h"

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
// flips are let go from the top screen's vblank callback.
// A frame is still shown after its CPU and GPU time together, though.
// On the Old 3DS a frame can take 30 ms of the CPU and 5 of the
// GPU: it shows a vblank late, and moving the next slot to that vblank cost
// the main thread a vblank of its time on every such frame (a third of
// Waterfall's frames stayed up for three vblanks at 30 fps). Instead, frames
// are shown a vblank after their slots from then on (the delay) and the slots
// keep their place, so the GPU has a vblank more for each frame and the main
// thread's time isn't cut; a frame a little over its slot on the CPU then
// costs only the time it is over. The delay goes once frames have been done
// in time without it for a while; that frame stays up a vblank less. It only
// starts on a second late frame soon after another: a lone late frame (the
// main thread held up once, by the SD card say) moves the slots as before,
// since with the delay a light frame's transfers wait a vblank for the last
// flip, and a hitch of half a slot then makes it late again.

void __real_gfxScreenSwapBuffers(gfxScreen_t screen, bool hasStereo);
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
    // Frames shown a vblank after their slot (the delay).
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
    // Vblanks frames are shown after their slots end (0 or 1), and how many
    // frames in a row would have been in time without it.
    uint32_t delay;
    uint32_t inTimeRun;
    // Frames begun so far, and the one whose flip was last late without the delay.
    uint32_t frameIndex;
    uint32_t lateFrame;
    bool lateFrameValid;
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

// Frames in a row done in time without the delay before it goes (3 s at 30 fps).
#define PACER_RECOVERY_LENGTH_FRAMES 90u
// A late frame starts the delay when the last one was at most this many frames before.
#define DELAY_TRIGGER_FRAMES 15

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
    pace.lateFrameValid = false;
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

static void levelCheckFlip(void);

void CtrPacer_frameBegun(void) {
    if (pace.levelMode) {
        levelCheckFlip();
        return;
    }
    // A frame that isn't shown keeps to the schedule.
    if (pace.skipping) return;
    // The GPU is done with the last frame, so its flip has been asked for. If
    // that was too late for the vblank it was due on (this slot's first, or
    // the one after with the delay), frames get the delay if another frame
    // was late shortly before, unless the main thread was a vblank or more
    // late for this slot (one slow frame); past that, the slot starts on the
    // vblank it shows on instead, so it stays up for a whole slot too.
    // Catching up, the slots keep to their schedule regardless.
    LightLock_Lock(&pace.lock);
    uint32_t delay = pace.delay;
    // Fixed 2:1 overlap: the shown frame before was handed to the GPU at the end of its own slot's step and draw, and
    // the GPU ran it during the unshown frame and this one. It is late if it flipped after its own slot's end plus
    // the delay (rather than after this slot's start), and the delay can grow to two slots (one of shown frames).
    bool deep = pace.overlap && pace.halfSkipped;
    uint32_t delayMax = deep ? 2 * pace.slotLength : 1;
    if (pace.lastShownValid) {
        int32_t late = (int32_t) (pace.lastShown - (deep ? pace.frameDue : pace.slotStart)) - (int32_t) delay;
        if (late > 0) {
            pace.inTimeRun = 0;
            if (delayMax > pace.delay && pace.slotLength >= 2 && !pace.slotLate) {
                if (pace.lateFrameValid && DELAY_TRIGGER_FRAMES >= pace.frameIndex - pace.lateFrame) {
                    uint32_t step = delayMax - pace.delay < (uint32_t) late ? delayMax - pace.delay : (uint32_t) late;
                    pace.delay += step;
                    late -= (int32_t) step;
                }
                pace.lateFrame = pace.frameIndex;
                pace.lateFrameValid = true;
            }
            if (late > 0 && !pace.catchUp) {
                pace.slotStart += (uint32_t) late;
                pace.stats.shifted++;
            }
        }
    }
    if (pace.doneValid) {
        pace.doneValid = false;
        if (delay > 0 && pace.delay > 0) {
            // Done in time for a vblank less of delay.
            pace.inTimeRun = 0 > (int32_t) (pace.doneVblank - pace.frameDue - (delay - 1)) ? pace.inTimeRun + 1 : 0;
            if (pace.inTimeRun >= PACER_RECOVERY_LENGTH_FRAMES) {
                pace.delay--;
                pace.inTimeRun = 0;
            }
        }
    }
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
    pace.holdUntil = pace.slotStart + pace.slotLength - 1 + pace.delay;
    pace.frameDue = pace.slotStart + pace.slotLength;
    if (pace.delay > 0) pace.stats.delayed++;
    pace.frameSlotTick = pace.slotTick;
    pace.doneRecorded = false;
    pace.halfSkipped = false;
    if (pace.levelMode) {
        // Auto's levels: due at the slot's end, but no sooner than the level's beat after the shown frame before.
        static const uint8_t beat[CTR_LEVEL_COUNT] = { 2, 3, 4, 6 };
        uint32_t period = pace.slotLength == 2 ? beat[pace.level] : pace.slotLength;
        uint32_t due = pace.slotStart + pace.slotLength;
        if (pace.displayDueValid && 0 > (int32_t) (due - (pace.displayDue + period))) due = pace.displayDue + period;
        pace.displayDue = due;
        pace.displayDueValid = true;
        pace.frameDue = due;
        pace.holdUntil = due - 1 + pace.delay;
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

static const uint8_t levelPeriod[CTR_LEVEL_COUNT] = { 1, 3, 2, 3 };
// The most delay (vblanks) shown frames get at each level: one at level 0 as before; at 1 the next shown frame can
// begin a slot after one, and would let go of that one's held flip early with more.
static const uint8_t levelDelayMax[CTR_LEVEL_COUNT] = { 1, 1, 3, 5 };

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

// From CtrPacer_frameBegun, the GPU done with the frame before: whether the last shown frame flipped after it was due
// (with the delay). If so, and another was late shortly before, the delay grows up to the level's most; after
// PACER_RECOVERY_LENGTH_FRAMES shown frames done a vblank of delay sooner, it shrinks.
static void levelCheckFlip(void) {
    LightLock_Lock(&pace.lock);
    uint32_t delayMax = pace.slotLength == 2 ? levelDelayMax[pace.level] : 1;
    if (pace.checkPending && pace.lastShownValid) {
        pace.checkPending = false;
        int32_t late = (int32_t) (pace.lastShown - pace.checkDue) - (int32_t) pace.delay;
        if (late > 0) {
            pace.inTimeRun = 0;
            if (delayMax > pace.delay) {
                if (pace.lateFrameValid && DELAY_TRIGGER_FRAMES >= pace.frameIndex - pace.lateFrame) {
                    uint32_t step = delayMax - pace.delay < (uint32_t) late ? delayMax - pace.delay : (uint32_t) late;
                    pace.delay += step;
                }
                pace.lateFrame = pace.frameIndex;
                pace.lateFrameValid = true;
            }
        } else if (pace.doneValid && pace.delay > 0) {
            pace.inTimeRun = 0 > (int32_t) (pace.doneVblank - pace.checkDue - (pace.delay - 1)) ? pace.inTimeRun + 1 : 0;
            if (pace.inTimeRun >= PACER_RECOVERY_LENGTH_FRAMES) {
                pace.delay--;
                pace.inTimeRun = 0;
            }
        }
        pace.doneValid = false;
    }
    if (pace.delay > delayMax) pace.delay = delayMax;
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

#if defined(CTRPORT_PACER_OBSERVE)
#include "ctr_frame_pacer_prototype.inc"
#endif
