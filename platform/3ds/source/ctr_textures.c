#include "ctr_textures.h"
#include "ctr_array.h"
#line 2

#include <malloc.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_audio_system.h"
#include "ctr_lz.h"
#include "ctr_memory.h"
#include "ctr_open_files.h"
#include "ctr_qoi.h"
#include "gettime.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"
#include "utils.h"

// After an allocation failure, wait this many frames before loading the page again.
#define OUT_OF_MEMORY_RETRY_FRAMES 30
// A page that isn't packed yet is decoded from its image whenever a draw needs
// a tile of it that isn't resident (hammer601, at the end of this file).

// Loads from RAM quicker than this aren't logged one by one.
#define QUIET_LOAD_MS 4.0
// A frame that spends this long loading tiles is logged.
#define HITCH_LOG_MS 50.0
// Loading the tiles a room wants (prefetchRoom) starts no new tile after this
// much of a frame; each frame loads at least one.
#define FILL_BUDGET_MS 4.0

// Packed copies in RAM may use this much heap, and at most a share of what
// is free at startup. Undertale's pages pack to about 14 MB in all, DELTARUNE
// Chapter 2's to 11 MB; a tile loads from a copy in RAM in about a quarter
// of the time a read from the SD card takes, and a room needs dozens.
// The share leaves out a reserve for what the heap needs on top: decoding a
// page from its image (a 2048x2048 page wants about 8 MB on top of the QOI
// workspace while it decodes) and the game's own growth, so heap freed
// elsewhere stays free for the peaks instead of mostly going to packed copies.
#define MB (1024u * 1024u)
#define PACKED_BUDGET_MAX (24u * MB)
#ifdef CTRPORT_TARGET_O3DS
// The Old 3DS reads its SD card at about 5 MB/s, so a page not in RAM costs
// the room that first draws from it 100 ms or more. Its heap is small, so
// the copies get what is free at startup less what the game takes on top
// as it plays (Undertale about 8 MB, with room loading's garbage): all of
// Undertale's pages in the 80 MB .cia, a few MB in a .3dsx's 64 MB.
#define PACKED_GAME_GROWTH (12u * MB)
#define PACKED_BUDGET_SHARE(free) ((free) > PACKED_GAME_GROWTH ? (free) - PACKED_GAME_GROWTH : 0)
#else
// The New 3DS likewise gets what is free at startup less a reserve for the
// game's growth as it plays. DELTARUNE Chapter 4's Titan fight raises the
// heap's high-water mark about 15 MB over the heap after loading (audio and
// the pieces the heap splits into included), so this leaves it 2-3 MB of
// the 36 MB heap. Chapter 2 then keeps 11 MB of pages in RAM: with a share of
// 3/8 (8 MB), entering the castle's transformed courtyard waited 218 ms for
// a page read from the SD card; with 9 MB, 190 ms; with 11 MB, not at all.
// Undertale's pages all fit either way.
#define PACKED_BUDGET_RESERVE (18u * MB)
#define PACKED_BUDGET_SHARE(free) ((free) > PACKED_BUDGET_RESERVE ? (free) - PACKED_BUDGET_RESERVE : 0)
#endif
// The heap is checked this often, and packed copies are dropped (least
// recently used first) while less than LOW_HEAP_BYTES of it is free: the
// game's allocations matter more than a quicker tile load. An allocation
// that fails between two checks has copies dropped there and then (the heap
// reliever), so this needn't cover a room's worth of growth. On the Old 3DS
// in a .3dsx's 64 MB it was 6 MB: Undertale's Flowey fight then had its page
// of sprites (4 MB packed) dropped, and loaded its tiles from the SD card,
// 28 loads in 880 ms, against 18 loads in 148 ms from RAM with 3 MB.
#define LOW_HEAP_BYTES (3u * MB)
#define HEAP_CHECK_INTERVAL_FRAMES 30
// A page requested from its cache file can be promoted before a later
// Step/Draw. Only wholly idle copies not used in this many frames may make
// room; resident and queued tiles protect their page's copy. A scene that keeps drawing
// from more pages than fit in RAM (Flowery's fight, 16 pages) then has most
// of them there, instead of reading a tile from the SD card at each turn
// (about 35 ms on a New 3DS XL, against 6-9 ms from RAM).
#define PACKED_KEEP_FRAMES 60
// Tiles fill linear memory up to their budget, which leaves a reserve for
// everything else that lives there, but that reserve isn't always enough.
// On a New 3DS (hw12) audio holds up to 7-8 MB of it (sound effects up to
// 6 MB, the music's stream rings), Azahar without DSP firmware none, and
// surfaces VRAM can't hold go there too. DELTARUNE Chapter 5's Flowery fight
// got down to 0.4 MB free with 51 MB of a 57 MB budget resident, and then a
// tile's texture couldn't be allocated. Each frame, tiles not drawn lately
// (and before them cached sound effects not played lately, see
// keepLinearFloor) make way until this much is free again, so that a sound
// effect, a surface or the next tile finds room without anything else
// failing first.
#define LINEAR_FLOOR_BYTES (3u * MB)
#define LINEAR_FLOOR_KEEP_FRAMES 2
// Tiles not drawn in this many frames are idle (see allocateTile).
#define ALLOC_IDLE_FRAMES 30

extern u32 __ctru_heap_size;

typedef enum {
    LOAD_OK,
    LOAD_BAD_DATA,
    LOAD_NO_MEMORY,
} LoadResult;

static double msSince(uint64_t start) {
    return (double) (nowNanos() - start) / 1e6;
}

static double megabytes(uint32_t bytes) {
    return (double) bytes / (1024.0 * 1024.0);
}

static uint32_t heapFree(void) {
    struct mallinfo mi = mallinfo();
    return __ctru_heap_size > (u32) mi.uordblks ? __ctru_heap_size - (u32) mi.uordblks : 0;
}

static uint32_t pageIdOf(const CtrTextureCache* cache, const CtrPage* page) {
    return (uint32_t) (page - cache->pages);
}

// Whether page is one of the cache's (pageIdOf is only meaningful then).
static bool cacheOwns(const CtrTextureCache* cache, const CtrPage* page) {
    uintptr_t at = (uintptr_t) page, first = (uintptr_t) cache->pages;
    return at >= first && first + (uintptr_t) cache->pageCount * sizeof(CtrPage) > at;
}

static int32_t tileCount(const CtrPage* page) {
    return page->tilesX * page->tilesY;
}

static uint32_t tileBytes(const CtrPage* page, const CtrTile* tile) {
    if (tile->format == CTR_TILE_EMPTY) return 0;
    return (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
}

// A tile that drawing needs and that isn't there: not resident, and not
// known to be fully transparent.
static bool tileMissing(const CtrPage* page, const CtrTile* tile) {
    return !tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY);
}

// ===[ Copies in VRAM ]===

// Frees a tile's copy in VRAM (its free is held back while the GPU may still
// read it, see ctr_memory.c). A batch holding the copy's texture draws the
// tile's own pixels from then on, which are the same.
static void dropMirror(CtrTextureCache* cache, CtrTile* tile) {
    if (tile->mirror == nullptr) return;
    CtrTileMirror* m = (CtrTileMirror*) tile->mirror;
    // A freed copy's VRAM may still be the target of a queued, not-started
    // copy, and a free while the GPU is idle isn't held back. Nothing in this
    // frame drew from the copy of a tile it didn't draw, so its copy goes; one
    // drawn from stays, and runs before anything the VRAM's next owner queues.
    if (tile->lastUsedFrame != cache->frame) CtrGpu_cancelCopy(m->copy, tile->tex.data, m->tex.data);
    vramFree(m->tex.data);
    m->tex.data = tile->tex.data;
    m->tile = nullptr;
    m->dead = true;
    cache->mirrorBytes -= m->tex.size;
    tile->mirror = nullptr;
}

uint32_t CtrTextures_takeHot(CtrTextureCache* cache, CtrTile** out, uint32_t max) {
    uint32_t n = cache->hotCount < max ? cache->hotCount : max;
    memcpy(out, cache->hot, n * sizeof(CtrTile*));
    cache->hotCount = 0;
    return n;
}

bool CtrTextures_addMirror(CtrTextureCache* cache, CtrTile* tile, void* vram) {
    if (!tile->valid || tile->mirror != nullptr) return false;
    for (uint32_t i = 0; CTR_MIRROR_MAX > i; i++) {
        CtrTileMirror* m = &cache->mirrors[i];
        if (m->tile != nullptr || m->dead) continue;
        _Static_assert(offsetof(CtrTileMirror, tex) == 0, "a tile's mirror pointer is its CtrTileMirror's");
        m->tex = tile->tex;
        m->tex.data = vram;
        m->tile = tile;
        m->copy = (CtrGpuQueued) { 0 };
        tile->mirror = &m->tex;
        cache->mirrorBytes += m->tex.size;
        return true;
    }
    return false;
}

void CtrTextures_noteMirrorCopy(CtrTile* tile, CtrGpuQueued copy) {
    if (tile->mirror != nullptr) ((CtrTileMirror*) tile->mirror)->copy = copy;
}

uint32_t CtrTextures_releaseMirrors(CtrTextureCache* cache, bool inUse) {
    uint32_t before = cache->mirrorBytes;
    for (uint32_t i = 0; CTR_MIRROR_MAX > i; i++) {
        CtrTile* tile = cache->mirrors[i].tile;
        if (tile != nullptr && (inUse || tile->lastUsedFrame != cache->frame)) dropMirror(cache, tile);
    }
    return before - cache->mirrorBytes;
}

// ===[ Residency ]===

// Tiles drawn in the current frame are never released (see oldestEvictable),
// so no batch holds a copy in VRAM being freed here.
static void releaseTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    if (!tile->valid) return;
    dropMirror(cache, tile);
    uint32_t bytes = tile->tex.size;
    C3D_TexDelete(&tile->tex);
    tile->valid = false;
    page->bytes -= bytes;
    page->residentTiles--;
    cache->residentBytes -= bytes;
}

static void releasePage(CtrTextureCache* cache, CtrPage* page) {
    if (page->tiles == nullptr) return;
    for (int32_t i = 0; tileCount(page) > i; i++) releaseTile(cache, page, &page->tiles[i]);
}

// The oldest frame any evictable tile last used before frame usedBefore was
// last used in; false if there is no such tile. Tiles used in the current
// frame may still be referenced by queued GPU commands and are never evicted.
static bool oldestEvictable(const CtrTextureCache* cache, uint32_t usedBefore, uint32_t* oldest) {
    if (usedBefore > cache->frame) usedBefore = cache->frame;
    bool found = false;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        const CtrPage* page = &cache->pages[p];
        if (page->residentTiles == 0) continue;
        for (int32_t i = 0; tileCount(page) > i; i++) {
            const CtrTile* tile = &page->tiles[i];
            if (!tile->valid || tile->lastUsedFrame >= usedBefore) continue;
            if (!found || *oldest > tile->lastUsedFrame) *oldest = tile->lastUsedFrame;
            found = true;
        }
    }
    return found;
}

// Evicts a tile, noting whether the room is thrashing (see
// CtrTextures_roomThrashing).
static void evictTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    if (tile->lastUsedFrame >= cache->roomStartFrame && tile->lastUsedFrame + CTR_THRASH_FRAMES >= cache->frame) cache->roomThrashed = true;
    releaseTile(cache, page, tile);
    cache->evictionCount++;
}

// Evicts least recently used tiles last used before frame usedBefore until
// `needed` more bytes fit in the budget.
static void evictUsedBefore(CtrTextureCache* cache, uint32_t needed, uint32_t usedBefore) {
    while (cache->residentBytes + needed > cache->budgetBytes) {
        // Find the oldest frame any evictable tile was used in, then evict
        // the tiles last used then.
        uint32_t oldest = 0;
        if (!oldestEvictable(cache, usedBefore, &oldest)) return;
        for (uint32_t p = 0; cache->pageCount > p; p++) {
            CtrPage* page = &cache->pages[p];
            if (page->residentTiles == 0) continue;
            for (int32_t i = 0; tileCount(page) > i; i++) {
                CtrTile* tile = &page->tiles[i];
                if (!tile->valid || tile->lastUsedFrame != oldest) continue;
                evictTile(cache, page, tile);
                if (cache->budgetBytes >= cache->residentBytes + needed) return;
            }
        }
    }
}

static void evict(CtrTextureCache* cache, uint32_t needed) {
    evictUsedBefore(cache, needed, cache->frame);
}

// Makes room for `needed` bytes by evicting the tiles of other pages that
// were loaded along with their page's image and never used since. Returns
// whether they fit now. Pages decoded in this frame keep theirs: the room
// being entered decodes several pages, and they'd only evict each other's.
static bool evictUnusedExtras(CtrTextureCache* cache, const CtrPage* keep, uint32_t needed) {
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* page = &cache->pages[p];
        if (page == keep || page->residentTiles == 0) continue;
        if (page->imageDecoded && page->imageFrame == cache->frame) continue;
        for (int32_t i = 0; tileCount(page) > i; i++) {
            if (cache->budgetBytes >= cache->residentBytes + needed) return true;
            CtrTile* tile = &page->tiles[i];
            if (!tile->valid || tile->lastUsedFrame != 0) continue;
            releaseTile(cache, page, tile);
            cache->evictionCount++;
        }
    }
    return cache->budgetBytes >= cache->residentBytes + needed;
}

// Evicts the least recently used tile (not the current frame's) whose
// texture is at least `bytes`, so that a texture that size fits in the hole
// it leaves however fragmented linear memory is. False if there is none.
static bool evictOneFitting(CtrTextureCache* cache, uint32_t bytes) {
    CtrPage* victimPage = nullptr;
    CtrTile* victim = nullptr;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* page = &cache->pages[p];
        if (page->residentTiles == 0) continue;
        for (int32_t i = 0; tileCount(page) > i; i++) {
            CtrTile* tile = &page->tiles[i];
            if (!tile->valid || tile->lastUsedFrame >= cache->frame || bytes > tile->tex.size) continue;
            if (victim != nullptr && tile->lastUsedFrame >= victim->lastUsedFrame) continue;
            victimPage = page;
            victim = tile;
        }
    }
    if (victim == nullptr) return false;
    evictTile(cache, victimPage, victim);
    return true;
}

static bool initTile(const CtrPage* page, CtrTile* tile) {
    if (!C3D_TexInit(&tile->tex, (u16) page->tileW, (u16) page->tileH, CtrPack_gpuFormat((CtrTileFormat) tile->format))) return false;
    C3D_TexSetFilter(&tile->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&tile->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return true;
}

static void unreserveTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile);

// Allocates a non-empty tile's texture, making room in the budget first. With
// mayEvict false, only if it fits without evicting anything. On running out
// of linear memory, the page is not loaded again for a while.
static bool allocateTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile, bool mayEvict) {
    // Reserved for a read that has finished (see reserveTile): allocated
    // and counted already, unless the tile's format changed since.
    if (tile->reserved && tile->tex.size == tileBytes(page, tile) && tile->tex.fmt == CtrPack_gpuFormat((CtrTileFormat) tile->format)) {
        tile->reserved = false;
        tile->valid = true;
        page->residentTiles++;
        return true;
    }
    unreserveTile(cache, page, tile);
    uint32_t bytes = tileBytes(page, tile);
    if (!mayEvict) {
        if (cache->residentBytes + bytes > cache->budgetBytes || !initTile(page, tile)) return false;
    } else {
        evict(cache, bytes);
        // The budget is a soft limit; linear memory is the hard one. The
        // least recently used tiles make way until the tile fits. Freeing
        // everything evictable at once instead, a scene that keeps surfaces
        // in linear memory (DELTARUNE Chapter 3's Roaring Knight) loaded its
        // tiles from the SD card again every few frames, for a second each.
        // Tiles idle for a while go that way, but of those drawn lately one
        // at least as big makes way where there is one: linear memory gets
        // fragmented, and evicting by age, a whole frame's tiles at a time,
        // the Old 3DS Flowery fight evicted up to 65 tiles (10 MB) drawn the
        // frame before for one tile, in each of several frames.
        while (!initTile(page, tile)) {
            if (!CtrTextures_evictOldestIdle(cache, ALLOC_IDLE_FRAMES) && !evictOneFitting(cache, bytes) && !CtrTextures_evictOldest(cache)) {
                logWarn("Textures: out of linear memory for a tile of page %u (%.1f MB resident, %.1f MB free)\n", pageIdOf(cache, page),
                        megabytes(cache->residentBytes), megabytes(linearSpaceFree()));
                page->retryFrame = cache->frame + OUT_OF_MEMORY_RETRY_FRAMES;
                return false;
            }
        }
    }
    tile->valid = true;
    bytes = tile->tex.size;
    page->bytes += bytes;
    page->residentTiles++;
    cache->residentBytes += bytes;
    return true;
}

// Allocates a tile's texture for the page's read to decode into (the tile's
// format must be known). It counts against the budget like a resident tile,
// but isn't drawn (or evicted) until the read is settled. Returns the
// texture memory, or nullptr.
static uint8_t* reserveTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    if (tile->valid || tile->reserved || tile->format == CTR_TILE_EMPTY) return nullptr;
    if (!allocateTile(cache, page, tile, true)) return nullptr;
    tile->valid = false;
    tile->reserved = true;
    page->residentTiles--;
    return tile->tex.data;
}

static void unreserveTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    if (!tile->reserved) return;
    uint32_t bytes = tile->tex.size;
    C3D_TexDelete(&tile->tex);
    tile->reserved = false;
    page->bytes -= bytes;
    cache->residentBytes -= bytes;
}

// ===[ Packed copies in RAM ]===

static void stopWorkerKeeping(CtrTextureCache* cache) {
    if (cache->worker != nullptr) CtrPackWorker_setKeepPacked(cache->worker, false);
    cache->workerKeepsPacked = false;
}

// Whether the worker has pages left to look at (read, or decode and pack).
static bool workerBusy(CtrTextureCache* cache) {
    if (cache->worker == nullptr) return false;
    uint32_t done = 0;
    uint32_t total = 0;
    CtrPackWorker_progress(cache->worker, &done, &total);
    return total > done;
}

// Whether the worker is still reading cache files into RAM: a page it hasn't
// got to yet will be in RAM soon, so the game thread reads only the tiles it
// needs now instead of the whole file.
static bool workerBringingPages(CtrTextureCache* cache) {
    return cache->workerKeepsPacked && workerBusy(cache);
}

static void releaseRead(CtrTextureCache* cache, CtrPage* page);

static void dropPacked(CtrTextureCache* cache, CtrPage* page) {
    // A decode of the copy must be done with it first.
    if (page->read != nullptr && page->read->src != nullptr) releaseRead(cache, page);
    CtrPacked_free(page->packed);
    cache->packedBytes -= page->packedSize;
    page->packed = nullptr;
    page->packedSize = 0;
}

// The page whose packed copy was used longest ago, leaving out `keep` and
// pages drawn from in this frame or the last; nullptr if there's none.
static CtrPage* packedVictim(CtrTextureCache* cache, const CtrPage* keep) {
    CtrPage* victim = nullptr;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        CtrPage* p = &cache->pages[i];
        if (p->packed == nullptr || p == keep || p->lastUsedFrame + 1 >= cache->frame) continue;
        if (victim == nullptr || victim->lastUsedFrame > p->lastUsedFrame) victim = p;
    }
    return victim;
}

// Promotion may reclaim only wholly idle copies. In particular, do not use
// dropPacked's ability to cancel/wait for a decoder, nor evict a copy that backs
// any resident or queued tile. This policy is serviced before Step/Draw only.
static __attribute__((noinline)) bool idleForPromotion(const CtrTextureCache* cache, const CtrPage* page, const CtrPage* keep) {
    if (page == keep || page == cache->busyPage || page->packed == nullptr || page->read != nullptr ||
        page->promotionPending || page->residentTiles != 0 ||
        (cache->wholeRead != nullptr && page == &cache->pages[cache->wholePage]) ||
        cache->frame - page->lastUsedFrame <= PACKED_KEEP_FRAMES) return false;
    for (int32_t i = 0; page->layoutKnown && i < page->tilesX * page->tilesY; i++) {
        const CtrTile* tile = &page->tiles[i];
        if (tile->valid || tile->reserved || tile->requested || tile->wanted) return false;
    }
    return true;
}

// Admit against BOTH packed budget and reader heap headroom. First prove that
// enough eligible bytes exist, so a doomed promotion evicts nothing. Then free
// LRU copies only until both limits are satisfied. No reads are waited for.
static bool makePromotionRoom(CtrTextureCache* cache, const CtrPage* page, uint32_t size) {
    if (size > cache->packedBudgetBytes) return false;
    uint64_t held = (uint64_t) cache->packedBytes + cache->wholeBytes;
    uint64_t heapNeeded = (uint64_t) size + CtrTileReader_heapReserve();
    uint32_t freeBytes = heapFree();
    uint64_t needed = heapNeeded > freeBytes ? heapNeeded - freeBytes : 0;
    if (held + size > cache->packedBudgetBytes && held + size - cache->packedBudgetBytes > needed)
        needed = held + size - cache->packedBudgetBytes;
    uint64_t eligible = 0;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        const CtrPage* p = &cache->pages[i];
        if (idleForPromotion(cache, p, page)) eligible += p->packedSize;
    }
    if (needed > eligible) return false;
    while (heapNeeded > heapFree() || (uint64_t) cache->packedBytes + cache->wholeBytes + size > cache->packedBudgetBytes) {
        CtrPage* victim = nullptr;
        for (uint32_t i = 0; i < cache->pageCount; i++) {
            CtrPage* p = &cache->pages[i];
            if (idleForPromotion(cache, p, page) && (victim == nullptr || p->lastUsedFrame < victim->lastUsedFrame)) victim = p;
        }
        if (victim == nullptr) return false; // heap growth on another thread: ordinary load remains the fallback
        uint32_t bytes = victim->packedSize;
        logInfo("Textures: reclaim idle packed page %u (%u KB) for promotion of page %u\n",
                (unsigned) (victim - cache->pages), (unsigned) (bytes / 1024), (unsigned) (page - cache->pages));
        dropPacked(cache, victim);
        cache->packedReclaims++;
        cache->packedReclaimBytes += bytes;
    }
    return true;
}

// Keeps a packed copy of the page in RAM, taking ownership of `packed`. Makes
// room by dropping copies of pages used less recently than this one (and not
// in this frame or the last); if that isn't enough, the copy is freed
// instead. Returns whether it was kept.
static bool keepPacked(CtrTextureCache* cache, CtrPage* page, CtrPacked* packed) {
    uint32_t size = packed->size;
    if (page->packed != nullptr) {
        CtrPacked_free(packed);
        return false;
    }
    while (cache->packedBytes + cache->wholeBytes + size > cache->packedBudgetBytes) {
        CtrPage* victim = packedVictim(cache, page);
        if (victim == nullptr || victim->lastUsedFrame >= page->lastUsedFrame) {
            CtrPacked_free(packed);
            // RAM is full of pages at least as useful: stop the worker
            // reading files into RAM just to have them thrown away.
            stopWorkerKeeping(cache);
            return false;
        }
        dropPacked(cache, victim);
    }
    page->packed = packed;
    page->packedSize = size;
    cache->packedBytes += size;
    return true;
}

// Drops packed copies, least recently used first, while the heap is low, and
// keeps the budget at what is left so that they aren't read back in.
static void relieveHeap(CtrTextureCache* cache) {
    if (cache->packedBytes == 0) return;
    uint32_t freeBytes = heapFree();
    if (freeBytes >= LOW_HEAP_BYTES) return;
    uint32_t before = cache->packedBytes;
    while (cache->packedBytes > 0 && LOW_HEAP_BYTES > freeBytes + (before - cache->packedBytes)) {
        CtrPage* victim = nullptr;
        for (uint32_t i = 0; cache->pageCount > i; i++) {
            CtrPage* p = &cache->pages[i];
            if (p->packed == nullptr) continue;
            if (victim == nullptr || victim->lastUsedFrame > p->lastUsedFrame) victim = p;
        }
        dropPacked(cache, victim);
    }
    cache->packedBudgetBytes = cache->packedBytes;
    stopWorkerKeeping(cache);
    logInfo("Textures: heap low (%.1f MB free), dropped %.1f MB of packed pages; %.1f MB kept\n", megabytes(freeBytes),
            megabytes(before - cache->packedBytes), megabytes(cache->packedBytes));
}

// The heap reliever (ctr_memory.c): an allocation of `wanted` bytes failed
// on the game thread, between two of the checks above (a room loading, say).
// Frees the idle decoding workspace, then packed copies least recently used
// first, until `wanted` and a margin more have gone, as the freed blocks
// needn't lie together. Leaves the copy of the page being loaded, which may
// be in use. Returns whether anything was freed, for another try.
static bool relieveFailedAlloc(void* context, size_t wanted) {
    CtrTextureCache* cache = (CtrTextureCache*) context;
    size_t target = wanted + MB;
    size_t freed = CtrQoi_releaseWorkspaceIfIdle();
    uint32_t before = cache->packedBytes;
    while (target > freed) {
        CtrPage* victim = nullptr;
        for (uint32_t i = 0; cache->pageCount > i; i++) {
            CtrPage* p = &cache->pages[i];
            if (p->packed == nullptr || p == cache->busyPage) continue;
            if (victim == nullptr || victim->lastUsedFrame > p->lastUsedFrame) victim = p;
        }
        if (victim == nullptr) break;
        freed += victim->packedSize;
        dropPacked(cache, victim);
    }
    if (freed == 0) return false;
    if (before > cache->packedBytes) {
        cache->packedBudgetBytes = cache->packedBytes;
        stopWorkerKeeping(cache);
    }
    logInfo("Textures: a %u KB allocation failed, freed %.1f MB (%.1f MB of packed pages, %.1f MB kept)\n", (unsigned) (wanted / 1024),
            megabytes((uint32_t) freed), megabytes(before - cache->packedBytes), megabytes(cache->packedBytes));
    return true;
}

// ===[ Layout ]===

// Gives the page its size and tile grid, with no tiles loaded.
static void setLayout(CtrPage* page, const CtrPageLayout* layout) {
    page->width = layout->width;
    page->height = layout->height;
    page->tileW = layout->tileW;
    page->tileH = layout->tileH;
    page->tilesX = layout->tilesX;
    page->tilesY = layout->tilesY;
    page->tiles = safeCalloc((size_t) tileCount(page), sizeof(CtrTile));
    page->layoutKnown = true;
}

static bool sameGrid(const CtrPage* page, const CtrPageLayout* layout) {
    return page->width == layout->width && page->height == layout->height && page->tileW == layout->tileW && page->tileH == layout->tileH &&
           page->tilesX == layout->tilesX && page->tilesY == layout->tilesY;
}

// Where a tile load's time went, for its log line: reset as each load starts.
// Only the game thread loads tiles.
typedef struct {
    uint64_t openNanos;  // opening cache files (and closing the ones they replace)
    uint64_t readNanos;  // reading from them
    uint64_t checkNanos; // checking tiles against their checksums
    uint64_t allocNanos; // allocating textures, evicting tiles to make room
    uint64_t unpackNanos;
    uint64_t flushNanos; // flushing the textures from the data cache
    uint32_t opens;
    uint32_t reads;
} LoadSteps;

static LoadSteps loadSteps;

static void closeCacheFiles(CtrTextureCache* cache) {
    for (int32_t i = 0; CTR_OPEN_FILES > i; i++) {
        if (cache->files[i] != nullptr) fclose(cache->files[i]);
        cache->files[i] = nullptr;
    }
}

static void shedCacheFiles(void* owner) {
    closeCacheFiles((CtrTextureCache*) owner);
}

// The page's cache file, opened (or kept open from an earlier read, in place
// of the one used longest ago, or of all of them if an open failed; with many
// files open, a free slot isn't taken either). Callers use one file at a
// time. On failure, cache->fileBusy tells whether too many files were open.
static FILE* cacheFile(CtrTextureCache* cache, uint32_t pageId) {
    CtrOpenFiles_poll();
    int32_t slot = 0;
    int32_t oldest = -1;
    for (int32_t i = 0; CTR_OPEN_FILES > i; i++) {
        if (cache->files[i] != nullptr && cache->filePages[i] == pageId) {
            cache->fileUsed[i] = ++cache->fileUses;
            return cache->files[i];
        }
        if (cache->files[slot] != nullptr && (cache->files[i] == nullptr || cache->fileUsed[slot] > cache->fileUsed[i])) slot = i;
        if (cache->files[i] != nullptr && (0 > oldest || cache->fileUsed[oldest] > cache->fileUsed[i])) oldest = i;
    }
    uint64_t start = nowNanos();
    if (cache->files[slot] == nullptr && 0 <= oldest && !CtrOpenFiles_mayKeep()) slot = oldest;
    if (cache->files[slot] != nullptr) fclose(cache->files[slot]);
    // Cleared first: closeCacheFiles may run during the open.
    cache->files[slot] = nullptr;
    bool busy = false;
    cache->files[slot] = CtrPack_openFile(pageId, &busy);
    if (cache->files[slot] == nullptr && (busy || cache->pages[pageId].cacheFile == CTR_CACHE_FILE_VALID)) {
        // Too many files are open (the reader's, audio streams): the ones
        // kept open are only a shortcut, and none is in use now.
        closeCacheFiles(cache);
        cache->files[slot] = CtrPack_openFile(pageId, &busy);
    }
    cache->fileBusy = busy;
    loadSteps.openNanos += nowNanos() - start;
    loadSteps.opens++;
    cache->filePages[slot] = pageId;
    cache->fileUsed[slot] = ++cache->fileUses;
    return cache->files[slot];
}

static void closePageFile(CtrTextureCache* cache, uint32_t pageId) {
    for (int32_t i = 0; CTR_OPEN_FILES > i; i++) {
        if (cache->files[i] == nullptr || cache->filePages[i] != pageId) continue;
        fclose(cache->files[i]);
        cache->files[i] = nullptr;
    }
}

// Installs the tiles a finished read decoded into their reserved texture
// memory: they only need flushing now.
static void settleRead(CtrTextureCache* cache, CtrPage* page) {
    CtrTileRead* read = page->read;
    for (int32_t i = 0; tileCount(page) > i; i++) {
        CtrTile* tile = &page->tiles[i];
        if (!tile->reserved || !read->decoded[i] || read->dst[i] != tile->tex.data) continue;
        C3D_TexFlush(&tile->tex);
        if (cache->probeHashes) tile->probeHash = CtrTextures_hashTexels(&tile->tex);
        read->decoded[i] = false;
        allocateTile(cache, page, tile, false);
        tile->lastUsedFrame = cache->frame;
        cache->decodedAhead++;
    }
}

// Drops the page's read-ahead (left to the reader to free if it is still
// reading). What it decoded is kept; texture memory reserved for what it
// didn't is freed.
static void releaseRead(CtrTextureCache* cache, CtrPage* page) {
    CtrTileRead* read = page->read;
    if (read == nullptr) return;
    if (CtrTileReader_finished(cache->reader, read)) {
        settleRead(cache, page);
        if (read->headerOk && read->src == nullptr) {
            cache->readPages++;
            cache->readBytes += read->bytes;
            cache->readMs += read->ms;
        }
    }
    // From here on nothing writes to the reserved tiles.
    CtrTileReader_release(cache->reader, read);
    page->read = nullptr;
    for (int32_t i = 0; tileCount(page) > i; i++) unreserveTile(cache, page, &page->tiles[i]);
}

static void releaseReads(CtrTextureCache* cache) {
    for (uint32_t i = 0; cache->pageCount > i; i++) releaseRead(cache, &cache->pages[i]);
}

// Has the reader read the page's cache file whole, `size` bytes, to keep in
// RAM (see collectWholeRead). Nothing happens if the read can't be made.
static bool queueWholeRead(CtrTextureCache* cache, uint32_t pageId, uint32_t size) {
    bool allowed = CtrMemory_allowRelief(false);
    CtrTileRead* read = CtrTileReader_newRead();
    CtrMemory_allowRelief(allowed);
    if (read == nullptr) return false;
    const Texture* txtr = &cache->dataWin->txtr.textures[pageId];
    read->pageId = pageId;
    read->blobOffset = txtr->blobOffset;
    read->blobSize = txtr->blobSize;
    read->whole = true;
    cache->wholeRead = read;
    cache->wholePage = pageId;
    cache->wholeBytes = size;
    CtrTileReader_queue(cache->reader, read);
    return true;
}

// Once the whole-file read has finished, keeps what it read as the page's
// packed copy. With drop, gives it up instead, finished or not.
static __attribute__((noinline)) void collectWholeRead(CtrTextureCache* cache, bool drop) {
    CtrTileRead* read = cache->wholeRead;
    if (read == nullptr) return;
    bool finished = CtrTileReader_finished(cache->reader, read);
    if (!finished && !drop) return;
    cache->wholeRead = nullptr;
    cache->wholeBytes = 0;
    CtrPage* page = &cache->pages[cache->wholePage];
    bool promoted = false;
    if (finished && !drop && read->headerOk && read->packed != nullptr && read->bytes == read->view.totalSize && page->packed == nullptr) {
        CtrPacked* packed = read->packed;
        read->packed = nullptr;
        if (keepPacked(cache, page, packed)) {
            cache->promotions++;
            promoted = true;
            logInfo("Textures: page %u read whole into RAM in the background in %.0f ms (%u KB)\n", (unsigned) cache->wholePage,
                    (double) read->ms, (unsigned) (read->bytes / 1024));
        }
    }
    if (finished && !drop && !promoted) cache->failedPromotions++;
    CtrTileReader_release(cache->reader, read);
}

// Draw only records intent; reclamation and queue admission happen here, before
// the next Step/Draw. A busy reader defers intent, and an admission failure is
// retried only on a subsequent demand after a cooldown. Current tiles always
// retain their existing synchronous fallback.
static void promotePending(CtrTextureCache* cache) {
    if (cache->reader == nullptr || cache->wholeRead != nullptr || workerBringingPages(cache)) return;
    CtrPage* page = nullptr;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        CtrPage* p = &cache->pages[i];
        if (!p->promotionPending) continue;
        if (p->packed != nullptr || p->failed || p->cacheFile != CTR_CACHE_FILE_VALID) {
            p->promotionPending = false;
            continue;
        }
        if (page == nullptr || p->lastUsedFrame > page->lastUsedFrame) page = p;
    }
    if (page == nullptr) return;
    page->promotionPending = false;
    page->promotionRetryFrame = cache->frame + HEAP_CHECK_INTERVAL_FRAMES;
    if (!makePromotionRoom(cache, page, page->promotionBytes) ||
        !queueWholeRead(cache, (uint32_t) (page - cache->pages), page->promotionBytes)) cache->failedPromotions++;
}

// Whether the reader may read the page's tiles: it isn't in RAM, and may
// have a cache file.
static bool mayRead(const CtrTextureCache* cache, uint32_t pageId, const CtrPage* page) {
    return cache->reader != nullptr && page->layoutKnown && page->packed == nullptr && page->cacheFile != CTR_CACHE_FILE_MISSING && !page->failed &&
           cache->frame >= page->retryFrame && cache->dataWin->txtr.textures[pageId].externalPath == nullptr;
}

// Whether the reader may decode the page's tiles from its packed copy in RAM.
static bool mayDecode(const CtrTextureCache* cache, const CtrPage* page) {
    return CtrTileReader_decodes(cache->reader) && page->layoutKnown && page->packed != nullptr && !page->failed && cache->frame >= page->retryFrame;
}

// Whether texture memory can be reserved for the tile to be decoded into:
// the reader decodes, and the tile's format is known (or remembered).
static bool mayReserve(const CtrTextureCache* cache, const CtrPage* page, const CtrTile* tile) {
    return CtrTileReader_decodes(cache->reader) && (page->formatsKnown || tile->formatRemembered);
}

// Has the read decode the tile into texture memory reserved for it, if it
// can. Before queueing. Returns whether it will.
static bool reserveForRead(CtrTextureCache* cache, CtrPage* page, CtrTileRead* read, int32_t t) {
    CtrTile* tile = &page->tiles[t];
    if (!mayReserve(cache, page, tile)) return false;
    uint8_t* dst = reserveTile(cache, page, tile);
    if (dst == nullptr) return false;
    read->dst[t] = dst;
    read->dstSize[t] = tile->tex.size;
    read->dstFormat[t] = tile->format;
    return true;
}

// The tiles a read (or decode) takes: the page's wanted tiles that aren't
// resident (with nowOnly, those drawn in the room's first frames).
static bool readTakes(const CtrPage* page, const CtrTile* tile, bool nowOnly) {
    return tile->wanted && (!nowOnly || tile->now) && tileMissing(page, tile) && !tile->reserved;
}

// Queues a read of the page's wanted tiles that aren't resident (see
// readTakes). Returns false if there are none (or no memory for the read).
static bool queueRead(CtrTextureCache* cache, uint32_t pageId, CtrPage* page, bool nowOnly) {
    CtrTileRead* read = nullptr;
    for (int32_t t = 0; tileCount(page) > t; t++) {
        if (!readTakes(page, &page->tiles[t], nowOnly)) continue;
        if (read == nullptr && (read = CtrTileReader_newRead()) == nullptr) return false;
        read->want[t] = true;
    }
    if (read == nullptr) return false;
    for (int32_t t = 0; tileCount(page) > t; t++) {
        if (read->want[t]) reserveForRead(cache, page, read, t);
    }
    const Texture* txtr = &cache->dataWin->txtr.textures[pageId];
    read->pageId = pageId;
    read->blobOffset = txtr->blobOffset;
    read->blobSize = txtr->blobSize;
    page->read = read;
    CtrTileReader_queue(cache->reader, read);
    return true;
}

static bool loadPackInfo(CtrTextureCache* cache, uint32_t pageId, CtrPage* page);

// Has the reader decode the page's wanted tiles that aren't resident (see
// readTakes) from its packed copy in RAM, which stays until the read is
// released (see dropPacked). Returns false if there are none.
static bool queueDecode(CtrTextureCache* cache, uint32_t pageId, CtrPage* page, bool nowOnly) {
    if (!loadPackInfo(cache, pageId, page) || page->packed == nullptr) return false;
    CtrTileRead* read = nullptr;
    for (int32_t t = 0; tileCount(page) > t; t++) {
        const CtrTile* tile = &page->tiles[t];
        if (!readTakes(page, tile, nowOnly) || tile->size == 0) continue;
        if (read == nullptr && (read = CtrTileReader_newRead()) == nullptr) return false;
        read->want[t] = true;
        read->tileAt[t] = tile->offset;
    }
    if (read == nullptr) return false;
    uint32_t reserved = 0;
    CtrPackView* view = &read->view;
    view->layout.width = page->width;
    view->layout.height = page->height;
    view->layout.tileW = page->tileW;
    view->layout.tileH = page->tileH;
    view->layout.tilesX = page->tilesX;
    view->layout.tilesY = page->tilesY;
    for (int32_t t = 0; tileCount(page) > t; t++) {
        const CtrTile* tile = &page->tiles[t];
        view->layout.formats[t] = tile->format;
        view->tileOffset[t] = tile->offset;
        view->tileSize[t] = tile->size;
        view->tileChecksum[t] = tile->checksum;
        if (read->want[t] && reserveForRead(cache, page, read, t)) reserved++;
    }
    view->totalSize = page->packedSize;
    read->pageId = pageId;
    read->src = page->packed;
    read->headerOk = true;
    page->read = read;
    // Nothing to decode into (no room in the budget): loaded when drawn.
    if (reserved == 0 || !CtrTileReader_queueDecode(cache->reader, read)) {
        releaseRead(cache, page);
        return false;
    }
    return true;
}

// Takes the tile formats and places from a packed page's header.
static void applyView(CtrPage* page, const CtrPackView* view) {
    for (int32_t i = 0; tileCount(page) > i; i++) {
        CtrTile* tile = &page->tiles[i];
        tile->format = view->layout.formats[i];
        tile->offset = view->tileOffset[i];
        tile->size = view->tileSize[i];
        tile->checksum = view->tileChecksum[i];
    }
    page->formatsKnown = true;
    page->packInfo = true;
}

// Takes the tile formats and places from a valid packed page's header, if
// it has the grid the page has.
static bool takeView(CtrTextureCache* cache, uint32_t pageId, CtrPage* page, const CtrPackView* view) {
    if (page->layoutKnown && !sameGrid(page, &view->layout)) {
        logWarn("Textures: packed page %u is %dx%d, but the page is %dx%d\n", pageId, view->layout.width, view->layout.height, page->width,
                page->height);
        return false;
    }
    if (!page->layoutKnown) setLayout(page, &view->layout);
    applyView(page, view);
    // The image is only needed again if the cache turns out damaged.
    Texture* txtr = &cache->dataWin->txtr.textures[pageId];
    if (txtr->blobData != nullptr && !txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }
    return true;
}

// Learns where the page's tiles are in its packed page, from the copy in RAM
// or from the cache file's header (one small read). Sets the layout too, if
// it wasn't known yet.
static bool loadPackInfo(CtrTextureCache* cache, uint32_t pageId, CtrPage* page) {
    if (page->packInfo) return true;
    Texture* txtr = &cache->dataWin->txtr.textures[pageId];
    if (txtr->externalPath != nullptr) return false;
    CtrPackView* view = safeMalloc(sizeof(CtrPackView));
    bool ok = false;
    CtrPage* wasBusy = cache->busyPage;
    cache->busyPage = page;
    if (page->packed != nullptr) {
        // The first piece holds the header and tile table.
        ok = CtrPack_parseHeader(page->packed->pieces[0], CtrPacked_pieceSize(page->packed, 0), &cache->key, txtr->blobOffset, txtr->blobSize,
                                 view) &&
             view->totalSize == page->packedSize;
        if (!ok) dropPacked(cache, page);
    }
    if (!ok && page->cacheFile != CTR_CACHE_FILE_MISSING) {
        FILE* f = cacheFile(cache, pageId);
        long size = f != nullptr ? CtrPack_fileSize(f) : -1;
        if (size > 0) {
            uint8_t* head = safeMalloc(CTR_PACK_HEADER_MAX);
            size_t headSize = (size_t) size < CTR_PACK_HEADER_MAX ? (size_t) size : CTR_PACK_HEADER_MAX;
            uint64_t readStart = nowNanos();
            bool readOk = fread(head, 1, headSize, f) == headSize;
            loadSteps.readNanos += nowNanos() - readStart;
            loadSteps.reads++;
            ok = readOk &&
                 CtrPack_parseHeader(head, headSize, &cache->key, txtr->blobOffset, txtr->blobSize, view) && view->totalSize == (uint32_t) size;
            free(head);
        }
        // A file that couldn't be opened for too many files open is tried
        // again next time, rather than made again from the image.
        if (f != nullptr || !cache->fileBusy) page->cacheFile = ok ? CTR_CACHE_FILE_VALID : CTR_CACHE_FILE_MISSING;
        // Never keep a file open that the worker may replace.
        if (!ok) closePageFile(cache, pageId);
    }
    ok = ok && takeView(cache, pageId, page, view);
    cache->busyPage = wasBusy;
    free(view);
    return ok;
}

// The page image: the blob in data.win, or GameMaker's external file. The
// external copy is the caller's to free.
static const uint8_t* pageImage(CtrTextureCache* cache, uint32_t pageId, size_t* size, uint8_t** external) {
    DataWin* dw = cache->dataWin;
    Texture* txtr = &dw->txtr.textures[pageId];
    *external = nullptr;
    if (txtr->externalPath != nullptr) {
        int32_t externalSize = 0;
        if (cache->fileSystem != nullptr) cache->fileSystem->vtable->readFileBinary(cache->fileSystem, txtr->externalPath, external, &externalSize);
        *size = *external != nullptr ? (size_t) externalSize : 0;
        return *external;
    }
    DataWin_loadTxtrIfNeeded(dw, pageId);
    *size = txtr->blobData != nullptr ? (size_t) txtr->blobSize : 0;
    return txtr->blobData;
}

static void dropImage(CtrTextureCache* cache, uint32_t pageId) {
    Texture* txtr = &cache->dataWin->txtr.textures[pageId];
    // Read from data.win again if the page is decoded again.
    if (txtr->blobData != nullptr && !txtr->mapped) {
        free(txtr->blobData);
        txtr->blobData = nullptr;
    }
}

// GameMaker 2022.9 and later store each page's size in data.win, which gives
// the grid without opening anything: marking the tiles a room wants used to
// open a dozen cache files for their headers. The tile formats and places
// come with the first load (or the read-ahead).
static bool layoutFromDataWin(CtrTextureCache* cache, uint32_t pageId, CtrPage* page) {
    const Texture* txtr = &cache->dataWin->txtr.textures[pageId];
    if (txtr->externalPath != nullptr || 0 >= txtr->textureWidth || 0 >= txtr->textureHeight) return false;
    if (txtr->textureWidth > 4096 || txtr->textureHeight > 4096) return false;
    CtrPageLayout layout;
    CtrPack_layoutGrid(&layout, txtr->textureWidth, txtr->textureHeight);
    if (layout.tilesX * layout.tilesY > CTR_PAGE_MAX_TILES) return false;
    setLayout(page, &layout);
    return true;
}

// Learns the page's size from its image header, when it has no packed page
// (yet). The image stays loaded, as decoding it usually follows.
static bool loadImageLayout(CtrTextureCache* cache, uint32_t pageId, CtrPage* page) {
    size_t size = 0;
    uint8_t* external = nullptr;
    const uint8_t* image = pageImage(cache, pageId, &size, &external);
    int32_t width = 0;
    int32_t height = 0;
    size_t decodeBytes = 0;
    bool ok = image != nullptr && CtrPack_imageInfo(image, size, &width, &height, &decodeBytes);
    free(external);
    if (!ok) {
        logWarn("Textures: page %u is not an image this port can decode\n", pageId);
        page->failed = true;
        dropImage(cache, pageId);
        return false;
    }
    CtrPageLayout layout;
    CtrPack_layoutGrid(&layout, width, height);
    if (layout.tilesX * layout.tilesY > CTR_PAGE_MAX_TILES) {
        page->failed = true;
        dropImage(cache, pageId);
        return false;
    }
    setLayout(page, &layout);
    return true;
}

// ===[ Loading tiles ]===

static void countLoad(CtrLoadStats* stats, uint32_t tiles, double ms) {
    stats->count++;
    stats->tiles += tiles;
    stats->msTotal += ms;
}

static void logLoad(CtrTextureCache* cache, uint32_t pageId, const CtrPage* page, uint32_t tiles, const char* source, double ms,
                    const char* detail) {
    logInfo("Textures: page %u %dx%d: %u tiles from %s in %.0f ms%s; page %u/%d tiles, %.1f MB; resident %.1f MB\n", pageId, page->width,
            page->height, (unsigned) tiles, source, ms, detail, (unsigned) page->residentTiles, tileCount(page), megabytes(page->bytes),
            megabytes(cache->residentBytes));
}

// Loads one tile from its compressed data.
static LoadResult installTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile, const uint8_t* data) {
    uint64_t t0 = nowNanos();
    if (!CtrPack_checkTile(data, tile->size, tile->checksum)) return LOAD_BAD_DATA;
    uint64_t t1 = nowNanos();
    loadSteps.checkNanos += t1 - t0;
    if (!allocateTile(cache, page, tile, true)) return LOAD_NO_MEMORY;
    uint64_t t2 = nowNanos();
    loadSteps.allocNanos += t2 - t1;
    if (!CtrLz_decompress(data, tile->size, tile->tex.data, tile->tex.size)) {
        releaseTile(cache, page, tile);
        return LOAD_BAD_DATA;
    }
    uint64_t t3 = nowNanos();
    loadSteps.unpackNanos += t3 - t2;
    C3D_TexFlush(&tile->tex);
    loadSteps.flushNanos += nowNanos() - t3;
    if (cache->probeHashes) tile->probeHash = CtrTextures_hashTexels(&tile->tex);
    tile->lastUsedFrame = cache->frame;
    return LOAD_OK;
}

static double stepMs(uint64_t nanos) {
    return (double) nanos / 1e6;
}

// The steps of a load, for its log line: the reading ones if there were any.
static void describeSteps(char* out, size_t size, uint32_t bytesRead) {
    const LoadSteps* s = &loadSteps;
    int used = 0;
    if (s->reads > 0) {
        used = snprintf(out, size, " (read %u KB in %u, %.0f ms", (unsigned) (bytesRead / 1024), (unsigned) s->reads, stepMs(s->readNanos));
        if (s->opens > 0) used += snprintf(out + used, size - (size_t) used, "; opened %u, %.0f ms", (unsigned) s->opens, stepMs(s->openNanos));
        used += snprintf(out + used, size - (size_t) used, "; ");
    } else {
        used = snprintf(out, size, " (");
    }
    snprintf(out + used, size - (size_t) used, "check %.1f, alloc %.1f, unpack %.1f, flush %.1f ms)", stepMs(s->checkNanos), stepMs(s->allocNanos),
             stepMs(s->unpackNanos), stepMs(s->flushNanos));
}

// Forgets the page's packed copies after one turned out damaged, so its tiles
// come from the image from now on.
static void distrustPack(CtrTextureCache* cache, uint32_t pageId, CtrPage* page) {
    logWarn("Textures: packed page %u is damaged; decoding its image instead\n", pageId);
    dropPacked(cache, page);
    releaseRead(cache, page);
    if (page->cacheFile == CTR_CACHE_FILE_VALID) {
        closePageFile(cache, pageId);
        CtrPack_removeFile(pageId);
    }
    page->cacheFile = CTR_CACHE_FILE_MISSING;
    page->packInfo = false;
    // The formats came from the same damaged page; the image tells again.
    page->formatsKnown = false;
}

static LoadResult loadFromPacked(CtrTextureCache* cache, CtrPage* page, uint32_t* loaded) {
    for (int32_t i = 0; tileCount(page) > i; i++) {
        CtrTile* tile = &page->tiles[i];
        if (!tile->requested || !tileMissing(page, tile)) continue;
        LoadResult result = installTile(cache, page, tile, CtrPacked_at(page->packed, tile->offset));
        if (result != LOAD_OK) return result;
        (*loaded)++;
    }
    return LOAD_OK;
}

// Reads the requested tiles from the cache file: the tiles of a row are
// stored one after another, so one read covers each row's run of them.
static LoadResult loadFromFile(CtrTextureCache* cache, uint32_t pageId, CtrPage* page, uint32_t* loaded, uint32_t* bytesRead) {
    FILE* f = cacheFile(cache, pageId);
    if (f == nullptr) return LOAD_BAD_DATA;
    for (int32_t ty = 0; page->tilesY > ty; ty++) {
        int32_t first = -1;
        int32_t last = -1;
        for (int32_t tx = 0; page->tilesX > tx; tx++) {
            CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
            if (!tile->requested || !tileMissing(page, tile)) continue;
            if (0 > first) first = tx;
            last = tx;
        }
        if (0 > first) continue;
        const CtrTile* a = &page->tiles[ty * page->tilesX + first];
        const CtrTile* b = &page->tiles[ty * page->tilesX + last];
        uint32_t start = a->offset;
        uint32_t size = b->offset + b->size - start;
        uint8_t* data = malloc(size > 0 ? size : 1);
        if (data == nullptr) return LOAD_NO_MEMORY;
        uint64_t readStart = nowNanos();
        bool readOk = fseek(f, (long) start, SEEK_SET) == 0 && fread(data, 1, size, f) == size;
        loadSteps.readNanos += nowNanos() - readStart;
        loadSteps.reads++;
        if (!readOk) {
            free(data);
            return LOAD_BAD_DATA;
        }
        *bytesRead += size;
        LoadResult result = LOAD_OK;
        for (int32_t tx = first; last >= tx && result == LOAD_OK; tx++) {
            CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
            if (!tile->requested || !tileMissing(page, tile)) continue;
            result = installTile(cache, page, tile, data + (tile->offset - start));
            if (result == LOAD_OK) (*loaded)++;
        }
        free(data);
        if (result != LOAD_OK) return result;
    }
    return LOAD_OK;
}

// Loading tiles from the page image: each row of tiles is converted as soon
// as it is decoded. The requested and wanted tiles load, and the others too
// while they fit in the budget, so that a page decoded once rarely needs
// decoding again.
typedef struct {
    CtrTextureCache* cache;
    CtrPage* page;
    uint32_t loaded;
    bool noLinearMemory;
    bool badLayout;
    uint64_t convertNanos;
} ImageLoad;

static bool loadImageRow(void* user, const CtrPageLayout* layout, int32_t ty, const uint8_t* rows) {
    ImageLoad* load = user;
    CtrPage* page = load->page;
    if (!sameGrid(page, layout)) {
        load->badLayout = true;
        return false;
    }
    uint64_t start = nowNanos();
    for (int32_t tx = 0; layout->tilesX > tx; tx++) {
        int32_t i = ty * layout->tilesX + tx;
        CtrTile* tile = &page->tiles[i];
        if (tile->valid) continue;
        tile->format = layout->formats[i];
        if (tile->format == CTR_TILE_EMPTY) continue;
        bool needed = tile->requested || tile->wanted;
        if (!allocateTile(load->cache, page, tile, needed)) {
            if (!needed) {
                // Tiles of this page are likelier to be drawn soon than
                // unused ones of a page decoded earlier: that saves decoding
                // this page again for a tile that wasn't on screen yet.
                bool fits = evictUnusedExtras(load->cache, page, tileBytes(page, tile));
                if (!fits || !allocateTile(load->cache, page, tile, false)) continue;
            } else {
                load->noLinearMemory = true;
                return false;
            }
        }
        CtrPack_convertTile(tile->tex.data, layout, tx, ty, rows);
        C3D_TexFlush(&tile->tex);
        if (load->cache->probeHashes) tile->probeHash = CtrTextures_hashTexels(&tile->tex);
        // Tiles nobody asked for are the first to go.
        tile->lastUsedFrame = needed ? load->cache->frame : 0;
        load->loaded++;
    }
    load->convertNanos += nowNanos() - start;
    return true;
}

static LoadResult loadFromImage(CtrTextureCache* cache, uint32_t pageId, CtrPage* page, uint32_t* loaded, double* outConvertMs) {
    size_t size = 0;
    uint8_t* external = nullptr;
    const uint8_t* image = pageImage(cache, pageId, &size, &external);
    ImageLoad load = { .cache = cache, .page = page };
    CtrPageLayout layout;
    CtrDecodeStatus status = CTR_DECODE_BAD_DATA;
    if (image != nullptr && size > 0) {
        status = CtrPack_decodeRows(image, size, DataWin_isVersionAtLeast(cache->dataWin, 2022, 5, 0, 0), true, &layout, loadImageRow, &load);
    }
    free(external);
    dropImage(cache, pageId);
    page->imageDecoded = true;
    page->imageFrame = cache->frame;
    *loaded = load.loaded;
    *outConvertMs = (double) load.convertNanos / 1e6;

    if (status == CTR_DECODE_OK) {
        page->formatsKnown = true;
        return LOAD_OK;
    }
    // allocateTile has already set the page to retry.
    if (load.noLinearMemory) return LOAD_NO_MEMORY;
    if (status == CTR_DECODE_NO_MEMORY) {
        logWarn("Textures: out of memory decoding page %u (%.1f MB heap free)\n", pageId, megabytes(heapFree()));
        page->retryFrame = cache->frame + OUT_OF_MEMORY_RETRY_FRAMES;
        return LOAD_NO_MEMORY;
    }
    logWarn("Textures: failed to decode page %u\n", pageId);
    page->failed = true;
    return LOAD_BAD_DATA;
}

// Whether the page's read-ahead holds a wanted tile that isn't resident.
static bool readHasMore(const CtrPage* page) {
    const CtrTileRead* read = page->read;
    for (int32_t i = 0; tileCount(page) > i; i++) {
        if (read->tileAt[i] != CTR_READ_NOT_READ && page->tiles[i].wanted && tileMissing(page, &page->tiles[i])) return true;
    }
    return false;
}

// Loads the requested tiles that the page's read-ahead brought, waiting for
// it if the reader is on it. One the reader hasn't started is taken back:
// the caller reads what it needs itself, as quickly as the reader would.
// Tiles decoded ahead only need flushing; the others are decompressed here.
static LoadResult loadFromRead(CtrTextureCache* cache, uint32_t pageId, CtrPage* page, uint32_t* loaded, double* waitedMs) {
    CtrTileRead* read = page->read;
    if (CtrTileReader_unqueue(cache->reader, read)) {
        releaseRead(cache, page);
        return LOAD_OK;
    }
    *waitedMs = CtrTileReader_wait(cache->reader, read);
    if (!read->headerOk) {
        releaseRead(cache, page);
        return LOAD_OK;
    }
    if (!page->packInfo) {
        if (!takeView(cache, pageId, page, &read->view)) {
            releaseRead(cache, page);
            return LOAD_OK;
        }
        page->cacheFile = CTR_CACHE_FILE_VALID;
    }
    uint32_t settled = cache->decodedAhead;
    settleRead(cache, page);
    *loaded += cache->decodedAhead - settled;
    if (read->data == nullptr && read->src == nullptr) {
        releaseRead(cache, page);
        return LOAD_OK;
    }
    for (int32_t i = 0; tileCount(page) > i; i++) {
        CtrTile* tile = &page->tiles[i];
        if (!tile->requested || !tileMissing(page, tile) || read->tileAt[i] == CTR_READ_NOT_READ) continue;
        // From the same file as the page's tile table, unless that changed.
        if (tile->offset != read->view.tileOffset[i] || tile->size != read->view.tileSize[i]) continue;
        LoadResult result = installTile(cache, page, tile, CtrTileReader_tileData(read, i));
        if (result != LOAD_OK) return result;
        (*loaded)++;
    }
    if (!readHasMore(page)) releaseRead(cache, page);
    return LOAD_OK;
}

static bool anyRequestedMissing(const CtrPage* page) {
    for (int32_t i = 0; tileCount(page) > i; i++) {
        if (page->tiles[i].requested && tileMissing(page, &page->tiles[i])) return true;
    }
    return false;
}

static void clearRequested(CtrPage* page) {
    for (int32_t i = 0; tileCount(page) > i; i++) page->tiles[i].requested = false;
}

// Loads the page's requested tiles from the first source that has them: the
// packed copy in RAM, the cache file, the image.
static void loadRequestedFrom(CtrTextureCache* cache, uint32_t pageId, CtrPage* page) {
    if (!anyRequestedMissing(page)) {
        clearRequested(page);
        return;
    }
    if (page->failed || page->retryFrame > cache->frame) {
        clearRequested(page);
        return;
    }
    uint64_t start = nowNanos();
    char detail[192];
    char steps[160];
    uint32_t loaded = 0;
    memset(&loadSteps, 0, sizeof(loadSteps));

    // A page that came into RAM since its read-ahead was queued loads from there.
    if (page->read != nullptr && page->packed != nullptr && page->read->src == nullptr) releaseRead(cache, page);
    if (page->read != nullptr) {
        double waitedMs = 0.0;
        bool fromRam = page->read->src != nullptr;
        LoadResult result = loadFromRead(cache, pageId, page, &loaded, &waitedMs);
        if (result == LOAD_NO_MEMORY) {
            clearRequested(page);
            return;
        }
        if (result == LOAD_BAD_DATA) {
            distrustPack(cache, pageId, page);
        } else if (loaded > 0) {
            double ms = msSince(start);
            countLoad(fromRam ? &cache->fromRam : &cache->fromRead, loaded, ms);
            describeSteps(steps, sizeof(steps), 0);
            snprintf(detail, sizeof(detail), " (waited %.0f ms)%s", waitedMs, steps);
            if (ms >= QUIET_LOAD_MS && !cache->filling) logLoad(cache, pageId, page, loaded, fromRam ? "RAM, decoding ahead" : "read-ahead", ms, detail);
        }
        if (!anyRequestedMissing(page)) {
            clearRequested(page);
            return;
        }
        // The rest come from wherever they are.
        start = nowNanos();
        loaded = 0;
        memset(&loadSteps, 0, sizeof(loadSteps));
    }

    if (loadPackInfo(cache, pageId, page)) {
        // With a reader, request promotion for the next pre-Step/Draw phase.
        // Never reclaim packed copies from this draw-time path. The requested
        // tiles still load below while promotion is pending or cannot fit.
        bool background = cache->reader != nullptr;
        if (page->packed == nullptr && page->cacheFile == CTR_CACHE_FILE_VALID && !cache->filling && !workerBringingPages(cache) &&
            (!background || (!page->promotionPending && cache->frame >= page->promotionRetryFrame &&
                             (cache->wholeRead == nullptr || cache->wholePage != pageId)))) {
            FILE* f = cacheFile(cache, pageId);
            long fileSize = f != nullptr ? CtrPack_fileSize(f) : -1;
            bool fits = fileSize > 0 && cache->packedBudgetBytes >= cache->packedBytes + cache->wholeBytes + (uint32_t) fileSize;
            if (background && fileSize > 0) {
                page->promotionBytes = (uint32_t) fileSize;
                page->promotionPending = true;
            } else if (fits && !background) {
                uint64_t readStart = nowNanos();
                // If the heap hasn't the room, the tiles come from the file
                // one by one, rather than other pages' copies going.
                bool allowed = CtrMemory_allowRelief(false);
                const Texture* txtr = &cache->dataWin->txtr.textures[pageId];
                CtrPacked* packed = CtrPack_readFile(f, &cache->key, txtr->blobOffset, txtr->blobSize);
                CtrMemory_allowRelief(allowed);
                loadSteps.readNanos += nowNanos() - readStart;
                loadSteps.reads++;
                if (packed != nullptr) {
                    uint32_t size = packed->size;
                    if (keepPacked(cache, page, packed)) {
                        LoadResult result = loadFromPacked(cache, page, &loaded);
                        if (result == LOAD_OK) {
                            double ms = msSince(start);
                            countLoad(&cache->fromFile, loaded, ms);
                            describeSteps(detail, sizeof(detail), size);
                            logLoad(cache, pageId, page, loaded, "SD cache, whole file", ms, detail);
                            clearRequested(page);
                            return;
                        }
                        if (result == LOAD_NO_MEMORY) {
                            clearRequested(page);
                            return;
                        }
                        distrustPack(cache, pageId, page);
                    }
                }
            }
        }
        if (page->packInfo && page->packed != nullptr) {
            LoadResult result = loadFromPacked(cache, page, &loaded);
            if (result == LOAD_OK) {
                double ms = msSince(start);
                countLoad(&cache->fromRam, loaded, ms);
                if (ms >= QUIET_LOAD_MS && !cache->filling) {
                    describeSteps(detail, sizeof(detail), 0);
                    logLoad(cache, pageId, page, loaded, "RAM", ms, detail);
                }
                clearRequested(page);
                return;
            }
            if (result == LOAD_NO_MEMORY) {
                clearRequested(page);
                return;
            }
            distrustPack(cache, pageId, page);
        }
        if (page->packInfo && page->cacheFile == CTR_CACHE_FILE_VALID) {
            uint32_t bytesRead = 0;
            LoadResult result = loadFromFile(cache, pageId, page, &loaded, &bytesRead);
            if (result == LOAD_OK) {
                double ms = msSince(start);
                countLoad(&cache->fromFile, loaded, ms);
                page->fileLoads++;
                describeSteps(detail, sizeof(detail), bytesRead);
                if (!cache->filling) logLoad(cache, pageId, page, loaded, "SD cache", ms, detail);
                clearRequested(page);
                return;
            }
            if (result == LOAD_NO_MEMORY) {
                clearRequested(page);
                return;
            }
            distrustPack(cache, pageId, page);
        }
    }

    // Not packed yet: the worker packs this page first, and tiles that
    // aren't drawn now load from that. A tile drawn now decodes here, even
    // if the page was decoded a moment ago, so that no draw goes without it
    // (hammer601, at the end of this file; loadRequestedFrom keeps its
    // slot, hammer601TexSlot).
    if (cache->worker != nullptr && page->cacheFile != CTR_CACHE_FILE_VALID) CtrPackWorker_hurry(cache->worker, pageId);

    double convertMs = 0.0;
    uint32_t fromImage = 0;
    LoadResult result = loadFromImage(cache, pageId, page, &fromImage, &convertMs);
    loaded += fromImage;
    if (result == LOAD_OK) {
        double ms = msSince(start);
        countLoad(&cache->fromImage, loaded, ms);
        snprintf(detail, sizeof(detail), " (decode %.0f ms + convert %.0f ms)", ms - convertMs, convertMs);
        logLoad(cache, pageId, page, loaded, "image", ms, detail);
    }
    clearRequested(page);
}

static void loadRequested(CtrTextureCache* cache, uint32_t pageId, CtrPage* page) {
    uint64_t start = nowNanos();
    cache->busyPage = page;
    loadRequestedFrom(cache, pageId, page);
    cache->busyPage = nullptr;
    cache->frameLoadNanos += nowNanos() - start;
}

// Takes the pages the worker has packed since the last frame.
static void collectPacked(CtrTextureCache* cache) {
    CtrPackResult result;
    while (CtrPackWorker_poll(cache->worker, &result)) {
        CtrPage* page = &cache->pages[result.pageId];
        page->cacheFile = CTR_CACHE_FILE_VALID;
        cache->workerPacked++;
        if (result.built) {
            logInfo("Textures: cached page %u, %u KB (background: decode %.0f ms, pack %.0f ms)\n", result.pageId,
                    (unsigned) (result.size / 1024), (double) result.decodeMs, (double) result.packMs);
        }
        if (result.packed != nullptr) keepPacked(cache, page, result.packed);
        // Its wanted tiles can be filled in now.
        if (!cache->fillPending || cache->fillPage > result.pageId) cache->fillPage = result.pageId;
        cache->fillPending = true;
    }
}

// ===[ Filling in a room's tiles ]===

// The next wanted tile that isn't resident, one the page's read brought if
// there is one.
static CtrTile* nextWantedTile(CtrPage* page) {
    if (!page->layoutKnown) return nullptr;
    const CtrTileRead* read = page->read;
    CtrTile* first = nullptr;
    for (int32_t i = 0; tileCount(page) > i; i++) {
        CtrTile* tile = &page->tiles[i];
        if (!tile->wanted || !tileMissing(page, tile)) continue;
        if (read == nullptr || read->tileAt[i] != CTR_READ_NOT_READ) return tile;
        if (first == nullptr) first = tile;
    }
    return first;
}

// Loads wanted tiles that aren't resident yet until FILL_BUDGET_MS of the
// frame is gone. From pages in RAM directly; the reader reads the others'
// first, and they are come back to once read. Pages that aren't packed yet
// wait for the worker (or to be drawn): decoding an image takes far longer
// than a frame.
static void fillWanted(CtrTextureCache* cache) {
    uint64_t start = nowNanos();
    cache->filling = true;
    while (cache->pageCount > cache->fillPage) {
        uint32_t pageId = cache->fillPage;
        CtrPage* page = &cache->pages[pageId];
        if (page->read != nullptr && page->packed != nullptr && page->read->src == nullptr) releaseRead(cache, page);
        if (page->read != nullptr && !CtrTileReader_finished(cache->reader, page->read)) {
            if (cache->fillRevisit > pageId) cache->fillRevisit = pageId;
            cache->fillPage++;
            continue;
        }
        CtrTile* tile = nextWantedTile(page);
        if (tile == nullptr || page->failed || page->retryFrame > cache->frame) {
            if (tile == nullptr) releaseRead(cache, page);
            cache->fillPage++;
            continue;
        }
        bool fromRead = page->read != nullptr && page->read->tileAt[tile - page->tiles] != CTR_READ_NOT_READ;
        // Reading takes a frame or more, and decoding several milliseconds a
        // tile: once per room, the reader reads (or decodes, from the copy
        // in RAM) all the page's wanted tiles that are left.
        if (!fromRead && !page->fillRead && (mayRead(cache, pageId, page) || mayDecode(cache, page))) {
            releaseRead(cache, page);
            page->fillRead = true;
            bool queued = page->packed != nullptr ? queueDecode(cache, pageId, page, false) : queueRead(cache, pageId, page, false);
            if (queued) {
                if (cache->fillRevisit > pageId) cache->fillRevisit = pageId;
                cache->fillPage++;
                continue;
            }
        }
        if (!fromRead && page->packed == nullptr && page->cacheFile != CTR_CACHE_FILE_VALID) {
            cache->fillPage++;
            continue;
        }
        if (msSince(start) >= FILL_BUDGET_MS) break;
        // Filling in is ahead of need: once the budget is full, it stops
        // rather than evict tiles, which the room may draw again (or which
        // were filled in themselves) and would then load again.
        if (cache->residentBytes + tileBytes(page, tile) > cache->budgetBytes) {
            cache->fillPage = cache->pageCount;
            cache->fillRevisit = UINT32_MAX;
            logInfo("Textures: the room's tiles fill the budget (%.1f of %.1f MB), filling in stopped\n", megabytes(cache->residentBytes),
                    megabytes(cache->budgetBytes));
            break;
        }
        // Speculation must leave the same linear floor the next frame
        // protects, or filling and reclaiming repeatedly undo each other.
        // A matching reservation already owns its bytes; publishing it
        // needs no additional allocation. Reader reservation is unchanged.
        uint32_t bytes = tileBytes(page, tile);
        bool reserved = tile->reserved && tile->tex.size == bytes &&
                        tile->tex.fmt == CtrPack_gpuFormat((CtrTileFormat) tile->format);
        uint32_t freeBytes = linearSpaceFree();
        if (!reserved && freeBytes < LINEAR_FLOOR_BYTES + bytes) {
            cache->fillPage = cache->pageCount;
            cache->fillRevisit = UINT32_MAX;
            logInfo("Textures: speculative filling stopped before the 3 MiB linear floor (%u KB free, %u KB tile needed)\n",
                    (unsigned) (freeBytes / 1024), (unsigned) (bytes / 1024));
            break;
        }
        tile->requested = true;
        loadRequested(cache, pageId, page);
        // One that can't load now is left for when it's drawn.
        if (tileMissing(page, tile)) tile->wanted = false;
        else cache->fillTiles++;
    }
    cache->filling = false;
    cache->fillMs += msSince(start);
    if (cache->pageCount > cache->fillPage) return;
    if (cache->fillRevisit != UINT32_MAX) {
        cache->fillPage = cache->fillRevisit;
        cache->fillRevisit = UINT32_MAX;
        return;
    }
    cache->fillPending = false;
    if (cache->fillTiles > 0) {
        logInfo("Textures: filled in %u tiles the room wants in %.0f ms over %u frames (resident %.1f MB)\n", (unsigned) cache->fillTiles, cache->fillMs,
                (unsigned) (cache->frame - cache->fillFrom + 1), megabytes(cache->residentBytes));
    }
    if (cache->readPages > 0) {
        logInfo("Textures: read ahead %u KB of %u pages in %.0f ms\n", (unsigned) (cache->readBytes / 1024), (unsigned) cache->readPages, cache->readMs);
    }
    cache->fillTiles = 0;
    cache->fillMs = 0.0;
    cache->readPages = 0;
    cache->readBytes = 0;
    cache->readMs = 0.0;
}

// ===[ Room memory ]===
//
// Many of the tiles a room draws first can't be told from its data: Draw
// events draw them (DELTARUNE Chapter 3's game board, Chapter 5's castle).
// So the tiles each room draws in its first frames are recorded, and on the
// next visit, in this run or a later one, they are read ahead with the rest.
// The record is a small file in the cache folder, which the reader writes.

#define ROOM_MEMORY_FILE "rooms.dtm"
#define ROOM_MEMORY_MAGIC 0x4D525444u // "DTRM"
#define ROOM_MEMORY_VERSION 3u
#define ROOM_MEMORY_MAX_SIZE (1024u * 1024u)
// Frames recorded from the room change on: the fade in, and a little more.
#define ROOM_RECORD_FRAMES 45
// Tiles kept per room at most.
#define ROOM_TILES_MAX 512
// Each tile is kept as late | formatKnown | format << 27 | page << 8 | tile.
// Late is set on a tile first drawn after the room's first drawn frame. Only
// the others are read for that frame; reading later ones too kept it
// waiting. The tile's format (CtrTileFormat), when known, lets texture
// memory be reserved for decoding it ahead before its page's tile table has
// been read.
#define ROOM_TILE_LATE 0x80000000u
#define ROOM_TILE_FORMAT_KNOWN 0x40000000u
#define ROOM_TILE_FORMAT_SHIFT 27
#define ROOM_TILE_KEY_MASK 0x07FFFFFFu // page and tile
#define ROOM_TILE_PAGE_MAX 0x7FFFFu
#define ROOM_TILE_PAGE(entry) (((entry) & ROOM_TILE_KEY_MASK) >> 8)
#define ROOM_TILE_INDEX(entry) ((int32_t) ((entry) & 0xFFu))
#define ROOM_TILE_FORMAT(entry) ((uint8_t) (((entry) >> ROOM_TILE_FORMAT_SHIFT) & 7u))

// Followed by, for each room with tiles: its index and tile count (uint16_t
// each), then the tiles (uint32_t each).
typedef struct {
    uint32_t magic;
    uint32_t version;
    CtrPackKey key;
    uint32_t tileMax;  // CTR_TILE_MAX: builds with other tile sizes number tiles differently
    uint32_t roomCount;
    uint32_t bodySize;
    uint32_t checksum; // of what follows
} RoomMemoryHeader;

static uint32_t fnv1a(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; size > i; i++) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

// Reads the record left by earlier runs, if it is for this data.win and
// intact. Anything else is ignored and replaced later.
static void loadRoomMemory(CtrTextureCache* cache) {
    cache->roomCount = cache->dataWin->room.count;
    cache->roomTiles = safeCalloc(cache->roomCount > 0 ? cache->roomCount : 1, sizeof(uint32_t*));
    cache->recordRoom = -1;
    char path[CTR_PACK_PATH_MAX];
    CtrPack_cachePath(path, sizeof(path), ROOM_MEMORY_FILE);
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return;
    long size = CtrPack_fileSize(f);
    uint8_t* data = nullptr;
    if (size > (long) sizeof(RoomMemoryHeader) && ROOM_MEMORY_MAX_SIZE >= (unsigned long) size) data = malloc((size_t) size);
    if (data != nullptr && fread(data, 1, (size_t) size, f) != (size_t) size) {
        free(data);
        data = nullptr;
    }
    fclose(f);
    if (data == nullptr) return;

    RoomMemoryHeader header;
    memcpy(&header, data, sizeof(header));
    const uint8_t* body = data + sizeof(header);
    size_t bodySize = (size_t) size - sizeof(header);
    uint32_t rooms = 0;
    uint32_t tiles = 0;
    if (header.magic == ROOM_MEMORY_MAGIC && header.version == ROOM_MEMORY_VERSION && memcmp(&header.key, &cache->key, sizeof(header.key)) == 0 &&
        header.tileMax == CTR_TILE_MAX && header.roomCount == cache->roomCount && header.bodySize == bodySize && header.checksum == fnv1a(body, bodySize)) {
        size_t at = 0;
        while (bodySize >= at + 4) {
            uint16_t room;
            uint16_t count;
            memcpy(&room, body + at, sizeof(room));
            memcpy(&count, body + at + 2, sizeof(count));
            at += 4;
            if (room >= cache->roomCount || count > ROOM_TILES_MAX || (size_t) count * 4 > bodySize - at) break;
            if (!CtrArray_resize(&cache->roomTiles[room], sizeof(uint32_t), count)) break;
#line 1657
            memcpy(cache->roomTiles[room], body + at, (size_t) count * 4);
            at += (size_t) count * 4;
            rooms++;
            tiles += count;
        }
    }
    free(data);
    logInfo("Textures: remembered the first tiles of %u rooms (%u tiles)\n", (unsigned) rooms, (unsigned) tiles);
}

void CtrTextures_forgetRoomMemory(void) {
    char path[CTR_PACK_PATH_MAX];
    CtrPack_cachePath(path, sizeof(path), ROOM_MEMORY_FILE);
    bool removed = remove(path) == 0;
    logInfo("Textures: room_memory_reset: %s %s\n", removed ? "deleted" : "no record at", path);
}

// Has the reader write the record.
static void saveRoomMemory(CtrTextureCache* cache) {
    if (cache->reader == nullptr) return;
    size_t bodySize = 0;
    for (uint32_t r = 0; cache->roomCount > r; r++) {
        if (arrlen(cache->roomTiles[r]) > 0) bodySize += 4 + (size_t) arrlen(cache->roomTiles[r]) * 4;
    }
    uint8_t* data = malloc(sizeof(RoomMemoryHeader) + bodySize);
    if (data == nullptr) return;
    uint8_t* body = data + sizeof(RoomMemoryHeader);
    size_t at = 0;
    for (uint32_t r = 0; cache->roomCount > r; r++) {
        uint16_t count = (uint16_t) arrlen(cache->roomTiles[r]);
        if (count == 0) continue;
        uint16_t room = (uint16_t) r;
        memcpy(body + at, &room, sizeof(room));
        memcpy(body + at + 2, &count, sizeof(count));
        memcpy(body + at + 4, cache->roomTiles[r], (size_t) count * 4);
        at += 4 + (size_t) count * 4;
    }
    RoomMemoryHeader header;
    memset(&header, 0, sizeof(header));
    header.magic = ROOM_MEMORY_MAGIC;
    header.version = ROOM_MEMORY_VERSION;
    header.key = cache->key;
    header.tileMax = CTR_TILE_MAX;
    header.roomCount = cache->roomCount;
    header.bodySize = (uint32_t) bodySize;
    header.checksum = fnv1a(body, bodySize);
    memcpy(data, &header, sizeof(header));
    char path[CTR_PACK_PATH_MAX];
    CtrPack_cachePath(path, sizeof(path), ROOM_MEMORY_FILE);
    CtrTileReader_writeFile(cache->reader, path, data, sizeof(header) + bodySize);
}

static void startRecording(CtrTextureCache* cache, int32_t room) {
    arrsetlen(cache->recording, 0);
    cache->recordVisit++;
    cache->recordUntil = cache->frame + 1 + ROOM_RECORD_FRAMES;
    cache->recordFirstFrame = UINT32_MAX;
    // The record keeps room indices in 16 bits.
    cache->recordRoom = 0 <= room && cache->roomCount > (uint32_t) room && 0x10000 > room ? room : -1;
}

static void recordTile(CtrTextureCache* cache, const CtrPage* page, CtrTile* tile) {
    tile->seenVisit = cache->recordVisit;
    if (arrlen(cache->recording) >= ROOM_TILES_MAX || pageIdOf(cache, page) > ROOM_TILE_PAGE_MAX) return;
    if (cache->recordFirstFrame == UINT32_MAX) cache->recordFirstFrame = cache->frame;
    uint32_t late = cache->frame != cache->recordFirstFrame ? ROOM_TILE_LATE : 0;
    if (!CTR_ARRAY_PUT(cache->recording, late | pageIdOf(cache, page) << 8 | (uint32_t) (tile - page->tiles))) {
        cache->recordRoom = -1; // Never publish an incomplete room record.
    }
#line 1724
}

// Whether the list has the entry (format and all), or with anyTile, the tile
// in any form.
static bool hasEntry(const uint32_t* list, uint32_t entry, bool anyTile) {
    uint32_t mask = anyTile ? ROOM_TILE_KEY_MASK : UINT32_MAX;
    for (ptrdiff_t i = 0; arrlen(list) > i; i++) {
        if ((list[i] & mask) == (entry & mask)) return true;
    }
    return false;
}

// Adds the tiles' formats to the recording, those known by now: the tiles
// are recorded when first drawn, before they are loaded.
__attribute__((noipa, section(".text.late.oomport"))) static void addFormats(CtrTextureCache* cache) {
    for (ptrdiff_t i = 0; arrlen(cache->recording) > i; i++) {
        uint32_t entry = cache->recording[i];
        if (ROOM_TILE_PAGE(entry) >= cache->pageCount) continue;
        const CtrPage* page = &cache->pages[ROOM_TILE_PAGE(entry)];
        int32_t t = ROOM_TILE_INDEX(entry);
        if (!page->layoutKnown || t >= tileCount(page)) continue;
        const CtrTile* tile = &page->tiles[t];
        if (!page->formatsKnown && !tile->valid) continue;
        cache->recording[i] = entry | ROOM_TILE_FORMAT_KNOWN | (uint32_t) tile->format << ROOM_TILE_FORMAT_SHIFT;
    }
}

// Keeps what the room drew. A whole recording replaces the last one; one
// cut short by leaving the room adds to it. Saved if anything changed.
static void finishRecording(CtrTextureCache* cache, bool whole) {
    int32_t room = cache->recordRoom;
    cache->recordRoom = -1;
    ptrdiff_t count = arrlen(cache->recording);
    if (0 > room || count == 0) return;
    addFormats(cache);
    uint32_t** kept = &cache->roomTiles[room];
    bool changed = false;
    if (whole) {
        // Recorded tiles are distinct, so the same count and nothing new
        // means the same tiles.
        for (ptrdiff_t i = 0; count > i && !changed; i++) changed = !hasEntry(*kept, cache->recording[i], false);
        changed = changed || arrlen(*kept) != count;
        if (changed) {
            arrfree(*kept);
            *kept = cache->recording;
            cache->recording = nullptr;
        }
    } else {
        for (ptrdiff_t i = 0; count > i && ROOM_TILES_MAX > arrlen(*kept); i++) {
            if (hasEntry(*kept, cache->recording[i], true)) continue;
            if (!CtrArray_putU32(kept, cache->recording[i])) break;
#line 1775
            changed = true;
        }
    }
    arrsetlen(cache->recording, 0);
    if (changed) saveRoomMemory(cache);
}

// ===[ Interface ]===

uint32_t CtrTextures_hashTexels(const C3D_Tex* tex) {
    const uint32_t* w = (const uint32_t*) tex->data;
    uint32_t h = 2166136261u;
    if (w == nullptr) return 0;
    for (uint32_t i = 0; tex->size / 4 > i; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

bool CtrTextures_roomThrashing(const CtrTextureCache* cache) {
    return cache->roomThrashed;
}

void CtrTextures_forgiveThrashing(CtrTextureCache* cache) {
    cache->roomThrashed = false;
}

bool CtrTextures_lendBudget(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames) {
    if (cache->budgetBytes < bytes || cache->roomThrashed) return false;
    uint32_t usedBefore = cache->frame > idleFrames ? cache->frame - idleFrames : 0;
    // Room is made the way a tile's would be, but only of tiles idle that long.
    evictUsedBefore(cache, bytes, usedBefore);
    if (cache->residentBytes + bytes > cache->budgetBytes) return false;
    cache->budgetBytes -= bytes;
    return true;
}

void CtrTextures_returnBudget(CtrTextureCache* cache, uint32_t bytes) {
    cache->budgetBytes += bytes;
}

void CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes) {
    memset(cache, 0, sizeof(*cache));
    cache->dataWin = dataWin;
    cache->fileSystem = fileSystem;
    cache->pageCount = dataWin->txtr.count;
    cache->pages = safeCalloc(cache->pageCount > 0 ? cache->pageCount : 1, sizeof(CtrPage));
    cache->budgetBytes = budgetBytes;
    CtrPack_init();
    cache->key = CtrPack_keyFor(dataWin);
    cache->fileKeeper = CtrOpenFiles_addKeeper(shedCacheFiles, nullptr, cache);

    C3D_TexInit(&cache->clear, 8, 8, GPU_RGBA8);
    memset(cache->clear.data, 0, cache->clear.size);
    C3D_TexFlush(&cache->clear);

    uint32_t heapFreeBytes = heapFree();
    uint32_t packedShare = PACKED_BUDGET_SHARE(heapFreeBytes);
    cache->packedBudgetBytes = packedShare < PACKED_BUDGET_MAX ? packedShare : PACKED_BUDGET_MAX;
    cache->worker = CtrPackWorker_start(dataWin, &cache->key, false);
    cache->workerKeepsPacked = cache->worker != nullptr;
    cache->reader = CtrTileReader_start(&cache->key);
    cache->fillRevisit = UINT32_MAX;
    for (uint32_t i = 0; cache->pageCount > i; i++) cache->pages[i].firstUse = UINT32_MAX;
    loadRoomMemory(cache);
    CtrMemory_setHeapReliever(relieveFailedAlloc, cache);
    logInfo("Textures: %u pages, %.1f MB RAM for packed pages, background packing %s, read-ahead %s\n", cache->pageCount,
            megabytes(cache->packedBudgetBytes), cache->worker != nullptr ? "on" : "off", cache->reader != nullptr ? "on" : "off");
}

void CtrTextures_stopWorker(CtrTextureCache* cache) {
    CtrPackWorker_stop(cache->worker);
    cache->worker = nullptr;
    if (cache->reader != nullptr) {
        // Stopping the reader writes the record if it changed.
        finishRecording(cache, false);
        releaseReads(cache);
        collectWholeRead(cache, true);
        CtrTileReader_stop(cache->reader);
        cache->reader = nullptr;
    }
}

void CtrTextures_destroy(CtrTextureCache* cache) {
    CtrMemory_setHeapReliever(nullptr, nullptr);
    CtrTextures_stopWorker(cache);
    CtrQoi_releaseWorkspace();
    CtrOpenFiles_removeKeeper(cache->fileKeeper);
    closeCacheFiles(cache);
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        CtrPage* page = &cache->pages[i];
        releasePage(cache, page);
        dropPacked(cache, page);
        free(page->tiles);
    }
    free(cache->pages);
    cache->pages = nullptr;
    cache->pageCount = 0;
    for (uint32_t r = 0; cache->roomCount > r; r++) arrfree(cache->roomTiles[r]);
    free(cache->roomTiles);
    cache->roomTiles = nullptr;
    cache->roomCount = 0;
    arrfree(cache->recording);
    C3D_TexDelete(&cache->clear);
}

static void secondLook(CtrTextureCache* cache);
// keepLinearFloor's eviction, one tile at a time: the least recently used tile
// not drawn in the last idleFrames frames (0: any but the current frame's),
// the first in page and tile order of those last used in the same frame, as
// CtrTextures_evictOldestIdle takes them. keepLinearFloor checks the floor
// after each tile, so it frees only what the floor needs: evicting the whole
// oldest frame at once could take dozens of tiles (MBs) for a deficit of a few
// hundred KB, and they all load again when drawn. Its age guard (nothing drawn
// in the last LINEAR_FLOOR_KEEP_FRAMES frames) is unchanged.
//
// Late code. It sits here, before its caller, because
// defined after CtrTextures_beginFrame it made GCC emit beginFrame after
// CtrTextures_logStats (UNDERTALE's text moved); the #line below puts every
// later __LINE__ back. keepLinearFloor's call keeps the same arguments, so
// only its target changes. False if no tile is that idle.
__attribute__((noinline, section(".text.late.texevict"))) bool CtrTextures_evictLeastRecentIdle(CtrTextureCache* cache, uint32_t idleFrames) {
    uint32_t usedBefore = cache->frame > idleFrames ? cache->frame - idleFrames : 0;
    CtrPage* victimPage = nullptr;
    CtrTile* victim = nullptr;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* page = &cache->pages[p];
        if (page->residentTiles == 0) continue;
        for (int32_t i = 0; tileCount(page) > i; i++) {
            CtrTile* tile = &page->tiles[i];
            if (!tile->valid || tile->lastUsedFrame >= usedBefore) continue;
            if (victim != nullptr && tile->lastUsedFrame >= victim->lastUsedFrame) continue;
            victimPage = page;
            victim = tile;
        }
    }
    if (victim == nullptr) return false;
    evictTile(cache, victimPage, victim);
    return true;
}
#line 1880

// Keeps LINEAR_FLOOR_BYTES of linear memory free: cached sound effects go
// first (as far as audio lets them, see CtrAudioSystem_shedSfx), being quicker
// to load again, then the least recently used tiles, leaving those drawn in
// the last LINEAR_FLOOR_KEEP_FRAMES frames. Logs what went, at most once a
// second.
static void keepLinearFloor(CtrTextureCache* cache) {
    static uint32_t evictedBytes = 0;
    static uint32_t shedBytes = 0;
    static uint32_t lastLogFrame = 0;
    uint32_t freeBytes = linearSpaceFree();
    if (freeBytes >= LINEAR_FLOOR_BYTES) return;
    shedBytes += CtrAudioSystem_shedSfx(LINEAR_FLOOR_BYTES - freeBytes);
    uint32_t before = cache->residentBytes;
    while (LINEAR_FLOOR_BYTES > linearSpaceFree()) {
        uint32_t oldest = 0;
        if (!oldestEvictable(cache, cache->frame, &oldest) || oldest + LINEAR_FLOOR_KEEP_FRAMES >= cache->frame) break;
        CtrTextures_evictLeastRecentIdle(cache, 0);
    }
    evictedBytes += before - cache->residentBytes;
    if ((evictedBytes > 0 || shedBytes > 0) && cache->frame - lastLogFrame >= 30) {
        logInfo("Textures: freed %.2f MB of sound effects and evicted %.1f MB of tiles to keep linear memory free (%.1f MB free, %.1f MB "
                "resident)\n",
                megabytes(shedBytes), megabytes(evictedBytes), megabytes(linearSpaceFree()), megabytes(cache->residentBytes));
        evictedBytes = 0;
        shedBytes = 0;
        lastLogFrame = cache->frame;
    }
}

#ifdef CTRPORT_RENDER_TIMING
// A settling test (timing builds only): texdelay.txt in the game folder holds
// a number of microseconds to busy-wait at the start of every frame, to see
// whether frame timing alone changes which pages the packed cache keeps.
static __attribute__((noinline, section(".text.late.texcache"))) void texDelay(void) {
    static int32_t delayUs = -1;
    if (delayUs < 0) {
        delayUs = 0;
        FILE* f = fopen(CTRPORT_GAME_DIR "/texdelay.txt", "r");
        if (f != nullptr) {
            int value = 0;
            if (fscanf(f, "%d", &value) == 1 && value > 0) delayUs = value;
            fclose(f);
        }
        logInfo("Textures: settling test: %d us busy-wait per frame\n", (int) delayUs);
    }
    if (delayUs == 0) return;
    uint64_t until = svcGetSystemTick() + (uint64_t) delayUs * (SYSCLOCK_ARM11 / 1000000u);
    while (svcGetSystemTick() < until) {}
}
#endif

// ===[ Copies ranked by the loads they save ]===
//
// A packed copy is worth the tile loads it turns from SD reads into RAM
// reads. Each page's value is a decaying count of those loads: its loads
// from the cache file while it has no copy (what a copy would save), and
// while it has one, the frames its copy serves loads in. Promotion used to
// try the pending page drawn last (ties to the lowest index) and could
// reclaim only copies nobody draws, so a scene's packed set froze on
// whichever page fit the startup copies' leftovers first, whatever it was
// worth (Chapter 4's Titan fight on the Old 3DS: 10 ms a frame of SD reads
// either way, by drawing mode; docs/3ds-hardware.md, "The packed cache
// freezes on whichever page fits first"). Now the pending page worth most
// per KB goes first, and where the idle copies don't make room for it, it
// may displace copies that are drawn but have served no load in
// TEXCACHE_SERVED_FRAMES, if its recent loads come to TEXCACHE_DENSITY per MB
// of it, it is worth TEXCACHE_BEAT times as much per KB as each, and each
// has been in RAM TEXCACHE_IMMUNE_FRAMES. The admission
// itself is unchanged (makePromotionRoom, then queueWholeRead): one promotion
// a frame at most, a whole-file read on the reader thread, budget and reader
// heap checked first, the tiles' synchronous load the fallback.
//
// This is late code, and the one call to it replaces
// beginFrame's call to collectWholeRead, which it makes first. Nothing above
// changes, as a line added there would move code (safeMalloc and the like
// pass __LINE__), and a call to it from makePromotionRoom in place of
// idleForPromotion would change how beginFrame keeps its registers.
#define TEXCACHE_LATE __attribute__((noinline, section(".text.late.texcache")))
// The most pages a cache has seen is 75 (DELTARUNE Chapter 5; Chapter 4 has 43,
// UNDERTALE 26). A cache with more gets no ranking: promotion works as it
// did before this code, and the table is forgotten.
#define TEXCACHE_PAGES 128
#define TEXCACHE_LOAD 16                   // the value one load adds
#define TEXCACHE_DECAY_FRAMES 30           // value falls by a quarter this often
#define TEXCACHE_FLOOR (4 * TEXCACHE_LOAD) // the least value that may displace a copy
#define TEXCACHE_DENSITY (8 * TEXCACHE_LOAD) // ... and the least per MB of the copy it brings in
#define TEXCACHE_BEAT 2
#define TEXCACHE_SERVED_FRAMES 60
#define TEXCACHE_IMMUNE_FRAMES 120

typedef struct {
    uint32_t value;
    uint32_t fileLoads;   // the page's fileLoads when last looked at
    uint32_t resident;    // its residentTiles when last looked at
    uint32_t served;      // frames its copies served loads in (for the log)
    uint32_t servedFrame; // when its copy last served a load (or came into RAM)
    uint32_t keptFrame;   // when its copy came into RAM
    bool packed;
} TexcachePage;

static struct {
    const CtrPage* pages; // the cache the table is for (a new chapter makes a new one)
    uint32_t pageCount;
    uint32_t frame;       // the frame last looked at
    uint32_t decayFrame;
    TexcachePage page[TEXCACHE_PAGES];
} texcache __attribute__((section(".bss.late.texcache")));

// Whether the copy of page `id` may go to make room for `keep` (worth
// `wanted`), beside the idle copies idleForPromotion lets go.
static TEXCACHE_LATE bool texcacheDisplaces(const CtrTextureCache* cache, uint32_t id, const CtrPage* keep, uint32_t wanted) {
    const CtrPage* page = &cache->pages[id];
    const TexcachePage* t = &texcache.page[id];
    if (page == keep || page == cache->busyPage || page->packed == nullptr || page->read != nullptr || page->promotionPending ||
        (cache->wholeRead != nullptr && id == cache->wholePage) || cache->frame - t->keptFrame < TEXCACHE_IMMUNE_FRAMES ||
        cache->frame - t->servedFrame < TEXCACHE_SERVED_FRAMES || idleForPromotion(cache, page, keep))
        return false;
    // Worth TEXCACHE_BEAT times as much per KB: wanted / its size >= BEAT * value / this size.
    if ((uint64_t) wanted * page->packedSize < (uint64_t) TEXCACHE_BEAT * t->value * keep->promotionBytes) return false;
    // Resident tiles stay (they don't need the copy); tiles on their way from it hold it.
    for (int32_t i = 0; page->layoutKnown && i < page->tilesX * page->tilesY; i++) {
        const CtrTile* tile = &page->tiles[i];
        if (tile->reserved || tile->requested || tile->wanted) return false;
    }
    return true;
}

// The bytes makePromotionRoom would have to free for a copy of `size` bytes
// (the same sums: packed budget and reader heap headroom).
static TEXCACHE_LATE uint64_t texcacheNeeded(const CtrTextureCache* cache, uint32_t size) {
    uint64_t held = (uint64_t) cache->packedBytes + cache->wholeBytes;
    uint64_t heapNeeded = (uint64_t) size + CtrTileReader_heapReserve();
    uint32_t freeBytes = heapFree();
    uint64_t needed = heapNeeded > freeBytes ? heapNeeded - freeBytes : 0;
    if (held + size > cache->packedBudgetBytes && held + size - cache->packedBudgetBytes > needed)
        needed = held + size - cache->packedBudgetBytes;
    return needed;
}

// Each frame before promotion: collects the whole-file read, brings the
// values up to date, leaves pending only the page worth most per KB (so that
// promotePending tries it), and if the idle copies can't make room for it,
// drops the copies it displaces, least worth per KB first. (noipa: so that
// beginFrame calls it exactly as it called collectWholeRead, `false` and all.)
void RoomLoad_collect(CtrTextureCache* cache, bool drop); // roomload, z_roomload.c: collectWholeRead and its own
#line 2059
static TEXCACHE_LATE __attribute__((noipa)) void texcacheCollectWholeRead(CtrTextureCache* cache, bool drop) {
    RoomLoad_collect(cache, drop);
    if (cache->pageCount > TEXCACHE_PAGES) {
        texcache.pages = nullptr;
        return;
    }
    uint32_t frame = cache->frame;
    // A new cache (a chapter's) can get the old one's page array back from
    // malloc, so its frames starting over count as well as a new address.
    if (texcache.pages != cache->pages || texcache.pageCount != cache->pageCount || frame < texcache.frame) {
        memset(&texcache, 0, sizeof(texcache));
        texcache.pages = cache->pages;
        texcache.pageCount = cache->pageCount;
        texcache.decayFrame = frame;
    }
    texcache.frame = frame;
    bool decay = frame - texcache.decayFrame >= TEXCACHE_DECAY_FRAMES;
    if (decay) texcache.decayFrame = frame;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        const CtrPage* p = &cache->pages[i];
        TexcachePage* t = &texcache.page[i];
        if (decay) t->value -= t->value >> 2;
        if (p->fileLoads > t->fileLoads) t->value += (p->fileLoads - t->fileLoads) * TEXCACHE_LOAD;
        t->fileLoads = p->fileLoads;
        bool packed = p->packed != nullptr;
        if (packed && !t->packed) {
            t->keptFrame = frame;
            t->servedFrame = frame;
        } else if (packed && (p->residentTiles != t->resident || p->read != nullptr)) {
            // Tiles came from the copy, or went and may come again, or the
            // reader is decoding some from it.
            t->servedFrame = frame;
            t->served++;
            t->value += TEXCACHE_LOAD;
        }
        t->resident = p->residentTiles;
#ifdef CTRPORT_RENDER_TIMING
        static const char dropped[] __attribute__((section(".rodata.late.texcache"))) =
            "Textures: [packdrop] frame %u page %u: value %u, last served frame %u, kept since frame %u, %u tiles resident\n";
        if (!packed && t->packed)
            logInfo(dropped, (unsigned) frame, (unsigned) i, (unsigned) t->value, (unsigned) t->servedFrame, (unsigned) t->keptFrame,
                    (unsigned) p->residentTiles);
#endif
        t->packed = packed;
    }
    // promotePending's own conditions for a page. (A page left out asks
    // again at its next load from the file, which is what gives it value.)
    if (cache->reader == nullptr || cache->wholeRead != nullptr || workerBringingPages(cache)) return;
    // promotePending's own pick (drawn last, ties to the lowest index): where
    // the idle copies make room for it, it goes as it always has.
    CtrPage* recent = nullptr;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        CtrPage* p = &cache->pages[i];
        if (p->promotionPending && p->packed == nullptr && !p->failed && p->cacheFile == CTR_CACHE_FILE_VALID &&
            (recent == nullptr || p->lastUsedFrame > recent->lastUsedFrame))
            recent = p;
    }
    if (recent == nullptr) return;
    if (recent->promotionBytes <= cache->packedBudgetBytes) {
        uint64_t room = 0;
        for (uint32_t i = 0; i < cache->pageCount; i++) {
            const CtrPage* p = &cache->pages[i];
            if (idleForPromotion(cache, p, recent)) room += p->packedSize;
        }
        if (texcacheNeeded(cache, recent->promotionBytes) <= room) return;
    }
    CtrPage* best = nullptr;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        CtrPage* p = &cache->pages[i];
        if (!p->promotionPending || p->promotionBytes == 0 || p->packed != nullptr || p->failed || p->cacheFile != CTR_CACHE_FILE_VALID)
            continue;
        if (best == nullptr || (uint64_t) texcache.page[i].value * best->promotionBytes >
                                   (uint64_t) texcache.page[best - cache->pages].value * p->promotionBytes)
            best = p;
    }
    if (best == nullptr) return;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        if (&cache->pages[i] != best) cache->pages[i].promotionPending = false;
    }
    // The room promotePending's makePromotionRoom will need (the same sums),
    // if it runs this frame.
    uint32_t wanted = texcache.page[best - cache->pages].value;
    uint32_t size = best->promotionBytes;
    // A displacement reads the whole page from SD, so it waits until the
    // page's recent loads are worth that much: TEXCACHE_DENSITY per MB. (On
    // the Old 3DS a MB takes about 200 ms to read and a tile load from the
    // file 25 ms, so 8 loads a MB is where the read starts to pay.)
    if (wanted < TEXCACHE_FLOOR || (uint64_t) wanted * MB < (uint64_t) TEXCACHE_DENSITY * size || size > cache->packedBudgetBytes) return;
    uint64_t needed = texcacheNeeded(cache, size);
    uint64_t idle = 0;
    uint64_t more = 0;
    for (uint32_t i = 0; i < cache->pageCount; i++) {
        const CtrPage* p = &cache->pages[i];
        if (idleForPromotion(cache, p, best)) idle += p->packedSize;
        else if (texcacheDisplaces(cache, i, best, wanted)) more += p->packedSize;
    }
    // Enough idle copies (makePromotionRoom does the rest), or not enough
    // even with these (so drop none).
    if (needed <= idle || needed > idle + more) return;
    while (needed > idle) {
        uint32_t victim = UINT32_MAX;
        for (uint32_t i = 0; i < cache->pageCount; i++) {
            if (!texcacheDisplaces(cache, i, best, wanted)) continue;
            if (victim == UINT32_MAX || (uint64_t) texcache.page[i].value * cache->pages[victim].packedSize <
                                            (uint64_t) texcache.page[victim].value * cache->pages[i].packedSize)
                victim = i;
        }
        if (victim == UINT32_MAX) return;
        uint32_t bytes = cache->pages[victim].packedSize;
        static const char gives[] __attribute__((section(".rodata.late.texcache"))) =
            "Textures: packed page %u (%u KB, value %u) gives way to page %u (%u KB, value %u)\n";
        logInfo(gives, (unsigned) victim, (unsigned) (bytes / 1024), (unsigned) texcache.page[victim].value, (unsigned) (best - cache->pages),
                (unsigned) (size / 1024), (unsigned) wanted);
        // dropPacked's work for a copy no read uses (a call to it would
        // change the order the compiler lays out the functions above in).
        CtrPacked_free(cache->pages[victim].packed);
        cache->packedBytes -= bytes;
        cache->pages[victim].packed = nullptr;
        cache->pages[victim].packedSize = 0;
        cache->packedReclaims++;
        cache->packedReclaimBytes += bytes;
        idle += bytes;
    }
}

void CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame) {
#ifdef CTRPORT_RENDER_TIMING
    texDelay();
#endif
    double lastFrameMs = (double) cache->frameLoadNanos / 1e6;
    if (lastFrameMs >= HITCH_LOG_MS) logInfo("Textures: frame %u spent %.0f ms loading tiles\n", (unsigned) cache->frame, lastFrameMs);
    cache->frameLoadNanos = 0;
    cache->frame = frame;
    // No batch of the new frame holds the copies given up in the last.
    for (uint32_t i = 0; CTR_MIRROR_MAX > i; i++) cache->mirrors[i].dead = false;
    if (frame - cache->heapCheckFrame >= HEAP_CHECK_INTERVAL_FRAMES) {
        cache->heapCheckFrame = frame;
        relieveHeap(cache);
        // The page decoding workspace (several MB, see ctr_qoi.c) is the
        // worker's while it packs. Once it is done, decodes from images are
        // rare (a cache file gone bad), and such a decode allocates it again.
        if (!workerBusy(cache)) {
            size_t freed = CtrQoi_releaseWorkspaceIfIdle();
            if (freed > 0) logInfo("Textures: freed the page decoding workspace, %.1f MB\n", megabytes((uint32_t) freed));
        }
    }
    keepLinearFloor(cache);
    texcacheCollectWholeRead(cache, false);
    if (cache->worker != nullptr) collectPacked(cache);
    promotePending(cache);
    if (0 <= cache->recordRoom && frame >= cache->recordUntil) finishRecording(cache, true);
    if (cache->fillPending && frame >= cache->fillFrom) {
        if (frame == cache->fillFrom && cache->runner != nullptr) secondLook(cache);
        fillWanted(cache);
    }
}

bool CtrTextures_evictOldest(CtrTextureCache* cache) {
    return CtrTextures_evictOldestIdle(cache, 0);
}

bool CtrTextures_evictOldestIdle(CtrTextureCache* cache, uint32_t idleFrames) {
    uint32_t oldest = 0;
    uint32_t usedBefore = cache->frame > idleFrames ? cache->frame - idleFrames : 0;
    if (!oldestEvictable(cache, usedBefore, &oldest)) return false;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* page = &cache->pages[p];
        if (page->residentTiles == 0) continue;
        for (int32_t i = 0; tileCount(page) > i; i++) {
            CtrTile* tile = &page->tiles[i];
            if (!tile->valid || tile->lastUsedFrame != oldest) continue;
            evictTile(cache, page, tile);
        }
    }
    return true;
}

bool CtrTextures_evictLeastRecent(CtrTextureCache* cache) {
    CtrPage* victimPage = nullptr;
    CtrTile* victim = nullptr;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* page = &cache->pages[p];
        if (page->residentTiles == 0) continue;
        for (int32_t i = 0; tileCount(page) > i; i++) {
            CtrTile* tile = &page->tiles[i];
            if (!tile->valid || (victim != nullptr && tile->lastUsedFrame >= victim->lastUsedFrame)) continue;
            victimPage = page;
            victim = tile;
        }
    }
    if (victim == nullptr) return false;
    evictTile(cache, victimPage, victim);
    return true;
}

CtrPage* CtrTextures_acquire(CtrTextureCache* cache, uint32_t pageId) {
    if (pageId >= cache->pageCount) return nullptr;
    CtrPage* page = &cache->pages[pageId];
    page->lastUsedFrame = cache->frame;
    if (page->layoutKnown) return page;
    if (page->failed || page->retryFrame > cache->frame) return nullptr;
    if (layoutFromDataWin(cache, pageId, page) || loadPackInfo(cache, pageId, page) || loadImageLayout(cache, pageId, page)) return page;
    return nullptr;
}

void CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1) {
    CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, 0);
}

void CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, uint64_t drawnElsewise) {
    // A sprite made from a surface has a page of its own (ctr_renderer.c),
    // not one of the cache's: there is nothing to load or record. Its page
    // number would be made up, as pageIdOf divides its distance from the
    // cache's pages by the page size, which it isn't a multiple of: the room
    // record got entries for pages far past the last, and finishRecording
    // read through them (a data abort on both consoles in Chapter 3's rooms
    // 89 and 147, which draw surface sprites in their first frames).
    if (!cacheOwns(cache, page)) return;
    bool missing = false;
    bool recording = 0 <= cache->recordRoom;
    uint32_t k = 0;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        CtrTile* tile = &page->tiles[ty * page->tilesX + tx0];
        for (int32_t tx = tx0; tx1 >= tx; tx++, tile++, k++) {
            if (recording && tile->seenVisit != cache->recordVisit) recordTile(cache, page, tile);
            if (64 > k && (drawnElsewise >> k & 1u) != 0) continue;
            // Drawn in the frame before and first drawn in this one now.
            if (tile->lastUsedFrame + 1 == cache->frame && tile->valid && tile->mirror == nullptr && CTR_HOT_MAX > cache->hotCount) {
                cache->hot[cache->hotCount++] = tile;
            }
            tile->lastUsedFrame = cache->frame;
            if (tileMissing(page, tile)) {
                tile->requested = true;
                missing = true;
            }
        }
    }
    if (missing) loadRequested(cache, pageIdOf(cache, page), page);
}

// ===[ Room prefetch ]===

// For markSprite: every frame of the sprite is drawn in the first frames.
#define ALL_FRAMES_NOW (-1)

static void markTpag(CtrTextureCache* cache, int32_t tpagIndex, int32_t depth, bool now) {
    const DataWin* dw = cache->dataWin;
    if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) return;
    const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
    if (0 > tpag->texturePageId) return;
    CtrPage* page = CtrTextures_acquire(cache, (uint32_t) tpag->texturePageId);
    if (page == nullptr || 0 >= tpag->sourceWidth || 0 >= tpag->sourceHeight) return;
    int32_t tx0 = tpag->sourceX / page->tileW;
    int32_t ty0 = tpag->sourceY / page->tileH;
    int32_t tx1 = (tpag->sourceX + tpag->sourceWidth - 1) / page->tileW;
    int32_t ty1 = (tpag->sourceY + tpag->sourceHeight - 1) / page->tileH;
    if (0 > tx0) tx0 = 0;
    if (0 > ty0) ty0 = 0;
    if (tx1 >= page->tilesX) tx1 = page->tilesX - 1;
    if (ty1 >= page->tilesY) ty1 = page->tilesY - 1;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++) {
            CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
            tile->wanted = true;
            tile->now = tile->now || now;
            // Keep it from being evicted to make room for the others.
            tile->lastUsedFrame = cache->frame;
        }
    }
    if (!now) return;
    if (!page->wantedNow || depth > page->wantDepth) page->wantDepth = depth;
    page->wantedNow = true;
}

// Marks every frame of the sprite wanted, and frame nowFrame (or all of them)
// drawn in the first frames.
static void markSprite(CtrTextureCache* cache, int32_t spriteIndex, int32_t depth, int32_t nowFrame) {
    const DataWin* dw = cache->dataWin;
    if (0 > spriteIndex || (uint32_t) spriteIndex >= dw->sprt.count) return;
    const Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->tpagIndices == nullptr) return;
    for (uint32_t i = 0; sprite->textureCount > i; i++) {
        markTpag(cache, sprite->tpagIndices[i], depth, nowFrame == ALL_FRAMES_NOW || (int32_t) i == nowFrame);
    }
}

// The frame a sprite shows at image index `index`.
static int32_t spriteFrame(const DataWin* dw, int32_t spriteIndex, float index) {
    if (0 > spriteIndex || (uint32_t) spriteIndex >= dw->sprt.count) return 0;
    int32_t count = (int32_t) dw->sprt.sprites[spriteIndex].textureCount;
    if (0 >= count) return 0;
    int32_t frame = (int32_t) floorf(index) % count;
    return 0 > frame ? frame + count : frame;
}

static void markBackground(CtrTextureCache* cache, int32_t backgroundIndex, int32_t depth) {
    const DataWin* dw = cache->dataWin;
    if (0 > backgroundIndex || (uint32_t) backgroundIndex >= dw->bgnd.count) return;
    markTpag(cache, dw->bgnd.backgrounds[backgroundIndex].tpagIndex, depth, true);
}

static void markTiles(CtrTextureCache* cache, const RoomTile* tiles, uint32_t count) {
    if (tiles == nullptr) return;
    for (uint32_t i = 0; count > i; i++) {
        if (tiles[i].useSpriteDefinition) markSprite(cache, tiles[i].backgroundDefinition, tiles[i].tileDepth, ALL_FRAMES_NOW);
        else markBackground(cache, tiles[i].backgroundDefinition, tiles[i].tileDepth);
    }
}

static void markInstances(CtrTextureCache* cache, const Runner* runner) {
    for (ptrdiff_t i = 0; arrlen(runner->instances) > i; i++) {
        const Instance* instance = runner->instances[i];
        if (instance->destroyed || !instance->visible) continue;
        markSprite(cache, instance->spriteIndex, instance->depth, spriteFrame(cache->dataWin, instance->spriteIndex, instance->imageIndex));
    }
}

// Marks the tiles of everything the room shows from the start: backgrounds,
// tiles, layer sprites and the sprites of visible instances (every frame),
// each with the depth it is drawn at.
static void markRoomTiles(CtrTextureCache* cache, const Runner* runner) {
    for (int32_t i = 0; 8 > i; i++) {
        const RuntimeBackground* bg = &runner->backgrounds[i];
        if (bg->visible) markBackground(cache, bg->backgroundIndex, bg->foreground ? INT32_MIN : INT32_MAX);
    }
    const Room* room = runner->currentRoom;
    if (room != nullptr && room->payloadLoaded) {
        markTiles(cache, room->tiles, room->tileCount);
        for (uint32_t i = 0; room->layers != nullptr && room->layerCount > i; i++) {
            const RoomLayer* layer = &room->layers[i];
            if (!layer->visible) continue;
            if (layer->backgroundData != nullptr && layer->backgroundData->visible) {
                markSprite(cache, layer->backgroundData->spriteIndex, layer->depth, ALL_FRAMES_NOW);
            }
            if (layer->tilesData != nullptr) markBackground(cache, layer->tilesData->backgroundIndex, layer->depth);
            if (layer->assetsData != nullptr) {
                const RoomLayerAssetsData* assets = layer->assetsData;
                markTiles(cache, assets->legacyTiles, assets->legacyTileCount);
                for (uint32_t j = 0; assets->sprites != nullptr && assets->spriteCount > j; j++) {
                    int32_t sprite = assets->sprites[j].spriteIndex;
                    markSprite(cache, sprite, layer->depth, spriteFrame(cache->dataWin, sprite, assets->sprites[j].frameIndex));
                }
            }
        }
    }
    markInstances(cache, runner);
}

// Marks the tiles the room drew in its first frames last time wanted, those
// of its first drawn frame as drawn first, and numbers their pages in the
// order first drawn. Returns how many are drawn first.
static uint32_t markRemembered(CtrTextureCache* cache, int32_t room) {
    if (0 > room || (uint32_t) room >= cache->roomCount) return 0;
    const uint32_t* entries = cache->roomTiles[room];
    uint32_t rank = 0;
    uint32_t first = 0;
    for (ptrdiff_t i = 0; arrlen(entries) > i; i++) {
        CtrPage* page = CtrTextures_acquire(cache, ROOM_TILE_PAGE(entries[i]));
        int32_t t = ROOM_TILE_INDEX(entries[i]);
        if (page == nullptr || t >= tileCount(page)) continue;
        CtrTile* tile = &page->tiles[t];
        tile->wanted = true;
        tile->lastUsedFrame = cache->frame;
        if (!page->formatsKnown && !tile->valid && (entries[i] & ROOM_TILE_FORMAT_KNOWN) != 0 && CTR_TILE_FORMAT_COUNT > ROOM_TILE_FORMAT(entries[i])) {
            tile->format = ROOM_TILE_FORMAT(entries[i]);
            tile->formatRemembered = true;
        }
        if ((entries[i] & ROOM_TILE_LATE) != 0) continue;
        tile->now = true;
        first++;
        if (page->firstUse == UINT32_MAX) page->firstUse = rank++;
        if (!page->wantedNow) page->wantDepth = INT32_MIN;
        page->wantedNow = true;
    }
    return first;
}

// Pages are read in the order the room first drew them last time, then
// those it hadn't drawn by depth, deepest (drawn first) first.
static bool readsBefore(const CtrPage* a, const CtrPage* b) {
    if (a->firstUse != b->firstUse) return a->firstUse < b->firstUse;
    return a->wantDepth > b->wantDepth;
}

// Has the reader read the tiles drawn in the room's first frames that aren't
// resident, from the pages that are neither in RAM nor known to have no
// cache file, in the order they are drawn (see readsBefore): the first frame
// then installs one page's tiles while the next page is read. Tiles of a
// page whose read hasn't started yet join it. Where the reader decodes, it
// also decodes those of pages in RAM. The other frames of animations wait
// for filling in. Returns how many reads were queued.
static uint32_t readAhead(CtrTextureCache* cache) {
    if (cache->reader == nullptr) return 0;
    uint32_t* order = malloc(cache->pageCount * sizeof(uint32_t));
    if (order == nullptr) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        CtrPage* page = &cache->pages[i];
        if (!page->wantedNow || !(mayRead(cache, i, page) || mayDecode(cache, page))) continue;
        if (page->read != nullptr) {
            // Only a read from the SD card that hasn't started takes more.
            for (int32_t t = 0; page->read->src == nullptr && tileCount(page) > t; t++) {
                CtrTile* tile = &page->tiles[t];
                if (!readTakes(page, tile, true) || page->read->want[t]) continue;
                uint8_t* dst = mayReserve(cache, page, tile) ? reserveTile(cache, page, tile) : nullptr;
                if (!CtrTileReader_addTile(cache->reader, page->read, t, dst, dst != nullptr ? tile->tex.size : 0, tile->format)) {
                    unreserveTile(cache, page, tile);
                    break;
                }
            }
            continue;
        }
        // Insertion sort.
        uint32_t at = count++;
        while (at > 0 && readsBefore(page, &cache->pages[order[at - 1]])) {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    uint32_t queued = 0;
    for (uint32_t k = 0; count > k; k++) {
        CtrPage* page = &cache->pages[order[k]];
        bool ok = page->packed != nullptr ? queueDecode(cache, order[k], page, true) : queueRead(cache, order[k], page, true);
        if (ok) queued++;
    }
    free(order);
    return queued;
}

// The room's first step creates instances and sets sprites (DELTARUNE
// creates the party there), so the room is looked at again once it has run.
static void secondLook(CtrTextureCache* cache) {
    uint64_t start = nowNanos();
    markInstances(cache, cache->runner);
    uint32_t queued = readAhead(cache);
    if (queued > 0) logInfo("Textures: %u more pages to read after the room's first step (%.1f ms)\n", (unsigned) queued, msSince(start));
}

void CtrTextures_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    if (runner == nullptr || cache->pageCount == 0) return;
    uint64_t start = nowNanos();
    cache->roomStartFrame = cache->frame;
    cache->roomThrashed = false;
    // What the last room didn't use of its read-ahead is no use now. Nor is a
    // whole file not started yet: the new room's tiles are read first.
    releaseReads(cache);
    if (cache->wholeRead != nullptr && CtrTileReader_unqueue(cache->reader, cache->wholeRead)) collectWholeRead(cache, true);
    cache->readPages = 0;
    cache->readBytes = 0;
    cache->readMs = 0.0;
    if (0 <= cache->recordRoom) finishRecording(cache, false);
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        CtrPage* page = &cache->pages[i];
        page->wantedNow = false;
        page->promotionPending = false;
        page->firstUse = UINT32_MAX;
        page->fillRead = false;
        for (int32_t t = 0; page->layoutKnown && tileCount(page) > t; t++) {
            page->tiles[t].wanted = false;
            page->tiles[t].now = false;
        }
    }
    uint32_t remembered = markRemembered(cache, runner->currentRoomIndex);
    markRoomTiles(cache, runner);
    startRecording(cache, runner->currentRoomIndex);

    uint32_t pages = 0;
    uint32_t tiles = 0;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        CtrPage* page = &cache->pages[i];
        uint32_t missing = 0;
        for (int32_t t = 0; page->layoutKnown && tileCount(page) > t; t++) {
            if (page->tiles[t].wanted && tileMissing(page, &page->tiles[t])) missing++;
        }
        if (missing > 0) pages++;
        tiles += missing;
    }
    double markMs = msSince(start);
    uint32_t reading = tiles > 0 ? readAhead(cache) : 0;
    // Filling starts once the room's first step has run, with what has been
    // read by then; the first frame draws (and loads) what it shows.
    cache->runner = runner;
    cache->fillPending = true;
    cache->fillPage = 0;
    cache->fillRevisit = UINT32_MAX;
    cache->fillFrom = cache->frame + 1;
    cache->fillTiles = 0;
    cache->fillMs = 0.0;
    if (tiles > 0) {
        logInfo("Textures: room %s wants %u tiles of %u pages that aren't resident (marked in %.1f ms, %u remembered), reading %u pages ahead\n",
                runner->currentRoom->name, (unsigned) tiles, (unsigned) pages, markMs, (unsigned) remembered, (unsigned) reading);
    }
}

static double averageMs(const CtrLoadStats* stats) {
    return stats->count > 0 ? stats->msTotal / stats->count : 0.0;
}

void CtrTextures_logStats(CtrTextureCache* cache) {
    uint32_t pages = 0;
    uint32_t tiles = 0;
    uint32_t packed = 0;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        if (cache->pages[i].residentTiles > 0) pages++;
        tiles += cache->pages[i].residentTiles;
        if (cache->pages[i].packed != nullptr) packed++;
    }
    logInfo("Textures: %u tiles of %u/%u pages resident, %.1f of %.1f MB; %u packed in RAM, %.1f of %.1f MB; %u cached; loads: %u RAM (%u tiles, "
            "avg %.1f ms), %u read ahead (%u tiles, avg %.1f ms), %u SD (%u tiles, avg %.0f ms), %u image (%u tiles, avg %.0f ms); %u tiles decoded "
            "ahead; %u evictions\n",
            tiles, pages, cache->pageCount, megabytes(cache->residentBytes), megabytes(cache->budgetBytes), packed, megabytes(cache->packedBytes),
            megabytes(cache->packedBudgetBytes), cache->workerPacked, cache->fromRam.count, cache->fromRam.tiles, averageMs(&cache->fromRam),
            cache->fromRead.count, cache->fromRead.tiles, averageMs(&cache->fromRead), cache->fromFile.count, cache->fromFile.tiles,
            averageMs(&cache->fromFile), cache->fromImage.count, cache->fromImage.tiles, averageMs(&cache->fromImage), cache->decodedAhead,
            cache->evictionCount);
#ifdef CTRPORT_RENDER_TIMING
    CtrTextures_logPackedSet(cache, "stats");
#endif
}

#ifdef CTRPORT_RENDER_TIMING
// Two kinds of line: "[packset]" names the packed pages (id:KB) and a hash of
// the set, "[pageloads]" each page's loads so far as id:ram/sd (frames its
// copies served loads in, loads from its cache file), for the pages with any. Both are
// cumulative since the chapter began.
TEXCACHE_LATE void CtrTextures_logPackedSet(const CtrTextureCache* cache, const char* label) {
    char line[400];
    int32_t used = 0;
    uint32_t hash = 2166136261u;
    uint32_t count = 0;
    line[0] = '\0';
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        const CtrPage* p = &cache->pages[i];
        if (p->packed == nullptr) continue;
        count++;
        hash = (hash ^ i) * 16777619u;
        if (used < (int32_t) sizeof(line) - 16) used += snprintf(line + used, sizeof(line) - used, " %u:%u", (unsigned) i, (unsigned) (p->packedSize / 1024));
    }
    logInfo("Textures: [packset] %s frame %u: %u pages, %u of %u KB, set %08lx:%s\n", label, (unsigned) cache->frame, (unsigned) count,
            (unsigned) (cache->packedBytes / 1024), (unsigned) (cache->packedBudgetBytes / 1024), (unsigned long) hash, line);
    used = 0;
    line[0] = '\0';
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        const CtrPage* p = &cache->pages[i];
        uint32_t served = texcache.pages == cache->pages && i < TEXCACHE_PAGES ? texcache.page[i].served : 0;
        if (served + p->fileLoads == 0) continue;
        if (used > (int32_t) sizeof(line) - 40) {
            logInfo("Textures: [pageloads] %s frame %u:%s\n", label, (unsigned) cache->frame, line);
            used = 0;
            line[0] = '\0';
        }
        used += snprintf(line + used, sizeof(line) - used, " %u:%u/%u", (unsigned) i, (unsigned) served, (unsigned) p->fileLoads);
    }
    if (used > 0) logInfo("Textures: [pageloads] %s frame %u:%s\n", label, (unsigned) cache->frame, line);
}
#endif

// ===[ roomload: the room change loads the room's tiles (z_roomload.c) ]===
//
// Late code at the end of the file, so nothing above moves; it calls only functions that were already out of line
// (markSprite, loadRequestedFrom) and repeats the one-line helpers instead of calling them.
#define ROOMLOAD_TEX_LATE __attribute__((noinline, section(".text.late.roomload")))
// A cache file page starts only if its estimated time fits in what is left of the budget: a fixed cost (the open, the
// seeks, waiting behind the tile reader) plus a cost per KB, from Old and New measurements of 2026-10-20.
// A tile whose place in the packed file isn't known yet counts as ROOMLOAD_FILE_TILE_BYTES.
#define ROOMLOAD_FILE_FIXED_NANOS (45ull * 1000000ull)
#define ROOMLOAD_FILE_NANOS_PER_KB 1000000ull
#define ROOMLOAD_FILE_TILE_BYTES (32u * 1024u)
// Tiles loaded here are left as last used in the frame before this one, as if not drawn yet: so they get no VRAM copy
// ahead of their first draw, and evicting them later doesn't count as the room thrashing (evictTile's check).
#define ROOMLOAD_STAMP_TILES 64
// Sprites the room's code assigns are marked only while their missing tiles fit below the budget less this much. Filling
// in loads what is marked and stamps it as used, so marks that filled the budget pushed out tiles later rooms draw (o2ds
// ch2_rooms: post_music_boss_slide's first frame waited 124 ms for one).
#define ROOMLOAD_SPRITE_HEADROOM_BYTES (4u * 1024u * 1024u)
// Pass 0 may run this far past the deadline. Its tiles are ones the room's first shown frame draws, which would load
// them itself at the same cost: finishing them here moves that cost into the hidden frame instead of adding to it
// (Old 2DS ruins1: the deadline stopped pass 0 before page 24's tile, which then took 95 ms of the first shown frame).
#define ROOMLOAD_NOW_EXTRA_NANOS (200ull * 1000000ull)

uint32_t CtrTextures_roomLoadSpriteRoom(CtrTextureCache* cache);
uint32_t CtrTextures_roomLoadSprite(CtrTextureCache* cache, int32_t spriteIndex, uint32_t room);
void CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);

// What the room load's sprite marks may take: the budget less ROOMLOAD_SPRITE_HEADROOM_BYTES, what is resident and what
// the room already wants and is missing (its own tiles, marked by the original prefetchRoom). Without the last term the
// base's filling in loaded the room's wants and the marks together: 18.8 of 20.3 MB after the fireworks room
// on the Old 2DS (base 9.8 MB), and post_music_boss_slide's first shown frame waited for an evicted tile.
ROOMLOAD_TEX_LATE static void rlAheadNote(const CtrTextureCache* cache, uint32_t pageId, uint32_t tileIndex);
ROOMLOAD_TEX_LATE static void rlAheadStart(const CtrTextureCache* cache);
ROOMLOAD_TEX_LATE static void rlAheadTakeBack(CtrTextureCache* cache);
ROOMLOAD_TEX_LATE static void rlEvictForFirstFrame(CtrTextureCache* cache, uint32_t needed, uint32_t usedBefore);

ROOMLOAD_TEX_LATE uint32_t CtrTextures_roomLoadSpriteRoom(CtrTextureCache* cache) {
    rlAheadStart(cache);
    uint64_t used = (uint64_t) cache->residentBytes + ROOMLOAD_SPRITE_HEADROOM_BYTES;
    for (uint32_t pageId = 0; cache->pageCount > pageId; pageId++) {
        const CtrPage* page = &cache->pages[pageId];
        if (!page->layoutKnown || page->tiles == nullptr) continue;
        int32_t tiles = page->tilesX * page->tilesY;
        for (int32_t t = 0; tiles > t; t++) {
            const CtrTile* tile = &page->tiles[t];
            // tileMissing and tileBytes, repeated (late code calls nothing that was inlined).
            if (!tile->wanted || tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
            used += (uint64_t) page->tileW * (uint64_t) page->tileH *
                    (!page->formatsKnown ? 4u : CtrPack_texelBytes((CtrTileFormat) tile->format));
        }
    }
    return cache->budgetBytes > used ? (uint32_t) (cache->budgetBytes - used) : 0;
}

// Marks every frame of the sprite wanted (not drawn first): filling in loads it ahead of its first draw. Unlike
// markTpag it leaves the tiles' last use alone (the sprite isn't drawn yet), so it changes no eviction order. Only if
// the missing tiles it would add fit in `room`: returns what they take, or UINT32_MAX when it marked nothing.
ROOMLOAD_TEX_LATE uint32_t CtrTextures_roomLoadSprite(CtrTextureCache* cache, int32_t spriteIndex, uint32_t room) {
    const DataWin* dw = cache->dataWin;
    if (0 > spriteIndex || (uint32_t) spriteIndex >= dw->sprt.count) return UINT32_MAX;
    const Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->tpagIndices == nullptr) return UINT32_MAX;
    uint32_t adds = 0;
    for (int pass = 0; 2 > pass; pass++) {
        for (uint32_t i = 0; sprite->textureCount > i; i++) {
            int32_t tpagIndex = sprite->tpagIndices[i];
            if (0 > tpagIndex || (uint32_t) tpagIndex >= dw->tpag.count) continue;
            const TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
            if (0 > tpag->texturePageId) continue;
            CtrPage* page = CtrTextures_acquire(cache, (uint32_t) tpag->texturePageId);
            if (page == nullptr || 0 >= tpag->sourceWidth || 0 >= tpag->sourceHeight) continue;
            int32_t tx0 = tpag->sourceX / page->tileW;
            int32_t ty0 = tpag->sourceY / page->tileH;
            int32_t tx1 = (tpag->sourceX + tpag->sourceWidth - 1) / page->tileW;
            int32_t ty1 = (tpag->sourceY + tpag->sourceHeight - 1) / page->tileH;
            if (0 > tx0) tx0 = 0;
            if (0 > ty0) ty0 = 0;
            if (tx1 >= page->tilesX) tx1 = page->tilesX - 1;
            if (ty1 >= page->tilesY) ty1 = page->tilesY - 1;
            for (int32_t ty = ty0; ty1 >= ty; ty++) {
                for (int32_t tx = tx0; tx1 >= tx; tx++) {
                    CtrTile* tile = &page->tiles[ty * page->tilesX + tx];
                    if (pass == 1) {
                        // Item 7: the tiles these marks add, kept apart (rlAheadNote, file end).
                        if (!tile->wanted && !tile->valid && !(page->formatsKnown && tile->format == CTR_TILE_EMPTY))
                            rlAheadNote(cache, (uint32_t) tpag->texturePageId, (uint32_t) (ty * page->tilesX + tx));
                        tile->wanted = true;
                        continue;
                    }
                    // tileMissing and tileBytes, repeated (late code calls nothing that was inlined).
                    if (tile->wanted || tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
                    adds += (uint32_t) page->tileW * (uint32_t) page->tileH *
                            (!page->formatsKnown ? 4u : CtrPack_texelBytes((CtrTileFormat) tile->format));
                }
            }
        }
        if (pass == 0 && adds > room) return UINT32_MAX;
    }
    return adds;
}

// Loads the room's wanted tiles until the deadline. Pass 0: the tiles its first frame draws, from RAM, their
// read-ahead or their cache file (what that frame's draws would load; a page image's decode is left to them), up to
// ROOMLOAD_NOW_EXTRA_NANOS past the deadline. Pass 1: the others in RAM or read ahead. Pass 2: the others in cache
// files, a page only if its estimate fits. Every pass stops at the linear floor. Passes 1-2 stop at the texture budget
// rather than evict. Pass 0 first evicts tiles not used since before the room change (the old room's last frame kept), as
// the first frame's draw would: with the budget full at the room's entry it loaded nothing, and the first shown frame
// waited for the read itself (o2ds post_music_boss_slide f246, 69-73 ms). Passes 1-2 fill in only
// up to the budget less ROOMLOAD_SPRITE_HEADROOM_BYTES, like the sprite marks: on the Old 2DS fills
// up to the budget left
// 18.8-19.5 of 20.3 MB resident after the fireworks room (base 9.8 MB), later rooms evicted twice the tiles,
// and post_music_boss_slide's first shown frame waited 86 ms for a page-2 tile (165-180 ms against base 82 ms).
ROOMLOAD_TEX_LATE static void roomLoadPasses(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    bool full = false;
    for (int pass = 0; 3 > pass && !full; pass++) {
        for (uint32_t pageId = 0; cache->pageCount > pageId && !full; pageId++) {
            CtrPage* page = &cache->pages[pageId];
            if (!page->layoutKnown || page->failed || page->retryFrame > cache->frame) continue;
            if (pass == 0 && !page->wantedNow) continue;
            bool readDone = page->read != nullptr && cache->reader != nullptr && CtrTileReader_finished(cache->reader, page->read) && page->read->headerOk;
            if (pass == 0 && page->packed == nullptr && page->read == nullptr && page->cacheFile != CTR_CACHE_FILE_VALID) continue;
            if (pass == 1 && page->packed == nullptr && !readDone) continue;
            if (pass == 2 && (page->packed != nullptr || readDone || page->cacheFile != CTR_CACHE_FILE_VALID || page->read != nullptr)) continue;
            uint32_t wantedBytes = 0;
            uint32_t fileBytes = 0;
            uint32_t requested = 0;
            uint64_t requestedMask = 0;
            int32_t tiles = page->tilesX * page->tilesY;
            for (int32_t t = 0; tiles > t; t++) {
                CtrTile* tile = &page->tiles[t];
                bool missing = !tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY);
                if (!tile->wanted || !missing) continue;
                if (pass == 0 ? !tile->now : tile->now) continue;
                if (pass == 1 && page->packed == nullptr && page->read->tileAt[t] == CTR_READ_NOT_READ) continue;
                uint32_t bytes = tile->format == CTR_TILE_EMPTY ? 0
                    : (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
                uint64_t keep = pass == 0 ? 0 : ROOMLOAD_SPRITE_HEADROOM_BYTES;
                // Pass 0 makes room as the first frame's draw would (evict, least recently used first), but only from
                // tiles last used before the frame before this one: the old room's last frame and the tiles this room
                // load stamped (frame - 1) stay, as do the room's marked tiles (markTpag stamps them now). Item 7: nor
                // does any tile the room wants (the sprite marks don't stamp theirs), and tiles loaded ahead and never
                // drawn go first (rlAhead, file end).
                if (pass == 0 && cache->frame > 1 && (uint64_t) cache->residentBytes + wantedBytes + bytes > cache->budgetBytes)
                    rlEvictForFirstFrame(cache, wantedBytes + bytes, cache->frame - 1);
                if ((uint64_t) cache->residentBytes + wantedBytes + bytes + keep > cache->budgetBytes ||
                    linearSpaceFree() < LINEAR_FLOOR_BYTES + wantedBytes + bytes) {
                    full = true;
                    break;
                }
                wantedBytes += bytes;
                fileBytes += page->packInfo ? tile->size : ROOMLOAD_FILE_TILE_BYTES;
                tile->requested = true;
                if (ROOMLOAD_STAMP_TILES > t) requestedMask |= 1ull << t;
                requested++;
            }
            if (requested == 0) continue;
            uint64_t now = nowNanos();
            if (now >= (pass == 0 ? deadline + ROOMLOAD_NOW_EXTRA_NANOS : deadline)) {
                for (int32_t t = 0; tiles > t; t++) page->tiles[t].requested = false;
                return;
            }
            if (pass == 2 && now + ROOMLOAD_FILE_FIXED_NANOS + (uint64_t) fileBytes * ROOMLOAD_FILE_NANOS_PER_KB / 1024u > deadline) {
                // A smaller page may still fit.
                for (int32_t t = 0; tiles > t; t++) page->tiles[t].requested = false;
                continue;
            }
            // Pass 0 leaves a page image's decode to the first frame's draws: a read-ahead that finds no cache file (a
            // page not packed yet) would fall back to it, and each decode puts its page first in the background
            // worker's order. On the Old models, with the cache cold, that moved Chapter 3's big pages to where their
            // packing met a draw's 1.8 MB image (DR44 Old, ch3_knight: malloc failed at data_win.c, roomload12). The
            // wait is the one loadFromRead would make.
            if (pass == 0 && page->packed == nullptr && page->cacheFile != CTR_CACHE_FILE_VALID && page->read != nullptr) {
                if (cache->reader != nullptr) CtrTileReader_wait(cache->reader, page->read);
                if (!page->read->headerOk) {
                    for (int32_t t = 0; tiles > t; t++) page->tiles[t].requested = false;
                    continue;
                }
            }
            uint32_t before = page->residentTiles;
            // Passes 1-2 load what the base's filling in would, so as filling: no promotion request, whose whole-page
            // read would otherwise start frames earlier and hold the card while the room's first scripts read code.
            bool wasFilling = cache->filling;
            cache->filling = wasFilling || pass > 0;
            cache->busyPage = page;
            loadRequestedFrom(cache, pageId, page);
            cache->busyPage = nullptr;
            cache->filling = wasFilling;
            cache->frameLoadNanos += nowNanos() - now;
            uint32_t loaded = page->residentTiles > before ? page->residentTiles - before : 0;
            if (pass == 0) *nowTiles += loaded;
            else *otherTiles += loaded;
            for (int32_t t = 0; ROOMLOAD_STAMP_TILES > t && tiles > t && cache->frame > 0; t++) {
                CtrTile* tile = &page->tiles[t];
                if ((requestedMask >> t) & 1u && tile->valid && tile->lastUsedFrame == cache->frame) tile->lastUsedFrame = cache->frame - 1;
            }
        }
    }
}

// The room load's passes, then the cache files they opened are closed again: cacheFile keeps up to CTR_OPEN_FILES
// open as a shortcut, and a room load that reads remembered tiles from many pages left 2-3 more open than the base
// keeps. On the Old 3DS that ran the SD card's open allowance out when the next frame's read-ahead opened its page
// (FS OutOfResource): the read waited 250 ms. Files kept open before the room load stay.
// The font pages' step (item 3, below) runs last, in what is left of the budget.
ROOMLOAD_TEX_LATE static void roomLoadFonts(CtrTextureCache* cache, uint64_t deadline); __attribute__((noinline, section(".text.late.utflowey"))) static void rlfPasses(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
ROOMLOAD_TEX_LATE void CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    FILE* kept[CTR_OPEN_FILES];
    for (int32_t i = 0; CTR_OPEN_FILES > i; i++) kept[i] = cache->files[i];
    rlfPasses(cache, deadline, nowTiles, otherTiles);
    roomLoadFonts(cache, deadline);
    rlAheadTakeBack(cache);
    for (int32_t i = 0; CTR_OPEN_FILES > i; i++) {
        if (cache->files[i] == nullptr || cache->files[i] == kept[i]) continue;
        fclose(cache->files[i]);
        cache->files[i] = nullptr;
    }
}

// ===[ roomload item 3: font pages ]===
//
// A dialogue's first text draw loaded its font page from the SD card while the card was busy: Papyrus's first dialogue
// in the snowy bedroom (o3ds) drew in 407-465 ms, and in 6.5 ms in the one run that had the
// page in RAM already. Font pages come late in the background worker's order (data.win order), and on the Old models the
// worker reads only while the game thread waits. So each room load, in what is left of its budget:
//   (C) learns the font pages' pack info (one cache file header each, smallest image first), so a first draw only reads
//       its tiles;
//   (A) puts the font pages whose cache file is at most ROOMLOAD_FONT_SMALL_BYTES next in the worker's order while it is
//       still reading pages into RAM. Once it is done, such a page that isn't in RAM (dropped since) is read back whole in
//       the background: a promotion request, served by promotePending under makePromotionRoom's rules, never blocking.
// UNDERTALE's English fonts are on two such pages (50 and 93 KB); no DELTARUNE font page qualifies (392 KB and up).
// Fonts made at run time from sprites (font_add_sprite) are left out: their frames are sprite pages.
#define ROOMLOAD_FONT_PAGES_MAX 32
#define ROOMLOAD_FONT_SMALL_BYTES (256u * 1024u)

bool CtrPackWorker_putFirst(CtrPackWorker* worker, uint32_t pageId);

static struct {
    CtrPackKey key; // the data.win the list is for
    uint32_t pageCount;
    uint32_t count;
    uint16_t pages[ROOMLOAD_FONT_PAGES_MAX];
    uint32_t sizes[ROOMLOAD_FONT_PAGES_MAX]; // cache file bytes, 0 while not known
} rlFonts __attribute__((section(".bss.late.roomload")));

static const char rlFontPagesLog[] __attribute__((section(".rodata.roomload_in_text"))) =
    "Font pages: %u headers read in %.0f ms; %u of %u pages small (%u KB); %u put next in the worker's order, %u read back\n";

// The pages that hold the data.win's fonts' glyph sheets, once per data.win.
ROOMLOAD_TEX_LATE static void roomLoadFontList(const CtrTextureCache* cache) {
    if (rlFonts.pageCount == cache->pageCount && memcmp(&rlFonts.key, &cache->key, sizeof(CtrPackKey)) == 0) return;
    memset(&rlFonts, 0, sizeof(rlFonts));
    rlFonts.key = cache->key;
    rlFonts.pageCount = cache->pageCount;
    const DataWin* dw = cache->dataWin;
    for (uint32_t f = 0; dw->font.count > f && ROOMLOAD_FONT_PAGES_MAX > rlFonts.count; f++) {
        const Font* font = &dw->font.fonts[f];
        if (!font->present || font->isSpriteFont || 0 > font->tpagIndex || (uint32_t) font->tpagIndex >= dw->tpag.count) continue;
        int32_t pageId = dw->tpag.items[font->tpagIndex].texturePageId;
        if (0 > pageId || (uint32_t) pageId >= cache->pageCount) continue;
        bool listed = false;
        for (uint32_t i = 0; rlFonts.count > i && !listed; i++) listed = rlFonts.pages[i] == (uint16_t) pageId;
        if (!listed) rlFonts.pages[rlFonts.count++] = (uint16_t) pageId;
    }
}

ROOMLOAD_TEX_LATE static void roomLoadFonts(CtrTextureCache* cache, uint64_t deadline) {
    roomLoadFontList(cache);
    const DataWin* dw = cache->dataWin;
    // (C) Pack info and cache file size, smallest image first, while a file's fixed cost fits in the budget.
    uint64_t start = nowNanos();
    uint32_t headers = 0;
    uint32_t tried = 0;
    for (;;) {
        int32_t pick = -1;
        for (uint32_t i = 0; rlFonts.count > i; i++) {
            const CtrPage* page = &cache->pages[rlFonts.pages[i]];
            if (rlFonts.sizes[i] != 0 || ((tried >> i) & 1u) != 0 || page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING ||
                dw->txtr.textures[rlFonts.pages[i]].externalPath != nullptr) continue;
            if (0 > pick || dw->txtr.textures[rlFonts.pages[pick]].blobSize > dw->txtr.textures[rlFonts.pages[i]].blobSize) pick = (int32_t) i;
        }
        if (0 > pick) break;
        uint32_t pageId = rlFonts.pages[pick];
        CtrPage* page = &cache->pages[pageId];
        bool fromFile = page->packed == nullptr;
        if (fromFile && nowNanos() + ROOMLOAD_FILE_FIXED_NANOS > deadline) break;
        tried |= 1u << pick;
        if (!loadPackInfo(cache, pageId, page)) continue;
        uint32_t size = page->packed != nullptr ? page->packedSize : 0;
        if (size == 0) {
            // The file loadPackInfo opened (kept in cache->files; the room load closes it again).
            FILE* f = cacheFile(cache, pageId);
            long bytes = f != nullptr ? CtrPack_fileSize(f) : -1;
            size = bytes > 0 ? (uint32_t) bytes : 0;
        }
        rlFonts.sizes[pick] = size;
        if (fromFile) headers++;
    }
    double headerMs = (double) (nowNanos() - start) / 1e6;

    // (A) The small ones: next in the worker's order while it reads pages into RAM, else read back if not in RAM. Last
    // listed first, so the first listed ends up first.
    uint32_t done = 0, total = 0;
    if (cache->worker != nullptr) CtrPackWorker_progress(cache->worker, &done, &total);
    bool workerReading = cache->workerKeepsPacked && total > done;
    uint32_t small = 0, smallBytes = 0, first = 0, back = 0;
    for (int32_t i = (int32_t) rlFonts.count - 1; 0 <= i; i--) {
        uint32_t size = rlFonts.sizes[i];
        if (size == 0 || size > ROOMLOAD_FONT_SMALL_BYTES) continue;
        small++;
        smallBytes += size;
        uint32_t pageId = rlFonts.pages[i];
        CtrPage* page = &cache->pages[pageId];
        if (page->packed != nullptr || page->failed || page->cacheFile != CTR_CACHE_FILE_VALID) continue;
        if (workerReading) {
            if (CtrPackWorker_putFirst(cache->worker, pageId)) first++;
            continue;
        }
        if (cache->reader == nullptr || page->promotionPending || page->promotionRetryFrame > cache->frame ||
            (cache->wholeRead != nullptr && cache->wholePage == pageId)) continue;
        page->promotionBytes = size;
        page->promotionPending = true;
        back++;
    }
    if (headers > 0 || first > 0 || back > 0) {
        logInfo(rlFontPagesLog, (unsigned) headers, headerMs, (unsigned) small, (unsigned) rlFonts.count, (unsigned) (smallBytes / 1024),
                (unsigned) first, (unsigned) back);
    }
}

// roomload item 4: collectWholeRead for z_roomload.c's RoomLoad_collect, which texcacheCollectWholeRead calls in
// its place (once a frame, beginFrame unchanged).
void CtrTextures_collectWholeRead(CtrTextureCache* cache, bool drop) __attribute__((noinline, section(".text.late.roomload")));
void CtrTextures_collectWholeRead(CtrTextureCache* cache, bool drop) {
    collectWholeRead(cache, drop);
}

// ===[ roomload item 7: what pass 0 evicts, and the sprite marks' tiles ]===
//
// Rows rl11/rl13 (Old models) against base main:
// - o3ds core battle room (rl13-o3ds-core*B): the room load's sprite marks (sprites the room's code
//   assigns) wanted tiles of a page not in RAM, so the base's filling in read them from the SD card one a frame in the
//   room's first shown frames (7 tiles in 266-359 ms: frames of 56-86 ms where base shows 29-33 ms).
// - o2ds ch2_rooms (rl11-o2ds-dr*B): tiles the marks filled in counted as used when filled, so when the budget filled up
//   the tiles rooms had drawn went first (the post-boss slide room's creation read two page-26 tiles from the
//   SD card again, and it wanted 15 tiles against base's 10; its room load frame 516-524 ms against 314-337).
// So: (1) marked tiles still missing when the room load ends, on pages not in RAM, are left to their first draw (as on
// base); (2) a marked tile loaded ahead and not drawn in the two frames after counts as unused (last used frame 0,
// like the extras of a decoded page): it goes first when room must be made; (3) pass 0's eviction spares every tile the
// incoming room wants. RoomLoad_frame (z_roomload.c) calls CtrTextures_roomLoadIdle once a frame.
#define ROOMLOAD_AHEAD_MAX 256
#define RL_AHEAD_MISSING 0
#define RL_AHEAD_LOADED 1
#define RL_AHEAD_IDLE 2
#define RL_AHEAD_DONE 3

void CtrTextures_roomLoadIdle(CtrTextureCache* cache);

static struct {
    const CtrPage* pages; // the cache the list is for
    uint32_t pageCount;
    uint32_t count;
    uint32_t idled; // marked tiles set back to unused, in this cache
    uint16_t page[ROOMLOAD_AHEAD_MAX];
    uint16_t tile[ROOMLOAD_AHEAD_MAX];
    uint32_t seen[ROOMLOAD_AHEAD_MAX]; // the frame it was first seen resident
    uint32_t last[ROOMLOAD_AHEAD_MAX]; // its last used frame then
    uint8_t state[ROOMLOAD_AHEAD_MAX];
} rlAhead __attribute__((section(".bss.late.roomload")));

static const char rlAheadLog[] __attribute__((section(".rodata.roomload_in_text"))) =
    "Textures: %u marked tiles left to their first draw (pages not in RAM); %u loaded ahead and not drawn went to unused so far\n";

ROOMLOAD_TEX_LATE static void rlAheadStart(const CtrTextureCache* cache) {
    if (rlAhead.pages != cache->pages || rlAhead.pageCount != cache->pageCount) {
        memset(&rlAhead, 0, sizeof(rlAhead));
        rlAhead.pages = cache->pages;
        rlAhead.pageCount = cache->pageCount;
    }
    rlAhead.count = 0;
}

ROOMLOAD_TEX_LATE static void rlAheadNote(const CtrTextureCache* cache, uint32_t pageId, uint32_t tileIndex) {
    if (rlAhead.pages != cache->pages || rlAhead.count >= ROOMLOAD_AHEAD_MAX || pageId > UINT16_MAX || tileIndex > UINT16_MAX) return;
    uint32_t i = rlAhead.count++;
    rlAhead.page[i] = (uint16_t) pageId;
    rlAhead.tile[i] = (uint16_t) tileIndex;
    rlAhead.seen[i] = 0;
    rlAhead.last[i] = 0;
    rlAhead.state[i] = RL_AHEAD_MISSING;
}

ROOMLOAD_TEX_LATE static CtrTile* rlAheadTile(CtrTextureCache* cache, uint32_t i, CtrPage** outPage) {
    if (rlAhead.page[i] >= cache->pageCount) return nullptr;
    CtrPage* page = &cache->pages[rlAhead.page[i]];
    if (!page->layoutKnown || page->tiles == nullptr || (int32_t) rlAhead.tile[i] >= page->tilesX * page->tilesY) return nullptr;
    *outPage = page;
    return &page->tiles[rlAhead.tile[i]];
}

ROOMLOAD_TEX_LATE static void rlAheadTakeBack(CtrTextureCache* cache) {
    if (rlAhead.pages != cache->pages) return;
    uint32_t taken = 0;
    for (uint32_t i = 0; rlAhead.count > i; i++) {
        CtrPage* page = nullptr;
        CtrTile* tile = rlAheadTile(cache, i, &page);
        if (tile == nullptr) {
            rlAhead.state[i] = RL_AHEAD_DONE;
            continue;
        }
        if (tile->valid || !tile->wanted || page->packed != nullptr) continue;
        tile->wanted = false;
        rlAhead.state[i] = RL_AHEAD_DONE;
        taken++;
    }
    if (taken > 0) logInfo(rlAheadLog, (unsigned) taken, (unsigned) rlAhead.idled);
}

ROOMLOAD_TEX_LATE void CtrTextures_roomLoadIdle(CtrTextureCache* cache) {
    if (rlAhead.pages != cache->pages || rlAhead.pageCount != cache->pageCount) return;
    uint32_t frame = cache->frame;
    for (uint32_t i = 0; rlAhead.count > i; i++) {
        if (rlAhead.state[i] == RL_AHEAD_DONE) continue;
        CtrPage* page = nullptr;
        CtrTile* tile = rlAheadTile(cache, i, &page);
        if (tile == nullptr) {
            rlAhead.state[i] = RL_AHEAD_DONE;
            continue;
        }
        if (rlAhead.state[i] == RL_AHEAD_MISSING) {
            if (!tile->valid) continue;
            rlAhead.state[i] = RL_AHEAD_LOADED;
            rlAhead.seen[i] = frame;
            rlAhead.last[i] = tile->lastUsedFrame;
        } else if (rlAhead.state[i] == RL_AHEAD_LOADED) {
            if (!tile->valid) rlAhead.state[i] = RL_AHEAD_MISSING;
            else if (tile->lastUsedFrame > rlAhead.last[i]) rlAhead.state[i] = RL_AHEAD_DONE; // drawn (or marked again)
            else if (frame >= rlAhead.seen[i] + 2) {
                tile->lastUsedFrame = 0;
                rlAhead.state[i] = RL_AHEAD_IDLE;
                rlAhead.idled++;
            }
        } else if (!tile->valid || tile->lastUsedFrame != 0) {
            rlAhead.state[i] = RL_AHEAD_DONE;
        }
    }
}

// evictTile for a valid tile: its check, releaseTile's and dropMirror's bodies, repeated rather than called. A call from
// here made GCC stop inlining evictTile into evictUsedBefore once main had five other callers (evictUsedBefore 444 ->
// 320 B, and every function after it in ctr_textures.o moved; roomload12 merge of main c41a962a).
ROOMLOAD_TEX_LATE static void rlEvictTile(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    if (tile->lastUsedFrame >= cache->roomStartFrame && tile->lastUsedFrame + CTR_THRASH_FRAMES >= cache->frame) cache->roomThrashed = true;
    if (tile->mirror != nullptr) {
        CtrTileMirror* m = (CtrTileMirror*) tile->mirror;
        if (tile->lastUsedFrame != cache->frame) CtrGpu_cancelCopy(m->copy, tile->tex.data, m->tex.data);
        vramFree(m->tex.data);
        m->tex.data = tile->tex.data;
        m->tile = nullptr;
        m->dead = true;
        cache->mirrorBytes -= m->tex.size;
        tile->mirror = nullptr;
    }
    uint32_t bytes = tile->tex.size;
    C3D_TexDelete(&tile->tex);
    tile->valid = false;
    page->bytes -= bytes;
    page->residentTiles--;
    cache->residentBytes -= bytes;
    cache->evictionCount++;
}

// evictUsedBefore, sparing the tiles the room wants.
ROOMLOAD_TEX_LATE static void rlEvictForFirstFrame(CtrTextureCache* cache, uint32_t needed, uint32_t usedBefore) {
    if (usedBefore > cache->frame) usedBefore = cache->frame;
    while ((uint64_t) cache->residentBytes + needed > cache->budgetBytes) {
        bool found = false;
        uint32_t oldest = 0;
        for (uint32_t p = 0; cache->pageCount > p; p++) {
            const CtrPage* page = &cache->pages[p];
            if (page->residentTiles == 0 || page->tiles == nullptr) continue;
            int32_t tiles = page->tilesX * page->tilesY;
            for (int32_t i = 0; tiles > i; i++) {
                const CtrTile* tile = &page->tiles[i];
                if (!tile->valid || tile->wanted || tile->lastUsedFrame >= usedBefore) continue;
                if (!found || oldest > tile->lastUsedFrame) oldest = tile->lastUsedFrame;
                found = true;
            }
        }
        if (!found) return;
        for (uint32_t p = 0; cache->pageCount > p; p++) {
            CtrPage* page = &cache->pages[p];
            if (page->residentTiles == 0 || page->tiles == nullptr) continue;
            int32_t tiles = page->tilesX * page->tilesY;
            for (int32_t i = 0; tiles > i; i++) {
                CtrTile* tile = &page->tiles[i];
                if (!tile->valid || tile->wanted || tile->lastUsedFrame != oldest) continue;
                rlEvictTile(cache, page, tile);
                if ((uint64_t) cache->budgetBytes >= (uint64_t) cache->residentBytes + needed) return;
            }
        }
    }
}

// hammer601 (2026-10-22). A page whose cache file wasn't ready (a
// preparation skipped with B, or a cache folder whose "complete" marker
// outlived its page files) used to be decoded from its image at most once in
// 60 frames. A tile it lost in between (an extra the budget had no room for,
// or one evicted since) was then skipped by every draw for up to 60 frames:
// on the Old 3DS, DELTARUNE's Hammer of Justice drew frame 601 without
// Susie's slash, and Gerson's hair, Susie's hurt sprite and gradients went
// missing for whole seconds, while a warm cache drew them all. Pictures must
// not depend on how far the background worker has got, so a draw that needs
// a tile decodes its page again, costing a stall instead (docs/3ds-hardware.md,
// "A draw skipped its tile while the page waited to be decoded again").
//
// loadRequestedFrom's interval check went, which left it shorter. It keeps its
// slot, so that nothing after it moves: the padding is a later subsection of
// its own section, after its code and literal pool, outside its symbol size
// (as smartEmitSlot's in ctr_renderer.c). The check was 104 bytes on all
// four release targets (DELTARUNE and UNDERTALE, New and Old; nm -S).
#if defined(__3DS__) && defined(NDEBUG) && !defined(CTRPORT_RENDER_TIMING)
#define HAMMER601_TEX_SLOT_PAD 104
#define HAMMER601_STR2(x) #x
#define HAMMER601_STR(x) HAMMER601_STR2(x)
static void __attribute__((used, noinline, section(".text.late.hammer601"))) hammer601TexSlot(void) {
    __asm__(".pushsection .text.loadRequestedFrom, 1\n.space " HAMMER601_STR(HAMMER601_TEX_SLOT_PAD) "\n.popsection");
}
#endif
#include "ctr_textures_oomport_slots.inc"

#define UTFLOWEY_TEXT __attribute__((noinline, section(".text.late.utflowey")))
UTFLOWEY_TEXT static bool rlSwapFit(CtrTextureCache* cache, uint32_t needed, uint64_t keep);

// utflowey: the room load's passes 1-2 may swap tiles of older rooms out for the room's own.
//
// Passes 1-2 load the room's wanted tiles only up to the budget less ROOMLOAD_SPRITE_HEADROOM_BYTES, so that filling in
// never grows what is resident. With the budget already full of older rooms' tiles at the room
// change, that loaded nothing: the room's remembered tiles then waited for their first draw, in play, read from the SD
// card (Old 2DS, ut_rt_gdmy_t7h: back in a Waterfall room after a fight, "wants 1 tiles", the room load took 0,
// filling in stopped at the full budget, and frame 11017 drew for 48 ms around a 33 ms page-11 read). That first draw
// would evict the least recently used tiles, which are the older rooms': the swap evicts the same tiles a few frames
// earlier, in the room change, which is never shown. Rules:
//   - only tiles not used since before the room change (the old room's last frame kept, as pass 0) and not wanted;
//   - only when they free enough for the tile (nothing is evicted for a tile that would not load);
//   - only with the total already above the budget less the headroom (where that draw would evict), and never past
//     what is resident: it trades tiles, it doesn't add any;
//   - only in the first room load call of a frame: a caller that marks more tiles and calls again (utstall's
//     later plan tiles) keeps the headroom rule for them.
// Textures only: every tile is the same texels whenever it loads.
static struct {
    uint32_t frame;  // the frame of the last room load call, plus one
    uint32_t tiles;  // swapped out in this room load
    uint32_t bytes;
    uint32_t line;   // the total the swaps keep to, or 0 before the first
    bool allowed;
} rlSwap __attribute__((section(".bss.late.utflowey")));

static const char rlSwapLog[] __attribute__((aligned(4), section(".rodata.utflowey_in_text"))) =
    "Room load: %u tiles of older rooms (%u KB) swapped out for the room's own\n";

UTFLOWEY_TEXT static void rlSwapBegin(const CtrTextureCache* cache) {
    rlSwap.allowed = cache->frame > 1 && rlSwap.frame != cache->frame + 1u;
    rlSwap.frame = cache->frame + 1u;
    rlSwap.tiles = 0;
    rlSwap.bytes = 0;
    rlSwap.line = 0;
}

// Whether `needed` more bytes fit once older rooms' tiles make way; if so, they have (and the caller drops the headroom).
UTFLOWEY_TEXT static bool rlSwapFit(CtrTextureCache* cache, uint32_t needed, uint64_t keep) {
    if (!rlSwap.allowed || cache->frame < 2) return false;
    uint64_t resident = cache->residentBytes;
    uint64_t budget = cache->budgetBytes;
    // Below the headroom line a draw would load the tile without evicting anything: no swap there (the base's rule).
    // Once swapping, the total stays at what was resident at the first swap (`needed` counts the page's tiles already
    // made room for).
    if (rlSwap.line == 0 && (budget >= resident + keep || resident > budget)) return false;
    uint64_t line = rlSwap.line != 0 ? rlSwap.line : resident;
    uint64_t need = resident + needed > line ? resident + needed - line : 0;
    uint32_t usedBefore = cache->frame - 1;
    uint64_t stale = 0;
    for (uint32_t p = 0; cache->pageCount > p && need > stale; p++) {
        const CtrPage* page = &cache->pages[p];
        if (page->residentTiles == 0 || page->tiles == nullptr) continue;
        int32_t tiles = page->tilesX * page->tilesY;
        for (int32_t i = 0; tiles > i; i++) {
            const CtrTile* tile = &page->tiles[i];
            if (tile->valid && !tile->wanted && usedBefore > tile->lastUsedFrame) stale += tile->tex.size;
        }
    }
    if (need > stale) return false;
    rlSwap.line = (uint32_t) line;
    if (need == 0) return true;
    uint32_t evictions = cache->evictionCount;
    // rlEvictForFirstFrame evicts until resident + its `needed` fits the budget: resident - need, least recently used first.
    rlEvictForFirstFrame(cache, (uint32_t) (budget - (resident - need)), usedBefore);
    rlSwap.tiles += cache->evictionCount - evictions;
    rlSwap.bytes += (uint32_t) (resident - cache->residentBytes);
    return (uint64_t) cache->residentBytes + needed <= budget;
}

UTFLOWEY_TEXT static void rlSwapEnd(const CtrTextureCache* cache) {
    (void) cache;
    if (rlSwap.tiles > 0) logInfo(rlSwapLog, (unsigned) rlSwap.tiles, (unsigned) (rlSwap.bytes / 1024u));
}

// roomLoadPasses stays where it was, unused but kept by the pointer below, so nothing after it moves (a late
// move): this copy of it, with the swap, runs instead.
static void (*const rlfOldPasses)(CtrTextureCache*, uint64_t, uint32_t*, uint32_t*)
    __attribute__((used, aligned(4), section(".rodata.utflowey_in_text"))) = roomLoadPasses;
UTFLOWEY_TEXT static void rlfPassesBody(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    bool full = false;
    for (int pass = 0; 3 > pass && !full; pass++) {
        for (uint32_t pageId = 0; cache->pageCount > pageId && !full; pageId++) {
            CtrPage* page = &cache->pages[pageId];
            if (!page->layoutKnown || page->failed || page->retryFrame > cache->frame) continue;
            if (pass == 0 && !page->wantedNow) continue;
            bool readDone = page->read != nullptr && cache->reader != nullptr && CtrTileReader_finished(cache->reader, page->read) && page->read->headerOk;
            if (pass == 0 && page->packed == nullptr && page->read == nullptr && page->cacheFile != CTR_CACHE_FILE_VALID) continue;
            if (pass == 1 && page->packed == nullptr && !readDone) continue;
            if (pass == 2 && (page->packed != nullptr || readDone || page->cacheFile != CTR_CACHE_FILE_VALID || page->read != nullptr)) continue;
            uint32_t wantedBytes = 0;
            uint32_t fileBytes = 0;
            uint32_t requested = 0;
            uint64_t requestedMask = 0;
            int32_t tiles = page->tilesX * page->tilesY;
            for (int32_t t = 0; tiles > t; t++) {
                CtrTile* tile = &page->tiles[t];
                bool missing = !tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY);
                if (!tile->wanted || !missing) continue;
                if (pass == 0 ? !tile->now : tile->now) continue;
                if (pass == 1 && page->packed == nullptr && page->read->tileAt[t] == CTR_READ_NOT_READ) continue;
                uint32_t bytes = tile->format == CTR_TILE_EMPTY ? 0
                    : (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
                uint64_t keep = pass == 0 ? 0 : ROOMLOAD_SPRITE_HEADROOM_BYTES;
                // Pass 0 makes room as the first frame's draw would (evict, least recently used first), but only from
                // tiles last used before the frame before this one: the old room's last frame and the tiles this room
                // load stamped (frame - 1) stay, as do the room's marked tiles (markTpag stamps them now). Item 7: nor
                // does any tile the room wants (the sprite marks don't stamp theirs), and tiles loaded ahead and never
                // drawn go first (rlAhead, file end).
                if (pass == 0 && cache->frame > 1 && (uint64_t) cache->residentBytes + wantedBytes + bytes > cache->budgetBytes)
                    rlEvictForFirstFrame(cache, wantedBytes + bytes, cache->frame - 1);
                // The swap (rlSwapFit), the only change from roomLoadPasses.
                if (pass > 0 && (uint64_t) cache->residentBytes + wantedBytes + bytes + keep > cache->budgetBytes &&
                    rlSwapFit(cache, wantedBytes + bytes, keep)) keep = 0;
                if ((uint64_t) cache->residentBytes + wantedBytes + bytes + keep > cache->budgetBytes ||
                    linearSpaceFree() < LINEAR_FLOOR_BYTES + wantedBytes + bytes) {
                    full = true;
                    break;
                }
                wantedBytes += bytes;
                fileBytes += page->packInfo ? tile->size : ROOMLOAD_FILE_TILE_BYTES;
                tile->requested = true;
                if (ROOMLOAD_STAMP_TILES > t) requestedMask |= 1ull << t;
                requested++;
            }
            if (requested == 0) continue;
            uint64_t now = nowNanos();
            if (now >= (pass == 0 ? deadline + ROOMLOAD_NOW_EXTRA_NANOS : deadline)) {
                for (int32_t t = 0; tiles > t; t++) page->tiles[t].requested = false;
                return;
            }
            if (pass == 2 && now + ROOMLOAD_FILE_FIXED_NANOS + (uint64_t) fileBytes * ROOMLOAD_FILE_NANOS_PER_KB / 1024u > deadline) {
                // A smaller page may still fit.
                for (int32_t t = 0; tiles > t; t++) page->tiles[t].requested = false;
                continue;
            }
            // Pass 0 leaves a page image's decode to the first frame's draws: a read-ahead that finds no cache file (a
            // page not packed yet) would fall back to it, and each decode puts its page first in the background
            // worker's order. On the Old models, with the cache cold, that moved Chapter 3's big pages to where their
            // packing met a draw's 1.8 MB image (DR44 Old, ch3_knight: malloc failed at data_win.c, roomload12). The
            // wait is the one loadFromRead would make.
            if (pass == 0 && page->packed == nullptr && page->cacheFile != CTR_CACHE_FILE_VALID && page->read != nullptr) {
                if (cache->reader != nullptr) CtrTileReader_wait(cache->reader, page->read);
                if (!page->read->headerOk) {
                    for (int32_t t = 0; tiles > t; t++) page->tiles[t].requested = false;
                    continue;
                }
            }
            uint32_t before = page->residentTiles;
            // Passes 1-2 load what the base's filling in would, so as filling: no promotion request, whose whole-page
            // read would otherwise start frames earlier and hold the card while the room's first scripts read code.
            bool wasFilling = cache->filling;
            cache->filling = wasFilling || pass > 0;
            cache->busyPage = page;
            loadRequestedFrom(cache, pageId, page);
            cache->busyPage = nullptr;
            cache->filling = wasFilling;
            cache->frameLoadNanos += nowNanos() - now;
            uint32_t loaded = page->residentTiles > before ? page->residentTiles - before : 0;
            if (pass == 0) *nowTiles += loaded;
            else *otherTiles += loaded;
            for (int32_t t = 0; ROOMLOAD_STAMP_TILES > t && tiles > t && cache->frame > 0; t++) {
                CtrTile* tile = &page->tiles[t];
                if ((requestedMask >> t) & 1u && tile->valid && tile->lastUsedFrame == cache->frame) tile->lastUsedFrame = cache->frame - 1;
            }
        }
    }
}

UTFLOWEY_TEXT static void rlfPasses(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    rlSwapBegin(cache);
    rlfPassesBody(cache, deadline, nowTiles, otherTiles);
    rlSwapEnd(cache);
}
