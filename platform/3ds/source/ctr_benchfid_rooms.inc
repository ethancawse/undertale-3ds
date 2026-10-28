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
            work.cpuMean, work.valid ? work.cpuTail : -1.0, work.gpuMean, work.valid ? work.gpuTail : -1.0, benchRooms.routeWork.cpu[0], benchRooms.routeWork.gpu[0]);
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

// ===== Presentation v1 (utflames, 10-25), in sections of its own (late_reserve.ld, last in each list) so that
// no late code or data already linked moves. benchfidRooms* above are unchanged; the bench calls these wrappers.
#define SHOWN_LATE __attribute__((noinline, noclone, no_reorder, optimize("Os"), section(".text.benchshown_late")))
#define SHOWN_CONST(name, value) static const char name[] __attribute__((section(".text.benchshown_late.constants"))) = value

// How long each shown frame stayed on the game's screen, flip to flip in vblanks, from the pacer's flip totals. After
// the room segment lines, one line per visit (its own line, so every room segment line stays byte for byte as it was):
//     Bench: <id>: room presentation <visit> room=<room> shown_flips=N shown_3vb=N shown_max_vb=N shown_1%low=F
//     room_presentation <id> visit=<visit> room=<room> ...                                   (benchmark.txt, [result])
// The interval fields count when the main thread STARTED frames, so the pacer realigning its phase after a room's
// first played frame (a 40-50 ms start interval while every frame still stays up 2 vblanks) reads as a slow frame
// there, and only a late presentation reads as one here.
//     shown_flips   flips counted in the visit (shown frames)
//     shown_3vb     of them, frames that stayed up 3 or more vblanks (a dropped 30 fps frame)
//     shown_max_vb  the longest a counted frame stayed up, in vblanks
//     shown_1%low   1000 * k / (vblanks of the k longest * 16.714 ms), k = flips / 100 (at least 1); holds of 8+
//                   vblanks count at their mean
// Like hidden_ms, a hidden load is left out: the one hold that ends at the first flip after a room change (the frame
// before it, kept on screen through the load and the new room's first played frame). Flips seen in a capture's
// frame are left out as its interval is. The flips are read once a frame, so a flip lands in the visit current when
// the bench next looks; that moves nothing but the hold, which is left out. A frame of the bench sees one flip, so
// shown_max_vb is exact; with two 8+ holds in one look it is an upper bound.
#define SHOWN_VBLANK_MS (1000.0 / 59.831)
typedef struct __attribute__((packed, aligned(4))) {
    uint16_t count[CTR_FLIP_BUCKETS]; // stayed up 1-7 vblanks, and 8 or more (saturating)
    uint32_t longSum;                 // vblanks of the 8+ holds
    uint32_t maxVb;
} BenchShownVisit;
static struct __attribute__((packed, aligned(4))) {
    BenchShownVisit visits[64];
    uint32_t lastCount[CTR_FLIP_BUCKETS];
    uint32_t lastSum;
    bool skipNext; // the next flip ends a hidden load's hold
} benchfidShown __attribute__((section(".bss.benchshown_late")));
SHOWN_CONST(shownFormat, "room=%s shown_flips=%lu shown_3vb=%lu shown_max_vb=%lu shown_1%%low=%.3f\n");
SHOWN_CONST(shownLogPrefix, "Bench: %s: room presentation %lu %s");
SHOWN_CONST(shownResultPrefix, "room_presentation %s visit=%lu %s");

// Reads the flip totals; with collect, adds the flips since the last read to the current visit.
static void SHOWN_LATE benchshownRead(bool collect) {
    CtrFlipStats now;
    CtrPacer_getFlipTotals(&now);
    uint32_t d[CTR_FLIP_BUCKETS], flips = 0, shortSum = 0;
    for (uint32_t i = 0; i < CTR_FLIP_BUCKETS; i++) {
        d[i] = now.count[i] - benchfidShown.lastCount[i];
        benchfidShown.lastCount[i] = now.count[i];
        flips += d[i];
        shortSum += d[i] * (i + 1);
    }
    const uint32_t L = CTR_FLIP_BUCKETS - 1;
    uint32_t longSum = (uint32_t) now.sum - benchfidShown.lastSum - (shortSum - d[L] * CTR_FLIP_BUCKETS);
    benchfidShown.lastSum = (uint32_t) now.sum;
    if (!collect) return;
    if (flips > 0) {
        if (!CtrRenderer_screenshotTimingAffected(bench.session.renderer)) {
            uint32_t top = CTR_FLIP_BUCKETS;
            while (d[top - 1] == 0) top--;
            if (benchfidShown.skipNext) {
                // The hold that ends the load: the longest flip seen.
                if (top == CTR_FLIP_BUCKETS) longSum -= longSum / d[L];
                d[top - 1]--;
                while (top > 0 && d[top - 1] == 0) top--;
            }
            BenchShownVisit* s = &benchfidShown.visits[benchRooms.count - 1];
            for (uint32_t i = 0; i < CTR_FLIP_BUCKETS; i++) {
                uint32_t c = s->count[i] + d[i];
                s->count[i] = c > 65535u ? 65535u : (uint16_t) c;
            }
            if (top == CTR_FLIP_BUCKETS) {
                s->longSum += longSum;
                top = longSum - (d[L] - 1) * CTR_FLIP_BUCKETS;
            }
            if (top > s->maxVb) s->maxVb = top;
        }
        benchfidShown.skipNext = false; // a hold ending in a capture's frame went with the capture's flips
    }
    if (benchRooms.hiddenPending) benchfidShown.skipNext = true;
}

static void SHOWN_LATE benchshownRoomsOpen(void) {
    memset(&benchfidShown, 0, sizeof(benchfidShown));
    benchshownRead(false);
    benchfidRoomsOpen();
}

static void SHOWN_LATE benchshownRoomsCollect(Runner* runner) {
    if (bench.measureFrames > (int32_t) benchRooms.work && benchRooms.count > 0 && !benchRooms.overflow)
        benchshownRead(true);
    benchfidRoomsCollect(runner);
}

// After benchfidRoomsReport's lines, one presentation line per visit it reported. Cold finalization only.
static void SHOWN_LATE benchshownRoomsReport(void) {
    benchfidRoomsReport();
    char path[256];
    snprintf(path, sizeof(path), roomsPathFormat, bench.session.gameDir, roomsFileName);
    FILE* f = fopen(path, roomsFileMode);
    for (uint32_t i = 0; i < benchRooms.count; i++) {
        const BenchShownVisit* s = &benchfidShown.visits[i];
        uint32_t n = 0;
        for (uint32_t j = 0; j < CTR_FLIP_BUCKETS; j++) n += s->count[j];
        uint32_t k = n / 100 > 0 ? n / 100 : n > 0 ? 1 : 0, left = k;
        double vb = 0.0;
        for (int32_t j = CTR_FLIP_BUCKETS - 1; j >= 0 && left > 0; j--) {
            uint32_t take = left < s->count[j] ? left : s->count[j];
            if (take == 0) continue;
            vb += j == CTR_FLIP_BUCKETS - 1 ? (double) s->longSum * take / s->count[j] : (double) take * (j + 1);
            left -= take;
        }
        char row[192];
        snprintf(row, sizeof(row), shownFormat, benchRooms.visits[i].room, (unsigned long) n,
                (unsigned long) (n - s->count[0] - s->count[1]), (unsigned long) s->maxVb,
                vb > 0.0 ? 1000.0 * k / (vb * SHOWN_VBLANK_MS) : 0.0);
        logInfo(shownLogPrefix, benchRooms.id, (unsigned long) i + 1, row);
        emit(f, shownResultPrefix, benchRooms.id, (unsigned long) i + 1, row);
    }
    if (f != nullptr) fclose(f);
}

#undef SHOWN_LATE
#undef SHOWN_CONST
