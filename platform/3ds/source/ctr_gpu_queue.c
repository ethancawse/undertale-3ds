// citro3d's frame and render target functions, from source/renderqueue.c of
// citro3d v1.7.1, altered so that the CPU can record a frame while the GPU
// still runs the one before (see ctr_gpu_queue.h). Being linked in, these take
// the place of libcitro3d's renderqueue.o, which defines nothing else.
//
// Changes from the original:
// - Frames alternate between two GX queues: citro3d's own and a second one.
// - CtrGpu_frameBeginOverlapped begins a frame without waiting for the GPU,
//   recording after the last frame's commands in the command buffer.
// - C3D_FrameEnd waits for the frame before, if it is still running, before
//   adding the screens' transfers and starting the queue.
// - The screens' transfers are held back in the queue until each screen's last
//   flip has been shown, and let through from the vblank callbacks (see
//   "Held-back transfers" below); the frame's draws start at once.
// - C3D_RenderTargetDelete no longer waits for the GPU: ctr_memory.c holds the
//   buffers' VRAM back until the GPU is done.
// - The queue's finish callback counts completions.
// - CtrGpu_frameEndBehind ends a frame that uses no screen without waiting for
//   the GPU: with a frame still running, it runs after it (fixed 2:1 frame
//   skip's unshown frames). A third queue then takes
//   the next frame.
//
// The original's notice:
//
//   Copyright (C) 2014-2018 fincs
//
//   This software is provided 'as-is', without any express or implied
//   warranty.  In no event will the authors be held liable for any
//   damages arising from the use of this software.
//
//   Permission is granted to anyone to use this software for any
//   purpose, including commercial applications, and to alter it and
//   redistribute it freely, subject to the following restrictions:
//
//   1. The origin of this software must not be misrepresented; you
//      must not claim that you wrote the original software. If you use
//      this software in a product, an acknowledgment in the product
//      documentation would be appreciated but is not required.
//   2. Altered source versions must be plainly marked as such, and
//      must not be misrepresented as being the original software.
//   3. This notice may not be removed or altered from any source
//      distribution.
#include "ctr_gpu_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <citro3d.h>

#include "common.h"
#include "ctr_frame_pacer.h"
#include "log.h"

// ===[ citro3d internals (its internal.h) ]===

// The start of citro3d's context, which is where its queue and command buffer are.
typedef struct {
    gxCmdQueue_s gxQueue;
    u32* cmdBuf;
    size_t cmdBufSize; // in words
} CitroContextHead;

extern uint8_t __C3D_Context[];
#define CITRO_CONTEXT ((CitroContextHead*) (void*) __C3D_Context)

bool C3Di_SplitFrame(u32** pBuf, u32* pSize);
void C3Di_RenderQueueInit(void);
void C3Di_RenderQueueExit(void);
void C3Di_RenderQueueWaitDone(void);
void C3Di_RenderQueueEnableVBlank(void);
void C3Di_RenderQueueDisableVBlank(void);

static inline bool addrIsVRAM(const void* addr) {
    u32 vaddr = (u32) addr;
    return vaddr >= OS_VRAM_VADDR && vaddr < OS_VRAM_VADDR + OS_VRAM_SIZE;
}

static inline vramAllocPos addrGetVRAMBank(const void* addr) {
    u32 vaddr = (u32) addr;
    return vaddr < OS_VRAM_VADDR + OS_VRAM_SIZE / 2 ? VRAM_ALLOC_A : VRAM_ALLOC_B;
}

// ===[ State ]===

static C3D_RenderTarget *firstTarget, *lastTarget;
static C3D_RenderTarget* linkedTarget[3];

static TickCounter cpuTime;

static bool inFrame, inSafeTransfer;
static bool needSwapTop, needSwapBot, isTopStereo;
static float framerate = 60.0f;
static float framerateCounter[2] = { 60.0f, 60.0f };
static u32 frameCounter[2];
static void (*frameEndCb)(void*);
static void* frameEndCbData;

// Frames alternate between these: citro3d's queue and a second one. Without
// the second, frames wait for each other as citro3d's do. The third is made
// when CtrGpu_frameEndBehind is first used: a frame begun while one frame is
// on the GPU and another handed over behind it takes it.
static gxCmdQueue_s secondQueue, thirdQueue;
static gxCmdQueue_s* queues[3];
static int queueIndex;
static bool overlapReady;
// A frame handed over behind the one on the GPU (CtrGpu_frameEndBehind); that
// one's finish starts it, and the GPU counts as busy until it is done too.
static gxCmdQueue_s* volatile pendingQueue;
// Words of the command buffer: where the last frame's commands ended, where
// this frame's began, and whether it may still go on at the start.
static u32 ringEnd;
static u32 frameStart;
static bool mayWrap;
static volatile u32 completions;
// Queue runs started, which hand the GPU what was queued.
static u32 queueRuns;
static uint64_t waitTicks, waitTotalTicks;


static void C3Di_RenderTargetDestroy(C3D_RenderTarget* target);

static inline gxCmdQueue_s* boundQueue(void) {
    return queues[queueIndex] != nullptr ? queues[queueIndex] : &CITRO_CONTEXT->gxQueue;
}

// ===[ Overlap ]===

// Starts a queue, noting that its finish callback is to come.
#ifdef CTRPORT_RENDER_TIMING
// What the GPU's time goes on, from when each GX command finishes: the time
// since the one before finished (or the queue started) is charged to it.
// GSP runs the commands one after another.
static volatile uint64_t opLastTick;
static volatile uint64_t opTicks[CTR_GPU_OP_KINDS];
static volatile uint32_t opCounts[CTR_GPU_OP_KINDS];

static volatile bool tracing;
static volatile uint32_t traceCount;
static CtrGpuTraceOp traceOps[CTR_GPU_TRACE_MAX];
// The commands of each queue run since CtrGpu_traceStart, as it starts.
static gxCmdEntry_s traceEntries[CTR_GPU_TRACE_MAX];
static uint32_t traceEntryCount;

static void onGpuOpDone(void* data) {
    uint32_t kind = (uint32_t) (uintptr_t) data;
    uint64_t now = svcGetSystemTick();
    opTicks[kind] += now - opLastTick;
    opCounts[kind]++;
    if (tracing && CTR_GPU_TRACE_MAX > traceCount) {
        traceOps[traceCount] = (CtrGpuTraceOp) { (uint8_t) kind, (uint32_t) (now - opLastTick) };
        traceCount++;
    }
    opLastTick = now;
}

void CtrGpu_traceStart(void) {
    traceCount = 0;
    traceEntryCount = 0;
    tracing = true;
}

uint32_t CtrGpu_traceTake(CtrGpuTraceOp* ops, gxCmdEntry_s* entries, uint32_t* entryCount) {
    tracing = false;
    uint32_t n = traceCount;
    memcpy(ops, traceOps, n * sizeof(CtrGpuTraceOp));
    memcpy(entries, traceEntries, traceEntryCount * sizeof(gxCmdEntry_s));
    *entryCount = traceEntryCount;
    return n;
}

static void setOpCallbacks(bool on) {
    gspSetEventCallback(GSPGPU_EVENT_P3D, on ? onGpuOpDone : nullptr, (void*) (uintptr_t) CTR_GPU_OP_DRAW, false);
    gspSetEventCallback(GSPGPU_EVENT_PSC0, on ? onGpuOpDone : nullptr, (void*) (uintptr_t) CTR_GPU_OP_FILL, false);
    gspSetEventCallback(GSPGPU_EVENT_PSC1, on ? onGpuOpDone : nullptr, (void*) (uintptr_t) CTR_GPU_OP_FILL, false);
    gspSetEventCallback(GSPGPU_EVENT_PPF, on ? onGpuOpDone : nullptr, (void*) (uintptr_t) CTR_GPU_OP_COPY, false);
}

void CtrGpu_takeOpStats(CtrGpuOpStats* out) {
    for (int i = 0; CTR_GPU_OP_KINDS > i; i++) {
        out->ms[i] = (float) ((double) opTicks[i] / CPU_TICKS_PER_MSEC);
        out->count[i] = opCounts[i];
        opTicks[i] = 0;
        opCounts[i] = 0;
    }
}
#endif

// ===[ Held-back transfers ]===
//
// A frame's screen transfers write the buffer each screen shows until its
// last flip takes effect on a vblank, so they can't run before then. citro3d
// started the frame's whole queue only after that (the frame pacer waited for
// the flips), and the GPU sat idle from finishing one frame to the next
// vblank whenever it was the slow part. Here the frame's draws start as soon
// as the frame ends, and its transfers wait in the queue: libctru hands the
// GPU a queue's entries up to numEntries only, so the transfers are kept
// past it and moved in, a screen at a time, once that screen has no flip
// pending in GSP or held by the frame pacer. The vblank callbacks (on the GSP
// event thread, which also runs the queue's interrupts, one at a time) and
// C3D_FrameEnd let them through; each screen's destination is read then,
// when its last flip has been asked for, as citro3d reads it.
//
// A frame is done when its transfers have run too: libctru marks the queue
// idle (gxCmdQueueWait returns) and calls the finish callback once the draws
// are done, and again after the transfers. frameBusy and doneEvent cover both.
typedef struct {
    gfxScreen_t screen;
    gfx3dSide_t side;
} HeldTransfer;

static LightLock gateLock;
static LightEvent doneEvent;
// Work handed to the GPU isn't done yet (its transfers included).
static volatile bool frameBusy;
// The queue the GPU runs, its transfers still held back (they follow its
// numEntries, in this order), and whether the GPU has run out of the rest.
static gxCmdQueue_s* runQueueNow;
static HeldTransfer held[3];
static u32 heldCount;
static bool drawsDone;
// The GPU's time on the frame so far, the time its work last (re)started,
// and the last finished frame's total, in ticks.
static uint64_t frameGpuTicks, segmentStart, lastGpuTicks;
// What the queue run last is: a frame with screen transfers, one without
// (an unshown frame), or one of citro3d's one-off transfers. And when it began.
typedef enum { RUN_SCREENS, RUN_NO_SCREENS, RUN_OTHER } RunKind;
static RunKind runKind;
static uint64_t runStart;
// The GPU's time on finished frames by kind (RUN_SCREENS, RUN_NO_SCREENS),
// since CtrGpu_takeFrameGpuStats.
static uint64_t kindTicks[2], kindMaxTicks[2];
static uint32_t kindFrames[2];
// Test hook (CtrGpu_setSimulatedGpu): a frame's draws count as done no sooner
// than simTicks[kind] after its queue began. A finish that comes earlier is
// put off (once per run) and made by simThread at simDue.
static uint64_t simTicks[2];
static bool simPutOff;
static gxCmdQueue_s* volatile simQueue;
static volatile uint64_t simDue;
static LightEvent simEvent;
static Thread simThread;
// When the GPU last ran out of work.
static uint64_t idleStart;
static CtrGpuPaceStats paceStats;
// Set by CtrGpu_setGateEnabled(false), the tear check's negative control.
static bool gateOff;
// For hang reports: where the GSP event thread is in the callbacks below, the
// step releaseReady is on (either thread), and the vblank callbacks run.
static const char* volatile gspAt = "not in a callback";
static const char* volatile gateStep = "none";
static volatile uint32_t vblankCallbacks[2];

// The last gate events, for hang reports.
typedef struct {
    const char* what;
    void* from;
    gxCmdQueue_s* queue;
    u16 entries, sent, done;
    u8 held, busy, drawsDone, running;
    u32 vblank;
} GateEvent;
#define GATE_EVENTS 32
static GateEvent gateEvents[GATE_EVENTS];
static volatile u32 gateEventCount;
bool __real_gxCmdQueueWait(gxCmdQueue_s* queue, s64 timeout);

static void noteEvent(const char* what, gxCmdQueue_s* queue, void* from) {
    u32 i = __atomic_fetch_add(&gateEventCount, 1, __ATOMIC_RELAXED) % GATE_EVENTS;
    GateEvent* e = &gateEvents[i];
    e->what = what;
    e->from = from;
    e->queue = queue;
    e->entries = queue != nullptr ? queue->numEntries : 0;
    e->sent = queue != nullptr ? queue->curEntry : 0;
    e->done = queue != nullptr ? queue->lastEntry : 0;
    e->held = (u8) heldCount;
    e->busy = frameBusy;
    e->drawsDone = drawsDone;
    e->running = !__real_gxCmdQueueWait(queue, 0);
    e->vblank = vblankCallbacks[0];
}
// With gateLock held: done with the queue run last, transfers and flips included.
static void finishFrame(gxCmdQueue_s* queue);

// A screen's buffers and which one it shows, read from the LCD's registers
// (physical addresses). GSP sets them on the vblank that takes a flip.
typedef struct {
    u32 a, b, select;
} ScreenRegs;

static bool readScreenRegs(gfxScreen_t screen, gfx3dSide_t side, ScreenRegs* out) {
    u32 base = screen == GFX_TOP ? 0x400400 : 0x400500;
    u32 regs[5]; // 0x68: left buffer A, 0x6C: left B, 0x70, 0x74, 0x78: which one shows (bit 0)
    if (R_FAILED(GSPGPU_ReadHWRegs(base + 0x68, regs, sizeof(regs)))) return false;
    out->a = regs[0];
    out->b = regs[1];
    out->select = regs[4];
    if (side == GFX_RIGHT) {
        u32 right[2]; // 0x94: right buffer A, 0x98: right B
        if (R_FAILED(GSPGPU_ReadHWRegs(base + 0x94, right, sizeof(right)))) return false;
        out->a = right[0];
        out->b = right[1];
    }
    return true;
}

// The first few transfers into a shown buffer since CtrGpu_logShownWrites.
typedef struct {
    gfxScreen_t screen;
    gfx3dSide_t side;
    bool atFrameEnd;
    u32 vblank, to;
    ScreenRegs regs;
} ShownWrite;
#define SHOWN_WRITES_KEPT 4
static ShownWrite shownWrites[SHOWN_WRITES_KEPT];
static u32 shownWritesKept;
// Each screen's last transfer destination (physical): top left, top right, bottom.
static u32 lastTo[3];

// With gateLock held: moves the held-back transfers of every screen ready for
// them in front of the rest and lets the GPU have them, restarting the queue
// if it ran out.
static void releaseReady(bool atFrameEnd) {
    if (heldCount == 0) return;
    gxCmdQueue_s* queue = runQueueNow;
    gateStep = "asking the pacer which screens are ready";
    bool ready[2] = { gateOff || CtrPacer_screenReady(GFX_TOP), gateOff || CtrPacer_screenReady(GFX_BOTTOM) };
    gateStep = "none";
    // Only a held transfer's own screen counts: the GPU is restarted below
    // only when something was let through, and with nothing let through, a
    // restart would find the queue empty and the next release (draws done
    // no longer set) would wait for a restart that never came.
    bool any = false;
    for (u32 i = 0; heldCount > i; i++) any = any || ready[held[i].screen == GFX_TOP ? 0 : 1];
    if (!any) return;
    u16 first = queue->numEntries;
    gxCmdEntry_s entries[3];
    HeldTransfer was[3];
    memcpy(entries, &queue->entries[first], heldCount * sizeof(gxCmdEntry_s));
    memcpy(was, held, heldCount * sizeof(HeldTransfer));
    u32 count = heldCount, released = 0, kept = 0;
    for (int pass = 0; 2 > pass; pass++) {
        for (u32 i = 0; count > i; i++) {
            bool go = ready[was[i].screen == GFX_TOP ? 0 : 1];
            if (go != (pass == 0)) continue;
            gxCmdEntry_s* e = &queue->entries[first + released + kept];
            *e = entries[i];
            if (go) {
                // GX_DisplayTransfer(in, out, ...): the screen's back buffer now.
                gateStep = "reading the back buffer from the pacer";
                u32* out = (u32*) CtrPacer_backFramebuffer(was[i].screen, was[i].side);
                e->data[2] = (u32) out;
                gateStep = "reading the LCD registers";
                ScreenRegs regs;
                u32 to = osConvertVirtToPhys(out);
                u32 slot = was[i].screen == GFX_TOP ? (was[i].side == GFX_RIGHT ? 1 : 0) : 2;
                bool sameAsLast = to == lastTo[slot];
                lastTo[slot] = to;
                if (readScreenRegs(was[i].screen, was[i].side, &regs) && to == ((regs.select & 1) != 0 ? regs.b : regs.a)) {
                    if (sameAsLast) {
                        paceStats.singleBufferWrites++;
                    } else {
                        paceStats.shownWrites++;
                        if (SHOWN_WRITES_KEPT > shownWritesKept) {
                            shownWrites[shownWritesKept++] = (ShownWrite) { was[i].screen, was[i].side, atFrameEnd, vblankCallbacks[0], to, regs };
                        }
                    }
                }
                gateStep = "none";
                released++;
            } else {
                held[kept++] = was[i];
            }
        }
    }
    heldCount = kept;
    queue->numEntries = (u16) (first + released);
    noteEvent(atFrameEnd ? "released at the frame end" : "released at a vblank", queue, nullptr);
    if (!atFrameEnd) paceStats.transfersWaited += released;
#ifdef CTRPORT_RENDER_TIMING
    for (u32 i = first; tracing && !atFrameEnd && queue->numEntries > i && CTR_GPU_TRACE_MAX > traceEntryCount; i++) traceEntries[traceEntryCount++] = queue->entries[i];
#endif
    if (drawsDone) {
        drawsDone = false;
        uint64_t now = svcGetSystemTick();
        paceStats.flipIdleTicks += now - idleStart;
        segmentStart = now;
#ifdef CTRPORT_RENDER_TIMING
        opLastTick = now;
#endif
        gateStep = "restarting the queue";
        gxCmdQueueRun(queue);
        noteEvent("restarted for the transfers", queue, nullptr);
        gateStep = "none";
    }
}

// With gateLock held: hands the GPU a queue. It counts as busy until
// finishFrame, its held-back transfers included.
static void startQueue(gxCmdQueue_s* queue, RunKind kind) {
    queueRuns++;
    runKind = kind;
    simPutOff = false;
    uint64_t now = svcGetSystemTick();
    if (idleStart != 0) paceStats.mainIdleTicks += now - idleStart;
    idleStart = 0;
    runQueueNow = queue;
    frameGpuTicks = 0;
    segmentStart = runStart = now;
    bool any = queue->lastEntry < queue->numEntries;
#ifdef CTRPORT_RENDER_TIMING
    opLastTick = now;
    for (u32 i = queue->curEntry; tracing && queue->numEntries > i && CTR_GPU_TRACE_MAX > traceEntryCount; i++) traceEntries[traceEntryCount++] = queue->entries[i];
#endif
    if (any || heldCount > 0) {
        frameBusy = true;
        LightEvent_Clear(&doneEvent);
    }
    // Run even with nothing to hand over yet: the queue becomes libctru's
    // current one, so the next frame's (the other queue) isn't started as
    // its commands are added.
    noteEvent("run: before", queue, __builtin_return_address(0));
    gxCmdQueueRun(queue);
    noteEvent("run: after", queue, nullptr);
    if (!any) {
        idleStart = now;
        if (heldCount > 0) drawsDone = true;
    }
}

// Hands the GPU a queue (C3D_FrameEnd, citro3d's one-off transfers).
static void runQueue(gxCmdQueue_s* queue, RunKind kind) {
    LightLock_Lock(&gateLock);
    startQueue(queue, kind);
    LightLock_Unlock(&gateLock);
}

// How long held-back transfers may wait, from the GPU finishing the rest,
// before CtrGpu_waitIdle drops them: no vblank came to let them through (the
// vblank callbacks are off while the app is suspended). The screens then keep
// what they show.
#define HELD_WAIT_LIMIT_TICKS ((uint64_t) SYSCLOCK_ARM11 / 4)
#define WAIT_STEP_NS (20 * 1000 * 1000)

static bool dropLogged;

static void dropHeldTransfers(void) {
    LightLock_Lock(&gateLock);
    if (frameBusy && drawsDone && heldCount > 0 && svcGetSystemTick() - idleStart > HELD_WAIT_LIMIT_TICKS) {
        paceStats.droppedTransfers += heldCount;
        for (u32 i = 0; heldCount > i; i++) {
            if (held[i].screen == GFX_TOP) needSwapTop = false;
            else needSwapBot = false;
        }
        heldCount = 0;
        drawsDone = false;
        noteEvent("dropped held transfers", runQueueNow, nullptr);
        paceStats.flipIdleTicks += svcGetSystemTick() - idleStart;
        finishFrame(runQueueNow);
        if (!dropLogged) {
            dropLogged = true;
            logWarn("GPU: no vblank let a frame's screen transfers through for 250 ms; dropped them\n");
        }
    }
    LightLock_Unlock(&gateLock);
}

void CtrGpu_waitIdle(void) {
    if (!frameBusy) return;
    noteEvent("waiting for the GPU", runQueueNow, __builtin_return_address(0));
    uint64_t start = svcGetSystemTick();
    while (frameBusy) {
        LightEvent_WaitTimeout(&doneEvent, WAIT_STEP_NS);
        if (frameBusy) dropHeldTransfers();
    }
    uint64_t waited = svcGetSystemTick() - start;
    waitTicks += waited;
    waitTotalTicks += waited;
}

bool CtrGpu_busy(void) {
    return frameBusy;
}

void CtrGpu_takePaceStats(CtrGpuPaceStats* out) {
    LightLock_Lock(&gateLock);
    *out = paceStats;
    memset(&paceStats, 0, sizeof(paceStats));
    LightLock_Unlock(&gateLock);
}

void CtrGpu_logShownWrites(void) {
    ShownWrite kept[SHOWN_WRITES_KEPT];
    LightLock_Lock(&gateLock);
    u32 count = shownWritesKept;
    memcpy(kept, shownWrites, sizeof(kept));
    shownWritesKept = 0;
    LightLock_Unlock(&gateLock);
    for (u32 i = 0; count > i; i++) {
        const ShownWrite* w = &kept[i];
        logInfo("GPU: transfer into the shown buffer: %s%s, let through at %s (vblank %lu), to %08lx; LCD buffers %08lx/%08lx, select %08lx\n",
                w->screen == GFX_TOP ? "top" : "bottom", w->side == GFX_RIGHT ? " right" : "", w->atFrameEnd ? "the frame end" : "a vblank",
                (unsigned long) w->vblank, (unsigned long) w->to, (unsigned long) w->regs.a, (unsigned long) w->regs.b,
                (unsigned long) w->regs.select);
    }
}

void CtrGpu_setGateEnabled(bool enabled) {
    LightLock_Lock(&gateLock);
    gateOff = !enabled;
    LightLock_Unlock(&gateLock);
}

void CtrGpu_describeStall(char* out, size_t size) {
    gxCmdQueue_s* queue = runQueueNow;
    u32 count = heldCount;
    char heldText[32] = "";
    for (u32 i = 0; count > i && 3 > i; i++) {
        size_t len = strlen(heldText);
        snprintf(heldText + len, sizeof(heldText) - len, "%s%s", i > 0 ? "," : "",
                 held[i].screen == GFX_TOP ? (held[i].side == GFX_RIGHT ? "top right" : "top") : "bottom");
    }
    snprintf(out, size,
             "gpu: busy %d, draws done %d, %lu transfers held (%s), gate %s, gate lock word %ld; queue %p entries %u sent %u done %u; "
             "flips pending in GSP: top %d bottom %d; vblank callbacks %lu/%lu; GSP event thread at \"%s\", gate step \"%s\"\n",
             (int) frameBusy, (int) drawsDone, (unsigned long) count, heldText, gateOff ? "off" : "on", (long) gateLock, (void*) queue,
             queue != nullptr ? (unsigned) queue->numEntries : 0u, queue != nullptr ? (unsigned) queue->curEntry : 0u,
             queue != nullptr ? (unsigned) queue->lastEntry : 0u, (int) gspIsPresentPending(GFX_TOP), (int) gspIsPresentPending(GFX_BOTTOM),
             (unsigned long) vblankCallbacks[0], (unsigned long) vblankCallbacks[1], gspAt, gateStep);
}

void CtrGpu_describeEvents(char* out, size_t size) {
    size_t len = 0;
    out[0] = 0;
    u32 count = gateEventCount;
    u32 first = count > GATE_EVENTS ? count - GATE_EVENTS : 0;
    for (u32 n = first; count > n && size > len + 1; n++) {
        const GateEvent* e = &gateEvents[n % GATE_EVENTS];
        char q = e->queue == nullptr ? '-' : e->queue == queues[0] ? 'A' : e->queue == queues[1] ? 'B' : '?';
        int w = snprintf(out + len, size - len, "  #%lu v%lu %s: queue %c %u/%u/%u, held %u, busy %u, draws done %u, running %u, from %p\n",
                         (unsigned long) n, (unsigned long) e->vblank, e->what != nullptr ? e->what : "?", q, (unsigned) e->entries,
                         (unsigned) e->sent, (unsigned) e->done, (unsigned) e->held, (unsigned) e->busy, (unsigned) e->drawsDone,
                         (unsigned) e->running, e->from);
        if (0 > w) break;
        len += (size_t) w;
    }
}

uint32_t CtrGpu_completions(void) {
    return completions;
}

uint32_t CtrGpu_queueCapacity(void) {
    gxCmdQueue_s* queue = boundQueue();
    return secondQueue.entries != nullptr && secondQueue.maxEntries < queue->maxEntries ? secondQueue.maxEntries : queue->maxEntries;
}

CtrGpuQueued CtrGpu_lastQueued(void) {
    gxCmdQueue_s* queue = boundQueue();
    return (CtrGpuQueued) { queue, queueRuns, (uint16_t) (queue->numEntries - 1) };
}

bool CtrGpu_cancelCopy(CtrGpuQueued queued, const void* from, const void* to) {
    gxCmdQueue_s* queue = boundQueue();
    if (!inFrame || queued.queue != queue || queued.runs != queueRuns || queued.index >= queue->numEntries) return false;
    // Nothing is handed to the GPU before the queue runs.
    u32* cmd = queue->entries[queued.index].data;
    if ((cmd[0] & 0xFF) != 0x04 || cmd[1] != (u32) from || cmd[2] != (u32) to) return false; // GX_TextureCopy(from, to)
    cmd[1] = cmd[2];
    cmd[3] = 16;
    cmd[4] = cmd[5] = GX_BUFFER_DIM(1, 0);
    return true;
}

bool CtrGpu_queuedCopyInto(const void* start, uint32_t size) {
    if (!inFrame) return false;
    gxCmdQueue_s* queue = boundQueue();
    u32 lo = (u32) start, hi = (u32) start + size;
    for (u32 i = queue->curEntry; queue->numEntries > i; i++) {
        u32* cmd = queue->entries[i].data;
        if ((cmd[0] & 0xFF) != 0x04 || cmd[1] == cmd[2]) continue; // GX_TextureCopy(from, to), not a cancelled one
        if (cmd[2] >= lo && hi > cmd[2]) return true;
    }
    return false;
}

// Whether the GPU runs the queue, or has it waiting behind the one it runs.
static bool queueBusy(gxCmdQueue_s* queue) {
    LightLock_Lock(&gateLock);
    bool busy = frameBusy && (queue == runQueueNow || queue == pendingQueue);
    LightLock_Unlock(&gateLock);
    return busy;
}

uint32_t CtrGpu_frameCommands(uint32_t* fills, uint32_t* copies) {
    *fills = *copies = 0;
    if (!inFrame) return 0;
    gxCmdQueue_s* queue = boundQueue();
    for (u32 i = 0; queue->numEntries > i; i++) {
        u32 id = queue->entries[i].data[0] & 0xFF;
        if (id == 0x02) (*fills)++;                    // GX_MemoryFill
        else if (id == 0x03 || id == 0x04) (*copies)++; // GX_DisplayTransfer, GX_TextureCopy
    }
    return queue->numEntries;
}

uint32_t CtrGpu_completionDue(void) {
    // The pending frame before the count: one started in between is counted
    // twice (memory is held a frame longer), never missed.
    bool pending = __atomic_load_n(&pendingQueue, __ATOMIC_ACQUIRE) != nullptr;
    uint32_t done = __atomic_load_n(&completions, __ATOMIC_ACQUIRE);
    return done + (pending ? 2u : 1u);
}

void CtrGpu_takeFrameGpuStats(CtrGpuFrameStats* out) {
    LightLock_Lock(&gateLock);
    for (int k = 0; 2 > k; k++) {
        out->frames[k] = kindFrames[k];
        out->avgMs[k] = kindFrames[k] > 0 ? (float) ((double) kindTicks[k] / kindFrames[k] / CPU_TICKS_PER_MSEC) : 0.0f;
        out->maxMs[k] = (float) ((double) kindMaxTicks[k] / CPU_TICKS_PER_MSEC);
        kindTicks[k] = kindMaxTicks[k] = 0;
        kindFrames[k] = 0;
    }
    LightLock_Unlock(&gateLock);
}

uint64_t CtrGpu_waitTotalTicks(void) {
    return waitTotalTicks;
}

uint64_t CtrGpu_takeWaitTicks(void) {
    uint64_t ticks = waitTicks;
    waitTicks = 0;
    return ticks;
}

bool CtrGpu_frameBeginOverlapped(uint32_t minWords) {
    if (inFrame) return false;
    if (!overlapReady) return C3D_FrameBegin(0);
    CitroContextHead* ctx = CITRO_CONTEXT;
    // The other queue's frame was done before the last frame went to the GPU
    // (C3D_FrameEnd waits for it), so it is idle. After CtrGpu_frameEndBehind
    // it may still run, with the last frame waiting behind it: the third queue
    // is idle then.
    // Without the third queue (no frame ever ended behind), exactly the two-queue alternation as before.
    int next = queueIndex == 1 ? 0 : 1;
    if (queues[2] != nullptr && queueBusy(queues[next])) next = 3 - queueIndex - next;
    queueIndex = next;
    gxCmdQueue_s* queue = queues[queueIndex];
    noteEvent("frame begins (overlapped): clear", queue, nullptr);
    gxCmdQueueClear(queue);
    GX_BindQueue(queue);
    u32 start = ringEnd;
    if (start + minWords > ctx->cmdBufSize) {
        CtrGpu_waitIdle();
        start = 0;
    }
    inFrame = true;
    osTickCounterStart(&cpuTime);
    GPUCMD_SetBuffer(ctx->cmdBuf + start, ctx->cmdBufSize - start, 0);
    frameStart = start;
    mayWrap = start > 0;
    return true;
}

uint32_t CtrGpu_frameStartWord(void) {
    return frameStart;
}

void CtrGpu_commandBuffer(uint32_t** words, uint32_t* bytes) {
    *words = CITRO_CONTEXT->cmdBuf;
    *bytes = (uint32_t) CITRO_CONTEXT->cmdBufSize * 4u;
}

bool CtrGpu_canWrapCommandBuffer(uint32_t minWords) {
    return inFrame && mayWrap && frameStart >= minWords;
}

void CtrGpu_wrapCommandBuffer(void) {
    if (!CtrGpu_canWrapCommandBuffer(0) || gpuCmdBufOffset != 0) svcBreak(USERBREAK_PANIC);
    // The start of the buffer holds the frame before's commands until it is done.
    CtrGpu_waitIdle();
    mayWrap = false;
    GPUCMD_SetBuffer(CITRO_CONTEXT->cmdBuf, frameStart, 0);
}

static void onQueueFinish(gxCmdQueue_s* queue);

// The third queue, made the first time a frame waits behind another.
static bool makeThirdQueue(void) {
    if (queues[2] != nullptr) return true;
    if (!overlapReady) return false;
    thirdQueue.maxEntries = secondQueue.maxEntries;
    thirdQueue.entries = (gxCmdEntry_s*) malloc(thirdQueue.maxEntries * sizeof(gxCmdEntry_s));
    if (thirdQueue.entries == nullptr) return false;
    gxCmdQueueClear(&thirdQueue);
    gxCmdQueueSetCallback(&thirdQueue, onQueueFinish, nullptr);
    queues[2] = &thirdQueue;
    return true;
}

bool CtrGpu_frameEndBehind(void) {
    CitroContextHead* ctx = CITRO_CONTEXT;
    if (!inFrame || !overlapReady) return false;
    // A screen's transfer has to wait for the frame before's flip, and the flip for the frame.
    for (int i = 0; 3 > i; i++) {
        if (linkedTarget[i] != nullptr && linkedTarget[i]->used) return false;
    }
    if (!makeThirdQueue()) return false;
    // As C3D_FrameEnd, up to its wait for the frame before.
    if (frameEndCb) frameEndCb(frameEndCbData);
    C3D_FrameSplit(0);
    ringEnd = gpuCmdBuf != nullptr ? (u32) (gpuCmdBuf - ctx->cmdBuf) : 0;
    GPUCMD_SetBuffer(nullptr, 0, 0);
    osTickCounterUpdate(&cpuTime);
    inFrame = false;
    extern u32 __ctru_linear_heap;
    extern u32 __ctru_linear_heap_size;
    GSPGPU_FlushDataCache((void*) __ctru_linear_heap, __ctru_linear_heap_size);
    gxCmdQueue_s* queue = boundQueue();
    // Even with nothing recorded, the queue is run (now or behind the GPU's) as C3D_FrameEnd runs it: it becomes
    // libctru's current one, so the next frame's commands aren't started as they are added to the queue before.
    // One frame waits behind the GPU's at most.
    if (pendingQueue != nullptr) CtrGpu_waitIdle();
    LightLock_Lock(&gateLock);
    if (frameBusy) {
        __atomic_store_n(&pendingQueue, queue, __ATOMIC_RELEASE);
        noteEvent("frame end: behind the frame on the GPU", queue, nullptr);
    } else {
        startQueue(queue, RUN_NO_SCREENS);
    }
    LightLock_Unlock(&gateLock);
    return true;
}

// ===[ citro3d's renderqueue.c ]===

static bool framerateLimit(int id) {
    framerateCounter[id] -= framerate;
    if (framerateCounter[id] <= 0.0f) {
        framerateCounter[id] += 60.0f;
        return true;
    }
    return false;
}

// Each screen's flip takes effect on its own vblank (VBlank0 the top,
// VBlank1 the bottom), so either can let held-back transfers through. The
// top's also lets the frame pacer flip what it held for that vblank, first,
// so that a transfer never goes ahead of the flip it waits for.
static void onVBlank0(void* unused) {
    (void) unused;
    vblankCallbacks[0]++;
    if (framerateLimit(0)) frameCounter[0]++;
    gspAt = "VBlank0: letting the pacer flip";
    CtrPacer_onVBlank();
    gspAt = "VBlank0: taking the gate lock";
    LightLock_Lock(&gateLock);
    gspAt = "VBlank0: letting transfers through";
    releaseReady(false);
    LightLock_Unlock(&gateLock);
    gspAt = "not in a callback";
}

static void onVBlank1(void* unused) {
    (void) unused;
    vblankCallbacks[1]++;
    if (framerateLimit(1)) frameCounter[1]++;
    gspAt = "VBlank1: taking the gate lock";
    LightLock_Lock(&gateLock);
    gspAt = "VBlank1: letting transfers through";
    releaseReady(false);
    LightLock_Unlock(&gateLock);
    gspAt = "not in a callback";
}

static void onQueueFinishFlips(gxCmdQueue_s* queue);

// With gateLock held: the queue's work is all done, transfers included. A
// frame handed over behind it (CtrGpu_frameEndBehind) starts now, and the GPU
// stays busy.
static void finishFrame(gxCmdQueue_s* queue) {
    noteEvent("finished", queue, nullptr);
    uint64_t now = svcGetSystemTick();
    frameGpuTicks += now - segmentStart;
    lastGpuTicks = frameGpuTicks;
    if (RUN_OTHER != runKind) {
        kindTicks[runKind] += frameGpuTicks;
        if (frameGpuTicks > kindMaxTicks[runKind]) kindMaxTicks[runKind] = frameGpuTicks;
        kindFrames[runKind]++;
    }
    // The count before the pending frame goes (CtrGpu_completionDue reads them the other way round).
    __atomic_store_n(&completions, completions + 1, __ATOMIC_RELEASE);
    onQueueFinishFlips(queue);
    gxCmdQueue_s* next = pendingQueue;
    if (next != nullptr) {
        __atomic_store_n(&pendingQueue, nullptr, __ATOMIC_RELEASE);
        noteEvent("starting the frame handed over behind it", next, nullptr);
        // An empty one only becomes libctru's current queue; the GPU is idle.
        bool any = next->lastEntry < next->numEntries;
        startQueue(next, RUN_NO_SCREENS);
        if (any) return;
    }
    idleStart = now;
    frameBusy = false;
    LightEvent_Signal(&doneEvent);
}

static void onQueueFinishLocked(gxCmdQueue_s* queue);

// With gateLock held, from onQueueFinish: puts the finish of the run's draws off
// to the made-up GPU time, if it came sooner (test hook, CtrGpu_setSimulatedGpu).
static bool simPutOffFinish(gxCmdQueue_s* queue) {
    if (RUN_OTHER == runKind || simTicks[runKind] == 0 || simPutOff || queue != runQueueNow) return false;
    uint64_t due = runStart + simTicks[runKind];
    if (svcGetSystemTick() >= due) return false;
    simPutOff = true;
    simQueue = queue;
    simDue = due;
    LightEvent_Signal(&simEvent);
    return true;
}

static void simThreadMain(void* unused) {
    (void) unused;
    for (;;) {
        LightEvent_Wait(&simEvent);
        int64_t left = (int64_t) (simDue - svcGetSystemTick());
        if (left > 0) svcSleepThread(left * 1000LL / (int64_t) (SYSCLOCK_ARM11 / 1000000));
        LightLock_Lock(&gateLock);
        onQueueFinishLocked(simQueue);
        LightLock_Unlock(&gateLock);
    }
}

void CtrGpu_setSimulatedGpu(float screensMs, float noScreensMs) {
    if (simThread == nullptr) {
        LightEvent_Init(&simEvent, RESET_ONESHOT);
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        simThread = threadCreate(simThreadMain, nullptr, 4096, priority > 0x18 ? priority - 1 : priority, -2, true);
        if (simThread == nullptr) return;
    }
    LightLock_Lock(&gateLock);
    simTicks[RUN_SCREENS] = (uint64_t) ((double) screensMs * CPU_TICKS_PER_MSEC);
    simTicks[RUN_NO_SCREENS] = (uint64_t) ((double) noScreensMs * CPU_TICKS_PER_MSEC);
    LightLock_Unlock(&gateLock);
}

// libctru calls this (on the GSP event thread) when the GPU has run all of
// the queue's entries up to its numEntries: with transfers still held back,
// only the draws are done.
static void onQueueFinish(gxCmdQueue_s* queue) {
    gspAt = "queue finished: taking the gate lock";
    LightLock_Lock(&gateLock);
    gspAt = "queue finished";
    if (!simPutOffFinish(queue)) onQueueFinishLocked(queue);
    LightLock_Unlock(&gateLock);
    gspAt = "not in a callback";
}

static void onQueueFinishLocked(gxCmdQueue_s* queue) {
    noteEvent(queue == runQueueNow ? "finish callback" : "finish callback (not the run queue)", queue, nullptr);
    if (heldCount > 0 && queue == runQueueNow) {
        uint64_t now = svcGetSystemTick();
        frameGpuTicks += now - segmentStart;
        drawsDone = true;
        idleStart = now;
        paceStats.framesFlipIdle++;
        releaseReady(false);
    } else {
        gspAt = "queue finished: flipping";
        finishFrame(queue);
    }
}

static void onQueueFinishFlips(gxCmdQueue_s* queue) {
    if (inSafeTransfer) {
        inSafeTransfer = false;
        if (inFrame) {
            noteEvent("safe transfer done in a frame: stop", queue, nullptr);
            gxCmdQueueStop(queue);
            gxCmdQueueClear(queue);
        }
    } else {
        if (needSwapTop) {
            gfxScreenSwapBuffers(GFX_TOP, isTopStereo);
            needSwapTop = false;
        }
        if (needSwapBot) {
            gfxScreenSwapBuffers(GFX_BOTTOM, false);
            needSwapBot = false;
        }
    }
}

void C3D_FrameSync(void) {
    u32 cur[2];
    u32 start[2] = { frameCounter[0], frameCounter[1] };
    do {
        gspWaitForAnyEvent();
        cur[0] = frameCounter[0];
        cur[1] = frameCounter[1];
    } while (cur[0] == start[0] || cur[1] == start[1]);
}

u32 C3D_FrameCounter(int id) {
    return frameCounter[id];
}

static bool C3Di_WaitAndClearQueue(s64 timeout) {
    gxCmdQueue_s* queue = boundQueue();
    if (0 > timeout) {
        CtrGpu_waitIdle();
    } else if (frameBusy) {
        if (timeout > 0) LightEvent_WaitTimeout(&doneEvent, timeout);
        if (frameBusy) return false;
    }
    noteEvent("wait and clear: stop", queue, __builtin_return_address(0));
    gxCmdQueueStop(queue);
    gxCmdQueueClear(queue);
    return true;
}

void C3Di_RenderQueueEnableVBlank(void) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, onVBlank0, nullptr, false);
    gspSetEventCallback(GSPGPU_EVENT_VBlank1, onVBlank1, nullptr, false);
}

void C3Di_RenderQueueDisableVBlank(void) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, nullptr, nullptr, false);
    gspSetEventCallback(GSPGPU_EVENT_VBlank1, nullptr, nullptr, false);
}

void C3Di_RenderQueueInit(void) {
    CitroContextHead* ctx = CITRO_CONTEXT;

    LightLock_Init(&gateLock);
    LightEvent_Init(&doneEvent, RESET_STICKY);
    frameBusy = false;
    heldCount = 0;
    drawsDone = false;
    idleStart = 0;
    C3Di_RenderQueueEnableVBlank();
#ifdef CTRPORT_RENDER_TIMING
    setOpCallbacks(true);
#endif

    // citro3d's queue holds 32 commands, which a frame drawing through several
    // surfaces passes (see CtrGpu_queueCapacity). libctru hands the GPU the
    // queued commands as it makes room, so the queue's size is only memory.
    gxCmdEntry_s* grown = (gxCmdEntry_s*) realloc(ctx->gxQueue.entries, CTR_GPU_QUEUE_ENTRIES * sizeof(gxCmdEntry_s));
    if (grown != nullptr) {
        ctx->gxQueue.entries = grown;
        ctx->gxQueue.maxEntries = CTR_GPU_QUEUE_ENTRIES;
    }

    queues[0] = &ctx->gxQueue;
    queues[1] = queues[2] = nullptr;
    queueIndex = 0;
    ringEnd = 0;
    secondQueue.maxEntries = ctx->gxQueue.maxEntries;
    secondQueue.entries = (gxCmdEntry_s*) malloc(secondQueue.maxEntries * sizeof(gxCmdEntry_s));
    overlapReady = secondQueue.entries != nullptr;
    if (overlapReady) {
        gxCmdQueueClear(&secondQueue);
        gxCmdQueueSetCallback(&secondQueue, onQueueFinish, nullptr);
        queues[1] = &secondQueue;
    }

    GX_BindQueue(&ctx->gxQueue);
    gxCmdQueueSetCallback(&ctx->gxQueue, onQueueFinish, nullptr);
    gxCmdQueueRun(&ctx->gxQueue);
}

void C3Di_RenderQueueExit(void) {
    int i;
    C3D_RenderTarget *a, *next;

    C3Di_WaitAndClearQueue(-1);
#ifdef CTRPORT_RENDER_TIMING
    setOpCallbacks(false);
#endif
    gxCmdQueueSetCallback(&CITRO_CONTEXT->gxQueue, nullptr, nullptr);
    GX_BindQueue(nullptr);
    if (overlapReady) {
        gxCmdQueueSetCallback(&secondQueue, nullptr, nullptr);
        free(secondQueue.entries);
        secondQueue.entries = nullptr;
        overlapReady = false;
    }
    if (queues[2] != nullptr) {
        gxCmdQueueSetCallback(&thirdQueue, nullptr, nullptr);
        free(thirdQueue.entries);
        thirdQueue.entries = nullptr;
    }
    queues[0] = queues[1] = queues[2] = nullptr;
    queueIndex = 0;

    C3Di_RenderQueueDisableVBlank();

    for (i = 0; i < 3; i++) linkedTarget[i] = nullptr;

    for (a = firstTarget; a; a = next) {
        next = a->next;
        C3Di_RenderTargetDestroy(a);
    }
}

void C3Di_RenderQueueWaitDone(void) {
    C3Di_WaitAndClearQueue(-1);
}

float C3D_FrameRate(float fps) {
    float old = framerate;
    if (fps > 0.0f && fps <= 60.0f) {
        framerate = fps;
        framerateCounter[0] = fps;
        framerateCounter[1] = fps;
    }
    return old;
}

bool C3D_FrameBegin(u8 flags) {
    CitroContextHead* ctx = CITRO_CONTEXT;
    if (inFrame) return false;

    if (flags & C3D_FRAME_SYNCDRAW) C3D_FrameSync();
    if (!C3Di_WaitAndClearQueue((flags & C3D_FRAME_NONBLOCK) ? 0 : -1)) return false;

    inFrame = true;
    osTickCounterStart(&cpuTime);
    GPUCMD_SetBuffer(ctx->cmdBuf, ctx->cmdBufSize, 0);
    frameStart = 0;
    mayWrap = false;
    return true;
}

bool C3D_FrameDrawOn(C3D_RenderTarget* target) {
    if (!inFrame) return false;

    target->used = true;
    C3D_SetFrameBuf(&target->frameBuf);
    C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height);
    return true;
}

void C3D_FrameSplit(u8 flags) {
    u32 *cmdBuf, cmdBufSize;
    if (!inFrame) return;
    if (C3Di_SplitFrame(&cmdBuf, &cmdBufSize)) GX_ProcessCommandList(cmdBuf, cmdBufSize * 4, flags);
}

void C3D_FrameEnd(u8 flags) {
    CitroContextHead* ctx = CITRO_CONTEXT;
    if (!inFrame) return;

    if (frameEndCb) frameEndCb(frameEndCbData);

    C3D_FrameSplit(flags);
    ringEnd = gpuCmdBuf != nullptr ? (u32) (gpuCmdBuf - ctx->cmdBuf) : 0;
    GPUCMD_SetBuffer(nullptr, 0, 0);
    osTickCounterUpdate(&cpuTime);
    inFrame = false;

    // Flush the entire linear memory if the user did not explicitly mandate to flush the command list
    if (!(flags & GX_CMDLIST_FLUSH)) {
        extern u32 __ctru_linear_heap;
        extern u32 __ctru_linear_heap_size;
        GSPGPU_FlushDataCache((void*) __ctru_linear_heap, __ctru_linear_heap_size);
    }

    // The frame before may still be on the GPU, its transfers too. Its
    // queue's end flips the screens: the transfers below are held back until
    // those flips have been shown (releaseReady).
    CtrGpu_waitIdle();

    gxCmdQueue_s* queue = boundQueue();
    u16 firstTransfer = queue->numEntries;
    HeldTransfer added[3];
    u32 addedCount = 0;
    int i;
    C3D_RenderTarget* target;
    isTopStereo = false;
    for (i = 2; i >= 0; i--) {
        target = linkedTarget[i];
        if (!target || !target->used) continue;
        target->used = false;
        // One GX_DisplayTransfer each; its destination is set again when it is let through.
        C3D_FrameBufTransfer(&target->frameBuf, target->screen, target->side, target->transferFlags);
        added[addedCount++] = (HeldTransfer) { target->screen, target->side };
        if (target->screen == GFX_TOP) {
            needSwapTop = true;
            if (target->side == GFX_RIGHT) isTopStereo = true;
        } else if (target->screen == GFX_BOTTOM)
            needSwapBot = true;
    }

    LightLock_Lock(&gateLock);
    noteEvent("frame end: transfers added", queue, nullptr);
    if (queue->numEntries == firstTransfer + addedCount) {
        memcpy(held, added, addedCount * sizeof(HeldTransfer));
        heldCount = addedCount;
        queue->numEntries = firstTransfer;
        runQueueNow = queue;
        drawsDone = false;
        releaseReady(true);
        if (heldCount > 0) paceStats.framesHeldBack++;
    }
    LightLock_Unlock(&gateLock);
    runQueue(queue, addedCount > 0 ? RUN_SCREENS : RUN_NO_SCREENS);
}

void C3D_FrameEndHook(void (*hook)(void*), void* param) {
    frameEndCb = hook;
    frameEndCbData = param;
}

// The GPU's time on the last frame it finished, without the time its
// transfers waited for a flip.
float C3D_GetDrawingTime(void) {
    return (float) ((double) lastGpuTicks / CPU_TICKS_PER_MSEC);
}

float C3D_GetProcessingTime(void) {
    return osTickCounterRead(&cpuTime);
}

static C3D_RenderTarget* C3Di_RenderTargetNew(void) {
    C3D_RenderTarget* target = (C3D_RenderTarget*) malloc(sizeof(C3D_RenderTarget));
    if (!target) return nullptr;
    memset(target, 0, sizeof(C3D_RenderTarget));
    return target;
}

static void C3Di_RenderTargetFinishInit(C3D_RenderTarget* target) {
    target->prev = lastTarget;
    target->next = nullptr;
    if (lastTarget) lastTarget->next = target;
    if (!firstTarget) firstTarget = target;
    lastTarget = target;
}

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, C3D_DEPTHTYPE depthFmt) {
    GPU_DEPTHBUF depthFmtReal = GPU_RB_DEPTH16;
    void* depthBuf = nullptr;
    void* colorBuf = vramAlloc(C3D_CalcColorBufSize(width, height, colorFmt));
    if (!colorBuf) goto _fail0;
    if (C3D_DEPTHTYPE_OK(depthFmt)) {
        depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
        size_t depthSize = C3D_CalcDepthBufSize(width, height, depthFmtReal);
        vramAllocPos vramBank = addrGetVRAMBank(colorBuf);
        depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY); // Attempt opposite bank first...
        if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank);   // ... if that fails, attempt same bank
        if (!depthBuf) goto _fail1;
    }

    C3D_RenderTarget* target = C3Di_RenderTargetNew();
    if (!target) goto _fail2;

    C3D_FrameBuf* fb = &target->frameBuf;
    C3D_FrameBufAttrib(fb, width, height, false);
    C3D_FrameBufColor(fb, colorBuf, colorFmt);
    target->ownsColor = true;
    if (depthBuf) {
        C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
        target->ownsDepth = true;
    }
    C3Di_RenderTargetFinishInit(target);
    return target;

_fail2:
    if (depthBuf) vramFree(depthBuf);
_fail1:
    vramFree(colorBuf);
_fail0:
    return nullptr;
}

C3D_RenderTarget* C3D_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE face, int level, C3D_DEPTHTYPE depthFmt) {
    if (!addrIsVRAM(tex->data)) return nullptr; // Render targets must be in VRAM
    C3D_RenderTarget* target = C3Di_RenderTargetNew();
    if (!target) return nullptr;

    C3D_FrameBuf* fb = &target->frameBuf;
    C3D_FrameBufTex(fb, tex, face, level);

    if (C3D_DEPTHTYPE_OK(depthFmt)) {
        GPU_DEPTHBUF depthFmtReal = C3D_DEPTHTYPE_VAL(depthFmt);
        size_t depthSize = C3D_CalcDepthBufSize(fb->width, fb->height, depthFmtReal);
        vramAllocPos vramBank = addrGetVRAMBank(tex->data);
        void* depthBuf = vramAllocAt(depthSize, vramBank ^ VRAM_ALLOC_ANY); // Attempt opposite bank first...
        if (!depthBuf) depthBuf = vramAllocAt(depthSize, vramBank);         // ... if that fails, attempt same bank
        if (!depthBuf) {
            free(target);
            return nullptr;
        }

        C3D_FrameBufDepth(fb, depthBuf, depthFmtReal);
        target->ownsDepth = true;
    }

    C3Di_RenderTargetFinishInit(target);
    return target;
}

// A render target over a texture anywhere the GPU can reach, linear memory
// included, which C3D_RenderTargetCreateFromTex refuses. For the renderer's
// FCRAM render-target self-test and source-patch construction; no depth buffer.
C3D_RenderTarget* CtrGpu_renderTargetAnywhere(C3D_Tex* tex) {
    C3D_RenderTarget* target = C3Di_RenderTargetNew();
    if (!target) return nullptr;
    C3D_FrameBufTex(&target->frameBuf, tex, GPU_TEXFACE_2D, 0);
    C3Di_RenderTargetFinishInit(target);
    return target;
}

static void C3Di_RenderTargetDestroy(C3D_RenderTarget* target) {
    if (target->ownsColor) vramFree(target->frameBuf.colorBuf);
    if (target->ownsDepth) vramFree(target->frameBuf.depthBuf);

    C3D_RenderTarget** prevNext = target->prev ? &target->prev->next : &firstTarget;
    C3D_RenderTarget** nextPrev = target->next ? &target->next->prev : &lastTarget;
    *prevNext = target->next;
    *nextPrev = target->prev;
    free(target);
}

void C3D_RenderTargetDelete(C3D_RenderTarget* target) {
    if (inFrame) svcBreak(USERBREAK_PANIC); // Shouldn't happen.
    // citro3d waits for the GPU here when the target isn't linked to a
    // screen. Its buffers' VRAM is only freed once the GPU is done with them
    // (ctr_memory.c), and the target itself is only ever read by the CPU.
    if (target->linked) C3D_RenderTargetDetachOutput(target);
    C3Di_RenderTargetDestroy(target);
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags) {
    int id = 0;
    if (screen == GFX_BOTTOM) id = 2;
    else if (side == GFX_RIGHT) id = 1;
    if (linkedTarget[id]) {
        linkedTarget[id]->linked = false;
        if (!inFrame) C3Di_WaitAndClearQueue(-1);
    }
    linkedTarget[id] = target;
    if (target) {
        target->linked = true;
        target->transferFlags = transferFlags;
        target->screen = screen;
        target->side = side;
    }
}

static void C3Di_SafeDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags) {
    C3Di_WaitAndClearQueue(-1);
    inSafeTransfer = true;
    GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
    runQueue(boundQueue(), RUN_OTHER);
}

static void C3Di_SafeTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags) {
    C3Di_WaitAndClearQueue(-1);
    inSafeTransfer = true;
    GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
    runQueue(boundQueue(), RUN_OTHER);
}

static void C3Di_SafeMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1) {
    C3Di_WaitAndClearQueue(-1);
    inSafeTransfer = true;
    GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
    runQueue(boundQueue(), RUN_OTHER);
}

// Waits for the transfer a C3Di_Safe* call just queued: the queue held
// nothing else (they wait for it first), so this waits for that one transfer.
// Not gspWaitForPPF: libctru doesn't discard an event already signalled, and
// every frame's display transfers to the screens signal PPF with nothing
// waiting for it, so that returned at once, before the copy had run. The CPU
// then read pixels not yet copied (surface readback, screenshots).
static void waitSafeTransfer(void) {
    C3Di_WaitAndClearQueue(-1);
}

void C3D_SyncDisplayTransfer(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 flags) {
    if (inFrame) {
        C3D_FrameSplit(0);
        GX_DisplayTransfer(inadr, indim, outadr, outdim, flags);
    } else {
        C3Di_SafeDisplayTransfer(inadr, indim, outadr, outdim, flags);
        waitSafeTransfer();
    }
}

void C3D_SyncTextureCopy(u32* inadr, u32 indim, u32* outadr, u32 outdim, u32 size, u32 flags) {
    if (inFrame) {
        C3D_FrameSplit(0);
        GX_TextureCopy(inadr, indim, outadr, outdim, size, flags);
    } else {
        C3Di_SafeTextureCopy(inadr, indim, outadr, outdim, size, flags);
        waitSafeTransfer();
    }
}

void C3D_SyncMemoryFill(u32* buf0a, u32 buf0v, u32* buf0e, u16 control0, u32* buf1a, u32 buf1v, u32* buf1e, u16 control1) {
    if (inFrame) {
        C3D_FrameSplit(0);
        GX_MemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
    } else {
        C3Di_SafeMemoryFill(buf0a, buf0v, buf0e, control0, buf1a, buf1v, buf1e, control1);
        waitSafeTransfer();
    }
}

// ===[ Ring start reuse (contention, late code) ]===
//
// A frame that would begin too near the end of the command ring waited for the GPU to go idle and began at word 0
// (CtrGpu_frameBeginOverlapped). On the Old models that wait is the whole GPU time of the frame before, 8-78 ms on
// the o3DS. This begins such a frame at word 0 without the wait, in the words before the last
// frame's start, when the GPU's completion count proves that nothing in them is still to run.
//
// Reached through the linker (-Wl,--wrap=CtrGpu_frameBeginOverlapped in CMakeLists.txt): CtrRenderer_beginFrame, the
// one caller, calls this, and the code before stays as it was.
//
// The fence. At each begin here, under gateLock, due = completions + 0 with the GPU idle, + 1 with a queue run on it,
// + 2 with one more waiting behind that (pendingQueue): the count at which every run handed to the GPU so far is done.
// That holds because:
//  - runs are strictly in order. A run starts only with the GPU idle (C3D_FrameEnd and the one-off transfers after
//    CtrGpu_waitIdle, CtrGpu_frameEndBehind when !frameBusy), or from finishFrame, for the one frame waiting behind;
//  - the run on the GPU (frameBusy) always ends in exactly one finishFrame, which adds 1 to completions;
//  - at most one more waits behind it. If that one turns out empty, it never counts, which only puts the proof off.
// So completions reaching due proves each run handed before it done. It is CtrGpu_waitIdle's own accounting
// (frameBusy, finishFrame) and ctr_memory.c's fence for held frees (CtrGpu_completionDue).
//
// The rule. Let L be the last frame, begun here at word S = frameStart > 0 (only this begin sets frameStart above 0;
// C3D_FrameBegin, runFrameSoFar's resume, waits for the GPU and sets it to 0), with its due recorded, and ended at
// E = ringEnd. If E > S, L didn't go round mid-frame (CtrGpu_wrapCommandBuffer leaves E <= S), so its commands are
// [S, E). Every command word in [0, S) was then written before L began, by frames handed to the GPU before L's begin,
// all done once completions reaches L's due. So the new frame takes [0, S) without waiting only when:
//  - S > 0, the record is L's (ringReuse.start == S), and E > S (L in one piece);
//  - no frame waits behind the GPU's (pendingQueue; frame skip ends frames behind);
//  - the GPU is idle, or completions has reached L's due;
//  - S >= minWords: room for the reserve.
// Otherwise it waits as before. The new frame's buffer ends at word S, with mayWrap off. When makeCommandRoom finds it
// nearly full, the frame runs on past S instead (__wrap_CtrGpu_canWrapCommandBuffer): past S are L's commands and,
// past E, older frames', all handed before the new frame began, so done once completions reaches its due. If the
// count isn't there, it waits for the GPU then; either way it goes on in the words it would have taken after a wait
// at its begin. A frame needing more than the whole ring is run so far by makeCommandRoom (runFrameSoFar), as before.
//
// The frame after a reuse (R, with K the frame before it at [S, E)) begins at R's end. Below S, it runs on over [S, E):
// K was handed before R's begin, so it is done once completions reaches R's due; if the GPU still runs and the count
// isn't there, that begin waits for the GPU first. Past S, R ran on, which proved those words free. From then on,
// everything past the last frame's end is from frames proven done, as in the ring without reuse (only a wait to word 0).
//
// The check. At each reuse (and each begin after one that doesn't wait), every queue's command list the GPU hasn't
// finished (entries from lastEntry on, libctru's own progress, not the count above) that lies in the words being
// taken is counted and logged as an overlap.
#define RING_LATE __attribute__((noinline, section(".text.late.ringstart")))
#define RING_LATE_RO __attribute__((section(".rodata.late.ringstart")))
#define RING_LOGGED_WAITS 32u
#define RING_LOGGED_OVERLAPS 16u
#define RING_SUMMARY_EVERY 64u
#define RING_TICKS_PER_US 268u // SYSCLOCK_ARM11 / 1e6 = 268.1 (a wait is far below the 16 s a u32 of ticks holds)

enum { RING_WAIT_START0, RING_WAIT_WRAPPED, RING_WAIT_BEHIND, RING_WAIT_UNPROVEN, RING_WAIT_SMALL, RING_WAIT_KINDS };

static struct {
    bool valid;  // the rest are the last begin's here
    bool reused; // it took the ring's start without waiting
    u32 start;   // its first word
    u32 due;     // completions by which everything on the GPU before it was done
    u32 cap;     // a frame that took the ring's start: the word its buffer ends at (the last frame's start)
    bool extended; // and it ran on past it
    u32 reuses, afterWaits, overlaps, loggedWaits;
    u32 waits[RING_WAIT_KINDS];
    u32 extensions, extendWaits, falls; // ran on past the cap; of those, waited for the GPU first; ran so far instead
    u32 extendWaitUs; // those waits, in microseconds (u32 and integer maths: no 8-byte alignment in late sections)
} ringReuse __attribute__((section(".bss.late.ringstart")));

#ifdef CTRPORT_RING_FINISH_CHECK
// The finish callback check (ringFinishCheck): written on the GSP thread under gateLock, logged by the main thread.
// Identity and diagnostic builds only (CTRPORT_RING_FINISH_CHECK): it adds a lock per queue finish.
static struct {
    u32 checked, mismatches, logged;
    gxCmdQueue_s *queue, *runQueue; // the first mismatch's (the main thread logs them as queue indices)
    bool busy;
    u32 completions;
} ringFinish __attribute__((section(".bss.late.ringstart")));
#endif

static const char ringReuseFirstLog[] RING_LATE_RO =
    "GPU: a frame took the command ring's start without waiting for the GPU (words 0-%lu; the frame before %lu-%lu)\n";
#ifdef CTRPORT_RING_FINISH_CHECK
static const char ringReuseSummaryLog[] RING_LATE_RO =
    "GPU: ring start: %lu frames took it without waiting; %lu waited (frame before at word 0 %lu, went round %lu, a "
    "frame behind %lu, unproven %lu, too small %lu); %lu waits after a reuse; %lu overlaps; %lu ran on past the "
    "ring start's end (%lu waited for the GPU first, %lu.%03lu ms), %lu ran so far instead; %lu finish callbacks, %lu "
    "mismatched\n";
static const char ringFinishMismatchLog[] RING_LATE_RO =
    "GPU: FINISH MISMATCH: %lu of %lu finish callbacks so far; the first: queue %d finished while the run queue was %d "
    "(busy %d, %lu completions)\n";
#else
static const char ringReuseSummaryLog[] RING_LATE_RO =
    "GPU: ring start: %lu frames took it without waiting; %lu waited (frame before at word 0 %lu, went round %lu, a "
    "frame behind %lu, unproven %lu, too small %lu); %lu waits after a reuse; %lu overlaps; %lu ran on past the "
    "ring start's end (%lu waited for the GPU first, %lu.%03lu ms), %lu ran so far instead\n";
#endif
static const char ringReuseExtendLog[] RING_LATE_RO =
    "GPU: a frame that took the ring's start ran on past word %lu at word %lu (waited %lu us for the GPU first)\n";
static const char ringReuseWaitLog[] RING_LATE_RO =
    "GPU: a frame begin at the ring's end waits for the GPU (%s): the frame before %lu-%lu of %lu words\n";
static const char ringReuseOverlapLog[] RING_LATE_RO =
    "GPU: RING OVERLAP: queue %d entry %lu of %lu (%lu done) has a command list at words %ld-%ld, in %lu-%lu being taken\n";
static const char ringReuseWaitNames[RING_WAIT_KINDS][24] RING_LATE_RO = {
    "frame before at word 0", "went round", "a frame behind", "unproven", "too small",
};

bool __wrap_CtrGpu_frameBeginOverlapped(uint32_t minWords) RING_LATE;
bool __wrap_CtrGpu_canWrapCommandBuffer(uint32_t minWords) RING_LATE;
// The real one, called through the linker's name: a call by its own name would be inlined here (as in
// CtrGpu_wrapCommandBuffer), leaving no reference, and --gc-sections would drop it and move the text after it.
bool __real_CtrGpu_canWrapCommandBuffer(uint32_t minWords);
#ifdef CTRPORT_RING_FINISH_CHECK
void ringFinishCheck(gxCmdQueue_s* queue) RING_LATE;
#endif
static void ringReuseCheck(gxCmdQueue_s* const* all, u32 base, u32 lo, u32 hi) RING_LATE;
static void ringReuseSummary(void) RING_LATE;

// Counts (and logs the first few) command lists the GPU hasn't finished in words [lo, hi) of the ring at base. The
// queues and the ring come from the caller: GCC compiles this helper before the file's other functions, and a static
// it named first would be placed first among the file's (section anchors), moving the rest.
static void ringReuseCheck(gxCmdQueue_s* const* all, u32 base, u32 lo, u32 hi) {
    for (int q = 0; 3 > q; q++) {
        gxCmdQueue_s* queue = all[q];
        if (queue == nullptr || queue->entries == nullptr) continue;
        u32 count = queue->numEntries;
        for (u32 i = queue->lastEntry; count > i; i++) {
            const u32* cmd = queue->entries[i].data;
            if ((cmd[0] & 0xFF) != 0x01) continue; // GX_ProcessCommandList(address, bytes)
            int32_t from = (int32_t) (cmd[1] - base) / 4, to = (int32_t) (cmd[1] + cmd[2] - base) / 4;
            if (from >= (int32_t) hi || (int32_t) lo >= to) continue;
            ringReuse.overlaps++;
            if (ringReuse.overlaps <= RING_LOGGED_OVERLAPS) {
                logInfo(ringReuseOverlapLog, q, (unsigned long) i, (unsigned long) count, (unsigned long) queue->lastEntry, (long) from,
                        (long) to, (unsigned long) lo, (unsigned long) hi);
            }
        }
    }
}

static void ringReuseSummary(void) {
    u32 waited = 0;
    for (int i = 0; RING_WAIT_KINDS > i; i++) waited += ringReuse.waits[i];
    u32 us = ringReuse.extendWaitUs;
    logInfo(ringReuseSummaryLog, (unsigned long) ringReuse.reuses, (unsigned long) waited, (unsigned long) ringReuse.waits[RING_WAIT_START0],
            (unsigned long) ringReuse.waits[RING_WAIT_WRAPPED], (unsigned long) ringReuse.waits[RING_WAIT_BEHIND],
            (unsigned long) ringReuse.waits[RING_WAIT_UNPROVEN], (unsigned long) ringReuse.waits[RING_WAIT_SMALL],
            (unsigned long) ringReuse.afterWaits, (unsigned long) ringReuse.overlaps, (unsigned long) ringReuse.extensions,
            (unsigned long) ringReuse.extendWaits, (unsigned long) (us / 1000u), (unsigned long) (us % 1000u), (unsigned long) ringReuse.falls
#ifdef CTRPORT_RING_FINISH_CHECK
            , (unsigned long) ringFinish.checked, (unsigned long) ringFinish.mismatches
#endif
            );
}

bool __wrap_CtrGpu_frameBeginOverlapped(uint32_t minWords) {
    if (inFrame || !overlapReady) return CtrGpu_frameBeginOverlapped(minWords);
    CitroContextHead* ctx = CITRO_CONTEXT;
#ifdef CTRPORT_RING_FINISH_CHECK
    // The finish callback check goes in front of onQueueFinish on every queue that has it (again after a queue is made
    // or set up).
    for (int q = 0; 3 > q; q++) {
        if (queues[q] != nullptr && queues[q]->callback == onQueueFinish) queues[q]->callback = ringFinishCheck;
    }
#endif
    LightLock_Lock(&gateLock);
    bool busy = frameBusy, behind = pendingQueue != nullptr;
    u32 done = completions;
#ifdef CTRPORT_RING_FINISH_CHECK
    u32 mismatches = ringFinish.mismatches;
#endif
    LightLock_Unlock(&gateLock);
#ifdef CTRPORT_RING_FINISH_CHECK
    if (mismatches != ringFinish.logged && RING_LOGGED_OVERLAPS > ringFinish.logged) {
        ringFinish.logged = mismatches;
        int finished = -1, run = -1; // -1: none, or not one of ours
        for (int q = 0; 3 > q; q++) {
            if (queues[q] != nullptr && queues[q] == ringFinish.queue) finished = q;
            if (queues[q] != nullptr && queues[q] == ringFinish.runQueue) run = q;
        }
        logInfo(ringFinishMismatchLog, (unsigned long) mismatches, (unsigned long) ringFinish.checked, finished, run, (int) ringFinish.busy,
                (unsigned long) ringFinish.completions);
    }
#endif
    // Every run handed to the GPU so far is done once completions reaches this (see above). pendingQueue is only
    // set, and only left set, while frameBusy.
    u32 due = done + (busy ? (behind ? 2u : 1u) : 0u);
    bool lastDone = !busy || (int32_t) (done - ringReuse.due) >= 0; // everything before the last begin here is done
    u32 start = frameStart, end = ringEnd, size = ctx->cmdBufSize;
    // The last frame took the ring's start and ended past its cap without running on: makeCommandRoom ran it so far.
    if (ringReuse.valid && ringReuse.reused && !ringReuse.extended && end > ringReuse.cap) ringReuse.falls++;
    ringReuse.extended = false;
    if (end + minWords > size) {
        int wait = -1;
        if (start == 0 || !ringReuse.valid || ringReuse.start != start) wait = RING_WAIT_START0;
        else if (start >= end) wait = RING_WAIT_WRAPPED;
        else if (behind) wait = RING_WAIT_BEHIND;
        else if (!lastDone) wait = RING_WAIT_UNPROVEN;
        else if (minWords > start) wait = RING_WAIT_SMALL;
        if (0 > wait) {
            ringReuseCheck(queues, (u32) ctx->cmdBuf, 0, start);
            // The queue switch as before; with no words asked for, it doesn't wait (ringEnd fits).
            CtrGpu_frameBeginOverlapped(0);
            GPUCMD_SetBuffer(ctx->cmdBuf, start, 0);
            frameStart = 0;
            mayWrap = false;
            ringReuse.valid = ringReuse.reused = true;
            ringReuse.start = 0;
            ringReuse.due = due;
            ringReuse.cap = start;
            ringReuse.reuses++;
            if (ringReuse.reuses == 1) logInfo(ringReuseFirstLog, (unsigned long) start, (unsigned long) start, (unsigned long) end);
            else if (ringReuse.reuses % RING_SUMMARY_EVERY == 0) ringReuseSummary();
            return true;
        }
        ringReuse.waits[wait]++;
        if (RING_LOGGED_WAITS > ringReuse.loggedWaits) {
            ringReuse.loggedWaits++;
            logInfo(ringReuseWaitLog, ringReuseWaitNames[wait], (unsigned long) start, (unsigned long) end, (unsigned long) size);
        }
    } else if (ringReuse.valid && ringReuse.reused && start == 0) {
        // After a reuse this frame runs on from the last's end over the frame before it, done once completions
        // reaches the last's due.
        if (!lastDone) {
            ringReuse.afterWaits++;
            CtrGpu_waitIdle();
        } else {
            ringReuseCheck(queues, (u32) ctx->cmdBuf, end, size);
        }
    }
    bool begun = CtrGpu_frameBeginOverlapped(minWords);
    ringReuse.valid = begun;
    ringReuse.reused = false;
    ringReuse.start = frameStart;
    ringReuse.due = due;
    return begun;
}

// makeCommandRoom asks this when the frame's buffer is nearly full. A frame that took the ring's start ends its
// buffer at the last frame's start (ringReuse.cap); past that, to the ring's end, every word is from frames handed
// before it began, done once completions reaches its due (the rule above). So instead of running the frame so far
// (two waits: for the frame before, then for this one's draws so far), it waits for the GPU only if the count isn't
// there yet, then lets the buffer run on to the ring's end. The commands go on in the same words, in the same order,
// as after a wait at the frame's begin; makeCommandRoom then finds room and returns, with no split.
bool __wrap_CtrGpu_canWrapCommandBuffer(uint32_t minWords) {
    CitroContextHead* ctx = CITRO_CONTEXT;
    if (!inFrame || !ringReuse.valid || !ringReuse.reused || ringReuse.extended || gpuCmdBuf == nullptr) return __real_CtrGpu_canWrapCommandBuffer(minWords);
    u32 base = (u32) (gpuCmdBuf - ctx->cmdBuf), size = ctx->cmdBufSize;
    if (base + gpuCmdBufSize != ringReuse.cap) return __real_CtrGpu_canWrapCommandBuffer(minWords); // run so far already
    u32 at = base + gpuCmdBufOffset;
    if (at + minWords > size) {
        // No room past the cap either: run so far, as before (and not counted again at the next begin).
        ringReuse.falls++;
        ringReuse.cap = UINT32_MAX;
        return __real_CtrGpu_canWrapCommandBuffer(minWords);
    }
    LightLock_Lock(&gateLock);
    bool done = !frameBusy || (int32_t) (completions - ringReuse.due) >= 0;
    LightLock_Unlock(&gateLock);
    u32 waitedUs = 0;
    if (!done) {
        uint64_t from = svcGetSystemTick();
        CtrGpu_waitIdle();
        waitedUs = (u32) (svcGetSystemTick() - from) / RING_TICKS_PER_US;
        ringReuse.extendWaits++;
        ringReuse.extendWaitUs += waitedUs;
    }
    ringReuseCheck(queues, (u32) ctx->cmdBuf, ringReuse.cap, size);
    gpuCmdBufSize += size - ringReuse.cap;
    ringReuse.extended = true;
    ringReuse.extensions++;
    if (ringReuse.extensions == 1) {
        logInfo(ringReuseExtendLog, (unsigned long) ringReuse.cap, (unsigned long) at, (unsigned long) waitedUs);
    }
    return true;
}

#ifdef CTRPORT_RING_FINISH_CHECK
// The finish callback check (ringreview, section 3), log-only: libctru calls this, on the GSP event thread, in front
// of onQueueFinish. The completion count the ring's reuse trusts is finishFrame's, which onQueueFinishLocked reaches
// for any queue. So a finish should only ever come for the queue last handed to the GPU, while it counts as busy:
// neither can change before this callback runs (a new run needs frameBusy cleared, which only this finish does). A
// mismatch is counted and kept here, and the main thread logs it at the next frame begin. Raw GX commands bound to no
// queue of ours, or a finish that matches by chance, aren't caught.
void ringFinishCheck(gxCmdQueue_s* queue) {
    LightLock_Lock(&gateLock);
    ringFinish.checked++;
    if (queue != runQueueNow || !frameBusy) {
        if (ringFinish.mismatches++ == 0) {
            ringFinish.queue = queue;
            ringFinish.runQueue = runQueueNow;
            ringFinish.busy = frameBusy;
            ringFinish.completions = completions;
        }
    }
    LightLock_Unlock(&gateLock);
    onQueueFinish(queue);
}
#endif


// ===[ Waiting for the draws alone (engfix) ]===
//
// Waits until the GPU has run the draws and copies handed to it, a held-back frame's screen transfers aside: those
// wait for a vblank and read only the screen targets. For the CPU about to write memory that the frame before may
// still sample (ctr_renderer.c, initLinearTextureForCpu); CtrGpu_waitIdle would also wait for that frame's vblank.
__attribute__((noinline, section(".text.late.engfix")))
void CtrGpu_waitDraws(void) {
    for (;;) {
        LightLock_Lock(&gateLock);
        bool done = !frameBusy || (drawsDone && pendingQueue == nullptr);
        LightLock_Unlock(&gateLock);
        if (done) return;
        svcSleepThread(100 * 1000);
    }
}

#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
// ===[ Completions of the draws alone (flowergpu, DELTARUNE New) ]===
//
// CtrGpu_completions, with the queue on the GPU counted once its draws are done: finishFrame counts it only after its
// held-back screen transfers, at a vblank. Only the queue running can be in that state, and the one waiting behind it
// starts after it, so every queue the count covers has had all its draws run. For the vertex ring (vine_new_retain.inc,
// cut 5c), which needs only the draws of the frames before the last done.
__attribute__((noinline, section(".text.late.flowergpu")))
uint32_t CtrGpu_drawCompletions(void) {
    LightLock_Lock(&gateLock);
    uint32_t n = completions + (frameBusy && drawsDone ? 1u : 0u);
    LightLock_Unlock(&gateLock);
    return n;
}
#endif
