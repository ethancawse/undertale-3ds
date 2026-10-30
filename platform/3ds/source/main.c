#include "main_fatalflush_layout.inc"
// the port 3DS entry point and main loop.
//
// Mirrors the frame order of engine/src/loop.c (desktop) and src/ps2/main.c:
// input -> Runner_step -> draw pre/views/post/GUI -> clear input edges ->
// audio -> pending room change -> present.
//
// The game renders to the top screen through the citro3d renderer. The bottom
// screen shows the text magnifier (drawn by the renderer), nothing, or live
// stats and the log; SELECT opens the options menu there.

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"
#include "data_win.h"
#include "gettime.h"
#include "input_recording.h"
#include "load_trace.h"
#include "overlay_file_system.h"
#include "profiler.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "sandbox_file_system.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"
#include "vm_surface_readers.h"
#include "cli/state_hash.h"
#define CTRPORT_CTR_ARGUI_REDIRECT
#include "ctr_aot_module.h"
#include "ctr_audio_system.h"
#include "ctr_benchmark.h"
#include "ctr_boot_trail.h"
#include "ctr_chapters.h"
#include "ctr_file_system.h"
#include "ctr_frame_pacer.h"
#include "ctr_gpu_queue.h"
#include "ctr_heap_profile.h"
#include "ctr_lab.h"
#include "ctr_lab_proto.h"
#include "ctr_launch_args.h"
#include "ctr_language.h"
#include "ctr_link_files.h"
#include "ctr_input_combo.h"
#include "ctr_input_map.h"
#include "ctr_log.h"
#include "ctr_exit.h"
#include "ctr_loadtime.h"
#include "ctr_startup_clock.h"
#include "ctr_memory.h"
#include "ctr_apt_trace.h"
#include "ctr_menu.h"
#include "ctr_notice.h"
#include "ctr_data_version.h"
#include "ctr_safe_save.h"
#include "ctr_open_files.h"
#include "ctr_renderer.h"
#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "ctr_texture_pack_screen.h"
#include "ctr_watchdog.h"
#include "ctr_video.h"
#include "ctr_build_id.h"       // CTRPORT_COMMIT
#include "ctrport_build_id.h" // CTRPORT_COMMIT_TEXT, generated on every build
#include "ctr_release_branding.h"
#include "ctr_fps_clock.h"
#line 72
#include "drawskip_txt.h"       // bench/drawskip_<game>.txt: deep frame skip's Draw classes

#define GAME_DIR CTRPORT_GAME_DIR

// The build ID's one definition (ctr_build_id.h): a leading NUL, the ID, NUL padding to a fixed size.
_Static_assert(sizeof(CTRPORT_COMMIT_TEXT) + 1 <= CTRPORT_BUILD_ID_SIZE, "build ID longer than CTRPORT_BUILD_ID_SIZE");
const char ctrportBuildId[CTRPORT_BUILD_ID_SIZE] = "\0" CTRPORT_COMMIT_TEXT;
#define SAVE_DIR GAME_DIR "/saves"
#define LOG_PATH GAME_DIR "/log.txt"
#define HANG_REPORT_PATH GAME_DIR "/hang.txt"
// Lab mode's crash and hang reports (ctr_lab.h).
#define CRASH_REPORT_PATH GAME_DIR "/crash.txt"
#define LAB_HANG_SECONDS 90
// Lab mode: the steps of the start, up to the log being open (ctr_boot_trail.h), and how long they may take.
#define BOOT_TRAIL_PATH GAME_DIR "/labboot.txt"
#define LAB_BOOT_SECONDS 60
// How long the exit may take in lab mode before it counts as a hang.
#define LAB_EXIT_SECONDS 40
#define SETTINGS_PATH GAME_DIR "/settings.ini"
// Test hooks for automated runs: a list of frame numbers to screenshot, and a
// recorded input file in the desktop runner's --playback-inputs JSON format.
#define AUTOSHOT_PATH GAME_DIR "/autoshot.txt"
// "<frame> <iterations>": the options menu opens at that frame, before its step, and stays open for that many
// iterations of the main loop; the last one's screens are saved with the suffix "p" (top_<frame>p.bmp), to check that
// the paused frame shows as it did. For tests: scheduling a screenshot at frame - 1 gives the frame before the pause.
#define AUTOPAUSE_PATH GAME_DIR "/autopause.txt"
// Frames whose GPU work profile builds log (see CtrRenderer_traceNextFrame).
#define GPU_TRACE_PATH GAME_DIR "/gputrace.txt"
// Frames the texture probe checks every tile and palette copy at, negated to list the frame's batches too (see
// CtrRenderer_probeNextFrame). A diagnostic: the file's presence turns texel hashing on.
#define TEX_PROBE_PATH GAME_DIR "/texprobe.txt"
#define AUTOPLAY_PATH GAME_DIR "/autoplay.json"
// The random generator starts at the seed this file holds (0 when it's empty) and randomize() keeps it (as
// --fixed-seed does): random choices repeat from run to run while the clock, and so every measurement, stays real.
// statehash.txt fixes the order too, but through a virtual clock.
#define FIXED_SEED_PATH GAME_DIR "/fixedseed.txt"
// Starts the performance test at boot with the scenes it lists (ctr_benchmark.h).
#define BENCHRUN_PATH GAME_DIR "/benchrun.txt"
// When this file exists, the GML profiler runs and its top scripts are logged
// with every [perf] line.
#define PROFILE_PATH GAME_DIR "/profile.txt"
// If present, every frame's game state and draw calls are hashed into
// STATE_HASH_OUT_PATH, as the desktop runner's --state-hash does (state_hash.c),
// so two runs of the same input playback can be compared frame by frame.
#define STATE_HASH_PATH GAME_DIR "/statehash.txt"
#define STATE_HASH_OUT_PATH GAME_DIR "/state_hash.log"
// Test hook: holds frame ends up at random, or turns the wait for the last
// flip off, to stress the screen transfers' gate (CtrPacer_loadStress).
#define PACE_STRESS_PATH GAME_DIR "/pacestress.txt"
// Present: the renderer counts the text box's pixels per frame (CtrRenderer_loadFlickerProbe).
#define FLICKER_PROBE_PATH GAME_DIR "/flickerprobe.txt"

// Names a self-test to run instead of the game, as --selftest does (ctr_launch_args.h), for an emulator, which passes
// no launch arguments. The file is left in place: remove it to play.
#define SELFTEST_PATH GAME_DIR "/selftest.txt"

// Turns on the debug path --force-materialize does (ctr_launch_args.h), for an emulator.
#define FORCE_MATERIALIZE_PATH GAME_DIR "/forcematerialize.txt"
// Test: the frame to set the Drawing setting to Full at (--drawing-full-at).
#define DRAWING_FULL_AT_PATH GAME_DIR "/drawingfullat.txt"
// Test: the frames the 3D slider goes up and down at (--slider-at).
#define SLIDER_AT_PATH GAME_DIR "/sliderat.txt"
// Test: forget the objects that read surfaces back (--no-surface-readers).
#define NO_SURFACE_READERS_PATH GAME_DIR "/nosurfacereaders.txt"
// Deep frame skip's Draw classes in place of the built-in list.
#define DRAWSKIP_PATH GAME_DIR "/drawskip.txt"

// libctru splits the application memory region between these two heaps at
// startup. Exposed so the boot log can show how much each one got.
extern u32 __ctru_heap;
extern u32 __ctru_heap_size;
extern u32 __ctru_linear_heap_size;
extern u32 __ctru_linear_heap;

static PrintConsole statsConsole;
static PrintConsole logConsole;
static PrintConsole menuConsole;
enum { STATS_CONSOLE_ROWS = 7, BOTTOM_CONSOLE_ROWS = 30 };

// Launch arguments (ctr_launch_args.h): only the network loader passes any.
static CtrLaunchArgs launch;
// A remote run (--exit-after or --timeout) has nobody at the console, so screens that wait for a button go away
// by themselves.
static bool unattended;
#define UNATTENDED_SCREEN_SECONDS 10
// Setup notices never block a test: with any of --bench (or its SD form, benchrun.txt), --exit-after, --timeout,
// --lab, --shots or an autoshot.txt they are logged and not shown (setupNotices).
static bool noticesUnattended;
// Set when a remote run is over: the main loop leaves as SELECT+START would.
static bool remoteQuit;

// Real milliseconds, for the run's own time limits (--timeout, the screens above). nowNanos() won't do for them:
// statehash.txt and --statehash switch it to the virtual clock, which starts again at 1 s, moves one frame per game
// step and stands still between steps.
static uint64_t realMs(void) {
    return (uint64_t) (svcGetSystemTick() / CPU_TICKS_PER_MSEC);
}

// ===[ Input ]===

// Passes the keys the held buttons send (ctr_input_map.h, the options menu's Controls page) to the game as key presses
// and releases. keysMask holds the keys last passed (bit k for CtrGameKey k).
static void sendInput(Runner* runner, u32 held, const CtrInputMap* controls, u32* keysMask) {
    u32 keys = CtrInputMap_keys(controls, held);
    u32 previous = *keysMask;
    *keysMask = keys;
    for (int32_t k = GAME_KEY_NONE + 1; GAME_KEY_ARROWS > k; k++) {
        u32 bit = 1u << k;
        if ((keys & bit) != 0 && (previous & bit) == 0) RunnerKeyboard_onKeyDown(runner->keyboard, CtrInputMap_keyCode((CtrGameKey) k));
        if ((keys & bit) == 0 && (previous & bit) != 0) RunnerKeyboard_onKeyUp(runner->keyboard, CtrInputMap_keyCode((CtrGameKey) k));
    }
}

// ===[ Status output ]===





static void logMemory(const char* stage) {
    CtrHeapProfile_dump(stage, 40);
    struct mallinfo mi = mallinfo();
    logInfo("[mem] %s: heap used %.1f MB (peak %.1f) of %.1f MB, linear free %.1f MB of %.1f MB\n", stage,
            (double) mi.uordblks / (1024.0 * 1024.0), (double) mi.arena / (1024.0 * 1024.0), (double) __ctru_heap_size / (1024.0 * 1024.0),
            (double) linearSpaceFree() / (1024.0 * 1024.0), (double) __ctru_linear_heap_size / (1024.0 * 1024.0));
    flushMemoryStatus();
}

static double msSince(uint64_t startNanos) {
    return (double) (nowNanos() - startNanos) / 1000000.0;
}

// Once the VM has patched the bytecode, only the scripts that run need to be in memory: the rest goes to
// cache/bytecode.bin beside data.win (written on the first boot, checked on later ones) and each script is read
// back the first time it runs. A DELTARUNE chapter runs about a fifth of its 6-9 MB of bytecode.
// Reading code back costs an SD read the first time each part of it runs, and real SD cards can stall for seconds
// behind a save, so the bytecode only moves when the memory it sits in is short.
// It is loaded into linear memory when that has room (CtrMemory_allocTemporary), and moving it then frees linear
// memory for textures but costs heap, since the scripts read back go to the heap: in Chapter 5 the heap peak is
// 4 MB higher with the bytecode moved. So bytecode in linear memory only moves when the texture budget would be
// small; bytecode on the heap moves when the heap is short (DELTARUNE's 44 MB, not UNDERTALE's 61 MB).
#define BYTECODE_KEEP_FREE_HEAP (32u * 1024u * 1024u)
#define BYTECODE_KEEP_FREE_LINEAR (40u * 1024u * 1024u)
// ===[ Deep frame skip ]===

// How deep a skipped frame skips: frame_skip_deep, and at least 1 for fixed 2:1.
// Auto's levels (CtrRenderer_setFrameSkipLevels): 0 not used, 1 the pacer picks, 2 fixed at frameSkipLevel's level.
static int32_t frameSkipLevels(const CtrDisplaySettings* display) {
    if (display->frameSkip == FRAME_SKIP_AUTO && display->frameSkipAuto == 1) return 1;
    return display->frameSkip == FRAME_SKIP_20 || display->frameSkip == FRAME_SKIP_15 ? 2 : 0;
}

static int32_t frameSkipLevel(const CtrDisplaySettings* display) {
    return display->frameSkip == FRAME_SKIP_20 ? 1 : display->frameSkip == FRAME_SKIP_15 ? 2 : 0;
}

static int32_t drawSkipDepth(const CtrDisplaySettings* display) {
    if ((display->frameSkip == FRAME_SKIP_HALF || frameSkipLevels(display) != 0) && 1 > display->frameSkipDeep) return 1;
    return display->frameSkip == FRAME_SKIP_OFF ? 0 : display->frameSkipDeep;
}

// The Draw classes for this data.win: drawskip.txt in the game folder if there is one, else the built-in list
// (bench/drawskip_<game>.txt). Its section is the chapter folder's name up to the underscore (chapter3_windows:
// chapter3).
static void loadDrawSkip(Runner* runner, const char* dataWinPath, bool temporalMuted) {
    char* list = nullptr;
    const char* from = DRAWSKIP_PATH;
    FILE* f = fopen(DRAWSKIP_PATH, "rb");
    if (f != nullptr) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        list = safeCalloc((size_t) (size > 0 ? size : 0) + 1, 1);
        size_t got = fread(list, 1, (size_t) (size > 0 ? size : 0), f);
        list[got] = '\0';
        fclose(f);
    } else {
        from = "the built-in list";
        list = safeMalloc(drawskip_txt_size + 1);
        memcpy(list, drawskip_txt, drawskip_txt_size);
        list[drawskip_txt_size] = '\0';
    }
    char section[32] = "";
    const char* chapter = strstr(dataWinPath, "chapter");
    if (chapter != nullptr) {
        size_t n = strcspn(chapter, "_/");
        if (n >= sizeof(section)) n = sizeof(section) - 1;
        memcpy(section, chapter, n);
        section[n] = '\0';
    }
    int32_t named = Runner_loadDrawSkip(runner, list, section[0] != '\0' ? section : nullptr, temporalMuted);
    free(list);
    logInfo("Frame skip: Draw classes from %s, section %s: %d objects named, temporal %s, self %s\n", from,
            section[0] != '\0' ? section : "-", (int) named, temporalMuted ? "muted" : "run", runner->drawSkip.skipSelf ? "left out" : "drawn");
}

static void moveBytecodeToCache(DataWin* dataWin, const char* dataWinPath) {
    uintptr_t at = (uintptr_t) dataWin->bytecodeBuffer;
    bool inLinear = __ctru_linear_heap <= at && __ctru_linear_heap + __ctru_linear_heap_size > at;
    struct mallinfo mi = mallinfo();
    uint32_t freeBytes = inLinear ? linearSpaceFree() : __ctru_heap_size - (uint32_t) mi.uordblks;
    if (freeBytes >= (inLinear ? BYTECODE_KEEP_FREE_LINEAR : BYTECODE_KEEP_FREE_HEAP)) {
        logInfo("Bytecode: kept in memory (%.1f MB %s free)\n", (double) freeBytes / (1024.0 * 1024.0), inLinear ? "linear" : "heap");
        return;
    }
    const char* slash = strrchr(dataWinPath, '/');
    int dirLength = slash != nullptr ? (int) (slash - dataWinPath) : 0;
    char path[256];
    int written = snprintf(path, sizeof(path), "%.*s/cache", dirLength, dataWinPath);
    if (0 > written || written >= (int) sizeof(path) - 16) return;
    mkdir(path, 0777);
    strcat(path, "/bytecode.bin");
    uint64_t t0 = nowNanos();
    if (DataWin_moveBytecodeToFile(dataWin, path)) {
        logInfo("Bytecode: %.1f MB moved to %s in %.0f ms\n", (double) dataWin->codeStats.totalBytes / (1024.0 * 1024.0), path,
                msSince(t0));
    } else {
        logWarn("Bytecode: could not use %s, keeping it in memory\n", path);
    }
}

// The consoles draw into the bottom framebuffer with the CPU; nothing else
// touches that screen, so write it back for the LCD once per frame.
static void flushBottomScreen(void) {
    u16 w, h;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    GSPGPU_FlushDataCache(fb, (u32) w * h * 2);
}

// Dialogue on the bottom screen (ctr_renderer.c) draws over the consoles'
// framebuffer, and once a screenshot has turned double buffering back on
// (CtrPacer_shownFramebuffer) its frames flip the bottom screen's buffers, so
// the buffer on screen may not be the one the consoles print on. When the
// dialogue goes, the consoles move to the buffer on screen, which stays
// single; it gets what the menu or the test already printed (keepText), or
// is cleared of the last line (the stats repaint within a second).
static void reclaimBottomForConsoles(bool keepText) {
    CtrLog_beginConsole();
    u16 w = 0, h = 0;
    u16* shown = (u16*) CtrPacer_shownFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    if (shown != nullptr && gfxGetScreenFormat(GFX_BOTTOM) == GSP_RGB565_OES) {
        size_t bytes = (size_t) w * h * 2u;
        if (!keepText) memset(shown, 0, bytes);
        else if (statsConsole.frameBuffer != shown) memcpy(shown, statsConsole.frameBuffer, bytes);
        statsConsole.frameBuffer = logConsole.frameBuffer = menuConsole.frameBuffer = shown;
        GSPGPU_FlushDataCache(shown, (u32) bytes);
    }
    CtrLog_endConsole();
}

// Only entered for a newly reported problem. The renderer keeps the top
// screen's last frame; no game frame or playback input advances here.
static void setupNotices(Renderer* renderer) {
    if (noticesUnattended) {
        // As if A were pressed, without remembering the dismissal: an attended boot still shows the notice.
        while (CtrNotice_pending) {
            logWarn("Setup notice not shown (unattended run), continuing: %s\n", CtrNotice_first());
            CtrNotice_dismiss(false);
        }
        return;
    }
    CtrLog_setConsoleEnabled(false);
    if (renderer != nullptr) CtrRenderer_yieldBottomScreen(renderer, true);
    CtrLog_beginConsole();
    fflush(stdout);
    consoleSelect(&menuConsole);
    while (CtrNotice_pending) {
        bool dismissed = false;
        while (aptMainLoop()) {
            CtrWatchdog_mark("setup notice");
            if (renderer != nullptr) CtrRenderer_beginFrame(renderer);
            // Paint after the renderer applies its screen configuration, and
            // repaint after HOME/sleep or an outstanding earlier transfer.
            consoleSelect(&menuConsole);
            consoleClear();
            printf("\x1b[1;1HSetup needs attention\n\n");
            const char* text = CtrNotice_first();
            for (size_t i = 0; text[i] != '\0'; i++) {
                putchar(text[i]);
                if (i % 38 == 37) putchar('\n');
            }
            printf("\n\n%s\n\nPress A to continue.\n", CtrNotice_advice());
            fflush(stdout);
            hidScanInput();
            u32 down = hidKeysDown();
            if (renderer != nullptr) CtrRenderer_endFrame(renderer);
            flushBottomScreen();
            if (renderer == nullptr) gspWaitForVBlank();
            if (down & KEY_A) {
                if (!CtrNotice_dismiss(true)) logWarn("Notice: could not remember dismissal in " SETTINGS_PATH "\n");
                dismissed = true;
                break;
            }
        }
        if (!dismissed) break;
    }
    consoleClear();
    CtrLog_endConsole();
}

static void saveFailureWarning(bool fullScreen) {
    CtrLog_beginConsole();
    consoleSelect(&menuConsole);
    if (fullScreen) {
        consoleClear();
        printf("\x1b[2;1H\x1b[1;31mSaving failed\x1b[0m\n\n"
               "Your latest progress may not be saved.\n\n"
               "Check the SD card and free space.\n"
               "Previous saves are at their normal\n"
               "names or in saves/*.bak.\n\n"
               "Copy saves/ to your PC before repairs.\n"
               "This warning stays until you quit.\n\n"
               "SELECT: options  SELECT+START: quit\n");
    } else {
        // Keep the warning visible while options or benchmark UI owns the rest.
        printf("\x1b[24;1H\x1b[1;31mSAVE FAILED: check SD card/free space\x1b[0m\x1b[K");
    }
    fflush(stdout);
    consoleSelect(&logConsole);
    CtrLog_endConsole();
}

// While a game runs, for the error screens to stop its threads and save.
static Runner* liveRunner;
static FileSystem* liveSaves;

static void exitStep(const char* step) {
    logInfo("Exit: %s\n", step);
    CtrExit_flush();
}

// After an error screen: shuts down what is still running, as the normal exit
// does (threads and NDSP left running can keep the console from getting back
// to the menu), then exits.
static __attribute__((noreturn)) void exitAfterScreen(void) {
    CtrLab_setExitNote("error");
    CtrExit_begin(LAB_EXIT_SECONDS);
    // The console draws into framebuffers that gfxExit frees.
    CtrLog_setConsoleEnabled(false);
    CtrWatchdog_stop();
    CtrAptTrace_stop();
    Runner* runner = liveRunner;
    liveRunner = nullptr;
    if (liveSaves != nullptr) {
        exitStep("writing saves");
        CtrFileSystem_flush(liveSaves);
    }
    if (runner != nullptr) {
        exitStep("closing the video");
        Video_closeAll(runner);
        exitStep("stopping audio");
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = nullptr;
        exitStep("stopping texture work");
        CtrRenderer_stopBackgroundWork(runner->renderer);
    }
    exitStep("stopping the GPU");
    C3D_Fini();
    if (runner != nullptr) {
        exitStep("restoring the top screen");
        CtrRenderer_leaveForExit(runner->renderer);
    }
    exitStep("closing graphics");
    gfxExit();
    exitStep("done");
    CtrExit_closeFilesLog();
    exit(1);
}

static __attribute__((noinline, section(".fatalflush")))
void fatalSummary(const char* fmt, va_list va) {
    char summary[1024];
    const char prefix[] = "[ERROR] Fatal: ";
    memcpy(summary, prefix, sizeof(prefix) - 1);
    vsnprintf(summary + sizeof(prefix) - 1, sizeof(summary) - sizeof(prefix), fmt, va);
    // The screen's detail can span lines; the emergency reason is one line.
    size_t n = strlen(summary);
    for (size_t i = 0; i < n; i++) if (summary[i] == '\n' || summary[i] == '\r') summary[i] = ' ';
    summary[n++] = '\n';
    summary[n] = '\0';
    CtrLog_emergencyKeepLink(summary, 2000);
}

// Shows an error and waits for the user to exit with START.
static __attribute__((noreturn, noinline, section(".fatalflush"))) void fatalScreen(const char* fmt, ...) {
    va_list va;
    va_start(va, fmt);
    va_list summaryArgs;
    va_copy(summaryArgs, va);
    fatalSummary(fmt, summaryArgs);
    va_end(summaryArgs);
    // Waiting for START isn't a hang. Log before joining/freeing any thread.
    CtrWatchdog_stop();
    vLogError(fmt, va);
    va_end(va);
    CtrLog_flush();

    // The log console's lines print first; nothing prints there after this.
    CtrLog_beginConsole();
    consoleSelect(&statsConsole);
    consoleClear();
    printf("\x1b[1;31mCannot continue.\x1b[0m See below or\n" LOG_PATH "\nPress START to exit.\n");
    consoleSelect(&logConsole);
    uint64_t shownAt = realMs();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        if (unattended && realMs() - shownAt > UNATTENDED_SCREEN_SECONDS * 1000ULL) break;
        flushBottomScreen();
        gspWaitForVBlank();
    }
    exitAfterScreen();
}

// The engine has no way back from an allocation it can't do without (safeMalloc
// and the like abort). The Old 3DS's .3dsx gets 64 MB, the CIA 80, and on the
// .3dsx DELTARUNE runs out in places, so say how to play on rather than show
// the log's bare FATAL line.
static __attribute__((noinline, section(".fatalflush")))
void oomSummary(size_t wanted) {
    static bool entered;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&entered, &expected, true, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (CtrLab_active()) CtrLab_fail("oom", "another out-of-memory screen is already active");
        svcExitProcess();
    }
    int32_t heapCached, linear, vram;
    CtrMemory_emergencyFree(&heapCached, &linear, &vram);
    char summary[256];
    snprintf(summary, sizeof(summary), "[ERROR] Out of memory: wanted=%lu B heap_free_cached=%ld B linear_free=%ld B vram_free=%ld B (-1=unavailable)\n",
             (unsigned long) wanted, (long) heapCached, (long) linear, (long) vram);
    // Even stopping the watchdog joins/frees a thread. Send the reason before
    // any cleanup, heap inspection or console work can block or fault.
    CtrLog_emergencyKeepLink(summary, 2000);
}

static __attribute__((noreturn, noinline, section(".fatalflush"))) void outOfMemoryScreen(size_t wanted) {
    oomSummary(wanted);
    CtrWatchdog_stop();
    CtrMemory_logHeapLayout("out of memory", 0);
    CtrLog_flush();
    CtrLog_setConsoleEnabled(false);
    CtrLog_beginConsole();
    consoleSelect(&menuConsole);
    consoleClear();
    printf("\x1b[8;0H  \x1b[1;31mOut of memory\x1b[0m\n\n");
    // DELTARUNE's .3dsx gets about 62 MB in all on an Old 3DS, its CIA about 78; UNDERTALE fits 64 MB (no CIA hint).
    if (CTR_OOM_CIA_HINT_BELOW > __ctru_heap_size + __ctru_linear_heap_size) {
        printf("  " CTRPORT_GAME_TITLE " needs more memory here than\n");
        printf("  a .3dsx gets on an Old 3DS.\n\n");
        printf("  Install the Old 3DS CIA, which gets\n");
        printf("  80 MB, and start the game from the\n");
        printf("  HOME Menu. Saves are shared.\n\n");
    } else {
        printf("  " CTRPORT_GAME_TITLE " ran out of memory.\n");
        printf("  Details are in\n  " LOG_PATH "\n\n");
    }
    printf("  Press START to exit.");
    uint64_t shownAt = realMs();
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        if (unattended && realMs() - shownAt > UNATTENDED_SCREEN_SECONDS * 1000ULL) break;
        flushBottomScreen();
        gspWaitForVBlank();
    }
    exitAfterScreen();
}

#ifndef NDEBUG
static __attribute__((noinline, section(".fatalflush")))
void forceSelfTestOom(void) {
    uint8_t* probe = nullptr;
    arrsetcap(probe, 128u * 1024u * 1024u);
    arrfree(probe);
}
#endif

void __real_abort(void) __attribute__((noreturn));
void __wrap_abort(void) __attribute__((noreturn));
void __wrap_abort(void) {
    size_t wanted = CtrMemory_failedAllocation();
    if (wanted != 0) outOfMemoryScreen(wanted);
    if (CtrLab_active()) CtrLab_fail("abort", "abort() was called (a failed assert, or the engine giving up)");
    __real_abort();
}

// The game's strings, kept for its whole run in one block of up to 4 MB
// (DELTARUNE Chapter 5): from the heap, or linear memory when an earlier
// chapter left the heap without a free block that large (it did on an Old 3DS
// in 64 MB mode, switching from Chapter 4 to 5 with 0.2 MB of it in use).
static void* allocStringsBuffer(size_t size) {
    void* p = CtrMemory_allocWork(size);
    if (p == nullptr) outOfMemoryScreen(size);
    return p;
}

// game_end closes the game as on PC: main returns, which takes a
// .3dsx back to the Homebrew Launcher and a CIA to the HOME Menu (see the exit below).
// UNDERTALE's notice that said so first is gone (ENABLE_WAD16 is UNDERTALE's build). Its
// code in main is nop padding of the same size, run once at the exit, and the padding
// names the notice's strings, so that main, everything after it and the merged string
// pool keep their addresses.
#if defined(ENABLE_WAD16)
#if defined(CTRPORT_TARGET_O3DS)
#define GAME_ENDED_NOPS "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
#else
#define GAME_ENDED_NOPS "nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop"
#endif
#define GAME_ENDED_SLOT(ended) \
    if ((ended) && !unattended) \
        __asm__ volatile(GAME_ENDED_NOPS :: "i"("\x1b[11;0H"), "i"("  " CTRPORT_GAME_TITLE " closed itself, as it"), \
                         "i"("  does on PC.\n"), "i"("  Start it again to continue.\n\n"), \
                         "i"("              \x1b[1;36mPress A\x1b[0m"), "i"("game ended notice"))
#define GAME_ENDED_TAIL() __asm__ volatile("" :: "i"("Exiting at frame %d%s\n"), "i"("save flush"))
#else
#define GAME_ENDED_SLOT(ended) do { } while (0)
#define GAME_ENDED_TAIL() do { } while (0)
#endif
#line 599

// Keep these call sites and their stack use the same size as stat/fopen.
// Cache implementation lives after warm text; no per-frame observer is added.



static bool fileExists(const char* path) {
    struct stat st;
    return bootHookStat(path, &st) == 0;
}

static void dataVersionProgress(void) {
    CtrWatchdog_mark("checking data.win version");
}

static void checkDataVersion(const CtrGameSession* session) {
#ifdef ENABLE_WAD16
    const char* game = "undertale";
#else
    const char* game = "deltarune";
#endif
    const char* part = "root";
    if (strcmp(session->bundleDir, GAME_DIR) != 0) {
        const char* slash = strrchr(session->bundleDir, '/');
        part = slash == nullptr ? session->bundleDir : slash + 1;
    }
    char cachePath[1024];
    snprintf(cachePath, sizeof(cachePath), "%s/cache", session->bundleDir);
    mkdir(cachePath, 0777);
    snprintf(cachePath, sizeof(cachePath), "%s/cache/data-version.txt", session->bundleDir);
    char digest[65];
    bool cached;
    logInfo("Checking data.win version: %s\n", session->dataWinPath);
    flushVersionStatus();
    u64 started = svcGetSystemTick();
    CtrDataVersionResult result = CtrDataVersion_check(session->dataWinPath, game, part, cachePath,
                                                      dataVersionProgress, &cached, digest);
    logInfo("Data version: %s, %s, %.1f ms, SHA-256 %s\n", result == CTR_DATA_KNOWN ? "known" : "unverified",
            cached ? "cached" : "hashed", (double) (svcGetSystemTick() - started) / CPU_TICKS_PER_MSEC, digest);
    if (result == CTR_DATA_KNOWN) return;
    char kind[100];
    if (result == CTR_DATA_UNKNOWN) snprintf(kind, sizeof(kind), "Unknown data.win (%s)", digest);
    else snprintf(kind, sizeof(kind), "Could not verify data.win");
    logWarn("%s: %s; continuing is allowed\n", kind, session->dataWinPath);
    CtrNotice_report(kind, session->dataWinPath,
                     "This file version has not been checked.\nIt may work; you can continue.\nIf it fails, verify your PC game files\nand copy data.win again.");
    if (CtrNotice_pending) setupNotices(nullptr);
}

// statehash.txt in the game folder, or --statehash.
static bool stateHashWanted(void) {
    return launch.stateHash || fileExists(STATE_HASH_PATH);
}

// Preserve fileExists' inlined stat call and stack use in main. The launch
// argument check lives after warm text, alongside the video fix.
static int __attribute__((noipa, no_reorder, section(".vidfix"))) videoStateHashStat(const char* path, struct stat* st);

static inline __attribute__((always_inline)) bool videoStateHashWanted(const char* path) {
    struct stat st;
    return videoStateHashStat(path, &st) == 0;
}

// Reads whitespace-separated frame numbers. Returns an stb_ds array.
static int32_t* loadAutoshotFrames(const char* path) {
    int32_t* frames = nullptr;
    FILE* f = openBootHook(path, "r");
    if (f == nullptr) return nullptr;
    long value;
    while (fscanf(f, "%ld", &value) == 1) arrput(frames, (int32_t) value);
    fclose(f);
    return frames;
}

// citro3d's command buffer (in linear memory) holds a whole frame's GPU
// commands; when it nearly fills, the renderer has the GPU run the frame so
// far and starts it over, a stall (see flushBatch in ctr_renderer.c). Depth
// mode draws every batch twice, so busy rooms get close. New 3DS builds can
// spare 1 MB; Old 3DS keeps 512 KB of its tighter linear heap.
#ifdef CTRPORT_TARGET_O3DS
#define GPU_CMDBUF_BYTES 0x80000
#else
#define GPU_CMDBUF_BYTES 0x100000
#endif

// The launch arguments' --ini settings, on top of settings.ini. Only the second call (log = true) reports them.
static void applyIniOverrides(CtrDisplaySettings* settings, CtrDisplayCaps caps, bool log) {
    for (int i = 0; launch.iniCount > i; i++) {
        bool known = CtrDisplay_override(settings, launch.ini[i], caps);
        if (!log) continue;
        if (known) logInfo("Remote: setting %s for this run\n", launch.ini[i]);
        else logWarn("Remote: --ini %s is not a setting this build knows (or the value is out of range)\n", launch.ini[i]);
    }
}

#ifdef CTRPORT_FAKE_3DSLINK
// Emulator tests of remote runs (tools/remote/run3ds.sh): Azahar starts a .3dsx without the Homebrew Launcher, so
// there are no arguments and no 3dslink host. A build configured with
//   -DCMAKE_C_FLAGS='-DCTRPORT_FAKE_3DSLINK="\"--bench ruins --exit-after\""'
// acts as if 3dslink on 127.0.0.1 had started it with those arguments.
#include <arpa/inet.h>
static char fakeArgsText[] = CTRPORT_FAKE_3DSLINK;
static char* fakeArgv[32];
static int fakeArgs(char** argv0) {
    int argc = 0;
    fakeArgv[argc++] = argv0 != nullptr ? *argv0 : (char*) "3dslink:/fake.3dsx";
    for (char* p = strtok(fakeArgsText, " "); p != nullptr && argc < 32; p = strtok(nullptr, " ")) fakeArgv[argc++] = p;
    __3dslink_host.s_addr = htonl(0x7F000001);
    return argc;
}
#endif

// Lab mode (ctr_lab.h): only the lab host asks for it (--lab-host), for unattended runs.
static CtrLabConfig labConfig;
static void startLab(void) {
    labConfig = (CtrLabConfig) {
        .appName = ctrReleaseAppName,
        .buildId = CTRPORT_COMMIT,
        .hostPath = launch.labHost[0] != '\0' ? launch.labHost : LAB_HOST_DEFAULT,
        .reportPath = CRASH_REPORT_PATH,
        .hangSeconds = launch.labHangSet ? launch.labHangSeconds : LAB_HANG_SECONDS,
        // The main loop leaves at --timeout itself; this is for a run stuck where it doesn't look.
        .deadlineSeconds = launch.timeoutSeconds > 0 ? launch.timeoutSeconds + 60 : 0,
        .logTail = CtrLog_tail,
        .emergencyLog = CtrLog_emergency,
        .progress = CtrWatchdog_progress,
        .watching = CtrWatchdog_watching,
        .phase = CtrWatchdog_phase,
        .threadName = CtrWatchdog_threadNameOf,
        .hangDetails = CtrWatchdog_lastReport,
        .bootSeconds = LAB_BOOT_SECONDS,
        .bootStep = CtrBootTrail_last,
        .trail = CtrBootTrail_mark,
    };
    CtrBootTrail_open(BOOT_TRAIL_PATH);
    CtrBootTrail_mark("lab mode: starting the guard");
    CtrLab_start(&labConfig);
}

static uint64_t loadTicks(void) { return svcGetSystemTick(); }
static double loadMs(uint64_t start) { return (double) (loadTicks() - start) * 1000.0 / (double) SYSCLOCK_ARM11; }

int main(int argc, char** argv) {
    uint64_t appEnteredAt = loadTicks();
    uint64_t mainEnteredUs = CtrStartup_elapsedUs();
    CtrOpenFiles_track();
    // Audio streams, cache files, saves and the log are open at once.
    CtrMemory_reserveFileSlots(48);
#ifdef CTRPORT_FAKE_3DSLINK
    argc = fakeArgs(argc > 0 ? argv : nullptr);
    argv = fakeArgv;
#endif
    // First, so lab mode catches crashes from the start; the log reports them later.
    bool launchArgs = CtrLaunchArgs_parse(&launch, argc, argv);
    // Without lab mode the trail isn't open and CtrBootTrail_mark only notes the step.
    if (launch.lab) startLab();
    CtrBootTrail_mark("gfxInitDefault");
    gfxInitDefault();
    CtrBootTrail_mark("C3D_Init");
    C3D_Init(GPU_CMDBUF_BYTES);

    // Bottom screen (40x30 cells): stats on top, log below.
    CtrBootTrail_mark("consoleInit");
    consoleInit(GFX_BOTTOM, &statsConsole);
    consoleInit(GFX_BOTTOM, &logConsole);
    // libctru window positions are 1-based, just like its cursor positions.
    consoleSetWindow(&statsConsole, 1, 1, 40, STATS_CONSOLE_ROWS);
    consoleSetWindow(&logConsole, 1, STATS_CONSOLE_ROWS + 1, 40, BOTTOM_CONSOLE_ROWS - STATS_CONSOLE_ROWS);
    consoleInit(GFX_BOTTOM, &menuConsole);
    consoleSelect(&logConsole);

    CtrBootTrail_mark("APT_CheckNew3DS");
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    // New 3DS: 804 MHz and the L2 cache. No-op on Old 3DS.
    CtrBootTrail_mark("osSetSpeedupEnable");
    osSetSpeedupEnable(true);

    CtrBootTrail_mark("mkdir");
    mkdir("sdmc:/3ds", 0777);
    { extern const char ctrSdLegacyDir[]; extern int CtrSdMigrate_run(const char*, int); CtrSdMigrate_run(ctrSdLegacyDir, 0777); }
    mkdir(GAME_DIR, 0777);
    mkdir(SAVE_DIR, 0777);
    CtrBootTrail_mark("CtrLog_openFile");
    CtrExitFiles_openLog(LOG_PATH);
    // Launched by the network loader: the log goes to the PC as well, from the first line.
    CtrBootTrail_mark("CtrLog_startLink");
    // Before the link starts, which then makes room for sending them (ctr_link_files.h).
    int sendFilesMB = CtrLaunchArgs_sendFilesMB(&launch);
    if (sendFilesMB > 0) CtrLinkFiles_enable((uint32_t) sendFilesMB << 20);
    bool linked = CtrLog_startLink();
    unattended = launch.exitAfter || launch.timeoutSeconds > 0 || launch.lab;
    noticesUnattended = unattended || launch.bench[0] != '\0' || launch.shots || fileExists(BENCHRUN_PATH) ||
                        fileExists(AUTOSHOT_PATH) || launch.autoshotCount > 0;
    uint64_t launchedAtMs = realMs();
    CtrBootTrail_mark(linked ? "CtrWatchdog_start (log linked)" : "CtrWatchdog_start (no log link)");
    CtrWatchdog_start(HANG_REPORT_PATH);
    CtrBootTrail_mark("booted: the log is open");
    CtrLab_booted();

    CtrLog_printTo(&statsConsole, ctrReleaseBootFormat, CTRPORT_GAME_TITLE, CTRPORT_COMMIT,
                   isNew3ds ? "New 3DS" : "Old 3DS");

    logInfo(ctrReleaseLogFormat, CTRPORT_GAME_TITLE, CTRPORT_COMMIT);
    logInfo("Startup clock: early runtime to main %.3f ms (libctru %.3f ms, app services/constructors %.3f ms); "
            "main to log %.3f ms; OS launch/executable load/BSS clear precede clock origin\n",
            (double) mainEnteredUs / 1000.0, (double) CtrStartup_libctruUs() / 1000.0,
            (double) (mainEnteredUs - CtrStartup_libctruUs()) / 1000.0, loadMs(appEnteredAt));
    logInfo("Console: %s, application memory %.1f MB\n", isNew3ds ? "New 3DS" : "Old 3DS",
            (double) osGetMemRegionSize(MEMREGION_APPLICATION) / (1024.0 * 1024.0));
#ifdef CTRPORT_TARGET_N3DS
    if (!isNew3ds) {
        fatalScreen(ctrReleaseWrongModelFormat,
                    strrchr(GAME_DIR, '/') + 1);
    }
#endif
    // Where the kernel put linear memory: 0x30000000 and up for a .3dsx and the n3ds .cia (exheader kernel 2.44+),
    // the old 0x14000000 mapping for an older exheader. The New 3DS video decoder needs the new one (ctr_video.c).
    logInfo("Memory: heap at 0x%08lX (%.1f MB), linear at 0x%08lX (%.1f MB, %s mapping)\n", (unsigned long) __ctru_heap,
            (double) __ctru_heap_size / (1024.0 * 1024.0), (unsigned long) __ctru_linear_heap,
            (double) __ctru_linear_heap_size / (1024.0 * 1024.0), __ctru_linear_heap >= 0x30000000u ? "new" : "old");
    if (linked) logInfo("Remote: started by 3dslink from %s; the log goes there too\n", CtrLog_linkHost());
    if (launchArgs) {
        char described[512];
        CtrLaunchArgs_describe(&launch, described, sizeof(described));
        logInfo("Remote: launch arguments %s\n", described);
        if (launch.problems > 0) logWarn("Remote: %s\n", launch.messages);
    }
    if (sendFilesMB > 0 && linked) {
        logInfo("Remote: the files this run writes (screenshots, state_hash.log) go to the PC when it ends, up to %d MB\n",
                sendFilesMB);
        CtrLog_setExitProgress(CtrLab_exitProgress);
    } else if (sendFilesMB > 0) {
        logWarn("Remote: not started by 3dslink, so the run's files stay on the SD card\n");
    }
    if (CtrLab_active()) {
        logInfo("Lab: crashes and hangs (no progress for %d s) end the run with a report in %s, then %s takes over\n",
                labConfig.hangSeconds, CRASH_REPORT_PATH, labConfig.hostPath);
        if (!CtrLab_hostReady()) logWarn("Lab: no lab host at %s: the console will go back to the Homebrew Launcher\n", labConfig.hostPath);
        if (launch.labCrash[0] != '\0') {
            logWarn("Lab: crashing on purpose (--lab-crash %s)\n", launch.labCrash);
            CtrLog_flush();
            CtrWatchdog_mark("lab crash test");
            CtrLab_crashTest(launch.labCrash);
        }
    }
    CtrMemory_markGameThread();
    CtrHeapProfile_init();
    logMemory("startup");
    // The game's clock then moves one frame per frame, so time-based GML
    // repeats exactly. The timings in the log read 0.
    if (stateHashWanted()) GetTime_enableVirtualClock();

    // DELTARUNE's game_change() swaps data.win files; each one starts again
    // here. Input playback carries on across the switch.
    CtrGameSession session;
    CtrNotice_init(SETTINGS_PATH);
    CtrChapters_init(&session, GAME_DIR);
    InputRecording* playback = nullptr;
    int32_t inputFrame = 0;
    uint64_t loadEnteredAt = appEnteredAt;
    bool dialogueWasShown = false; // the bottom screen showed dialogue last frame (reclaimBottomForConsoles)
startGame:
    CtrLog_setFrameTimed(false);
    if (!fileExists(session.dataWinPath)) {
        if (strcmp(session.bundleDir, GAME_DIR) != 0) {
            logWarn("Missing chapter: %s; returning to launcher\n", session.dataWinPath);
            CtrNotice_missing("Missing chapter", session.dataWinPath);
            if (CtrNotice_pending) setupNotices(nullptr);
            CtrChapters_returnToLauncher(&session, GAME_DIR);
            goto startGame;
        }
        fatalScreen("data.win not found at %s\n", session.dataWinPath);
    }
    CtrPack_setDataWinPath(session.dataWinPath);
    checkDataVersion(&session);

    // Read diagnostics before parsing; applying these settings still happens at
    // the original point after parsing. No display or loading policy changes.
    CtrDisplaySettings early;
    CtrDisplay_defaults(&early);
    CtrDisplay_load(&early, SETTINGS_PATH, (CtrDisplayCaps) { true, true });
    applyIniOverrides(&early, (CtrDisplayCaps) { true, true }, false);
    logInfo("Startup phase: load=%u entry-to-parse %.3f ms\n", (unsigned) session.loadCount, loadMs(loadEnteredAt));

    // ===[ Parse data.win ]===
    uint64_t t0 = nowNanos();
    DataWinParserOptions options = {0};
    options.parseGen8 = true;
    options.parseOptn = true;
    options.parseLang = true;
    options.parseExtn = true;
    options.parseSond = true;
    options.parseAgrp = true;
    options.parseSprt = true;
    options.parseBgnd = true;
    options.parsePath = true;
    options.parseScpt = true;
    options.parseGlob = true;
    options.parseShdr = true;
    options.parseFont = true;
    options.parseTmln = true;
    options.parseObjt = true;
    options.parseRoom = true;
    options.parseTpag = true;
    options.parseCode = true;
    options.parseVari = true;
    options.parseFunc = true;
    options.parseStrg = true;
    options.parseTxtr = true;
    options.parseAudo = true;
    options.skipLoadingPreciseMasksForNonPreciseSprites = true;
    options.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
    options.lazyLoadRooms = true;
    options.lazyLoadTextures = true;
    options.lazyLoadAudio = true;
    options.unbufferedParse = true;
    options.allocChunkBuffer = CtrMemory_allocTemporary;
    options.freeChunkBuffer = CtrMemory_freeWork;
    options.bytecodeInChunkMemory = true;
    options.allocStringsBuffer = allocStringsBuffer;
    options.freeStringsBuffer = CtrMemory_freeWork;

    logInfo("Parsing %s...\n", session.dataWinPath);
    flushParseStatus();
    CtrBootTrail_mark("parsing data.win");
    if (early.loadTrace) LoadTrace_begin("parse", loadTicks, SYSCLOCK_ARM11);
    uint64_t loadStart = loadTicks();
    DataWin* dataWin = DataWin_parse(session.dataWinPath, options);
    double parseWallMs = loadMs(loadStart);
    double parseGameMs = msSince(t0);
    LoadTrace_end(CtrLog_flush);
    Gen8* gen8 = &dataWin->gen8;
    logInfo("Loaded \"%s\" in %.0f ms [WAD %u, GameMaker %u.%u.%u.%u]\n", gen8->displayName, parseGameMs, gen8->wadVersion,
            dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build);
    logInfo("Startup phase: load=%u data-win %.3f ms\n", (unsigned) session.loadCount, parseWallMs);
    logMemory("after data.win");

    bool wadSupported = false;
#ifdef ENABLE_WAD16
    if (gen8->wadVersion == 15 || gen8->wadVersion == 16) wadSupported = true;
#endif
#ifdef ENABLE_WAD17
    if (gen8->wadVersion == 17) wadSupported = true;
#endif
    if (!wadSupported) {
        fatalScreen("This build is for %s, but data.win has bytecode version %u.\n", CTRPORT_GAME_TITLE, gen8->wadVersion);
    }
    // Only the texture settings matter this early (whether preparation may use
    // the system core, exact colours) and the open-file limits. The console's
    // display capabilities come later.
    CtrPackWorker_setUseSyscore(early.prepareSyscore);
    CtrOpenFiles_setLimit(early.openFilesMax, early.openFilesFakeMax);
    // Exact colours have their own cache files; the setting is read at each
    // data.win load, so a change in the menu applies from the next one.
    CtrPack_setExact(early.exactColours);
    CtrPack_setTestRun((unsigned) early.textureCacheRun, early.cachePublishDirect);
    if (early.textureCacheRun > 0) {
        char directory[CTR_PACK_PATH_MAX];
        CtrPack_cachePath(directory, sizeof(directory), "");
        logInfo("Textures: test cache %s (existing pages reused; never deletes the normal cache)\n", directory);
    }
    logInfo("Textures: publication %s (flushed temporary files, then rename)\n", early.cachePublishDirect ? "SD archive" : "stdio reference");
    if (!CtrPack_lossy()) logInfo("Textures: exact colours (exact_colours = 1; cache files in cache_exact)\n");
    else if (early.exactColours) logInfo("Textures: rounded colours (exact colours are for the New 3DS only)\n");
    else logInfo("Textures: rounded colours\n");
    if (early.sdWriteTest) CtrPack_sdWriteTest();
    if (early.roomMemoryReset) CtrTextures_forgetRoomMemory();
    if (early.repackTextures) CtrPack_forgetCache(dataWin->txtr.count);
    loadStart = loadTicks();
    CtrPackScreen_run(dataWin);
    logInfo("Startup phase: load=%u texture-preparation %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));

    // The window size is what the game lays itself out for. Games made for a
    // 640x480 window render 320x240 views scaled up 2x but draw some text at
    // half scale, so the application surface keeps the full window size and
    // the renderer filters it down to the top screen.
    int32_t windowW = (int32_t) gen8->defaultWindowWidth;
    int32_t windowH = (int32_t) gen8->defaultWindowHeight;
    int32_t appW = windowW;
    int32_t appH = windowH;
    while (appW > 1024 || appH > 1024) {
        appW /= 2;
        appH /= 2;
    }

    // ===[ Engine objects ]===
#ifdef CTRPORT_AOT_MODULES
    // This chapter's generated code, in place of the last one's (the last VMContext is gone by now).
    loadStart = loadTicks();
    CtrAotModule_load(session.dataWinPath);
    logInfo("Startup phase: load=%u aot-total %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));
#endif
    t0 = nowNanos();
    loadStart = loadTicks();
    VMContext* vm = VM_create(dataWin);
    logInfo("Startup phase: load=%u vm %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));
    logInfo("VM created in %.0f ms\n", msSince(t0));
    // Before the bytecode goes to the cache file: reads all of it.
    t0 = nowNanos();
    int32_t surfaceReaderCount = 0;
    bool* surfaceReaders = VM_findSurfaceReaders(vm, &surfaceReaderCount);
    logInfo("VM: %d objects read surfaces back (found in %.0f ms)\n", (int) surfaceReaderCount, msSince(t0));
    loadStart = loadTicks();
    moveBytecodeToCache(dataWin, session.dataWinPath);
    logInfo("Startup phase: load=%u bytecode-cache %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));
    // Input playback is for reproducible test runs, so keep randomize() from reseeding: the run then matches a
    // desktop run of the same recording with --seed 0 (random encounters, attack patterns).
    if (fileExists(AUTOPLAY_PATH)) vm->hasFixedSeed = true;
    // For A/B performance runs of scenes with random choices (a battle's attack order): both builds then play the same.
    // The launch option's seed wins over the file's.
    uint32_t randomSeed = 0;
    bool fixedSeedFile = fileExists(FIXED_SEED_PATH);
    if (launch.fixedSeed || fixedSeedFile) {
        vm->hasFixedSeed = true;
        if (launch.fixedSeed) {
            randomSeed = launch.fixedSeedValue;
        } else {
            FILE* f = fopen(FIXED_SEED_PATH, "r");
            unsigned long seed = 0;
            if (f != nullptr) {
                if (fscanf(f, "%lu", &seed) != 1) seed = 0;
                fclose(f);
            }
            randomSeed = (uint32_t) seed;
        }
        logInfo("Fixed random seed %u (%s)\n", (unsigned) randomSeed, launch.fixedSeed ? "--fixed-seed" : FIXED_SEED_PATH);
    }
    bool profileFile = fileExists(PROFILE_PATH);
    // Step only (--profile=step, or a profile.txt that starts with "step"): the profiler is parked while the frame
    // draws, so the report holds what the Step half of the frame spends and nothing of the Draw half.
#ifdef ENABLE_VM_GML_PROFILER
    // Azahar cannot supply homebrew argv. Keep its private test hook off hardware;
    // console diagnostics use --knightdiag and require no SD flag file.
    s64 knightEmulator = 0;
    if (!launch.knightDiagnostic && R_SUCCEEDED(svcGetSystemInfo(&knightEmulator, 0x20000, 0)) && knightEmulator != 0 &&
        fileExists(GAME_DIR "/knightdiag.txt")) launch.knightDiagnostic = true;
    bool profileStepOnly = launch.profileStepOnly;
    if (profileFile && profileStepOnly == false) {
        FILE* f = fopen(PROFILE_PATH, "r");
        char word[8] = { 0 };
        if (f != nullptr) {
            if (fgets(word, sizeof(word), f) != nullptr && strncmp(word, "step", 4) == 0) profileStepOnly = true;
            fclose(f);
        }
    }
    if (launch.profile || launch.knightDiagnostic || profileFile) {
        Profiler_setEnabled(&vm->profiler, true);
        logInfo("GML profiler on (%s%s%s)%s\n", launch.knightDiagnostic ? "--knightdiag" : launch.profile ? "--profile" : "",
                (launch.profile || launch.knightDiagnostic) && profileFile ? " and " : "", profileFile ? PROFILE_PATH : "",
                profileStepOnly ? ", Step only" : "");
    }
#else
    const bool profileStepOnly = false; // no profiler to park
    if (launch.profile || launch.knightDiagnostic || profileFile) {
        Profiler_setEnabled(&vm->profiler, true);
        logInfo("GML profiler on (%s%s%s)\n", launch.knightDiagnostic ? "--knightdiag" : launch.profile ? "--profile" : "",
                (launch.profile || launch.knightDiagnostic) && profileFile ? " and " : "",
                profileFile ? PROFILE_PATH : "");
    }
#endif

    // The game's files go through a sandbox the performance test switches on (ctr_benchmark.h); it shows the game
    // folder's own files through bundleFs while it is on.
    loadStart = loadTicks();
    FileSystem* saveFs = CtrFileSystem_create(session.bundleDir, SAVE_DIR);
    liveSaves = saveFs;
    OverlayFileSystem* bundleFs = OverlayFileSystem_create(session.bundleDir, session.bundleDir);
    FileSystem* fileSystem = SandboxFileSystem_create(saveFs, (FileSystem*) bundleFs);
    logInfo("Startup phase: load=%u filesystem %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));
    loadStart = loadTicks();
    Renderer* renderer = CtrRenderer_create(saveFs, appW, appH, GAME_DIR);
    logInfo("Startup phase: load=%u renderer %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));
    CtrPacer_loadStress(PACE_STRESS_PATH);
    if (launch.noSurfaceReaders || fileExists(NO_SURFACE_READERS_PATH)) {
        free(surfaceReaders);
        surfaceReaders = nullptr;
        logInfo("Renderer: objects that read surfaces back are not kept full (%s)\n",
                launch.noSurfaceReaders ? "--no-surface-readers" : NO_SURFACE_READERS_PATH);
    }
    CtrRenderer_setSurfaceReaders(renderer, surfaceReaders, dataWin->objt.count); CtrSurfaceCensus_install(renderer);
    if (launch.forceMaterialize || fileExists(FORCE_MATERIALIZE_PATH)) {
        CtrRenderer_setForceMaterialize(renderer, true);
        logInfo("Renderer: every surface starts recorded and is materialized at its first draw (%s)\n",
                launch.forceMaterialize ? "--force-materialize" : FORCE_MATERIALIZE_PATH);
    }
    if (!CtrRenderer_loadFlickerProbe(renderer, FLICKER_PROBE_PATH) && launch.flickerProbe[0] != '\0')
        CtrRenderer_startFlickerProbe(renderer, launch.flickerProbe, "--flicker-probe");
    CtrDisplaySettings display;
    CtrDisplay_defaults(&display);
    CtrDisplay_load(&display, SETTINGS_PATH, CtrRenderer_getDisplayCaps(renderer));
    applyIniOverrides(&display, CtrRenderer_getDisplayCaps(renderer), session.loadCount == 1);
    CtrBtnprompt_setDisplay(renderer, &display);
    logInfo("Display: top %s, drawing %s, 3D %s, smoothing %s, bottom %s\n", CtrDisplay_topModeName(display.topMode),
            CtrDisplay_drawingName(display.drawing), CtrDisplay_stereoModeName(display.stereoMode), CtrDisplay_smoothingName(display.smoothing),
            CtrDisplay_bottomModeName(display.bottomMode));
    if (!CtrInputMap_isDefault(&display.controls)) {
        CtrInputMap defaults;
        CtrInputMap_defaults(&defaults);
        for (int32_t row = 0; MAP_ROW_COUNT > row; row++) {
            if (display.controls.keys[row] == defaults.keys[row]) continue;
            logInfo("Controls: %s sends %s\n", CtrInputMap_rowName((CtrMapRow) row), CtrInputMap_keyName((CtrGameKey) display.controls.keys[row]));
        }
    }
    logInfo("Frame skip: %s\n", display.frameSkip == FRAME_SKIP_AUTO ? (display.frameSkipMax > 1 ? "auto, up to 2 in a row" : "auto")
                                : display.frameSkip == FRAME_SKIP_HALF ? "2:1"
                                                                       : "off");
    CtrPacer_levelReset();
    if (frameSkipLevels(&display) != 0) logInfo("Frame skip: levels (%s)\n", frameSkipLevels(&display) == 1 ? "auto" : CtrDisplay_frameSkipName(display.frameSkip));
    if (display.frameSkip != FRAME_SKIP_OFF) {
        logInfo("Frame skip: deep %d, policy %d\n", (int) drawSkipDepth(&display), (int) display.frameSkipPolicy);
        if (display.frameSkip == FRAME_SKIP_HALF && display.frameSkipOverlap) logInfo("Frame skip: overlap\n");
        if (!display.frameSkipRecord) logInfo("Frame skip: record off\n");
    }
    loadStart = loadTicks();
    AudioSystem* audioSystem = (AudioSystem*) CtrAudioSystem_create();
    CtrAudioSystem_setDataWinPath((CtrAudioSystem*) audioSystem, session.dataWinPath);
    logInfo("Startup phase: load=%u audio %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));

    loadStart = loadTicks();
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, randomSeed);
    logInfo("Startup phase: load=%u runner %.3f ms\n", (unsigned) session.loadCount, loadMs(loadStart));
    runner->osType = OS_WINDOWS;
    snprintf(runner->osLanguage, sizeof(runner->osLanguage), "%s", CtrLanguage_systemCode());
    logInfo("System language: %s\n", runner->osLanguage);
    loadDrawSkip(runner, session.dataWinPath, display.frameSkipDeep == 2);
    liveRunner = runner;
    Runner_setGameArgs(runner, session.gameArgs, (int32_t) arrlen(session.gameArgs));
    logMemory("after runner");
    // After the renderer and NDSP have hooked APT and the DSP, so its hooks run first.
    CtrAptTrace_init();
    CtrWatchdog_watch(runner);
    // Before the first room, so a chapter the test starts never sees the player's saves.
    CtrBenchSession benchSession = {
        .runner = runner,
        .renderer = renderer,
        .sandbox = fileSystem,
        .saves = saveFs,
        .bundleDir = session.bundleDir,
        .gameDir = GAME_DIR,
        .gameArgs = session.gameArgs,
        .display = display,
        .isNew3ds = isNew3ds,
    };
    CtrBench_sessionStartedLate(&benchSession);

    // A fixed seed makes a bench reproducible without changing normal play's
    // clock. State-hash mode also covers direct room/input replay tests.
    bool frameClockBench = (launch.fixedSeed || fixedSeedFile) &&
        (CtrBench_running() || launch.bench[0] != '\0' || fileExists(BENCHRUN_PATH));
    CtrVideo_setFrameClock(videoStateHashWanted(STATE_HASH_PATH) || (frameClockBench && !CtrBench_returnedToGame()) ? runner : nullptr);

    t0 = nowNanos();
    if (stateHashWanted() && fileExists(STATE_HASH_OUT_PATH ".trace-enable"))
        StateHash_traceBegin(STATE_HASH_OUT_PATH ".trace", renderer);
    if (early.loadTrace) LoadTrace_begin("first-room", loadTicks, SYSCLOCK_ARM11);
    loadStart = loadTicks();
    Runner_initFirstRoom(runner);
    double firstRoomWallMs = loadMs(loadStart);
    double firstRoomGameMs = msSince(t0);
    LoadTrace_end(CtrLog_flush);
    logInfo("First room \"%s\" ready in %.0f ms\n", runner->currentRoom->name, firstRoomGameMs);
    logInfo("Startup phase: load=%u first-room %.3f ms\n", (unsigned) session.loadCount, firstRoomWallMs);
    logMemory("after first room");
    CtrLog_setFrameTimed(true);

    // Screenshots are for the test: the game it goes back to afterwards, counting frames from 0 again, would
    // save its own over them.
    int32_t* autoshotFrames = CtrBench_returnedToGame() ? nullptr : loadAutoshotFrames(AUTOSHOT_PATH);
    if (autoshotFrames != nullptr) logInfo("Screenshots scheduled for %d frames (%s)\n", (int) arrlen(autoshotFrames), AUTOSHOT_PATH);
    if (!CtrBench_returnedToGame() && launch.autoshotCount > 0) {
        for (int i = 0; launch.autoshotCount > i; i++) arrput(autoshotFrames, (int32_t) launch.autoshotFrames[i]);
        logInfo("Screenshots scheduled for %d frames (--autoshot)\n", launch.autoshotCount);
    }
    int32_t* autopause = CtrBench_returnedToGame() ? nullptr : loadAutoshotFrames(AUTOPAUSE_PATH);
    if (arrlen(autopause) >= 2) logInfo("Menu scheduled at frame %d for %d iterations (%s)\n", (int) autopause[0], (int) autopause[1], AUTOPAUSE_PATH);
    int32_t autopauseLeft = -1; // iterations the scheduled menu stays open; -1 before it opens, 0 after
    int32_t* traceFrames = CtrBench_returnedToGame() ? nullptr : loadAutoshotFrames(GPU_TRACE_PATH);
    if (traceFrames != nullptr) logInfo("GPU traces scheduled for %d frames (%s)\n", (int) arrlen(traceFrames), GPU_TRACE_PATH);
    int32_t drawingFullAt = launch.drawingFullAt;
    if (drawingFullAt == 0 && !CtrBench_returnedToGame()) {
        int32_t* frames = loadAutoshotFrames(DRAWING_FULL_AT_PATH);
        if (arrlen(frames) > 0) drawingFullAt = frames[0];
        arrfree(frames);
    }
    if (drawingFullAt > 0) logInfo("Drawing set to Full at frame %d\n", (int) drawingFullAt);
    int32_t sliderUpAt = launch.sliderUpAt, sliderDownAt = launch.sliderDownAt;
    if (sliderUpAt == 0 && !CtrBench_returnedToGame()) {
        int32_t* frames = loadAutoshotFrames(SLIDER_AT_PATH);
        if (arrlen(frames) > 0) sliderUpAt = frames[0];
        if (arrlen(frames) > 1 && frames[1] > sliderUpAt) sliderDownAt = frames[1];
        arrfree(frames);
    }
    if (CtrBench_returnedToGame()) sliderUpAt = sliderDownAt = 0;
    if (sliderUpAt > 0) logInfo("3D slider up at frame %d, down at %d (test)\n", (int) sliderUpAt, (int) sliderDownAt);
    CtrRenderer_setSliderOverride(renderer, -1.0f);
    int32_t* probeFrames = CtrBench_returnedToGame() ? nullptr : loadAutoshotFrames(TEX_PROBE_PATH);
    if (probeFrames != nullptr) {
        CtrRenderer_probeTextures(renderer);
        logInfo("Texture probe on, %d frames (%s)\n", (int) arrlen(probeFrames), TEX_PROBE_PATH);
    }
    if (!CtrBench_returnedToGame() && launch.gpuTraceCount > 0) {
        for (int i = 0; launch.gpuTraceCount > i; i++) arrput(traceFrames, (int32_t) launch.gpuTraceFrames[i]);
        logInfo("GPU traces scheduled for %d frames (--gputrace)\n", launch.gpuTraceCount);
    }
    if (session.loadCount == 1 && !CtrBench_running() && launch.bench[0] != '\0') {
        // Nobody is there to leave the results screen, so it goes by itself ("done").
        char words[LAUNCH_ARGS_BENCH_MAX + 16];
        snprintf(words, sizeof(words), "%s done%s", launch.bench, launch.shots ? " shots" : "");
        if (!CtrBench_autorunList(words, "--bench", &menuConsole, &display) && launch.exitAfter) {
            logWarn("Remote: no test to run, leaving (--exit-after)\n");
            remoteQuit = true;
        }
    } else if (session.loadCount == 1 && !CtrBench_running() && fileExists(BENCHRUN_PATH)) {
        CtrBench_autorun(BENCHRUN_PATH, &menuConsole, &display);
    }
    char selftest[sizeof(launch.selftest)];
    snprintf(selftest, sizeof(selftest), "%s", launch.selftest);
    FILE* selftestFile = selftest[0] == 0 ? openBootHook(SELFTEST_PATH, "r") : nullptr;
    if (selftestFile != nullptr) {
        if (fscanf(selftestFile, "%23s", selftest) != 1) selftest[0] = 0;
        fclose(selftestFile);
    }
    if (session.loadCount == 1 && selftest[0] != 0) {
        // Before the game's first frame; the run ends after it.
#ifndef NDEBUG
        if (strcmp(selftest, "oom") == 0) {
            // Diagnostic build only: one impossible growth, through stb_ds's
            // real checked realloc and the ordinary fatal path. No frame hook.
            forceSelfTestOom();
        }
#endif
        if (!CtrRenderer_selfTest(renderer, selftest)) logWarn("Remote: no self-test named '%s'\n", selftest);
        logInfo("Remote: self-test %s over, leaving\n", selftest);
        remoteQuit = true;
    }
    if (playback == nullptr && fileExists(AUTOPLAY_PATH)) {
        playback = InputRecording_createPlayer(AUTOPLAY_PATH, nullptr);
        logInfo("Playing back input from %s\n", AUTOPLAY_PATH);
    }

    bool stateHash = stateHashWanted();
    bool firstFramePending = true;
    int32_t lastPresentedRoom = -1;
    if (stateHash) {
        StateHash_begin(STATE_HASH_OUT_PATH, renderer);
        CtrLinkFiles_note(STATE_HASH_OUT_PATH);
        logInfo("Hashing every frame's state into %s\n", STATE_HASH_OUT_PATH);
    }
#ifdef CTRPORT_OFFDRAW
    { extern void CtrOffdraw_init(Runner*, const char*); CtrOffdraw_init(runner, GAME_DIR "/offdraw.txt"); }
#endif
#line 1244

    // ===[ Main loop ]===
    CtrBootTrail_mark("the main loop");
    u32 heldKeys = 0;      // as last passed to the game (sendInput)
    u32 ignoredButtons = 0; // held when the menu closed; kept from the game until released
    CtrInputCombo inputCombo = {0};
    CtrChordGuard chordGuard = {0}; // L as a game button stays out of L+SELECT
    CtrMenu menu = {0};
    CtrDisplaySettings displayAtMenuOpen = display; // what the menu's edits are saved against
    uint64_t lastFrameStart = nowNanos();
    uint64_t statsWindowStart = lastFrameStart;
    double stepMsSum = 0.0, drawMsSum = 0.0, stepMsMax = 0.0, drawMsMax = 0.0;
    int32_t statsFrames = 0;
    int32_t statsWindows = 0;
#ifdef CTRPORT_TYPED_FRAMELOG
    static uint16_t ftStep[128], ftDraw[128];
    int ftCount = 0, ftFirst = 0;
#endif
    int32_t lastRoomIndex = -1;
    uint32_t frameRate = 0;

    while (aptMainLoop() && !runner->shouldExit && !CtrChapters_changeRequested(runner)) { FW_MARK(0);
        if (CtrNotice_pending && !menu.open && !CtrBench_pickerOpen()) {
            setupNotices(renderer);
            ignoredButtons = hidKeysHeld();
            // A notice's time is not a game frame's delta time.
            lastFrameStart = nowNanos();
            statsWindowStart = lastFrameStart;
            stepMsSum = drawMsSum = stepMsMax = drawMsMax = 0.0;
            statsFrames = 0;
        }
        if (launch.videoSuspendSeconds && runner->frameCount >= (int) launch.videoSuspendFrame) {
            CtrVideo_debugSuspend(runner, launch.videoSuspendSeconds);
            launch.videoSuspendSeconds = 0;
        }
        // In lab mode, stops here for good when another thread crashed.
        if (!FW_SKIP(8)) CtrLab_poll();
        // The game speed the game set with game_set_speed() or room_speed, else
        // the room's, as on the desktop runner.
        uint32_t roomSpeed = (uint32_t) (Runner_getEffectiveGameSpeed(runner) + 0.5);
        if (roomSpeed != frameRate && roomSpeed > 0) {
            frameRate = roomSpeed;
            CtrRenderer_setFrameRate(renderer, (float) roomSpeed);
            logInfo("Game speed: %u fps\n", (unsigned) roomSpeed);
        }

        // A frame that starts late may go unshown so the game keeps its
        // speed, but never one a scheduled screenshot will show, nor while the
        // menu or the test's scene list is up.
        // Fixed 2:1 skips the odd frames, so a scheduled screenshot (an even frame) lines up with a shown frame.
        bool mayskip = display.frameSkip != FRAME_SKIP_OFF && !menu.open && !CtrBench_pickerOpen();
        for (ptrdiff_t i = 0; mayskip && arrlen(autoshotFrames) > i; i++) {
            if (autoshotFrames[i] == runner->frameCount) mayskip = false;
        }
        for (ptrdiff_t i = 0; arrlen(traceFrames) > i; i++) {
            if (traceFrames[i] != runner->frameCount) continue;
            mayskip = false;
            CtrRenderer_traceNextFrame(renderer);
        }
        for (ptrdiff_t i = 0; arrlen(probeFrames) > i; i++) {
            if (probeFrames[i] != runner->frameCount && -probeFrames[i] != runner->frameCount) continue;
            mayskip = false;
            CtrRenderer_probeNextFrame(renderer, 0 > probeFrames[i]);
        } FW_MARK(16);
        CtrRenderer_setFrameSkip(renderer, mayskip && (display.frameSkip == FRAME_SKIP_AUTO || frameSkipLevels(&display) != 0) ? display.frameSkipMax : 0);
        CtrRenderer_setFrameSkipLevels(renderer, frameSkipLevels(&display), frameSkipLevel(&display));
        CtrRenderer_setFrameSkipPolicy(renderer, display.frameSkipPolicy);
        CtrRenderer_setFrameSkipOverlap(renderer, display.frameSkip == FRAME_SKIP_HALF && display.frameSkipOverlap);
        CtrRenderer_setFrameSkipRecord(renderer, display.frameSkipRecord != 0);
        CtrRenderer_setFrameSkipKeepApp(renderer, drawSkipDepth(&display) == 3);
        CtrRenderer_setHalfRate(renderer, display.frameSkip == FRAME_SKIP_HALF, mayskip && (runner->frameCount & 1) != 0);

        // Waits for the frame slot (vsync paced to the room speed).
        CtrWatchdog_mark("wait for frame"); FW_MARK(1);
        CtrRenderer_beginFrame(renderer); FW_MARK(2);
        bool frameSkipped = CtrRenderer_frameSkipped(renderer);

        // A test's screenshot taken after the wait belongs to this frame, not the one before (ctr_renderer.h).
        uint64_t frameStart = nowNanos() - (uint64_t) (CtrRenderer_surfaceShotMs(renderer) * 1e6);
        runner->deltaTime = (double) (frameStart - lastFrameStart) / 1000.0;
        lastFrameStart = frameStart;

        hidScanInput();
        u32 held = hidKeysHeld();
        u32 down = hidKeysDown();
        u32 gameHeld = CtrChordGuard_filter(&chordGuard, held, down, CtrInputMap_sendsFromL(&display.controls));
        // The performance test and picker own their input while active.
        bool benchActive = CtrBench_pickerOpen() || CtrBench_running();
        CtrInputComboAction combo = CtrInputCombo_step(&inputCombo, held, down, hidKeysUp());
        if (benchActive || menu.open) inputCombo.selectAlone = false;
        if (combo.quit) {
            CtrRenderer_endFrame(renderer);
            break;
        }
        if (combo.screenshot) CtrRenderer_requestPlayerScreenshot(renderer);
        bool openMenu = !menu.open && !benchActive && combo.menu;
        if (openMenu) {
            CtrLog_setConsoleEnabled(false);
            displayAtMenuOpen = display;
            CtrMenu_open(&menu, &menuConsole, &display, CtrRenderer_getDisplayCaps(renderer));
            down = 0; // the release that opened it isn't a menu input
        } else if (menu.open) {
            CtrMenuAction action = CtrMenu_update(&menu, down);
            if (action == MENU_ACTION_CHANGED) {
                display = menu.settings;
            } else if (action == MENU_ACTION_RESUME || action == MENU_ACTION_QUIT || action == MENU_ACTION_PERF_TEST) {
                if (menu.edited) {
                    // Only the menu's changes: settings given with --ini stay for this run.
                    CtrBtnprompt_saveEdits(&displayAtMenuOpen, &display, SETTINGS_PATH, CtrRenderer_getDisplayCaps(renderer));
                    logInfo("Display settings saved to %s\n", SETTINGS_PATH);
                }
                CtrMenu_close(&menu);
                ignoredButtons = held;
                if (action == MENU_ACTION_QUIT) {
                    CtrRenderer_endFrame(renderer);
                    break;
                }
                if (action == MENU_ACTION_PERF_TEST) {
                    CtrBench_openPicker(&menuConsole, &display);
                    down = 0; // the press that chose it isn't a picker input
                }
            }
        }
        if (autopauseLeft < 0 && arrlen(autopause) >= 2 && autopause[0] == runner->frameCount && !menu.open) {
            displayAtMenuOpen = display;
            CtrMenu_open(&menu, &menuConsole, &display, CtrRenderer_getDisplayCaps(renderer));
            autopauseLeft = autopause[1] > 2 ? autopause[1] : 2;
            logInfo("Menu opened as scheduled (%s)\n", AUTOPAUSE_PATH);
        }
        if (autopauseLeft > 0 && menu.open) {
            autopauseLeft--;
            // Saved as the next iteration begins, from what this one shows; that one closes the menu.
            if (autopauseLeft == 1) CtrRenderer_requestScreenshotSuffixed(renderer, "p");
            if (autopauseLeft == 0) CtrMenu_close(&menu);
        } FW_MARK(19);
        // The game stays paused while a screen of the test is up (the scene list, loading a chapter, the results).
        bool benchPaused = CtrBench_beginFrameTimed(down, runner->deltaTime / 1000.0) == BENCH_FRAME_PAUSE; FW_MARK(20);
        if (launch.exitAfter && CtrBench_takeFinished()) {
            logInfo("Remote: test done, leaving (--exit-after)\n");
            remoteQuit = true;
        }
        // Without a test, --exit-after with --flicker-probe leaves once the probe has reported.
        if (launch.exitAfter && !remoteQuit && launch.bench[0] == '\0' && CtrRenderer_flickerProbeDone(renderer)) {
            logInfo("Remote: flicker probe done, leaving (--exit-after)\n");
            remoteQuit = true;
        }
        if (launch.timeoutSeconds > 0 && !remoteQuit && realMs() - launchedAtMs > (uint64_t) launch.timeoutSeconds * 1000ULL) {
            logWarn("Remote: --timeout %d reached, leaving\n", launch.timeoutSeconds);
            remoteQuit = true;
        }
        if (remoteQuit) {
            CtrRenderer_endFrame(renderer);
            break;
        }
        bool paused = menu.open || benchPaused;
        // The renderer owns the bottom screen in magnifier mode; the consoles
        // draw there otherwise, and the menu and the test borrow it.
        bool saveFailed = __atomic_load_n(&CtrSave_failed, __ATOMIC_ACQUIRE);
        bool bottomBorrowed = menu.open || CtrBench_wantsBottomScreen() || saveFailed;
        if (drawingFullAt > 0 && drawingFullAt == runner->frameCount && display.drawing != DRAWING_FULL) {
            display.drawing = DRAWING_FULL;
            logInfo("Display: drawing Full from frame %d (test)\n", (int) runner->frameCount);
        }
        if (sliderUpAt > 0) {
            bool up = runner->frameCount >= sliderUpAt && (sliderDownAt == 0 || sliderDownAt > runner->frameCount);
            CtrRenderer_setSliderOverride(renderer, up ? 1.0f : 0.0f);
        } FW_MARK(17);
        CtrRenderer_setDisplay(renderer, &display);
        CtrRenderer_yieldBottomScreen(renderer, bottomBorrowed);
        bool dialogueShown = CtrRenderer_dialogueShown(renderer);
        bool consoleShown = bottomBorrowed || (display.bottomMode == BOTTOM_MODE_CONSOLE && !dialogueShown);
        bool statsShown = consoleShown && !bottomBorrowed; // the stats and log consoles
        if (dialogueWasShown && !dialogueShown && consoleShown) reclaimBottomForConsoles(bottomBorrowed);
        dialogueWasShown = dialogueShown;
        CtrLog_setConsoleEnabled(statsShown); FW_MARK(18);

        ignoredButtons &= held;
        // During a test the scene script plays (bench_script.h), not the player.
        bool benchRunning = CtrBench_running();
        sendInput(runner, paused || benchRunning ? 0 : gameHeld & ~ignoredButtons, &display.controls, &heldKeys);

        if (paused) {
            // Paused: keep presenting the last frame and let streamed music
            // keep playing.
            runner->audioSystem->vtable->update(runner->audioSystem, roomSpeed > 0 ? 1.0f / (float) roomSpeed : 0.0f);
            if (saveFailed) saveFailureWarning(false);
            CtrRenderer_endFrame(renderer);
            flushBottomScreen();
            continue;
        }
        for (ptrdiff_t i = 0; arrlen(autoshotFrames) > i; i++) {
            if (autoshotFrames[i] == runner->frameCount) CtrRenderer_requestScreenshot(renderer);
        }
        if (playback != nullptr && !benchRunning) InputRecording_processFrame(playback, runner->keyboard, inputFrame++);

        uint64_t stepStart = nowNanos(); FW_NOTE(runner); FW_MARK(3);
        CtrWatchdog_mark("step");
        Runner_step(runner);
        double stepMs = msSince(stepStart); FW_MARK(4);

#ifdef ENABLE_VM_GML_PROFILER
        // Diagnostic reports isolate Draw; Step and audio must not inflate the beam attribution.
        if (launch.knightDiagnostic) Profiler_reset(vm->profiler);
#endif
        uint64_t drawStart = nowNanos();
        CtrWatchdog_mark("draw");
        Profiler* parkedProfiler = profileStepOnly ? vm->profiler : nullptr;
        if (parkedProfiler != nullptr) vm->profiler = nullptr;
        // Deep frame skip: an unshown frame's Draw events go by their classes (Runner_loadDrawSkip).
        runner->drawSkip.active = frameSkipped && drawSkipDepth(&display) > 0;
        Runner_drawPre(runner, windowW, windowH);
        Runner_beginFrame(runner, appW, appH, windowW, windowH, appW, appH);
        Runner_drawViews(runner, appW, appH, false);
        renderer->vtable->endFrameInit(renderer);
        Runner_drawPost(runner, windowW, windowH);
        renderer->vtable->endFrameEnd(renderer);
        Runner_drawGUI(runner, windowW, windowH, appW, appH);
        if (parkedProfiler != nullptr) vm->profiler = parkedProfiler;
        runner->drawSkip.active = false;
        renderer->drawMuted = false;
        double drawMs = msSince(drawStart); FW_MARK(5);

#ifdef ENABLE_VM_GML_PROFILER
        if (launch.knightDiagnostic) {
            int f = runner->frameCount;
            if (f == 340 || f == 546 || f == 601 || f == 647 || f == 721 || f == 781 || f == 841 || f == 901 || f == 961) {
                CtrRenderStats ks = CtrRenderer_getStats(renderer);
                logInfo("[knightdiag] frame %d step %.3f ms draw %.3f ms draws %u vertices %u (Draw only)\n", f, stepMs, drawMs,
                        (unsigned) ks.drawCalls, (unsigned) ks.vertices);
                char* report = Profiler_createReport(vm->profiler, 0, 1);
                if (report != nullptr) {
                    for (char* line = report; *line != '\0';) {
                        char* end = strchr(line, '\n');
                        if (end != nullptr) *end = '\0';
                        logInfo("%s\n", line);
                        if (end == nullptr) break;
                        line = end + 1;
                    }
                    free(report);
                }
                logInfo("[knightdiag] end frame %d\n", f);
            }
        }
#endif

        // Games read input in Draw events too (Undertale's naming screen), so
        // edges are cleared only after both Step and Draw ran.
        RunnerKeyboard_beginFrame(runner->keyboard);

        float dt = roomSpeed > 0 ? 1.0f / (float) roomSpeed : 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        CtrWatchdog_mark("audio update"); FW_MARK(6);
        runner->audioSystem->vtable->update(runner->audioSystem, dt); FW_MARK(7);

        // A frame that called room_goto (or game_restart) ran no events after
        // the call, Draw included, so it shows the old room without the fade
        // objects that cover it, and the new room's first frame is slow while
        // its textures load. Like the desktop loop (and GameMaker), don't
        // present it.
        bool roomChanging = runner->pendingRoom != -1;
        if (roomChanging) CtrRenderer_skipPresent(renderer);
#ifdef CTRPORT_OFFDRAW
        { extern void CtrOffdraw_frameEnd(Runner*); CtrOffdraw_frameEnd(runner); }
#endif
#line 1505
        if (stateHash) UTSW_STATEHASH(runner);
        CtrWatchdog_mark("room change");
        UTSW_ROOMCHANGE(runner);

        CtrWatchdog_mark("present"); FW_MARK(8);
        CtrRenderer_endFrame(renderer); FW_MARK(9);
        if (benchRunning) CtrBench_endFrameTimed(stepMs, drawMs, frameSkipped); FW_MARK(10);
        if (firstFramePending && !frameSkipped && !roomChanging) {
            firstFramePending = false;
            logInfo("Startup ready: load=%u room=%ld frame=%ld %.3f ms since load, %.3f ms since app entry%s\n",
                    (unsigned) session.loadCount, (long) runner->currentRoomIndex, (long) runner->frameCount,
                    loadMs(loadEnteredAt), loadMs(appEnteredAt), early.loadTrace ? " (includes diagnostic output)" : "");
        }
        if (!frameSkipped && !roomChanging && runner->currentRoomIndex != lastPresentedRoom) {
            lastPresentedRoom = runner->currentRoomIndex;
            const char* name = runner->currentRoom->name;
            if (strcmp(name, "room_introstory") == 0 || strcmp(name, "room_introimage") == 0 ||
                strcmp(name, "room_intromenu") == 0 || strcmp(name, "PLACE_CHAPTER_SELECT_2x") == 0 ||
                strcmp(name, "PLACE_MENU") == 0) {
                logInfo("Startup screen: load=%u %s room=%ld frame=%ld submitted (not LCD scan-out)\n",
                        (unsigned) session.loadCount, name, (long) runner->currentRoomIndex, (long) runner->frameCount);
            }
        }
        if (saveFailed) saveFailureWarning(!CtrBench_wantsBottomScreen()); FW_MARK(11);
        // An open on another thread that failed may be waiting for the texture cache's files.
        if (!FW_SKIP(4)) CtrOpenFiles_poll(); FW_MARK(12);

        if (runner->currentRoomIndex != lastRoomIndex) {
            lastRoomIndex = runner->currentRoomIndex;
            CtrPacer_levelRoomChanged();
            logInfo("Room %d: %s\n", lastRoomIndex, runner->currentRoom->name);
            CtrHeapProfile_roomChanged(lastRoomIndex);
            CtrOpenFiles_log("room", false);
            CtrRenderer_prefetchRoom(renderer);
        } ROOMLOAD_TRACE_FRAME(runner, renderer, frameStart, stepMs, drawMs);

        // Only every fifth stats window is logged, so a hitch is logged on
        // its own as well.
        if (stepMs + drawMs > 100.0) {
            CtrLog_setConsoleEnabled(false);
            logInfo("Slow frame %d in %s: step %.0f ms, draw %.0f ms\n", runner->frameCount, runner->currentRoom->name, stepMs, drawMs);
            CtrLog_setConsoleEnabled(statsShown);
        }
        stepMsSum += stepMs;
        drawMsSum += drawMs;
        if (stepMs > stepMsMax) stepMsMax = stepMs;
        if (drawMs > drawMsMax) drawMsMax = drawMs;
        statsFrames++; FW_MARK(13);
#ifdef CTRPORT_TYPED_FRAMELOG
        // typedaot prototype: every frame's step and draw time (10 us units), printed once a stats window, so A/B arms
        // can be compared over the same game frames even when the bench window (wall-clock based) covers different ones.
        if (ftCount < 128) { ftStep[ftCount] = (uint16_t) (stepMs * 100.0 > 65535.0 ? 65535 : stepMs * 100.0); ftDraw[ftCount] = (uint16_t) (drawMs * 100.0 > 65535.0 ? 65535 : drawMs * 100.0); if (ftCount == 0) ftFirst = runner->frameCount; ftCount++; }
#endif
        if (!FW_SKIP(2) && frameStart - statsWindowStart >= 1000000000ULL) {
            double seconds = (double) (frameStart - statsWindowStart) / 1e9;
            extern struct mallinfo CtrStats_mallinfo(void); struct mallinfo mi = CtrStats_mallinfo(); // at this file's end
            CtrRenderStats rs = CtrRenderer_getStats(renderer);
            if (statsShown) {
                // Printed by the log's writer thread, from the numbers as they are now.
                __wrap_CtrLog_printTo(&statsConsole,
                               ctrReleaseStatsFormat,
                               CTRPORT_GAME_TITLE, CTRPORT_COMMIT, isNew3ds ? "New 3DS" : "Old 3DS", runner->frameCount,
                               runner->currentRoom->name, (double) statsFrames / seconds, (unsigned long) roomSpeed,
                               stepMsSum / statsFrames, stepMsMax, drawMsSum / statsFrames, (double) rs.gpuMs,
                               (unsigned long) rs.drawCalls, (unsigned long) rs.vertices, (double) mi.uordblks / (1024.0 * 1024.0));
                CtrLog_selectConsole(&logConsole);
            }
#ifdef CTRPORT_TYPED_STATS
            {
                extern void aotTypedLog(void);
                aotTypedLog();
            }
#endif
#ifdef CTRPORT_TYPED_FRAMELOG
            if (ftCount > 0) {
                char fb[128 * 12 + 32];
                int fn = snprintf(fb, sizeof fb, "[ft] %d", ftFirst);
                for (int fi = 0; fi < ftCount && fn < (int) sizeof fb - 14; fi++) fn += snprintf(fb + fn, sizeof fb - fn, " %u,%u", (unsigned) ftStep[fi], (unsigned) ftDraw[fi]);
                logInfo("%s\n", fb);
                ftCount = 0;
            }
#endif
#ifdef CTRPORT_TYPED_CENSUS
            {
                extern uint32_t aotCensus[];
                char cb[1400];
                int cn = snprintf(cb, sizeof cb, "[census] frame %d", runner->frameCount);
                for (int ci = 0; ci < 128 && cn < (int) sizeof cb - 12; ci++) cn += snprintf(cb + cn, sizeof cb - cn, " %lu", (unsigned long) aotCensus[ci]);
                logInfo("%s\n", cb);
            }
#endif
            if (++statsWindows % 5 == 0) {
                CtrLog_setConsoleEnabled(false);
                CtrSkipStats ss = CtrRenderer_takeSkipStats(renderer);
                logInfo("[perf] frame %d: %.1f fps, step avg %.2f ms max %.2f ms, draw avg %.2f ms max %.2f ms, gpu %.2f ms, %u draws, "
                        "%u verts, heap %.1f MB (peak %.1f MB), skipped %u (%u draws dropped, %u stale reads), "
                        "packed reclaims %lu (%lu KB), promotions %lu ok/%lu failed\n",
                        runner->frameCount, (double) statsFrames / seconds, stepMsSum / statsFrames, stepMsMax, drawMsSum / statsFrames,
                        drawMsMax, (double) rs.gpuMs, (unsigned) rs.drawCalls, (unsigned) rs.vertices, (double) mi.uordblks / (1024.0 * 1024.0),
                        (double) mi.arena / (1024.0 * 1024.0), (unsigned) ss.skipped, (unsigned) ss.dropped, (unsigned) ss.staleReads,
                        (unsigned long) rs.packedReclaims, (unsigned long) (rs.packedReclaimBytes / 1024),
                        (unsigned long) rs.promotions, (unsigned long) rs.failedPromotions);
                if (display.frameSkip != FRAME_SKIP_OFF) {
                    logInfo("[skip] frame %d: deep %d: %u Draw events left out, %u muted, %u self draws left out, %u muted draws left out\n",
                            runner->frameCount, (int) drawSkipDepth(&display), (unsigned) runner->drawSkip.skippedEvents,
                            (unsigned) runner->drawSkip.mutedEvents, (unsigned) runner->drawSkip.skippedSelf, (unsigned) ss.muted);
                    runner->drawSkip.skippedEvents = runner->drawSkip.mutedEvents = runner->drawSkip.skippedSelf = 0;
                    CtrGpuFrameStats gs;
                    CtrGpu_takeFrameGpuStats(&gs);
                    logInfo("[skipwork] frame %d: %u of %u skipped frames queued GPU work (%u draws, %u GX commands: %u fills, %u copies), %u behind; "
                            "gpu shown %u avg %.2f max %.2f ms, unshown %u avg %.2f max %.2f ms\n",
                            runner->frameCount, (unsigned) ss.workFrames, (unsigned) ss.skipped, (unsigned) ss.workDraws,
                            (unsigned) ss.workCommands, (unsigned) ss.workFills, (unsigned) ss.workCopies, (unsigned) ss.behind, (unsigned) gs.frames[0], (double) gs.avgMs[0],
                            (double) gs.maxMs[0], (unsigned) gs.frames[1], (double) gs.avgMs[1], (double) gs.maxMs[1]);
                    CtrSkipRecStats rs5 = CtrRenderer_takeSkipRecStats(renderer);
                    logInfo("[skiprec] frame %d: %u draws recorded, %u replays (%u draws), %u dropped unread, %u overflows, %u unrecordable, "
                            "%u snapshots, %u lost, %u late; peak %u bytes\n",
                            runner->frameCount, (unsigned) rs5.recOps, (unsigned) rs5.replays, (unsigned) rs5.replayOps, (unsigned) rs5.discards,
                            (unsigned) rs5.overflows, (unsigned) rs5.unrecordable, (unsigned) rs5.snapshots, (unsigned) rs5.lost,
                            (unsigned) rs5.lateSyncs, (unsigned) rs5.peakBytes);
                }
                CtrRenderer_logStats(renderer);
                CtrPacer_logStats();
                CtrPacer_logLevelStats();
                if (dataWin->bytecodeFile != nullptr) {
                    DataWinCodeStats* cs = &dataWin->codeStats;
                    logInfo("[code] %lu reads (%lu KB in %.1f ms), %lu of %lu KB resident\n", (unsigned long) cs->loads,
                            (unsigned long) (cs->loadedBytes / 1024), (double) cs->loadNanos / 1e6,
                            (unsigned long) (cs->residentBytes / 1024), (unsigned long) (cs->totalBytes / 1024));
                }
                if (statsWindows % 30 == 0) CtrHeapProfile_dump("running", 40);
                char* profile = vm->profiler != nullptr ? Profiler_createReport(vm->profiler, 30, statsFrames * 5) : nullptr;
                if (profile != nullptr) {
                    // A line at a time: the report (30 scripts and the total) is longer than a log line holds
                    // (LOG_BUFFER_SIZE), which cut it off after about 25 scripts.
                    for (char* line = profile; *line != '\0';) {
                        char* end = strchr(line, '\n');
                        if (end != nullptr) *end = '\0';
                        logInfo("%s\n", line);
                        if (end == nullptr) break;
                        line = end + 1;
                    }
                    free(profile);
                    Profiler_reset(vm->profiler);
                }
                CtrLog_setConsoleEnabled(statsShown);
            }
            statsWindowStart = frameStart;
            stepMsSum = drawMsSum = stepMsMax = drawMsMax = 0.0;
            statsFrames = 0;
            CtrLog_flushSoon();
        } FW_MARK(14);
        if (consoleShown && !FW_SKIP(1)) flushBottomScreen(); FW_MARK(15);
    }

    CtrRenderer_endFlickerProbe(renderer);
    if (stateHash) StateHash_end();
    uint64_t switchAt = loadTicks();
    if (CtrChapters_takeGameChange(&session, runner)) {
        loadEnteredAt = switchAt;
        arrfree(autoshotFrames);
        arrfree(autopause);
        arrfree(traceFrames);
        CtrWatchdog_watch(nullptr);
        runner->fileSystem = saveFs; // unloadGame closes the CtrFileSystem
        liveRunner = nullptr;
        liveSaves = nullptr;
        CtrChapters_unloadGame(runner);
        SandboxFileSystem_destroy(fileSystem);
        OverlayFileSystem_destroy(bundleFs);
        // The main thread's number caches were made during play and would split the heap.
        CtrMemory_releaseNumberCaches();
        logInfo("Startup phase: load=%u chapter-teardown %.3f ms\n", (unsigned) session.loadCount, loadMs(switchAt));
        logMemory("after unloading");
        // What is left is what splits the heap for the next chapter's large blocks.
        CtrMemory_logHeapLayout("after unloading", 64);
        CtrOpenFiles_log("after unloading", true);
        goto startGame;
    }

    // shouldExit is only set by the game (game_end); SELECT+START and the
    // menu's quit leave the loop without it.
    bool gameEnded = runner->shouldExit;
    CtrExit_logBegin("Exiting at frame %d%s\n", runner->frameCount, gameEnded ? " (the game ended itself)" : "");
    CtrExit_begin(LAB_EXIT_SECONDS);
    // Saves are written in the background; finish them before the notice in
    // case the console is switched off there.
    CtrWatchdog_mark("save flush");
    CtrFileSystem_flush(saveFs);
    if (__atomic_load_n(&CtrSave_failed, __ATOMIC_ACQUIRE)) {
        fatalScreen("Saving failed. Latest progress may not be saved.\n"
                    "Copy saves/ to your PC before repairs.\n"
                    "Previous saves are at their normal names or in .bak files.\n");
    }
    GAME_ENDED_SLOT(gameEnded);
#line 1716
    CtrWatchdog_stop();
    CtrAptTrace_stop();
    // Shut everything down before returning: threads and NDSP left running
    // can keep the console from getting back to the menu. Each step is logged
    // so a freeze here shows where it stopped; only to the file, since the
    // console draws into framebuffers that gfxExit frees.
    CtrLog_setConsoleEnabled(false);
    liveRunner = nullptr;
    liveSaves = nullptr;
    // The writer prints what is queued for the console, then never again.
    CtrLog_beginConsole();
    exitStep("closing the video");
    Video_closeAll(runner);
    exitStep("stopping audio");
    runner->audioSystem->vtable->destroy(runner->audioSystem);
    runner->audioSystem = nullptr;
    exitStep("stopping texture work");
    CtrRenderer_stopBackgroundWork(renderer);
    exitStep("closing the file system");
    CtrFileSystem_destroy(saveFs);
    SandboxFileSystem_destroy(fileSystem);
    OverlayFileSystem_destroy(bundleFs);
    runner->fileSystem = nullptr;
    exitStep("stopping the GPU");
    C3D_Fini();
    exitStep("restoring the top screen");
    CtrRenderer_leaveForExit(renderer);
    exitStep("closing graphics");
    gfxExit();
    exitStep("done");
    CtrExit_closeFilesLog();
    GAME_ENDED_TAIL(); return 0;
}

// An external assembler name refers to the existing private launch object but
// keeps this cold reference out of GCC's local BSS anchor grouping. Otherwise
// merely adding this helper changes main's console setup and text size.
static int videoStateHashStat(const char* path, struct stat* st) {
    extern CtrLaunchArgs videoLaunchArgs __asm__("launch");
    return videoLaunchArgs.stateHash ? 0 : bootHookStat(path, st);
}

// B10: retain the previous literal pool so every existing object after main.o
// keeps its address. The new strings and these pointers use the late reserve.
static const char* const relprepMainLegacy[]
    __attribute__((used, section(".rodata.late.relprep"))) = {
        "ctrport " CTRPORT_GAME_TITLE,
        "\x1b[1;36mctrport\x1b[0m %s (%s)\n%s\n",
        "ctrport %s build %s\n",
#ifdef CTRPORT_TARGET_N3DS
        "This n3ds build needs a New 3DS\nor New 2DS XL.\n\n"
        "On this Old 3DS/2DS, use\n%s-3ds-o3ds.3dsx\n",
#endif
        "\x1b[0;0H\x1b[1;36mctrport\x1b[0m %s (%s)\x1b[K\n"
        "%s, SELECT for options\x1b[K\n"
        "frame %-7d %-25.25s\n"
        "fps %5.1f/%-3lu step %5.2f max %6.2f ms\n"
        "cpu draw %5.2f ms  gpu %5.2f ms     \n"
        "%4lu draws %5lu verts  heap %5.1f MB\n"
        "SELECT+START exit, SELECT+L shot",
    };

// ===[ The consoles on the screen without the game (screens-port) ]===
//
// Late code (platform/3ds/late_reserve.ld, "screens"), called by the renderer's (the end of ctr_renderer.c's "The
// game on either screen"). The consoles are reached by external assembler names, as videoStateHashStat reaches
// launch, so that main's own code keeps its layout.

#define SCREENS_LATE __attribute__((noinline, section(".text.late.screens")))
extern bool CtrScreens_consolesOnTop; // ctr_screens.c
void CtrScreens_moveConsoles(bool top);
void CtrScreens_flushConsoles(void);

// Moves the stats and log consoles to the screen without the game, that screen blank, RGB565 and single buffered
// (the renderer's move): 50 columns on the top screen, 40 on the bottom. The menu's console stays on the bottom
// screen. While the game is there, what it prints (the menu, a test's screens, as they open) goes to the top
// screen's other buffer, unseen, and shows once the game has moved up for it.
SCREENS_LATE CTR_COLD_OS void CtrScreens_moveConsoles(bool top) {
    extern PrintConsole screensStats __asm__("statsConsole");
    extern PrintConsole screensLog __asm__("logConsole");
    extern PrintConsole screensMenu __asm__("menuConsole");
    gfxScreen_t screen = top ? GFX_TOP : GFX_BOTTOM;
    u16 w = 0, h = 0;
    u16* shown = (u16*) CtrPacer_shownFramebuffer(screen, GFX_LEFT, &w, &h);
    u16* spare = (u16*) CtrPacer_backFramebuffer(screen, GFX_LEFT);
    gfxSetDoubleBuffering(screen, false);
    int columns = top ? 50 : 40;
    screensStats.frameBuffer = screensLog.frameBuffer = shown;
    screensStats.consoleWidth = screensLog.consoleWidth = columns;
    consoleSetWindow(&screensStats, 1, 1, columns, STATS_CONSOLE_ROWS);
    consoleSetWindow(&screensLog, 1, STATS_CONSOLE_ROWS + 1, columns, BOTTOM_CONSOLE_ROWS - STATS_CONSOLE_ROWS);
    if (top) {
        screensMenu.frameBuffer = spare;
    } else {
        size_t bytes = (size_t) GSP_SCREEN_WIDTH * GSP_SCREEN_HEIGHT_BOTTOM * 2u;
        if (screensMenu.frameBuffer != shown) memcpy(shown, screensMenu.frameBuffer, bytes);
        screensMenu.frameBuffer = shown;
        GSPGPU_FlushDataCache(shown, (u32) bytes);
    }
    CtrScreens_consolesOnTop = top;
}

// The consoles draw into their framebuffer with the CPU: on the top screen, write it back for the LCD once per frame
// (flushBottomScreen does the bottom screen's).
SCREENS_LATE void CtrScreens_flushConsoles(void) {
    extern PrintConsole screensStats __asm__("statsConsole");
    if (CtrScreens_consolesOnTop && screensStats.frameBuffer != nullptr) {
        GSPGPU_FlushDataCache(screensStats.frameBuffer, (u32) GSP_SCREEN_WIDTH * GSP_SCREEN_HEIGHT_TOP * 2u);
    }
}

// ===[ Tests, a screenshot's notice and the error screens with the game on either screen (screens-port, part 2) ]===

#include <sys/iosupport.h>

#define SCREENS_RO __attribute__((section(".rodata.late.screens")))
#define SCREENS_BSS __attribute__((section(".bss.late.screens")))
extern bool CtrScreens_noticeShown; // ctr_screens.c
extern bool CtrScreens_topBacklightOff;
bool CtrScreens_benchLaunched(void);
void CtrScreens_screenshotNotice(bool paint);
void __real_CtrLog_emergencyKeepLink(const char* text, int waitMs);
void __wrap_CtrLog_emergencyKeepLink(const char* text, int waitMs);

// This run was started for a test: --bench, or benchrun.txt on the card (as main() checks them). Its settings then
// keep the game on top (ctr_screens.c's CtrDisplay_load).
SCREENS_LATE bool CtrScreens_benchLaunched(void) {
    extern __typeof__(launch) screensLaunch __asm__("launch");
    static const char path[(sizeof(BENCHRUN_PATH) + 3u) & ~3u] SCREENS_RO = BENCHRUN_PATH;
    struct stat st;
    return screensLaunch.bench[0] != '\0' || stat(path, &st) == 0;
}

// "Screenshot saved" on the top screen's consoles, with the game on the bottom screen: where main paints it on the
// bottom screen's while waiting for each frame (ctr_renderer.c's waitWithScreenshotNotice, which leaves an RGB8
// screen alone), the same row and letters, across the 400 columns. The first paint keeps what it covers, and
// !paint puts that back (the row is the stats' second line, which doesn't change).
SCREENS_LATE CTR_COLD_OS void CtrScreens_screenshotNotice(bool paint) {
    extern PrintConsole screensStats __asm__("statsConsole");
    static u16 saved[400 * 8] SCREENS_BSS;
    static const char message[20] SCREENS_RO = "Screenshot saved";
    u16* fb = (u16*) screensStats.frameBuffer;
    if (!CtrScreens_consolesOnTop || fb == nullptr || gfxGetScreenFormat(GFX_TOP) != GSP_RGB565_OES) {
        CtrScreens_noticeShown = false;
        return;
    }
    if (!paint && !CtrScreens_noticeShown) return;
    CtrLog_beginConsole();
    const ConsoleFont* font = &consoleGetDefault()->font;
    size_t length = strlen(message);
    for (int x = 0; x < 400; x++) {
        for (int y = 0; y < 8; y++) {
            int i = x * 240 + 239 - (y + 8);
            if (!paint) {
                fb[i] = saved[x * 8 + y];
                continue;
            }
            if (!CtrScreens_noticeShown) saved[x * 8 + y] = fb[i];
            uint8_t bits = 0;
            if (x >= 8 && x < 8 + (int) length * 8) {
                unsigned char c = (unsigned char) message[(x - 8) / 8];
                bits = font->gfx[(c - font->asciiOffset) * 8 + y];
            }
            fb[i] = (bits & (0x80u >> (x % 8))) ? 0xffff : 0;
        }
    }
    CtrScreens_noticeShown = paint;
    GSPGPU_FlushDataCache(fb, (u32) GSP_SCREEN_WIDTH * GSP_SCREEN_HEIGHT_TOP * 2u);
    CtrLog_endConsole();
}

// The error screens' console output, written back for the LCD as it is printed: they print once and then only
// flush the bottom screen (flushBottomScreen), and with the game on the bottom screen they print on the top one.
static const devoptab_t* screensErrorOut SCREENS_BSS;
static devoptab_t screensErrorTab SCREENS_BSS;

SCREENS_LATE static ssize_t screensErrorWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    extern PrintConsole screensStats __asm__("statsConsole");
    ssize_t written = screensErrorOut->write_r(r, fd, ptr, len);
    if (screensStats.frameBuffer != nullptr) {
        GSPGPU_FlushDataCache(screensStats.frameBuffer, (u32) GSP_SCREEN_WIDTH * GSP_SCREEN_HEIGHT_TOP * 2u);
    }
    return written;
}

// fatalScreen and outOfMemoryScreen, the only callers, send their reason with this before anything else. Their text
// goes where it is seen: with the consoles on the top screen (the game on the bottom one), the out-of-memory screen
// prints on the top screen with fatalScreen's (the menu's console moved there, across its 50 columns), lit if Off
// had switched its backlight off; with the game on top, on the bottom screen as before, its backlight switched back
// on if Off had switched it off (main showed both screens there unlit then).
SCREENS_LATE void __wrap_CtrLog_emergencyKeepLink(const char* text, int waitMs) {
    extern PrintConsole screensStats __asm__("statsConsole");
    extern PrintConsole screensMenu __asm__("menuConsole");
    static bool done SCREENS_BSS;
    void CtrBootlog_reveal(bool keepText); // below: the boot's log, if still unseen, shows with the error
    CtrBootlog_reveal(true);
    __real_CtrLog_emergencyKeepLink(text, waitMs);
    if (done) return;
    done = true;
    bool top = CtrScreens_consolesOnTop && screensStats.frameBuffer != nullptr;
    if (R_SUCCEEDED(gspLcdInit())) {
        GSPLCD_PowerOnBacklight(top ? GSPLCD_SCREEN_TOP : GSPLCD_SCREEN_BOTTOM);
        gspLcdExit();
    }
    if (!top) return;
    CtrScreens_topBacklightOff = false;
    screensMenu.frameBuffer = screensStats.frameBuffer;
    screensMenu.consoleWidth = 50;
    consoleSetWindow(&screensMenu, 1, 1, 50, BOTTOM_CONSOLE_ROWS);
    screensErrorOut = devoptab_list[STD_OUT];
    if (screensErrorOut != nullptr && screensErrorOut->write_r != nullptr) {
        screensErrorTab = *screensErrorOut;
        screensErrorTab.write_r = screensErrorWrite;
        devoptab_list[STD_OUT] = &screensErrorTab;
    }
}

// Private Old observer: route existing emitted callers.

// Timing policy reads parsed arguments before log.txt opens. Appended late so
// main's existing code, private storage and source line numbers stay unchanged.
__attribute__((noinline, section(".text.late.pacer_io")))
CtrLaunchArgs* CtrBenchIo_launch(void) { return &launch; }

// ===[ The boot's log unseen unless Other screen is Stats and log ]===
//
// With the log set to Off (the default), the game must not start with the log shown and switch it off once loaded;
// it should never appear. main() above
// sets up the stats and log consoles on the bottom screen right after C3D_Init, and they print from the first line
// (the boot header, then every log line: data.win, textures, the VM, the first room), lit; the settings first reach
// them at the main loop's first frame (CtrLog_setConsoleEnabled(statsShown)), and Off then darkens the screen. The
// default is Off: a missing bottom_mode is the retired magnifier, which loads as Off (ctr_screens.c's screensLoad), and
// no shipped file sets it; tests set bottom_mode = 2 in their files or with --ini, and keep the log.
//
// Here, late code only (platform/3ds/late_reserve.ld, "bootlog"; main's code and constants stay where they were):
// - CtrSdMigrate_run (z_sdmigrate.c, a stub since 2026-10-27), which main calls once the consoles are set up and
//   before the log's first line, points the stats and log consoles at the bottom screen's other buffer, never shown
//   (the screen is single buffered for the consoles, so that buffer is free until the game takes the screen). The
//   menu's console stays on the shown buffer: notices print there. (Until 10-29 this waited for CtrLab_booted, after
//   the log file opened: the game folder's two lines, which the log's opening prints again (z_gamedir.c), stayed on
//   the bottom screen through the whole load with the log Off. Play-test 10-29: "even with logs off, i still
//   saw boot logs for a few seconds while the game was starting".)
// - CtrLab_booted, which main calls just before the boot header (argcrash's own guard may call it earlier, before the
//   consoles: then nothing happens), reads Other screen as main's settings will (other_screen, or bottom_mode, the
//   later line winning), from settings.ini and then the --ini lines. Stats and log points the consoles back at the
//   shown buffer with what they printed so far, the same picture as printing there from the start.
// - With the game on the bottom screen, the renderer moves the consoles to the top screen while it starts, before the
//   first frame (ctr_renderer.c's screensMoveGame, CtrScreens_moveConsoles above). Still unseen, they then print into
//   a buffer of their own (the top screen's other buffer is the menu console's there), freed at the first frame.
//   (Also seen in the 10-29 Azahar proof: the log on the top screen from the renderer's start to the first frame.)
// - The first CtrLog_setConsoleEnabled (the main loop's first frame, or a notice or out-of-memory screen before it)
//   points them back at the shown buffer, without the boot's text unless it enables them (it doesn't with Off).
// - A fatal error (CtrLog_emergencyKeepLink, from the error screens, before they print) points them back with the
//   text, so the boot's log shows above the error as before.
// What the consoles print and the log file are unchanged; only where the boot's lines are drawn.
//
// Shots builds also keep a boot capture for the proof (boot_capture = 1 in settings.ini): a thread saves both
// screens' shown buffers, as the LCD registers point at them, each time one changes, from here until 3 s after the
// main loop's first frame (bootcap_NN_top.bmp / _bot.bmp beside settings.ini, the times in the log), and
// boot_log_legacy = 1 leaves the consoles as before, for the comparison. boot_capture = 2 saves the bottom screen
// only, for the whole run (up to 250 captures in 300 s): the proof of the stats header after an Other screen toggle,
// with fps_overlap_legacy = 1 for the FPS counter painted over the consoles as before (ctr_argui.c).

#define BOOTLOG_LATE __attribute__((noinline, section(".text.late.bootlog")))
#define BOOTLOG_RO __attribute__((aligned(4), section(".rodata.bootlog_in_text")))
#define BOOTLOG_BSS __attribute__((section(".bss.late.bootlog")))
// Once a run each, compiled for size on every target (on UNDERTALE Old, CTR_COLD_OS's flag; freebytes 10-29).
#define BOOTLOG_OS __attribute__((optimize("Os")))

void __real_CtrLab_booted(void);
void __wrap_CtrLab_booted(void);
void __real_CtrLog_setConsoleEnabled(bool enabled);
void __wrap_CtrLog_setConsoleEnabled(bool enabled);
void CtrBootlog_reveal(bool keepText);
int CtrBootlog_hide(int status);
void __real_CtrScreens_moveConsoles(bool top);
void __wrap_CtrScreens_moveConsoles(bool top);

static struct {
    u16* hidden; // the bottom screen's buffer not shown (or one of their own), while the stats and log consoles print there
    u16* shown;
    u32 bytes;
    bool looked;
    bool owned; // hidden is their own (malloc), after a move of the consoles
#ifdef CTRPORT_PLAYER_SHOTS
    u8 capture; // 1 the boot, 2 the bottom screen all run
    bool legacy;
    volatile bool looping; // the main loop's first frame has come
    volatile u64 loopTick;
#endif
} bootlog BOOTLOG_BSS;

static const char bootlogPath[(sizeof(SETTINGS_PATH) + 3u) & ~3u] BOOTLOG_RO = SETTINGS_PATH;
static const char bootlogRead[4] BOOTLOG_RO = "r";
static const char bootlogKey[20] BOOTLOG_RO = " bottom_mode = %d";
static const char bootlogOtherKey[20] BOOTLOG_RO = " other_screen = %d";
static const char bootlogHidden[84] BOOTLOG_RO = "Boot: the stats and log consoles print unseen until the first frame (Other screen)\n";
#ifdef CTRPORT_PLAYER_SHOTS
static const char bootlogCapKey[20] BOOTLOG_RO = " boot_capture = %d";
static const char bootlogLegacyKey[24] BOOTLOG_RO = " boot_log_legacy = %d";
static const char bootlogFpsKey[28] BOOTLOG_RO = " fps_overlap_legacy = %d";
bool CtrBootlog_fpsLegacy BOOTLOG_BSS; // ctr_argui.c's flushConsoles wrap reads it
#endif

// A settings line's other_screen or bottom_mode, the later winning as in ctr_screens.c (or a shots build's test
// keys), else mode.
BOOTLOG_LATE static int bootlogLine(const char* line, int mode) {
    int v = 0;
    if (sscanf(line, bootlogKey, &v) == 1 && 0 <= v && BOTTOM_MODE_COUNT > v) return v;
    if (sscanf(line, bootlogOtherKey, &v) == 1 && 0 <= v && BOTTOM_MODE_COUNT > v) return v;
#ifdef CTRPORT_PLAYER_SHOTS
    if (sscanf(line, bootlogCapKey, &v) == 1) bootlog.capture = (u8) v;
    if (sscanf(line, bootlogFpsKey, &v) == 1) CtrBootlog_fpsLegacy = v != 0;
    if (sscanf(line, bootlogLegacyKey, &v) == 1) bootlog.legacy = v != 0;
#endif
    return mode;
}

#ifdef CTRPORT_PLAYER_SHOTS
static void bootcapStart(void);
#endif

// main's CtrSdMigrate_run, before the log's first line (the loading screen's, before the consoles, does nothing). It
// returns the stub's status as given.
BOOTLOG_LATE BOOTLOG_OS int CtrBootlog_hide(int status) {
    extern PrintConsole bootStats __asm__("statsConsole");
    extern PrintConsole bootLog __asm__("logConsole");
    if (bootlog.looked || bootStats.frameBuffer == nullptr || gfxGetScreenFormat(GFX_BOTTOM) != GSP_RGB565_OES) return status;
    u16 w = 0, h = 0;
    u16* shown = (u16*) CtrPacer_shownFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
    u16* hidden = (u16*) CtrPacer_backFramebuffer(GFX_BOTTOM, GFX_LEFT);
    gfxSetDoubleBuffering(GFX_BOTTOM, false); // the consoles' single buffer, as consoleInit left it
    if (shown != nullptr && hidden != nullptr && hidden != shown && bootStats.frameBuffer == shown && bootLog.frameBuffer == shown) {
        bootlog.bytes = (u32) w * h * 2u;
        memset(hidden, 0, bootlog.bytes); // as consoleInit cleared the shown one
        bootlog.shown = shown;
        bootlog.hidden = hidden;
        bootStats.frameBuffer = bootLog.frameBuffer = hidden;
    }
    return status;
}

// The renderer's move of the consoles (to the top screen with the game on the bottom one) while they print unseen:
// they go on unseen there, into their own buffer, as large as the top screen's (the larger). Without the memory they
// print where the move put them, as before.
BOOTLOG_LATE BOOTLOG_OS void __wrap_CtrScreens_moveConsoles(bool top) {
    extern PrintConsole bootStats __asm__("statsConsole");
    extern PrintConsole bootLog __asm__("logConsole");
    __real_CtrScreens_moveConsoles(top);
    if (bootlog.hidden == nullptr) return;
    if (!bootlog.owned) bootlog.hidden = (u16*) malloc((size_t) GSP_SCREEN_WIDTH * GSP_SCREEN_HEIGHT_TOP * 2u);
    bootlog.owned = bootlog.hidden != nullptr;
    if (!bootlog.owned) return;
    bootlog.bytes = (u32) GSP_SCREEN_WIDTH * (top ? GSP_SCREEN_HEIGHT_TOP : GSP_SCREEN_HEIGHT_BOTTOM) * 2u;
    memset(bootlog.hidden, 0, bootlog.bytes);
    bootlog.shown = (u16*) bootStats.frameBuffer;
    bootStats.frameBuffer = bootLog.frameBuffer = bootlog.hidden;
}

BOOTLOG_LATE BOOTLOG_OS void __wrap_CtrLab_booted(void) {
    extern PrintConsole bootStats __asm__("statsConsole");
    extern __typeof__(launch) bootLaunch __asm__("launch");
    __real_CtrLab_booted();
    if (bootlog.looked || bootStats.frameBuffer == nullptr) return;
    bootlog.looked = true;
    // As main reads them (CtrDisplay_defaults, settings.ini, the --ini lines), bottom_mode alone.
    int mode = BOTTOM_MODE_MAGNIFIER;
    FILE* f = fopen(bootlogPath, bootlogRead);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) mode = bootlogLine(line, mode);
        fclose(f);
    }
    for (int i = 0; bootLaunch.iniCount > i; i++) mode = bootlogLine(bootLaunch.ini[i], mode);
#ifdef CTRPORT_PLAYER_SHOTS
    if (bootlog.capture) bootcapStart();
    if (bootlog.legacy) mode = BOTTOM_MODE_CONSOLE;
#endif
    if (bootlog.hidden == nullptr) return;
    if (mode != BOTTOM_MODE_CONSOLE) {
        logInfo(bootlogHidden);
        return;
    }
    CtrLog_beginConsole(); // the log's writer prints them now
    CtrBootlog_reveal(true);
    CtrLog_endConsole();
}

// The consoles back on the shown buffer, with what they printed meanwhile (keepText) or without it. Once. With the
// text it is an error's path (argcrash's handler among them), where the console's lock may never come: no lock then.
BOOTLOG_LATE BOOTLOG_OS void CtrBootlog_reveal(bool keepText) {
    extern PrintConsole bootStats __asm__("statsConsole");
    extern PrintConsole bootLog __asm__("logConsole");
    if (bootlog.hidden == nullptr) return;
    if (!keepText) CtrLog_beginConsole();
    if (bootStats.frameBuffer == bootlog.hidden) {
        if (keepText) {
            memcpy(bootlog.shown, bootlog.hidden, bootlog.bytes);
            GSPGPU_FlushDataCache(bootlog.shown, bootlog.bytes);
        }
        bootStats.frameBuffer = bootlog.shown;
    }
    if (bootLog.frameBuffer == bootlog.hidden) bootLog.frameBuffer = bootlog.shown;
    if (bootlog.owned) { // their own buffer, not freed on an error's path
        if (!keepText) free(bootlog.hidden);
    } else { // the boot's text gone from the other buffer too, should the screen ever show it
        memset(bootlog.hidden, 0, bootlog.bytes);
        GSPGPU_FlushDataCache(bootlog.hidden, bootlog.bytes);
    }
    bootlog.hidden = nullptr;
    if (!keepText) CtrLog_endConsole();
}

BOOTLOG_LATE void __wrap_CtrLog_setConsoleEnabled(bool enabled) {
    if (bootlog.hidden != nullptr) CtrBootlog_reveal(enabled);
#ifdef CTRPORT_PLAYER_SHOTS
    if (!bootlog.looping) {
        bootlog.loopTick = svcGetSystemTick();
        bootlog.looping = true;
    }
#endif
    __real_CtrLog_setConsoleEnabled(enabled);
}

#ifdef CTRPORT_PLAYER_SHOTS
// ===[ Boot capture (shots builds, boot_capture = 1) ]===
static const char bootcapName[(sizeof(GAME_DIR "/bootcap_%02u_%s.bmp") + 3u) & ~3u] BOOTLOG_RO = GAME_DIR "/bootcap_%02u_%s.bmp";
static const char bootcapTop[4] BOOTLOG_RO = "top";
static const char bootcapBot[4] BOOTLOG_RO = "bot";
static const char bootcapWrite[4] BOOTLOG_RO = "wb";
static const char bootcapLog[52] BOOTLOG_RO = "bootcap: %02u %s at %lu ms (format %lu, loop %d)\n";
static const char bootcapDone[28] BOOTLOG_RO = "bootcap: done, %u captures\n";

static struct {
    u32 delta;  // a linear address minus its physical one
    u32 sum[2]; // the last capture's checksum, per screen
    u32 count;
    u64 startTick;
    u8 row[400 * 3];
} bootcap BOOTLOG_BSS;

BOOTLOG_LATE static void bootcapPut32(u8* p, u32 v) {
    p[0] = (u8) v;
    p[1] = (u8) (v >> 8);
    p[2] = (u8) (v >> 16);
    p[3] = (u8) (v >> 24);
}

// The screen's shown buffer as the LCD registers have it: its address, format (0 RGBA8, 1 RGB8, 2 RGB565, ...) and
// height (400 or 320; 240 wide, column-major).
BOOTLOG_LATE static const u8* bootcapShown(u32 screen, u32* fmt, u32* h) {
    u32 regs[6] = { 0 };
    if (R_FAILED(GSPGPU_ReadHWRegs(0x400468u + screen * 0x100u, regs, sizeof(regs)))) return nullptr;
    u32 phys = (regs[4] & 1u) != 0 ? regs[1] : regs[0];
    *fmt = regs[2] & 7u;
    *h = screen == 0 ? 400u : 320u;
    if (phys >= 0x18000000u && 0x18600000u > phys) return (const u8*) (uintptr_t) (phys - 0x18000000u + 0x1F000000u);
    if (phys >= 0x20000000u && 0x28000000u > phys) return (const u8*) (uintptr_t) (phys + bootcap.delta);
    return nullptr;
}

BOOTLOG_LATE static void bootcapSave(u32 screen, const u8* fb, u32 fmt, u32 h, u32 ms) {
    char name[64];
    snprintf(name, sizeof(name), bootcapName, (unsigned) bootcap.count, screen == 0 ? bootcapTop : bootcapBot);
    FILE* f = fopen(name, bootcapWrite);
    if (f == nullptr) return;
    u8 hd[54];
    memset(hd, 0, sizeof(hd));
    hd[0] = 'B';
    hd[1] = 'M';
    bootcapPut32(hd + 2, 54u + h * 720u);
    hd[10] = 54;
    hd[14] = 40;
    bootcapPut32(hd + 18, h);
    bootcapPut32(hd + 22, 240u);
    hd[26] = 1;
    hd[28] = 24;
    bootcapPut32(hd + 34, h * 720u);
    fwrite(hd, 1, sizeof(hd), f);
    u32 bpp = fmt == 0 ? 4u : fmt == 1 ? 3u : 2u;
    for (int32_t y = 239; y >= 0; y--) {
        for (u32 x = 0; h > x; x++) {
            u32 idx = x * 240u + 239u - (u32) y;
            u8* o = bootcap.row + x * 3u;
            if (bpp == 4u) {
                memcpy(o, fb + idx * 4u + 1u, 3);
            } else if (bpp == 3u) {
                memcpy(o, fb + idx * 3u, 3);
            } else {
                u32 v = ((const u16*) fb)[idx];
                if (fmt == 2u) {
                    o[0] = (u8) ((v & 31u) << 3);
                    o[1] = (u8) (((v >> 5) & 63u) << 2);
                    o[2] = (u8) ((v >> 11) << 3);
                } else { // RGB5A1 and RGBA4, as RGB5A1 (the app shows neither)
                    o[0] = (u8) (((v >> 1) & 31u) << 3);
                    o[1] = (u8) (((v >> 6) & 31u) << 3);
                    o[2] = (u8) ((v >> 11) << 3);
                }
            }
        }
        fwrite(bootcap.row, 1, (size_t) h * 3u, f);
    }
    fclose(f);
    CtrLinkFiles_note(name);
    logInfo(bootcapLog, (unsigned) bootcap.count, screen == 0 ? bootcapTop : bootcapBot, (unsigned long) ms, (unsigned long) fmt,
            (int) bootlog.looping);
}

BOOTLOG_LATE static void bootcapThread(void* arg) {
    (void) arg;
    for (;;) {
        u64 now = svcGetSystemTick();
        u32 ms = (u32) ((now - bootcap.startTick) / (SYSCLOCK_ARM11 / 1000u));
        bool whole = bootlog.capture == 2u;
        if (bootcap.count >= (whole ? 250u : 60u) || ms > (whole ? 300000u : 120000u)) break;
        if (!whole && bootlog.looping && now - bootlog.loopTick > 3u * (u64) SYSCLOCK_ARM11) break;
        for (u32 screen = whole ? 1u : 0u; 2u > screen; screen++) {
            u32 fmt = 0, h = 0;
            const u8* fb = bootcapShown(screen, &fmt, &h);
            if (fb == nullptr) continue;
            u32 bytes = h * 240u * (fmt == 0 ? 4u : fmt == 1 ? 3u : 2u);
            GSPGPU_FlushDataCache(fb, bytes);
            GSPGPU_InvalidateDataCache(fb, bytes);
            u32 sum = 0;
            for (u32 i = 0; bytes / 4u > i; i++) sum = sum * 31u + ((const u32*) fb)[i];
            sum ^= (u32) (uintptr_t) fb;
            if (sum == bootcap.sum[screen]) continue;
            bootcap.sum[screen] = sum;
            bootcapSave(screen, fb, fmt, h, ms);
            bootcap.count++;
        }
        svcSleepThread(33000000LL);
    }
    logInfo(bootcapDone, (unsigned) bootcap.count);
}

BOOTLOG_LATE static void bootcapStart(void) {
    u32 linear = (u32) (uintptr_t) gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    bootcap.delta = linear - osConvertVirtToPhys((const void*) (uintptr_t) linear);
    bootcap.startTick = svcGetSystemTick();
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    threadCreate(bootcapThread, nullptr, 16 * 1024, prio > 0x19 ? prio - 1 : prio, -2, true);
}
#endif

// ===[ The stats block's heap figures (freewins) ]===
// mallinfo walks the heap: about 0.1 ms on UNDERTALE and 0.4 ms on DELTARUNE (console rows, cheaplevers), once a
// second. main's stats block reads its figures in the stats console's line (when statsShown, which main gave
// CtrLog_setConsoleEnabled this frame) and in the [perf] line (every fifth stats window). This is called once per
// stats window where main called mallinfo, so its count follows main's statsWindows. Other windows get the last
// figures, which nothing reads, except when a page (the options menu, the screens) turned the console off during this
// frame: then the stats line, which that page is about to cover, shows the last figures. Gameplay never reads them.
__attribute__((section(".bss.freewins_late"))) static struct mallinfo statsHeap;
__attribute__((section(".bss.freewins_late"))) static uint8_t statsHeapCalls; // main's statsWindows % 5
__attribute__((noinline, section(".text.freewins_late"))) struct mallinfo CtrStats_mallinfo(void) {
    if (++statsHeapCalls == 5) statsHeapCalls = 0;
    if (statsHeapCalls == 0 || CtrLog_consoleEnabled()) statsHeap = mallinfo();
    return statsHeap;
}
