// Appended to ctr_frame_pacer.c: do not change the original pace object or bodies.
// Link wrappers keep all original functions, their sizes and their addresses.
#include "ctr_pacer_policy.h"
#define PACER_LATE __attribute__((noinline, section(".text.late.pacer")))
extern __typeof__(pace) pacerPace __asm__("pace");

static struct {
    bool roomNeededDelay;
    uint64_t doneSlot, doneTicks;
    // The benchmark calls getFlipTotals with one persistent start pointer and
    // a different, stack-local result pointer. Only those fences log, never flips.
    CtrFlipStats* startPointer;
    bool measuring, delayed;
    uint64_t startTick, modeTick, delayedTicks, delaySum;
    uint32_t frames, shown, lastShown, startFlips, firstActivations, reactivations;
    uint32_t longest[64];
} pacerPrototype __attribute__((section(".bss.late.pacer")));

static PACER_LATE void pacerRoomReset(void) {
    LightLock_Lock(&pacerPace.lock);
    pacerPrototype.roomNeededDelay = false;
    pacerPrototype.doneSlot = 0;
    // Neither an inherited cushion nor another room's first miss is evidence
    // that the new room needs buffering. Keep the inherited delay itself.
#if CTRPORT_PACER_R2R3A
    pacerPace.lateFrameValid = false;
    pacerPace.inTimeRun = 0;
#endif
    LightLock_Unlock(&pacerPace.lock);
}

void __real_CtrPacer_setFrameRate(float fps);
void __wrap_CtrPacer_setFrameRate(float fps);
PACER_LATE void __wrap_CtrPacer_setFrameRate(float fps) {
    pacerRoomReset();
    __real_CtrPacer_setFrameRate(fps);
}
void __real_CtrPacer_levelRoomChanged(void);
void __wrap_CtrPacer_levelRoomChanged(void);
PACER_LATE void __wrap_CtrPacer_levelRoomChanged(void) {
    pacerRoomReset();
    __real_CtrPacer_levelRoomChanged();
}
void __real_CtrPacer_levelReset(void);
void __wrap_CtrPacer_levelReset(void);
PACER_LATE void __wrap_CtrPacer_levelReset(void) {
    pacerRoomReset();
    __real_CtrPacer_levelReset();
}

void __real_CtrScreens_pacerSwap(gfxScreen_t screen, bool stereo);
void __wrap_CtrScreens_pacerSwap(gfxScreen_t screen, bool stereo);
PACER_LATE void __wrap_CtrScreens_pacerSwap(gfxScreen_t screen, bool stereo) {
    __real_CtrScreens_pacerSwap(screen, stereo);
    // Conservative duration: the original tests the tick before its lock;
    // this sample is after that callback. Keep its doneVblank test as well.
    if (screen != (gfxScreen_t) CtrScreens_pacerGame || !pacerPace.ready) return;
    LightLock_Lock(&pacerPace.lock);
    if (pacerPace.doneValid && pacerPace.frameSlotTick != pacerPrototype.doneSlot) {
        pacerPrototype.doneSlot = pacerPace.frameSlotTick;
        pacerPrototype.doneTicks = svcGetSystemTick() - pacerPace.frameSlotTick;
    }
    LightLock_Unlock(&pacerPace.lock);
}

void __real_CtrPacer_frameBegun(void);
void __wrap_CtrPacer_frameBegun(void);
PACER_LATE void __wrap_CtrPacer_frameBegun(void) {
    LightLock_Lock(&pacerPace.lock);
    uint32_t before = pacerPace.delay;
#if CTRPORT_PACER_R2R3A
    // This prototype changes only the normal, no-skip slot policy. Auto and
    // fixed overlap have different deadlines and remain on their existing rule.
    bool normal = !pacerPace.levelMode && !pacerPace.skipping && !pacerPace.overlap && !pacerPace.catchUp;
    if (normal) {
        if (pacerCanReactivate(pacerPrototype.roomNeededDelay, before, true,
                               pacerPace.slotLength, pacerPace.slotLate)) {
            // Arm the existing branch, which checks lateness under its own lock.
            // This also covers a held release between the wrapper and that lock.
            // An actual miss is consumed before residual lateness shifts slots.
            pacerPace.lateFrameValid = true;
            pacerPace.lateFrame = pacerPace.frameIndex;
        }
        if (before > 0 && pacerPace.doneValid) {
            uint64_t budget = (pacerPace.slotLength + before - 1) * VBLANK_TICKS;
            if (pacerPrototype.doneSlot != pacerPace.frameSlotTick ||
                !pacerRecoveryHasMargin(pacerPrototype.doneTicks, budget, SYSCLOCK_ARM11 / 1000)) {
                pacerPace.inTimeRun = 0;
                pacerPace.doneValid = false;
            }
        }
    }
#endif
    LightLock_Unlock(&pacerPace.lock);
    __real_CtrPacer_frameBegun();
    LightLock_Lock(&pacerPace.lock);
    if (before == 0 && pacerPace.delay > 0) {
        if (pacerPrototype.roomNeededDelay) pacerPrototype.reactivations++;
        else pacerPrototype.firstActivations++;
        pacerPrototype.roomNeededDelay = true;
    }
    LightLock_Unlock(&pacerPace.lock);
}

// Actual present calls include held releases on the vblank thread. Their caller
// already holds pace.lock; taking it again here would deadlock. No log or clock
// call is made on this path. lastShown only advances on a non-dropped game flip.
bool __real_gspPresentBuffer(unsigned screen, unsigned swap, const void* a, const void* b, u32 stride, u32 mode);
bool __wrap_gspPresentBuffer(unsigned screen, unsigned swap, const void* a, const void* b, u32 stride, u32 mode);
PACER_LATE bool __wrap_gspPresentBuffer(unsigned screen, unsigned swap, const void* a, const void* b, u32 stride, u32 mode) {
    bool pending = __real_gspPresentBuffer(screen, swap, a, b, stride, mode);
    if (pacerPrototype.measuring && screen == CtrScreens_pacerGame && pacerPace.lastShownValid &&
        pacerPace.lastShown != pacerPrototype.lastShown) {
        uint32_t interval = pacerPace.lastShown - pacerPrototype.lastShown;
        pacerPrototype.lastShown = pacerPace.lastShown;
        pacerPrototype.shown++;
        // Keep the exact longest 64 intervals. Refuse a low requiring >64,
        // rather than silently treating the old histogram's 8+ as exactly 8.
        for (unsigned i = 0; i < 64; i++) {
            if (interval > pacerPrototype.longest[i]) {
                uint32_t old = pacerPrototype.longest[i];
                pacerPrototype.longest[i] = interval;
                interval = old;
            }
        }
    }
    return pending;
}

static PACER_LATE void pacerModeAccount(uint64_t tick) {
    if (pacerPrototype.delayed) pacerPrototype.delayedTicks += tick - pacerPrototype.modeTick;
    pacerPrototype.modeTick = tick;
}
void __real_CtrPacer_frameEnding(void);
void __wrap_CtrPacer_frameEnding(void);
PACER_LATE void __wrap_CtrPacer_frameEnding(void) {
    __real_CtrPacer_frameEnding();
    LightLock_Lock(&pacerPace.lock);
    if (pacerPrototype.measuring) {
        pacerModeAccount(svcGetSystemTick());
        pacerPrototype.delayed = pacerPace.delay > 0;
        pacerPrototype.delaySum += pacerPace.delay;
        pacerPrototype.frames++;
    }
    LightLock_Unlock(&pacerPace.lock);
}

static const char pacerMeasureFormat[] __attribute__((section(".rodata.pacer_in_text"))) =
    "[pacer_measure] rule=%u frames=%u shown=%u expected_shown=%u shown_low1=%.3f_fps delay_time=%.3f_ms window=%.3f_ms cushion_mean=%.3f_ms first=%u reactivate=%u\n";
void __real_CtrPacer_getFlipTotals(CtrFlipStats* out);
void __wrap_CtrPacer_getFlipTotals(CtrFlipStats* out);
PACER_LATE void __wrap_CtrPacer_getFlipTotals(CtrFlipStats* out) {
    __real_CtrPacer_getFlipTotals(out);
    LightLock_Lock(&pacerPace.lock);
    *out = pacerPace.flipTotals;
    uint64_t tick = svcGetSystemTick();
    if (!pacerPrototype.startPointer) pacerPrototype.startPointer = out;
    if (out == pacerPrototype.startPointer) {
        pacerPrototype.measuring = true;
        pacerPrototype.startTick = pacerPrototype.modeTick = tick;
        pacerPrototype.delayedTicks = pacerPrototype.delaySum = 0;
        pacerPrototype.frames = pacerPrototype.shown = 0;
        pacerPrototype.firstActivations = pacerPrototype.reactivations = 0;
        pacerPrototype.delayed = pacerPace.delay > 0;
        pacerPrototype.lastShown = pacerPace.lastShown;
        pacerPrototype.startFlips = out->flips;
        for (unsigned i = 0; i < 64; i++) pacerPrototype.longest[i] = 0;
        LightLock_Unlock(&pacerPace.lock);
        return;
    }
    if (!pacerPrototype.measuring) {
        LightLock_Unlock(&pacerPace.lock);
        return;
    }
    pacerModeAccount(tick);
    pacerPrototype.measuring = false;
    unsigned tail = pacerPrototype.shown / 100;
    if (tail == 0 && pacerPrototype.shown > 0) tail = 1;
    uint64_t sum = 0;
    if (tail <= 64) for (unsigned i = 0; i < tail; i++) sum += pacerPrototype.longest[i];
    double low = sum > 0 ? (double) tail * SYSCLOCK_ARM11 / ((double) sum * VBLANK_TICKS) : -1.0;
    double ms = 1000.0 / SYSCLOCK_ARM11;
    double delayedMs = pacerPrototype.delayedTicks * ms;
    double windowMs = (tick - pacerPrototype.startTick) * ms;
    double cushionMs = pacerPrototype.frames > 0 ? (double) pacerPrototype.delaySum * VBLANK_TICKS * ms / pacerPrototype.frames : 0.0;
    unsigned frames = pacerPrototype.frames, shown = pacerPrototype.shown;
    unsigned expected = out->flips - pacerPrototype.startFlips;
    unsigned first = pacerPrototype.firstActivations, again = pacerPrototype.reactivations;
    LightLock_Unlock(&pacerPace.lock);
    logInfo(pacerMeasureFormat, (unsigned) CTRPORT_PACER_R2R3A, frames, shown, expected, low,
            delayedMs, windowMs, cushionMs, first, again);
}
