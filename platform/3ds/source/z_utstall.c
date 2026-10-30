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
static UTSTALL_TEXT bool usMakeRoom(CtrTextureCache* cache, const uint32_t* pages, uint32_t count, uint32_t size, uint32_t reserve,
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

// utflowey (integrate17's flowey second run): in a room scene rule A cut, the record's page that holds the most of
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
        // A small copy stays (utflowey): it frees little room, and a tile of the page the plan doesn't list would
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
        CTR_PKLIN_KEEP_PLANNED(cache, (uint32_t) runner->currentRoomIndex);
    }
    utflDialogueCode(runner); __real_CtrRoomload_prefetchRoom(cache, runner);
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

UTSTALL7_TEXT void usFinish7(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    Runner* runner = us.cache == cache && cache != nullptr && 0 <= us.room ? us.runner : nullptr;
    usRoomLoadFinish(cache, deadline, nowTiles, otherTiles);
    if (runner != nullptr) usReadCode(runner);
}

// ===[ utflowey: in play, a cut scene's copy-backed tiles look older than they are ]===
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
// Packed-tier fix (utflowey, round 18; UNDERTALE). On a 64 MB Old 3DS the heap has 4-6 MB free in play, so a
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

// ===[ utflowey: the dialogue's code, read in the hidden room load ]===
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
// ===[ utflowey R4: a whole set's room load gets the time its big pages need ]===
// Item 5 gives a scene's whole set (or the start rule A keeps) UTSTALL_WHOLE_NANOS past the base deadline, and the room
// load's last pass reads a page's tiles from its cache file only if its estimate, 45 ms plus 1 ms a KB, fits what is
// left. Page 14 of the Omega Flowey fight (Flowey's body, 64 tiles, 4.1 MB) never fits 2.5 s, so with UNDERTALE Old's
// 24.3 MB budget (ctr_memory.c, 1a) and no packed copy of it, its tiles came from the SD card in the fight's first
// phases, in play (Azahar s4 run: 57 SD tiles in play against 23). Tile by tile it takes about 1.35 s on the o3DS
// (21 ms a tile). So the room load of a scene with a whole set runs to UTFL_R4_NANOS past the deadline it was given
// (decided 10-20/10-21: a longer load at a transition is fine where it buys smooth play). The passes still stop once the
// set is in, at the budget less the headroom, or at the linear floor; a room with nothing left to read takes no longer.
// The round-16 wrap keeps its bytes as usFinish7 (tools/analysis/check_utflowey_wrap_move.py). Sections of its own,
// last in late_reserve.ld's lists, so that nothing linked before them moves.
#define UTFLR4_TEXT __attribute__((noinline, section(".text.late.utflr4")))
#define UTFLR4_CONST __attribute__((section(".rodata.utflr4_in_text")))
// The Old models only (ruling 2026-10-25: 1a + R4 for UNDERTALE Old); New passes the deadline on as it was.
#ifdef CTRPORT_TARGET_O3DS
#define UTFL_R4_NANOS (3000ull * 1000000ull)
#else
#define UTFL_R4_NANOS 0ull
#endif
static const char utflR4Log[] UTFLR4_CONST =
    "Room load (utflowey): %s: whole set's room load allowed %.0f ms more; %.0f ms in all, %u tiles resident (%u KB)\n";
void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);
UTFLR4_TEXT bool utflWholeAll(CtrTextureCache* cache);
UTFLR4_TEXT void __wrap_CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles) {
    bool more = UTFL_R4_NANOS > 0 && us.cache == cache && cache != nullptr && us.runner != nullptr && 0 <= us.room && us.whole && us.scene != nullptr;
    Runner* runner = more ? us.runner : nullptr;
    uint64_t start = nowNanos();
    bool all = more && utflWholeAll(cache); // the headroom waived for a set rule A cut that fits the budget itself
    if (all) cache->budgetBytes += UTSTALL_WHOLE_HEADROOM;
    usFinish7(cache, more ? deadline + UTFL_R4_NANOS : deadline, nowTiles, otherTiles);
    if (all) cache->budgetBytes -= UTSTALL_WHOLE_HEADROOM;
    if (runner == nullptr) return;
    uint32_t tiles = 0;
    for (uint32_t p = 0; cache->pageCount > p; p++) tiles += cache->pages[p].residentTiles;
    logInfo(utflR4Log, runner->currentRoom != nullptr ? runner->currentRoom->name : "?", (double) UTFL_R4_NANOS / 1e6,
            (double) (nowNanos() - start) / 1e6, (unsigned) tiles, (unsigned) (cache->residentBytes / 1024));
}

// ===[ utflowey: a whole set that fits the budget itself comes in whole (full-resolution Omega Flowey) ]===
// Rule A (item 5, usWholeFits) brings in the start of a scene's set that fits the budget less UTSTALL_WHOLE_HEADROOM
// (4 MB), the room left for tiles outside the set. The Omega Flowey fight's set is 132 tiles, 24.5 MB at full
// resolution: with UNDERTALE Old's 26.3 MB budget (ctr_renderer.c's reserve, ctr_memory.c's 1a) rule A cut it at
// about 22 MB, and the rest came in during the fight, evicting and reloading tiles (548 reloads at 20.3 MB). When rule
// A cut a set that fits the budget itself, the room load brings it all in: the R4 wrap above marks every tile of the
// set (us.wholeCount) and runs the room load with the budget raised by the headroom, so that making room, the marks
// and the passes stop at the budget itself; the linear floor still applies. The pages the set's tiles are on are
// learnt first, as usRoomLoadFinish does for the start rule A kept, so that each tile counts at its format's size
// (usTileBytes's rules, repeated). UNDERTALE Old only: a room whose set rule A didn't cut is left alone. Ruling
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

// ===[ utflowey2: packed copies in spare linear memory (option 4; UNDERTALE Old only) ]===
// Option 1 (ruling 2026-10-26: linear 38 MB, heap 20.1 MB on UNDERTALE Old) gave Omega Flowey's whole set all of
// linear memory, but the heap's share for packed copies fell to 0.1 MB (the heap less the game's measured need, above),
// so pages A19 kept as copies (19, 21, 4: 0.1-0.5 MB each) came from the SD card in play instead, 43-59 ms a tile, most
// of it the file's open (bf_papyrus's fog room, bf_undyne, bf_sans, bf_asriel: one held frame each). Here a packed copy
// has a second home: linear memory the room's plan leaves spare. One budget for both (cache->packedBudgetBytes): the
// heap share first, then the linear share, which each room load sets to the texture budget less the plan scene's tiles
// (usTileBytes's sizes, the whole set however much of it rule A keeps), less the headroom rule A leaves for other tiles
// (UTSTALL_WHOLE_HEADROOM), at most PL_LIN_MAX (A19's share, 6.1 MB, was enough for every scene above). The linear
// share is lent from the texture budget (CtrTextures_lendBudgetInRoom, as palette copies borrow it: tiles idle since
// the room began make room), so tiles and copies never hold more linear memory than the texture budget alone did, and
// the room's planned tiles and the headroom always fit beside the copies. The Omega Flowey fight's set leaves no share:
// its room load drops every linear copy and gives the whole budget back, as option 1 had it.
//   Where the copy goes: the heap if the heap's copies stay within its share and PL_HEAP_RESERVE stays free, else
// linear if the linear copies stay within theirs and PL_FLOOR + PL_MARGIN of linear memory stays free; otherwise as
// before (the heap, under each caller's own check). Both paths that make copies in play decide so: the game thread's
// reads (z_utstall.c's items 4 and 6, ctr_textures.c's: the CtrPack_readFile wrap) and the reader thread's promotions
// (the CtrPacked_new wrap). Their heap checks count the linear room as free heap (CtrPkLin_copyMallinfo), so a copy that
// fits linear memory is no longer turned away by a heap that can't hold it. The background worker's startup reads
// (ctr_texture_pack.c's findExisting, unchanged: its call is inside its own file) would fill the heap past its share
// under the larger budget, so they stop once the shares are set, as A19's 0.1 MB budget stopped them at the first page.
//   Going first: at each room load, linear copies over the new share (other pages' before the scene's); when linear
// memory runs short, the floor keeper's shed (after the cached sound effects, before any tile) and a tile allocation
// that failed (before any tile). The low-heap relief drops only copies in the heap and keeps the linear share: it runs
// each frame here, ahead of beginFrame's check (relieveHeap, every 30 frames, unchanged), which so finds the heap over
// its 3 MB unless the game itself holds it under (then relieveHeap drops every copy and cuts the budget, as before;
// the next frame restores the shares).
//   A promotion read in flight on the reader thread holds a reference to its copy. A room load that needs its room
// marks it dead; the read stops at its next 64 KB and fails, and the last reference frees it (CtrPacked_free's wrap),
// so nothing is freed under a reader. Linear copies are never read by the GPU, so their memory is freed at once.
// UNDERTALE Old only (CMakeLists.txt defines CTRPORT_PKLIN there and links the wraps); every other build compiles the
// shared files' original calls. Late code, constants and statics only, in sections of their own (late_reserve.ld).
#if defined(CTRPORT_PKLIN)
#ifndef CTRPORT_TARGET_O3DS
#error "CTRPORT_PKLIN is for UNDERTALE Old builds only"
#endif
#include "ctr_audio_system.h"
#include "ctr_memory_lock.h"
#include "ctr_wrap_chain.h"

// noipa (the calls from existing code): their callers compile exactly as against an outside function. no_reorder: the
// block's functions are emitted in source order, apart from the compiler's call-graph order of the existing ones.
#define UTFL2_TEXT __attribute__((noinline, no_reorder, section(".text.utfl2_late"), optimize("Os")))
#define UTFL2_ENTRY __attribute__((noipa, no_reorder, section(".text.utfl2_late"), optimize("Os")))
#define UTFL2_CONST __attribute__((aligned(4), section(".rodata.utfl2_in_text")))
#define UTFL2_BSS __attribute__((section(".bss.utfl2_late")))
#define PL_MB (1024u * 1024u)
#define PL_FLOOR (3u * PL_MB)           // ctr_textures.c's LINEAR_FLOOR_BYTES: a linear copy leaves it free, and this more:
#define PL_MARGIN (512u * 1024u)
#define PL_LIN_MAX (6u * PL_MB)         // the linear share at most (A19's whole packed budget was 6.1 MB)
#define PL_HEAP_RESERVE (4u * PL_MB)    // a copy in the heap leaves this free (the reader's promotion reserve, Old)
#define PL_READER_HEAP (3u * PL_MB)     // ctr_tile_reader.c's READER_WHOLE_START_RESERVE on UNDERTALE Old
#define PL_LOW_HEAP (3u * PL_MB)        // ctr_textures.c's LOW_HEAP_BYTES
#define PL_LEND_STEP (512u * 1024u)
#define PL_LEND_STEPS 4u                // lent per frame at most: 2 MB
#define PL_LEND_IDLE 60u                // frames, as palette copies lend
#define PL_TRIM_SLACK (256u * 1024u)
#define PL_FLIGHTS 4u
#define PL_MAX_FILE (64u * PL_MB)       // ctr_texture_pack.c's PACK_MAX_FILE_SIZE
#define PL_CENSUS_MAX 24u
#define PL_CENSUS_MIN (64u * 1024u)
#ifdef CTRPORT_PKLIN_STRESS
#define PL_STRESS_NANOS (250ll * 1000000ll) // stress builds: each piece of a promotion read waits this long
#endif

extern u32 __ctru_linear_heap;
extern u32 __ctru_linear_heap_size;
extern LightLock CtrMemory_linearLockStore;
void __real_linearFree(void* mem);
void* __real_linearMemAlign(size_t size, size_t alignment);
u32 __real_linearSpaceFree(void);
CtrPacked* __real_CtrPacked_new(const CtrPackView* view);
void __real_CtrPacked_free(CtrPacked* packed);
bool __real_CtrPacked_read(CtrPacked* packed, FILE* f, uint32_t chunk);
uint32_t __real_CtrAudioSystem_shedSfx(uint32_t bytes);
CtrPacked* __wrap_CtrPacked_new(const CtrPackView* view);
void __wrap_CtrPacked_free(CtrPacked* packed);
bool __wrap_CtrPacked_read(CtrPacked* packed, FILE* f, uint32_t chunk);
CtrPacked* __wrap_CtrPack_readFile(FILE* f, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize);

typedef struct {
    CtrPacked* packed; // a linear copy a promotion read is filling, or nullptr
    uint32_t refs;     // the reader's while it reads
    uint32_t size;
    bool dead;         // a room load wants its memory: the read stops and fails
} PlFlight;

static UTFL2_BSS struct {
    CtrTextureCache* cache;  // the cache the rest is for (set once utflPk has sized the heap share)
    bool on;                 // copies at all (a packed budget at startup)
    bool lock;               // the flight table's
    uint32_t linLive;        // every linear copy's bytes, attached, being read or not yet taken (atomic)
    uint32_t linBudget;      // the linear share
    uint32_t heapBudget;     // the heap share
    uint32_t heapHeld;       // the heap copies' bytes, as last counted on the game thread
    uint32_t lent;           // of the texture budget, for the linear share
    uint32_t lastSet;        // the packed budget as last set here
    uint32_t setBytes;       // the room's plan, for the log
    PlFlight flight[PL_FLIGHTS];
    uint32_t made;           // linear copies made since the last room load's log
    uint32_t madeBytes;
    uint32_t dropped;        // linear copies' bytes dropped since then
    uint32_t stopped;        // promotion reads stopped by a room load
} pl;

static const char plInitLog[] UTFL2_CONST =
    "Textures (pklin): packed copies in spare linear memory: heap share %u KB, linear share %u KB (at most %u KB)\n";
static const char plCutLog[] UTFL2_CONST = "Textures (pklin): packed budget cut to %u KB elsewhere: heap share now %u KB\n";
static const char plRoomLog[] UTFL2_CONST =
    "Textures (pklin): %s: linear share %u KB (plan %u KB of a %u KB texture budget), %u KB lent; linear copies %u KB, heap %u KB of %u KB; "
    "since the last room load %u linear copies made (%u KB), %u KB dropped, %u reads stopped%s\n";
static const char plRefitLog[] UTFL2_CONST = "; rule A looked again: %u of the set's entries";
static const char plNone[] UTFL2_CONST = "";
static const char plUnknown[] UTFL2_CONST = "?";
static const char plLowLog[] UTFL2_CONST =
    "Textures (pklin): heap low (%u KB free), dropped %u KB of packed pages in the heap; heap share now %u KB, linear share %u KB\n";
static const char plStopLog[] UTFL2_CONST = "Textures (pklin): a promotion read stopped for a room load at %u of %u KB\n";
#ifdef CTRPORT_PKLIN_DIAG
static const char plCensusHead[] UTFL2_CONST = "Textures (pklin): %s linear census: %u KB free, largest blocks (KB):";
static const char plCensusItem[] UTFL2_CONST = " %u";
static const char plCensusEnd[] UTFL2_CONST = "%s\n";
static const char plCensusBusy[] UTFL2_CONST = "Textures (pklin): %s linear census: lock busy\n";
static const char plCheckLog[] UTFL2_CONST =
    "Textures (pklin): accounting: linear copies counted %u KB, attached %u KB, in flight %u KB (worker results not yet taken: the rest)\n";
#endif

static UTFL2_TEXT void plLock(void) {
    while (__atomic_test_and_set(&pl.lock, __ATOMIC_ACQUIRE)) svcSleepThread(50000);
}

static UTFL2_TEXT void plUnlock(void) {
    __atomic_clear(&pl.lock, __ATOMIC_RELEASE);
}

static UTFL2_TEXT bool plIsLinear(const void* p) {
    uint32_t at = (uint32_t) p;
    return __ctru_linear_heap <= at && __ctru_linear_heap + __ctru_linear_heap_size > at;
}

static UTFL2_TEXT uint32_t plLive(void) {
    return __atomic_load_n(&pl.linLive, __ATOMIC_RELAXED);
}

// Linear copies are never read by the GPU: their memory goes back at once, not after the GPU's frame.
static UTFL2_TEXT void plFreeBlock(void* p) {
    CtrMemory_linearLockAcquire(&CtrMemory_linearLockStore);
    __real_linearFree(p);
    CtrMemory_linearLockRelease(&CtrMemory_linearLockStore);
}

static UTFL2_TEXT void plFreeLinear(CtrPacked* packed) {
    uint32_t size = packed->size;
    for (uint32_t i = 0; packed->pieceCount > i; i++) {
        if (packed->pieces[i] != nullptr) plFreeBlock(packed->pieces[i]);
    }
    plFreeBlock(packed);
    __atomic_sub_fetch(&pl.linLive, size, __ATOMIC_RELAXED);
}

// CtrPacked_new's pieces, in linear memory, its tables too (so where a copy lives shows in its pointer).
static UTFL2_TEXT CtrPacked* plNewLinear(const CtrPackView* view) {
    uint32_t starts[CTR_PAGE_MAX_TILES + 1];
    uint32_t count = 1;
    starts[0] = 0;
    int32_t tiles = view->layout.tilesX * view->layout.tilesY;
    for (int32_t i = 0; tiles > i && CTR_PAGE_MAX_TILES > i; i++) {
        uint32_t at = view->tileOffset[i];
        if (view->tileSize[i] == 0 || starts[count - 1] >= at) continue;
        if (at + view->tileSize[i] - starts[count - 1] > CTR_PACKED_PIECE_BYTES) starts[count++] = at;
    }
    CtrPacked* packed = CtrMemory_linearAllocOnce(sizeof(CtrPacked) + (size_t) count * (sizeof(uint32_t) + sizeof(uint8_t*)));
    if (packed == nullptr) return nullptr;
    packed->size = view->totalSize;
    packed->pieceCount = count;
    packed->starts = (uint32_t*) (packed + 1);
    packed->pieces = (uint8_t**) (packed->starts + count);
    memcpy(packed->starts, starts, count * sizeof(uint32_t));
    for (uint32_t i = 0; count > i; i++) packed->pieces[i] = nullptr;
    __atomic_add_fetch(&pl.linLive, packed->size, __ATOMIC_RELAXED);
    for (uint32_t i = 0; count > i; i++) {
        uint32_t end = count > i + 1 ? starts[i + 1] : packed->size;
        packed->pieces[i] = CtrMemory_linearAllocOnce(end - starts[i]);
        if (packed->pieces[i] == nullptr) {
            plFreeLinear(packed);
            return nullptr;
        }
    }
    pl.made++;
    pl.madeBytes += packed->size;
    return packed;
}

// What a copy may still take of linear memory: the share's rest, as far as the floor and margin allow.
static UTFL2_TEXT uint32_t plLinRoom(void) {
    uint32_t live = plLive();
    if (!pl.on || live >= pl.linBudget) return 0;
    uint32_t room = pl.linBudget - live;
    uint32_t freeBytes = linearSpaceFree();
    uint32_t phys = freeBytes > PL_FLOOR + PL_MARGIN ? freeBytes - PL_FLOOR - PL_MARGIN : 0;
    return room < phys ? room : phys;
}

// Whether a copy of `size` bytes goes to linear memory (else the heap, under the caller's own check).
static UTFL2_TEXT bool plWantsLinear(uint32_t size) {
    if (!pl.on) return false;
    if (pl.heapHeld + size <= pl.heapBudget && usHeapFree() >= size + PL_HEAP_RESERVE) return false;
    return plLinRoom() >= size;
}

UTFL2_TEXT struct mallinfo CtrPkLin_copyMallinfo(void) {
    struct mallinfo mi = mallinfo();
    uint32_t room = plLinRoom();
    mi.uordblks = mi.uordblks > room ? mi.uordblks - room : 0;
    return mi;
}

UTFL2_ENTRY uint32_t CtrPkLin_copyHeapFree(void) {
    return usHeapFree() + plLinRoom();
}

// The reader thread's promotion (ctr_tile_reader.c's perform, its only caller): a linear copy is entered in the flight
// table, the reader holding its one reference. In the heap as before only with the reader's own reserve free.
UTFL2_TEXT CtrPacked* __wrap_CtrPacked_new(const CtrPackView* view) {
    uint32_t size = view->totalSize;
    if (plWantsLinear(size)) {
        CtrPacked* packed = plNewLinear(view);
        if (packed != nullptr) {
            bool entered = false;
            plLock();
            for (uint32_t i = 0; PL_FLIGHTS > i && !entered; i++) {
                if (pl.flight[i].packed != nullptr) continue;
                pl.flight[i] = (PlFlight) { packed, 1u, size, false };
                entered = true;
            }
            plUnlock();
            if (entered) return packed;
            plFreeLinear(packed);
        }
    }
    if (pl.on && size + PL_READER_HEAP > usHeapFree()) return nullptr;
    return __real_CtrPacked_new(view);
}

// The promotion's read: a copy in the flight table is read 64 KB (`chunk`) at a time, stopping once marked dead.
UTFL2_TEXT bool __wrap_CtrPacked_read(CtrPacked* packed, FILE* f, uint32_t chunk) {
    PlFlight* fl = nullptr;
    plLock();
    for (uint32_t i = 0; PL_FLIGHTS > i; i++) {
        if (pl.flight[i].packed == packed && packed != nullptr) fl = &pl.flight[i];
    }
    plUnlock();
    if (fl == nullptr) return __real_CtrPacked_read(packed, f, chunk);
    bool ok = fseek(f, 0, SEEK_SET) == 0;
    uint32_t done = 0;
    for (uint32_t i = 0; ok && packed->pieceCount > i; i++) {
        uint32_t end = packed->pieceCount > i + 1 ? packed->starts[i + 1] : packed->size;
        uint32_t size = end - packed->starts[i];
        for (uint32_t at = 0; ok && size > at;) {
            if (__atomic_load_n(&fl->dead, __ATOMIC_RELAXED)) {
                ok = false;
                break;
            }
#ifdef CTRPORT_PKLIN_STRESS
            svcSleepThread(PL_STRESS_NANOS);
#endif
            uint32_t piece = size - at < chunk ? size - at : chunk;
            if (fread(packed->pieces[i] + at, 1, piece, f) != piece) ok = false;
            at += piece;
            done += piece;
        }
    }
    plLock();
    bool dead = fl->dead;
    if (ok && !dead) {
        fl->packed = nullptr; // read: the copy is the reader's result now, like any other
        fl->refs = 0;
    }
    plUnlock();
    if (dead) {
        __atomic_add_fetch(&pl.stopped, 1u, __ATOMIC_RELAXED);
        logInfo(plStopLog, (unsigned) (done / 1024u), (unsigned) (packed->size / 1024u));
    }
    return ok && !dead;
}

// The last reference frees a linear copy (a copy in flight only once its reader lets it go).
UTFL2_TEXT void __wrap_CtrPacked_free(CtrPacked* packed) {
    if (packed == nullptr) return;
    if (!plIsLinear(packed)) {
        __real_CtrPacked_free(packed);
        return;
    }
    plLock();
    for (uint32_t i = 0; PL_FLIGHTS > i; i++) {
        if (pl.flight[i].packed != packed) continue;
        if (pl.flight[i].refs > 1) {
            pl.flight[i].refs--;
            plUnlock();
            return;
        }
        pl.flight[i].packed = nullptr;
        pl.flight[i].refs = 0;
    }
    plUnlock();
    plFreeLinear(packed);
}

// CtrPack_readFile, its copy's home chosen as above (the game thread's reads).
UTFL2_TEXT CtrPacked* CtrPkLin_readFile(FILE* f, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize) {
    long size = CtrPack_fileSize(f);
    if (0 >= size || (unsigned long) size > PL_MAX_FILE) return nullptr;
    uint8_t* head = malloc(CTR_PACK_HEADER_MAX);
    CtrPackView* view = malloc(sizeof(CtrPackView));
    CtrPacked* packed = nullptr;
    if (head != nullptr && view != nullptr) {
        size_t headSize = (size_t) size < CTR_PACK_HEADER_MAX ? (size_t) size : CTR_PACK_HEADER_MAX;
        if (fread(head, 1, headSize, f) == headSize && CtrPack_parseHeader(head, headSize, key, blobOffset, blobSize, view) &&
            view->totalSize == (uint32_t) size) {
            if (plWantsLinear((uint32_t) size)) packed = plNewLinear(view);
            if (packed == nullptr) packed = __real_CtrPacked_new(view);
        }
    }
    free(head);
    free(view);
    if (packed != nullptr && !__real_CtrPacked_read(packed, f, packed->size)) {
        __wrap_CtrPacked_free(packed);
        packed = nullptr;
    }
    return packed;
}

UTFL2_TEXT CtrPacked* __wrap_CtrPack_readFile(FILE* f, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize) {
    return CtrPkLin_readFile(f, key, blobOffset, blobSize);
}

// Whether page i's copy may go now: idle (no read of it, not the page being loaded, not the whole read's page).
static UTFL2_TEXT bool plIdle(const CtrTextureCache* cache, uint32_t i) {
    const CtrPage* p = &cache->pages[i];
    return p->packed != nullptr && p->read == nullptr && p != cache->busyPage && !(cache->wholeRead != nullptr && cache->wholePage == i);
}

// Drops the least recently used idle copy in linear memory (`linear`) or the heap, a page outside `keep` (a bit a page)
// first. Returns its bytes, 0 if none could go.
static UTFL2_TEXT uint32_t plDropOne(CtrTextureCache* cache, bool linear, uint64_t keep) {
    CtrPage* victim = nullptr;
    bool victimKept = true;
    for (uint32_t i = 0; cache->pageCount > i && UTFLPK_PAGES > i; i++) {
        CtrPage* p = &cache->pages[i];
        if (!plIdle(cache, i) || plIsLinear(p->packed) != linear) continue;
        bool kept = (keep >> i & 1u) != 0;
        if (victim == nullptr || (victimKept && !kept) || (kept == victimKept && victim->lastUsedFrame > p->lastUsedFrame)) {
            victim = p;
            victimKept = kept;
        }
    }
    if (victim == nullptr) return 0;
    uint32_t bytes = victim->packedSize;
    CtrPacked_free(victim->packed);
    victim->packed = nullptr;
    victim->packedSize = 0;
    cache->packedBytes -= bytes;
    if (linear) pl.dropped += bytes;
    return bytes;
}

// The copies in the heap, and (attached) in linear memory.
static UTFL2_TEXT uint32_t plHeld(const CtrTextureCache* cache, bool linear) {
    uint32_t bytes = 0;
    for (uint32_t i = 0; cache->pageCount > i; i++) {
        const CtrPage* p = &cache->pages[i];
        if (p->packed != nullptr && plIsLinear(p->packed) == linear) bytes += p->packedSize;
    }
    return bytes;
}

static UTFL2_TEXT void plSetBudget(CtrTextureCache* cache) {
    pl.heapHeld = plHeld(cache, false);
    cache->packedBudgetBytes = pl.heapBudget + pl.linBudget;
    pl.lastSet = cache->packedBudgetBytes;
}

// The texture budget lent for the linear share follows it: given back at once, lent a step at a time.
static UTFL2_TEXT void plLend(CtrTextureCache* cache) {
    if (pl.lent > pl.linBudget) {
        CtrTextures_returnBudget(cache, pl.lent - pl.linBudget);
        pl.lent = pl.linBudget;
        return;
    }
    for (uint32_t k = 0; PL_LEND_STEPS > k && pl.linBudget > pl.lent; k++) {
        uint32_t step = pl.linBudget - pl.lent < PL_LEND_STEP ? pl.linBudget - pl.lent : PL_LEND_STEP;
        if (!CtrTextures_lendBudgetInRoom(cache, step, PL_LEND_IDLE)) break;
        pl.lent += step;
    }
}

// Low heap (each frame, from the collectWholeRead wrap, ahead of beginFrame's relieveHeap): relieveHeap, but only copies in the
// heap go, and the heap share shrinks to what is left (the linear share stays).
static UTFL2_TEXT void plRelieveHeap(CtrTextureCache* cache) {
    uint32_t held = plHeld(cache, false);
    if (held == 0) return;
    uint32_t freeBytes = usHeapFree();
    if (freeBytes >= PL_LOW_HEAP) return;
    uint32_t dropped = 0;
    while (PL_LOW_HEAP > freeBytes + dropped) {
        uint32_t bytes = plDropOne(cache, false, 0);
        if (bytes == 0) break;
        dropped += bytes;
    }
    pl.heapBudget = held - dropped;
    plSetBudget(cache);
    logInfo(plLowLog, (unsigned) (freeBytes / 1024u), (unsigned) (dropped / 1024u), (unsigned) (pl.heapBudget / 1024u),
            (unsigned) (pl.linBudget / 1024u));
}

// A tile allocation that failed (allocateTile): an idle linear copy goes before any tile.
UTFL2_TEXT bool CtrPkLin_evictOldestIdle(CtrTextureCache* cache, uint32_t idleFrames) {
    if (pl.on && pl.cache == cache && plDropOne(cache, true, 0) > 0) return true;
    return CtrTextures_evictOldestIdle(cache, idleFrames);
}

// The floor keeper's shed (keepLinearFloor): the cached sound effects as before, then idle linear copies, before the
// tiles it evicts next. Returns the sound effects' bytes, as its log counts them.
// The second link of the shedSfx chain (ctr_wrap_chain.h): flowend's wrap calls it.
UTFL2_TEXT uint32_t CtrAudioSystem_chainPklin(uint32_t bytes) {
    uint32_t shed = __real_CtrAudioSystem_shedSfx(bytes);
    if (shed >= bytes || !pl.on || pl.cache == nullptr) return shed;
    uint32_t freed = 0;
    while (bytes - shed > freed) {
        uint32_t one = plDropOne(pl.cache, true, 0);
        if (one == 0) break;
        freed += one;
    }
    return shed;
}

#ifdef CTRPORT_PKLIN_DIAG
// The largest free blocks of linear memory (memory_report.c's largestLinear, again and again, holding each found).
static UTFL2_TEXT void plCensus(const char* room) {
    void* held[PL_CENSUS_MAX];
    uint32_t sizes[PL_CENSUS_MAX];
    uint32_t n = 0;
    int tries = 0;
    for (; LightLock_TryLock(&CtrMemory_linearLockStore) != 0 && 50 > tries; tries++) svcSleepThread(1000000);
    if (tries == 50) {
        logInfo(plCensusBusy, room);
        return;
    }
    bool allowed = CtrMemory_allowRelief(false);
    uint32_t total = __real_linearSpaceFree();
    while (PL_CENSUS_MAX > n) {
        uint32_t space = __real_linearSpaceFree();
        uint32_t fits = 0, fails = (space & ~0x7fu) + 0x80;
        while (fails - fits > 0x80) {
            uint32_t size = ((fits + fails) / 2) & ~0x7fu;
            if (size <= fits) size = fits + 0x80;
            void* p = __real_linearMemAlign(size, 0x80);
            if (p != nullptr) {
                __real_linearFree(p);
                fits = size;
            } else {
                fails = size;
            }
        }
        if (PL_CENSUS_MIN > fits) break;
        held[n] = __real_linearMemAlign(fits, 0x80);
        if (held[n] == nullptr) break;
        sizes[n++] = fits;
    }
    for (uint32_t i = 0; n > i; i++) __real_linearFree(held[i]);
    CtrMemory_allowRelief(allowed);
    LightLock_Unlock(&CtrMemory_linearLockStore);
    char line[PL_CENSUS_MAX * 8 + 160];
    int at = snprintf(line, sizeof(line), plCensusHead, room, (unsigned) (total / 1024u));
    for (uint32_t i = 0; n > i && 0 <= at && (int) sizeof(line) > at; i++) at += snprintf(line + at, sizeof(line) - (size_t) at, plCensusItem, (unsigned) (sizes[i] / 1024u));
    logInfo(plCensusEnd, line);
}
#endif

// The linear share at a room change, before item 4's keeps (the prefetch wrap): see the top. Copies over it go, other
// pages' first; reads in flight are stopped if still over (stress builds: always).
static UTFL2_TEXT void plRoomLoad(CtrTextureCache* cache) {
    uint32_t base = cache->budgetBytes + pl.lent;
    uint64_t set = 0;
    uint64_t keep = 0;
    if (us.scene != nullptr) {
        RoomPlanScene s;
        memcpy(&s, us.scene, sizeof(s));
        for (uint32_t i = 0; s.tileCount > i; i++) {
            uint32_t entry;
            memcpy(&entry, us.scene + sizeof(s) + (size_t) i * 4, 4);
            uint32_t id = UTSTALL_TILE_PAGE(entry);
            if (UTFLPK_PAGES > id) keep |= 1ull << id;
            int32_t t = 0;
            set += usTileBytes(usSceneTile(cache, us.scene, i, &t), t);
        }
    }
    uint64_t room = base > UTSTALL_WHOLE_HEADROOM ? base - UTSTALL_WHOLE_HEADROOM : 0;
    if (set > room) set = room;
    uint64_t share = room - set;
    pl.linBudget = share > PL_LIN_MAX ? PL_LIN_MAX : (uint32_t) share;
    pl.setBytes = (uint32_t) set;
    while (plLive() > pl.linBudget) {
        if (plDropOne(cache, true, keep) == 0) break;
    }
    bool stop = plLive() > pl.linBudget;
#ifdef CTRPORT_PKLIN_STRESS
    stop = true;
#endif
    if (stop) {
        plLock();
        for (uint32_t i = 0; PL_FLIGHTS > i; i++) {
            if (pl.flight[i].packed != nullptr) __atomic_store_n(&pl.flight[i].dead, true, __ATOMIC_RELAXED);
        }
        plUnlock();
    }
    uint32_t was = pl.lent;
    if (pl.lent > pl.linBudget) {
        CtrTextures_returnBudget(cache, pl.lent - pl.linBudget);
        pl.lent = pl.linBudget;
    }
    // Rule A looked at the budget less the old share: it looks again at this one.
    bool refit = was > 0 && us.scene != nullptr;
    if (refit) {
        us.wholeCount = usWholeFits(cache, us.scene);
        us.whole = us.wholeCount > 0;
    }
    plSetBudget(cache);
    const char* name = us.runner != nullptr && us.runner->currentRoom != nullptr ? us.runner->currentRoom->name : plUnknown;
    char refitText[64];
    refitText[0] = 0;
    if (refit) snprintf(refitText, sizeof(refitText), plRefitLog, (unsigned) us.wholeCount);
    logInfo(plRoomLog, name, (unsigned) (pl.linBudget / 1024u), (unsigned) (pl.setBytes / 1024u), (unsigned) (base / 1024u),
            (unsigned) (pl.lent / 1024u), (unsigned) (plHeld(cache, true) / 1024u), (unsigned) (pl.heapHeld / 1024u),
            (unsigned) (pl.heapBudget / 1024u), (unsigned) pl.made, (unsigned) (pl.madeBytes / 1024u), (unsigned) (pl.dropped / 1024u),
            (unsigned) __atomic_load_n(&pl.stopped, __ATOMIC_RELAXED), refit ? refitText : plNone);
    pl.made = 0;
    pl.madeBytes = 0;
    pl.dropped = 0;
    __atomic_store_n(&pl.stopped, 0u, __ATOMIC_RELAXED);
#ifdef CTRPORT_PKLIN_DIAG
    uint32_t flying = 0;
    plLock();
    for (uint32_t i = 0; PL_FLIGHTS > i; i++) {
        if (pl.flight[i].packed != nullptr) flying += pl.flight[i].size;
    }
    plUnlock();
    logInfo(plCheckLog, (unsigned) (plLive() / 1024u), (unsigned) (plHeld(cache, true) / 1024u), (unsigned) (flying / 1024u));
    plCensus(name);
#endif
}

UTFL2_ENTRY void CtrPkLin_keepPlanned(CtrTextureCache* cache, uint32_t room) {
    if (pl.on && pl.cache == cache) plRoomLoad(cache);
    usKeepPlanned(cache, room);
}

// Once a frame (z_roomload.c's RoomLoad_collect, under utflPkCollect: after the frame's whole read is collected, before
// utflPk's look at the SD card's loads): the heap relief, the share's lending, the copies over the shares trimmed one a
// frame, and the packed budget kept at the two shares. A wrap, so that no existing call changes for it.
void __real_CtrTextures_collectWholeRead(CtrTextureCache* cache, bool drop);
void __wrap_CtrTextures_collectWholeRead(CtrTextureCache* cache, bool drop);
UTFL2_TEXT void __wrap_CtrTextures_collectWholeRead(CtrTextureCache* cache, bool drop) {
    __real_CtrTextures_collectWholeRead(cache, drop);
    if (cache == nullptr || cache->pageCount > UTFLPK_PAGES || utflPk.pages != cache->pages || utflPk.pageCount != cache->pageCount) return;
    if (pl.cache != cache) {
        // utflPk sized the heap share in the frame before (0: no copies at all, and none here either).
        memset(&pl, 0, sizeof(pl));
        pl.cache = cache;
        pl.on = cache->packedBudgetBytes != 0;
        if (!pl.on) return;
        pl.heapBudget = cache->packedBudgetBytes;
        uint32_t room = cache->budgetBytes > UTSTALL_WHOLE_HEADROOM ? cache->budgetBytes - UTSTALL_WHOLE_HEADROOM : 0;
        pl.linBudget = room > PL_LIN_MAX ? PL_LIN_MAX : room;
        plSetBudget(cache);
        // The background worker's startup reads go to the heap only (see the top), so they stop here.
        if (cache->worker != nullptr) CtrPackWorker_setKeepPacked(cache->worker, false);
        cache->workerKeepsPacked = false;
        logInfo(plInitLog, (unsigned) (pl.heapBudget / 1024u), (unsigned) (pl.linBudget / 1024u), (unsigned) (PL_LIN_MAX / 1024u));
    }
    if (!pl.on) return;
    if (cache->packedBudgetBytes != pl.lastSet) {
        // relieveFailedAlloc (a heap allocation failed): the budget is the copies it kept.
        uint32_t held = plHeld(cache, false);
        if (pl.heapBudget > held) pl.heapBudget = held;
        logInfo(plCutLog, (unsigned) (cache->packedBudgetBytes / 1024u), (unsigned) (pl.heapBudget / 1024u));
    }
    plRelieveHeap(cache);
    plLend(cache);
    if (plLive() > pl.linBudget + PL_TRIM_SLACK) plDropOne(cache, true, 0);
    if (plHeld(cache, false) > pl.heapBudget) plDropOne(cache, false, 0);
    plSetBudget(cache);
}
#endif
#endif
