// What is left of the SD folder move, and DELTARUNE's chapter check line (hashfix, 2026-10-27).
//
// Builds before 2026-10-25 kept each game in a subfolder of the pre-10-25 shared folder. This file used to move that
// folder to CTRPORT_GAME_DIR (sdmc:/3ds/<game>-3ds) at a player's first launch, and in lab mode it read the old folder
// in place ("borrowing", through a swapped sdmc device table and a wrap of fsMakePath). It was decided on 2026-10-27 that
// both old game paths are dead and only the new ones are used. The old folder is no longer
// read, moved or named: no build given outside the lab ever used it, and the lab consoles' cards were moved by
// a script. Borrowing also hid every data.win receipt from the version check (libctru's
// archive_getmtime builds its own path, past both rewrites), so every lab boot hashed every data.win
// (docs/3ds-hardware.md, "Lab mode's old folder hid every data.win receipt").
//
// main() and the loading screen (z_loadscreen.c) still call CtrSdMigrate_run(ctrSdLegacyDir, 0777): it moves nothing.
// main's call, right after the consoles are set up and before the log's first line, is where the boot's log starts
// printing unseen unless Other screen is Stats and log (main.c's CtrBootlog_hide, freebytes 10-29); the loading
// screen's, before the consoles, does nothing.
// Its constants and buffers are gone; their late bss is kept as a placeholder (smSpare), so the late bss doesn't move.
//
// DELTARUNE only: the boot's check of the launcher and every chapter (BxN_init / BxO_init) and the check of a chapter
// being entered (bnRoute / boRoute) go through BxChapterHash_check (z_loadscreen.c), which logged nothing. Its wrap
// here logs one line per check (the data.win, "cached", "hashed" or "unreadable", and the time), and when a check has
// to hash, it makes the data.win's cache folder first (at the hash's first progress call, so a cached check costs no
// card call): main.c makes that folder only for the data.win it loads, so on a card without the chapters' cache
// folders (a manual copy; the builder makes them) every chapter never entered was hashed again at every boot, its
// result never written. A timing bench row (--bench) doesn't make it either: ctr_bench_io.c's mkdir wrap skips every
// "cache" folder then, on purpose.
//
// Everything here is late code (late_reserve.ld's z_sdmigrate.o lines), so nothing linked before it moves.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <3ds/types.h>
#include <3ds/svc.h>

#define SM_LATE __attribute__((noinline, section(".text.late.sdmigrate")))
#define SM_CONST __attribute__((aligned(4), section(".rodata.sdmigrate_in_text")))
// The late bss this file had (0x110d bytes at 2026-10-27 on all four targets): the chapter check's state first, then
// the placeholder, each in its own section in that order (late_reserve.ld), so the total and every later address stay.
#define SM_BSS_BYTES 0x110d

int CtrSdMigrate_run(const char* legacyParent, int mode);
int CtrBootlog_hide(int status);

// Kept for main()'s and the loading screen's call; nothing reads it.
const char ctrSdLegacyDir[] SM_CONST = "";

SM_LATE int CtrSdMigrate_run(const char* legacyParent, int mode) {
    (void) legacyParent;
    (void) mode;
    return CtrBootlog_hide(0); // 8 B as before (a move and the jump), so nothing after it moves
}

static struct {
    char* cache;           // the check's cache path: the caller's own buffer (bnCheck's / boCheck's cache[])
    void (*tick)(void);    // the caller's progress call
    bool made;             // the cache folder is made (or tried) for this check
} hf __attribute__((used, section(".bss.late.sdmigrate")));
static unsigned char smSpare[SM_BSS_BYTES - sizeof(hf)] __attribute__((used, section(".bss.sdmigrate_spare")));

#ifdef ENABLE_WAD17
#include "ctr_data_version.h"
#include "log.h"

static const char hfLine[] SM_CONST = "Chapter check: %s %s, %lu ms\n";
static const char hfCached[] SM_CONST = "cached";
static const char hfHashed[] SM_CONST = "hashed";
static const char hfUnreadable[] SM_CONST = "unreadable";

static SM_LATE void hfTick(void) {
    if (!hf.made) {
        hf.made = true;
        char* slash = strrchr(hf.cache, '/');
        if (slash != NULL) {
            *slash = '\0';
            mkdir(hf.cache, 0777);
            *slash = '/';
        }
    }
    if (hf.tick != NULL) hf.tick();
}

CtrDataVersionResult __real_BxChapterHash_check(const char*, const char*, const char*, const char*, void (*)(void),
                                                bool*, char[65]);
CtrDataVersionResult __wrap_BxChapterHash_check(const char*, const char*, const char*, const char*, void (*)(void),
                                                bool*, char[65]);
SM_LATE CtrDataVersionResult __wrap_BxChapterHash_check(const char* path, const char* game, const char* part,
                                                        const char* cachePath, void (*progress)(void), bool* cached,
                                                        char digest[65]) {
    hf.cache = (char*) cachePath;
    hf.tick = progress;
    hf.made = false;
    u64 started = svcGetSystemTick();
    CtrDataVersionResult result = __real_BxChapterHash_check(path, game, part, cachePath, hfTick, cached, digest);
    // 268111 ticks a millisecond, 261.8 per 1024: a 32-bit divide (a 64-bit one would put a 64-bit constant here, and
    // its 8-byte alignment would move this section).
    u64 elapsed = svcGetSystemTick() - started;
    logInfo(hfLine, path, result == CTR_DATA_UNREADABLE ? hfUnreadable : *cached ? hfCached : hfHashed,
            (unsigned long) (elapsed >> 10) / 262u);
    return result;
}
#endif
