// noshots (credfix, ruling 2026-10-28): a timing row takes no scene "shot" pictures.
//
// Why: --bench noshots (and bench_io=0, which ctr_bench_io.c turns into noshots on every --bench row unless
// bench_io=1) only stopped the halfway and sweep shots; a scene's own "shot" steps still saved top, bottom and shot
// pictures to the card. Each one costs a ~1.5 s capture and leaves held frames after it, so timing rows counted the
// capture aftermath as held frames (credfix-fons-o3-2, 2026-10-28: 138 "Screenshot: saved" with both flags;
// docs/3ds-hardware.md, "Bench screenshots leave held frames").
//
// What it does: ctr_benchmark_rooms.inc's benchRoomsTakeShot is the only caller of BenchScript_takeShot, wrapped here
// (a call from another file, so every existing function keeps its code; only the call target changes). The real
// function still consumes the step's flag; when the run's scene list has the word noshots, the wrapper reports no
// shot, so no picture is requested. Pictures never change game state, and the scene's frames and rooms are as before.
// Coverage and picture rows pass --ini bench_io=1 and keep their shots.
#include <stdbool.h>
#include <string.h>

#include "bench_script.h"
#include "ctr_launch_args.h"

#define NOSHOTS_CODE __attribute__((noinline, section(".text.noshots_late")))
// In late text (late_reserve.ld), not a mergeable pooled string: that would go to .rodata, outside the late reserve.
#define NOSHOTS_RO __attribute__((section(".rodata.noshots_in_text"), aligned(4)))

CtrLaunchArgs* CtrBenchIo_launch(void);
bool __real_BenchScript_takeShot(BenchScript* script);
bool __wrap_BenchScript_takeShot(BenchScript* script);

static const char noShotsWord[7] NOSHOTS_RO = {'n', 'o', 's', 'h', 'o', 't', 's'};

// True when the scene list (ids separated by spaces or commas) has the word noshots.
static bool NOSHOTS_CODE noShots(void) {
    for (const char* p = CtrBenchIo_launch()->bench;; p++) {
        const char* w = p;
        while (*p != '\0' && *p != ' ' && *p != ',') p++;
        if (p - w == sizeof(noShotsWord) && memcmp(w, noShotsWord, sizeof(noShotsWord)) == 0) return true;
        if (*p == '\0') return false;
    }
}

bool NOSHOTS_CODE __wrap_BenchScript_takeShot(BenchScript* script) {
    return __real_BenchScript_takeShot(script) && !noShots();
}
