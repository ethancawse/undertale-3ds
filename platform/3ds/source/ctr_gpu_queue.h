// Lets the CPU record a frame while the GPU still runs the one before.
//
// citro3d's C3D_FrameBegin waits for the GPU to finish the last frame before
// recording can start, so a frame took the CPU's time plus the GPU's.
// ctr_gpu_queue.c replaces citro3d's frame functions (its renderqueue.c) with
// a version that alternates two GX queues and records each frame into the
// command buffer after the last one's commands (a ring), so the frame before
// can stay on the GPU while the next is recorded. C3D_FrameEnd then waits for
// it before handing over the new one, as the screens' transfers and flips
// must follow it.
//
// What the GPU still reads while the next frame is recorded has to stay put:
// its commands and vertices (rings), and the memory it samples, which
// ctr_memory.c keeps from being reused until the GPU is done with it (it
// holds back linear and VRAM frees made while the GPU is busy).
#ifndef CTRPORT_CTR_GPU_QUEUE_H
#define CTRPORT_CTR_GPU_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <3ds.h>
#include <citro3d.h>

// C3D_FrameBegin without waiting for the GPU: the frame's commands follow the
// last frame's in the command buffer, or start it over (after waiting) when
// fewer than minWords are left after them.
bool CtrGpu_frameBeginOverlapped(uint32_t minWords);

// Waits until the GPU has run everything handed to it, a frame's held-back
// screen transfers included (and so its flips asked for). Recording a frame
// hands it nothing until C3D_FrameEnd (or C3D_FrameBegin's full wait).
void CtrGpu_waitIdle(void);

// Whether work handed to the GPU isn't done yet (held-back transfers too).
bool CtrGpu_busy(void);

// How many times the GPU has finished the work handed to it; anything
// handed over before a count was read is done once the count has moved on.
uint32_t CtrGpu_completions(void);
// The count at which everything handed over so far is done: one more than
// now, or two with a frame waiting behind the one on the GPU
// (CtrGpu_frameEndBehind). Memory freed now is reusable from then on.
uint32_t CtrGpu_completionDue(void);

// C3D_FrameEnd for a frame that drew on no screen (fixed 2:1 frame skip's
// unshown frames), without waiting for the frame
// before: if that is still on the GPU, this one waits behind it in the GPU's
// queue and is started when it finishes (from the GSP thread), and the GPU
// counts as busy until both are done. The next frame then records into a
// third queue. False, with the frame still open, if it drew on a screen.
bool CtrGpu_frameEndBehind(void);
// The GX commands (command lists, fills, copies) the frame being recorded has
// queued so far, and how many of them are fills and copies; its last command
// list is added when it ends.
uint32_t CtrGpu_frameCommands(uint32_t* fills, uint32_t* copies);

// The GPU's time on the frames it finished since the last call: [0] frames
// with screen transfers (shown), [1] those without (unshown, skipped).
typedef struct {
    uint32_t frames[2];
    float avgMs[2], maxMs[2];
} CtrGpuFrameStats;
void CtrGpu_takeFrameGpuStats(CtrGpuFrameStats* out);

// Test hook (pacestress.txt "gpu<ms>" and "gpuskip<ms>", CtrPacer_loadStress):
// the GPU counts a frame's draws as done no sooner than this long after the
// frame's queue started, for frames with screen transfers and without, so an
// emulator that runs the GPU's work at once shows a console's GPU-bound
// pacing. 0 turns it off.
void CtrGpu_setSimulatedGpu(float screensMs, float noScreensMs);

// The GX commands a frame's queue holds. citro3d's 32 are raised to
// CTR_GPU_QUEUE_ENTRIES when there is memory for them. Nothing takes commands
// out of the queue until the frame ends, and libctru panics when it is full.
#define CTR_GPU_QUEUE_ENTRIES 128
uint32_t CtrGpu_queueCapacity(void);

// Once the command buffer's end is near: whether this frame can carry on at
// its start, with at least minWords there, rather than having the GPU run the
// frame so far. True only if the frame began past the start (an overlapped
// begin) and hasn't wrapped yet.
bool CtrGpu_canWrapCommandBuffer(uint32_t minWords);

// Carries on at the command buffer's start, after waiting for the frame
// before. Call right after C3D_FrameSplit, with nothing recorded since, and
// only when CtrGpu_canWrapCommandBuffer said so.
void CtrGpu_wrapCommandBuffer(void);

// A GX command added to the queue of the frame being recorded, which the GPU
// is handed when the frame ends (or is ended early).
typedef struct {
    void* queue;
    uint32_t runs;
    uint16_t index;
} CtrGpuQueued;

// The GX command added last.
CtrGpuQueued CtrGpu_lastQueued(void);

// Turns a queued texture copy from `from` to `to` into one that changes
// nothing (16 bytes of its destination onto themselves), if the GPU hasn't
// been handed it yet. The queue keeps its length, so the GPU's completions
// still count as before.
bool CtrGpu_cancelCopy(CtrGpuQueued queued, const void* from, const void* to);

// Whether the frame being recorded holds a texture copy, not yet handed to
// the GPU, that writes somewhere in [start, start + size) from elsewhere (a
// cancelled copy writes its destination onto itself and doesn't count).
bool CtrGpu_queuedCopyInto(const void* start, uint32_t size);

// A render target over tex wherever it is, linear memory included (citro3d
// allows only VRAM). Used by the FCRAM self-test and cached source patches.
C3D_RenderTarget* CtrGpu_renderTargetAnywhere(C3D_Tex* tex);

// System ticks the main thread waited in CtrGpu_waitIdle since the last call.
uint64_t CtrGpu_takeWaitTicks(void);
// The same, since the start (CtrGpu_takeWaitTicks doesn't reset it).
uint64_t CtrGpu_waitTotalTicks(void);

// The word of the command buffer the frame being recorded began at, and the
// whole buffer (flickerprobe.txt's records and its "flush" switch).
uint32_t CtrGpu_frameStartWord(void);
void CtrGpu_commandBuffer(uint32_t** words, uint32_t* bytes);

// A frame's screen transfers are held back in its queue until each screen's
// last flip has been shown (and let go by the frame pacer); the frame's draws
// don't wait for that. Counts since the last call:
typedef struct {
    // The GPU idle with transfers held back (waiting for a flip), and idle
    // with nothing handed to it (waiting for the main thread to end a frame).
    uint64_t flipIdleTicks, mainIdleTicks;
    // Frames with transfers held back at C3D_FrameEnd, and those whose draws
    // were done before the transfers were let through.
    uint32_t framesHeldBack, framesFlipIdle;
    // Transfers let through from a vblank (or a finished queue).
    uint32_t transfersWaited;
    // Transfers let through into the buffer the screen's LCD registers said
    // it was showing: a tear. Never expected; the check reads the hardware,
    // not this file's own state.
    uint32_t shownWrites;
    // Transfers into the shown buffer of a single-buffered screen (the
    // bottom one while it has the debug console): into the same buffer as
    // the screen's last transfer, which is the only one it has. Not a tear
    // the gate can prevent. A tear on a double-buffered screen writes the
    // other buffer than the last transfer did (the one a pending flip is
    // leaving), so it is never counted here.
    uint32_t singleBufferWrites;
    // Held-back transfers dropped because no vblank let them through (the
    // vblank callbacks are off while suspended).
    uint32_t droppedTransfers;
} CtrGpuPaceStats;

// Logs the first few transfers into a shown buffer since the last call: the
// screen, when it was let through, and the registers it was checked against.
void CtrGpu_logShownWrites(void);
void CtrGpu_takePaceStats(CtrGpuPaceStats* out);

// Off, transfers go through at once as citro3d's did, but without the frame
// pacer waiting for the flips first: the tear check's negative control
// (pacestress.txt with "nogate"). On by default.
void CtrGpu_setGateEnabled(bool enabled);

// For hang reports (ctr_watchdog.c): the gate's state and where the GSP event
// thread is in this file's callbacks, read without locks (one of them may be
// what is stuck) and printed without the heap.
void CtrGpu_describeStall(char* out, size_t size);
// The last gate events, oldest first, one a line.
void CtrGpu_describeEvents(char* out, size_t size);

#ifdef CTRPORT_RENDER_TIMING
// The GPU's time by kind of GX command, since the last call.
enum { CTR_GPU_OP_DRAW, CTR_GPU_OP_FILL, CTR_GPU_OP_COPY, CTR_GPU_OP_KINDS };
typedef struct {
    float ms[CTR_GPU_OP_KINDS];
    uint32_t count[CTR_GPU_OP_KINDS];
} CtrGpuOpStats;
void CtrGpu_takeOpStats(CtrGpuOpStats* out);

// A trace of the GPU's work: each GX command's time, as it finishes, from
// CtrGpu_traceStart on. CtrGpu_traceTake (with the GPU idle) stops it and
// hands out the times and the GX commands of the queue run last, in order.
#define CTR_GPU_TRACE_MAX 1024
typedef struct {
    uint8_t kind; // CTR_GPU_OP_*
    uint32_t ticks;
} CtrGpuTraceOp;
void CtrGpu_traceStart(void);
uint32_t CtrGpu_traceTake(CtrGpuTraceOp* ops, gxCmdEntry_s* entries, uint32_t* entryCount);
#endif

// Waits until the GPU has run the draws handed to it; a held-back frame's screen transfers may
// still wait for their vblank (ctr_gpu_queue.c).
void CtrGpu_waitDraws(void);
#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS)
// CtrGpu_completions with the queue on the GPU counted once its draws are done, its screen transfers aside.
uint32_t CtrGpu_drawCompletions(void);
#endif

#endif
