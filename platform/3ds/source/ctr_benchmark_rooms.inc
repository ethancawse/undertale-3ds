// Measured-room reporting v2. Included last: existing structures, source lines,
// warm functions and their slots stay fixed. The callers only change BL targets.
// The cold result calculation/callers are unchanged; buffer release uses a late wrapper.
// No game data/variables/keys are changed by this collector.
#define ROOMS_LATE __attribute__((noinline, noclone, no_reorder, section(".text.late.coverage.rooms")))
#define ROOMS_CONST(name, value) static const char name[] __attribute__((section(".rodata.late.coverage.rooms"))) = value
#define ROOMS_VISITS 64
#define ROOMS_TAIL 128

// Exact top-K work samples without allocating or logging during measurement.
// 128 entries cover floor(N/100) through N=12,899. Longer windows explicitly
// report UNAVAILABLE rather than estimating the missing tail. CPU is paired.
typedef struct __attribute__((packed, aligned(4))) {
    double cpu[ROOMS_TAIL], gpu[ROOMS_TAIL], cpuSum, gpuSum;
    uint32_t frames;
} BenchWorkTail;
typedef struct __attribute__((packed, aligned(4))) {
    double cpuMean, gpuMean, cpuTail, gpuTail;
    uint32_t frames;
    bool valid, pass;
} BenchWorkResult;

// Packed, four-aligned doubles avoid increasing the existing late BSS alignment.
// Per-frame work does not allocate. Sorting allocates only after the window ends.
typedef struct __attribute__((packed, aligned(4))) {
    char room[128];
    int32_t roomIndex, firstFrame, lastFrame;
    uint32_t frames, intervals, firstInterval;
    double drawSum, drawMax;
    BenchWorkResult headroom;
} BenchRoomVisit;
static struct __attribute__((packed, aligned(4))) {
    BenchRoomVisit visits[ROOMS_VISITS];
    char id[32], expected[128], destination[128];
    uint32_t count, work, intervals, excluded;
    int32_t openingIndex, requested;
    double previousStep, previousDraw, previousGpu;
    int32_t previousSkipped;
    uint32_t hiddenExcluded, workExcluded;
    BenchWorkTail routeWork, visitWork;
    bool hiddenPending;
    bool active, battleOnly, invalid, overflow, departurePending, errorPending;
} benchRooms __attribute__((section(".bss.late.coverage.rooms")));

ROOMS_CONST(roomsPass, "PASS");
ROOMS_CONST(roomsFail, "FAIL");
ROOMS_CONST(roomsUnavailable, "UNAVAILABLE");
ROOMS_CONST(roomsLocked, "LOCKED");
ROOMS_CONST(roomsDips, "DIPS");
ROOMS_CONST(roomsUnder, "UNDER");
ROOMS_CONST(roomsHeadroomFormat,
    "Bench: %s: HEADROOM=%s work_contract=work/v1 frames=%lu hidden_load_excluded=%lu other_excluded=%lu "
    "cpu_mean_ms=%.3f cpu_slowest1_ms=%.3f gpu_mean_ms=%.3f gpu_slowest1_ms=%.3f limit_ms=30.000 cpu_max_ms=%.3f gpu_max_ms=%.3f\n");
ROOMS_CONST(roomsHeadroomResultFormat,
    "headroom %s work_contract=work/v1 HEADROOM=%s frames=%lu hidden_load_excluded=%lu other_excluded=%lu "
    "cpu_mean_ms=%.3f cpu_slowest1_ms=%.3f gpu_mean_ms=%.3f gpu_slowest1_ms=%.3f limit_ms=30.000\n");
ROOMS_CONST(roomsUnknown, "?");
ROOMS_CONST(roomsNone, "none");
ROOMS_CONST(roomsRoute, "route/v2");
ROOMS_CONST(roomsBattle, "battle-only/v2");
ROOMS_CONST(roomsToken, "# measurement-contract ");
ROOMS_CONST(roomsInvalid, "invalid measurement-contract v2 (expected route/v2 or battle-only/v2 ROOM)");
ROOMS_CONST(roomsDepartedEarly, "battle-only/v2 departed before measurement opened");
ROOMS_CONST(roomsOverflow, "room segments incomplete: more than 64 measured room visits");
ROOMS_CONST(roomsFormat,
    "Bench: %s: measurement contract=%s requested_frames=%ld actual_frames=%lu timed_intervals=%lu "
    "capture_excluded=%lu start_room=%s measured_room_visits=%lu departed_to=%s incomplete=%d\n");
ROOMS_CONST(roomsResultFormat,
    "measurement %s contract=%s requested_frames=%ld actual_frames=%lu timed_intervals=%lu "
    "capture_excluded=%lu aggregate_room=start_room start_room=%s measured_room_visits=%lu departed_to=%s incomplete=%d\n");
ROOMS_CONST(roomsPathFormat, "%s/%s");
ROOMS_CONST(roomsFileMode, "a");
ROOMS_CONST(roomsFileName, "benchmark.txt");
ROOMS_CONST(roomsRangeFormat, "Bench: %s measured frames %d-%d\n");

static void ROOMS_LATE benchWorkAdd(BenchWorkTail* t, double cpu, double gpu) {
    uint32_t n = t->frames < ROOMS_TAIL ? t->frames : ROOMS_TAIL;
    uint32_t at = n;
    while (at > 0 && cpu > t->cpu[at - 1]) at--;
    if (at < ROOMS_TAIL) {
        uint32_t last = n < ROOMS_TAIL ? n : ROOMS_TAIL - 1;
        for (uint32_t i = last; i > at; i--) t->cpu[i] = t->cpu[i - 1];
        t->cpu[at] = cpu;
    }
    at = n;
    while (at > 0 && gpu > t->gpu[at - 1]) at--;
    if (at < ROOMS_TAIL) {
        uint32_t last = n < ROOMS_TAIL ? n : ROOMS_TAIL - 1;
        for (uint32_t i = last; i > at; i--) t->gpu[i] = t->gpu[i - 1];
        t->gpu[at] = gpu;
    }
    t->frames++;
    t->cpuSum += cpu;
    t->gpuSum += gpu;
}

static BenchWorkResult ROOMS_LATE benchWorkResult(const BenchWorkTail* t) {
    BenchWorkResult r = {0};
    r.frames = t->frames;
    if (r.frames == 0) return r;
    r.cpuMean = t->cpuSum / r.frames;
    r.gpuMean = t->gpuSum / r.frames;
    uint32_t k = r.frames / 100 > 0 ? r.frames / 100 : 1;
    if (k > ROOMS_TAIL) return r;
    r.valid = true;
    for (uint32_t i = 0; i < k; i++) {
        r.cpuTail += t->cpu[i];
        r.gpuTail += t->gpu[i];
    }
    r.cpuTail /= k;
    r.gpuTail /= k;
    r.pass = r.cpuTail <= 30.0 && r.gpuTail <= 30.0;
    return r;
}

static const char* ROOMS_LATE benchWorkVerdict(const BenchWorkResult* r) {
    return !r->valid ? roomsUnavailable : r->pass ? roomsPass : roomsFail;
}

// This replaces only the frontend's existing call target. Record the hidden
// load BEFORE the runner clears pendingRoom; same-room restarts count too.
// No timing state enters Runner, StateHash or any game data.
void ROOMS_LATE CtrBench_handleRoomChange(Runner* runner) {
    if (benchRooms.active && bench.phase == PHASE_MEASURE && runner->pendingRoom != -1)
        benchRooms.hiddenPending = true;
    Runner_handlePendingRoomChange(runner);
}

static const char* ROOMS_LATE benchRoomsName(Runner* runner) {
    return runner->currentRoom != nullptr ? runner->currentRoom->name : roomsUnknown;
}

// A full comment line, not a game-script operation. Includes preserve comments.
// Do not silently reinterpret unknown versions or duplicate declarations.
static void ROOMS_LATE benchRoomsContract(const char* script) {
    bool seen = false;
    for (const char* p = script; p != nullptr && *p != '\0';) {
        const char* end = strchr(p, '\n');
        if (end == nullptr) end = p + strlen(p);
        const char* next = *end == '\n' ? end + 1 : end;
        while (p < end && (*p == ' ' || *p == '\t')) p++;
        size_t prefix = sizeof(roomsToken) - 1;
        if ((size_t) (end - p) >= prefix && strncmp(p, roomsToken, prefix) == 0) {
            if (seen) { benchRooms.invalid = true; return; }
            seen = true;
            p += prefix;
            while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
            size_t battle = sizeof(roomsBattle) - 1, route = sizeof(roomsRoute) - 1;
            if ((size_t) (end - p) == route && strncmp(p, roomsRoute, route) == 0) {
                // Explicit route/v2 is equivalent to the default.
            } else if ((size_t) (end - p) > battle && strncmp(p, roomsBattle, battle) == 0 && p[battle] == ' ') {
                p += battle + 1;
                while (p < end && (*p == ' ' || *p == '\t')) p++;
                size_t n = (size_t) (end - p);
                if (n == 0 || n >= sizeof(benchRooms.expected)) { benchRooms.invalid = true; return; }
                for (const char* q = p; q < end; q++) {
                    if (*q == ' ' || *q == '\t') { benchRooms.invalid = true; return; }
                }
                memcpy(benchRooms.expected, p, n);
                benchRooms.expected[n] = '\0';
                benchRooms.battleOnly = true;
            } else { benchRooms.invalid = true; return; }
        }
        p = next;
    }
}

static void ROOMS_LATE benchRoomsVisit(Runner* runner) {
    if (benchRooms.overflow) return;
    if (benchRooms.count > 0 && benchRooms.visits[benchRooms.count - 1].roomIndex == runner->currentRoomIndex) return;
    if (benchRooms.count >= ROOMS_VISITS) { benchRooms.overflow = true; return; }
    if (benchRooms.count > 0)
        benchRooms.visits[benchRooms.count - 1].headroom = benchWorkResult(&benchRooms.visitWork);
    memset(&benchRooms.visitWork, 0, sizeof(benchRooms.visitWork));
    BenchRoomVisit* v = &benchRooms.visits[benchRooms.count++];
    copyText(v->room, sizeof(v->room), benchRoomsName(runner));
    v->roomIndex = runner->currentRoomIndex;
    v->firstFrame = runner->frameCount;
    v->lastFrame = runner->frameCount - 1;
    v->firstInterval = benchRooms.intervals;
}

static void ROOMS_LATE benchRoomsOpen(void) {
    benchPreOpen();
    memset(&benchRooms, 0, sizeof(benchRooms));
    if (sweep.on) return; // The sweep has its own independently reset segments.
    Runner* runner = bench.session.runner;
    benchRooms.active = true;
    benchRooms.openingIndex = runner->currentRoomIndex;
    benchRooms.requested = frameWindow();
    copyText(benchRooms.id, sizeof(benchRooms.id), currentResult()->id);
    if (!tour.on) benchRoomsContract(bench.scenes[bench.queue[bench.queuePos]].script);
    benchRoomsVisit(runner);
    if (benchRooms.battleOnly && strcmp(benchRooms.expected, benchRoomsName(runner)) != 0) {
        benchRooms.invalid = true;
        copyText(benchRooms.destination, sizeof(benchRooms.destination), benchRoomsName(runner));
    }
}

// The incoming interval belongs to the preceding work frame. Use the same
// float conversion and screenshot exclusion as CtrBench_beginFrame, before
// it appends that interval to the unchanged route buffer.
static void ROOMS_LATE benchRoomsCollect(Runner* runner) {
    if (bench.measureFrames <= (int32_t) benchRooms.work || benchRooms.count == 0) return;
    BenchRoomVisit* v = &benchRooms.visits[benchRooms.count - 1];
    double draw = bench.drawSum - benchRooms.previousDraw;
    double cpu = bench.stepSum - benchRooms.previousStep + draw;
    double gpu = bench.gpuSum - benchRooms.previousGpu;
    bool skipped = bench.skippedFrames != benchRooms.previousSkipped;
    benchRooms.previousStep = bench.stepSum;
    benchRooms.previousDraw = bench.drawSum;
    benchRooms.previousGpu = bench.gpuSum;
    benchRooms.previousSkipped = bench.skippedFrames;
    // Cadence/route totals remain unchanged. Only the new work contract
    // excludes hidden load frames, captures and deliberately skipped frames.
    if (benchRooms.hiddenPending) benchRooms.hiddenExcluded++;
    else if (CtrRenderer_screenshotTimingAffected(bench.session.renderer) || skipped) benchRooms.workExcluded++;
    else {
        benchWorkAdd(&benchRooms.routeWork, cpu, gpu);
        if (!benchRooms.overflow) benchWorkAdd(&benchRooms.visitWork, cpu, gpu);
    }
    benchRooms.hiddenPending = false;
    if (!benchRooms.overflow) {
        v->drawSum += draw;
        if (draw > v->drawMax) v->drawMax = draw;
        v->frames++;
        v->lastFrame = runner->frameCount - 1;
    }
    benchRooms.work++;
    if (CtrRenderer_screenshotTimingAffected(bench.session.renderer)) benchRooms.excluded++;
    else {
        benchRooms.intervals++;
        if (!benchRooms.overflow) v->intervals++;
    }
}

// Layout anchor, never called. GCC aligns a string constant by the first function that expands it. Here
// utils.h's safeMallocFunction message was first reached by speed-compiled code. Without this, the first is
// benchfidRoomsReport (optimize "Os"), so the message leaves .rodata.str1.4 (the pool's copy) for a new
// .rodata.str1.1. That moves the linker's merged str1.1 strings into early .rodata (+40 B).
static void* __attribute__((used)) ROOMS_LATE benchRoomsAnchor(size_t size) {
    return safeMallocFunction(size, roomsFileName, __LINE__);
}

static BenchScriptState ROOMS_LATE benchRoomsTick(BenchScript* script, Runner* runner) {
    if (!benchRooms.active || bench.phase != PHASE_MEASURE) return benchPreTick(script, runner);
    if (benchRooms.invalid) {
        benchRooms.errorPending = true;
        return BENCH_SCRIPT_MEASURING;
    }
    bool departed = benchRooms.battleOnly && runner->currentRoomIndex != benchRooms.openingIndex;
    // No extra script tick on an explicitly terminated departure. Default
    // route scenes still run the exact original tick, including their final one.
    BenchScriptState state = departed ? BENCH_SCRIPT_MEASURING : benchPreTick(script, runner);
    benchshownRoomsCollect(runner);
    if (departed) {
        copyText(benchRooms.destination, sizeof(benchRooms.destination), benchRoomsName(runner));
        benchRooms.departurePending = true;
    } else if (state != BENCH_SCRIPT_FAILED) benchRoomsVisit(runner);
    return state;
}

// Consume the shot flag while the script is still alive. The original caller
// uses it immediately after tick; endScene releases the script, so an explicit
// termination must happen here rather than inside the tick wrapper.
static bool ROOMS_LATE benchRoomsTakeShot(BenchScript* script) {
    bool shot = BenchScript_takeShot(script);
    Runner* runner = bench.session.runner;
    if (benchRooms.errorPending) {
        benchRooms.errorPending = false;
        benchRooms.active = false;
        endScene(benchRooms.destination[0] != '\0' ? roomsDepartedEarly : roomsInvalid);
        return false;
    }
    if (benchRooms.departurePending) {
        benchRooms.departurePending = false;
        // The original loop will see PHASE_NEXT and not append this twice.
        if (bench.measureFrames > 0 && !CtrRenderer_screenshotTimingAffected(bench.session.renderer))
            arrput(bench.frameMs, (float) (runner->deltaTime / 1000.0));
        logInfo(roomsRangeFormat, currentResult()->id, bench.measureStartFrame, runner->frameCount - 1);
        endScene(nullptr);
        return false;
    }
    return shot;
}

// Cold route-buffer release only. Keep computeResult's original direct callers
// and string-pool order. Finalize after the original calculation/log and before
// its intervals are freed. No script pointer is used: dropScript already ran.
// A zero-interval result is unmeasured in the original result contract.
static void ROOMS_LATE benchRoomsRelease(void* buffer) {
    SceneResult* r = currentResult();
    if (benchRooms.active && bench.phase == PHASE_MEASURE && !sweep.on && r != nullptr && r->measured)
        benchshownRoomsReport();
    benchRooms.active = false;
    free(buffer);
}

#undef ROOMS_LATE
#undef ROOMS_CONST
#undef ROOMS_TAIL
#undef ROOMS_VISITS
