// flowstream (UNDERTALE Old): plan tiles that leave in play come back in the background.
//
// keepLinearFloor (ctr_textures.c) keeps 3 MiB of linear memory free. When sound effects alone can't cover it, it
// evicts the least recently used tiles. In a scene whose whole plan set was loaded at the room change (Omega Flowey:
// 139 tiles, 26.0 of 26.0 MB), tiles loaded at the room change and not drawn yet look the oldest, so they go first,
// and each comes back from the SD card on the main thread at its first draw (20-33 ms a tile on the Old models, up to
// 137 ms with an open; a held frame each).
//
// fs5 (lead brief 2026-10-29: the same cut in under 2,500 B of late text). The floor's eviction order is unchanged.
// Once a frame, from RoomLoad_collect's call in utflPkCollect (z_utstall.c, just before CtrTextures_beginFrame's
// fillWanted), the plan scene this room visit loaded (CtrUtstall_activeScene) is walked: each of its tiles seen
// resident is noted, and a noted tile that is gone again (an eviction) gets a reader read of its own, every
// FS_REFILL_FRAMES frames, one at a time, while linear memory leaves FS_REFILL_SPARE above the floor after it and the
// texture budget has room. Once the read holds its bytes (where the page's pack info gives), the tile is installed
// from them right there (CtrTextures_flowstreamInstall: installTile, then the read released), still with the spare
// left; nothing else loads with it, and no fill is armed (rev 2, c26rev R32 F2: a fill's secondLook and its other
// sources could reach the SD card). A read that finishes without them (the reader's heap reserve, a bad header) is
// dropped: the tile loads at its draw as before. A tile comes back at most FS_REFILL_MAX times a room visit. Textures
// only: the same texels whenever a tile loads; no game state, no RNG.
//
// The refill read (lead ruling 2026-10-29) is admitted with the reader's promotion reserve (4 MiB on UT Old) plus its
// bytes instead of 8 MiB, its buffer allocated with heap relief off (a failure drops the read, never spills into linear
// memory), and never performed on the game thread: a wait there that takes it gets no file for it, before any open or
// read (rev 2, R32 F1), so perform reads nothing and the read is dropped. ctr_tile_reader.c calls the hooks at the end
// (CTR_READ_* and its page file calls). Everything else compiles this file empty. The logs are the lab diag build's
// only (CTRPORT_FLOWSTREAM_DIAG).
#include <3ds.h>
#include <malloc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ENABLE_WAD16) && defined(CTRPORT_FLOWSTREAM_READ)
#include "ctr_textures.h"
#include "ctr_tile_reader.h"
#include "log.h"
#include "roomplan_format.h"

#define FS_TEXT __attribute__((noinline, section(".text.flowstream_late")))
#define FS_CONST __attribute__((aligned(4), section(".rodata.flowstream_in_text")))
#define FS_BSS __attribute__((section(".bss.flowstream_late")))

#define FS_FLOOR (3u * 1024u * 1024u)  // ctr_textures.c's LINEAR_FLOOR_BYTES
#define FS_REFILL_SPARE (256u * 1024u) // above the floor, after the tile a refill takes
#define FS_REFILL_FRAMES 8u
#define FS_REFILL_MAX 4u
#define FS_TILES 256u                          // plan scene entries followed (Omega Flowey's has 140)
#define FS_SEEN 0x80u                          // fs.seen: resident at a look in this room visit; low bits: refills
#define FS_READER_RESERVE (8u * 1024u * 1024u) // ctr_tile_reader.c's READER_HEAP_RESERVE
#define FS_KEY_MASK 0x07FFFFFFu                 // plan entries: page << 8 | tile (ctr_textures.c's room tile key mask)

const uint8_t* CtrUtstall_activeScene(const CtrTextureCache* cache); // z_utstall.c: the plan scene this room visit loaded
void __real_RoomLoad_collect(CtrTextureCache* cache, bool drop);
void CtrFlowstream_collect(CtrTextureCache* cache, bool drop);
bool CtrMemory_allowRelief(bool allow);
bool CtrTextures_flowstreamInstall(CtrTextureCache* cache, CtrPage* page, int32_t t); // ctr_textures.c's end
FILE* CtrTextures_readerGameFile(uint32_t pageId);

static FS_BSS struct {
    const CtrTextureCache* cache; // what the rest is for: the cache, its room start and the scene
    uint32_t roomStart;
    const uint8_t* scene;
    uint32_t nextFrame; // the next frame a refill read may start
    // The refill read (one at a time), set before it is queued: the reader's hooks know it by it, its page and its
    // tile. performing: that read, while the reader thread is between its header and its allocation.
    CtrTileRead* volatile read;
    uint32_t page;
    uint32_t tile;
    CtrTileRead* volatile performing;
    uint8_t seen[FS_TILES];
} fs;

#ifdef CTRPORT_FLOWSTREAM_DIAG
// Frame, what (0 read queued, 1 installed (n: 1) or not (0), 3 dropped: read without it), page:tile, n; linear free.
static const char fsRefillLog[] FS_CONST = "Textures (flowstream): f%u refill %u %u:%u #%u; free %u KB\n";
static const char fsReadLog[] FS_CONST = "Textures (flowstream): refill read %u:%u on the reader, %u B, heap %u KB free\n";
#define FS_LOG(cache, what, n) \
    logInfo(fsRefillLog, (unsigned) (cache)->frame, (unsigned) (what), (unsigned) fs.page, (unsigned) fs.tile, (unsigned) (n), (unsigned) (linearSpaceFree() / 1024u))
#else
#define FS_LOG(cache, what, n) ((void) 0)
#endif

extern u32 __ctru_heap_size;

// ctr_tile_reader.c's heapFree, with the refill's reserve: whether a read of `bytes` would carry them.
static FS_TEXT bool fsReadFits(uint32_t bytes) {
    struct mallinfo mi = mallinfo();
    uint32_t used = (uint32_t) mi.uordblks;
    uint32_t heapFree = __ctru_heap_size > used ? __ctru_heap_size - used : 0;
    return heapFree >= CtrTileReader_heapReserve() + bytes;
}

// Linear memory left after `bytes` more, with the margin.
static FS_TEXT bool fsRoomFor(uint32_t bytes) {
    uint32_t freeBytes = linearSpaceFree();
    return freeBytes >= FS_FLOOR + FS_REFILL_SPARE && freeBytes - FS_FLOOR - FS_REFILL_SPARE >= bytes;
}

// The tile is in its page's read, where the page's pack info says (loadFromRead installs it, nothing else).
static FS_TEXT bool fsReadHas(const CtrPage* page, const CtrTileRead* read, uint32_t t) {
    const CtrTile* tile = &page->tiles[t];
    return read->headerOk && read->data != nullptr && read->tileAt[t] != CTR_READ_NOT_READ && page->packInfo &&
           tile->offset == read->view.tileOffset[t] && tile->size == read->view.tileSize[t];
}

FS_TEXT void CtrFlowstream_collect(CtrTextureCache* cache, bool drop) {
    __real_RoomLoad_collect(cache, drop);
    if (cache == nullptr) return;
    const uint8_t* scene = CtrUtstall_activeScene(cache);
    if (fs.cache != cache || fs.roomStart != cache->roomStartFrame || fs.scene != scene) {
        memset(&fs, 0, sizeof(fs)); // a read still attached to its page is the cache's to release with the room
        fs.cache = cache;
        fs.roomStart = cache->roomStartFrame;
        fs.scene = scene;
    }
    if (scene == nullptr) return;
    uint32_t frame = cache->frame;
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    uint32_t count = s.tileCount < FS_TILES ? s.tileCount : FS_TILES;
    bool start = fs.read == nullptr && !cache->fillPending && frame >= fs.nextFrame && cache->reader != nullptr;
    if (start) fs.nextFrame = frame + FS_REFILL_FRAMES;
    for (uint32_t i = 0; count > i; i++) {
        uint32_t entry;
        memcpy(&entry, scene + sizeof(s) + (size_t) i * 4u, 4);
        uint32_t p = (entry & FS_KEY_MASK) >> 8;
        uint32_t t = entry & 0xFFu;
        if (p >= cache->pageCount) continue;
        CtrPage* page = &cache->pages[p];
        if (page->tiles == nullptr || t >= (uint32_t) (page->tilesX * page->tilesY) || t >= CTR_PAGE_MAX_TILES) continue;
        CtrTile* tile = &page->tiles[t];
        if (tile->valid) {
            fs.seen[i] |= FS_SEEN;
            continue;
        }
        if (!(fs.seen[i] & FS_SEEN)) continue;
        // Gone since it was seen (evicted): only a refill marks it wanted again, so that no fill brings it back from
        // the SD card on the main thread.
        tile->wanted = false;
        if (!start || (fs.seen[i] & ~FS_SEEN) >= FS_REFILL_MAX) continue;
        if (!page->layoutKnown || !page->formatsKnown || !page->packInfo || page->failed || frame < page->retryFrame || page->packed != nullptr ||
            page->read != nullptr || page->cacheFile != CTR_CACHE_FILE_VALID || cache->dataWin->txtr.textures[p].externalPath != nullptr)
            continue;
        uint32_t bytes = (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
        if (!fsRoomFor(bytes) || cache->residentBytes + bytes > cache->budgetBytes || !fsReadFits(tile->size)) continue;
        CtrTileRead* read = CtrTileReader_newRead();
        if (read == nullptr) break;
        const Texture* txtr = &cache->dataWin->txtr.textures[p];
        read->want[t] = true;
        read->pageId = p;
        read->blobOffset = txtr->blobOffset;
        read->blobSize = txtr->blobSize;
        page->read = read;
        fs.page = p;
        fs.tile = t;
        fs.read = read; // before the queue's lock hands it to the reader
        CtrTileReader_queue(cache->reader, read);
        fs.seen[i]++;
        FS_LOG(cache, 0, fs.seen[i] & ~FS_SEEN);
        start = false;
    }
    CtrTileRead* read = fs.read;
    if (read == nullptr) return;
    CtrPage* page = &cache->pages[fs.page];
    CtrTile* tile = &page->tiles[fs.tile];
    if (page->read != read || tile->valid) { // the cache took it (drawn, or released): as before
        fs.read = nullptr;
        return;
    }
    if (!CtrTileReader_finished(cache->reader, read)) return;
    if (!fsReadHas(page, read, fs.tile)) {
        // No bytes for it (the reader's heap reserve, a bad header): dropped; it loads when drawn, as before.
        CtrTileReader_release(cache->reader, read);
        page->read = nullptr;
        fs.read = nullptr;
        FS_LOG(cache, 3, 0);
        return;
    }
    // Installed from the read now, with the spare still left after it (the same test as at the read's start); else
    // the read waits for memory, held by its page (a draw meanwhile takes the tile from it, as from any read).
    uint32_t bytes = (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
    if (!fsRoomFor(bytes) || cache->residentBytes + bytes > cache->budgetBytes) return;
    fs.read = nullptr;
    tile->wanted = true;
    if (!CtrTextures_flowstreamInstall(cache, page, (int32_t) fs.tile)) tile->wanted = false;
    FS_LOG(cache, 1, tile->valid);
}

// The game thread's wait for a read performs the next queued one meanwhile: the refill read is the page's read (one
// read a page at a time, and only the page's read is queued for tiles), so a page match while it is in flight is it.
// It gets no file: perform reads nothing, and the refill is dropped.
static FS_TEXT bool fsStolen(uint32_t pageId) {
    return threadGetCurrent() == nullptr && fs.read != nullptr && pageId == fs.page && fs.cache->pages[pageId].read == fs.read;
}

// performNextOnGameFile's CtrTextures_readerGameFile (the game thread only).
FILE* CtrFlowstream_gameFile(uint32_t pageId);
FS_TEXT FILE* CtrFlowstream_gameFile(uint32_t pageId) {
    return fsStolen(pageId) ? nullptr : CtrTextures_readerGameFile(pageId);
}

// CtrPack_openFile in ctr_tile_reader.c: the original wait's performNext (game thread), and readerFile (the reader).
FILE* CtrFlowstream_openFile(uint32_t pageId, bool* busy);
FS_TEXT FILE* CtrFlowstream_openFile(uint32_t pageId, bool* busy) {
    return fsStolen(pageId) ? nullptr : CtrPack_openFile(pageId, busy);
}

// ctr_tile_reader.c's perform, through CTR_READ_HEADER: its header parse. On the reader thread, whether the read is
// the refill read in flight (the game thread never performs it: fsStolen).
bool CtrFlowstream_readHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize,
                              CtrPackView* out);
FS_TEXT bool CtrFlowstream_readHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize,
                                      CtrPackView* out) {
    CtrTileRead* read = (CtrTileRead*) (void*) ((uint8_t*) out - offsetof(CtrTileRead, view));
    if (threadGetCurrent() != nullptr) fs.performing = read == fs.read && read->pageId == fs.page && read->want[fs.tile] ? read : nullptr;
    return CtrPack_parseHeader(data, size, key, blobOffset, blobSize, out);
}

// heapFree's mallinfo, through CTR_READ_MALLINFO: for the refill read on the reader thread, the heap in use counts the
// difference between the reader's 8 MiB and the promotion reserve less, so perform's test is the refill's.
struct mallinfo CtrFlowstream_readMallinfo(void);
FS_TEXT struct mallinfo CtrFlowstream_readMallinfo(void) {
    struct mallinfo mi = mallinfo();
    if (fs.performing != nullptr && threadGetCurrent() != nullptr) {
        uint32_t lift = FS_READER_RESERVE - CtrTileReader_heapReserve();
        mi.uordblks = (uint32_t) mi.uordblks > lift ? (int) ((uint32_t) mi.uordblks - lift) : 0;
    }
    return mi;
}

// perform's malloc, through CTR_READ_MALLOC: the refill read's buffer with heap relief off, so a heap that can't
// serve it drops the read instead of putting it in linear memory.
void* CtrFlowstream_readMalloc(size_t size);
FS_TEXT void* CtrFlowstream_readMalloc(size_t size) {
    if (fs.performing == nullptr || threadGetCurrent() == nullptr) return malloc(size);
    fs.performing = nullptr;
    bool allowed = CtrMemory_allowRelief(false);
    void* data = malloc(size);
    CtrMemory_allowRelief(allowed);
#ifdef CTRPORT_FLOWSTREAM_DIAG
    logInfo(fsReadLog, (unsigned) fs.page, (unsigned) fs.tile, (unsigned) (data != nullptr ? size : 0), (unsigned) (mallinfo().fordblks / 1024));
#endif
    return data;
}
#endif
