// freewins: main-loop segment timers and off switches, for diagnostic builds only (build.sh ... freewins).
// CMakeLists.txt compiles this file only then; in every other build ctr_benchmark.h makes FW_MARK and FW_NOTE empty
// and FW_SKIP 0, and main.c's FW_ calls sit at the ends of existing lines, so main.c's code and line numbers are the
// release build's.
//
// main.c marks points of its frame loop (FW_MARK(n)). Each frame's ticks between consecutive points are summed per
// segment, with each segment's worst, and logged once per window (150 frames, or a room change):
//   [fw] frames <first>-<last> room <index> <name> n <frames> cpu <avg>/<max> wait <avg>/<max> <segment> <avg>/<max>...
// all in microseconds of svcGetSystemTick (on Azahar: emulated time, which counts instructions, not cache misses).
// "cpu" is the whole frame, from one loop top to the next, less the slot wait (the beginFrame call).
// GAME_DIR/freewins.txt, read once, holds a hex mask of FW_SKIP switches: work the release build does that the
// diagnostic leaves out, to cost it by its absence (main.c says what each bit skips).
//
// Probes inside the engine and the renderer (FW_T, FW_ACC and FW_N from utils.h, at the ends of existing lines of
// runner.c and ctr_renderer.c, empty in every other build) add to accumulators, logged on the same line as
// " acc <name> <avg>/<max> <calls>": the average and worst per frame in microseconds, and the calls per frame. FW_N
// accumulators count instead (" acc <name> <avg>/<max>" per frame). The first line gives one svcGetSystemTick's cost:
// each timed call pays about two.
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "runner.h"

#define FW_POINTS 24
#define FW_WINDOW 150
#define FW_ACCS 20
#define FW_COUNT_ACC 14

// The probes' ids (runner.c, ctr_renderer.c).
static const char* const accNames[FW_ACCS] = {
    "kbd",      // 0 Runner_step: the keyboard event scan and any-key/no-key checks
    "timeline", // 1 tickTimelines
    "mouse",    // 2 dispatchMouseEvents
    "cleanup",  // 3 Runner_cleanupDestroyedInstances + Runner_sweepDeadStructs
    "drawsub",  // 4 fireDrawSubtype, whole (the calls that find an event slot)
    "drawev",   // 5 the draw events it runs (inside drawsub)
    "texbeg",   // 6 CtrTextures_beginFrame
    "rbpre",    // 7 CtrRenderer_beginFrame before the slot wait
    "rbpost",   // 8 CtrRenderer_beginFrame after the wait (holds texbeg, mirhot, pal)
    "blit",     // 9 CtrRenderer_endFrame: the last batch flush and present()
    "gpuwait",  // 10 CtrRenderer_endFrame: CtrGpu_waitIdle (waiting, not work)
    "submit",   // 11 CtrRenderer_endFrame: C3D_FrameEnd
    "pal",      // 12 palette bookkeeping in beginFrame
    "mirhot",   // 13 mirrorHotTiles
    "drawlist", // 14 (count) drawables in the passes that find an event slot
    "dpidle",   // 15 (count) idle passes (no responder instance): their drawables
    "dpone",    // 16 (count) passes with one responder instance: their drawables
    "dpmany",   // 17 (count) passes with more: their responder instances
    "dpent",    // 18 (count) responder objects the idle test read
    "dpdiag",   // 19 the census itself (inside drawsub; take it out)
};

typedef struct {
    uint8_t from, to;
    const char* name;
} FwSegment;

// In loop order. A segment counts in a frame only when both its points were passed (the paused path skips most).
static const FwSegment segments[] = {
    {0, 16, "head"},      // notices, lab poll, game speed, autoshot/trace/probe frame lists
    {16, 1, "setters"},   // the frame-skip setters before the wait
    {2, 19, "input"},     // hid, chord guard, combo, menu
    {19, 20, "benchbeg"}, // CtrBench_beginFrameTimed
    {20, 17, "checks"},   // exit checks, drawing/slider overrides
    {17, 18, "display"},  // setDisplay, yieldBottomScreen, dialogueShown, console switch
    {18, 3, "send"},      // sendInput, autoshot list, playback
    {3, 4, "step"},
    {4, 5, "draw"},
    {5, 6, "kbd"},
    {6, 7, "audio"},      // the audio system's update
    {7, 8, "roomchg"},    // state hash (off), room change call
    {8, 9, "present"},    // CtrRenderer_endFrame
    {9, 10, "benchend"},  // CtrBench_endFrameTimed
    {10, 11, "startup"},  // startup log lines, save failure
    {11, 12, "openfile"}, // CtrOpenFiles_poll
    {12, 13, "roomlog"},  // room change log/prefetch, slow frame log, sums
    {13, 14, "stats"},    // the once-a-second stats block
    {14, 15, "flushbot"}, // the bottom console's framebuffer flush
    {15, 0, "loopend"},
};
#define FW_SEGMENTS (sizeof(segments) / sizeof(segments[0]))

static struct {
    bool loaded;
    uint32_t mask;
    uint64_t t[FW_POINTS];
    uint32_t seen;
    int32_t frame, room;
    const Runner* runner;
    // the window
    int32_t first, last, windowRoom;
    uint32_t frames;
    uint64_t sum[FW_SEGMENTS], max[FW_SEGMENTS];
    uint32_t count[FW_SEGMENTS];
    uint64_t cpuSum, cpuMax, waitSum, waitMax;
    int32_t cpuMaxFrame;
    // accumulators: this frame's, and the window's
    uint64_t accFrame[FW_ACCS], accSum[FW_ACCS], accMax[FW_ACCS];
    uint32_t accCallsFrame[FW_ACCS], accCalls[FW_ACCS];
} fw;

static unsigned us(uint64_t ticks) { return (unsigned) (ticks * 1000000ULL / SYSCLOCK_ARM11); }

static void fwLoad(void) {
    fw.loaded = true;
    FILE* f = fopen(CTRPORT_GAME_DIR "/freewins.txt", "r");
    if (f != nullptr) {
        char line[32] = {0};
        if (fgets(line, sizeof(line), f) != nullptr) fw.mask = (uint32_t) strtoul(line, nullptr, 16);
        fclose(f);
    }
    uint64_t t0 = svcGetSystemTick();
    for (int i = 0; 1000 > i; i++) (void) svcGetSystemTick();
    uint64_t thousand = svcGetSystemTick() - t0;
    logInfo("[fw] freewins probe on, skip mask %lx, 1000 svcGetSystemTick %u us\n", (unsigned long) fw.mask, us(thousand));
}

bool FreeWins_skip(unsigned bit) {
    if (!fw.loaded) fwLoad();
    return (fw.mask & bit) != 0;
}

static void fwFlush(void) {
    if (fw.frames == 0) return;
    char text[1400];
    const char* name = "?";
    if (fw.runner != nullptr && fw.runner->currentRoom != nullptr) name = fw.runner->currentRoom->name;
    int n = snprintf(text, sizeof(text), "[fw] frames %ld-%ld room %ld %s n %lu cpu %u/%u maxf %ld wait %u/%u",
                     (long) fw.first, (long) fw.last, (long) fw.windowRoom, name, (unsigned long) fw.frames,
                     us(fw.cpuSum / fw.frames), us(fw.cpuMax), (long) fw.cpuMaxFrame, us(fw.waitSum / fw.frames),
                     us(fw.waitMax));
    for (size_t i = 0; FW_SEGMENTS > i && (int) sizeof(text) - 40 > n; i++) {
        if (fw.count[i] == 0) continue;
        n += snprintf(text + n, sizeof(text) - n, " %s %u/%u", segments[i].name, us(fw.sum[i] / fw.count[i]),
                      us(fw.max[i]));
    }
    for (size_t i = 0; FW_ACCS > i && (int) sizeof(text) - 48 > n; i++) {
        if (fw.accCalls[i] == 0) continue;
        if (i >= FW_COUNT_ACC && i != 19) {
            n += snprintf(text + n, sizeof(text) - n, " acc %s %lu/%lu %lu", accNames[i],
                          (unsigned long) (fw.accSum[i] / fw.frames), (unsigned long) fw.accMax[i],
                          (unsigned long) (fw.accCalls[i] / fw.frames));
        } else {
            n += snprintf(text + n, sizeof(text) - n, " acc %s %u/%u %lu", accNames[i], us(fw.accSum[i] / fw.frames),
                          us(fw.accMax[i]), (unsigned long) (fw.accCalls[i] / fw.frames));
        }
    }
    logInfo("%s\n", text);
    fw.frames = 0;
    fw.cpuSum = fw.cpuMax = fw.waitSum = fw.waitMax = 0;
    memset(fw.sum, 0, sizeof(fw.sum));
    memset(fw.max, 0, sizeof(fw.max));
    memset(fw.count, 0, sizeof(fw.count));
    memset(fw.accSum, 0, sizeof(fw.accSum));
    memset(fw.accMax, 0, sizeof(fw.accMax));
    memset(fw.accCalls, 0, sizeof(fw.accCalls));
}

// The frame that ended at this loop top: add its segments to the window.
static void fwCloseFrame(uint64_t now) {
    if ((fw.seen & 1u) == 0 || (fw.seen & (1u << 3)) == 0) return; // no loop top or no Step: a paused frame
    if (fw.frames > 0 && (fw.room != fw.windowRoom || fw.frames >= FW_WINDOW)) fwFlush();
    if (fw.frames == 0) {
        fw.first = fw.frame;
        fw.windowRoom = fw.room;
    }
    fw.last = fw.frame;
    fw.frames++;
    fw.t[FW_POINTS - 1] = now;
    for (size_t i = 0; FW_SEGMENTS > i; i++) {
        uint8_t from = segments[i].from, to = segments[i].to;
        uint64_t end;
        if (to == 0) {
            end = now;
        } else {
            if ((fw.seen & (1u << to)) == 0) continue;
            end = fw.t[to];
        }
        if ((fw.seen & (1u << from)) == 0 || fw.t[from] > end) continue;
        uint64_t d = end - fw.t[from];
        fw.sum[i] += d;
        fw.count[i]++;
        if (d > fw.max[i]) fw.max[i] = d;
    }
    uint64_t total = now - fw.t[0];
    uint64_t wait = (fw.seen & (1u << 1)) && (fw.seen & (1u << 2)) ? fw.t[2] - fw.t[1] : 0;
    uint64_t cpu = total - wait;
    fw.cpuSum += cpu;
    fw.waitSum += wait;
    if (cpu > fw.cpuMax) {
        fw.cpuMax = cpu;
        fw.cpuMaxFrame = fw.frame;
    }
    if (wait > fw.waitMax) fw.waitMax = wait;
    for (size_t i = 0; FW_ACCS > i; i++) {
        fw.accSum[i] += fw.accFrame[i];
        if (fw.accFrame[i] > fw.accMax[i]) fw.accMax[i] = fw.accFrame[i];
        fw.accCalls[i] += fw.accCallsFrame[i];
    }
}

void FreeWins_note(const void* runner) {
    fw.runner = (const Runner*) runner;
    fw.frame = fw.runner->frameCount;
    fw.room = fw.runner->currentRoomIndex;
}

void FreeWins_mark(int point) {
    uint64_t now = svcGetSystemTick();
    if (point == 0) {
        if (!fw.loaded) fwLoad();
        fwCloseFrame(now);
        fw.seen = 0;
        memset(fw.accFrame, 0, sizeof(fw.accFrame));
        memset(fw.accCallsFrame, 0, sizeof(fw.accCallsFrame));
        now = svcGetSystemTick(); // the window's log line is not the next frame's work
    }
    fw.t[point] = now;
    fw.seen |= 1u << point;
}

uint64_t FreeWins_tick(void) { return svcGetSystemTick(); }

void FreeWins_acc(int id, uint64_t t0) {
    fw.accFrame[id] += svcGetSystemTick() - t0;
    fw.accCallsFrame[id]++;
}

void FreeWins_count(int id, int32_t n) {
    fw.accFrame[id] += (uint64_t) n;
    fw.accCallsFrame[id]++;
}
