// GOG's measured data-version row, in its own late slot so previous tables keep their addresses and sizes.
#include "ctr_wrap_chain.h"
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CTRPORT_GOG_COMPAT
#include "ctr_chapters.h"
#include "ctr_launch_args.h"
#include "log.h"
#include <stdlib.h>

#define GOGUT_TEXT __attribute__((noinline, section(".text.late.gogut")))
#define GOGUT_CONST __attribute__((aligned(4), section(".rodata.gogut_in_text")))
static const char gogLabFlag[] GOGUT_CONST = "--lab-gog-data";
static const char gogLabDir[] GOGUT_CONST = "sdmc:/3ds/undertale-3ds-gog";
static const char gogLabWin[] GOGUT_CONST = "sdmc:/3ds/undertale-3ds-gog/data.win";
static const char gogLabPart[] GOGUT_CONST = "root";
static const char gogLabSelected[] GOGUT_CONST = "Lab data: %s (music and cache beside data.win)\n";
static const char gogLabMissing[] GOGUT_CONST = "Lab data missing: %s; refusing the default data folder\n";
static const char gogLabNeedsBench[] GOGUT_CONST = "--lab-gog-data needs --bench; data folder unchanged";
static const char gogLabMessageFormat[] GOGUT_CONST = "%s%s";
static const char gogLabSeparator[] GOGUT_CONST = "; ";
static const char gogLabEmpty[] GOGUT_CONST = "";
static bool gogLabData __attribute__((section(".bss.late.gogut")));
// Set only by the full data-version digest check, before renderer/font initialization.
bool ctrGogutSmartDecoded __attribute__((section(".bss.late.gogut")));

bool __real_CtrLaunchArgs_parse(CtrLaunchArgs* args, int argc, char** argv);
void __real_CtrChapters_init(CtrGameSession* session, const char* gameDir);
void __real_CtrChapters_returnToLauncher(CtrGameSession* session, const char* gameDir);

// Bench-only preset: no ini key, SD trigger, options-menu entry or player-path change. Compact argv in a
// private stack array so the original parser and its public record keep their exact layout and behaviour.
GOGUT_TEXT bool CtrLaunchArgs_chainGogut(CtrLaunchArgs* args, int argc, char** argv) {
    gogLabData = false;
    if (argc < 1 || argc > 64) return __real_CtrLaunchArgs_parse(args, argc, argv);
    char* filtered[64];
    int count = 1;
    bool requested = false;
    filtered[0] = argv[0];
    for (int i = 1; argc > i; i++) {
        if (argv[i] != nullptr && strcmp(argv[i], gogLabFlag) == 0) requested = true;
        else filtered[count++] = argv[i];
    }
    bool result = __real_CtrLaunchArgs_parse(args, count, filtered);
    if (requested && args->bench[0] != '\0') gogLabData = true;
    else if (requested) {
        args->problems++;
        size_t used = strlen(args->messages);
        snprintf(args->messages + used, sizeof(args->messages) - used, gogLabMessageFormat,
                 used ? gogLabSeparator : gogLabEmpty, gogLabNeedsBench);
    }
    return result || requested;
}

GOGUT_TEXT void __wrap_CtrChapters_init(CtrGameSession* session, const char* gameDir) {
    if (gogLabData) {
        struct stat st;
        if (stat(gogLabWin, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
            logError(gogLabMissing, gogLabWin);
            abort();
        }
        gameDir = gogLabDir;
        logInfo(gogLabSelected, gameDir);
    }
    __real_CtrChapters_init(session, gameDir);
}

GOGUT_TEXT void __wrap_CtrChapters_returnToLauncher(CtrGameSession* session, const char* gameDir) {
    __real_CtrChapters_returnToLauncher(session, gogLabData ? gogLabDir : gameDir);
}

typedef struct GogKnownFile {
    uint64_t size;
    char game[12];
    char part[20];
    char digest[65];
} GogKnownFile;

#define DATA_VERSIONS_GOG_ONLY
#define DATA_VERSION(game, part, size, digest) {size, game, part, digest},
static const GogKnownFile gogKnownFiles[] __attribute__((aligned(8), section(".rodata.gogut_in_text"))) = {
#include "../data_versions.inc"
};
#undef DATA_VERSION

__attribute__((noinline, section(".text.late.gogut"))) CtrDataVersionResult CTR_DATAVERSION_GOGUT(
    CTR_DATAVERSION_CHAIN_PARAMS) {
    // main identifies chapter parts by the bundle folder's name. This bench-only UT folder is still root,
    // including for the downstream room-plan digest gate; no player or DELTARUNE chapter key changes.
    if (gogLabData && strcmp(path, gogLabWin) == 0) part = gogLabPart;
    ctrGogutSmartDecoded = false;
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_GOGUT(path, game, part, cachePath, progress, cached, digest);
    struct stat st;
    if (result != CTR_DATA_UNKNOWN || stat(path, &st) != 0) return result;
    for (size_t i = 0; i < sizeof(gogKnownFiles) / sizeof(gogKnownFiles[0]); i++) {
        const GogKnownFile* known = &gogKnownFiles[i];
        if (known->size == (uint64_t) st.st_size && strcmp(known->game, game) == 0 && strcmp(known->part, part) == 0
            && strcmp(known->digest, digest) == 0) {
            ctrGogutSmartDecoded = true;
            return CTR_DATA_KNOWN;
        }
    }
    return result;
}
#endif

#ifdef CTRPORT_GOG_COMPAT
// The lab sibling is another location for this verified UNDERTALE data, not another RomFS bundle.
// Keep all pack consumers (glyphs, verdicts and smoothing profiles) on the packaged canonical name.
// Unknown data and player launches retain the original helper, including its bounds/path checks.
static const char gogRomfsName[] GOGUT_CONST = "undertale";
bool __real_CtrPack_romfsName(char* name, size_t size);
GOGUT_TEXT bool __wrap_CtrPack_romfsName(char* name, size_t size) {
    if (!__real_CtrPack_romfsName(name, size)) return false;
    if (!gogLabData || !ctrGogutSmartDecoded) return true;
    if (size < sizeof(gogRomfsName)) return false;
    memcpy(name, gogRomfsName, sizeof(gogRomfsName));
    return true;
}
#endif
