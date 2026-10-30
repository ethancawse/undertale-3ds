// LAB ONLY (build.sh undertale <o3ds|n3ds> flowdiag): flowstream round 3's diag. Never a release input; without the
// word this file is not compiled and its define reaches no object.
//
// What tilecensus's diag (tilecensus) does not log, for planning first-draw prefetch (flowstream round 3,
// firstuse's schedule): at every draw that loads tiles (the renderer's CtrTextures_ensureTiles/-But calls, wrapped),
// one line per tile loaded, with
//   - frame, room, frames since the room change, page, tile, where it came from (ram/read/sd/image), the call's ms,
//   - its texture bytes (tex.size) and its compressed bytes (tile->size),
//   - whether the room plan lists it for this room and its first-use bucket (plan; 255: not in the plan), whether the
//     room's record (rooms.dtm or the plan seed: what the room load reads first) has it (mem), whether it was wanted
//     before the draw, and whether the floor guard evicted it earlier in this room visit (ev),
//   - free linear memory, free heap (the reader's own measure; it carries tile bytes only above 8 MiB of it), packed
//     bytes in RAM, and whether a fill was in progress.
// CtrFlowdiag_guard logged fs3-fs4's floor guard decisions (PCM flush that met the floor, tile evict) with the same memory
// figures and the evicted tile's plan bucket; fs5 keeps the stock eviction, so nothing calls it now.
// Nothing here changes state the game or the cache reads: it reads flags before and after the original call.

#if defined(CTRPORT_FLOWSTREAM_DIAG) && defined(ENABLE_WAD16)
#include <3ds.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ctr_textures.h"
#include "log.h"
#include "roomplan_format.h"
#include "runner.h"
#include "stb_ds.h"

#define FD_TEXT __attribute__((noinline, section(".text.flowstream_late")))
#define FD_CONST __attribute__((aligned(4), section(".rodata.flowstream_in_text")))
#define FD_BSS __attribute__((section(".bss.flowstream_late")))
#define FD_EVICTED 256u
#define FD_KEY_MASK 0x07FFFFFFu // ctr_textures.c's room tile key mask: page << 8 | tile

extern u32 __ctru_heap_size;
const uint8_t* RoomPlan_utstallPlan(const CtrTextureCache* cache);
void CtrFlowdiag_guard(CtrTextureCache* cache, uint32_t kind, uint32_t page, uint32_t tile, uint32_t rank, uint32_t bytes);

static FD_BSS struct {
    const CtrTextureCache* cache;
    uint32_t roomStart;
    uint32_t count;
    uint32_t evicted[FD_EVICTED]; // page << 8 | tile evicted by the guard this room visit (the first FD_EVICTED)
} fd;

static const char fdLoadLog[] FD_CONST =
    "Flowdiag: load f%u room %ld +%u page %u tile %u %s %u.%u ms; tex %u B packed %u B; plan %u mem %u wanted %u ev %u; "
    "linear %u KB heap %u KB packedram %u KB filling %u\n";
static const char fdGuardLog[] FD_CONST =
    "Flowdiag: guard f%u room %ld +%u %s %u:%u r%u %u B plan %u mem %u; linear %u KB heap %u KB packedram %u KB\n";
static const char fdSources[] FD_CONST = "ram\0\0read\0sd\0\0\0image";
static const char fdKinds[] FD_CONST = "flush\0evict";

static FD_TEXT uint32_t fdHeapFree(void) {
    struct mallinfo mi = mallinfo();
    uint32_t used = (uint32_t) mi.uordblks;
    return __ctru_heap_size > used ? __ctru_heap_size - used : 0;
}

static FD_TEXT int32_t fdRoom(const CtrTextureCache* cache) {
    const Runner* runner = cache->runner;
    if (runner == nullptr || 0 > runner->currentRoomIndex || cache->roomCount <= (uint32_t) runner->currentRoomIndex) return -1;
    return runner->currentRoomIndex;
}

static FD_TEXT void fdVisit(const CtrTextureCache* cache) {
    if (fd.cache == cache && fd.roomStart == cache->roomStartFrame) return;
    fd.cache = cache;
    fd.roomStart = cache->roomStartFrame;
    fd.count = 0;
}

// The tile's first-use bucket in any of the plan's scenes for this room (whole room or a battle group), 255 if none.
static FD_TEXT uint32_t fdPlan(const CtrTextureCache* cache, int32_t room, uint32_t key) {
    const uint8_t* plan = RoomPlan_utstallPlan(cache);
    if (plan == nullptr || 0 > room) return 255u;
    RoomPlanHeader h;
    memcpy(&h, plan, sizeof(h));
    size_t at = sizeof(h);
    uint32_t best = 255u;
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        memcpy(&scene, plan + at, sizeof(scene));
        if (scene.room == (uint32_t) room) {
            const uint8_t* tiles = plan + at + sizeof(scene);
            const uint8_t* buckets = tiles + (size_t) scene.tileCount * 4u;
            for (uint32_t i = 0; scene.tileCount > i; i++) {
                uint32_t entry;
                memcpy(&entry, tiles + (size_t) i * 4u, 4);
                if ((entry & FD_KEY_MASK) == key && buckets[i] < best) best = buckets[i];
            }
        }
        at += RoomPlan_sceneSize(&scene);
    }
    return best;
}

static FD_TEXT uint32_t fdMem(const CtrTextureCache* cache, int32_t room, uint32_t key) {
    if (0 > room || cache->roomTiles == nullptr) return 0;
    const uint32_t* entries = cache->roomTiles[room];
    for (ptrdiff_t i = 0; arrlen(entries) > i; i++) {
        if ((entries[i] & FD_KEY_MASK) == key) return 1;
    }
    return 0;
}

static FD_TEXT uint32_t fdEvicted(uint32_t key) {
    for (uint32_t k = 0; fd.count > k; k++) {
        if (fd.evicted[k] == key) return 1;
    }
    return 0;
}

FD_TEXT void CtrFlowdiag_guard(CtrTextureCache* cache, uint32_t kind, uint32_t page, uint32_t tile, uint32_t rank, uint32_t bytes) {
    fdVisit(cache);
    int32_t room = fdRoom(cache);
    uint32_t key = page << 8 | tile;
    if (kind == 1 && FD_EVICTED > fd.count && !fdEvicted(key)) fd.evicted[fd.count++] = key;
    logInfo(fdGuardLog, (unsigned) cache->frame, (long) room, (unsigned) (cache->frame - cache->roomStartFrame), fdKinds + kind * 6u,
            (unsigned) page, (unsigned) tile, (unsigned) rank, (unsigned) bytes, (unsigned) (kind == 1 ? fdPlan(cache, room, key) : 255u),
            (unsigned) (kind == 1 ? fdMem(cache, room, key) : 0u), (unsigned) (linearSpaceFree() / 1024u), (unsigned) (fdHeapFree() / 1024u),
            (unsigned) (cache->packedBytes / 1024u));
}

static inline __attribute__((always_inline)) bool fdMissing(const CtrPage* page, const CtrTile* tile) {
    return !tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY); // ctr_textures.c's tileMissing
}

// Before the call: the range's missing tiles (the first 64) and which were wanted. After it: those now resident.
static FD_TEXT void fdAfter(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, uint64_t missing,
                            uint64_t wanted, uint64_t t0, uint32_t ram, uint32_t read, uint32_t file) {
    uint32_t src = cache->fromRam.count != ram ? 0u : cache->fromRead.count != read ? 1u : cache->fromFile.count != file ? 2u : 3u;
    uint32_t tenths = (uint32_t) ((svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 10000u));
    uint32_t pageId = (uint32_t) (page - cache->pages);
    int32_t room = fdRoom(cache);
    uint32_t linearKb = linearSpaceFree() / 1024u, heapKb = fdHeapFree() / 1024u;
    fdVisit(cache);
    uint32_t k = 0;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx && 64u > k; tx++, k++) {
            if ((missing >> k & 1u) == 0) continue;
            uint32_t t = (uint32_t) (ty * page->tilesX + tx);
            const CtrTile* tile = &page->tiles[t];
            if (!tile->valid) continue;
            uint32_t key = pageId << 8 | t;
            logInfo(fdLoadLog, (unsigned) cache->frame, (long) room, (unsigned) (cache->frame - cache->roomStartFrame), (unsigned) pageId,
                    (unsigned) t, fdSources + src * 5u, (unsigned) (tenths / 10u), (unsigned) (tenths % 10u), (unsigned) tile->tex.size,
                    (unsigned) tile->size, (unsigned) fdPlan(cache, room, key), (unsigned) fdMem(cache, room, key),
                    (unsigned) (wanted >> k & 1u), (unsigned) fdEvicted(key), (unsigned) linearKb, (unsigned) heapKb,
                    (unsigned) (cache->packedBytes / 1024u), (unsigned) cache->filling);
        }
    }
}

void __real_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                       uint64_t drawnElsewise);
void __wrap_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                       uint64_t drawnElsewise);
void __real_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
void __wrap_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);

FD_TEXT void __wrap_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                               uint64_t drawnElsewise) {
    uint64_t missing = 0, wanted = 0;
    bool own = cache != nullptr && page != nullptr && page >= cache->pages && page < cache->pages + cache->pageCount && page->tiles != nullptr;
    if (own) {
        uint32_t k = 0;
        for (int32_t ty = ty0; ty1 >= ty; ty++) {
            for (int32_t tx = tx0; tx1 >= tx && 64u > k; tx++, k++) {
                if ((drawnElsewise >> k & 1u) != 0) continue;
                const CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
                if (!fdMissing(page, tile)) continue;
                missing |= 1ull << k;
                if (tile->wanted) wanted |= 1ull << k;
            }
        }
    }
    if (missing == 0) {
        __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
        return;
    }
    uint32_t ram = cache->fromRam.count, read = cache->fromRead.count, file = cache->fromFile.count;
    uint64_t t0 = svcGetSystemTick();
    __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
    fdAfter(cache, page, tx0, ty0, tx1, ty1, missing, wanted, t0, ram, read, file);
}

FD_TEXT void __wrap_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1) {
    // Its body is ensureTilesBut(..., 0) inside ctr_textures.o, which the wrap doesn't reach: the same steps here.
    uint64_t missing = 0, wanted = 0;
    bool own = cache != nullptr && page != nullptr && page >= cache->pages && page < cache->pages + cache->pageCount && page->tiles != nullptr;
    if (own) {
        uint32_t k = 0;
        for (int32_t ty = ty0; ty1 >= ty; ty++) {
            for (int32_t tx = tx0; tx1 >= tx && 64u > k; tx++, k++) {
                const CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
                if (!fdMissing(page, tile)) continue;
                missing |= 1ull << k;
                if (tile->wanted) wanted |= 1ull << k;
            }
        }
    }
    if (missing == 0) {
        __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
        return;
    }
    uint32_t ram = cache->fromRam.count, read = cache->fromRead.count, file = cache->fromFile.count;
    uint64_t t0 = svcGetSystemTick();
    __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
    fdAfter(cache, page, tx0, ty0, tx1, ty1, missing, wanted, t0, ram, read, file);
}
#endif
