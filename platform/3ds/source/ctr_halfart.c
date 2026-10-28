// halfart (build argument halfart=point|box, prototype, flowhalf): art stored at half resolution, from the patch
// tools/analysis/halfart_pack.py makes at build time (romfs:/halfart/<game>.dthr; its comment has the format). Only
// in its own builds, all late code, through three link wraps:
//   * CtrPack_keyFor: at the first call for a data.win the patch was made from (before the texture cache or the setup
//     screen packs a page), each listed texture page item's source rectangle becomes its half-size copy and its
//     target size twice that: the same geometry, drawn from a quarter of the texels. The key itself is unchanged, so
//     the font, item and Crisp art tables still match, and the cache folder below keeps the pages apart instead.
//   * CtrPack_setTestRun (main.c, once at startup, before any page is read): the SD texture cache goes in a folder
//     of the patch's own, cache_lt_<n> with n = CTRPORT_HALFART_CACHE_RUN (the patch's SHA-256, top bit set; CMake)
//     XOR the row's texture_cache_run. Settings give at most 10^9 < 2^31, so no full-art build's folder (cache, or
//     cache_lt_<run>) and no other patch's is ever read: pages made from other art under the same key never load.
//     Through the existing test-run name, so ctr_texture_pack.c keeps its bytes and layout.
//   * CtrPng_decode: a page the patch names (by a hash of its first 4096 bytes) has, in each strip of rows on its way
//     to the texture cache's packer or loader, the listed items' full-size rectangles cleared and their copies put in
//     from the patch. The PNG decoder unfilters the next strip's first row from this strip's last row after the
//     callback returns, so that row is put back as decoded first.
// UNDERTALE's pages are PNG; QOI pages (DELTARUNE) are not patched here, so DELTARUNE gets no patch from the builder.
#ifdef CTRPORT_HALFART
#include "ctr_png.h"
#include "ctr_texture_pack.h"
#include "data_win.h"
#include "log.h"
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HAL __attribute__((noinline, noclone, section(".text_flowhalf_late")))
#define HAB __attribute__((section(".bss.flowhalf_late")))
#define HAR __attribute__((section(".rodata.late.flowhalf")))

typedef struct {
    uint32_t tpag;
    uint16_t old[6]; // source x, y, w, h, target w, h
    uint16_t now[6];
} HaItem;

typedef struct {
    uint32_t page, hash, first, count;
} HaPage;

typedef struct {
    uint16_t kind, pad, x, y, w, h;
    uint32_t data;
} HaOp;

typedef struct {
    char magic[4];
    uint32_t version, dataWinSize, gameId;
    uint64_t timestamp;
    uint32_t items, pages, ops, reserved;
} __attribute__((packed)) HaHeader;

static HAB struct {
    bool tried, ok;
    const DataWin* patched;
    HaHeader head;
    HaItem* items;
    HaPage* pages;
    HaOp* ops;
} ha;

static const char haPath[] HAR = CTRPORT_HALFART_PATH;
static const char haMount[] HAR = "romfs";
static const char haLoaded[] HAR = "[halfart] %s: %lu items, %lu pages, %lu ops\n";
static const char haNone[] HAR = "[halfart] %s: not read\n";
static const char haApplied[] HAR = "[halfart] %lu of %lu items now drawn from their half-size copies\n";
static const char haStale[] HAR = "[halfart] the patch is for another data.win: nothing changed\n";
static const char haMismatch[] HAR = "[halfart] item %lu does not match the patch: nothing changed\n";
static const char haPageLog[] HAR = "[halfart] page %lu: %lu rectangles cleared or copied in on its way to the cache (%ld)\n";

CtrPackKey __real_CtrPack_keyFor(const DataWin* dataWin);
CtrPackKey __wrap_CtrPack_keyFor(const DataWin* dataWin) HAL;
CtrPngStatus __real_CtrPng_decode(const uint8_t* png, size_t size, int32_t stripRows, CtrPngStripFn fn, void* user);
CtrPngStatus __wrap_CtrPng_decode(const uint8_t* png, size_t size, int32_t stripRows, CtrPngStripFn fn, void* user) HAL;
void __real_CtrPack_setTestRun(unsigned run, bool direct);
void __wrap_CtrPack_setTestRun(unsigned run, bool direct) HAL;

static HAL bool haRead(FILE* f, void** out, size_t bytes) {
    *out = malloc(bytes > 0 ? bytes : 1);
    return *out != nullptr && fread(*out, 1, bytes, f) == bytes;
}

Result CtrRomfs_init(const char* name);

static HAL void haLoad(void) {
    ha.tried = true;
    // The first key is asked for before the AOT module mounts the RomFS (ctr_romfs.c mounts it once, for everyone).
    FILE* f = R_SUCCEEDED(CtrRomfs_init(haMount)) ? fopen(haPath, "rb") : nullptr;
    bool ok = f != nullptr && fread(&ha.head, sizeof(ha.head), 1, f) == 1 && memcmp(ha.head.magic, "DTHR", 4) == 0 && ha.head.version == 1 &&
              haRead(f, (void**) &ha.items, (size_t) ha.head.items * sizeof(HaItem)) &&
              haRead(f, (void**) &ha.pages, (size_t) ha.head.pages * sizeof(HaPage)) &&
              haRead(f, (void**) &ha.ops, (size_t) ha.head.ops * sizeof(HaOp));
    if (f != nullptr) fclose(f);
    if (!ok) {
        logInfo(haNone, haPath);
        return;
    }
    ha.ok = true;
    logInfo(haLoaded, haPath, (unsigned long) ha.head.items, (unsigned long) ha.head.pages, (unsigned long) ha.head.ops);
}

static HAL void haPatch(DataWin* dw, CtrPackKey key) {
    ha.patched = dw;
    if (key.dataWinSize != ha.head.dataWinSize || key.gameId != ha.head.gameId || key.timestamp != ha.head.timestamp) {
        logInfo(haStale);
        ha.ok = false;
        return;
    }
    for (uint32_t i = 0; ha.head.items > i; i++) {
        const HaItem* it = &ha.items[i];
        const TexturePageItem* t = it->tpag < dw->tpag.count ? &dw->tpag.items[it->tpag] : nullptr;
        if (t == nullptr || t->sourceX != it->old[0] || t->sourceY != it->old[1] || t->sourceWidth != it->old[2] || t->sourceHeight != it->old[3] ||
            t->targetWidth != it->old[4] || t->targetHeight != it->old[5]) {
            logInfo(haMismatch, (unsigned long) it->tpag);
            ha.ok = false;
            return;
        }
    }
    for (uint32_t i = 0; ha.head.items > i; i++) {
        const HaItem* it = &ha.items[i];
        TexturePageItem* t = &dw->tpag.items[it->tpag];
        t->sourceX = it->now[0];
        t->sourceY = it->now[1];
        t->sourceWidth = it->now[2];
        t->sourceHeight = it->now[3];
        t->targetWidth = it->now[4];
        t->targetHeight = it->now[5];
    }
    logInfo(haApplied, (unsigned long) ha.head.items, (unsigned long) dw->tpag.count);
}

CtrPackKey __wrap_CtrPack_keyFor(const DataWin* dataWin) {
    CtrPackKey key = __real_CtrPack_keyFor(dataWin);
    if (!ha.tried) haLoad();
    if (ha.ok && dataWin != ha.patched) haPatch((DataWin*) dataWin, key);
    return key;
}

typedef struct {
    CtrPngStripFn fn;
    void* user;
    const HaPage* page;
    size_t stride;
    uint8_t* last;
    FILE* f;
} HaStrip;

static HAL bool haStrip(void* user, const uint8_t* rows, int32_t y, int32_t count) {
    HaStrip* s = user;
    uint8_t* r = (uint8_t*) rows;
    bool saved = false;
    for (uint32_t i = 0; s->page->count > i; i++) {
        const HaOp* op = &ha.ops[s->page->first + i];
        int32_t a = op->y > y ? op->y : y;
        int32_t b = op->y + op->h < y + count ? op->y + op->h : y + count;
        if (a >= b) continue;
        if (!saved) {
            memcpy(s->last, r + (size_t) (count - 1) * s->stride, s->stride);
            saved = true;
        }
        size_t bytes = (size_t) op->w * 4;
        if (op->kind == 0) {
            for (int32_t row = a; b > row; row++) memset(r + (size_t) (row - y) * s->stride + (size_t) op->x * 4, 0, bytes);
            continue;
        }
        if (s->f == nullptr) {
            s->f = fopen(haPath, "rb");
            if (s->f == nullptr) break;
            setvbuf(s->f, nullptr, _IOFBF, 32768);
        }
        if (fseek(s->f, (long) (op->data + (uint32_t) (a - op->y) * bytes), SEEK_SET) != 0) continue;
        for (int32_t row = a; b > row; row++) {
            if (fread(r + (size_t) (row - y) * s->stride + (size_t) op->x * 4, 1, bytes, s->f) != bytes) break;
        }
    }
    bool ok = s->fn(s->user, rows, y, count);
    if (saved) memcpy(r + (size_t) (count - 1) * s->stride, s->last, s->stride);
    return ok;
}

static HAL uint32_t haHash(const uint8_t* data, size_t size) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; size > i; i++) h = (h ^ data[i]) * 0x01000193u;
    return h;
}

CtrPngStatus __wrap_CtrPng_decode(const uint8_t* png, size_t size, int32_t stripRows, CtrPngStripFn fn, void* user) {
    if (!ha.ok || ha.patched == nullptr || 4096 > size) return __real_CtrPng_decode(png, size, stripRows, fn, user);
    uint32_t h = haHash(png, 4096);
    const HaPage* page = nullptr;
    for (uint32_t i = 0; ha.head.pages > i; i++) {
        if (ha.pages[i].hash == h) page = &ha.pages[i];
    }
    int32_t width = 0, height = 0;
    if (page == nullptr || !CtrPng_info(png, size, &width, &height) || 0 >= width) return __real_CtrPng_decode(png, size, stripRows, fn, user);
    HaStrip s = { fn, user, page, (size_t) width * 4, malloc((size_t) width * 4), nullptr };
    if (s.last == nullptr) return CTR_PNG_NO_MEMORY;
    CtrPngStatus status = __real_CtrPng_decode(png, size, stripRows, haStrip, &s);
    logInfo(haPageLog, (unsigned long) page->page, (unsigned long) page->count, (long) status);
    if (s.f != nullptr) fclose(s.f);
    free(s.last);
    return status;
}

static const char haCacheLog[] HAR = "[halfart] texture cache folder cache_lt_%lu (texture_cache_run %lu): this patch's own\n";

void __wrap_CtrPack_setTestRun(unsigned run, bool direct) {
    unsigned own = (unsigned) (CTRPORT_HALFART_CACHE_RUN) ^ run;
    logInfo(haCacheLog, (unsigned long) own, (unsigned long) run);
    __real_CtrPack_setTestRun(own, direct);
}
#endif
