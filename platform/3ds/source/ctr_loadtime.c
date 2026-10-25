#include "ctr_loadtime.h"
#include "ctr_log.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

#define GAME_DIR CTRPORT_GAME_DIR
#define LOG_PATH GAME_DIR "/log.txt"
#define HANG_REPORT_PATH GAME_DIR "/hang.txt"
#define CRASH_REPORT_PATH GAME_DIR "/crash.txt"
#define BOOT_TRAIL_PATH GAME_DIR "/labboot.txt"
#define SETTINGS_PATH GAME_DIR "/settings.ini"
#define AUTOSHOT_PATH GAME_DIR "/autoshot.txt"
#define AUTOPAUSE_PATH GAME_DIR "/autopause.txt"
#define GPU_TRACE_PATH GAME_DIR "/gputrace.txt"
#define TEX_PROBE_PATH GAME_DIR "/texprobe.txt"
#define AUTOPLAY_PATH GAME_DIR "/autoplay.json"
#define FIXED_SEED_PATH GAME_DIR "/fixedseed.txt"
#define BENCHRUN_PATH GAME_DIR "/benchrun.txt"
#define PROFILE_PATH GAME_DIR "/profile.txt"
#define STATE_HASH_PATH GAME_DIR "/statehash.txt"
#define STATE_HASH_OUT_PATH GAME_DIR "/state_hash.log"
#define PACE_STRESS_PATH GAME_DIR "/pacestress.txt"
#define FLICKER_PROBE_PATH GAME_DIR "/flickerprobe.txt"
#define SELFTEST_PATH GAME_DIR "/selftest.txt"
#define FORCE_MATERIALIZE_PATH GAME_DIR "/forcematerialize.txt"
#define DRAWING_FULL_AT_PATH GAME_DIR "/drawingfullat.txt"
#define SLIDER_AT_PATH GAME_DIR "/sliderat.txt"
#define NO_SURFACE_READERS_PATH GAME_DIR "/nosurfacereaders.txt"
#define DRAWSKIP_PATH GAME_DIR "/drawskip.txt"

// Release status messages remain ordered in the existing writer queue. Fatal
// paths, final completion and explicit load traces still drain it separately.
void flushMemoryStatus(void) {
#if !defined(NDEBUG) || defined(CTRPORT_HEAP_PROFILE)
    CtrLog_flush();
#endif
}

// First-use status checkpoints keep their startup durability without putting
// new branches or literal-pool entries into main's warm frame loop. Explicit
// load traces drain at LoadTrace_end, after their measured interval.
void flushVersionStatus(void) {
    static bool checked __attribute__((section(".bss.late.loadtime")));
    if (!checked) {
        checked = true;
        CtrLog_flush();
    }
}

void flushParseStatus(void) {
    static bool parsed __attribute__((section(".bss.late.loadtime")));
    if (!parsed) {
        parsed = true;
        CtrLog_flush();
    }
}

// Only this fixed diagnostic whitelist has session-cached presence. Contents
// of present hooks are reopened on each load; game files and saves remain live.
static int* __attribute__((noinline, noipa, no_reorder, section(".text.late.loadtime"))) bootHook(const char* path) {
    static const char hookPath0[] __attribute__((section(".rodata.late.loadtime"))) = AUTOSHOT_PATH;
    static const char hookPath1[] __attribute__((section(".rodata.late.loadtime"))) = AUTOPAUSE_PATH;
    static const char hookPath2[] __attribute__((section(".rodata.late.loadtime"))) = GPU_TRACE_PATH;
    static const char hookPath3[] __attribute__((section(".rodata.late.loadtime"))) = TEX_PROBE_PATH;
    static const char hookPath4[] __attribute__((section(".rodata.late.loadtime"))) = AUTOPLAY_PATH;
    static const char hookPath5[] __attribute__((section(".rodata.late.loadtime"))) = FIXED_SEED_PATH;
    static const char hookPath6[] __attribute__((section(".rodata.late.loadtime"))) = BENCHRUN_PATH;
    static const char hookPath7[] __attribute__((section(".rodata.late.loadtime"))) = PROFILE_PATH;
    static const char hookPath8[] __attribute__((section(".rodata.late.loadtime"))) = STATE_HASH_PATH;
    static const char hookPath9[] __attribute__((section(".rodata.late.loadtime"))) = STATE_HASH_OUT_PATH ".trace-enable";
    static const char hookPath10[] __attribute__((section(".rodata.late.loadtime"))) = NO_SURFACE_READERS_PATH;
    static const char hookPath11[] __attribute__((section(".rodata.late.loadtime"))) = FORCE_MATERIALIZE_PATH;
    static const char hookPath12[] __attribute__((section(".rodata.late.loadtime"))) = GAME_DIR "/knightdiag.txt";
    static const char hookPath13[] __attribute__((section(".rodata.late.loadtime"))) = SELFTEST_PATH;
    static const char hookPath14[] __attribute__((section(".rodata.late.loadtime"))) = DRAWING_FULL_AT_PATH;
    static const char hookPath15[] __attribute__((section(".rodata.late.loadtime"))) = SLIDER_AT_PATH;
    static const char* const paths[] __attribute__((section(".rodata.late.loadtime"))) = {
        hookPath0, hookPath1, hookPath2, hookPath3, hookPath4, hookPath5, hookPath6, hookPath7, hookPath8, hookPath9, hookPath10, hookPath11, hookPath12, hookPath13, hookPath14, hookPath15
    };
    // Kept out of .data and the default .bss anchor group: initialized metadata
    // would shift every existing BSS address, even with unchanged warm text.
    static int presence[sizeof(paths) / sizeof(paths[0])] __attribute__((section(".bss.late.loadtime")));
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++)
        if (strcmp(path, paths[i]) == 0) return &presence[i];
    return NULL;
}

int bootHookStat(const char* path, struct stat* st) {
    int* hook = bootHook(path);
    if (hook != NULL && *hook != 0) {
        if (*hook > 0) return 0; // Callers only use the existence result.
        errno = ENOENT;
        return -1;
    }
    int result = stat(path, st);
    if (hook != NULL && (result == 0 || errno == ENOENT)) *hook = result == 0 ? 1 : -1;
    return result;
}

FILE* openBootHook(const char* path, const char* mode) {
    int* hook = bootHook(path);
    if (hook != NULL && *hook < 0) {
        errno = ENOENT;
        return NULL;
    }
    FILE* file = fopen(path, mode);
    if (hook != NULL && file == NULL && errno == ENOENT) *hook = -1;
    return file;
}
