// Cadence v3: hidden loads are reported separately; work/v1 is unchanged.
// Preserve the old collector's slots, structures and string pool. Only three
// call targets change. New constants use the late text reserve: Old rodata is full.
#define HIDDEN_LATE __attribute__((noinline, noclone, no_reorder, section(".text.late.benchfid.hidden")))
#define HIDDEN_CONST(name, value) static const char name[] __attribute__((section(".text.late.benchfid.hidden.constants"))) = value
static struct __attribute__((packed, aligned(4))) {
    uint32_t* at;
    double ms[64];
    uint32_t frames[64];
} benchfidHidden __attribute__((section(".bss.late.benchfid.hidden")));
HIDDEN_CONST(hiddenSource, __FILE__);
HIDDEN_CONST(hiddenVisitFormat,
    "%lu room=%s frames=%lu range=%ld-%ld intervals=%lu "
    "fps=%.3f 1%%low=%.3f draw=%.3f/%.3f ms class=%s HEADROOM=%s work_frames=%lu "
    "cpu_ms=%.3f/%.3f gpu_ms=%.3f/%.3f work_contract=work/v1 hidden_ms=%.3f hidden_frames=%lu\n");
HIDDEN_CONST(hiddenVisitLogPrefix, "Bench: %s: room segment %s");
HIDDEN_CONST(hiddenVisitResultPrefix, "room_segment %s contract=%s visit=%s");

static void HIDDEN_LATE benchfidRoomsOpen(void) {
    arrfree(benchfidHidden.at);
    memset(&benchfidHidden, 0, sizeof(benchfidHidden));
    benchRoomsOpen();
}

static void HIDDEN_LATE benchfidRoomsCollect(Runner* runner) {
    if (bench.measureFrames > (int32_t) benchRooms.work && benchRooms.count > 0 &&
            !benchRooms.overflow && benchRooms.hiddenPending) {
        uint32_t visit = benchRooms.count - 1;
        benchfidHidden.ms[visit] += (float) (runner->deltaTime / 1000.0);
        benchfidHidden.frames[visit]++;
        // Capture intervals are already absent from the original buffer.
        if (!CtrRenderer_screenshotTimingAffected(bench.session.renderer))
            arrput(benchfidHidden.at, benchRooms.intervals);
    }
    benchRoomsCollect(runner);
}

// Cold finalization only: size optimization leaves measured frame work alone.
static void __attribute__((optimize("Os"))) HIDDEN_LATE benchfidRoomsReport(void) {
    // The original result calculation has consumed the complete, unchanged
    // route buffer. A room seen only at the ending boundary did no measured work.
    if (benchRooms.count > 0)
        benchRooms.visits[benchRooms.count - 1].headroom = benchWorkResult(&benchRooms.visitWork);
    while (benchRooms.count > 1 && benchRooms.visits[benchRooms.count - 1].frames == 0) benchRooms.count--;
    const char* contract = benchRooms.battleOnly ? roomsBattle : roomsRoute;
    const char* destination = benchRooms.destination[0] != '\0' ? benchRooms.destination : roomsNone;
    char path[256];
    snprintf(path, sizeof(path), roomsPathFormat, bench.session.gameDir, roomsFileName);
    FILE* f = fopen(path, roomsFileMode);
    logInfo(roomsFormat, benchRooms.id, contract, (long) benchRooms.requested, (unsigned long) benchRooms.work,
            (unsigned long) benchRooms.intervals, (unsigned long) benchRooms.excluded, benchRooms.visits[0].room,
            (unsigned long) benchRooms.count, destination, (int) benchRooms.overflow);
    emit(f, roomsResultFormat, benchRooms.id, contract, (long) benchRooms.requested, (unsigned long) benchRooms.work,
            (unsigned long) benchRooms.intervals, (unsigned long) benchRooms.excluded, benchRooms.visits[0].room,
            (unsigned long) benchRooms.count, destination, (int) benchRooms.overflow);
    BenchWorkResult work = benchWorkResult(&benchRooms.routeWork);
    logInfo(roomsHeadroomFormat, benchRooms.id, benchWorkVerdict(&work), (unsigned long) work.frames,
            (unsigned long) benchRooms.hiddenExcluded, (unsigned long) benchRooms.workExcluded,
            work.cpuMean, work.valid ? work.cpuTail : -1.0, work.gpuMean, work.valid ? work.gpuTail : -1.0);
    emit(f, roomsHeadroomResultFormat, benchRooms.id, benchWorkVerdict(&work), (unsigned long) work.frames,
            (unsigned long) benchRooms.hiddenExcluded, (unsigned long) benchRooms.workExcluded,
            work.cpuMean, work.valid ? work.cpuTail : -1.0, work.gpuMean, work.valid ? work.gpuTail : -1.0);
    if (benchRooms.overflow) logWarn(roomsOverflow);
    ptrdiff_t hiddenAt = 0;
    for (uint32_t i = 0; i < benchRooms.count; i++) {
        BenchRoomVisit* v = &benchRooms.visits[i];
        uint32_t n = 0;
        float* sorted = v->intervals > 0 ? safeMallocFunction(sizeof(float) * v->intervals, hiddenSource, __LINE__) : nullptr;
        double sum = 0.0, slowSum = 0.0;
        for (uint32_t j = 0; j < v->intervals; j++) {
            uint32_t at = v->firstInterval + j;
            // Both streams are in route-buffer order, including room revisits.
            if (hiddenAt < arrlen(benchfidHidden.at) && benchfidHidden.at[hiddenAt] == at) {
                hiddenAt++;
                continue;
            }
            float ms = bench.frameMs[at];
            sorted[n++] = ms;
            sum += ms;
        }
        if (n > 0) qsort(sorted, n, sizeof(float), compareSlowestFirst);
        uint32_t low = n / 100 > 0 ? n / 100 : n > 0 ? 1 : 0;
        for (uint32_t j = 0; j < low; j++) slowSum += sorted[j];
        free(sorted);
        double fps = sum > 0.0 ? 1000.0 * n / sum : 0.0;
        double lowFps = slowSum > 0.0 ? 1000.0 * low / slowSum : 0.0;
        double draw = v->frames > 0 ? v->drawSum / v->frames : 0.0;
        const char* cadence = n == 0 ? roomsUnavailable : fps < 29.8 ? roomsUnder : lowFps < 28.5 ? roomsDips : roomsLocked;
        BenchWorkResult* w = &v->headroom;
        // Format the shared fields once; only the two record prefixes differ.
        char row[768];
        snprintf(row, sizeof(row), hiddenVisitFormat, (unsigned long) i + 1, v->room, (unsigned long) v->frames,
                (long) v->firstFrame, (long) v->lastFrame, (unsigned long) n, fps, lowFps, draw, v->drawMax,
                cadence, benchWorkVerdict(w), (unsigned long) w->frames, w->cpuMean,
                w->valid ? w->cpuTail : -1.0, w->gpuMean, w->valid ? w->gpuTail : -1.0, benchfidHidden.ms[i], (unsigned long) benchfidHidden.frames[i]);
        logInfo(hiddenVisitLogPrefix, benchRooms.id, row);
        emit(f, hiddenVisitResultPrefix, benchRooms.id, contract, row);
    }
    if (f != nullptr) fclose(f);
    benchRooms.active = false;
}


#undef HIDDEN_LATE
#undef HIDDEN_CONST
