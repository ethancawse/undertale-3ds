// A log line for each bench frame over two vblanks (utpolish).
//
// A scene's result line gives its worst frame and 1% low, not which frames those were or what they spent. While a
// test runs, each frame whose interval is over 33.4 ms (two vblanks at 59.83 Hz are 33.43 ms) gets a line: its game
// frame, interval, Step, Draw and GPU time, texture loads, and whether it changed room:
//
//     Frame over 33.4 ms: frame <f>: <ms> ms, step <s>, draw <d>, gpu <g>, textures <n>/<t> ms[, room changed] in <room>
//
// The frame numbers are the ones the bench's "Bench: <id> measured frames A-B" line uses, so
// tools/analysis/ut_lock_check.py keeps the lines inside each scene's measured window (setup and settle frames are
// logged too). At most SLOW_ROOM_LINES lines per room visit.
//
// The main loop calls these in place of CtrBench_sessionStarted, CtrBench_beginFrame and CtrBench_endFrame. It is a
// translation unit of its own, with all code and state in utpolish's late sections (platform/3ds/late_reserve.ld),
// so ctr_benchmark.c's code is unchanged.

#include "ctr_benchmark.h"

#include "log.h"
#include "runner.h"
#include "ctr_renderer.h"

#define SLOW_LATE __attribute__((noinline, section(".text.late.utpolish")))
#define SLOW_BSS __attribute__((section(".bss.late.utpolish")))
#define SLOW_RODATA __attribute__((section(".rodata.late.utpolish")))
#define SLOW_FRAME_MS 33.4
#define SLOW_ROOM_LINES 16

static struct {
    Runner* runner; // the session's, from CtrBench_sessionStarted
    Renderer* renderer;
    bool valid; // the frame before this one was recorded by CtrBench_endFrameTimed
    bool roomChanged;
    int32_t room; // the room the frame began in
    int32_t linesRoom; // the room visit the lines were counted for
    int32_t lines;
    double stepMs, drawMs, gpuMs;
    uint32_t loadsAtStart, loads;
    float loadMsAtStart, loadMs;
} slowFrame SLOW_BSS;

static const char slowFrameLine[] SLOW_RODATA =
    "Frame over 33.4 ms: frame %ld: %.1f ms, step %.1f, draw %.1f, gpu %.1f, textures %lu/%.1f ms%s in %s\n";
static const char slowFrameRoomChanged[] SLOW_RODATA = ", room changed";
static const char slowFrameNothing[] SLOW_RODATA = "";

void SLOW_LATE CtrBench_sessionStartedLate(const CtrBenchSession* session) {
    slowFrame.runner = session != NULL ? session->runner : NULL;
    slowFrame.renderer = session != NULL ? session->renderer : NULL;
    slowFrame.valid = false;
    slowFrame.linesRoom = -1;
    CtrBench_sessionStarted(session);
}

CtrBenchFrame SLOW_LATE CtrBench_beginFrameTimed(u32 down, double frameMs) {
    CtrBenchFrame benchfidBeginFrame(u32, double); CtrBenchFrame frame = benchfidBeginFrame(down, frameMs);
    Runner* runner = slowFrame.runner;
    if (runner == NULL || slowFrame.renderer == NULL || !CtrBench_running()) {
        slowFrame.valid = false;
        return frame;
    }
    // frameMs is the interval of the frame before this one.
    if (slowFrame.valid && frameMs > SLOW_FRAME_MS) {
        if (slowFrame.linesRoom != slowFrame.room) {
            slowFrame.linesRoom = slowFrame.room;
            slowFrame.lines = 0;
        }
        if (SLOW_ROOM_LINES > slowFrame.lines) {
            slowFrame.lines++;
            const char* room = runner->currentRoom != NULL ? runner->currentRoom->name : slowFrameNothing;
            logInfo(slowFrameLine, (long) runner->frameCount - 1, frameMs, slowFrame.stepMs, slowFrame.drawMs, slowFrame.gpuMs,
                    (unsigned long) slowFrame.loads, (double) slowFrame.loadMs, slowFrame.roomChanged ? slowFrameRoomChanged : slowFrameNothing,
                    room);
        }
    }
    slowFrame.valid = false;
    CtrRenderStats stats = CtrRenderer_getStats(slowFrame.renderer);
    slowFrame.loadsAtStart = stats.textureLoads;
    slowFrame.loadMsAtStart = stats.textureLoadMs;
    slowFrame.room = runner->currentRoomIndex;
    return frame;
}

void SLOW_LATE CtrBench_endFrameTimed(double stepMs, double drawMs, bool skipped) {
    CtrBench_endFrame(stepMs, drawMs, skipped);
    Runner* runner = slowFrame.runner;
    if (runner == NULL || slowFrame.renderer == NULL || !CtrBench_running()) return;
    CtrRenderStats stats = CtrRenderer_getStats(slowFrame.renderer);
    slowFrame.stepMs = stepMs;
    slowFrame.drawMs = drawMs;
    slowFrame.gpuMs = skipped ? 0.0 : (double) stats.gpuMs;
    slowFrame.loads = stats.textureLoads - slowFrame.loadsAtStart;
    slowFrame.loadMs = stats.textureLoadMs - slowFrame.loadMsAtStart;
    slowFrame.roomChanged = runner->currentRoomIndex != slowFrame.room;
    slowFrame.valid = true;
}
