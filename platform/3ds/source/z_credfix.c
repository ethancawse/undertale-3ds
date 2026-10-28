// credfix: a notice at start for a game file read only deep into the game. Late code and constants only
// (platform/3ds/late_reserve.ld): nothing here moves existing code or data, and it runs once per data.win check.
//
// UNDERTALE's True Pacifist ending reads credits.txt (its credits loader, the PC branch) for the names that fly at the
// player in the special thanks; without it that room runs with no names at all, after the whole game (a playtest run,
// 2026-10-26). DELTARUNE's Chapter 1 reads its English text from lang/lang_en.json (its language loader, after
// file_exists); the other chapters' lang folders hold only lang_ja.json, so only Chapter 1 is checked. The builder
// copies both. When the file is missing next to data.win, the player gets the same one-time notice as for a missing
// music file (CtrNotice), naming the path to copy it to, when the game starts instead of when it reads the file.
//
// Hook: a link of CtrDataVersion_check's wrap chain (ctr_wrap_chain.h), which main.c's start-up check and the chapter
// check (z_builderx_chapters.c) both go through, so their callers keep their code. Pending notices show at the top of
// the main loop (main.c), and a dismissed one stays dismissed (settings.ini, notice_dismissed).

#if defined(ENABLE_WAD16) || defined(ENABLE_WAD17)

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "ctr_notice.h"
#include "ctr_wrap_chain.h"
#include "log.h"

#define CREDFIX_CODE __attribute__((noinline, section(".text.credfix_late")))
#define CREDFIX_RO __attribute__((section(".rodata.credfix_late")))

#ifdef ENABLE_WAD16
static const char credfixNeeded[] CREDFIX_RO = "/credits.txt";
static const char credfixPart[] CREDFIX_RO = "root";
static const char credfixKind[] CREDFIX_RO = "Missing credits";
#else
static const char credfixNeeded[] CREDFIX_RO = "/lang/lang_en.json";
static const char credfixPart[] CREDFIX_RO = "chapter1_windows";
static const char credfixKind[] CREDFIX_RO = "Missing text";
#endif
// The first line of the missing music notice's advice (ctr_notice.c, CtrNotice_missing).
static const char credfixAdvice[] CREDFIX_RO = "Copy to the path above from your PC.";
static const char credfixLog[] CREDFIX_RO = "%s: %s\n";

CREDFIX_CODE CtrDataVersionResult CTR_DATAVERSION_CREDFIX(const char* path, const char* game, const char* part,
                                                          const char* cachePath, void (*progress)(void), bool* cached,
                                                          char digest[65]) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_CREDFIX(path, game, part, cachePath, progress, cached, digest);
    const char* slash = path != NULL ? strrchr(path, '/') : NULL;
    size_t dir = slash != NULL ? (size_t) (slash - path) : 0;
    char needed[512];
    struct stat st;
    // GCC copies a constant array it can see into a string literal (.rodata.str, which would move the merged strings
    // after it): the empty asm hides the arrays' contents, so the strings stay in .rodata.credfix_late only.
    const char* wanted = credfixPart;
    const char* file = credfixNeeded;
    __asm__("" : "+r"(wanted), "+r"(file));
    if (slash != NULL && part != NULL && strcmp(part, wanted) == 0 && dir + sizeof(credfixNeeded) <= sizeof(needed)) {
        memcpy(needed, path, dir);
        memcpy(needed + dir, file, sizeof(credfixNeeded));
        if (stat(needed, &st) != 0) {
            logWarn(credfixLog, credfixKind, needed);
            CtrNotice_report(credfixKind, needed, credfixAdvice);
        }
    }
    return result;
}

#endif
