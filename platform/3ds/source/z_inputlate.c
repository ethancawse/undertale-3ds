// inputlatch 0b18ff39, attached through existing calls: no main/engine body grows.
#include <3ds.h>
#include <stdlib.h>
#include "ctr_benchmark.h"
#include "ctr_chapters.h"
#include "ctr_input.h"
#include "ctr_input_delivery.h"
#include "ctr_menu.h"
#include "ctr_renderer.h"
#include "ctr_wrap_chain.h"
#include "input_recording.h"
#include "log.h"

#define CODE __attribute__((noinline, section(".text.late.inputlate")))
#define BSS __attribute__((section(".bss.late.inputlate")))
#define RO __attribute__((section(".rodata.inputlate_in_text")))

static Runner* game BSS;
static Renderer* gameRenderer BSS;
static uint32_t previousKeys BSS;
static CtrInputStats stats BSS;
static bool inputlateInFrame BSS;
static bool menuOpen BSS;
static bool menuWasOpen BSS;
static bool benchWasActive BSS;
static bool benchPaused BSS;
static bool suppressHeld BSS;
static bool blockPolledKeys BSS;
static bool playback BSS;
static const char startFailed[] RO = "Input: could not start the HID sampler\n";
static const char stopped[] RO = "Input: stopped, worker core %d, %lu samples / %lu worker drains, %lu read retries, %lu history overruns; %lu main takeovers, %lu worker returns, %lu retry misses, %lu main drains; elapsed %.3f ms, max worker gap %.3f ms\n";

void __real_CtrBench_sessionStarted(const CtrBenchSession* session);
void __wrap_CtrBench_sessionStarted(const CtrBenchSession* session);
void __real_CtrRenderer_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
void __real_CtrRenderer_beginFrame(Renderer* renderer);
void __wrap_CtrRenderer_beginFrame(Renderer* renderer);
void __real_CtrRenderer_endFrame(Renderer* renderer);
void __wrap_CtrRenderer_endFrame(Renderer* renderer);
CtrBenchFrame __real_CtrBench_beginFrameTimed(u32 down, double seconds);
CtrBenchFrame __wrap_CtrBench_beginFrameTimed(u32 down, double seconds);
u32 __real_hidKeysHeld(void);
u32 __wrap_hidKeysHeld(void);
void __real_RunnerKeyboard_onKeyDown(RunnerKeyboardState* kb, int32_t key);
void __wrap_RunnerKeyboard_onKeyDown(RunnerKeyboardState* kb, int32_t key);
void __real_RunnerKeyboard_onKeyUp(RunnerKeyboardState* kb, int32_t key);
void __wrap_RunnerKeyboard_onKeyUp(RunnerKeyboardState* kb, int32_t key);
void __real_Runner_step(Runner* runner);
void __wrap_Runner_step(Runner* runner);
InputRecording* __real_InputRecording_createPlayer(const char* path, const char* record);
InputRecording* __wrap_InputRecording_createPlayer(const char* path, const char* record);
void __real_InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frame);
void __wrap_InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frame);
bool __real_CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner);
bool __wrap_CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner);
void __real__exit(int status) __attribute__((noreturn));
void __wrap__exit(int status) __attribute__((noreturn));

CODE static void inputlateFinish(void) {
    blockPolledKeys = inputlateInFrame = false;
    CtrInput_stop();
    CtrInput_getStats(&stats);
    if (game != nullptr) {
        logInfo(stopped, stats.core, (unsigned long) stats.samples, (unsigned long) stats.drains,
                (unsigned long) stats.retries, (unsigned long) stats.overruns,
                (unsigned long) stats.mainTakeovers, (unsigned long) stats.workerReturns,
                (unsigned long) stats.retryMisses, (unsigned long) stats.mainDrains,
                (double) stats.elapsedTicks / CPU_TICKS_PER_MSEC, (double) stats.maxGapTicks / CPU_TICKS_PER_MSEC);
    }
    game = nullptr;
    gameRenderer = nullptr;
}

CODE void __wrap_CtrBench_sessionStarted(const CtrBenchSession* session) {
    inputlateFinish();
    game = session->runner;
    gameRenderer = session->renderer;
    previousKeys = 0;
    stats = (CtrInputStats) {0};
    menuOpen = menuWasOpen = benchWasActive = benchPaused = suppressHeld = false;
    if (!CtrInput_start(session->isNew3ds, &session->display.controls)) {
        logError(startFailed);
        abort();
    }
    __real_CtrBench_sessionStarted(session);
}

CODE void __wrap_CtrRenderer_beginFrame(Renderer* renderer) {
    if (renderer == gameRenderer && game != nullptr) {
        blockPolledKeys = false;
        inputlateInFrame = true;
        menuWasOpen = menuOpen;
        benchWasActive = CtrBench_pickerOpen() || CtrBench_running();
    }
    __real_CtrRenderer_beginFrame(renderer);
}

CODE void __wrap_CtrRenderer_endFrame(Renderer* renderer) {
    if (renderer == gameRenderer) blockPolledKeys = inputlateInFrame = false;
    CTR_ENDFRAME_AFTER_INPUTLATE(renderer);
}

CODE u32 __wrap_hidKeysHeld(void) {
    u32 held = __real_hidKeysHeld();
    // main reads held outside the begin/end bracket after setupNotices, to
    // suppress the dismissing button. The sampler never calls this API.
    if (game != nullptr && !inputlateInFrame) suppressHeld = true;
    return held;
}

CODE CtrBenchFrame __wrap_CtrBench_beginFrameTimed(u32 down, double seconds) {
    CtrBenchFrame result = __real_CtrBench_beginFrameTimed(down, seconds);
    benchPaused = result == BENCH_FRAME_PAUSE;
    return result;
}

// Screens remains at its original address; only its downstream call changes.
CODE void CTR_DISPLAY_INPUTLATE(Renderer* renderer, const CtrDisplaySettings* settings) {
    __real_CtrRenderer_setDisplay(renderer, settings);
    if (game == nullptr || renderer != gameRenderer || !inputlateInFrame) return;
    bool enabled = !menuOpen && !benchPaused && !menuWasOpen && !benchWasActive && !CtrBench_running() && !playback;
    CtrInputEdges input = CtrInput_take(&settings->controls, __real_hidKeysHeld(), hidKeysDown(), enabled, suppressHeld, nullptr);
    suppressHeld = false;
    blockPolledKeys = false;
    sendInput(game, input, &previousKeys);
    // From setDisplay to Runner_step/endFrame the only onKeyDown/Up calls
    // for this keyboard are main's inlined, frame-polled adapter. Replay
    // disarms this guard before processing its recording. Game events run
    // after Runner_step disarms it, including simulated keyboard events.
    blockPolledKeys = true;
}

CODE void __wrap_RunnerKeyboard_onKeyDown(RunnerKeyboardState* kb, int32_t key) {
    if (!blockPolledKeys || game == nullptr || kb != game->keyboard) __real_RunnerKeyboard_onKeyDown(kb, key);
}
CODE void __wrap_RunnerKeyboard_onKeyUp(RunnerKeyboardState* kb, int32_t key) {
    if (!blockPolledKeys || game == nullptr || kb != game->keyboard) __real_RunnerKeyboard_onKeyUp(kb, key);
}
CODE void __wrap_Runner_step(Runner* runner) {
    blockPolledKeys = false;
    CTR_STEP_AFTER_INPUTLATE(runner);
}
CODE InputRecording* __wrap_InputRecording_createPlayer(const char* path, const char* record) {
    InputRecording* result = __real_InputRecording_createPlayer(path, record);
    playback = result != nullptr && result->isPlayback && !result->playbackEnded;
    return result;
}
CODE void __wrap_InputRecording_processFrame(InputRecording* recording, RunnerKeyboardState* kb, int frame) {
    blockPolledKeys = false;
    __real_InputRecording_processFrame(recording, kb, frame);
    // Completion belongs to the recording object, which main keeps calling
    // even after its last frame. Live input resumes on the following step.
    playback = recording != nullptr && recording->isPlayback && !recording->playbackEnded;
}

CODE void CTR_MENUOPEN_INPUTLATE(CtrMenu* menu, PrintConsole* console, const CtrDisplaySettings* settings, CtrDisplayCaps caps) {
    menuOpen = true;
    CtrMenu_open(menu, console, settings, caps);
}
CODE void CTR_MENUCLOSE_INPUTLATE(CtrMenu* menu) {
    menuOpen = false;
    suppressHeld = true;
    CtrMenu_close(menu);
}

CODE bool __wrap_CtrChapters_takeGameChange(CtrGameSession* session, Runner* runner) {
    inputlateFinish();
    return __real_CtrChapters_takeGameChange(session, runner);
}
CODE void CTR_WATCHDOGSTOP_INPUTLATE(void) {
    inputlateFinish();
    CTR_WATCHDOGSTOP_AFTER_INPUTLATE();
}
CODE const char* CTR_HBLDR_INPUTLATE(const char* path, const char* const* args, int count) {
    // Lab recovery cannot join a possibly faulted thread or wait for its lock.
    CtrInput_requestStop();
    return CTR_HBLDR_AFTER_INPUTLATE(path, args, count);
}

CODE void __wrap__exit(int status) {
    // Every exceptional leave reaches newlib _exit, including argcrash's
    // HBLDR early return. Stop before libctru unmaps threadCreate's heap stack.
    // Self/faulted joins are bounded; the dedicated sleep guard covers those.
    CtrInput_stop();
    CTR_EXIT_AFTER_INPUTLATE(status);
}
