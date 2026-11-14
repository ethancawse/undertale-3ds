// Compile the original TU unchanged first; import only .text_asrholdbenchready
// from this helper pass. The product adapter retargets four existing calls.
#include "../ctr_benchmark.c"
#include "ctr_benchready.h"

#define BR_CODE __attribute__((noinline, noipa, optimize("Os"), section(".text_asrholdbenchready")))
extern __typeof__(bench) brBench __asm__("bench");
extern __typeof__(tour) brTour __asm__("tour");

// Public to later benchmark adapters: call after ready, and once per warmup
// tick. Only the existing lastStatus timestamp changes; no private state.
BR_CODE void CtrBench_readySuppressStatus(void) {
    brBench.lastStatus = nowNanos();
}

// Only beginMeasuring's select/clear calls target these no-ops. Its existing
// console lock, pace boundary and every other console caller stay untouched.
BR_CODE PrintConsole* CtrBench_readySkipSelect(PrintConsole* console) {
    return console;
}

BR_CODE void CtrBench_readySkipClear(void) {
}

BR_CODE BenchScriptState CtrBench_readyScriptTick(BenchScript* script, Runner* runner) {
    BenchScriptState state = BenchScript_tick(script, runner);
    bool ready = brBench.phase == PHASE_SETUP && script == brBench.script &&
        runner == brBench.session.runner && state == BENCH_SCRIPT_MEASURING &&
        !brTour.booting && !brTour.ending;
    if (ready) {
        // Before benchPreTick arms prewindow and before direct sweep opens.
        // Use the plain console lock: BENCH_PACE_OPEN must cut pacealign at
        // window open, not at this earlier ready transition.
        CtrLog_beginConsole();
        consoleSelect(brBench.console);
        consoleClear();
        CtrLog_endConsole();
    }
    if (ready || brBench.phase == PHASE_WARMUP) CtrBench_readySuppressStatus();
    return state;
}
