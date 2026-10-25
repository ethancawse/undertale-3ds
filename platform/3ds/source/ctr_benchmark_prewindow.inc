// Included after the original benchmark functions: all new state/code/constants stay in the late reserves.
// Existing callers change only a BL target. The per-frame interpreter and benchmark entry stay in their slots.
typedef struct {
    bool active;
    int32_t ready;
    unsigned previousActivity;
    uint32_t frames, intervals, setup, gameplay, guarded, seek, warp, input, writes, captures;
    float tileMs, setupTileMs, gameplayTileMs, worstMs;
    float lastTileMs;
} BenchPrewindow;
_Static_assert(_Alignof(BenchPrewindow) == 4, "pre-window state must not realign earlier late data");
static BenchPrewindow prewindow __attribute__((section(".bss.late.benchromfs.prewindow")));
static const char prewindowFormat[] __attribute__((section(".rodata.late.benchromfs.prewindow"))) =
    "[prewindow] %s ready=%ld open=%ld frames=%lu tile_ms=%.3f worst_ms=%.3f over_33_4=%lu "
    "setup_frames=%lu gameplay_frames=%lu guarded_frames=%lu seek_frames=%lu warp_frames=%lu input_frames=%lu "
    "write_frames=%lu setup_tile_ms=%.3f gameplay_tile_ms=%.3f interval_frames=%lu capture_excluded=%lu\n";
static uint32_t prewindowSlow __attribute__((section(".bss.late.benchromfs.prewindow")));

static BenchScriptState __attribute__((noinline, section(".text.late.benchromfs.prewindow")))
benchPreTick(BenchScript* script, Runner* runner) {
    // Incoming deltaTime and renderer totals belong to the preceding game frame, exactly as beginFrame's
    // frameMs does. Collect before this tick can warp, inject keys or otherwise perform new setup work.
    if (prewindow.active && bench.phase == PHASE_WARMUP) {
        float total = CtrRenderer_getStats(bench.session.renderer).textureLoadMs;
        float tiles = total >= prewindow.lastTileMs ? total - prewindow.lastTileMs : 0.0f;
        prewindow.lastTileMs = total;
        prewindow.tileMs += tiles;
        unsigned flags = prewindow.previousActivity;
        bool setup = (flags & (BENCH_ACTIVITY_SEEK | BENCH_ACTIVITY_WARP | BENCH_ACTIVITY_WRITE | BENCH_ACTIVITY_INPUT)) != 0;
        prewindow.frames++;
        if (setup) { prewindow.setup++; prewindow.setupTileMs += tiles; }
        else { prewindow.gameplay++; prewindow.gameplayTileMs += tiles; }
        if (flags & BENCH_ACTIVITY_GUARD) prewindow.guarded++;
        if (flags & BENCH_ACTIVITY_SEEK) prewindow.seek++;
        if (flags & BENCH_ACTIVITY_WARP) prewindow.warp++;
        if (flags & BENCH_ACTIVITY_INPUT) prewindow.input++;
        if (flags & BENCH_ACTIVITY_WRITE) prewindow.writes++;
        if (CtrRenderer_screenshotTimingAffected(bench.session.renderer)) prewindow.captures++;
        else {
            // Float literals keep this input section 4-byte aligned; doubles realign the whole late output.
            float us = (float) runner->deltaTime;
            float ms = us * 0.001f;
            prewindow.intervals++;
            if (ms > prewindow.worstMs) prewindow.worstMs = ms;
            if (us > 33400.0f) prewindowSlow++;
        }
    }
    unsigned activity = 0;
    bool observe = bench.phase == PHASE_SETUP || bench.phase == PHASE_WARMUP;
    if (observe) BenchScript_observeBefore(script);
    // Keep the original call outside bench_script.c: a same-unit caller makes GCC outline/split its body,
    // changing the warm layout even though every added observer function has a late section attribute.
    BenchScriptState state = BenchScript_tick(script, runner);
    if (observe) activity = BenchScript_observeAfter(script);
    if (bench.phase == PHASE_SETUP && state == BENCH_SCRIPT_MEASURING && !tour.booting && !tour.ending) {
        prewindow = (BenchPrewindow) {0};
        prewindowSlow = 0;
        prewindow.active = true;
        prewindow.ready = runner->frameCount;
        prewindow.lastTileMs = CtrRenderer_getStats(bench.session.renderer).textureLoadMs;
    }
    prewindow.previousActivity = activity;
    return state;
}

static void __attribute__((noinline, section(".text.late.benchromfs.prewindow"))) benchPreOpen(void) {
    if (prewindow.active) {
        logInfo(prewindowFormat, currentResult()->id, (long) prewindow.ready, (long) bench.session.runner->frameCount,
                (unsigned long) prewindow.frames, prewindow.tileMs, prewindow.worstMs, (unsigned long) prewindowSlow,
                (unsigned long) prewindow.setup, (unsigned long) prewindow.gameplay, (unsigned long) prewindow.guarded,
                (unsigned long) prewindow.seek, (unsigned long) prewindow.warp, (unsigned long) prewindow.input,
                (unsigned long) prewindow.writes, prewindow.setupTileMs, prewindow.gameplayTileMs,
                (unsigned long) prewindow.intervals, (unsigned long) prewindow.captures);
        prewindow.active = false;
    }
    resetMeasuring();
}
