// texprep: writes a data.win's texture cache folder (pageNNN.dtx and complete) on a PC, as the console's
// "Preparing textures" screen would on first run. See texprep_host.h.
//
// Usage: texprep [--exact] [--verbose] [--gm2022-5=auto|0|1] <data.win> <cache folder>
//        texprep --sources   the digest of the sources it was built from, then those files (sources.py)
//
// Exit status: 0 when every page was prepared and the folder's "complete" marker written; 1 when the folder is
// incomplete (the console then prepares the rest itself); 2 for bad arguments or an unreadable data.win.
//
// The console's texture preparation (ctr_texture_pack.c) is included whole, unchanged, so the driver can point
// its cache folder somewhere other than beside data.win (the player's game folder stays read-only). Nothing
// below changes tile formats or compression. UT selects cache epochs 7/6 and clears both recognized heads' hidden
// RGB after format selection, matching console output; see tools/lanes/results/redpix.md for its alpha-blend use audit.

// Declarations the console code expects from newlib, where the host's C library spells them differently.
#ifdef _WIN32
#define main texprep_main
#endif
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <io.h>
#include <direct.h>
static int texprep_mkdir(const char* path, int mode) {
    (void) mode;
    return _mkdir(path);
}
#define mkdir(path, mode) texprep_mkdir(path, mode)
#define fsync(fd) _commit(fd)
#endif

// The PC driver calls the original packer directly. Console-only game-thread
// wrappers are included with that source, but this host link has no --wrap
// support on every platform; bind their real calls to the original functions.
#define __real_CtrPackWorker_poll CtrPackWorker_poll
#define __real_CtrPack_isComplete CtrPack_isComplete
#define __real_CtrPackWorker_setKeepPacked CtrPackWorker_setKeepPacked
// Use the console's post-conversion mitigation, preserving format selection exactly.
bool texprepUtCache;
#define CTRPORT_TEXPREP_UT_CACHE texprepUtCache
#include "ctr_texture_pack.c"
#undef __real_CtrPackWorker_poll
#undef __real_CtrPack_isComplete
#undef __real_CtrPackWorker_setKeepPacked

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "ctr_qoi.h"
#include "texprep_host.h"
#include "texprep_paths.h"
#ifndef TEXPREP_DEPS_PASS
#include "texprep_sources.h" // written by build.sh
#endif

// ===[ data.win: the fields the cache key and the worker read ]===
//
// The same fields engine/src/data_win.c gives the console: the file size, GEN8's gameID and timestamp (the cache
// key, CtrPack_keyFor), and each TXTR page's blob offset and size (blobSize runs to the next page's blob, or to
// the end of the TXTR chunk for the last one).

static int gm2022_5Flag = -1; // -1: decided from the TXTR entry layout

typedef struct {
    bool enabled;
    uint32_t page;
    uint16_t x, y, w, h;
    char temporary[PACK_PATH_MAX + 16];
} AlphaZeroScope;
static AlphaZeroScope alphaZero[2];

typedef struct {
    FILE* f;
    long size;
} Reader;

static bool readAtOffset(Reader* r, long offset, void* dst, size_t n) {
    return offset >= 0 && offset + (long) n <= r->size && fseek(r->f, offset, SEEK_SET) == 0 && fread(dst, 1, n, r->f) == n;
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Name-keyed selection, like the host item-verdict tables: no ordinal or atlas coordinate is assumed. The
// FNV-1a-64 keys select the normal and hurt Final Froggit heads, never the body. WAD16 only.
static bool findAlphaZero(Reader* r, const DataWin* dw, long sprt, long sprtSize, long tpag, long tpagSize) {
    if (dw->gen8.wadVersion != 16 || sprt < 0 || tpag < 0) return true;
    uint8_t data[64];
    if (sprtSize < 4 || !readAtOffset(r, sprt, data, 4)) return false;
    uint32_t count = le32(data);
    if (count > (uint32_t) ((sprtSize - 4) / 4)) return false;
    for (uint32_t i = 0; count > i; i++) {
        if (!readAtOffset(r, sprt + 4 + 4 * (long) i, data, 4)) return false;
        uint32_t p = le32(data);
        if (p == 0) continue;
        if (!readAtOffset(r, p, data, 4)) return false;
        uint32_t name = le32(data);
        uint64_t hash = UINT64_C(14695981039346656037);
        bool ended = false;
        for (unsigned j = 0; sizeof(data) > j; j++) {
            if (!readAtOffset(r, (long) name + j, data, 1)) return false;
            if (data[0] == 0) { ended = true; break; }
            hash = (hash ^ data[0]) * UINT64_C(1099511628211);
        }
        int scopeIndex = hash == UINT64_C(0x2821e0fa13f349be) ? 0 :
                         hash == UINT64_C(0x67cea240c7c5a6ba) ? 1 : -1;
        if (!ended || scopeIndex < 0) continue;
        if (alphaZero[scopeIndex].enabled || !readAtOffset(r, (long) p + 56, data, 8) || le32(data) != 1) return false;
        uint32_t item = le32(data + 4);
        if ((long) item < tpag || (long) item + 22 > tpag + tpagSize || !readAtOffset(r, item, data, 22)) return false;
        uint16_t v[11];
        for (unsigned j = 0; 11 > j; j++) v[j] = (uint16_t) (data[2*j] | ((uint16_t) data[2*j+1] << 8));
        if (v[2] != 51 || v[3] != 38 || v[10] >= dw->txtr.count) return false;
        alphaZero[scopeIndex] = (AlphaZeroScope) { .enabled = true, .page = v[10], .x = v[0], .y = v[1], .w = v[2], .h = v[3] };
    }
    return true;
}

static bool pageMagicOk(Reader* r, uint32_t offset) {
    uint8_t m[4];
    if (!readAtOffset(r, (long) offset, m, 4)) return false;
    return memcmp(m, "\x89PNG", 4) == 0 || memcmp(m, "fioq", 4) == 0 || memcmp(m, "2zoq", 4) == 0;
}

// Fills dw with the cache key and the TXTR pages. Returns false with a message on stderr.
static bool loadDataWin(const char* path, DataWin* dw) {
    Reader r = { fopen(path, "rb"), 0 };
    if (r.f == NULL) {
        fprintf(stderr, "texprep: cannot open %s\n", path);
        return false;
    }
    bool ok = false;
    uint8_t head[8];
    uint32_t* ptrs = NULL;
    if (fseek(r.f, 0, SEEK_END) != 0 || (r.size = ftell(r.f)) < 8 || !readAtOffset(&r, 0, head, 8) || memcmp(head, "FORM", 4) != 0) {
        fprintf(stderr, "texprep: %s is not a GameMaker data file\n", path);
        goto done;
    }
    long formEnd = 8 + (long) le32(head + 4);
    if (formEnd > r.size) formEnd = r.size;
    long gen8 = -1, gen8Size = 0, txtr = -1, txtrSize = 0, sprt = -1, sprtSize = 0, tpag = -1, tpagSize = 0;
    for (long at = 8; formEnd >= at + 8;) {
        uint8_t chunk[8];
        if (!readAtOffset(&r, at, chunk, 8)) break;
        long size = (long) le32(chunk + 4);
        if (memcmp(chunk, "GEN8", 4) == 0) gen8 = at + 8, gen8Size = size;
        if (memcmp(chunk, "TXTR", 4) == 0) txtr = at + 8, txtrSize = size;
        if (memcmp(chunk, "SPRT", 4) == 0) sprt = at + 8, sprtSize = size;
        if (memcmp(chunk, "TPAG", 4) == 0) tpag = at + 8, tpagSize = size;
        at += 8 + size;
    }
    if (gen8 < 0 || txtr < 0 || gen8Size < 100) {
        fprintf(stderr, "texprep: %s has no GEN8 or TXTR chunk\n", path);
        goto done;
    }
    uint8_t g[104];
    if (!readAtOffset(&r, gen8, g, sizeof(g))) goto bad;
    uint32_t wadVersion = g[1];
    if (8 >= wadVersion && 108 > gen8Size) {
        fprintf(stderr, "texprep: %s is an early GameMaker 1 file (WAD %u), which this tool does not read\n", path, (unsigned) wadVersion);
        goto done;
    }
    dw->fileSize = (size_t) r.size;
    dw->gen8.wadVersion = (uint8_t) wadVersion;
    dw->gen8.gameID = le32(g + 20);
    dw->gen8.major = le32(g + 44);
    dw->gen8.minor = le32(g + 48);
    // WAD 12 and older store a signed 32-bit timestamp, sign-extended (data_win.c parseGEN8).
    dw->gen8.timestamp = 12 >= wadVersion ? (uint64_t) (int64_t) (int32_t) le32(g + 92) : (uint64_t) le32(g + 92) | ((uint64_t) le32(g + 96) << 32);

    uint8_t c[4];
    if (!readAtOffset(&r, txtr, c, 4)) goto bad;
    uint32_t count = le32(c);
    if (count > 4096) goto bad;
    ptrs = calloc(count > 0 ? count : 1, sizeof(uint32_t));
    Texture* textures = calloc(count > 0 ? count : 1, sizeof(Texture));
    if (ptrs == NULL || textures == NULL) goto bad;
    dw->txtr.count = count;
    dw->txtr.textures = textures;
    for (uint32_t i = 0; count > i; i++) {
        if (!readAtOffset(&r, txtr + 4 + 4 * (long) i, c, 4)) goto bad;
        ptrs[i] = le32(c);
    }
    // The entry's last field is the blob offset; the entry's length depends on the GameMaker version (8, 12, 16
    // or 28 bytes, data_win.c parseTXTR). Take it from the distance between the first two entries, and check the
    // result: every blob must start with an image's magic.
    uint32_t entrySize = 0;
    for (uint32_t i = 0; count > i + 1 && entrySize == 0; i++) {
        if (ptrs[i] != 0 && ptrs[i + 1] != 0) entrySize = ptrs[i + 1] - ptrs[i];
    }
    if (entrySize != 8 && entrySize != 12 && entrySize != 16 && entrySize != 28) {
        fprintf(stderr, "texprep: %s: unexpected TXTR entry size %u\n", path, (unsigned) entrySize);
        goto done;
    }
    if (gm2022_5Flag < 0) gm2022_5Flag = entrySize >= 16;
    long txtrEnd = txtr + txtrSize;
    for (uint32_t i = 0; count > i; i++) {
        if (ptrs[i] == 0) continue;
        if (!readAtOffset(&r, (long) ptrs[i] + (long) entrySize - 4, c, 4)) goto bad;
        textures[i].present = true;
        textures[i].blobOffset = le32(c);
        if (textures[i].blobOffset != 0 && !pageMagicOk(&r, textures[i].blobOffset)) {
            fprintf(stderr, "texprep: %s: page %u does not start with an image\n", path, (unsigned) i);
            goto done;
        }
    }
    for (uint32_t i = 0; count > i; i++) {
        if (textures[i].blobOffset == 0) continue;
        uint32_t end = count > i + 1 && textures[i + 1].blobOffset != 0 ? textures[i + 1].blobOffset : (uint32_t) txtrEnd;
        textures[i].blobSize = end - textures[i].blobOffset;
    }
    if (!findAlphaZero(&r, dw, sprt, sprtSize, tpag, tpagSize)) goto bad;
    ok = true;
    goto done;
bad:
    fprintf(stderr, "texprep: %s is truncated or damaged\n", path);
done:
    free(ptrs);
    fclose(r.f);
    return ok;
}

// The one version question the texture code asks (CtrQoi's 2zoq header length; it also checks the stream's
// "BZh" signature at both lengths, so either answer finds the stream).
bool DataWin_isVersionAtLeast(const DataWin* dw, uint32_t major, uint32_t minor, uint32_t release, uint32_t build) {
    (void) dw;
    (void) release;
    (void) build;
    if (major == 2022 && minor == 5) return gm2022_5Flag > 0;
    fprintf(stderr, "texprep: unexpected version question %u.%u\n", (unsigned) major, (unsigned) minor);
    abort();
}

// ===[ Driver ]===

static double seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double) t.tv_sec + (double) t.tv_nsec * 1e-9;
}

static void sleepMs(unsigned ms) {
    struct timespec ts = { 0, (long) ms * 1000000L };
    nanosleep(&ts, NULL);
}

static int usage(void) {
    fprintf(stderr, "usage: texprep [--exact] [--verbose] [--gm2022-5=auto|0|1] <data.win> <cache folder>\n"
                    "       texprep --sources\n");
    return 2;
}

int main(int argc, char** argv) {
#ifndef TEXPREP_DEPS_PASS
    if (argc == 2 && strcmp(argv[1], "--sources") == 0) {
        printf("%s\n%s", TEXPREP_SOURCES_DIGEST, TEXPREP_SOURCES_LIST);
        return 0;
    }
#endif
    bool exact = false;
    const char* paths[2];
    int pathCount = 0;
    for (int i = 1; argc > i; i++) {
        if (strcmp(argv[i], "--exact") == 0) exact = true;
        else if (strcmp(argv[i], "--verbose") == 0) texprepVerbose = true;
        else if (strcmp(argv[i], "--gm2022-5=0") == 0) gm2022_5Flag = 0;
        else if (strcmp(argv[i], "--gm2022-5=1") == 0) gm2022_5Flag = 1;
        else if (strcmp(argv[i], "--gm2022-5=auto") == 0) gm2022_5Flag = -1;
        else if (argv[i][0] == '-' || pathCount == 2) return usage();
        else paths[pathCount++] = argv[i];
    }
    if (pathCount != 2) return usage();
    const char* dataWinArg = paths[0];
    const char* outDir = paths[1];
    // The console's paths hold 256 bytes; one that doesn't fit would be ignored by CtrPack_setDataWinPath.
    if (strlen(dataWinArg) >= PACK_PATH_MAX || strlen(outDir) + 1 + 16 >= PACK_PATH_MAX) {
        fprintf(stderr, "texprep: path too long (at most %d bytes)\n", PACK_PATH_MAX - 17);
        return 2;
    }
    if (!texprep_output_safe(dataWinArg, outDir)) {
        fprintf(stderr, "texprep: choose a cache folder outside the source game folder (paths must be resolvable)\n");
        return 2;
    }

    double start = seconds();
    DataWin* dw = calloc(1, sizeof(DataWin));
    if (dw == NULL || !loadDataWin(dataWinArg, dw)) return 2;
    texprepUtCache = dw->gen8.wadVersion == 16;

    // As main.c and the prepare screen do, in that order.
    CtrPack_setDataWinPath(dataWinArg);
    CtrPack_setExact(exact);
    if (exact && CtrPack_lossy()) {
        fprintf(stderr, "texprep: exact tiles are not available in this build\n");
        return 2;
    }
    // The cache folder goes where asked instead of beside data.win.
    snprintf(cacheDir, sizeof(cacheDir), "%s", outDir);
    CtrPack_init();
    CtrPackKey key = CtrPack_keyFor(dw);

    for (unsigned n = 0; n < 2; n++) {
        AlphaZeroScope* scope = &alphaZero[n];
        if (!scope->enabled) continue;
        // Refresh the selected page on each host invocation. UT's epoch also invalidates legacy page headers
        // and complete markers; DR keeps its original epochs. Publication removes complete before invalidation
        // and writes it last. Other pages retain their normal version/key-based cache behavior.
        char marker[PACK_PATH_MAX + 16], page[PACK_PATH_MAX + 16];
        markerPath(marker, sizeof(marker));
        pathFor(page, sizeof(page), scope->page, "dtx");
        pathFor(scope->temporary, sizeof(scope->temporary), scope->page, "tmp");
        if (!CtrCachePublish_begin(marker, false) || (remove(page) != 0 && errno != ENOENT)) {
            fprintf(stderr, "texprep: could not refresh alpha-zero page %u\n", (unsigned) scope->page);
            return 1;
        }
        printf("texprep: alpha-zero RGB: page %u rectangle %u,%u %ux%u\n", (unsigned) scope->page,
               (unsigned) scope->x, (unsigned) scope->y, (unsigned) scope->w, (unsigned) scope->h);
    }

    CtrPackWorker* worker = CtrPackWorker_start(dw, &key, true);
    if (worker == NULL) {
        fprintf(stderr, "texprep: could not start (is %s readable, and %s writable?)\n", dataWinArg, outDir);
        return 1;
    }
    uint32_t done = 0, total = 0, built = 0;
    uint64_t bytes = 0;
    for (;;) {
        CtrPackResult result;
        while (CtrPackWorker_poll(worker, &result)) {
            if (result.built) built++;
            bytes += result.size;
            CtrPacked_free(result.packed);
        }
        CtrPackWorker_progress(worker, &done, &total);
        if (done >= total) break;
        sleepMs(2);
    }
    CtrPackWorker_stop(worker);
    CtrQoi_releaseWorkspace();

    bool complete = CtrPack_isComplete(&key, dw->txtr.count);
    printf("texprep: %s: %u pages (%u prepared now), %.1f MB, %.2f s, %s, %s\n", dataWinArg, (unsigned) total, (unsigned) built,
           (double) bytes / (1024.0 * 1024.0), seconds() - start, exact ? "exact" : "rounded", complete ? "complete" : "INCOMPLETE");
    return complete ? 0 : 1;
}

#ifdef _WIN32
#undef main
int wmain(int argc, wchar_t** wide) {
    char** argv = calloc((size_t) argc + 1, sizeof(char*));
    if (!argv) return 2;
    int result = 2;
    for (int i = 0; i < argc; i++) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, NULL, 0, NULL, NULL);
        if (!size || !(argv[i] = malloc((size_t) size))) goto done;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide[i], -1, argv[i], size, NULL, NULL)) goto done;
    }
    result = texprep_main(argc, argv);
done:
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
    return result;
}
#endif
