// Test packages only: their scene and input tape belong to their own RomFS.
// The shared pen owner/parser and the existing bench-output policy are unchanged.
#if defined(CTRPORT_TOUCH_QUEUE)
#include <stdio.h>
#include <string.h>
#ifdef CTRPORT_TOUCH_QUEUE_HOST
int CtrRomfs_init(const char* name);
#else
#include "ctr_benchromfs.h"
#endif

#define TQ_CODE __attribute__((noinline, section(".touchqueue_text")))
#define TQ_RO __attribute__((section(".touchqueue_ro")))
static const char tqSdTape[] TQ_RO = CTRPORT_GAME_DIR "/touchscript.txt";
static const char tqSdScenes[] TQ_RO = CTRPORT_GAME_DIR "/benchscenes.txt";
static const char tqTape[] TQ_RO = "romfs:/touchscript.txt";
static const char tqScenes[] TQ_RO = "romfs:/bench_scenes.txt";
static const char tqMount[] TQ_RO = "romfs";

// This is ctr_bench_io.c's existing __wrap_fopen, renamed only in queue builds.
// Forward through it, including on the two substituted paths: log-only I/O
// continues to own its sink tokens, writes, flushes and closes.
FILE* CtrTouchQueue_originalOpen(const char* path, const char* mode);
TQ_CODE FILE* __wrap_fopen(const char* path, const char* mode) {
    if (mode[0] == 'r' && (mode[1] == '\0' || (mode[1] == 'b' && mode[2] == '\0'))) {
        const char* replacement = NULL;
        if (strcmp(path, tqSdTape) == 0) replacement = tqTape;
        else if (strcmp(path, tqSdScenes) == 0) replacement = tqScenes;
        if (replacement != NULL) {
            // Never fall back to another build's SD inputs on mount/open failure.
            if (CtrRomfs_init(tqMount) < 0) return NULL;
            return CtrTouchQueue_originalOpen(replacement, mode);
        }
    }
    return CtrTouchQueue_originalOpen(path, mode);
}
#endif
