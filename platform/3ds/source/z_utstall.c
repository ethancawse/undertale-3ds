// utstall: more of a room's known texture loads move into the hidden room load (UNDERTALE). Late code, constants
// and statics only (platform/3ds/late_reserve.ld): nothing that existed before moves.
//
// Old 3DS route runs still stalled in play on tile loads the
// builder already knows about:
//   1. A battle's tiles. UNDERTALE fights all happen in one battle room, so rooms.dtm keeps the last fight's
//      tiles for it, and the room plan's room-level scene for it holds only what every fight draws. The plan has a
//      scene per battle group (tools/roomplan/build_plan.py), but only its code entries were used: each new monster's
//      sprites loaded from the SD card in the fight's first shown frames (bat_water f3507: 3 tiles, 146 ms of draw).
//      At a battle room change the group's scene now stands in for the room's record, so the hidden room load takes
//      its first-frame tiles (pass 0) and its other early tiles (passes 1-2) like any remembered room's.
//   2. Tiles a room first draws after its first 45 frames (rooms.dtm keeps only those). The plan records first use up
//      to frame 254; such tiles loaded from the SD card in play (c0r180 on the New 3DS: page 24 at f46-f52, one tile a
//      frame in 99-404 ms). Once the room's own tiles are done, those are marked wanted and the hidden room load's
//      passes 1-2 run again for them. Whatever of them is still missing then is no longer wanted, so filling in never
//      loads them in play: they load at their first draw, as before. (A battle group's tiles of the first 45 frames
//      are the room's record, so filling in treats them as it treats any record's.)
//   3. The room load's time budget (z_roomload.c, 200 ms of added work, pass 0 up to 200 ms more) ran out in the
//      fights that stalled (the first fight: 312 ms). A room load whose record a battle group's scene stood in for
//      (1) gets UTSTALL_EXTRA_NANOS more, in the room-change frame, which is never presented: a transition
//      that is accepted as longer. Other room loads keep the base budget, and the later tiles (2) load
//      only in what is left of it. Extra time in every room load (build C) made two Old 3DS scenes slower on hardware
//      (utstall2 rows): with the time, the room load also reaches the font pages (ctr_textures.c roomLoadFonts),
//      whose read-back then shares the SD card with the room's first shown frames, and the background worker reaches
//      a big page before the room that draws it, so its packed copy is turned away and its tiles load one by one in
//      play (the final Flowey room, page 14).
//   4. A room's planned pages are kept or read into RAM first. The background worker reads cache files whole in data.win
//      order once, from boot, and keepPacked (ctr_textures.c) keeps a copy over the packed budget only by dropping
//      copies used longer ago than its page. A page no room has drawn yet has nothing older than it, so when the
//      budget is full its copy is freed and the worker keeps no more copies: on the Old 3DS, page 14 (4.1 MB, 62 of
//      its 64 tiles in the Flowey fight room's first frames) was turned away whenever the worker reached it before
//      the Flowey fight room did (utstall3 o3ds e294 3B: "14 packed in RAM, 2.6 of 5.7 MB", heap peak 22.7 MB = the 18.4 MB
//      left plus the 4.1 MB copy, freed), and its tiles then came from the SD card one by one in the room's first
//      shown frames (frame 5: 790 ms). The tile reader's whole-file reads need the copy and 7 MB more free, which
//      never holds on the Old 3DS for that page, so only the worker's rule (the copy and an eighth of the heap free,
//      4 to 8 MB) can bring it. Now, before the room load, each page of the room's record (or plan) that has no
//      copy and whose missing planned tiles are at least half of its tiles gets one, ahead of later work: copies of
//      pages the room doesn't plan are dropped, least recently used first, until it fits both the packed budget and
//      the worker's heap rule (nothing is dropped if it can't fit); then a page the worker hasn't looked at yet goes
//      first in its order, and one it has passed by is read whole here, in the hidden room-change frame (one per
//      room load).
//   5. A room's whole set, when it fits. The plan's scene for UNDERTALE holds, after the tiles its record saw, every
//      tile the room can draw that the builder can name (tools/roomplan/closure.py: every frame of each sprite the
//      record drew from, the room's backgrounds and tiles, the sprite of each visible object placed in it), at bucket
//      255. Items 2-3 load only what the record saw in its first 254 frames, in what is left of the base budget, so a
//      room's later or unrecorded tiles still came from the SD card one by one in play (Old 2DS a multi-tile fire room:
//      frames 11, 14 and 15 at 82-89 ms each; the background worker keeps no copy of pages 15-25 on the Old models, so
//      Hotland's, the Core's and the battles' pages are read tile by tile). Every UNDERTALE scene but one debug room
//      fits the Old texture budget less ROOMLOAD_SPRITE_HEADROOM_BYTES (largest: the Flowey fight room 16.1 MB, the battles
//      10.2 MB at most). When the scene's whole set fits (its tiles at their
//      formats, 4 bytes a texel where a page's formats aren't known yet), the room change brings it all in:
//      (a) item 4 looks at the set's pages too, and reads a page whole when a fifth of its tiles are wanted and missing
//          (z_roomload's estimates: a page's tiles one by one cost 45 ms and 1 ms a KB, the whole file 45 ms and
//          0.2 ms a KB), any number of pages, as the packed budget and heap rule allow;
//      (b) after the room load has run as before, tiles neither in the set nor wanted that weren't drawn in the old
//          room's last frame make room, least recently used first, until the set fits below the budget less the
//          headroom (the set's resident tiles are stamped as last used in the frame before, as the room load stamps
//          its own); then the set's missing tiles are marked and the room load runs again, until UTSTALL_WHOLE_NANOS
//          past the base deadline. Marked tiles it didn't load are no longer wanted, as in item 2.
//      A scene whose set doesn't all fit brings in the longest start of it that does (rule A), in first-use order, so
//      the record's own tiles come first. Before the count, the pages of the scene's tiles first used by
//      ROOMPLAN_SEED_FRAMES are learnt (CtrTextures_acquire: their cache file headers, which the room load's marking
//      opens in this frame anyway), so their tiles count at their formats rather than at 4 bytes a texel (Old 2DS,
//      the largest scene: 80 planned tiles counted at 20 MiB were 13.8 MB, and 62 of a page's tiles were left to
//      their first draw: a 786 ms frame and 16 more of 50-195 ms).
//   6. The planned tiles past that start load in play. After the hidden load (rule B), a copy whose page has no missing
//      tile left in the scene is dropped, and the pages with the most of the rest missing get copies, as item 4 gives
//      them, until UTSTALL_WHOLE_NANOS past the base deadline: the copy kept in RAM is then the one play needs (the
//      largest scene's attacks: about 6 MB of one page, 33-43 ms a tile from the SD card on the Old models, 6-8 ms
//      from a copy). Choosing those pages before the room load instead turned away the copy of the page the scene's
//      start comes from, whose tiles then loaded in play (Azahar Old, utstall.md section 15).
// Textures only: no game state, script or timing the game sees changes, and every tile is the same texels whenever it
// loads.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(ENABLE_WAD16)
#include <malloc.h>
#include <stdio.h>

#include "ctr_memory.h"
#include "ctr_texture_pack.h"
#include "ctr_textures.h"
#include "data_win.h"
#include "gettime.h"
#include "log.h"
#include "roomplan_format.h"
#include "runner.h"
#include "stb_ds.h"

#define UTSTALL_TEXT __attribute__((noinline, section(".text.late.utstall")))
#define UTSTALL_CONST __attribute__((aligned(4), section(".rodata.utstall_in_text")))
#define UTSTALL_BSS __attribute__((section(".bss.late.utstall")))
#define UTSTALL7_TEXT __attribute__((noinline, section(".text.late.utstall7")))
#define UTSTALL7_CONST __attribute__((aligned(4), section(".rodata.utstall7_in_text")))
#define UTSTALL7_BSS __attribute__((section(".bss.late.utstall7")))

// The hidden room load's extra time (on top of z_roomload.c's ROOMLOAD_BUDGET_NANOS).
#define UTSTALL_EXTRA_NANOS (300ull * 1000000ull)
// A plan tile first used up to this frame after the room change is marked for the room load (254: the plan's last
// exact bucket; 255 means later).
#define UTSTALL_LATER_FRAMES 254u
#define UTSTALL_MARKS_MAX 512u
// rooms.dtm's entry (ctr_textures.c's tile-entry macros, repeated).
#define UTSTALL_TILE_PAGE(entry) (((entry) & 0x07FFFFFFu) >> 8)
#define UTSTALL_TILE_INDEX(entry) ((int32_t) ((entry) & 0xFFu))

void __real_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner);
void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner);
void __real_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
void usRoomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
// ctr_texture_pack.c: the background worker's state for a page and its heap reserve (item 4, at the file's end), and
// roomload's putFirst.
int32_t CtrPackWorker_utstallState(CtrPackWorker* worker, uint32_t pageId, uint32_t* reserve);
bool CtrPackWorker_putFirst(CtrPackWorker* worker, uint32_t pageId);
extern uint32_t __ctru_heap_size;
// Pages of a room's record looked at for item 4, at most.
#define UTSTALL_KEEP_PAGES 16u
// Item 5: the whole set's room load may run this far past the base deadline (the slowest cold set on the Old models,
// the largest scene's, is estimated at 1.9 s with its big page read whole, 2.5 s tile by tile; the others at 0.9 s at
// most). A longer hidden transition is accepted where it buys smooth play.
#define UTSTALL_WHOLE_NANOS (2500ull * 1000000ull)
// ROOMLOAD_SPRITE_HEADROOM_BYTES (ctr_textures.c), repeated: what the set leaves free of the texture budget.
#define UTSTALL_WHOLE_HEADROOM (4u * 1024u * 1024u)
// A page's tile grid at most (CTR_PAGE_MAX_TILES), as 64-bit words.
#define UTSTALL_MASK_WORDS 4u
// z_roomplan.c, utstall's accessors at its end, and the plan scene's code entries (z_roomload reads them too).
const uint8_t* RoomPlan_utstallPlan(const CtrTextureCache* cache);
int32_t RoomPlan_utstallGroup(Runner* runner);
uint32_t RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max);
// Item 7: the plan's code read after the tiles takes this long at most, stops at this much, and leaves the heap at least
// this much free (the code arena's safeMalloc aborts where the lazy read would fail).
#define UTSTALL_CODE_NANOS (400ull * 1000000ull)
#define UTSTALL_CODE_BYTES (512u * 1024u)
#define UTSTALL_CODE_HEAP_FREE (2u * 1024u * 1024u)
#define UTSTALL_CODE_IDS 1024u

static UTSTALL_BSS struct {
    const CtrTextureCache* cache; // the room change the fields below are for
    Runner* runner;
    const uint8_t* scene;         // the plan's scene for the room (its battle group's if planned), or nullptr
    int32_t room;
    int32_t group;                // the battle group whose scene stood in for the room's record, or -1
    uint32_t battleTiles;
    uint16_t page[UTSTALL_MARKS_MAX]; // the later tiles marked for this room load
    uint8_t tile[UTSTALL_MARKS_MAX];
    uint32_t marks;
    bool whole;                   // item 5: the scene's whole set fits, so the room load brings it all in
    uint32_t wholeCount;          // the scene's tiles it brings in: all of them, or the start that fits (rule A)
    uint32_t wholeTiles;          // the set's tiles that aren't empty, and their texture memory (4 bytes a texel where
    uint32_t wholeBytes;          // their page's formats aren't known)
    uint32_t evictedBytes;        // what made room for the set
} us;

static const char usLog[] UTSTALL_CONST =
    "Room load (utstall): %s group %ld: %u battle tiles for the record; later tiles %u marked, %u loaded, %u left to their first draw; %.0f ms, %.0f ms past the base budget\n";

static const char usKeepLog[] UTSTALL_CONST =
    "Room load (utstall): page %u (%u KB; %u of its %u tiles planned and missing) read whole into RAM in %u ms; %u KB of unplanned pages' copies dropped\n";
static const char usAheadLog[] UTSTALL_CONST =
    "Room load (utstall): page %u (%u KB; %u of its %u tiles planned and missing) first for the background worker; %u KB of unplanned pages' copies dropped\n";
static const char usNoRoomLog[] UTSTALL_CONST =
    "Room load (utstall): page %u (%u KB; %u of its %u tiles planned and missing) left to its tiles: %u KB needed, %u KB of copies could go\n";
static const char usReadFailLog[] UTSTALL_CONST =
    "Room load (utstall): page %u (%u KB) not read whole (no room in the heap); %u KB of copies dropped for it\n";
static const char usDoneLog[] UTSTALL_CONST =
    "Room load (utstall): page %u's copy (%u KB) dropped: the scene's tiles from it are all resident\n";
static const char usWholeLog[] UTSTALL_CONST =
    "Room load (utstall): %s group %ld: whole set %u of %u entries (%u tiles), %u KB of a %u KB budget; %u KB of other tiles evicted; %u marked, %u loaded, %u left; %.0f ms, %.0f ms past the base budget\n";

// The plan's scene for (room, sub), or nullptr. sub ROOMPLAN_NO_SUB is the room-level scene.
static UTSTALL_TEXT const uint8_t* usFindScene(const uint8_t* plan, uint32_t room, uint32_t sub) {
    RoomPlanHeader h;
    memcpy(&h, plan, sizeof(h));
    size_t at = sizeof(h);
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        memcpy(&scene, plan + at, sizeof(scene));
        if (scene.room == room && scene.sub == sub) return plan + at;
        at += RoomPlan_sceneSize(&scene);
    }
    return nullptr;
}

// 5. The scene's tiles: entry i, its page (nullptr if not a cache page with a known grid) and tile index.
static UTSTALL_TEXT CtrPage* usSceneTile(CtrTextureCache* cache, const uint8_t* scene, uint32_t i, int32_t* t) {
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    uint32_t entry;
    memcpy(&entry, scene + sizeof(s) + (size_t) i * 4, 4);
    uint32_t pageId = UTSTALL_TILE_PAGE(entry);
    *t = UTSTALL_TILE_INDEX(entry);
    if (pageId >= cache->pageCount) return nullptr;
    CtrPage* page = &cache->pages[pageId];
    if (!page->layoutKnown || page->failed || page->tiles == nullptr || *t >= page->tilesX * page->tilesY) return nullptr;
    return page;
}

// 5. A scene tile's texture memory: its format's, 0 if empty, the largest tile (CTR_TILE_MAX texels a side, 4 bytes a
//    texel) where its page's grid isn't known, 4 bytes a texel where its formats aren't.
static UTSTALL_TEXT uint64_t usTileBytes(const CtrPage* page, int32_t t) {
    if (page == nullptr) return (uint64_t) CTR_TILE_MAX * CTR_TILE_MAX * 4u;
    const CtrTile* tile = &page->tiles[t];
    if (page->formatsKnown && tile->format == CTR_TILE_EMPTY) return 0;
    return (uint64_t) page->tileW * (uint64_t) page->tileH * (page->formatsKnown ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u);
}

// 5. How many of the scene's tiles, in order, fit the texture budget less the headroom: all of them when the whole set
//    fits, else the longest start that does (rule A); 0 if no tile does. The pages of its tiles first used by
//    ROOMPLAN_SEED_FRAMES are learnt first (the room load's marking opens their cache files in this frame anyway; a
//    page without one is left alone, as item 4 does). A page whose grid isn't known counts each of its tiles at the
//    largest tile (CTR_TILE_MAX texels a side, 4 bytes a texel).
static UTSTALL_TEXT uint32_t usWholeFits(CtrTextureCache* cache, const uint8_t* scene) {
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    const uint8_t* buckets = scene + sizeof(s) + (size_t) s.tileCount * 4;
    for (uint32_t i = 0; s.tileCount > i && ROOMPLAN_SEED_FRAMES >= buckets[i]; i++) {
        uint32_t entry;
        memcpy(&entry, scene + sizeof(s) + (size_t) i * 4, 4);
        uint32_t pageId = UTSTALL_TILE_PAGE(entry);
        if (pageId >= cache->pageCount) continue;
        const CtrPage* page = &cache->pages[pageId];
        if (page->layoutKnown || page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING || cache->dataWin->txtr.textures[pageId].externalPath != nullptr) continue;
        CtrTextures_acquire(cache, pageId);
    }
    uint64_t bytes = 0;
    uint32_t tiles = 0;
    uint32_t count = 0;
    for (uint32_t i = 0; s.tileCount > i; i++) {
        int32_t t = 0;
        uint64_t b = usTileBytes(usSceneTile(cache, scene, i, &t), t);
        if (bytes + b + UTSTALL_WHOLE_HEADROOM > (uint64_t) cache->budgetBytes) break;
        bytes += b;
        tiles += b > 0;
        count = i + 1;
    }
    us.wholeTiles = tiles;
    us.wholeBytes = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t) bytes;
    return tiles > 0 ? count : 0;
}

// 5. Makes room for the set: its resident tiles count as last used in the frame before this one (as the room load
//    stamps its own), then tiles not drawn in the old room's last frame go, least recently used first, until the set's
//    missing tiles and the room's other wanted ones fit below the budget less the headroom. Returns the bytes evicted.
static UTSTALL_TEXT uint32_t usWholeMakeRoom(CtrTextureCache* cache, const uint8_t* scene) {
    if (cache->frame == 0) return 0;
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    uint64_t missing = 0;
    for (uint32_t i = 0; s.tileCount > i && us.wholeCount > i; i++) {
        int32_t t = 0;
        CtrPage* page = usSceneTile(cache, scene, i, &t);
        if (page == nullptr) continue;
        CtrTile* tile = &page->tiles[t];
        if (tile->valid) {
            if (cache->frame - 1 > tile->lastUsedFrame) tile->lastUsedFrame = cache->frame - 1;
        } else if (!tile->wanted && !tile->reserved && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY)) {
            missing += (uint64_t) page->tileW * page->tileH * (page->formatsKnown ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u);
        }
    }
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* page = &cache->pages[p];
        if (!page->layoutKnown || page->tiles == nullptr) continue;
        int32_t tiles = page->tilesX * page->tilesY;
        for (int32_t t = 0; tiles > t; t++) {
            CtrTile* tile = &page->tiles[t];
            if (!tile->wanted) continue;
            if (tile->valid) {
                if (cache->frame - 1 > tile->lastUsedFrame) tile->lastUsedFrame = cache->frame - 1;
            } else if (!tile->reserved && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY)) {
                missing += (uint64_t) page->tileW * page->tileH * (page->formatsKnown ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u);
            }
        }
    }
    uint32_t before = cache->residentBytes;
    while ((uint64_t) cache->residentBytes + missing + UTSTALL_WHOLE_HEADROOM > cache->budgetBytes) {
        if (!CtrTextures_evictLeastRecentIdle(cache, 1)) break;
    }
    return before > cache->residentBytes ? before - cache->residentBytes : 0;
}

// 4. The heap free, as ctr_textures.c and ctr_texture_pack.c count it.
static UTSTALL_TEXT uint32_t usHeapFree(void) {
    struct mallinfo mi = mallinfo();
    return __ctru_heap_size > (uint32_t) mi.uordblks ? __ctru_heap_size - (uint32_t) mi.uordblks : 0;
}

static UTSTALL_TEXT bool usListed(const uint32_t* pages, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; count > i; i++) {
        if (pages[i] == id) return true;
    }
    return false;
}

// Whether page i's packed copy may go for item 4: the room doesn't plan the page, and the copy is idle (no read of it
// in flight, not the whole-file read's page, not the page being loaded).
static UTSTALL_TEXT bool usMayDrop(const CtrTextureCache* cache, uint32_t i, const uint32_t* pages, uint32_t count) {
    const CtrPage* p = &cache->pages[i];
    return p->packed != nullptr && p->read == nullptr && p != cache->busyPage && !(cache->wholeRead != nullptr && cache->wholePage == i) &&
           !usListed(pages, count, i);
}

// Drops copies item 4 may drop, least recently used first, until `size` bytes more fit the packed budget and leave
// `reserve` of the heap free. First proves that enough can go, so a page that can't fit drops nothing. Returns whether
// it fits; *dropped gets what went, *needed and *eligible what was needed and what could go.
static UTSTALL_TEXT bool usMakeRoom(CtrTextureCache* cache, const uint32_t* pages, uint32_t count, uint32_t size, uint32_t reserve,
                                    uint32_t* dropped, uint32_t* needed, uint32_t* eligible) {
    *dropped = 0;
    *needed = 0;
    *eligible = 0;
    if (size > cache->packedBudgetBytes) return false;
    uint64_t held = (uint64_t) cache->packedBytes + cache->wholeBytes;
    uint64_t budgetNeed = held + size > cache->packedBudgetBytes ? held + size - cache->packedBudgetBytes : 0;
    uint64_t freeBytes = usHeapFree();
    uint64_t heapNeed = (uint64_t) size + reserve > freeBytes ? (uint64_t) size + reserve - freeBytes : 0;
    uint64_t need = budgetNeed > heapNeed ? budgetNeed : heapNeed;
    uint64_t can = 0;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        if (usMayDrop(cache, i, pages, count)) can += cache->pages[i].packedSize;
    }
    *needed = need > UINT32_MAX ? UINT32_MAX : (uint32_t) need;
    *eligible = can > UINT32_MAX ? UINT32_MAX : (uint32_t) can;
    if (need > can) return false;
    uint64_t freed = 0;
    while (need > freed) {
        CtrPage* victim = nullptr;
        for (uint32_t i = 0; cache->pageCount > i; i++) {
            if (usMayDrop(cache, i, pages, count) && (victim == nullptr || victim->lastUsedFrame > cache->pages[i].lastUsedFrame)) {
                victim = &cache->pages[i];
            }
        }
        if (victim == nullptr) return false;
        uint32_t bytes = victim->packedSize;
        CtrPacked_free(victim->packed);
        victim->packed = nullptr;
        victim->packedSize = 0;
        cache->packedBytes -= bytes;
        freed += bytes;
        *dropped += bytes;
    }
    return true;
}

// 6. Rule B: the pages of the scene's tiles past the start that fits (us.wholeCount), most missing bytes first, into
//    pages[]. Returns how many.
static UTSTALL_TEXT uint32_t usDemandPages(CtrTextureCache* cache, const uint8_t* scene, uint32_t* pages) {
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    uint64_t want[UTSTALL_KEEP_PAGES];
    uint32_t n = 0;
    for (uint32_t i = us.wholeCount; s.tileCount > i; i++) {
        uint32_t entry;
        memcpy(&entry, scene + sizeof(s) + (size_t) i * 4, 4);
        uint32_t id = UTSTALL_TILE_PAGE(entry);
        int32_t t = 0;
        const CtrPage* page = usSceneTile(cache, scene, i, &t);
        if (id >= cache->pageCount || (page != nullptr && page->tiles[t].valid)) continue;
        uint32_t k = 0;
        while (n > k && pages[k] != id) k++;
        if (k == n) {
            if (UTSTALL_KEEP_PAGES == n) continue;
            pages[n] = id;
            want[n++] = 0;
        }
        want[k] += usTileBytes(page, t);
    }
    for (uint32_t m = 0; n > m; m++) {
        uint32_t best = m;
        for (uint32_t k = m + 1; n > k; k++) {
            if (want[k] > want[best]) best = k;
        }
        uint32_t id = pages[best];
        uint64_t w = want[best];
        pages[best] = pages[m];
        want[best] = want[m];
        pages[m] = id;
        want[m] = w;
    }
    return n;
}

// 4. The room's planned pages first: see the header. Runs before the room load (the original's marking acquires the
//    same pages in this frame), so its read-ahead and first-frame loads take the tiles from RAM.
//    Item 5 (whole set): the set's pages are looked at too, after the record's, and a page is read whole when a fifth
//    of its tiles are wanted by the record or set and missing, as many pages as fit.
static UTSTALL_TEXT void usKeepPlanned(CtrTextureCache* cache, uint32_t room) {
    if (cache->worker == nullptr || cache->packedBudgetBytes == 0 || cache->roomTiles == nullptr) return;
    const uint32_t* entries = cache->roomTiles[room];
    uint32_t pages[UTSTALL_KEEP_PAGES];
    uint32_t count = 0;
    for (ptrdiff_t i = 0; arrlen(entries) > i && UTSTALL_KEEP_PAGES > count; i++) {
        uint32_t id = UTSTALL_TILE_PAGE(entries[i]);
        if (cache->pageCount > id && !usListed(pages, count, id)) pages[count++] = id;
    }
    RoomPlanScene set = { 0 };
    const uint8_t* setTiles = nullptr;
    if (us.whole && us.scene != nullptr) {
        memcpy(&set, us.scene, sizeof(set));
        setTiles = us.scene + sizeof(set);
        for (uint32_t i = 0; set.tileCount > i && UTSTALL_KEEP_PAGES > count; i++) {
            uint32_t entry;
            memcpy(&entry, setTiles + (size_t) i * 4, 4);
            uint32_t id = UTSTALL_TILE_PAGE(entry);
            if (cache->pageCount > id && !usListed(pages, count, id)) pages[count++] = id;
        }
    }
    bool readOne = false;
    for (uint32_t k = 0; count > k; k++) {
        uint32_t id = pages[k];
        CtrPage* page = &cache->pages[id];
        const Texture* txtr = &cache->dataWin->txtr.textures[id];
        if (page->packed != nullptr || page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING || txtr->externalPath != nullptr) continue;
        if (CtrTextures_acquire(cache, id) == nullptr || page->tiles == nullptr) continue;
        int32_t tiles = page->tilesX * page->tilesY;
        uint32_t missing = 0;
        uint64_t seen[UTSTALL_MASK_WORDS] = { 0 };
        for (ptrdiff_t i = 0; arrlen(entries) > i; i++) {
            int32_t t = UTSTALL_TILE_INDEX(entries[i]);
            if (UTSTALL_TILE_PAGE(entries[i]) != id || t >= tiles) continue;
            seen[t >> 6] |= 1ull << (t & 63);
            const CtrTile* tile = &page->tiles[t];
            if (!tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY)) missing++;
        }
        for (uint32_t i = 0; setTiles != nullptr && set.tileCount > i; i++) {
            uint32_t entry;
            memcpy(&entry, setTiles + (size_t) i * 4, 4);
            int32_t t = UTSTALL_TILE_INDEX(entry);
            if (UTSTALL_TILE_PAGE(entry) != id || t >= tiles || (int32_t) (UTSTALL_MASK_WORDS * 64u) <= t || (seen[t >> 6] >> (t & 63) & 1u) != 0) continue;
            seen[t >> 6] |= 1ull << (t & 63);
            const CtrTile* tile = &page->tiles[t];
            if (!tile->valid && (!page->formatsKnown || tile->format != CTR_TILE_EMPTY)) missing++;
        }
        // Reading the whole file pays only when the room wants at least half of the page's tiles that aren't resident
        // (a fifth for item 5's whole set).
        if (0 >= tiles || tiles > (int32_t) ((setTiles != nullptr ? 5u : 2u) * missing)) continue;
        uint32_t reserve = 0;
        int32_t state = CtrPackWorker_utstallState(cache->worker, id, &reserve);
        if (0 > state) return;
        // A copy the worker has read, or is reading while it keeps copies, comes at a frame's start, and keepPacked
        // takes it: the page is now as recent as any.
        bool coming = state == 3 || (cache->workerKeepsPacked && 2 > state);
        if (coming && state != 0) continue;
        if (!coming && readOne && setTiles == nullptr) continue;
        FILE* f = CtrPack_openFile(id, nullptr);
        if (f == nullptr) continue;
        long size = CtrPack_fileSize(f);
        uint32_t dropped = 0;
        uint32_t needed = 0;
        uint32_t eligible = 0;
        if (0 >= size) {
            // Not a cache file to keep: the original's loads find out.
        } else if (!usMakeRoom(cache, pages, count, (uint32_t) size, reserve, &dropped, &needed, &eligible)) {
            logInfo(usNoRoomLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) missing, (unsigned) tiles, (unsigned) (needed / 1024),
                    (unsigned) (eligible / 1024));
        } else if (coming) {
            // Next in its order, and taken at once (hurry also wakes a worker waiting for memory).
            CtrPackWorker_putFirst(cache->worker, id);
            CtrPackWorker_hurry(cache->worker, id);
            logInfo(usAheadLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) missing, (unsigned) tiles, (unsigned) (dropped / 1024));
        } else {
            uint64_t start = nowNanos();
            // If the heap hasn't the room after all, the tiles come from the file one by one, as before.
            bool allowed = CtrMemory_allowRelief(false);
            CtrPacked* packed = CtrPack_readFile(f, &cache->key, txtr->blobOffset, txtr->blobSize);
            CtrMemory_allowRelief(allowed);
            readOne = true;
            if (packed == nullptr) {
                logInfo(usReadFailLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) (dropped / 1024));
            } else {
                page->packed = packed;
                page->packedSize = packed->size;
                page->cacheFile = CTR_CACHE_FILE_VALID;
                cache->packedBytes += packed->size;
                logInfo(usKeepLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) missing, (unsigned) tiles,
                        (unsigned) ((nowNanos() - start) / 1000000u), (unsigned) (dropped / 1024));
            }
        }
        fclose(f);
    }
}

// 6. Rule B, after the hidden load of a scene whose set didn't all fit: a copy whose page has no missing tile in the
//    scene is dropped (the room load has brought its tiles in), then the pages the rest of the set comes from in play
//    (usDemandPages: most missing bytes first) get copies, as item 4 gives them, until the limit. A copy of a page with
//    a scene tile still missing (one the room load didn't reach in time, say) is never dropped for them.
static UTSTALL_TEXT void usKeepDemand(CtrTextureCache* cache, uint64_t limit) {
    if (cache->worker == nullptr || cache->packedBudgetBytes == 0 || us.scene == nullptr) return;
    RoomPlanScene s;
    memcpy(&s, us.scene, sizeof(s));
    uint32_t pages[UTSTALL_KEEP_PAGES];
    uint32_t count = usDemandPages(cache, us.scene, pages);
    if (count == 0) return;
    // The scene's pages (the first 256) and those with a tile still missing.
    uint64_t held[4] = { 0 };
    uint64_t want[4] = { 0 };
    for (uint32_t i = 0; s.tileCount > i; i++) {
        uint32_t entry;
        memcpy(&entry, us.scene + sizeof(s) + (size_t) i * 4, 4);
        uint32_t id = UTSTALL_TILE_PAGE(entry);
        if (id >= cache->pageCount || id >= 256u) continue;
        held[id >> 6] |= 1ull << (id & 63);
        int32_t t = 0;
        const CtrPage* page = usSceneTile(cache, us.scene, i, &t);
        if (page == nullptr || (!page->tiles[t].valid && usTileBytes(page, t) > 0)) want[id >> 6] |= 1ull << (id & 63);
    }
    // usMakeRoom spares the listed pages: the demand pages, then every other page with a scene tile missing.
    uint32_t spare[UTSTALL_KEEP_PAGES * 2];
    uint32_t spared = count;
    memcpy(spare, pages, (size_t) count * 4);
    for (uint32_t id = 0; cache->pageCount > id && 256u > id && UTSTALL_KEEP_PAGES * 2 > spared; id++) {
        if ((want[id >> 6] >> (id & 63) & 1u) != 0 && !usListed(spare, spared, id)) spare[spared++] = id;
    }
    for (uint32_t id = 0; cache->pageCount > id && 256u > id; id++) {
        if ((held[id >> 6] >> (id & 63) & 1u) == 0 || (want[id >> 6] >> (id & 63) & 1u) != 0 || !usMayDrop(cache, id, pages, count)) continue;
        CtrPage* page = &cache->pages[id];
        uint32_t bytes = page->packedSize;
        CtrPacked_free(page->packed);
        page->packed = nullptr;
        page->packedSize = 0;
        cache->packedBytes -= bytes;
        logInfo(usDoneLog, (unsigned) id, (unsigned) (bytes / 1024));
    }
    for (uint32_t k = 0; count > k && limit > nowNanos(); k++) {
        uint32_t id = pages[k];
        CtrPage* page = &cache->pages[id];
        const Texture* txtr = &cache->dataWin->txtr.textures[id];
        if (page->packed != nullptr || page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING || txtr->externalPath != nullptr) continue;
        uint32_t reserve = 0;
        int32_t state = CtrPackWorker_utstallState(cache->worker, id, &reserve);
        if (0 > state) return;
        bool coming = state == 3 || (cache->workerKeepsPacked && 2 > state);
        if (coming && state != 0) continue;
        FILE* f = CtrPack_openFile(id, nullptr);
        if (f == nullptr) continue;
        long size = CtrPack_fileSize(f);
        uint32_t dropped = 0;
        uint32_t needed = 0;
        uint32_t eligible = 0;
        int32_t tiles = page->layoutKnown ? page->tilesX * page->tilesY : 0;
        // The page's scene tiles still missing, for the logs.
        uint32_t missing = 0;
        for (uint32_t i = 0; s.tileCount > i; i++) {
            uint32_t entry;
            memcpy(&entry, us.scene + sizeof(s) + (size_t) i * 4, 4);
            int32_t t = 0;
            const CtrPage* p = usSceneTile(cache, us.scene, i, &t);
            if (UTSTALL_TILE_PAGE(entry) == id && (p == nullptr || (!p->tiles[t].valid && usTileBytes(p, t) > 0))) missing++;
        }
        if (0 >= size) {
            // Not a cache file to keep.
        } else if (!usMakeRoom(cache, spare, spared, (uint32_t) size, reserve, &dropped, &needed, &eligible)) {
            logInfo(usNoRoomLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) missing, (unsigned) tiles, (unsigned) (needed / 1024),
                    (unsigned) (eligible / 1024));
        } else if (coming) {
            CtrPackWorker_putFirst(cache->worker, id);
            CtrPackWorker_hurry(cache->worker, id);
            logInfo(usAheadLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) missing, (unsigned) tiles, (unsigned) (dropped / 1024));
        } else {
            uint64_t start = nowNanos();
            bool allowed = CtrMemory_allowRelief(false);
            CtrPacked* packed = CtrPack_readFile(f, &cache->key, txtr->blobOffset, txtr->blobSize);
            CtrMemory_allowRelief(allowed);
            if (packed == nullptr) {
                logInfo(usReadFailLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) (dropped / 1024));
            } else {
                page->packed = packed;
                page->packedSize = packed->size;
                page->cacheFile = CTR_CACHE_FILE_VALID;
                cache->packedBytes += packed->size;
                logInfo(usKeepLog, (unsigned) id, (unsigned) (size / 1024), (unsigned) missing, (unsigned) tiles,
                        (unsigned) ((nowNanos() - start) / 1000000u), (unsigned) (dropped / 1024));
            }
        }
        fclose(f);
    }
}

// 1. A battle room's record: its battle group's planned tiles of the first ROOMPLAN_SEED_FRAMES frames, in place of
//    the last fight's (or the plan's tiles common to every fight), before the original marks the remembered tiles.
UTSTALL_TEXT void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    memset(&us, 0, sizeof(us));
    us.room = -1;
    us.group = -1;
    const uint8_t* plan = cache != nullptr ? RoomPlan_utstallPlan(cache) : nullptr;
    if (plan != nullptr && runner != nullptr && runner->currentRoom != nullptr && 0 <= runner->currentRoomIndex &&
        cache->roomCount > (uint32_t) runner->currentRoomIndex) {
        uint32_t room = (uint32_t) runner->currentRoomIndex;
        us.cache = cache;
        us.runner = runner;
        us.room = (int32_t) room;
        const uint8_t* whole = usFindScene(plan, room, ROOMPLAN_NO_SUB);
        us.scene = whole;
        // Only rooms the plan has battle groups for ask for the group (as RoomPlan_sceneCode does).
        int32_t group = -1;
        RoomPlanHeader h;
        memcpy(&h, plan, sizeof(h));
        size_t at = sizeof(h);
        for (uint32_t s = 0; h.sceneCount > s; s++) {
            RoomPlanScene scene;
            memcpy(&scene, plan + at, sizeof(scene));
            if (scene.room == room && scene.sub != ROOMPLAN_NO_SUB) {
                group = RoomPlan_utstallGroup(runner);
                break;
            }
            at += RoomPlan_sceneSize(&scene);
        }
        const uint8_t* sub = 0 <= group && (int32_t) ROOMPLAN_NO_SUB > group ? usFindScene(plan, room, (uint32_t) group) : nullptr;
        if (sub != nullptr) {
            RoomPlanScene scene;
            memcpy(&scene, sub, sizeof(scene));
            const uint8_t* tiles = sub + sizeof(scene);
            const uint8_t* buckets = tiles + (size_t) scene.tileCount * 4;
            uint32_t count = 0;
            while (scene.tileCount > count && ROOMPLAN_SEED_FRAMES >= buckets[count]) count++;
            if (count > 0) {
                arrsetlen(cache->roomTiles[room], count);
                memcpy(cache->roomTiles[room], tiles, (size_t) count * 4);
                us.battleTiles = count;
            }
            us.scene = sub;
            us.group = group;
        }
        // 5. The whole set, when it fits.
        if (us.scene != nullptr) us.wholeCount = usWholeFits(cache, us.scene);
        us.whole = us.wholeCount > 0;
    }
    if (cache != nullptr && runner != nullptr && 0 <= runner->currentRoomIndex && cache->roomCount > (uint32_t) runner->currentRoomIndex &&
        cache->pageCount > 0) {
        usKeepPlanned(cache, (uint32_t) runner->currentRoomIndex);
    }
    __real_CtrRoomload_prefetchRoom(cache, runner);
}

// 2 and 3. The room load runs as before, with UTSTALL_EXTRA_NANOS more where a battle group's scene stood in for the
// record. Then, in what is left of that time, the plan's tiles first used after ROOMPLAN_SEED_FRAMES are marked wanted
// (not drawn first) and the room load's passes run again for them, so they never take time or budget from the room's
// own tiles (review F3). Marked tiles it didn't load are no longer wanted. Only pages whose layout is known are marked:
// learning one opens its cache file or loads its image (review F1), and the room load's passes skip such pages anyway.
UTSTALL_TEXT void usRoomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    uint64_t start = nowNanos();
    bool ours = us.cache == cache && cache != nullptr && us.runner != nullptr && 0 <= us.room;
    uint64_t limit = ours && us.battleTiles > 0 ? deadline + UTSTALL_EXTRA_NANOS : deadline;
    __real_CtrTextures_roomLoadFinish(cache, limit, nowTiles, otherTiles);
    bool whole = ours && us.whole && us.scene != nullptr;
    if (whole) {
        // 5. The whole set: room for it, then all of its missing tiles are marked below, with more time.
        limit = deadline + UTSTALL_WHOLE_NANOS;
        RoomPlanScene scene;
        memcpy(&scene, us.scene, sizeof(scene));
        const uint8_t* tiles = us.scene + sizeof(scene);
        for (uint32_t i = 0; scene.tileCount > i && us.wholeCount > i; i++) {
            uint32_t entry;
            memcpy(&entry, tiles + (size_t) i * 4, 4);
            uint32_t pageId = UTSTALL_TILE_PAGE(entry);
            // Its grid from the data.win (CtrTextures_acquire), so that its tiles can be marked; not for a page without a
            // cache file, whose layout would come from loading its image (review F1): the passes skip it anyway.
            if (cache->pageCount <= pageId) continue;
            const CtrPage* page = &cache->pages[pageId];
            if (page->layoutKnown || page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING ||
                cache->dataWin->txtr.textures[pageId].externalPath != nullptr) continue;
            CtrTextures_acquire(cache, pageId);
        }
        us.evictedBytes = usWholeMakeRoom(cache, us.scene);
    }
    if (ours && us.scene != nullptr && limit > nowNanos()) {
        RoomPlanScene scene;
        memcpy(&scene, us.scene, sizeof(scene));
        const uint8_t* tiles = us.scene + sizeof(scene);
        const uint8_t* buckets = tiles + (size_t) scene.tileCount * 4;
        for (uint32_t i = 0; scene.tileCount > i && UTSTALL_MARKS_MAX > us.marks && (!whole || us.wholeCount > i); i++) {
            uint8_t bucket = buckets[i];
            if (!whole && (ROOMPLAN_SEED_FRAMES >= bucket || bucket > UTSTALL_LATER_FRAMES)) continue;
            uint32_t entry;
            memcpy(&entry, tiles + (size_t) i * 4, 4);
            uint32_t pageId = UTSTALL_TILE_PAGE(entry);
            int32_t t = UTSTALL_TILE_INDEX(entry);
            if (pageId >= cache->pageCount || pageId > UINT16_MAX) continue;
            CtrPage* page = &cache->pages[pageId];
            if (!page->layoutKnown || page->failed || page->tiles == nullptr || t >= page->tilesX * page->tilesY) continue;
            CtrTile* tile = &page->tiles[t];
            // Only tiles the room doesn't want yet and that are missing: the others are the original's.
            if (tile->wanted || tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
            tile->wanted = true;
            us.page[us.marks] = (uint16_t) pageId;
            us.tile[us.marks] = (uint8_t) t;
            us.marks++;
        }
        if (us.marks > 0 || (whole && us.evictedBytes > 0)) __real_CtrTextures_roomLoadFinish(cache, limit, nowTiles, otherTiles);
        if (whole && scene.tileCount > us.wholeCount) usKeepDemand(cache, limit);
    }
    uint64_t end = nowNanos();
    double past = end > deadline ? (double) (end - deadline) / 1e6 : 0.0;
    if (!ours) return;
    uint32_t loaded = 0, left = 0;
    for (uint32_t i = 0; us.marks > i; i++) {
        if (us.page[i] >= cache->pageCount) continue;
        CtrPage* page = &cache->pages[us.page[i]];
        if (!page->layoutKnown || page->tiles == nullptr || (int32_t) us.tile[i] >= page->tilesX * page->tilesY) continue;
        CtrTile* tile = &page->tiles[us.tile[i]];
        if (tile->valid) {
            loaded++;
        } else if (tile->wanted) {
            tile->wanted = false;
            left++;
        }
    }
    if (whole) {
        RoomPlanScene scene0;
        memcpy(&scene0, us.scene, sizeof(scene0));
        logInfo(usWholeLog, us.runner->currentRoom->name, (long) us.group, (unsigned) us.wholeCount, (unsigned) scene0.tileCount,
                (unsigned) us.wholeTiles, (unsigned) (us.wholeBytes / 1024),
                (unsigned) (cache->budgetBytes / 1024), (unsigned) (us.evictedBytes / 1024), (unsigned) us.marks, (unsigned) loaded,
                (unsigned) left, (double) (end - start) / 1e6, past);
    } else if (us.battleTiles > 0 || us.marks > 0 || past > 0.0) {
        logInfo(usLog, us.runner->currentRoom->name, (long) us.group, (unsigned) us.battleTiles, (unsigned) us.marks, (unsigned) loaded,
                (unsigned) left, (double) (end - start) / 1e6, past);
    }
    us.cache = nullptr;
}

// 7. The plan scene's code entries (RoomPlan_roomCode) whose bytecode isn't in memory, read back as their first run
//    would read them (DataWin_loadCodeBytes: the blob's whole group, into the code arena), in the hidden load after the
//    tiles. z_roomload reads the same list first but stops at ROOMLOAD_CODE_NANOS (120 ms), which the Old models' SD
//    card reaches after about 11 reads: a battle group's other groups were read in play, 8-18 ms in a Step each (o2ds
//    utstall7 codebox rows: 19 of 29 groups, 5 reads in play). Only where the bytecode is on file (the Old models).
//    It has sections of its own at the end of late_reserve.ld's lists (utstall7): the earlier utstall sections sit
//    between other late sections, and growing them would move those. So the wrap is a new function here, and the one
//    above keeps its bytes as usRoomLoadFinish.
static UTSTALL7_BSS int32_t usCodeIds[UTSTALL_CODE_IDS];

static const char usCodeLog[] UTSTALL7_CONST =
    "Room load (utstall): %s: the plan's code after the tiles, %u reads, %u KB in %.0f ms; %u entries not reached\n";

static UTSTALL7_TEXT void usReadCode(Runner* runner) {
    DataWin* dw = runner->dataWin;
    if (dw == nullptr || dw->bytecodeFile == nullptr || dw->codeBlobs == nullptr) return;
    uint32_t count = RoomPlan_roomCode(runner, usCodeIds, UTSTALL_CODE_IDS);
    uint64_t start = nowNanos();
    uint32_t loads = dw->codeStats.loads;
    uint32_t bytes = dw->codeStats.loadedBytes;
    uint32_t left = 0;
    for (uint32_t i = 0; count > i; i++) {
        int32_t id = usCodeIds[i];
        if (0 > id || (uint32_t) id >= dw->code.count) continue;
        CodeEntry* code = &dw->code.entries[id];
        if (!code->present || code->bytecode != nullptr) continue;
        if (nowNanos() - start >= UTSTALL_CODE_NANOS || dw->codeStats.loadedBytes - bytes >= UTSTALL_CODE_BYTES ||
            UTSTALL_CODE_HEAP_FREE > usHeapFree()) {
            left++;
            continue;
        }
        DataWin_loadCodeBytes(dw, code);
    }
    if (dw->codeStats.loads != loads || left > 0) {
        logInfo(usCodeLog, runner->currentRoom->name, (unsigned) (dw->codeStats.loads - loads),
                (unsigned) ((dw->codeStats.loadedBytes - bytes + 1023u) / 1024u), (double) (nowNanos() - start) / 1e6, (unsigned) left);
    }
}

UTSTALL7_TEXT void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    Runner* runner = us.cache == cache && cache != nullptr && 0 <= us.room ? us.runner : nullptr;
    usRoomLoadFinish(cache, deadline, nowTiles, otherTiles);
    if (runner != nullptr) usReadCode(runner);
}
#endif
