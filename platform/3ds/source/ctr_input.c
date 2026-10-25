#include "ctr_input.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "ctr_watchdog.h"
#include "ctr_input_schedule.h"

#define INPUT_LATE __attribute__((noinline, section(".text.late.inputlate")))
#define INPUT_BSS __attribute__((section(".bss.late.inputlate")))
#define INPUT_RO __attribute__((section(".rodata.inputlate_in_text")))

#ifndef CTRPORT_INPUTLATE_PERIOD_MS
#define CTRPORT_INPUTLATE_PERIOD_MS 16
#endif
#ifndef CTRPORT_INPUTLATE_STALL_MS
#define CTRPORT_INPUTLATE_STALL_MS 0
#endif
#define INPUT_STACK_BYTES 4096

static const char inputText0[] INPUT_RO = "input sampler";
static const char inputText1[] INPUT_RO = "Input: system core time limit %lu%% (audio policy: at least 50%%)\n";
static const char inputText2[] INPUT_RO = "Input: HID ring sampler core %d priority 0x%lX, %u ms drains, %u bytes stack; forced stall %u ms\n";
static const char handoverText[] INPUT_RO = "Input: %s at %llu ms, backoff %lu ms, %lu history overruns, %lu retry misses; worker stays core %d\n";
static const char fallbackText[] INPUT_RO = "main takeover";
static const char retryText[] INPUT_RO = "worker return";
static const char rejectText[] INPUT_RO = "worker retry deferred";
#if CTRPORT_INPUTLATE_STALL_MS
static const char stallText[] INPUT_RO = "Input: diagnostic Core 1 sampler stall %u ms at %llu ms\n";
static const char stallEndText[] INPUT_RO = "Input: worker blackout end, %lu main drains, %lu new history overruns, %lu new takeovers\n";
#endif

typedef struct { u64 tick, previousTick; u32 index, buttons[8]; } PadRing;
static Thread worker INPUT_BSS;
static LightLock handoff INPUT_BSS;
static aptHookCookie inputCookie INPUT_BSS;
static bool inputHooked INPUT_BSS;
static bool registeredExit INPUT_BSS;
static u32 quit INPUT_BSS;
static u32 inputSuspended INPUT_BSS;
static u32 epoch INPUT_BSS;
// All mapping, key union, chord and edge state lives under this short lock.
// No HID reads, services, logs or engine calls execute while holding it. Each
// drain maps at most eight samples. Ownership changes and supplemental controls
// commit on this frame; a contended handoff cannot discard or defer either.
static CtrInputLatch latch INPUT_BSS;
static CtrPadCursor cursor INPUT_BSS;
static CtrInputStats published INPUT_BSS;
static u32 publishedEpoch INPUT_BSS;
static bool suppressWakeExtras INPUT_BSS;
static CtrInputEdges lastTaken INPUT_BSS;
static CtrInputStats lastStats INPUT_BSS;
static u32 handoffMisses INPUT_BSS;
static int initialCore INPUT_BSS;
static u64 startedTicks INPUT_BSS;
static CtrInputSchedule schedule INPUT_BSS;
// Dedicated registers-only sleep save area; sharing o2crash's would race its
// guard thread. This sampler's heap stack must also survive exceptional exit.
__attribute__((used, section(".bss.late.inputlate"))) u32 CtrInput_sleepSave[2];
void CtrInput_guardSleep(s64 ns);
__asm__(
    ".section .text.late.inputlate,\"ax\",%progbits\n.arm\n.align 2\n"
    ".global CtrInput_guardSleep\n.type CtrInput_guardSleep, %function\n"
    "CtrInput_guardSleep:\n"
    "svc 0x0a\nldr r12, =CtrInput_sleepSave\nstm r12, {r4,r5}\n"
    "mov r2, sp\nsvc 0x02\nldr r12, =CtrInput_sleepSave\nldm r12, {r4,r5}\n"
    "cmp r0, #0\nbxne lr\nand r3, r3, #3\ncmp r3, #3\nbxeq lr\n"
    "1: mvn r0, #0\nmvn r1, #0x80000000\nsvc 0x0a\nb 1b\n.ltorg\n"
    ".size CtrInput_guardSleep, . - CtrInput_guardSleep\n.previous\n");

static INPUT_LATE u64 readTick(unsigned word) {
    u32 hi, lo, again;
    do {
        hi = hidSharedMem[word + 1];
        lo = hidSharedMem[word];
        again = hidSharedMem[word + 1];
    } while (hi != again);
    return ((u64) hi << 32) | lo;
}

static INPUT_LATE bool readRing(PadRing* ring) {
    if (hidSharedMem == NULL) return false;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        ring->tick = readTick(0);
        ring->previousTick = readTick(2);
        ring->index = hidSharedMem[4];
        __dmb();
        for (unsigned i = 0; i < 8; ++i) ring->buttons[i] = hidSharedMem[10 + 4 * i];
        __dmb();
        if (ring->tick == readTick(0) && ring->previousTick == readTick(2) && ring->index == hidSharedMem[4])
            return ring->index < 8 && ring->tick != 0 && (s64) ring->tick >= 0;
    }
    return false;
}

// Called under handoff, by either the sampler or the frame consumer. A snapshot
// can become stale while waiting for the lock; never replay it backwards.
static INPUT_LATE bool drain(const PadRing* ring, u32 currentEpoch) {
    if (publishedEpoch != currentEpoch) {
        cursor.ready = false;
        latch.raw = latch.extras = 0;
        latch.edges = (CtrInputEdges) {0};
        latch.guard = (CtrChordGuard) {0};
        latch.lAllowed = latch.lPulse = latch.lCandidate = latch.lTap = false;
        suppressWakeExtras = true;
        publishedEpoch = currentEpoch;
    }
    if (cursor.ready && (ring->tick < cursor.tick ||
            (ring->tick == cursor.tick && ring->index < cursor.index))) return false;
    bool first = !cursor.ready;
    CtrPadAdvance advance = CtrPadCursor_advance(&cursor, ring->tick, ring->previousTick, ring->index);
    if (first) {
        if (currentEpoch != 0) latch.ignoredKeys |= CtrInputLate_keys(&latch.map, ring->buttons[ring->index]);
        advance.count = 1;
    }
    if (advance.overrun) published.overruns++;
    for (unsigned n = advance.count; n > 0; --n) {
        CtrInputLatch_sample(&latch, ring->buttons[(ring->index + 9 - n) & 7]);
        published.samples++;
    }
    return advance.overrun;
}

static INPUT_LATE void reportHandover(int event, u64 nowMs, const CtrInputSchedule* state, u32 overruns) {
    logInfo(handoverText, event == 1 ? retryText : event == 2 ? rejectText : fallbackText, (unsigned long long) nowMs,
            (unsigned long) (event == 1 ? 0 : state->nextMs - nowMs), (unsigned long) overruns,
            (unsigned long) state->retryMisses, initialCore);
}

static INPUT_LATE void publishSchedule(void) {
    published.mainTakeovers = schedule.takeovers;
    published.workerReturns = schedule.returns;
    published.retryMisses = schedule.retryMisses;
}

static INPUT_LATE void sampler(void* arg) {
    (void) arg;
    CtrWatchdog_nameThread(inputText0);
    u32 seenEpoch = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
    u64 lastDrain = 0;
#if CTRPORT_INPUTLATE_STALL_MS
    bool stalled = false;
#endif
    while (!__atomic_load_n(&quit, __ATOMIC_ACQUIRE)) {
        u64 begin = svcGetSystemTick();
        u32 currentEpoch = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
        bool changedEpoch = seenEpoch != currentEpoch;
        if (changedEpoch) {
            seenEpoch = currentEpoch;
            lastDrain = 0;
        }
        PadRing ring;
        bool sleeping = __atomic_load_n(&inputSuspended, __ATOMIC_ACQUIRE) != 0;
        bool valid = !sleeping && readRing(&ring);
        u64 nowMs = (begin - startedTicks) * 1000 / SYSCLOCK_ARM11;
#if CTRPORT_INPUTLATE_STALL_MS
        // Pause only background reads. Main must keep the existing frame path
        // alive throughout the blackout, including ownership detection. Wait
        // for game frames so this diagnostic cannot finish during loading.
        LightLock_Lock(&handoff);
        bool inject = CTRPORT_INPUTLATE_STALL_MS && !stalled && initialCore == 1 && published.mainDrains >= 120;
        u32 beforeMain = published.mainDrains, beforeOverruns = published.overruns, beforeTakeovers = published.mainTakeovers;
        LightLock_Unlock(&handoff);
        if (inject) {
            stalled = true;
            logInfo(stallText, CTRPORT_INPUTLATE_STALL_MS, (unsigned long long) nowMs);
            CtrInput_guardSleep(CTRPORT_INPUTLATE_STALL_MS * 1000000LL);
            LightLock_Lock(&handoff);
            u32 mainDrains = published.mainDrains - beforeMain, overruns = published.overruns - beforeOverruns;
            u32 takeovers = published.mainTakeovers - beforeTakeovers;
            LightLock_Unlock(&handoff);
            logInfo(stallEndText, (unsigned long) mainDrains, (unsigned long) overruns, (unsigned long) takeovers);
            continue;
        }
#endif
        int event = -1;
        CtrInputSchedule snapshot;
        LightLock_Lock(&handoff);
        if (sleeping || changedEpoch) {
            schedule.workerMs = nowMs;
            schedule.mainReads = schedule.retrying = false;
            schedule.delayMs = 1000;
            schedule.seenOverruns = published.overruns;
        }
        if (!sleeping && CtrInputSchedule_check(&schedule, nowMs, published.overruns)) event = 0;
        // A stale snapshot cannot acknowledge a hand-back or replay old edges.
        bool current = valid && (!cursor.ready || ring.tick > cursor.tick ||
                (ring.tick == cursor.tick && ring.index >= cursor.index));
        if (current && CtrInputSchedule_due(&schedule, nowMs)) {
            if (lastDrain != 0 && begin - lastDrain > published.maxGapTicks) published.maxGapTicks = begin - lastDrain;
            lastDrain = begin;
            published.drains++;
            drain(&ring, currentEpoch);
            if (CtrInputSchedule_check(&schedule, nowMs, published.overruns)) event = 0;
            else if (CtrInputSchedule_ack(&schedule, nowMs, true)) event = 1;
        } else if (!sleeping && CtrInputSchedule_due(&schedule, nowMs)) {
            ++published.retries;
            if (CtrInputSchedule_reject(&schedule, nowMs)) event = 2;
        }
        publishSchedule();
        snapshot = schedule;
        u32 overruns = published.overruns;
        LightLock_Unlock(&handoff);
        if (event >= 0) reportHandover(event, nowMs, &snapshot, overruns);
        u64 waitMs = snapshot.mainReads && snapshot.nextMs > nowMs ? snapshot.nextMs - nowMs : CTRPORT_INPUTLATE_PERIOD_MS;
        if (waitMs > 100) waitMs = 100; // stop/join remains bounded at 250 ms
        CtrInput_guardSleep(waitMs * 1000000LL);
    }
}

INPUT_LATE void CtrInput_requestStop(void) { __atomic_store_n(&quit, 1, __ATOMIC_RELEASE); }

static INPUT_LATE void onApt(APT_HookType hook, void* arg) {
    (void) arg;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) {
        __atomic_store_n(&inputSuspended, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&epoch, 1, __ATOMIC_ACQ_REL);
    } else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) {
        __atomic_add_fetch(&epoch, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&inputSuspended, 0, __ATOMIC_RELEASE);
    } else if (hook == APTHOOK_ONEXIT) CtrInput_requestStop();
}

INPUT_LATE bool CtrInput_start(bool isNew3ds, const CtrInputMap* map) {
    if (worker != NULL) return !__atomic_load_n(&quit, __ATOMIC_ACQUIRE);
    quit = inputSuspended = epoch = publishedEpoch = handoffMisses = 0;
    lastTaken = (CtrInputEdges) {0};
    latch = (CtrInputLatch) {.map = *map}; // loading/setup owns input until the first game frame
    cursor = (CtrPadCursor) {0};
    suppressWakeExtras = false;
    published = lastStats = (CtrInputStats) {0};
    LightLock_Init(&handoff);
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority = priority > 0x1B ? priority - 3 : 0x18; // above audio/read-ahead
    initialCore = isNew3ds ? 2 : 1;
    if (initialCore == 1) {
        u32 limit = 0;
        Result rc = APT_GetAppCpuTimeLimit(&limit);
        if (R_SUCCEEDED(rc) && limit < 50) rc = APT_SetAppCpuTimeLimit(50);
        if (R_FAILED(rc) || R_FAILED(APT_GetAppCpuTimeLimit(&limit)) || limit < 50) initialCore = -1;
        logInfo(inputText1, (unsigned long) limit);
    }
    published.core = initialCore;
    startedTicks = svcGetSystemTick();
    schedule = (CtrInputSchedule) {.recovery = initialCore == 1 && CTRPORT_INPUTLATE_PERIOD_MS != 0, .delayMs = 1000};
    if (CTRPORT_INPUTLATE_PERIOD_MS != 0 && initialCore >= 1)
        worker = threadCreate(sampler, NULL, INPUT_STACK_BYTES, priority, initialCore, false);
    // Allocation or time-cap failure leaves main's frame reader in charge.
    // Never create a background worker on the game's CPU-bound Core 0.
    if (worker == NULL) { schedule.recovery = false; published.core = -1; }
    aptHook(&inputCookie, onApt, NULL);
    inputHooked = true;
    if (!registeredExit) { atexit(CtrInput_stop); registeredExit = true; }
    logInfo(inputText2,
            published.core, (unsigned long) priority, worker ? CTRPORT_INPUTLATE_PERIOD_MS : 0, INPUT_STACK_BYTES,
            CTRPORT_INPUTLATE_STALL_MS);
    return true;
}

INPUT_LATE void CtrInput_stop(void) {
    if (worker == NULL && !inputHooked) return;
    CtrInput_requestStop();
    if (worker != NULL) {
        bool self = threadGetCurrent() == worker;
        Result result = self ? -1 : threadJoin(worker, 250000000ULL);
        if (R_FAILED(result)) {
            return; // don't free a live thread, or hang exceptional exit forever
        }
        threadFree(worker);
        worker = NULL;
    }
    published.elapsedTicks = svcGetSystemTick() - startedTicks;
    if (inputHooked) { aptUnhook(&inputCookie); inputHooked = false; }
}

INPUT_LATE void CtrInput_getStats(CtrInputStats* stats) {
    // Exit diagnostics must not wait on a possibly faulted producer. After a
    // successful join there are no writers; otherwise take a best-effort lock.
    if (worker != NULL && LightLock_TryLock(&handoff) != 0) {
        *stats = (CtrInputStats) {.core = -1};
        return;
    }
    *stats = published;
    if (worker != NULL) LightLock_Unlock(&handoff);
}

INPUT_LATE CtrInputEdges CtrInput_take(const CtrInputMap* map, u32 held, u32 down,
                          bool enabled, bool suppressHeld, CtrInputStats* stats) {
    PadRing ring;
    u32 currentEpoch = __atomic_load_n(&epoch, __ATOMIC_ACQUIRE);
    bool sleeping = __atomic_load_n(&inputSuspended, __ATOMIC_ACQUIRE) != 0;
    bool valid = !sleeping && readRing(&ring);
    u64 nowMs = schedule.recovery ? (svcGetSystemTick() - startedTicks) * 1000 / SYSCLOCK_ARM11 : 0;
    bool takeover = false;
    CtrInputSchedule snapshot;
    u32 overruns;
    LightLock_Lock(&handoff);
    {
        bool reset = suppressHeld || currentEpoch != publishedEpoch;
        if (valid) { drain(&ring, currentEpoch); ++published.mainDrains; }
        else if (!sleeping) CtrInputLatch_sample(&latch, held);
        // Frame reads never wait for worker health, retry deadlines or an ack.
        // Both readers share one cursor, so a hand-back cannot replay a press.
        if (!sleeping && !reset) takeover = CtrInputSchedule_check(&schedule, nowMs, published.overruns);
        if (takeover) { publishSchedule(); snapshot = schedule; }
        overruns = published.overruns;
        // The worker can rebase first after wake, before this frame sees the new
        // epoch. Supplemental controls still need the same release-to-arm rule.
        if (suppressWakeExtras && !sleeping) {
            reset = true;
            suppressWakeExtras = false;
        }
        if (reset) CtrInputLatch_rebase(&latch, map, held);
        lastTaken = CtrInputLatch_frame(&latch, map, held, down,
                enabled && !sleeping && publishedEpoch == currentEpoch, 0);
        if (stats != NULL) lastStats = published;
        LightLock_Unlock(&handoff);
    }
    if (takeover) reportHandover(false, nowMs, &snapshot, overruns);
    lastStats.handoffMisses = handoffMisses;
    if (stats != NULL) *stats = lastStats;
    return lastTaken;
}
