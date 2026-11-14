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
void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner); void utflDialogueCode(Runner* runner);
void __real_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
void usFinish7(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles); // R4, file end
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
// The eviction loop is usWholeEvict (z_utstall_l2.c, late code last in merge order); UTSTALL_L2_PAD keeps
// usWholeMakeRoom at its size before L2, so no symbol after it moves (ARM nops, run once a room load).
#define UTSTALL_L2_PAD __asm__ volatile(".rept 13\n\tnop\n\t.endr")
void usWholeEvict(CtrTextureCache* cache, uint64_t missing);
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

#ifdef CTRPORT_TARGET_O3DS
// flowstream (z_flowstream.c, the plan tiles it follows; Old models): the scene this room visit loaded, while it is the
// current room's (the test item 5's play-time code uses: us.cache is cleared once the room load ends, the scene stays
// until the next room's prefetch). In flowstream's late section: nothing of this file's own code or data moves.
const uint8_t* CtrUtstall_activeScene(const CtrTextureCache* cache);
__attribute__((noinline, section(".text.flowstream_late"))) const uint8_t* CtrUtstall_activeScene(const CtrTextureCache* cache) {
    (void) cache;
    return 0 <= us.room && us.runner != nullptr && us.runner->currentRoomIndex == us.room ? us.scene : nullptr;
}
#endif

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
        int32_t t = 0; CtrPage* page = usSceneTile(cache, scene, i, &t);
        uint64_t b = usTileBytes(page, t);
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
//    missing tiles and the room's other wanted ones fit below the budget less the headroom, and in linear memory above
//    its floor (freewins L2: the budget assumes the other linear users' share, and where they hold more, as in
//    Flowey's room, the passes stopped at the floor with stale tiles resident). Returns the bytes evicted.
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
    usWholeEvict(cache, missing);
    UTSTALL_L2_PAD;
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
static UTSTALL_TEXT CTR_COLD_OS bool usMakeRoom(CtrTextureCache* cache, const uint32_t* pages, uint32_t count, uint32_t size, uint32_t reserve,
                                    uint32_t* dropped, uint32_t* needed, uint32_t* eligible) {
    *dropped = 0;
    *needed = 0;
    *eligible = 0;
    if (size > cache->packedBudgetBytes) return false;
    uint64_t held = (uint64_t) cache->packedBytes + cache->wholeBytes;
    uint64_t budgetNeed = held + size > cache->packedBudgetBytes ? held + size - cache->packedBudgetBytes : 0;
    uint64_t freeBytes = CTR_PKLIN_US_HEAP_FREE();
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

// change utflowey (integrate17's flowey second run): in a room scene rule A cut, the record's page that holds the most of
// the set's tiles goes to item 4 first; the others keep their order. The record lists pages in the order its tiles were
// first drawn, and since round 17's item verdicts (.dtiv) it holds only what the room drew: Flowey's room 11 tiles, page
// 16's first, where the run-time verdicts had touched 82. Page 16 (2 MB) then took the copy, page 14 (4.2 MB, 64 set
// tiles) no longer fitted, and its tiles were too many for the room load's per-page estimate: 54-57 of them came from
// the SD card in play (Azahar Old 64 MB: 79 marked, 25 loaded, 54 left). The run without a record (the plan's first
// tiles) already has page 14 first, so it is unchanged, and the second run now takes the first run's order. Battle
// rooms (the group's tiles in place of the record) are left alone.
#define UTFLCUT_TEXT __attribute__((noinline, section(".text.late.utflpin")))
static UTFLCUT_TEXT void utflCutOrder(uint32_t* pages, uint32_t count, const uint8_t* setTiles, uint32_t setCount) {
    if (count == 0) return;
    uint32_t best = 0, most = 0;
    for (uint32_t k = 0; count > k; k++) {
        uint32_t held = 0;
        for (uint32_t i = 0; setCount > i; i++) {
            uint32_t entry;
            memcpy(&entry, setTiles + (size_t) i * 4, 4);
            if (UTSTALL_TILE_PAGE(entry) == pages[k]) held++;
        }
        if (held > most) {
            most = held;
            best = k;
        }
    }
    uint32_t id = pages[best];
    for (; best > 0; best--) pages[best] = pages[best - 1];
    pages[0] = id;
}

// 4. The room's planned pages first: see the header. Runs before the room load (the original's marking acquires the
//    same pages in this frame), so its read-ahead and first-frame loads take the tiles from RAM.
//    Item 5 (whole set): the set's pages are looked at too, after the record's, and a page is read whole when a fifth
//    of its tiles are wanted by the record or set and missing, as many pages as fit.
static UTSTALL_TEXT CTR_COLD_OS void usKeepPlanned(CtrTextureCache* cache, uint32_t room) {
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
        if (set.tileCount > us.wholeCount && 0 > us.group) utflCutOrder(pages, count, setTiles, set.tileCount);
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
#define UTSTALL_KEEP_SMALL (256u * 1024u)
static UTSTALL_TEXT CTR_COLD_OS void usKeepDemand(CtrTextureCache* cache, uint64_t limit) {
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
        // A small copy stays (change utflowey): it frees little room, and a tile of the page the plan doesn't list would
        // otherwise open the SD cache file on the game thread at first use (Omega Flowey's text box at the first ACT,
        // page 19, 93 KB: 16-20 ms of the 24 ms load is the open on Azahar). usMakeRoom may still drop it if needed.
        if (UTSTALL_KEEP_SMALL >= page->packedSize) continue;
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
UTSTALL_TEXT CTR_COLD_OS void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
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
        CTR_PKLIN_KEEP_PLANNED(cache, (uint32_t) runner->currentRoomIndex);
    }
    utflDialogueCode(runner); __real_CtrRoomload_prefetchRoom(cache, runner);
}

// 2 and 3. The room load runs as before, with UTSTALL_EXTRA_NANOS more where a battle group's scene stood in for the
// record. Then, in what is left of that time, the plan's tiles first used after ROOMPLAN_SEED_FRAMES are marked wanted
// (not drawn first) and the room load's passes run again for them, so they never take time or budget from the room's
// own tiles (review F3). Marked tiles it didn't load are no longer wanted. Only pages whose layout is known are marked:
// learning one opens its cache file or loads its image (review F1), and the room load's passes skip such pages anyway.
UTSTALL_TEXT CTR_COLD_OS void usRoomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
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

UTSTALL7_TEXT void usFinish7(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    Runner* runner = us.cache == cache && cache != nullptr && 0 <= us.room ? us.runner : nullptr;
    usRoomLoadFinish(cache, deadline, nowTiles, otherTiles);
    if (runner != nullptr) usReadCode(runner);
}

// ===[ change utflowey: in play, a cut scene's copy-backed tiles look older than they are ]===
// A scene rule A had to cut (its whole set doesn't fit the room load) is over the tile budget in play too: Omega
// Flowey's finale draws 22.7-24.7 MB of tiles in a stats window against the Old 3DS's 20.3 MB, so tiles go and come back
// whatever is kept (docs/3ds-hardware.md). A tile from a packed RAM copy loads in about 7 ms on the o3DS, one from the SD
// card cache in about 21 ms, and plain LRU evicted both alike: page 14 (Flowey's body, its copy dropped by rule B) came
// back from the SD card 285 times in the fight. So once a frame, before the frame's loads, each tile of the scene's
// pages that have a copy is made to look UTFL_AGE_FRAMES older, once per draw: eviction takes them before tiles that
// would come from the SD card, and keeps LRU order within each kind (no ties). Only draws at least UTFL_AGE_AFTER
// frames old are aged, so the tests on the last two frames (hot tiles, the linear floor's guard) see real frames, and
// a frame of 0 (loaded, never drawn) is left alone. An aged tile's real frame is kept: if the tile is evicted, the
// thrash test evictTile makes (ctr_textures.c, CtrTextures_roomThrashing) is made with it here, at the next frame. A page
// whose copy goes gets its tiles' real frames back, as do all tiles when the room changes. Nothing is allocated.
// (texcacheCollectWholeRead's once-a-frame call, ctr_textures.c, in CtrTextures_beginFrame after the frame number.)
#define UTFLPIN_TEXT __attribute__((noinline, section(".text.late.utflpin")))
#define UTFLPIN_CONST __attribute__((section(".rodata.utflpin_in_text")))
#define UTFLPIN_BSS __attribute__((section(".bss.late.utflpin")))
#define UTFL_AGE_FRAMES 600u
#define UTFL_AGE_AFTER 3u
#define UTFL_PAGES 256u
#define UTFL_TILES 512u
#define UTFL_NONE UINT32_MAX
static UTFLPIN_BSS struct {
    bool ready;                     // set up (bss starts zeroed; base[] needs -1s)
    CtrTextureCache* cache;         // the cache the scene is aged in, or nullptr
    const uint8_t* scene;           // the scene the pages below are for
    int32_t room;
    uint32_t used;                  // entries of aged[] given to pages
    bool refused;                   // a page was refused entries (logged once)
    uint64_t pages[UTFL_PAGES / 64]; // the scene's pages
    int16_t base[UTFL_PAGES];       // a page's first entry in aged[], or -1
    uint32_t aged[UTFL_TILES];      // the frame an aged tile was given, or UTFL_NONE (its real frame is UTFL_AGE_FRAMES more)
} utflAge;
static const char utflSlotLog[] UTFLPIN_CONST = "Room load (utflowey): page %u's %u tiles are tracked, %u of %u entries used\n";
static const char utflFullLog[] UTFLPIN_CONST =
    "Room load (utflowey): page %u's %u tiles aren't aged: %u of %u entries used\n";
static const char utflAgeLog[] UTFLPIN_CONST =
    "Room load (utflowey): %s: the set was cut, so in play tiles of its %u pages with a copy look %u frames older\n";

// Gives every tile still aged its real frame back, and forgets the scene.
static UTFLPIN_TEXT void utflAgeRestore(void) {
    CtrTextureCache* cache = utflAge.cache;
    for (uint32_t id = 0; cache != nullptr && cache->pageCount > id && UTFL_PAGES > id; id++) {
        if (0 > utflAge.base[id]) continue;
        CtrPage* page = &cache->pages[id];
        int32_t n = page->tiles != nullptr ? page->tilesX * page->tilesY : 0;
        for (int32_t i = 0; n > i && UTFL_TILES > (uint32_t) utflAge.base[id] + (uint32_t) i; i++) {
            uint32_t* e = &utflAge.aged[utflAge.base[id] + i];
            CtrTile* tile = &page->tiles[i];
            if (*e != UTFL_NONE && tile->valid && tile->lastUsedFrame == *e) tile->lastUsedFrame = *e + UTFL_AGE_FRAMES;
        }
    }
    memset(&utflAge, 0, sizeof(utflAge));
    memset(utflAge.base, 0xFF, sizeof(utflAge.base));
    utflAge.room = -1;
    utflAge.ready = true;
}

void __real_RoomLoad_collect(CtrTextureCache* cache, bool drop); void utflPkCollect(CtrTextureCache* cache, bool drop); void CtrFlowstream_collect(CtrTextureCache* cache, bool drop);
void __wrap_RoomLoad_collect(CtrTextureCache* cache, bool drop);
UTFLPIN_TEXT void __wrap_RoomLoad_collect(CtrTextureCache* cache, bool drop) {
    utflPkCollect(cache, drop);
    if (cache == nullptr) return;
    if (!utflAge.ready) utflAgeRestore();
    bool cut = us.whole && us.scene != nullptr && 0 <= us.room && us.runner != nullptr && us.runner->currentRoomIndex == us.room;
    RoomPlanScene scene;
    if (cut) {
        memcpy(&scene, us.scene, sizeof(scene));
        cut = scene.tileCount > us.wholeCount; // rule A didn't bring the whole set in
    }
    if (!cut || utflAge.cache != cache || utflAge.scene != us.scene || utflAge.room != us.room) {
        if (utflAge.cache != nullptr) utflAgeRestore();
        if (!cut) return;
        utflAge.cache = cache;
        utflAge.scene = us.scene;
        utflAge.room = us.room;
        uint32_t count = 0;
        for (uint32_t i = 0; scene.tileCount > i; i++) {
            uint32_t entry;
            memcpy(&entry, us.scene + sizeof(scene) + (size_t) i * 4, 4);
            uint32_t id = UTSTALL_TILE_PAGE(entry);
            if (id >= cache->pageCount || id >= UTFL_PAGES || (utflAge.pages[id >> 6] >> (id & 63) & 1u) != 0) continue;
            utflAge.pages[id >> 6] |= 1ull << (id & 63);
            if (cache->pages[id].packed != nullptr) count++;
        }
        for (uint32_t i = 0; UTFL_TILES > i; i++) utflAge.aged[i] = UTFL_NONE;
        logInfo(utflAgeLog, us.runner->currentRoom != nullptr ? us.runner->currentRoom->name : "?", (unsigned) count,
                (unsigned) UTFL_AGE_FRAMES);
    }
    uint32_t frame = cache->frame;
    for (uint32_t id = 0; cache->pageCount > id && UTFL_PAGES > id; id++) {
        if ((utflAge.pages[id >> 6] >> (id & 63) & 1u) == 0) continue;
        CtrPage* page = &cache->pages[id];
        if (!page->layoutKnown || page->tiles == nullptr) continue;
        int32_t n = page->tilesX * page->tilesY;
        if (0 > utflAge.base[id]) {
            if (page->packed == nullptr) continue; // nothing to age
            if ((uint32_t) n > UTFL_TILES - utflAge.used) { // no room to track it
                if (!utflAge.refused) logInfo(utflFullLog, (unsigned) id, (unsigned) n, (unsigned) utflAge.used, (unsigned) UTFL_TILES);
                utflAge.refused = true;
                continue;
            }
            utflAge.base[id] = (int16_t) utflAge.used;
            utflAge.used += (uint32_t) n;
            logInfo(utflSlotLog, (unsigned) id, (unsigned) n, (unsigned) utflAge.used, (unsigned) UTFL_TILES);
        }
        bool copy = page->packed != nullptr;
        for (int32_t i = 0; n > i; i++) {
            uint32_t* e = &utflAge.aged[utflAge.base[id] + i];
            CtrTile* tile = &page->tiles[i];
            if (*e != UTFL_NONE) {
                if (!tile->valid) {
                    // Evicted since the last frame: evictTile's thrash test, with the real frame.
                    uint32_t real = *e + UTFL_AGE_FRAMES;
                    if (real >= cache->roomStartFrame && real + CTR_THRASH_FRAMES + 1u >= frame) cache->roomThrashed = true;
                    *e = UTFL_NONE;
                    continue;
                }
                if (tile->lastUsedFrame == *e) {
                    if (copy) continue; // still aged
                    tile->lastUsedFrame = *e + UTFL_AGE_FRAMES; // the copy went: its real frame back
                    *e = UTFL_NONE;
                    continue;
                }
                *e = UTFL_NONE; // drawn (or reset) since
            }
            uint32_t real = tile->lastUsedFrame;
            if (!copy || !tile->valid || real == 0 || real + UTFL_AGE_AFTER > frame || UTFL_AGE_FRAMES >= real) continue;
            tile->lastUsedFrame = real - UTFL_AGE_FRAMES;
            *e = tile->lastUsedFrame;
        }
    }
}
// Packed-tier fix (change utflowey, round 18; UNDERTALE). On a 64 MB Old 3DS the heap has 4-6 MB free in play, so a
// promotion (a page read whole into a packed copy) that wanted its size plus 8 MB free never happened, and a page first
// drawn once the copies had filled the budget came from the SD card for good (nx_opera's page 24: tiles in 86-103 ms
// rather than 4-9 ms). The reserve is now 4 MB there (ctr_tile_reader.c, CMakeLists.txt): within the budget idle copies
// go one for one, so the heap stays where it was. Here, once a frame before the promotion pick (RoomLoad_collect's
// wrap): a page asks for promotion only when the SD card served it again within UTFLPK_REPEAT_FRAMES (a page drawn
// once doesn't push a copy out), and the Old budget is the heap less the game's measured need instead of what was
// free at startup less 12 MB (so a little more heap taken before startup doesn't shrink it). Need: heap used less
// packed copies, worst over the hardware logs, 19.7 MB (Omega Flowey; battles 17.5-18.2), rounded up. The low-heap
// drop (relieveHeap, 3 MB) stays the safety net. Nothing else is allocated.
#define UTFLPK_TEXT __attribute__((noinline, section(".text.late.utflpk")))
#define UTFLPK_CONST __attribute__((aligned(4), section(".rodata.utflpk_in_text")))
#define UTFLPK_BSS __attribute__((section(".bss.late.utflpk")))
#define UTFLPK_PAGES 64u
#define UTFLPK_REPEAT_FRAMES 1800u
#define UTFLPK_GAME_NEED (20u * 1024u * 1024u)
#define UTFLPK_BUDGET_MAX (24u * 1024u * 1024u) // ctr_textures.c's PACKED_BUDGET_MAX
#ifdef CTRPORT_TARGET_O3DS
static UTFLPK_BSS struct {
    const CtrPage* pages; // the cache's pages the rest is for
    uint32_t pageCount;
    uint32_t fileLoads[UTFLPK_PAGES]; // each page's fileLoads when last looked at
    uint32_t sdFrame[UTFLPK_PAGES];   // the frame of its last load from the SD card, plus 1 (0: none)
} utflPk;
static const char utflPkBudgetLog[] UTFLPK_CONST =
    "Textures: packed budget %u KB (heap %u KB less the game's measured need, %u KB; was %u KB)\n";
#endif
UTFLPK_TEXT void utflPkCollect(CtrTextureCache* cache, bool drop) {
#ifdef CTRPORT_TARGET_O3DS
    CtrFlowstream_collect(cache, drop); // flowstream (Old models): RoomLoad_collect, then its refills
#else
    __real_RoomLoad_collect(cache, drop);
#endif
#ifdef CTRPORT_TARGET_O3DS
    if (cache == nullptr || cache->pageCount > UTFLPK_PAGES) return;
    if (utflPk.pages != cache->pages || utflPk.pageCount != cache->pageCount) {
        memset(&utflPk, 0, sizeof(utflPk));
        utflPk.pages = cache->pages;
        utflPk.pageCount = cache->pageCount;
        for (uint32_t i = 0; cache->pageCount > i; i++) utflPk.fileLoads[i] = cache->pages[i].fileLoads;
        if (cache->packedBudgetBytes != 0) { // 0: no copies at all
            uint32_t share = __ctru_heap_size > UTFLPK_GAME_NEED ? __ctru_heap_size - UTFLPK_GAME_NEED : 0;
            if (share > UTFLPK_BUDGET_MAX) share = UTFLPK_BUDGET_MAX;
            logInfo(utflPkBudgetLog, (unsigned) (share / 1024), (unsigned) (__ctru_heap_size / 1024), (unsigned) (UTFLPK_GAME_NEED / 1024),
                    (unsigned) (cache->packedBudgetBytes / 1024));
            cache->packedBudgetBytes = share;
        }
    }
    uint32_t frame = cache->frame + 1;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        CtrPage* p = &cache->pages[i];
        uint32_t loads = p->fileLoads - utflPk.fileLoads[i];
        utflPk.fileLoads[i] = p->fileLoads;
        if (loads == 0) continue;
        bool again = loads > 1 || (utflPk.sdFrame[i] != 0 && frame - utflPk.sdFrame[i] <= UTFLPK_REPEAT_FRAMES);
        utflPk.sdFrame[i] = frame;
        // A first load asks again at its next one (promotionRetryFrame is set only by an attempt).
        if (!again && p->packed == nullptr) p->promotionPending = false;
    }
#endif
}

// ===[ change utflowey: the dialogue's code, read in the hidden room load ]===
// Where the bytecode lives on file (the Old models), a code entry's group is read the first time it runs
// (DataWin_loadCodeBytes). The first textbox after the game starts runs the dialogue scripts, about 190 KB of bytecode
// (SCR_TEXT alone 146 KB), read in that Step: 38 ms on Azahar, a 57-64 ms Step on the Old 3DS and 2DS (tour r17:
// room_flowey_regret's first line, reached straight from the start by obj_time's Step; the first textbox of each
// fresh tour session in tundra_sansroom, water_friendlyhub and fire_multitile). The room plan's scene code misses them,
// since its recordings had them in memory from earlier rooms. So at each room load, before the original (the prefetch
// wrap above), the entries named here that aren't in memory are read as their first run would read them; nothing
// evicts bytecode, so from the first room load after startup on they stay, and later room loads look at nothing.
// Names, not indices, so any copy of the game matches; a name the game lacks is skipped.
#define UTFLDLG_COUNT 9u
static const char utflDlg0[] UTFLPK_CONST = "gml_Script_scr_gettext";
static const char utflDlg1[] UTFLPK_CONST = "gml_Object_obj_dialoguer_Create_0";
static const char utflDlg2[] UTFLPK_CONST = "gml_Script_SCR_TEXT";
static const char utflDlg3[] UTFLPK_CONST = "gml_Object_obj_base_writer_Create_0";
static const char utflDlg4[] UTFLPK_CONST = "gml_Script_SCR_TEXTTYPE";
static const char utflDlg5[] UTFLPK_CONST = "gml_Script_SCR_TEXTSETUP";
static const char utflDlg6[] UTFLPK_CONST = "gml_Script_scr_replace_buttons_pc";
static const char utflDlg7[] UTFLPK_CONST = "gml_Object_obj_base_writer_Draw_0";
static const char utflDlg8[] UTFLPK_CONST = "gml_Script_snd_stop";
static const char* const utflDlgNames[UTFLDLG_COUNT] UTFLPK_CONST = {utflDlg0, utflDlg1, utflDlg2, utflDlg3, utflDlg4,
                                                                     utflDlg5, utflDlg6, utflDlg7, utflDlg8};
static const char utflDlgLog[] UTFLPK_CONST = "Room load (utflowey): %s: the dialogue's code, %u reads, %u KB in %u ms%s\n";
static const char utflDlgShort[] UTFLPK_CONST = "; heap short, rest at the next room load";
static const char utflDlgNone[] UTFLPK_CONST = "";
static UTFLPK_BSS struct {
    const DataWin* dw;          // the data.win the entries were found in
    int32_t ids[UTFLDLG_COUNT]; // their code entries, -1 where the game has none
    bool done;                  // all of them in memory
} utflDlg;

UTFLPK_TEXT void utflDialogueCode(Runner* runner) {
    DataWin* dw = runner != nullptr ? runner->dataWin : nullptr;
    if (dw == nullptr || dw->bytecodeFile == nullptr || dw->codeBlobs == nullptr) return;
    if (utflDlg.dw != dw) {
        utflDlg.dw = dw;
        utflDlg.done = false;
        for (uint32_t n = 0; UTFLDLG_COUNT > n; n++) utflDlg.ids[n] = -1;
        for (uint32_t i = 0; dw->code.count > i; i++) {
            const char* name = dw->code.entries[i].name;
            if (name == nullptr) continue;
            for (uint32_t n = 0; UTFLDLG_COUNT > n; n++) {
                if (0 > utflDlg.ids[n] && strcmp(name, utflDlgNames[n]) == 0) utflDlg.ids[n] = (int32_t) i;
            }
        }
    }
    if (utflDlg.done) return;
    uint64_t start = nowNanos();
    uint32_t loads = dw->codeStats.loads;
    uint32_t bytes = dw->codeStats.loadedBytes;
    bool all = true;
    for (uint32_t n = 0; UTFLDLG_COUNT > n; n++) {
        if (0 > utflDlg.ids[n]) continue;
        CodeEntry* code = &dw->code.entries[utflDlg.ids[n]];
        if (!code->present || code->bytecode != nullptr) continue;
        if (UTSTALL_CODE_HEAP_FREE > usHeapFree()) {
            all = false;
            continue;
        }
        DataWin_loadCodeBytes(dw, code);
    }
    utflDlg.done = all;
    if (dw->codeStats.loads != loads || !all) {
        logInfo(utflDlgLog, runner->currentRoom != nullptr ? runner->currentRoom->name : utflDlgNone, (unsigned) (dw->codeStats.loads - loads),
                (unsigned) ((dw->codeStats.loadedBytes - bytes + 1023u) / 1024u), (unsigned) ((nowNanos() - start) / 1000000u),
                all ? utflDlgNone : utflDlgShort);
    }
}
// ===[ change utflowey R4: a whole set's room load gets the time its big pages need ]===
// Item 5 gives a scene's whole set (or the start rule A keeps) UTSTALL_WHOLE_NANOS past the base deadline, and the room
// load's last pass reads a page's tiles from its cache file only if its estimate, 45 ms plus 1 ms a KB, fits what is
// left. Page 14 of the Omega Flowey fight (Flowey's body, 64 tiles, 4.1 MB) never fits 2.5 s, so with UNDERTALE Old's
// 24.3 MB budget (ctr_memory.c, 1a) and no packed copy of it, its tiles came from the SD card in the fight's first
// phases, in play (Azahar s4 run: 57 SD tiles in play against 23). Tile by tile it takes about 1.35 s on the o3DS
// (21 ms a tile). So the room load of a scene with a whole set runs to UTFL_R4_NANOS past the deadline it was given
// (maintainer, 10-20/10-21: a longer load at a transition is fine where it buys smooth play). The passes still stop once the
// set is in, at the budget less the headroom, or at the linear floor; a room with nothing left to read takes no longer.
// The round-16 wrap keeps its bytes as usFinish7 (tools/analysis/check_utflowey_wrap_move.py). Sections of its own,
// last in late_reserve.ld's lists, so that nothing linked before them moves.
#define UTFLR4_TEXT __attribute__((noinline, section(".text.late.utflr4")))
#define UTFLR4_CONST __attribute__((section(".rodata.utflr4_in_text")))
// The Old models only (review decision 2026-10-25: 1a + R4 for UNDERTALE Old); New passes the deadline on as it was.
#ifdef CTRPORT_TARGET_O3DS
#define UTFL_R4_NANOS (3000ull * 1000000ull)
#else
#define UTFL_R4_NANOS 0ull
#endif
static const char utflR4Log[] UTFLR4_CONST =
    "Room load (utflowey): %s: whole set's room load allowed %.0f ms more; %.0f ms in all, %u tiles resident (%u KB)\n";
// UNDERTALE Old: the whole set's passes go through sfxshed (file end), which calls usFinish7 and then sheds cached sound
// effects for any set tile linear memory stopped. Every other build calls usFinish7 itself.
#ifdef CTRPORT_TARGET_O3DS
__attribute__((noinline, noipa, section(".text_sfxshed"))) void utshedFinish(CtrTextureCache* cache, uint64_t deadline,
        uint32_t* nowTiles, uint32_t* otherTiles);
#define UTFL_R4_FINISH utshedFinish
#else
#define UTFL_R4_FINISH usFinish7
#endif
void utflR4Finish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
UTFLR4_TEXT bool utflWholeAll(CtrTextureCache* cache);
UTFLR4_TEXT void utflR4Finish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    bool more = UTFL_R4_NANOS > 0 && us.cache == cache && cache != nullptr && us.runner != nullptr && 0 <= us.room && us.whole && us.scene != nullptr;
    Runner* runner = more ? us.runner : nullptr;
    uint64_t start = nowNanos();
    bool all = more && utflWholeAll(cache); // the headroom waived for a set rule A cut that fits the budget itself
    if (all) cache->budgetBytes += UTSTALL_WHOLE_HEADROOM;
    UTFL_R4_FINISH(cache, more ? deadline + UTFL_R4_NANOS : deadline, nowTiles, otherTiles);
    if (all) cache->budgetBytes -= UTSTALL_WHOLE_HEADROOM;
    if (runner == nullptr) return;
    uint32_t tiles = 0;
    for (uint32_t p = 0; cache->pageCount > p; p++) tiles += cache->pages[p].residentTiles;
    logInfo(utflR4Log, runner->currentRoom != nullptr ? runner->currentRoom->name : "?", (double) UTFL_R4_NANOS / 1e6,
            (double) (nowNanos() - start) / 1e6, (unsigned) tiles, (unsigned) (cache->residentBytes / 1024));
}

// ===[ change utflowey: a whole set that fits the budget itself comes in whole (full-resolution Omega Flowey) ]===
// Rule A (item 5, usWholeFits) brings in the start of a scene's set that fits the budget less UTSTALL_WHOLE_HEADROOM
// (4 MB), the room left for tiles outside the set. The Omega Flowey fight's set is 132 tiles, 24.5 MB at full
// resolution: with UNDERTALE Old's 26.3 MB budget (ctr_renderer.c's reserve, ctr_memory.c's 1a) rule A cut it at
// about 22 MB, and the rest came in during the fight, evicting and reloading tiles (548 reloads at 20.3 MB). When rule
// A cut a set that fits the budget itself, the room load brings it all in: the R4 wrap above marks every tile of the
// set (us.wholeCount) and runs the room load with the budget raised by the headroom, so that making room, the marks
// and the passes stop at the budget itself; the linear floor still applies. The pages the set's tiles are on are
// learnt first, as usRoomLoadFinish does for the start rule A kept, so that each tile counts at its format's size
// (usTileBytes's rules, repeated). UNDERTALE Old only: a room whose set rule A didn't cut is left alone. review decision
// 2026-10-26 (flowfull review, option c). Only what loads when changes, never what is drawn.
#ifndef CTRPORT_TARGET_O3DS
UTFLR4_TEXT bool utflWholeAll(CtrTextureCache* cache) {
    (void) cache;
    return false;
}
#else
static const char utflAllLog[] UTFLR4_CONST =
    "Room load (utflowey): %s: whole set, all %u entries (rule A kept %u; %u tiles, %u KB) in a %u KB budget, the headroom waived\n";
UTFLR4_TEXT bool utflWholeAll(CtrTextureCache* cache) {
    RoomPlanScene s;
    memcpy(&s, us.scene, sizeof(s));
    if (s.tileCount <= us.wholeCount) return false;
    const uint8_t* entries = us.scene + sizeof(s);
    for (uint32_t i = 0; s.tileCount > i; i++) {
        uint32_t entry;
        memcpy(&entry, entries + (size_t) i * 4, 4);
        uint32_t pageId = UTSTALL_TILE_PAGE(entry);
        if (cache->pageCount <= pageId) continue;
        const CtrPage* page = &cache->pages[pageId];
        if (page->layoutKnown || page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING ||
            cache->dataWin->txtr.textures[pageId].externalPath != nullptr) continue;
        CtrTextures_acquire(cache, pageId);
    }
    uint64_t bytes = 0;
    uint32_t tiles = 0;
    for (uint32_t i = 0; s.tileCount > i; i++) {
        uint32_t entry;
        memcpy(&entry, entries + (size_t) i * 4, 4);
        uint32_t pageId = UTSTALL_TILE_PAGE(entry);
        int32_t t = UTSTALL_TILE_INDEX(entry);
        const CtrPage* page = cache->pageCount > pageId ? &cache->pages[pageId] : nullptr;
        uint64_t b = (uint64_t) CTR_TILE_MAX * CTR_TILE_MAX * 4u;
        if (page != nullptr && page->layoutKnown && !page->failed && page->tiles != nullptr && page->tilesX * page->tilesY > t) {
            const CtrTile* tile = &page->tiles[t];
            b = page->formatsKnown && tile->format == CTR_TILE_EMPTY ? 0
                : (uint64_t) page->tileW * page->tileH * (page->formatsKnown ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u);
        }
        bytes += b;
        tiles += b > 0;
    }
    if (bytes > cache->budgetBytes) return false;
    logInfo(utflAllLog, us.runner->currentRoom != nullptr ? us.runner->currentRoom->name : "?", (unsigned) s.tileCount,
            (unsigned) us.wholeCount, (unsigned) tiles, (unsigned) (bytes / 1024), (unsigned) (cache->budgetBytes / 1024));
    us.wholeCount = s.tileCount;
    us.wholeTiles = tiles;
    us.wholeBytes = (uint32_t) bytes;
    return true;
}
#endif

// ===[ change utflowey2: packed copies in spare linear memory (option 4; UNDERTALE Old only): removed 11-11 ]===
// The linear tier never ran on a console: it started only when the heap's packed share was above 0, and the UNDERTALE
// Old split (20/38) leaves the heap at exactly the game's need, so the share is 0 on every Old 3DS (review decision 11-11,
// letterdraw notes, "The utflowey2 linear tier on Old"). What is left are the five names the shared
// files and z_utstall.c's own calls reach through the CTR_PKLIN_* names (ctr_textures.h, ctr_texture_pack.h,
// ctr_wrap_chain.h), each a straight forward to the call it stood in for. They stay so that every existing caller
// compiles as before: only its call's target moves. noipa: a caller in this file compiles as against an outside
// function, as it did. CTRPORT_PKLIN itself stays: freewins L1 (ctr_memory.c, z_utstall_l2.c) keys on it.
#if defined(CTRPORT_PKLIN)
#ifndef CTRPORT_TARGET_O3DS
#error "CTRPORT_PKLIN is for UNDERTALE Old builds only"
#endif
// The block's includes as they were: later blocks of this file use their declarations.
#include "ctr_audio_system.h"
#include "ctr_memory_lock.h"
#include "ctr_wrap_chain.h"

#define UTFL2_ENTRY __attribute__((noipa, no_reorder, section(".text.utfl2_late"), optimize("Os")))
uint32_t __real_CtrAudioSystem_shedSfx(uint32_t bytes);

UTFL2_ENTRY struct mallinfo CtrPkLin_copyMallinfo(void) {
    return mallinfo();
}

UTFL2_ENTRY uint32_t CtrPkLin_copyHeapFree(void) {
    return usHeapFree();
}

UTFL2_ENTRY bool CtrPkLin_evictOldestIdle(CtrTextureCache* cache, uint32_t idleFrames) {
    return CtrTextures_evictOldestIdle(cache, idleFrames);
}

// The second link of the shedSfx chain (ctr_wrap_chain.h): the link before it calls this one.
UTFL2_ENTRY uint32_t CtrAudioSystem_chainPklin(uint32_t bytes) {
    return __real_CtrAudioSystem_shedSfx(bytes);
}

UTFL2_ENTRY void CtrPkLin_keepPlanned(CtrTextureCache* cache, uint32_t room) {
    usKeepPlanned(cache, room);
}
#line 1781
#endif
#endif

// ===[ change omegaflowey sfxshed: a whole set's room load sheds cached sound effects at the linear floor (Old only) ]===
// The room load's passes (ctr_textures.c) stop at the linear floor (LINEAR_FLOOR_BYTES free) without shedding anything,
// while the floor keeper in play sheds the cached sound effects first, as they are the quicker to load again. After a
// long session the sound cache holds more linear memory, so a whole set whose tiles fit the texture budget came in short:
// the Omega Flowey fight room entered a second time (sweep ut_flowey then ut_flowey6, Azahar Old and o2ds alike) loaded
// 66 of its 73 missing tiles with 4.33 MB of cached sounds (1.57 MB on the first entry: 44 of 44), and the 7 left came
// from the SD card in play, one each at their first draw (27-47 ms frames, a held image each). So once usFinish7 is done
// with a whole set (item 5), the set's tiles still missing are sorted by what the passes would do with them again:
// the set's missing tiles are marked as usRoomLoadFinish marks them, and CtrTextures_shedWalk (ctr_textures.c, beside the
// passes) goes through what the passes would load with memory no object, with their own page and tile conditions in their
// own order: the budget's 4 MiB headroom past pass 0 and its stop, a page's retry frame, the sources, pass 2's estimate of
// its time. Only when one of the set's tiles is among those, and linear memory is short for them (free below the floor plus
// their size), are the cached sound effects shed (the whole shedSfx chain, as the floor keeper sheds: ranked by reload
// class, size and last use, idle only; a shed sound plays as a stream until the next room change; the chain may also drop
// idle packed copies) for that shortfall plus a margin, and the passes run again if linear memory free went up (the chain's
// return counts only the sounds). The chain's own list keeps the room's first 8 planned sounds; for this shed and its
// passes every planned sound counts as just played (CtrAudioSystem_keepPlan, a plan too long to note: no shed), so a sound
// the room is about to play stays cached, and what can't be shed around them stays missing, as without this code. The
// budget, the passes and their checks are as before: the walk decides only whether to shed. UNDERTALE Old only: R4's call
// goes here instead of usFinish7 (its only change, the decoded call target); every other build calls usFinish7 as before.
// Inside ENABLE_WAD16, as the rest of the file: DELTARUNE compiles it empty.
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#define SFXSHED_TEXT __attribute__((noinline, noipa, section(".text_sfxshed")))
#define SFXSHED_CONST __attribute__((section(".rodata.sfxshed_in_text")))
#define SFXSHED_FLOOR (3u * 1024u * 1024u)                // ctr_textures.c's LINEAR_FLOOR_BYTES
#define SFXSHED_MARGIN (128u * 1024u)                      // each tile's linear allocation is aligned; a little over
#define SFXSHED_MINE_MAX 1024u                             // set entries it marks at most (RoomPlan_check: 512)
#define SFXSHED_PLAN_MAX 64u                               // planned sounds it can keep (the built plan: 24 at most)
static const char sfxshedLog[] SFXSHED_CONST =
    "Room load (sfxshed): %s: %u set tiles (%u KB) left, %u (%u KB) to load again at the linear floor (%u KB free); shed "
    "%u KB of sound effects (asked %u KB, %u KB freed); %u loaded, %u still left; %.0f ms; %u planned sounds kept\n";
void __real_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
void CtrTextures_shedWalk(const CtrTextureCache* cache, uint64_t deadline, const uint8_t* entries, uint32_t count, uint64_t* bytes,
                          uint32_t* tiles, bool* ofSet);
const uint8_t* UtSfx_planNow(uint32_t* count);
void CtrAudioSystem_keepPlan(const uint8_t* plan, uint32_t count, uint64_t* played);
void CtrAudioSystem_keepPlanEnd(const uint8_t* plan, uint32_t count, const uint64_t* played);
// The set's tile at entry i, or null (a tile of a page whose grid isn't known can't be marked, as usRoomLoadFinish skips it).
SFXSHED_TEXT static CtrTile* utshedTile(CtrTextureCache* cache, const uint8_t* entries, uint32_t i) {
    uint32_t entry;
    memcpy(&entry, entries + (size_t) i * 4, 4);
    uint32_t pageId = UTSTALL_TILE_PAGE(entry);
    int32_t t = UTSTALL_TILE_INDEX(entry);
    if (pageId >= cache->pageCount) return nullptr;
    CtrPage* page = &cache->pages[pageId];
    if (!page->layoutKnown || page->failed || page->tiles == nullptr || t >= page->tilesX * page->tilesY) return nullptr;
    CtrTile* tile = &page->tiles[t];
    return page->formatsKnown && tile->format == CTR_TILE_EMPTY ? nullptr : tile;
}

// As usRoomLoadFinish leaves them: a marked tile the passes didn't load is no longer wanted. The set's tiles still missing.
SFXSHED_TEXT static uint32_t utshedUnmark(CtrTextureCache* cache, const uint8_t* entries, uint32_t count, const uint32_t* mine) {
    uint32_t left = 0;
    for (uint32_t i = 0; count > i; i++) {
        CtrTile* tile = utshedTile(cache, entries, i);
        if (tile == nullptr || tile->valid) continue;
        if (SFXSHED_MINE_MAX > i && (mine[i / 32] & (1u << (i % 32))) != 0) tile->wanted = false;
        left++;
    }
    return left;
}

SFXSHED_TEXT void utshedFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
SFXSHED_TEXT void utshedFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    bool ours = us.cache == cache && cache != nullptr && us.runner != nullptr && 0 <= us.room && us.whole && us.scene != nullptr;
    Runner* runner = ours ? us.runner : nullptr;
    const uint8_t* scene = ours ? us.scene : nullptr;
    uint32_t count = ours ? us.wholeCount : 0;
    usFinish7(cache, deadline, nowTiles, otherTiles);
    if (scene == nullptr || nowNanos() >= deadline) return;
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    if (count > s.tileCount) count = s.tileCount;
    const uint8_t* entries = scene + sizeof(s);
    // The set's tiles still missing, a tile's own format's size, marked for the walk and the passes.
    uint64_t missing = 0;
    uint32_t tiles = 0;
    uint32_t mine[SFXSHED_MINE_MAX / 32] = {0}; // the set entries this marked, to unmark the ones not loaded
    for (uint32_t i = 0; count > i; i++) {
        CtrTile* tile = utshedTile(cache, entries, i);
        if (tile == nullptr || tile->valid) continue;
        uint32_t entry;
        memcpy(&entry, entries + (size_t) i * 4, 4);
        const CtrPage* page = &cache->pages[UTSTALL_TILE_PAGE(entry)];
        missing += (uint64_t) page->tileW * (uint64_t) page->tileH *
                   (page->formatsKnown ? CtrPack_texelBytes((CtrTileFormat) tile->format) : 4u);
        tiles++;
        if (tile->wanted || SFXSHED_MINE_MAX <= i) continue;
        tile->wanted = true;
        mine[i / 32] |= 1u << (i % 32);
    }
    if (tiles == 0) return;
    uint64_t eligible = 0;
    uint32_t eligibleTiles = 0;
    bool ofSet = false;
    CtrTextures_shedWalk(cache, deadline, entries, count, &eligible, &eligibleTiles, &ofSet);
    uint32_t freeBytes = linearSpaceFree();
    uint32_t planCount = 0;
    const uint8_t* plan = UtSfx_planNow(&planCount);
    // None of the set's tiles would load, or linear memory is enough for what would: the budget, the time or a retry frame
    // stopped the passes, not the floor. Or the room's sounds can't all be kept. Nothing is shed.
    if (!ofSet || freeBytes >= (uint64_t) SFXSHED_FLOOR + eligible || planCount > SFXSHED_PLAN_MAX) {
        utshedUnmark(cache, entries, count, mine);
        return;
    }
    uint64_t start = nowNanos();
    uint32_t ask = (uint32_t) ((uint64_t) SFXSHED_FLOOR + eligible + SFXSHED_MARGIN - freeBytes);
    uint64_t played[SFXSHED_PLAN_MAX];
    CtrAudioSystem_keepPlan(plan, planCount, played);
    uint32_t shed = CtrAudioSystem_shedSfx(ask);
    uint32_t after = linearSpaceFree();
    uint32_t freed = after > freeBytes ? after - freeBytes : 0;
    // Marked as usRoomLoadFinish marks them, so the passes take them as the room's (not drawn first).
    if (freed > 0 && nowNanos() < deadline) __real_CtrTextures_roomLoadFinish(cache, deadline, nowTiles, otherTiles);
    CtrAudioSystem_keepPlanEnd(plan, planCount, played);
    uint32_t left = utshedUnmark(cache, entries, count, mine);
    uint32_t loaded = tiles > left ? tiles - left : 0;
    logInfo(sfxshedLog, runner->currentRoom != nullptr ? runner->currentRoom->name : "?", (unsigned) tiles,
            (unsigned) (missing / 1024), (unsigned) eligibleTiles, (unsigned) (eligible / 1024), (unsigned) (freeBytes / 1024),
            (unsigned) (shed / 1024), (unsigned) (ask / 1024), (unsigned) (freed / 1024), (unsigned) loaded, (unsigned) left,
            (double) (nowNanos() - start) / 1e6, (unsigned) planCount);
}
#if defined(CTRPORT_TILENAMES) && defined(CTRPORT_TARGET_O3DS)
// ===[ tilenames (change tilemiss, 2026-11-08): a log line for each tile a draw reads from the SD card ]===
//
// A tile that a draw finds missing is read on the game thread, in play (the "Textures: page ... from SD cache" line).
// The plan can only take such a tile into the room load once it is named, so each one gets a line after the read: its
// page and tile, the room (and battle group) and the frame of the room visit, whether the plan scene loaded for this
// visit has it (and its frame bucket), whether it had texture memory before (C3D_TexDelete leaves a 2D texture's size
// as it was, and a page's tiles start zeroed, so a nonzero size means it was in memory and was evicted), how long ago
// it was last used, and the instance whose draw asked (object, sprite and image, the event and the depth). Numbers
// only: the names are looked up from them. At most TN_ROOM_LINES lines a room visit; the rest are counted in a line at
// the next read in a later room. Only draws that read from the card log anything; a draw whose tiles are all in memory
// costs one pass over its tile range's flags.
//
// ctr_textures.c's entryload wraps call these in place of the real ensure functions (CMakeLists.txt, by name: their
// code is the same size), and these call the real ones. Late code, constants and statics in their own sections
// (late_reserve.ld): nothing before them moves. Nothing here is called from the file's other code, and nothing of it
// is called from here, so no inlining or clone of the file's code changes.
#define TN_TEXT __attribute__((noinline, section(".text.tilenames_late")))
#define TN_CONST __attribute__((aligned(4), section(".rodata.tilenames_in_text")))
#define TN_BSS __attribute__((section(".bss.tilenames_late")))
#define TN_SCAN_MAX 8u    // missing tiles kept per draw
#define TN_ROOM_LINES 16u // lines per room visit

void __real_CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                       uint64_t drawnElsewise);
void __real_CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
void CtrTilenames_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                 uint64_t drawnElsewise) TN_TEXT;
void CtrTilenames_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1) TN_TEXT;

static TN_BSS struct {
    const CtrTextureCache* cache; // the room visit the counts are for
    uint32_t roomStart;
    uint32_t lines, more;
} tn;

static const char tnLine[] TN_CONST =
    "Textures: in-play read: page %u tile %u, room %ld group %ld, frame %u of the room; plan: %s; %s; asked by object %ld "
    "(sprite %ld image %ld, event %ld/%ld, depth %ld)\n";
static const char tnMore[] TN_CONST = "Textures: in-play reads: %u more in the room before, not listed\n";
static const char tnNoScene[] TN_CONST = "no scene";
static const char tnNo[] TN_CONST = "no";
static const char tnLater[] TN_CONST = "later";
static const char tnAt[] TN_CONST = "frame %u";
static const char tnFirst[] TN_CONST = "first time in memory";
static const char tnAsked[] TN_CONST = "never in memory, asked for %u frames before";
static const char tnEvicted[] TN_CONST = "evicted, last used %u frames before (%s)";
static const char tnIdle[] TN_CONST = "evicted, loaded and never used";
static const char tnThis[] TN_CONST = "this room";
static const char tnEarlier[] TN_CONST = "an earlier room";

typedef struct {
    uint32_t tile[TN_SCAN_MAX];
    uint32_t last[TN_SCAN_MAX]; // lastUsedFrame before the draw
    bool had[TN_SCAN_MAX];      // texture memory before the draw
    uint32_t count;
} TnMissing;

// The missing tiles of the range the draw loads (its drawnElsewise ones left out, as the real function does).
static TN_TEXT void tnScan(const CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, uint64_t drawnElsewise,
                           TnMissing* m) {
    m->count = 0;
    if (page->tiles == nullptr) return;
    uint32_t k = 0;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++, k++) {
            if (64 > k && (drawnElsewise >> k & 1u) != 0) continue;
            if (0 > tx || 0 > ty || tx >= page->tilesX || ty >= page->tilesY) continue;
            uint32_t t = (uint32_t) (ty * page->tilesX + tx);
            const CtrTile* tile = &page->tiles[t];
            if (tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY) || m->count >= TN_SCAN_MAX) continue;
            m->tile[m->count] = t;
            m->last[m->count] = tile->lastUsedFrame;
            m->had[m->count] = tile->tex.size != 0;
            m->count++;
        }
    }
}

static TN_TEXT void tnLog(const CtrTextureCache* cache, const CtrPage* page, const TnMissing* m) {
    if (tn.cache != cache || tn.roomStart != cache->roomStartFrame) {
        if (tn.more > 0) logInfo(tnMore, (unsigned) tn.more);
        tn.cache = cache;
        tn.roomStart = cache->roomStartFrame;
        tn.lines = 0;
        tn.more = 0;
    }
    uint32_t pageId = (uint32_t) (page - cache->pages);
    Runner* runner = us.runner;
    const uint8_t* scene = runner != nullptr && 0 <= us.room && runner->currentRoomIndex == us.room ? us.scene : nullptr;
    const VMContext* ctx = runner != nullptr ? runner->vmContext : nullptr;
    const Instance* inst = ctx != nullptr && ctx->currentEventType == EVENT_DRAW ? ctx->currentInstance : nullptr;
    for (uint32_t i = 0; m->count > i; i++) {
        uint32_t t = m->tile[i];
        if (!page->tiles[t].valid) continue; // not loaded by this draw
        if (tn.lines >= TN_ROOM_LINES) {
            tn.more++;
            continue;
        }
        tn.lines++;
        char planAt[16];
        char pastText[64];
        const char* plan = tnNoScene;
        const char* past = pastText;
        if (scene != nullptr) {
            RoomPlanScene s;
            memcpy(&s, scene, sizeof(s));
            uint32_t key = pageId << 8 | t;
            int32_t bucket = -1;
            for (uint32_t e = 0; s.tileCount > e && 0 > bucket; e++) {
                uint32_t entry;
                memcpy(&entry, scene + sizeof(s) + (size_t) e * 4u, 4);
                if ((entry & 0x07FFFFFFu) == key) bucket = scene[sizeof(s) + (size_t) s.tileCount * 4u + e];
            }
            if (0 > bucket) {
                plan = tnNo;
            } else if (bucket == 255) {
                plan = tnLater;
            } else {
                snprintf(planAt, sizeof(planAt), tnAt, (unsigned) bucket);
                plan = planAt;
            }
        }
        uint32_t last = m->last[i];
        uint32_t ago = cache->frame > last ? cache->frame - last : 0;
        if (!m->had[i] && last == 0) past = tnFirst;
        else if (!m->had[i]) snprintf(pastText, sizeof(pastText), tnAsked, (unsigned) ago);
        else if (last == 0) past = tnIdle;
        else snprintf(pastText, sizeof(pastText), tnEvicted, (unsigned) ago, last >= cache->roomStartFrame ? tnThis : tnEarlier);
        logInfo(tnLine, (unsigned) pageId, (unsigned) t, (long) (runner != nullptr ? runner->currentRoomIndex : -1),
                (long) (scene != nullptr ? us.group : -1), (unsigned) (cache->frame - cache->roomStartFrame), plan, past,
                (long) (inst != nullptr ? inst->objectIndex : -1), (long) (inst != nullptr ? inst->spriteIndex : -1),
                (long) (inst != nullptr ? (int32_t) inst->imageIndex : -1), (long) (ctx != nullptr ? ctx->currentEventType : -1),
                (long) (ctx != nullptr ? ctx->currentEventSubtype : -1), (long) (runner != nullptr ? runner->drawDepth : 0));
    }
}

// After a draw whose page had no tile table yet: the tiles it loaded, all first time in memory.
static TN_TEXT void tnFresh(const CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, TnMissing* m) {
    m->count = 0;
    if (page->tiles == nullptr) return;
    for (int32_t ty = ty0; ty1 >= ty; ty++) {
        for (int32_t tx = tx0; tx1 >= tx; tx++) {
            if (0 > tx || 0 > ty || tx >= page->tilesX || ty >= page->tilesY || m->count >= TN_SCAN_MAX) continue;
            uint32_t t = (uint32_t) (ty * page->tilesX + tx);
            if (!page->tiles[t].valid) continue;
            m->tile[m->count] = t;
            m->last[m->count] = 0;
            m->had[m->count] = false;
            m->count++;
        }
    }
}

static TN_TEXT void tnEnsure(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                             uint64_t drawnElsewise, bool but) {
    TnMissing m;
    bool fresh = page->tiles == nullptr;
    tnScan(page, tx0, ty0, tx1, ty1, drawnElsewise, &m);
    if (m.count == 0 && !fresh) {
        if (but) __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
        else __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
        return;
    }
    uint32_t cardLoads = cache->fromFile.count;
    if (but) __real_CtrTextures_ensureTilesBut(cache, page, tx0, ty0, tx1, ty1, drawnElsewise);
    else __real_CtrTextures_ensureTiles(cache, page, tx0, ty0, tx1, ty1);
    if (cache->fromFile.count == cardLoads || cache->filling) return;
    if (fresh) tnFresh(page, tx0, ty0, tx1, ty1, &m);
    tnLog(cache, page, &m);
}

void CtrTilenames_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1,
                                 uint64_t drawnElsewise) {
    tnEnsure(cache, page, tx0, ty0, tx1, ty1, drawnElsewise, true);
}

void CtrTilenames_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1) {
    tnEnsure(cache, page, tx0, ty0, tx1, ty1, 0, false);
}
#endif
#endif

#if defined(ENABLE_WAD16)
// ===[ bootahead (first update, change bootmask, review decision 11-07): the menu level's tiles behind the loading bar ]===
// The story's room load is the first of a boot (just after the loading bar, its picture still the full bar). It names
// the menu's song to the music spare (z_bootahead.c, which works the menu's level out from the save files the way the
// menu's Create does), and after its own set it loads the plan's tiles of that level's menu picture (room 3's scene
// with sub BOOTAHEAD_LEVEL_SUB + level, tools/roomplan/build_plan.py's menu_levels): the level-6 picture's sprites
// were loaded at draw time, a shown frame of 83-109 ms on Azahar. Levels 0, 1 and 7 draw nothing more. The tiles
// stay resident through the title, as the story's set does; the budget's headroom is kept. Only what loads when
// changes. R4's wrap keeps its slot and bytes as utflR4Finish; this one is boot-only code in UNDERTALE's retired AAC
// slot (aacout.cmake), with its constants, and keeps its marks on the stack
// (tools/analysis/check_bootahead_wrap_move.py).
#define BOOTAHEAD_TEXT __attribute__((noinline, section(".aacout.text.bootahead")))
#define BOOTAHEAD_CONST __attribute__((aligned(4), section(".aacout.rodata.bootahead")))
#define BOOTAHEAD_MARKS 64u
#define BOOTAHEAD_NANOS (1500ull * 1000000ull)
#define BOOTAHEAD_LEVEL_SUB 0x8000u // build_plan.py's MENU_LEVEL_SUB: no loader takes these scenes
int CtrBootahead_begin(Runner* runner);
static const char baTilesLog[] BOOTAHEAD_CONST = "Boot ahead: menu level %d, %u tiles marked, %u loaded, %u left, %u KB resident, %.0f ms\n";

// Marks the scene's missing tiles the budget less the headroom has room for (each one's page and tile in baPage, baTile).
static BOOTAHEAD_TEXT uint32_t baMark(CtrTextureCache* cache, const uint8_t* found, uint16_t* baPage, uint8_t* baTile) {
    RoomPlanScene scene;
    memcpy(&scene, found, sizeof(scene));
    const uint8_t* tiles = found + sizeof(scene);
    uint64_t bytes = cache->residentBytes;
    uint32_t marks = 0;
    for (uint32_t i = 0; scene.tileCount > i && BOOTAHEAD_MARKS > marks; i++) {
        uint32_t entry;
        memcpy(&entry, tiles + (size_t) i * 4, 4);
        uint32_t pageId = UTSTALL_TILE_PAGE(entry);
        int32_t t = UTSTALL_TILE_INDEX(entry);
        if (pageId >= cache->pageCount || pageId > UINT16_MAX) continue;
        CtrPage* page = &cache->pages[pageId];
        if (!page->layoutKnown && !page->failed && page->cacheFile != CTR_CACHE_FILE_MISSING && cache->dataWin->txtr.textures[pageId].externalPath == nullptr)
            CtrTextures_acquire(cache, pageId);
        if (!page->layoutKnown || page->failed || page->tiles == nullptr || t >= page->tilesX * page->tilesY) continue;
        CtrTile* tile = &page->tiles[t];
        if (tile->wanted || tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) continue;
        bytes += usTileBytes(page, t);
        if (bytes + UTSTALL_WHOLE_HEADROOM > (uint64_t) cache->budgetBytes) break;
        tile->wanted = true;
        baPage[marks] = (uint16_t) pageId;
        baTile[marks] = (uint8_t) t;
        marks++;
    }
    return marks;
}

static BOOTAHEAD_TEXT void baLevelTiles(CtrTextureCache* cache, int level, uint32_t* nowTiles, uint32_t* otherTiles) {
    const uint8_t* plan = RoomPlan_utstallPlan(cache);
    const uint8_t* found = plan != nullptr && 2 <= level && 6 >= level ? usFindScene(plan, 3u, BOOTAHEAD_LEVEL_SUB + (uint32_t) level) : nullptr;
    if (found == nullptr) return;
    uint64_t start = nowNanos();
    uint16_t baPage[BOOTAHEAD_MARKS];
    uint8_t baTile[BOOTAHEAD_MARKS];
    uint32_t marks = baMark(cache, found, baPage, baTile);
    if (marks > 0) __real_CtrTextures_roomLoadFinish(cache, nowNanos() + BOOTAHEAD_NANOS, nowTiles, otherTiles);
    uint32_t loaded = 0, left = 0;
    for (uint32_t i = 0; marks > i; i++) {
        CtrTile* tile = &cache->pages[baPage[i]].tiles[baTile[i]];
        if (tile->valid) {
            loaded++;
        } else if (tile->wanted) {
            tile->wanted = false;
            left++;
        }
    }
    logInfo(baTilesLog, level, (unsigned) marks, (unsigned) loaded, (unsigned) left, (unsigned) (cache->residentBytes / 1024),
            (double) (nowNanos() - start) / 1e6);
}

void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
BOOTAHEAD_TEXT void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    // The room load's own state doesn't outlast it (usFinish7): the story's room is taken first.
    bool story = us.cache == cache && cache != nullptr && us.runner != nullptr && us.room == 1;
    int level = story ? CtrBootahead_begin(us.runner) : 0;
    utflR4Finish(cache, deadline, nowTiles, otherTiles);
    if (story) baLevelTiles(cache, level, nowTiles, otherTiles);
}
#endif
