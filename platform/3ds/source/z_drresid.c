// drresid: a fight's later tiles stream in ahead of their first draw (DELTARUNE, New 3DS). Late code, constants and
// statics in sections of their own, linked last (platform/3ds/late_reserve.ld): nothing that existed before moves.
// UNDERTALE and the Old models compile this file empty.
//
// The room plan and rooms.dtm cover a room's first frames, and the New DR whole-room sets (change hitches) the rooms
// whose every scene fits the texture budget together. The oversized rooms (Knight's snow zone with Tenna's episode
// and the sax fight, Titan, Flowery and Flowery's end room) don't fit whole, so their fights drew their attacks' art
// for the first time in play: each a tile load from the SD card (7-30 ms; Flowery 43 loads, 556 ms in the bench
// window; Knight 9-16 loads, up to 459 ms).
//
// The builder (tools/roomplan/build_tracks.py) ships romfs:/roomplan/tracks.dtp: per fight (a track), the tiles it
// draws after the room's first 45 frames, in first-use order, each with its first-use frame counted from the track's
// activation, and a few activation tiles: the fight's first tiles that no other track of the room draws. Indices only,
// from our recorder runs, used only for the data.win they were made on.
//   1. Activation. Each frame (CtrTextures_beginFrame is wrapped on the New model), until a track of the room is
//      active, its activation tiles are looked at: one drawn since the room change makes the track active. A room's
//      entry track (no activation tiles: what every recorded scene of the room draws before its own track starts) is
//      active from the room change until then. A track may have no tiles: once it starts, no other can (Tenna's
//      episode and the sax fight, so that Knight's starts with its intro, at tiles they draw only later).
//   2. Progress. The track's clock is the largest first-use frame among its tiles drawn since activation (game time, as
//      the game shows it: a fight's turns take as long as the player takes, so frames since activation would run ahead).
//      A fight can't be further on than the frames since activation, though: a tile whose recorded first use is more
//      than DRRESID_SLACK past them (an attack the recorded run had later, a tile two phases share) doesn't move it.
//   3. Admission. The tiles whose first use is at most DRRESID_AHEAD frames past the clock are marked wanted, a few at
//      a time, and the room's filling in (ctr_textures.c fillWanted: the tile reader reads them off the game thread,
//      and at most FILL_BUDGET_MS a frame installs them) is started again for them. A tile marked or upcoming is
//      stamped as used, so least-recently-used eviction takes other tiles first.
//   4. Room. A tile is marked only while it leaves DRRESID_LINEAR_KEEP of linear memory free and fits the budget less
//      DRRESID_HEADROOM. Short of budget, tiles nobody has drawn for DRRESID_RETIRE frames (and that aren't upcoming)
//      are evicted first, least recently used first; short of linear memory, or if evicting doesn't make room, nothing
//      more is marked until there is room, and the tile loads at its first draw, as before.
// Textures only: no game state, script or timing the game sees changes, and a tile is the same texels whenever it
// loads.
#include <stdbool.h>
#include <stdint.h>

#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_N3DS) && !defined(CTRPORT_ROOMPLAN_RECORDER)
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "log.h"
#include "runner.h"

#define DR_TEXT __attribute__((noinline, section(".text.drresid_late")))
#define DR_CONST __attribute__((aligned(4), section(".rodata.drresid_late")))
#define DR_BSS __attribute__((section(".bss.drresid_late")))

#define DRRESID_MAGIC 0x4B525444u // "DTRK"
#define DRRESID_VERSION 1u
#define DRRESID_MAX_SIZE (256u * 1024u)
#define DRRESID_TRACKS_MAX 8u          // tracks of one room
#define DRRESID_TILES_MAX 512u         // the builder writes at most this many a track
#define DRRESID_AHEAD 900u             // frames past the clock admitted
#define DRRESID_RETIRE 150u            // a tile not drawn for this long may make room
#define DRRESID_MARKS_PER_FRAME 8u
#define DRRESID_EVICTS_PER_FRAME 16u
#define DRRESID_HEADROOM (4u * 1024u * 1024u)
// Above the renderer's PARK_BELOW_FREE (8 MiB, ctr_renderer.c) and a batch of marks: marks never make it park surfaces.
// Below it parking is exact too (Azahar identity with cut 3's and cut 4b's textures in FCRAM), but 4 MiB bought Flowery
// nothing (its rooms run at 3-4 MiB free) and parked and unparked a tower layer mid-fight.
#define DRRESID_LINEAR_KEEP (10u * 1024u * 1024u)
#define DRRESID_SEED_FRAMES 45u
#define DRRESID_SCAN_FRAMES 4u         // the clock is brought up to date this often
#define DRRESID_SLACK 150u             // a drawn tile moves the clock only to at most this past the frames since activation
#define DRRESID_INSTALLED 0x80000000u  // markFrame: the tile is resident and its install stamp was seen

typedef struct {
    uint16_t room, actCount, tileCount, pad;
} DrTrack;

void __real_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame);
void __wrap_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame);

static DR_BSS struct {
    uint8_t* file;
    bool tried;
    const CtrTextureCache* cache;  // the room visit below is this cache's
    uint32_t keySize;              // and this data.win's
    uint32_t roomStart;
    const uint8_t* section;        // this data.win's section, or nullptr
    uint32_t sectionTracks;
    const DrTrack* tracks[DRRESID_TRACKS_MAX];
    uint32_t trackCount;
    const DrTrack* active;
    const uint32_t* entries;
    const uint16_t* frames;
    uint32_t count;
    uint32_t activeFrame;
    uint32_t clock;
    uint32_t next;                 // the first tile not looked at for admission
    uint32_t marked, evicted, blocked;
    uint32_t markFrame[DRRESID_TILES_MAX]; // when each tile was marked or stamped by us (0 never)
} dr;

static const char drPath[] DR_CONST = "romfs:/roomplan/tracks.dtp";
static const char drMode[] DR_CONST = "rb";
static const char drLoaded[] DR_CONST = "Tracks: %lu bytes, %lu sections\n";
static const char drActive[] DR_CONST = "Tracks: room %ld, %u tiles, active at %lu (room +%lu)\n";
static const char drSummary[] DR_CONST =
    "Tracks: room %ld left: %lu/%u looked at, %lu marked, %lu evicted, %lu blocked, clock %lu\n";

static DR_TEXT uint32_t drTrackSize(const DrTrack* t) {
    return 8u + 4u * t->actCount + 4u * t->tileCount + ((2u * t->tileCount + 3u) & ~3u);
}

// The builder wrote the file for this build's romfs (tools/roomplan/build_tracks.py): only its header is checked.
static DR_TEXT void drLoad(void) {
    dr.tried = true;
    FILE* f = fopen(drPath, drMode);
    if (f == nullptr) return;
    uint8_t* data = malloc(DRRESID_MAX_SIZE);
    size_t size = data != nullptr ? fread(data, 1, DRRESID_MAX_SIZE, f) : 0;
    fclose(f);
    uint32_t head[3] = {0, 0, 0};
    if (size >= 12 && DRRESID_MAX_SIZE > size) memcpy(head, data, 12);
    if (head[0] != DRRESID_MAGIC || head[1] != DRRESID_VERSION) {
        free(data);
        return;
    }
    dr.file = data;
    logInfo(drLoaded, (unsigned long) size, (unsigned long) head[2]);
}

// This cache's section: the texture pack key and room count it was made for.
static DR_TEXT void drFindSection(const CtrTextureCache* cache) {
    dr.section = nullptr;
    if (dr.file == nullptr) return;
    uint32_t sections;
    memcpy(&sections, dr.file + 8, 4);
    size_t at = 12;
    for (uint32_t s = 0; sections > s; s++) {
        uint32_t sec[6];
        memcpy(sec, dr.file + at, 24);
        uint64_t timestamp = (uint64_t) sec[2] | (uint64_t) sec[3] << 32;
        at += 24;
        if (sec[0] == cache->key.dataWinSize && sec[1] == cache->key.gameId && timestamp == cache->key.timestamp &&
            sec[4] == cache->roomCount) {
            dr.section = dr.file + at;
            dr.sectionTracks = sec[5];
            return;
        }
        for (uint32_t t = 0; sec[5] > t; t++) at += drTrackSize((const DrTrack*) (dr.file + at));
    }
}

static DR_TEXT CtrTile* drTile(CtrTextureCache* cache, uint32_t entry, CtrPage** outPage) {
    uint32_t pageId = entry >> 8;
    if (pageId >= cache->pageCount) return nullptr;
    CtrPage* page = &cache->pages[pageId];
    if (!page->layoutKnown) return nullptr;
    int32_t t = (int32_t) (entry & 0xFFu);
    if (t >= page->tilesX * page->tilesY) return nullptr;
    *outPage = page;
    return &page->tiles[t];
}

// The active track ends (its summary logged): the room changed, or the entry track gives way to a fight's.
static DR_TEXT void drLeave(void) {
    if (dr.active != nullptr) {
        logInfo(drSummary, (long) dr.active->room, (unsigned long) dr.next, (unsigned) dr.count, (unsigned long) dr.marked,
                (unsigned long) dr.evicted, (unsigned long) dr.blocked, (unsigned long) dr.clock);
    }
    dr.active = nullptr;
}

static DR_TEXT void drActivate(CtrTextureCache* cache, const DrTrack* track);

static DR_TEXT void drEnter(CtrTextureCache* cache) {
    drLeave();
    dr.trackCount = 0;
    if (cache != dr.cache || cache->key.dataWinSize != dr.keySize) drFindSection(cache);
    dr.cache = cache;
    dr.keySize = cache->key.dataWinSize;
    dr.roomStart = cache->roomStartFrame;
    if (dr.section == nullptr || cache->runner == nullptr) return;
    int32_t room = cache->runner->currentRoomIndex;
    const uint8_t* at = dr.section;
    for (uint32_t t = 0; dr.sectionTracks > t; t++) {
        const DrTrack* track = (const DrTrack*) at;
        if (track->room == room && DRRESID_TRACKS_MAX > dr.trackCount) {
            dr.tracks[dr.trackCount++] = track;
            if (track->actCount == 0) drActivate(cache, track); // the entry track, from the room change
        }
        at += drTrackSize(track);
    }
}

static DR_TEXT void drActivate(CtrTextureCache* cache, const DrTrack* track) {
    const uint8_t* base = (const uint8_t*) track + 8;
    drLeave(); // the entry track's, when a fight's starts
    dr.active = track;
    dr.entries = (const uint32_t*) (base + 4u * track->actCount);
    dr.frames = (const uint16_t*) (base + 4u * track->actCount + 4u * track->tileCount);
    dr.count = track->tileCount;
    dr.activeFrame = cache->frame;
    dr.clock = 0;
    dr.next = 0;
    dr.marked = dr.evicted = dr.blocked = 0;
    memset(dr.markFrame, 0, sizeof(dr.markFrame));
    logInfo(drActive, (long) track->room, (unsigned) track->tileCount, (unsigned long) cache->frame,
            (unsigned long) (cache->frame - dr.roomStart));
}

// 1: an activation tile drawn since the room change (the entry track has none).
static DR_TEXT void drLookForTrack(CtrTextureCache* cache) {
    if (cache->frame < dr.roomStart + DRRESID_SEED_FRAMES) return;
    for (uint32_t k = 0; dr.trackCount > k; k++) {
        const DrTrack* track = dr.tracks[k];
        const uint32_t* act = (const uint32_t*) ((const uint8_t*) track + 8);
        for (uint32_t i = 0; track->actCount > i; i++) {
            CtrPage* page;
            CtrTile* tile = drTile(cache, act[i], &page);
            if (tile != nullptr && tile->lastUsedFrame > dr.roomStart) {
                drActivate(cache, track);
                return;
            }
        }
    }
}

// A resident upcoming tile's stamp (sun5): its last use is moved up to the frame before this one, never back. The
// phase plan (z_phaseplan.c, ppDrawn: used in the frame before) has looked at this frame already when this runs, and the
// renderer's shortcuts (used in this frame: drawn already) look at this frame's draws, so neither reads it as a draw at
// the next frame; to the eviction ages (DRRESID_RETIRE, ALLOC_IDLE_FRAMES, PHASEPLAN_PIN_AGE, CTR_THRASH_FRAMES) it is
// one frame older than a stamp of this frame. A tile installed in this frame keeps its install stamp. Returns the stamp.
// A draw in a scan's frame now differs from the scan's stamp, so drScan sees it at the next scan, as any other draw.
static inline __attribute__((always_inline)) uint32_t drStamp(CtrTile* tile, uint32_t frame) {
    // A tile's last use is never past this frame (only UNDERTALE's z_utstall.c stamps ahead), so this is the max of the two.
    uint32_t stamp = tile->lastUsedFrame == frame ? frame : frame - 1u;
    tile->lastUsedFrame = stamp;
    return stamp;
}

// 2: the clock follows the tiles drawn; upcoming tiles marked or resident are stamped as used. Installing a tile
// stamps it too (ctr_textures.c), so a marked tile's first stamp after it became resident is taken as its install
// (DRRESID_INSTALLED, cleared when the tile is evicted, so that a tile filled in again doesn't read as drawn either),
// and only a later one as a draw. A draw that came first only delays the clock to the next.
static DR_TEXT void drScan(CtrTextureCache* cache) {
    uint32_t limit = cache->frame - dr.activeFrame + DRRESID_SLACK;
    for (uint32_t i = 0; dr.count > i; i++) {
        CtrPage* page;
        CtrTile* tile = drTile(cache, dr.entries[i], &page);
        uint32_t m = dr.markFrame[i];
        if (tile == nullptr || m == UINT32_MAX) continue;
        if (!tile->valid) {
            dr.markFrame[i] = m & ~DRRESID_INSTALLED; // evicted: its next install stamps it again
            continue;
        }
        if (tile->lastUsedFrame > dr.activeFrame && tile->lastUsedFrame != (m & ~DRRESID_INSTALLED)) {
            if (m == 0 || (m & DRRESID_INSTALLED) != 0) {
                if (limit >= dr.frames[i] && dr.frames[i] > dr.clock) dr.clock = dr.frames[i];
                dr.markFrame[i] = UINT32_MAX; // drawn: the cache's own recency rules it from now on
                continue;
            }
            m |= DRRESID_INSTALLED;
        }
        if (m != 0) dr.markFrame[i] = drStamp(tile, cache->frame) | (m & DRRESID_INSTALLED);
    }
    __asm__ volatile("nop"); // layout pad (sun5): drScan keeps its 236 B, so the late code after it doesn't move
}

// 4: room for bytes more in the budget, by evicting tiles not drawn for DRRESID_RETIRE frames. Short of linear memory,
// nothing is evicted: in Flowery's room (2.9 MB free at the least) evicting until 10 MiB were free took 570 tiles the
// fight drew again (238 loads against 47).
static DR_TEXT bool drRoomFor(CtrTextureCache* cache, uint32_t bytes) {
    for (uint32_t n = 0;; n++) {
        if (bytes + DRRESID_LINEAR_KEEP > linearSpaceFree()) return false;
        if (cache->budgetBytes >= cache->residentBytes + bytes + DRRESID_HEADROOM) return true;
        uint32_t before = cache->evictionCount;
        if (n >= DRRESID_EVICTS_PER_FRAME || !CtrTextures_evictOldestIdle(cache, DRRESID_RETIRE)) return false;
        dr.evicted += cache->evictionCount - before;
    }
}

// 3: marks the next upcoming tiles and starts filling in again.
static DR_TEXT void drAdmit(CtrTextureCache* cache) {
    if (cache->fillPending) return; // the last batch is still being filled in
    uint32_t marks = 0;
    while (dr.count > dr.next && DRRESID_MARKS_PER_FRAME > marks && dr.clock + DRRESID_AHEAD >= dr.frames[dr.next]) {
        uint32_t i = dr.next, entry = dr.entries[i];
        CtrPage* page; // drTile sets it when it finds the tile (sun5: entry and this save drStamp's 12 B below)
        CtrTile* tile = drTile(cache, entry, &page);
        if (tile == nullptr) {
            // Its page's layout isn't known yet: learning it may read the cache file's header, once a frame at most.
            if (marks != 0) break;
            CtrTextures_acquire(cache, entry >> 8);
            tile = drTile(cache, entry, &page);
            if (tile == nullptr) {
                dr.next++;
                continue;
            }
        }
        if (dr.markFrame[i] == UINT32_MAX || tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) {
            if (tile->valid && dr.markFrame[i] != UINT32_MAX) dr.markFrame[i] = drStamp(tile, cache->frame) | DRRESID_INSTALLED;
            dr.next++;
            continue;
        }
        uint32_t texel = page->formatsKnown || tile->formatRemembered ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u;
        uint32_t bytes = (uint32_t) page->tileW * (uint32_t) page->tileH * texel;
        if (!drRoomFor(cache, bytes)) {
            dr.blocked++;
            break;
        }
        tile->wanted = true;
        tile->lastUsedFrame = cache->frame;
        dr.markFrame[i] = cache->frame;
        page->fillRead = false;
        dr.marked++;
        dr.next++;
        marks++;
    }
    if (marks == 0) return;
    cache->fillPending = true;
    cache->fillPage = 0;
    cache->fillRevisit = UINT32_MAX;
    cache->fillFrom = cache->frame; // already past this frame's second look
    cache->fillTiles = 0;
    cache->fillMs = 0.0;
}

DR_TEXT void __wrap_CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame) {
    __real_CtrTextures_beginFrame(cache, frame);
    if (!dr.tried) drLoad();
    if (dr.file == nullptr || cache->pageCount == 0) return;
    if (cache != dr.cache || cache->roomStartFrame != dr.roomStart || cache->key.dataWinSize != dr.keySize) drEnter(cache);
    if (dr.active == nullptr || dr.active->actCount == 0) {
        if (dr.trackCount > 0) drLookForTrack(cache);
        if (dr.active == nullptr) return;
    }
    if ((cache->frame - dr.activeFrame) % DRRESID_SCAN_FRAMES == 0) drScan(cache);
    drAdmit(cache);
}

#ifdef CTRPORT_FLOWERHW_COLD
// ===[ flowerhw cold: the first room after a save or chapter load finishes its seed in the room load ]===
//
// The room change's hidden load (z_roomload.c, 200 ms after marking) stops at its budget before the room's own
// first-second tiles in every big DELTARUNE room (Titan climb, Knight's snow zone, flowerydash, Titan's battle room):
// "tiles N first, 0 more; ... (budget reached)", then 5-17 tiles, 0.2-0.6 s of SD loads in the first 45 frames of play
// (n3ds console logs; Azahar's instant SD never reaches the budget). On the first room after a front-end or game-over
// room (a save load, a chapter load, a retry after a game over: what maintainer's playpack tests start with) the player is
// already waiting through a load, so there the room load runs once more, with COLD_NANOS as its deadline: the same
// CtrTextures_roomLoadFinish (no ctr_textures.c change), whose passes 1-2 never evict and stop at the cache's linear
// floor (LINEAR_FLOOR_BYTES) and at the budget less its 4 MiB sprite headroom. The deadline is a budget, not a cap
// (c26rev R140 F1): it is checked between page operations, so the page started before it runs to its end (an SD page
// load or a wait on its read, 50-300 ms in n3ds logs); first-frame pages may still start up to ROOMLOAD_NOW_EXTRA_NANOS
// (200 ms) past it and those already in RAM up to ROOMLOAD_RAM_EXTRA_NANOS (150 ms) more; pass 2 starts a cache-file
// page only if its estimated cost ends before it. So at most about 1.35 s and one page operation more. All of it is
// inside the room change: this runs from the room-change path (main.c ends that frame, its present suppressed, before
// the prefetch), and the new room's first frame is drawn only after it returns: a longer transition, never a play frame. Only when linear memory has COLD_FLOOR
// free (z_phaseplan.c's PHASEPLAN_FLOOR and 1 MiB). The front-end rooms are each chapter's list in tracks.dtp (a track
// for room 0xFFFF with COLD_FRONT_END in its pad, room indices as its activation entries; tools/roomplan/build_tracks.py
// writes it for the exact data.win only; drresid's own reading never matches room 0xFFFF). Warm room changes are as
// before. review decision 2026-11-03 (L1-alt (b)). Only what loads when changes, never what is drawn.
// It wraps CtrRoomload_prefetchRoom (ctr_argui.c's call), which nothing else wraps on DELTARUNE New (flowcode, a probe
// build, does; CMakeLists.txt keeps the two apart). Its own sections, appended last (late_reserve.ld).
#define COLD_TEXT __attribute__((noinline, section(".text_flowerhw_cold")))
#define COLD_CONST __attribute__((aligned(4), section(".flowerhw_cold_ro")))
#define COLD_BSS __attribute__((section(".bss_flowerhw_cold,\"aw\",%nobits @"))) // nobits, as z_phaseplan.c's L2 state
#define COLD_NANOS (1000ull * 1000000ull)
#define COLD_FLOOR (3u * 1024u * 1024u + 256u * 1024u + 1024u * 1024u)

uint64_t nowNanos(void);
void __real_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner);
void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner);
void __real_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);

static COLD_BSS struct {
    const DataWin* dataWin; // whose room the last room load was for
    bool frontEnd;          // that room was a front-end or game-over room
} cold;

#define COLD_LIST_ROOM 0xFFFFu
#define COLD_FRONT_END 1u // DrTrack.pad: the track lists the section's front-end rooms
static const char coldLog[] COLD_CONST =
    "Room load (cold): %s after a front-end room: %u first and %u more tiles in %.0f ms more; %u wanted left (%u read "
    "ahead, %u in cache files, %u other); linear %u KB free\n";
static const char coldSkipLog[] COLD_CONST = "Room load (cold): %s after a front-end room: skipped, linear %u KB free\n";

// Whether room is in this cache's section's front-end list. Before drresid has read the file (the boot room's load) no
// room is: a game room is always reached through a later front-end room.
COLD_TEXT static bool coldFrontEnd(const CtrTextureCache* cache, int32_t room) {
    if (dr.file == nullptr) return false;
    uint32_t sections;
    memcpy(&sections, dr.file + 8, 4);
    size_t at = 12;
    for (uint32_t s = 0; sections > s; s++) {
        uint32_t sec[6];
        memcpy(sec, dr.file + at, 24);
        at += 24;
        bool mine = sec[0] == cache->key.dataWinSize && sec[1] == cache->key.gameId &&
                    ((uint64_t) sec[2] | (uint64_t) sec[3] << 32) == cache->key.timestamp && sec[4] == cache->roomCount;
        for (uint32_t t = 0; sec[5] > t; t++) {
            const DrTrack* track = (const DrTrack*) (dr.file + at);
            if (mine && track->room == COLD_LIST_ROOM && (track->pad & COLD_FRONT_END) != 0) {
                const uint32_t* list = (const uint32_t*) (dr.file + at + 8);
                for (uint32_t i = 0; track->actCount > i; i++) {
                    if (list[i] == (uint32_t) room) return true;
                }
            }
            at += drTrackSize(track);
        }
        if (mine) return false;
    }
    return false;
}

COLD_TEXT void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    __real_CtrRoomload_prefetchRoom(cache, runner);
    if (runner == nullptr || cache == nullptr || runner->currentRoom == nullptr) return;
    bool after = cold.dataWin == runner->dataWin && cold.frontEnd;
    cold.dataWin = runner->dataWin;
    cold.frontEnd = coldFrontEnd(cache, runner->currentRoomIndex);
    if (!after || cold.frontEnd || cache->pageCount == 0) return;
    uint32_t linear = linearSpaceFree();
    if (COLD_FLOOR > linear) {
        logInfo(coldSkipLog, runner->currentRoom->name, (unsigned) (linear / 1024u));
        return;
    }
    uint64_t start = nowNanos();
    uint32_t nowTiles = 0, otherTiles = 0;
    __real_CtrTextures_roomLoadFinish(cache, start + COLD_NANOS, &nowTiles, &otherTiles);
    uint64_t took = nowNanos() - start;
    // What the room still wants and lacks, by where it would come from (a console row's reading aid).
    uint32_t left = 0, ahead = 0, files = 0;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        const CtrPage* page = &cache->pages[p];
        if (!page->layoutKnown || page->tiles == nullptr) continue;
        int32_t tiles = page->tilesX * page->tilesY;
        for (int32_t t = 0; tiles > t; t++) {
            const CtrTile* tile = &page->tiles[t];
            if (!tile->wanted || tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
            left++;
            if (page->read != nullptr || page->packed != nullptr) ahead++;
            else if (page->cacheFile == CTR_CACHE_FILE_VALID) files++;
        }
    }
    logInfo(coldLog, runner->currentRoom->name, (unsigned) nowTiles, (unsigned) otherTiles, (double) took / 1e6,
            (unsigned) left, (unsigned) ahead, (unsigned) files, (unsigned) (left - ahead - files),
            (unsigned) (linearSpaceFree() / 1024u));
}

#endif // CTRPORT_FLOWERHW_COLD
#endif
