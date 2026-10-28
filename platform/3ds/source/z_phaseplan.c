// phaseplan (utflowey, option 1; DELTARUNE only): a long room's later tiles readied a phase ahead. Late code,
// constants and statics only (platform/3ds/late_reserve.ld, after utflpin): nothing here moves existing code or data.
//
// The room plan covers a room's first frames, so a long fight draws the rest of its art for the first time in play,
// one tile load on the game thread each (7-30 ms; DELTARUNE's Flowery, room 226, loads 43 tiles inside the ch5_flower
// window on the New 3DS). The builder (tools/roomplan/build_phases.py) ships romfs:/roomplan/phases.dtph: for each
// recorded room, its tiles split into phases at gaps of 60 frames or more without a new one, in first-use order.
//
// Once a frame (texcacheCollectWholeRead's call of RoomLoad_collect, before the frame's fill and loads), for the
// current room's table:
// - Phase k+1 has started when one of its tiles, resident at the last frame, was drawn in the last frame (or one not
//   readied here was loaded by a draw), or when a later phase's marker was. Its start frame is kept.
// - Phase k+1's resident tiles look at least PHASEPLAN_PIN_AGE frames old, no older: LRU eviction and the linear
//   floor's take other tiles before them, and evicting one still isn't a thrash (CTR_THRASH_FRAMES). Phase k's are
//   drawn, so LRU keeps them as it keeps any other (pinning them too held a long last phase's tiles all room and cost
//   Omega Flowey's working set its room: UNDERTALE Old, twice the loads).
// - Phase k+1's missing tiles are readied: marked `wanted`, so that the fill and the reader read and install them
//   in the background (ctr_textures.c, fillWanted; the page's fill read is allowed once more). Only what fits both the
//   tile budget and linear memory above PHASEPLAN_FLOOR. To make room, tiles not drawn since phase k-1 started (since
//   the room change, for phase 1) are evicted, oldest first; never one of phase k+1. What still doesn't fit
//   loads when drawn, as without the table.
// - The marks of earlier phases' tiles still missing are dropped: `wanted` otherwise holds all room.
// A room the table doesn't have, or a cache without a table, costs a compare a frame. The table is used only if its
// key is the cache's (the data.win's size, game id and timestamp, the tile size and grid rule, the room count) and its
// checksum and layout hold; the builder made it only from records of this exact data.win (SHA-256).
//
// Hook: __wrap_RoomLoad_collect, below (-Wl,--wrap=RoomLoad_collect, CMakeLists.txt). UNDERTALE doesn't build this
// file (ruling, 2026-10-25: about nothing gained there for 4.9 KB of late code); its wrap is utflpin's.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ENABLE_WAD17
#include <3ds.h>

#include "ctr_benchromfs.h"
#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "log.h"
#include "roomplan_format.h"
#include "runner.h"

#define PHASEPLAN_TEXT __attribute__((section(".text.late.phaseplan")))
#define PHASEPLAN_FN __attribute__((noinline, section(".text.late.phaseplan")))
#define PHASEPLAN_CONST __attribute__((aligned(4), section(".rodata.phaseplan_in_text")))
#define PHASEPLAN_BSS __attribute__((section(".bss.late.phaseplan")))

#define PHASEPLAN_MAGIC 0x48505444u // "DTPH"
#define PHASEPLAN_VERSION 1u
#define PHASEPLAN_PHASES 32u   // build_phases.py MAX_PHASES
#define PHASEPLAN_TILES 512u   // build_phases.py MAX_TILES
#define PHASEPLAN_MAX_SIZE (256u * 1024u)
#define PHASEPLAN_PIN_AGE (CTR_THRASH_FRAMES + 1u)
#define PHASEPLAN_FLOOR (3u * 1024u * 1024u + 256u * 1024u) // the cache's 3 MiB linear floor, and some
#define PHASEPLAN_WAIT 30u      // frames a phase waits for the room's own fill before readying the next anyway
#define PHASEPLAN_EVICTIONS 64u // eviction rounds (one frame's tiles each) at most per readying
#define PHASEPLAN_RETRY 15u     // frames after the fill stopped (at linear memory's floor) before readying again
#define PHASEPLAN_TRIES 8u      // readyings at most per phase

uint32_t RoomPlan_fnv1a(const uint8_t* data, size_t size); // z_roomplan.c

typedef struct { // build_phases.py HEADER (44 bytes)
    uint32_t magic;
    uint16_t version, rooms;
    uint32_t dataWinSize, gameId;
    uint64_t timestamp;
    uint32_t tileMax, gridVersion, roomCount, bodySize, checksum;
} __attribute__((packed)) PhasePlanHeader;

typedef struct {
    uint16_t first, last, count, zero;
    uint32_t marker;
} PhasePlanPhase;

static PHASEPLAN_BSS struct {
    CtrTextureCache* cache; // the cache below is for, told apart from a new one by its pages and key too
    const CtrPage* pages;
    uint32_t pageCount;
    uint32_t dataWinSize;
    bool looked;            // the table was looked for (once per cache)
    uint8_t* file;          // romfs:/roomplan/phases.dtph, while it holds this cache's table
    const uint8_t* body;    // that table's rooms
    uint32_t bodySize;
    uint32_t rooms;
    uint32_t roomStart;     // the cache's roomStartFrame when last seen
    int32_t room;
    uint32_t phaseCount;    // the current room's phases (0: nothing to do)
    const uint8_t* phase[PHASEPLAN_PHASES];
    uint32_t k;             // the current phase
    uint32_t start[PHASEPLAN_PHASES];
    uint32_t tries;         // phase k+1's readyings so far
    uint32_t retryAt;       // the frame the next may happen
    uint64_t nextValid[PHASEPLAN_TILES / 64]; // phase k+1's tiles resident at the last frame
    uint32_t markerValid;   // later phases' markers resident at the last frame
} pp;

static const char ppPath[] PHASEPLAN_CONST = "romfs:/roomplan/phases.dtph";
static const char ppRomfs[] PHASEPLAN_CONST = "romfs";
static const char ppRead[] PHASEPLAN_CONST = "rb";
static const char ppFoundLog[] PHASEPLAN_CONST = "Phase plan: %u rooms (%u bytes)\n";
static const char ppPhaseLog[] PHASEPLAN_CONST = "Phase plan: room %d phase %u/%u at frame %u (+%u)\n";
static const char ppReadyLog[] PHASEPLAN_CONST = "Phase plan: room %d phase %u: readying %u of %u tiles (%u KB), %u KB evicted, %u KB linear free\n";

// The tile of a table entry (page << 8 | tile), or nullptr if its page's layout isn't known.
static PHASEPLAN_FN CtrTile* ppTile(CtrTextureCache* cache, uint32_t entry, CtrPage** outPage) {
    uint32_t id = entry >> 8;
    int32_t t = (int32_t) (entry & 0xFFu);
    if (id >= cache->pageCount) return nullptr;
    CtrPage* page = &cache->pages[id];
    if (!page->layoutKnown || page->tiles == nullptr || t >= page->tilesX * page->tilesY) return nullptr;
    *outPage = page;
    return &page->tiles[t];
}

static PHASEPLAN_FN const uint32_t* ppTiles(uint32_t p, uint32_t* count) {
    PhasePlanPhase ph;
    memcpy(&ph, pp.phase[p], sizeof(ph));
    *count = PHASEPLAN_TILES > ph.count ? ph.count : PHASEPLAN_TILES;
    return (const uint32_t*) (const void*) (pp.phase[p] + sizeof(ph));
}

// Phase j's marker tile (nullptr if its page's layout isn't known).
static PHASEPLAN_FN CtrTile* ppMarker(CtrTextureCache* cache, uint32_t j) {
    PhasePlanPhase ph;
    CtrPage* page = nullptr;
    memcpy(&ph, pp.phase[j], sizeof(ph));
    return ppTile(cache, ph.marker, &page);
}

// What a readied tile may take: the tile budget's room, and linear memory's above PHASEPLAN_FLOOR.
static PHASEPLAN_FN uint32_t ppRoomFor(const CtrTextureCache* cache) {
    uint32_t budget = cache->budgetBytes > cache->residentBytes ? cache->budgetBytes - cache->residentBytes : 0;
    uint32_t linear = linearSpaceFree();
    linear = linear > PHASEPLAN_FLOOR ? linear - PHASEPLAN_FLOOR : 0;
    return budget > linear ? linear : budget;
}

// Walks a table's rooms: whether they hold together; room's phases (none for -1) go to pp.phase.
static PHASEPLAN_FN bool ppWalk(const uint8_t* body, uint32_t size, uint32_t rooms, uint32_t roomCount, int32_t room) {
    uint32_t at = 0;
    for (uint32_t r = 0; rooms > r; r++) {
        if (4u > size - at) return false;
        uint16_t head[2];
        memcpy(head, body + at, 4);
        at += 4;
        if (head[0] >= roomCount) return false;
        bool mine = head[0] == (uint32_t) room;
        for (uint32_t p = 0; head[1] > p; p++) {
            PhasePlanPhase ph;
            if (sizeof(ph) > size - at) return false;
            memcpy(&ph, body + at, sizeof(ph));
            if (mine && PHASEPLAN_PHASES > pp.phaseCount) pp.phase[pp.phaseCount++] = body + at;
            at += sizeof(ph);
            if (ph.count == 0 || (uint32_t) ph.count * 4u > size - at) return false;
            at += (uint32_t) ph.count * 4u;
        }
    }
    return at == size;
}

// Reads the table file and keeps it if it holds a table for this cache.
static PHASEPLAN_TEXT void ppLoad(CtrTextureCache* cache) {
    CtrRomfs_init(ppRomfs);
    FILE* f = fopen(ppPath, ppRead);
    if (f == nullptr) return; // no table in this build
    long size = 0;
    uint8_t* data = nullptr;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size > 0 && PHASEPLAN_MAX_SIZE >= (unsigned long) size && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc((size_t) size);
        if (data != nullptr && fread(data, 1, (size_t) size, f) != (size_t) size) {
            free(data);
            data = nullptr;
        }
    }
    fclose(f);
    if (data == nullptr) return;
    uint32_t at = 0;
    while (sizeof(PhasePlanHeader) <= (uint32_t) size - at) {
        PhasePlanHeader h;
        memcpy(&h, data + at, sizeof(h));
        at += sizeof(h);
        if (h.magic != PHASEPLAN_MAGIC || h.version != PHASEPLAN_VERSION || h.bodySize > (uint32_t) size - at) break;
        const uint8_t* body = data + at;
        at += h.bodySize;
        if (h.dataWinSize == cache->key.dataWinSize && h.gameId == cache->key.gameId && h.timestamp == cache->key.timestamp &&
            h.tileMax == ROOMPLAN_TILE_MAX && h.gridVersion == ROOMPLAN_GRID_VERSION && h.roomCount == cache->roomCount &&
            h.checksum == RoomPlan_fnv1a(body, h.bodySize) && ppWalk(body, h.bodySize, h.rooms, h.roomCount, -1)) {
            pp.file = data;
            pp.body = body;
            pp.bodySize = h.bodySize;
            pp.rooms = h.rooms;
            logInfo(ppFoundLog, (unsigned) h.rooms, (unsigned) h.bodySize);
            return;
        }
    }
    free(data);
}

// Whether the tile was drawn in the last frame: resident at the last frame (a fill or read load can't have stamped
// it since), or loaded since without being readied (only a draw loads a tile not wanted).
static PHASEPLAN_TEXT bool ppDrawn(const CtrTile* tile, bool wasValid, uint32_t frame) {
    return tile != nullptr && tile->valid && tile->lastUsedFrame + 1u == frame && (wasValid || !tile->wanted);
}

// Phase q's missing tiles (not resident, not reserved for a read under way, which owns its memory already, and not known
// to be fully transparent: ctr_textures.c, tileMissing) and their bytes (their format's, or 4 a texel while that isn't known).
// With room, only those that fit it, which are marked `wanted` (their bytes taken from it) and their pages' fill read
// allowed once more. *n counts the tiles.
static PHASEPLAN_FN uint32_t ppPass(CtrTextureCache* cache, uint32_t q, uint32_t* room, uint32_t* n, uint32_t* firstPage) {
    uint32_t count = 0, bytes = 0;
    const uint32_t* tiles = ppTiles(q, &count);
    *n = 0;
    for (uint32_t i = 0; count > i; i++) {
        CtrPage* page = nullptr;
        CtrTile* tile = ppTile(cache, tiles[i], &page);
        if (tile == nullptr || tile->valid || tile->reserved || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
        uint32_t b = (uint32_t) page->tileW * (uint32_t) page->tileH;
        b = !page->formatsKnown && !tile->formatRemembered ? b * 4u : b * CtrPack_texelBytes((CtrTileFormat) tile->format);
        if (room != nullptr) {
            if (b > *room) continue;
            *room -= b;
            tile->wanted = true;
            page->fillRead = false;
            uint32_t id = (uint32_t) (page - cache->pages);
            if (*firstPage > id) *firstPage = id;
        }
        bytes += b;
        ++*n;
    }
    return bytes;
}

// Readies phase q's missing tiles (see the top).
// Returns how many were missing.
static PHASEPLAN_TEXT uint32_t ppReady(CtrTextureCache* cache, uint32_t q) {
    uint32_t frame = cache->frame;
    uint32_t missing = 0, marked = 0, firstPage = UINT32_MAX;
    uint32_t need = ppPass(cache, q, nullptr, &missing, nullptr);
    uint32_t room = ppRoomFor(cache);
    uint32_t cutoff = q >= 2 ? pp.start[q - 2] : pp.roomStart;
    uint32_t evicted = 0;
    for (uint32_t n = 0; need > room && PHASEPLAN_EVICTIONS > n && frame > cutoff + PHASEPLAN_PIN_AGE; n++) {
        uint32_t before = cache->residentBytes;
        if (!CtrTextures_evictOldestIdle(cache, frame - cutoff)) break;
        evicted += before - cache->residentBytes;
        room = ppRoomFor(cache);
    }
    uint32_t bytes = ppPass(cache, q, &room, &marked, &firstPage);
    if (marked > 0) { // as collectPacked arms the fill (ctr_textures.c)
        if (!cache->fillPending || cache->fillPage > firstPage) cache->fillPage = firstPage;
        cache->fillPending = true;
    }
    logInfo(ppReadyLog, (int) pp.room, (unsigned) q, (unsigned) marked, (unsigned) missing, (unsigned) (bytes / 1024u),
            (unsigned) (evicted / 1024u), (unsigned) (linearSpaceFree() / 1024u));
    return missing;
}

void PhasePlan_frame(CtrTextureCache* cache);
PHASEPLAN_FN void PhasePlan_frame(CtrTextureCache* cache) {
    if (cache == nullptr) return;
    if (pp.cache != cache || pp.pages != cache->pages || pp.pageCount != cache->pageCount || pp.dataWinSize != cache->key.dataWinSize) {
        free(pp.file);
        memset(&pp, 0, sizeof(pp));
        pp.cache = cache;
        pp.pages = cache->pages;
        pp.pageCount = cache->pageCount;
        pp.dataWinSize = cache->key.dataWinSize;
        pp.roomStart = UINT32_MAX;
    }
    uint32_t frame = cache->frame;
    if (cache->roomStartFrame != pp.roomStart) {
        pp.roomStart = cache->roomStartFrame;
        pp.phaseCount = 0;
        pp.k = 0;
        pp.tries = 0;
        pp.retryAt = 0;
        pp.markerValid = 0;
        memset(pp.nextValid, 0, sizeof(pp.nextValid));
        if (!pp.looked && cache->pageCount > 0) {
            pp.looked = true;
            ppLoad(cache);
        }
        if (pp.body != nullptr && cache->runner != nullptr) {
            pp.room = cache->runner->currentRoomIndex;
            pp.start[0] = pp.roomStart;
            ppWalk(pp.body, pp.bodySize, pp.rooms, UINT32_MAX, pp.room);
        }
    }
    if (pp.phaseCount == 0 || pp.roomStart >= frame) return;

    // Which phase: a tile of the next drawn, or a later phase's marker (whose residency is noted for the next frame).
    uint32_t k = pp.k;
    uint32_t next = k;
    uint32_t count = 0;
    const uint32_t* tiles = nullptr;
    if (pp.phaseCount > k + 1) {
        tiles = ppTiles(k + 1, &count);
        for (uint32_t i = 0; count > i; i++) {
            CtrPage* page = nullptr;
            if (ppDrawn(ppTile(cache, tiles[i], &page), (pp.nextValid[i >> 6] >> (i & 63) & 1u) != 0, frame)) {
                next = k + 1;
                break;
            }
        }
    }
    uint32_t markers = 0;
    for (uint32_t j = k + 2; pp.phaseCount > j; j++) {
        CtrTile* tile = ppMarker(cache, j);
        if (ppDrawn(tile, (pp.markerValid >> j & 1u) != 0, frame)) next = j;
        if (tile != nullptr && tile->valid) markers |= 1u << j;
    }
    if (next != k) {
        // The earlier phases' marks still missing go: `wanted` otherwise holds all room.
        for (uint32_t p = 0; next > p; p++) {
            tiles = ppTiles(p, &count);
            for (uint32_t i = 0; count > i; i++) {
                CtrPage* page = nullptr;
                CtrTile* tile = ppTile(cache, tiles[i], &page);
                if (tile != nullptr && !tile->valid && !tile->reserved) tile->wanted = false;
            }
        }
        k = pp.k = next;
        pp.start[k] = frame - 1;
        pp.tries = 0;
        pp.retryAt = 0;
        logInfo(ppPhaseLog, (int) pp.room, (unsigned) k, (unsigned) pp.phaseCount, (unsigned) (frame - 1),
                (unsigned) (frame - 1 - pp.roomStart));
    }
    pp.markerValid = markers; // the bits below k+2 are never read

    // Phase k+1 looks no older than PHASEPLAN_PIN_AGE frames; note which of its tiles are resident.
    memset(pp.nextValid, 0, sizeof(pp.nextValid));
    if (pp.phaseCount > k + 1) {
        tiles = ppTiles(k + 1, &count);
        for (uint32_t i = 0; count > i; i++) {
            CtrPage* page = nullptr;
            CtrTile* tile = ppTile(cache, tiles[i], &page);
            if (tile == nullptr || !tile->valid) continue;
            if (frame > PHASEPLAN_PIN_AGE && frame - PHASEPLAN_PIN_AGE > tile->lastUsedFrame) tile->lastUsedFrame = frame - PHASEPLAN_PIN_AGE;
            pp.nextValid[i >> 6] |= 1ull << (i & 63);
        }
    }

    // Ready the next phase once the room's own fill is done (or has had PHASEPLAN_WAIT frames). If the fill stopped
    // with some still missing (linear memory's floor), again PHASEPLAN_RETRY frames later, PHASEPLAN_TRIES at most.
    if (pp.phaseCount > k + 1 && PHASEPLAN_TRIES > pp.tries && frame >= pp.retryAt &&
        (!cache->fillPending || (pp.tries == 0 && frame >= pp.start[k] + PHASEPLAN_WAIT))) {
        pp.tries++;
        pp.retryAt = frame + PHASEPLAN_RETRY;
        if (ppReady(cache, k + 1) == 0) pp.tries = PHASEPLAN_TRIES;
    }
}

// The once-a-frame hook.
void __real_RoomLoad_collect(CtrTextureCache* cache, bool drop);
void __wrap_RoomLoad_collect(CtrTextureCache* cache, bool drop);
PHASEPLAN_FN void __wrap_RoomLoad_collect(CtrTextureCache* cache, bool drop) {
    __real_RoomLoad_collect(cache, drop);
    PhasePlan_frame(cache);
}
#endif
