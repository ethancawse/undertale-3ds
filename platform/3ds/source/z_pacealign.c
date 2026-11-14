// pacealign (change auditfixe, Option A, 2026-11-05; UNDERTALE only: CMakeLists.txt builds this file and defines
// CTRPORT_PACEALIGN for UNDERTALE alone).
//
// The whole-frame numbers of a bench row come from the pacer's [pace] lines, which main.c's stats block prints every
// 5 s after a "[perf] frame N:" line naming the frame. A measured window rarely starts or ends on one of those prints,
// so the line that spans a window's start also holds warm-up frames, and the window's last stretch is printed only by
// the next stats block, which --exit-after never reaches: the game leaves in the frame the window ends. A battle sweep
// printed no window at all. Here a window's opening and end, and a sweep's end, mark an edge in the bench's step (the
// window's first frame A, or the frame after its last, B+1), and the pacer cuts its numbers at that frame's end, once
// the GPU has finished the frame before (ctr_frame_pacer.c's end says what each report holds). The cut prints the
// same pair of lines for each edge (a "[perf] frame N: bench window opens|ends" line, then CtrPacer_logStats; an
// opening in the frame a window ends prints its line only, as there is nothing between the two), so every window is
// covered by [pace] lines that begin and end on its edges, its last frame's GPU time included. A sweep's end also
// prints the "Bench: <id> measured frames A-B" line a timed scene prints: A is the frame the battle was ready on, B the
// last one it played.
//
// Logging only: no game state, no picture, nothing on the screens (the console is off for the cut's lines, as in
// main.c's stats block). ctr_benchmark.c calls CtrBench_paceOpen, CtrBench_paceWindowEnd and CtrBench_paceSweepEnd in
// place of CtrLog_beginConsole and two logInfo calls, with the same arguments (ctr_benchmark.h). All of it is late
// code and constants in .text_pacealign and .rodata.pacealign_in_text, appended at the end of late text, and the
// edges waiting for their cut in .bss.pacealign_late, at the end of the late statics (late_reserve.ld).
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "log.h"

#include "ctr_benchmark.h"
#include "ctr_frame_pacer.h"
#include "ctr_log.h"

#define PACEALIGN_TEXT __attribute__((noinline, noclone, noipa, section(".text_pacealign")))
#define PACEALIGN_CONST __attribute__((section(".rodata.pacealign_in_text")))
#define PACEALIGN_BSS __attribute__((section(".bss.pacealign_late")))

static const char paceEdgeFormat[] PACEALIGN_CONST = "[perf] frame %d: bench window %s\n";
static const char paceOpens[] PACEALIGN_CONST = "opens";
static const char paceEnds[] PACEALIGN_CONST = "ends";
// The same text as ctr_benchmark.c's timed-scene window line, which the boards read.
static const char paceWindowFormat[] PACEALIGN_CONST = "Bench: %s measured frames %d-%d\n";

// Edges waiting for the pacer's cut at this frame's end: PACE_END and PACE_OPEN, and the frames their lines name.
enum { PACE_END = 1, PACE_OPEN = 2 };
static uint8_t paceDue PACEALIGN_BSS;
static int32_t paceEndFrame PACEALIGN_BSS;
static int32_t paceOpenFrame PACEALIGN_BSS;

// An edge, cut at the end of this frame (CtrBench_paceCutLog).
static void PACEALIGN_TEXT paceEdge(int32_t frame, bool end) {
    if (end) {
        paceEndFrame = frame;
        paceDue |= PACE_END;
    } else {
        paceOpenFrame = frame;
        paceDue |= PACE_OPEN;
    }
}

bool PACEALIGN_TEXT CtrBench_paceCutDue(void) {
    return paceDue != 0;
}

// The pacer's cut (ctr_frame_pacer.c's end), with the closing report's numbers in place: each edge's frame-anchor line,
// then that report. An end comes first; an opening in the same frame has nothing since it, so prints its line alone.
void PACEALIGN_TEXT CtrBench_paceCutLog(void) {
    uint8_t due = paceDue;
    paceDue = 0;
    bool console = CtrLog_consoleEnabled();
    CtrLog_setConsoleEnabled(false);
    if ((due & PACE_END) != 0) {
        logInfo(paceEdgeFormat, (int) paceEndFrame, paceEnds);
        CtrPacer_logStats();
    }
    if ((due & PACE_OPEN) != 0) {
        logInfo(paceEdgeFormat, (int) paceOpenFrame, paceOpens);
        if ((due & PACE_END) == 0) CtrPacer_logStats();
    }
    CtrLog_setConsoleEnabled(console);
}

// beginMeasuring: measuring begins with the frame about to step. Then the call it stands in for.
void PACEALIGN_TEXT CtrBench_paceOpen(void) {
    int32_t now = 0, first = 0;
    CtrBench_paceFrames(&now, &first);
    paceEdge(now, false);
    CtrLog_beginConsole();
}

// A timed scene's window line ("Bench: %s measured frames %d-%d\n": its id, first and last frame), then its end.
void PACEALIGN_TEXT CtrBench_paceWindowEnd(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const char* id = va_arg(args, const char*);
    int first = va_arg(args, int);
    int last = va_arg(args, int);
    va_end(args);
    logInfo(format, id, first, last);
    paceEdge((int32_t) last + 1, true);
}

// A sweep's summary line (sweepFinishBattle's: id, title, why, turns, seconds, rows, worst segment, its ms, lowest fps,
// lowest 1% low), then the battle's window line and its end.
void PACEALIGN_TEXT CtrBench_paceSweepEnd(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const char* id = va_arg(args, const char*);
    const char* title = va_arg(args, const char*);
    const char* why = va_arg(args, const char*);
    long turns = va_arg(args, long);
    double seconds = va_arg(args, double);
    long rows = va_arg(args, long);
    const char* worst = va_arg(args, const char*);
    double worstMs = va_arg(args, double);
    double minFps = va_arg(args, double);
    double minLow = va_arg(args, double);
    va_end(args);
    logInfo(format, id, title, why, turns, seconds, rows, worst, worstMs, minFps, minLow);
    int32_t now = 0, first = 0;
    CtrBench_paceFrames(&now, &first);
    logInfo(paceWindowFormat, id, (int) first, (int) now - 1);
    paceEdge(now, true);
}
