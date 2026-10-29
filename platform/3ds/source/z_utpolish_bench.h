#pragma once
// utpolish's slow-frame line state (z_utpolish_bench.c), shared with ctr_benchmark.c's benchfix code, which gives a
// bench window its own line budget (the per-room-visit cap, SLOW_ROOM_LINES, ran out in room intros before Knight's
// windows began).

#include <stdbool.h>
#include <stdint.h>

#include "ctr_renderer.h"
#include "runner.h"

typedef struct {
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
} UtSlowFrame;

extern UtSlowFrame utSlowFrame; // utpolish's static slowFrame
