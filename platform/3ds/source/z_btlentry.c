// btlentry (UNDERTALE Old): a battle's planned tiles are read during the encounter animation, before the room change.
//
// Playtest, 11-02 (Old 3DS XL, 75zm): "some battle screen entrance lag". When a battle starts, the screen goes black and the
// soul blinks and flies to the FIGHT button (about 31 frames); then the game goes to the battle room, and on the 64 MB Old
// models the room change frame loads the battle group's whole tile set (z_utstall.c item 5) and its code: 0.5-1.15 s at
// the first fight of each group in his logs (Asgore 1153 ms, Mettaton 922 ms, Mad Dummy 805 ms), with the soul frozen
// on screen. Nearly all of it is tiles from the SD card, one page at a time on the game thread (10-110 ms a page), and
// the tile reader's read-ahead brought nothing ("read ahead 0 KB"): its reads want 8 MiB of heap free besides their
// bytes, and the Old heap has 6-7 MB free in play.
//
// So, once a frame in the overworld (from RoomLoad_collect's chain, as flowstream: utflPkCollect -> here -> flowstream),
// while an obj_battler or obj_battleblcon exists (the battle's start; neither reads anything here), the plan scene of
// global.battlegroup (the one the battle room's load will take) is walked in order, and its missing tiles are read on
// the reader thread, a page's missing planned tiles in one read (its header once, neighbouring tiles in one run) and
// three reads at a time (on different pages), admitted as flowstream's refill reads are (the promotion reserve, 4 MiB
// on UT Old, plus its bytes; heap relief off). A read whose bytes are in is installed tile by tile in later frames
// (CtrTextures_btlentryInstall: installTile, one tile's check, unpack and flush, 3-4 ms on the Old models), one a frame
// and more only while the frame's installs stay within BT_INSTALL_MS (by the costliest tile so far), each tile only if
// the texture budget and the linear floor leave room for it, else room is made by evicting tiles last drawn before
// the encounter started (round 4, below); a tile that still has no room is dropped. A read is released once its last
// tile is done. A page not loaded from yet this session takes its grid and tile table
// from the read's header, as its first load would (setLayout, applyView). The room change releases whatever is still
// in flight or not installed yet (releaseReads, which frees nothing installed), and the room load then finds the
// installed tiles resident and skips them, and reads the rest itself.
//
// The installs run in CtrTextures_beginFrame, before main.c takes the frame's start time, so a frame with installs
// starts (as the bench counts starts) that much after its slot, and the start interval into it is longer by the same
// (r2 console rows: one ~42 ms interval in the encounter room = 33.3 ms and two 3-4 ms installs of round 2's two reads,
// every frame still shown on time). BT_INSTALL_MS keeps a frame's installs within those two (what r2's rows passed on
// all three Old models), so the interval stays under ~42 ms; the log line gives the most install time one frame took.
//
// Optional all the way (c26rev r101): the game thread never does a read of ours (a wait for one of its own reads that
// takes ours from the queue gets no file, as flowstream's refill: F1); the tile's texture is allocated here without
// evicting anything and counted as a read's reserved tile, so installTile allocates nothing (F2); and the read record
// and a learned tile table are allocated with heap relief off.
//
// Round 4 (Asgore: the cache is full at the encounter, 26.0 of 26.0 MB, and r3 left 12 of his 42 missing tiles for
// the battle room's load, which then evicted 5.8 MB of other tiles itself): a tile without room evicts, oldest first as
// the cache's own eviction does (evictTile, through CtrTextures_btlentryEvict), only tiles last drawn before the
// encounter's first frame and not in the last CTR_THRASH_FRAMES frames, never a tile of the battle's planned set. Those
// are the tiles the battle room's load would evict for the same tiles a few frames later. Each evicted tile is
// remembered (the first BT_GONE), and one drawn again before the room change, so loaded again, is counted in the log
// line ("reloaded"); it must stay 0.
// Rev 2 (c26rev R109 F1: a tile last drawn before the encounter may still be one the encounter room draws again, a
// later animation frame or an object drawn now and then, and evicting it moves its reload into the animation): a
// victim must also be provably outside the room the encounter starts in, so never a tile the room wants (wanted), in
// its plan scene (the record's tiles of all its visits, CtrUtstall_activeScene) or among the tiles it drew on its last
// visits' entries (the cache's roomTiles). The plan scene is the room's whole set (tools/roomplan/build_plan.py
// whole_set: every frame of each sprite the records drew from, the room's backgrounds, tiles and placed objects' sprites,
// and the sprites of objects created in recorded play). A room with no plan scene, a scene without tiles (5 UT rooms
// have code only) or one cut at ROOMPLAN_TILES_MAX has no such proof: nothing is evicted there, and a tile without room
// is deferred, then dropped, as in round 3.
// Rev 2b (the r4 console rows: the cache wasn't full at Asgore, nothing could be evicted, and r4 still read tiles with
// no room and held a read up to 9 frames waiting for room, so it read fewer tiles than r3): at the encounter's first
// armed frame the victims btMakeRoom may take are counted once (their bytes, the pool, up to BT_POOL_ENOUGH). A tile
// without room is read only while the pool covers it and the tiles already taken on it, and only then waits for room or
// evicts. With an empty pool every choice is round 3's.
//
// Round 5 (the battle's code: r3 console rows left about 95-120 ms of the hold in the room load's bytecode reads,
// z_roomload.c rlReadCode, 8.5 ms a read on the Old models): while armed, the plan scene's code entries (the list the
// room load reads, RoomPlan_roomCode) whose group isn't in memory are read on the reader too, in file order, one read
// at a time of up to BT_CODE_SPANS runs (neighbouring groups with at most BT_CODE_GAP bytes between them in one run)
// and BT_CODE_BYTES. The read is a tile read with no cache page (pageId BT_CODE_PAGE): the reader's own file for it is
// the bytecode cache file (DataWin_moveBytecodeToFile's path, kept by its wrap), its "header" is that file's header,
// checked, and its "tiles" are the runs, in a one-column grid set up before it is queued; it is admitted and allocated
// as the tile reads are. Once its bytes are in, each of its groups not in memory meanwhile gets its code memory as the
// lazy path would give it (data_win.c allocCodeArena's 32 KB blocks and sizes; heap relief off, the promotion reserve
// left free, no growth of the block list) and its bytes, and its blobs point there: what DataWin_loadCodeBytes and
// rlReadCode do, on the game thread, with the same bytes from the same file. A group refused memory, a read refused
// or emptied (the game thread's wait gets no file for it, F1), and whatever the room change finds still in flight
// are left to the room load's own reads, as before.
//
// Textures and code loads only: no game state, script, timing the game sees or RNG changes; every tile is the same
// texels and every group the same bytes whenever it loads. Late code and statics only (late_reserve.ld, last).
#include <3ds.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(ENABLE_WAD16) && defined(CTRPORT_FLOWSTREAM_READ)
#include "ctr_textures.h"
#include "ctr_tile_reader.h"
#include "log.h"
#include "roomplan_format.h"
#include "runner.h"
#include "stb_ds.h"

// Late code, every function here (no noinline: GCC may inline a static into its callers, all of them in this section;
// that keeps btlentry inside its fixed 7,428 B slot, late_reserve.ld).
#ifdef CTRPORT_BTLENTRY_DIAG
#define BT_TEXT __attribute__((section(".text.btlentry_diag"))) // LAB ONLY: the diag's hooks outgrow the slot; .text takes it
#else
#define BT_TEXT __attribute__((section(".text.btlentry_late")))
#endif
#define BT_CONST __attribute__((aligned(4), section(".rodata.btlentry_in_text")))
#define BT_BSS __attribute__((section(".bss.btlentry_late")))
#define BT_READS 3u // in flight, on different pages
#define BT_READ_TILES 16u             // a read's tiles at most
#define BT_READ_BYTES (160u * 1024u)  // and their compressed bytes, where the page's tile table is known
#ifdef CTRPORT_BTLENTRY_FAULT
#define BT_INSTALL_MS 0u // LAB ONLY: one tile a frame, so every read's installs cross frames
#else
#define BT_INSTALL_MS 8u // game-thread install time a frame: one tile, then another only if it fits within this
#endif
#define BT_FLOOR (3u * 1024u * 1024u + 256u * 1024u) // flowstream's linear floor and spare
#define BT_LIFT (8u * 1024u * 1024u)                 // ctr_tile_reader.c's READER_HEAP_RESERVE
#define BT_KEY_MASK 0x07FFFFFFu
#define BT_POOL_ENOUGH (4u * 1024u * 1024u) // the most the victim pool is counted to (Asgore evicts at most 2.7 MB)
#define BT_GONE 64u // evicted tiles remembered, to count their reloads
#define BT_CODE_PAGE 0xFFFFu        // a code read's pageId: no cache page has it
#define BT_CODE_SPANS 16u           // a code read's runs at most
#define BT_CODE_BYTES (64u * 1024u) // and its bytes (z_roomload.c's ROOMLOAD_SPAN_BYTES)
#define BT_CODE_GAP 256u            // bytes between groups one run reads over (ROOMLOAD_GAP_BYTES)
#define BT_CODE_HEADER 20u          // data_win.c's CodeCacheHeader: magic, version, base, size, checksum
#define BT_CODE_MAGIC 0x31434244u   // its CODE_CACHE_MAGIC
#define BT_ARENA_BLOCK (32u * 1024u) // its CODE_ARENA_BLOCK
#ifdef CTRPORT_BTLENTRY_DIAG
#define BTD(...) __VA_ARGS__ // LAB ONLY (btlentry heap proof): see the end of this file
// Out of line, in .text: never inlined into the late code, so the diag twins fit btlentry's slot too.
#define BTD_TEXT __attribute__((noinline))
static BTD_TEXT void btdMark(const char* what);
static BTD_TEXT void btdQueued(uint32_t k);
static BTD_TEXT uint32_t btdFault(uint32_t k, uint32_t kind);
static BTD_TEXT bool btdFile(uint32_t pageId);
static BTD_TEXT void btdMain(const CtrTileRead* read);
static BTD_TEXT bool btdHold(void);
#else
#define BTD(...)
#endif

void __real_CtrFlowstream_collect(CtrTextureCache* cache, bool drop);
void __wrap_CtrFlowstream_collect(CtrTextureCache* cache, bool drop);
bool __real_CtrFlowstream_readHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize,
                                     CtrPackView* out);
bool __wrap_CtrFlowstream_readHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize,
                                     CtrPackView* out);
struct mallinfo __real_CtrFlowstream_readMallinfo(void);
struct mallinfo __wrap_CtrFlowstream_readMallinfo(void);
void* __real_CtrFlowstream_readMalloc(size_t size);
void* __wrap_CtrFlowstream_readMalloc(size_t size);
FILE* __real_CtrFlowstream_gameFile(uint32_t pageId);
FILE* __wrap_CtrFlowstream_gameFile(uint32_t pageId);
FILE* __real_CtrFlowstream_openFile(uint32_t pageId, bool* busy);
FILE* __wrap_CtrFlowstream_openFile(uint32_t pageId, bool* busy);
const uint8_t* RoomPlan_utstallPlan(const CtrTextureCache* cache);
const uint8_t* CtrUtstall_activeScene(const CtrTextureCache* cache); // z_utstall.c: the plan scene this room visit loaded
int32_t RoomPlan_utstallGroup(Runner* runner);
bool CtrTextures_btlentryInstall(CtrTextureCache* cache, CtrPage* page, int32_t t); // ctr_textures.c's end
void CtrTextures_btlentryEvict(CtrTextureCache* cache, CtrPage* page, CtrTile* tile);
bool CtrMemory_allowRelief(bool allow);
bool __real_DataWin_moveBytecodeToFile(DataWin* dw, const char* cachePath);
bool __wrap_DataWin_moveBytecodeToFile(DataWin* dw, const char* cachePath);
void CtrOpenFiles_beginCacheOpen(void);
void CtrOpenFiles_endCacheOpen(void);
bool CtrOpenFiles_missing(int err);
extern u32 __ctru_heap_size;

static BT_BSS struct {
    const DataWin* objectsOf;     // the data.win the object indices are for
    int32_t objects[2];           // obj_battler, obj_battleblcon (or -1)
    CtrTileRead* volatile performing; // a read of ours, on the reader between its header and its allocation
    const DataWin* codeOf;        // the data.win whose bytecode moved to the file at codePath
    char codePath[84];            // the path is 43 characters; a longer one turns code reads off (84: bt stays 512 B)
    const DataWin* namesOf;       // the data.win the round 6 indices are for (btNames)
    int16_t flier, story, early, storyRoom; // the flying soul, the story battle's starters (object, or -1); its room
    // From here on, a room visit's (cleared at a room change):
    const CtrTextureCache* cache; // the cache and its room visit
    uint32_t roomStart;
    const uint8_t* scene;         // the battle group's plan scene, once a battle starts
    int32_t group;
    uint32_t frames, installed, missing, untabled; // frames armed, tiles installed; at the first look: missing, on pages with no tile table
    uint32_t reads, installTicks, maxTicks, tileTicks; // reads queued; install time in all, in the costliest frame, of the costliest tile
    CtrTileRead* volatile read[BT_READS]; // the reads in flight, each set before it is queued; their pages
    uint32_t page[BT_READS];
    uint16_t next[BT_READS];              // the next tile of the read to install
    bool dropped[BT_READS];               // a tile of the read was not installed
    bool full;                            // a tile found no room even after evicting all it may: read only tiles with room
    uint32_t encounter;                   // the encounter's first frame
    uint32_t pool, poolTaken;             // the victims' bytes at the encounter (rev 2b); the bytes of no-room tiles read on it
    uint32_t evicted, evictedBytes, reloads, gone; // tiles evicted and their bytes; of the first BT_GONE: reloaded, remembered
    uint32_t goneKey[BT_GONE];            // page << 8 | tile
    CtrTileRead* volatile codeRead;       // the code read in flight: its groups in dstSize[], each one's run in dstFormat[]
    uint32_t codeGroups, codeAt;          // its groups; where in the bytecode the next one starts looking (UINT32_MAX: done)
    uint32_t codeReads, codePlaced, codeBytes, codeLeft; // code reads queued; groups placed and their bytes; not placed
} bt;

static const char btObject0[] BT_CONST = "obj_battler";
static const char btObject1[] BT_CONST = "obj_battleblcon";
static const char btLog[] BT_CONST =
    "Battle entry (btlentry): group %ld: %u of %u missing planned tiles read ahead in %u frames (%u untabled), "
    "%u reads; installs %.1f ms, %.1f ms most a frame; evicted %u tiles (%u KB), %u reloaded; "
    "code %u groups (%u KB) placed from %u reads, %u not placed\n";

static BT_TEXT bool btStarting(Runner* runner) {
    const DataWin* dw = runner->dataWin;
    if (bt.objectsOf != dw) {
        bt.objectsOf = dw;
        bt.objects[0] = bt.objects[1] = -1;
        for (uint32_t i = 0; dw->objt.count > i; i++) {
            const char* name = dw->objt.objects[i].name;
            if (name == nullptr) continue;
            if (strcmp(name, btObject0) == 0) bt.objects[0] = (int32_t) i;
            if (strcmp(name, btObject1) == 0) bt.objects[1] = (int32_t) i;
        }
    }
    for (uint32_t k = 0; 2 > k; k++) {
        int32_t o = bt.objects[k];
        if (0 <= o && runner->instancesByExactObject != nullptr && arrlen(runner->instancesByExactObject[o]) > 0) return true;
    }
    return false;
}

static BT_TEXT const uint8_t* btScene(const uint8_t* plan, int32_t group) {
    RoomPlanHeader h;
    memcpy(&h, plan, sizeof(h));
    size_t at = sizeof(h);
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        memcpy(&scene, plan + at, sizeof(scene));
        if (scene.sub != ROOMPLAN_NO_SUB && (int32_t) scene.sub == group) return plan + at;
        at += RoomPlan_sceneSize(&scene);
    }
    return nullptr;
}

// Texture memory for the tile, and linear memory above flowstream's floor and spare after it, with nothing evicted.
// A tile's texture bytes (what btRoomFor makes room for and btReserve checks the texture against).
static __attribute__((noinline)) BT_TEXT uint32_t btTileBytes(const CtrPage* page, const CtrTile* tile) {
    return (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
}

static BT_TEXT bool btRoomFor(const CtrTextureCache* cache, const CtrPage* page, const CtrTile* tile) {
    uint32_t bytes = btTileBytes(page, tile);
    uint32_t linear = linearSpaceFree();
    return cache->residentBytes + bytes <= cache->budgetBytes && linear >= BT_FLOOR && linear - BT_FLOOR >= bytes;
}

// Whether the tile (page << 8 | tile) is in the plan scene's tiles.
static BT_TEXT bool btPlanned(const uint8_t* scene, uint32_t key) {
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    for (uint32_t i = 0; s.tileCount > i; i++) {
        uint32_t entry;
        memcpy(&entry, scene + sizeof(s) + (size_t) i * 4u, 4);
        if ((entry & BT_KEY_MASK) == key) return true;
    }
    return false;
}

// Whether the tile may be needed by the room the encounter starts in (rev 2): wanted by it, in its plan scene, or among
// the tiles its last visits drew on entry.
static BT_TEXT bool btRoomUses(const CtrTextureCache* cache, const uint8_t* room, const CtrTile* tile, uint32_t key) {
    if (tile->wanted || btPlanned(room, key)) return true;
    int32_t r = cache->runner->currentRoomIndex;
    if (0 > r || (uint32_t) r >= cache->roomCount) return true;
    const uint32_t* drew = cache->roomTiles[r];
    for (ptrdiff_t i = 0; arrlen(drew) > i; i++) {
        if ((drew[i] & BT_KEY_MASK) == key) return true;
    }
    return false;
}

// Room for the tile (btRoomFor), evicting for it, oldest first (the first in page order of the oldest frame, as
// evictUsedBefore), tiles last drawn before the encounter's first frame and not in the last CTR_THRASH_FRAMES frames
// (so evictTile never marks the room thrashing, and no queued GPU command still uses one), never one of the battle's
// planned set, and (rev 2) never one the room the encounter starts in may use (btRoomUses). False if there is still no
// room.
static BT_TEXT const uint8_t* btRoomProof(const CtrTextureCache* cache) {
    const uint8_t* room = CtrUtstall_activeScene(cache);
    if (room == nullptr) return nullptr;
    RoomPlanScene s;
    memcpy(&s, room, sizeof(s));
    return s.tileCount == 0 || s.tileCount >= ROOMPLAN_TILES_MAX ? nullptr : room;
}

// The tiles btMakeRoom may evict: last drawn before the encounter, outside the battle's planned set and the room's
// (btRoomUses). With pick, also idle longer than CTR_THRASH_FRAMES: the oldest (the first in page order of the oldest
// frame), its key in *out. Without, their bytes in *out, counted up to BT_POOL_ENOUGH (rev 2b's pool).
static BT_TEXT CtrTile* btVictims(CtrTextureCache* cache, const uint8_t* room, bool pick, uint32_t* out) {
    CtrTile* best = nullptr;
    uint32_t sum = 0;
    for (uint32_t p = 0; cache->pageCount > p; p++) {
        CtrPage* each = &cache->pages[p];
        if (each->residentTiles == 0) continue;
        for (int32_t i = 0; each->tilesX * each->tilesY > i; i++) {
            CtrTile* other = &each->tiles[i];
            uint32_t key = p << 8 | (uint32_t) i;
            if (!other->valid || other->lastUsedFrame >= bt.encounter ||
                (pick && (other->lastUsedFrame + CTR_THRASH_FRAMES >= cache->frame ||
                          (best != nullptr && other->lastUsedFrame >= best->lastUsedFrame))) ||
                btPlanned(bt.scene, key) || btRoomUses(cache, room, other, key))
                continue;
            if (pick) {
                best = other;
                *out = key;
            } else if ((sum += other->tex.size) >= BT_POOL_ENOUGH) {
                break;
            }
        }
        if (!pick && sum >= BT_POOL_ENOUGH) break;
    }
    if (!pick) *out = sum;
    return best;
}

static BT_TEXT bool btMakeRoom(CtrTextureCache* cache, const CtrPage* page, const CtrTile* tile) {
    const uint8_t* room = btRoomProof(cache);
    while (!btRoomFor(cache, page, tile)) {
        if (room == nullptr) return false;
        uint32_t from = 0;
        CtrTile* best = btVictims(cache, room, true, &from);
        if (best == nullptr) return false;
        if (BT_GONE > bt.gone) bt.goneKey[bt.gone++] = from;
        bt.evicted++;
        bt.evictedBytes += best->tex.size;
        CtrTextures_btlentryEvict(cache, &cache->pages[from >> 8], best);
    }
    return true;
}

// Rev 2b: the bytes btMakeRoom may evict for the battle (btVictims without the age), counted at its first armed frame.
// Tiles drawn again later only leave it, so it is never short of what btMakeRoom finds.
static BT_TEXT uint32_t btPool(CtrTextureCache* cache) {
    const uint8_t* room = btRoomProof(cache);
    uint32_t pool = 0;
    if (room != nullptr) btVictims(cache, room, false, &pool);
    return pool;
}

// Room for the tile, or (rev 2b) a pool that still covers it after the no-room tiles already read on it.
static BT_TEXT bool btFits(const CtrTextureCache* cache, const CtrPage* page, const CtrTile* tile) {
    if (btRoomFor(cache, page, tile)) return true;
    uint32_t bytes = btTileBytes(page, tile);
    return bt.pool > 0 && !bt.full && bytes > 0 && bt.pool >= bt.poolTaken && bt.pool - bt.poolTaken >= bytes;
}

// Counts the remembered evicted tiles resident again that were last drawn before frame before (loaded again), and
// forgets them.
static BT_TEXT void btReloads(const CtrTextureCache* cache, uint32_t before) {
    for (uint32_t i = 0; bt.gone > i;) {
        uint32_t key = bt.goneKey[i];
        const CtrTile* tile = &cache->pages[key >> 8].tiles[key & 0xFFu];
        if (tile->valid && tile->lastUsedFrame < before) {
            bt.reloads++;
            bt.goneKey[i] = bt.goneKey[--bt.gone];
        } else {
            i++;
        }
    }
}

// The tile's texture, allocated without evicting anything (initTile, as allocateTile does with mayEvict false) and
// counted as reserveTile counts a read's: installTile's allocateTile then takes it as allocated, and releaseRead's
// unreserveTile frees it if the install fails first. False (nothing allocated) if linear memory can't give it.
static BT_TEXT bool btReserve(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    uint32_t bytes = btTileBytes(page, tile);
    if (tile->reserved || !C3D_TexInit(&tile->tex, (u16) page->tileW, (u16) page->tileH, CtrPack_gpuFormat((CtrTileFormat) tile->format)))
        return false;
    if (tile->tex.size != bytes) {
        C3D_TexDelete(&tile->tex);
        return false;
    }
    C3D_TexSetFilter(&tile->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&tile->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    tile->reserved = true;
    page->bytes += bytes;
    cache->residentBytes += bytes;
    return true;
}

// Whether the heap keeps the promotion reserve free after bytes more.
static BT_TEXT bool btHeapFor(uint32_t bytes) {
    struct mallinfo mi = mallinfo();
    return __ctru_heap_size >= (uint32_t) mi.uordblks + CtrTileReader_heapReserve() + bytes;
}

// A read record, allocated with heap relief off.
static BT_TEXT CtrTileRead* btNewRead(void) {
    bool allowed = CtrMemory_allowRelief(false);
    CtrTileRead* read = CtrTileReader_newRead();
    CtrMemory_allowRelief(allowed);
    return read;
}

// ---- Round 5: the battle's code.

// The bytecode file's path, for the reader's own handle on it (the game thread's, dw->bytecodeFile, is its own).
// AUD-16 (this file's end) by its link name only: a call GCC sees into a function defined below makes it place this
// wrap after the file's other late code, moving per-frame functions (c26rev3 R194).
bool cvMoveViaLink(DataWin* dw, const char* cachePath) __asm__("CtrCodeVerify_move");
BT_TEXT bool __wrap_DataWin_moveBytecodeToFile(DataWin* dw, const char* cachePath) {
    bool moved = cvMoveViaLink(dw, cachePath);
    bt.codeOf = nullptr;
    if (moved && sizeof(bt.codePath) > strlen(cachePath)) {
        strcpy(bt.codePath, cachePath);
        bt.codeOf = dw;
    }
    return moved;
}

// As ctr_texture_pack.c's CtrPack_openFile, for the bytecode file: on the reader only. The mode is late rodata (a
// literal would go to the merged strings of .rodata and move what follows).
static const char btRead[] BT_CONST = "rb";
static BT_TEXT FILE* btOpenCode(bool* busy) {
    CtrOpenFiles_beginCacheOpen();
    errno = 0;
    FILE* f = fopen(bt.codePath, btRead);
    if (busy != nullptr) *busy = f == nullptr && !CtrOpenFiles_missing(errno);
    CtrOpenFiles_endCacheOpen();
    if (f != nullptr) setvbuf(f, nullptr, _IONBF, 0);
    return f;
}

// The blob starting at offset (z_roomload.c's rlBlobOf), and a group's end (by its first blob, rlGroupEnd).
static BT_TEXT uint32_t btBlobOf(const DataWin* dw, uint32_t offset) {
    uint32_t low = 0, high = dw->codeBlobCount;
    while (high > low) {
        uint32_t mid = low + (high - low) / 2u;
        if (offset > dw->codeBlobs[mid].absoluteOffset) low = mid + 1u;
        else high = mid;
    }
    return low;
}

static BT_TEXT uint32_t btGroupEnd(const DataWin* dw, uint32_t first) {
    uint32_t end = 0;
    for (uint32_t b = first; dw->codeBlobCount > b && dw->codeBlobs[b].group == first; b++) {
        uint32_t blobEnd = dw->codeBlobs[b].absoluteOffset + dw->codeBlobs[b].length;
        if (blobEnd > end) end = blobEnd;
    }
    return end;
}

// The first group in file order, starting at from or later, of the plan scene's code entries not in memory (the
// first blob's index), or UINT32_MAX.
static BT_TEXT uint32_t btNextGroup(const DataWin* dw, uint32_t from) {
    RoomPlanScene s;
    memcpy(&s, bt.scene, sizeof(s));
    const uint8_t* codes = bt.scene + sizeof(s) + (size_t) s.tileCount * 4u + (((size_t) s.tileCount + 3u) & ~(size_t) 3u);
    uint32_t best = UINT32_MAX, bestAt = UINT32_MAX;
    for (uint32_t i = 0; s.codeCount > i; i++) {
        uint16_t index;
        memcpy(&index, codes + (size_t) i * 2u, 2);
        if (index >= dw->code.count) continue;
        const CodeEntry* code = &dw->code.entries[index];
        if (!code->present || code->bytecode != nullptr) continue;
        uint32_t b = btBlobOf(dw, code->bytecodeAbsoluteOffset);
        if (b >= dw->codeBlobCount || dw->codeBlobs[b].absoluteOffset != code->bytecodeAbsoluteOffset || dw->codeBlobs[b].bytes != nullptr)
            continue;
        uint32_t g = dw->codeBlobs[b].group;
        uint32_t at = dw->codeBlobs[g].absoluteOffset;
        if (at >= from && bestAt > at) best = g, bestAt = at;
    }
    return best;
}

// Queues the next code read, if none is in flight and the heap leaves the promotion reserve after its most bytes.
static BT_TEXT void btQueueCode(CtrTextureCache* cache) {
    DataWin* dw = cache->dataWin;
    if (bt.codeRead != nullptr || bt.codeAt == UINT32_MAX || bt.codeOf != dw || dw->bytecodeFile == nullptr || dw->codeBlobs == nullptr)
        return;
    uint32_t g = btNextGroup(dw, bt.codeAt);
    if (g == UINT32_MAX) {
        bt.codeAt = UINT32_MAX;
        return;
    }
    CtrTileRead* read = btHeapFor(BT_CODE_BYTES) ? btNewRead() : nullptr;
    if (read == nullptr) return;
    // The runs, as a page of one column: run r is tile r, at its offset in the file.
    uint32_t base = (uint32_t) dw->bytecodeBufferBase;
    uint32_t groups = 0, total = 0, row = 0, runStart = 0, runEnd = 0;
    while (g != UINT32_MAX && CTR_PAGE_MAX_TILES > groups) {
        uint32_t start = dw->codeBlobs[g].absoluteOffset, end = btGroupEnd(dw, g);
        bool join = groups > 0 && runEnd + BT_CODE_GAP >= start;
        uint32_t add = end - (join ? runEnd : start);
        if (total + add > BT_CODE_BYTES || (!join && groups > 0 && row + 1u == BT_CODE_SPANS)) break;
        total += add;
        if (!join && groups > 0) row++;
        if (!join) runStart = start;
        runEnd = end;
        read->want[row] = true;
        read->view.tileOffset[row] = BT_CODE_HEADER + runStart - base;
        read->view.tileSize[row] = runEnd - runStart;
        read->dstSize[groups] = g; // dst stays nullptr: nothing is decoded
        read->dstFormat[groups++] = (uint8_t) row;
        g = btNextGroup(dw, end);
    }
    bt.codeAt = groups > 0 ? runEnd : dw->codeBlobs[g].absoluteOffset + 1u; // a group alone over BT_CODE_BYTES is left
    if (groups == 0) {
        CtrTileReader_release(cache->reader, read);
        return;
    }
    read->view.layout.tilesX = 1;
    read->view.layout.tilesY = (int32_t) row + 1;
    read->view.totalSize = BT_CODE_HEADER + dw->codeStats.totalBytes;
    read->pageId = BT_CODE_PAGE;
    bt.codeGroups = groups;
    bt.codeRead = read;
    bt.codeReads++;
    BTD(btdQueued(BT_READS));
    CtrTileReader_queue(cache->reader, read);
}

// Code memory for a group of size bytes, as data_win.c's allocCodeArena gives it, but only with the promotion reserve
// still free after it and room in the block list (no growth), and with heap relief off; nullptr otherwise.
static BT_TEXT uint8_t* btArena(DataWin* dw, uint32_t size) {
    size = (size + 3u) & ~3u;
    bool own = size > BT_ARENA_BLOCK / 4u;
    uint32_t take = own ? size : size > dw->codeArenaLeft ? BT_ARENA_BLOCK : 0;
    if (take > 0) {
        stbds_array_header* list = dw->codeArenaBlocks != nullptr ? stbds_header(dw->codeArenaBlocks) : nullptr;
        uint8_t* block = list != nullptr && list->capacity > list->length && btHeapFor(take) ? malloc(take) : nullptr;
        if (block == nullptr) return nullptr;
        dw->codeArenaBlocks[list->length++] = block; // arrput, with room for it
        dw->codeStats.residentBytes += take;
        if (own) return block;
        dw->codeArena = block;
        dw->codeArenaLeft = BT_ARENA_BLOCK;
    }
    uint8_t* at = dw->codeArena;
    dw->codeArena += size;
    dw->codeArenaLeft -= size;
    return at;
}

// The code read, once finished: each of its groups still not in memory gets its memory and bytes (rlPlace and
// rlReadCode's copy, a group at a time), then the read is released.
static BT_TEXT void btPlaceCode(CtrTextureCache* cache) {
    CtrTileRead* read = bt.codeRead;
    if (read == nullptr || !CtrTileReader_finished(cache->reader, read) BTD(|| btdHold())) return;
    DataWin* dw = cache->dataWin;
    DataWinCodeBlob* blobs = dw->codeBlobs;
    uint32_t base = (uint32_t) dw->bytecodeBufferBase;
    uint32_t left = bt.codeLeft;
    bool allowed = CtrMemory_allowRelief(false);
    for (uint32_t i = 0; bt.codeGroups > i; i++) {
        uint32_t g = read->dstSize[i], row = read->dstFormat[i];
        if (blobs[g].bytes != nullptr) continue; // run meanwhile, so read by the lazy path
        uint32_t start = blobs[g].absoluteOffset, end = btGroupEnd(dw, g);
        uint8_t* at = read->headerOk && read->data != nullptr && read->tileAt[row] != CTR_READ_NOT_READ BTD(&& !btdFault(BT_READS, 3))
                          ? btArena(dw, end - start)
                          : nullptr;
        if (at == nullptr) {
            bt.codeLeft++;
            continue;
        }
        memcpy(at, read->data + read->tileAt[row] + (BT_CODE_HEADER + start - base - read->view.tileOffset[row]), end - start);
        for (uint32_t b = g; dw->codeBlobCount > b && blobs[b].group == g; b++) blobs[b].bytes = at + (blobs[b].absoluteOffset - start);
        bt.codePlaced++;
        bt.codeBytes += end - start;
    }
    CtrMemory_allowRelief(allowed);
    BTD(if (read->data == nullptr || bt.codeLeft != left) btdFault(BT_READS, read->data == nullptr ? 4 : 5);)
    (void) left;
    CtrTileReader_release(cache->reader, read);
    bt.codeRead = nullptr;
}

// The game thread's file for a read of ours that its wait took from the queue (performNextOnGameFile's gameFile, the
// original wait's openFile): none, so it reads nothing and is dropped, as flowstream's refill (fsStolen). One read a
// page, so the page's read is ours.
static BT_TEXT bool btStolen(uint32_t pageId) {
    if (threadGetCurrent() != nullptr || bt.cache == nullptr || pageId >= bt.cache->pageCount) return false;
    for (uint32_t k = 0; BT_READS > k; k++) {
        if (bt.read[k] != nullptr && bt.page[k] == pageId && bt.cache->pages[pageId].read == bt.read[k]) return BTD(btdFault(k, 6),) true;
    }
    return false;
}

BT_TEXT FILE* __wrap_CtrFlowstream_gameFile(uint32_t pageId) {
    if (pageId == BT_CODE_PAGE) return BTD(btdFault(BT_READS, 6),) nullptr;
    return btStolen(pageId) BTD(|| btdFile(pageId)) ? nullptr : __real_CtrFlowstream_gameFile(pageId);
}

// A code read's file is the bytecode file, opened by the reader only (none on the game thread, as btStolen).
BT_TEXT FILE* __wrap_CtrFlowstream_openFile(uint32_t pageId, bool* busy) {
    if (pageId == BT_CODE_PAGE) return threadGetCurrent() == nullptr ? (BTD(btdFault(BT_READS, 6),) nullptr) : BTD(btdFile(pageId) ? nullptr :) btOpenCode(busy);
    return btStolen(pageId) BTD(|| btdFile(pageId)) ? nullptr : __real_CtrFlowstream_openFile(pageId, busy);
}

// A planned tile the reader may read now (*outPage set if it is missing, or its page's grid isn't known yet): on a
// cache page with a cache file not known to be missing, that no read or copy holds, and, if the page's tile table is
// known, with room for it (else the read learns the table first, from its header). 1: yes; 0: not now (or not missing).
static BT_TEXT bool btReadable(CtrTextureCache* cache, uint32_t entry, CtrPage** outPage, uint32_t* outTile) {
    uint32_t p = (entry & BT_KEY_MASK) >> 8;
    uint32_t t = entry & 0xFFu;
    if (p >= cache->pageCount) return 0;
    CtrPage* page = &cache->pages[p];
    if (page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING || cache->dataWin->txtr.textures[p].externalPath != nullptr) return 0;
    CtrTile* tile = nullptr;
    if (page->layoutKnown) {
        if (t >= (uint32_t) (page->tilesX * page->tilesY)) return 0;
        tile = &page->tiles[t];
        if (tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) return 0;
    }
    *outPage = page;
    *outTile = t;
    return cache->frame >= page->retryFrame && page->packed == nullptr && page->read == nullptr &&
           (!page->packInfo || btFits(cache, page, tile));
}

// Round 6 (11-08), appended late code outside the 7,428 B slot (late_reserve.ld); its 12 B of state come out
// of the code path's buffer, so bt keeps its 512 B and the bss slot its pad.
// 1. The soul does not reach its spot before the battle loads, so it jumps: its flight to its battle spot
//    (obj_transheart, moving until it lands) is the only motion of an encounter, so nothing is installed and no code
//    placed while it flies (btMoving); both run in the blink before it and the still frames after it.
// 2. A New Home story battle (ethanlog4 item 2): its room (room_storybattle) loads its own whole set, not a battle
//    group's, so while one starts (obj_battlerstory there, or the alert with an early story battle's object) the
//    reads are of that room's own scene (btTarget), and its room change finds them resident.
#define BT_R6 __attribute__((noinline, section(".text.btmoving_late")))
#define BT_R6_CONST __attribute__((aligned(4), section(".rodata.btmoving_in_text")))
static const char btFlier[] BT_R6_CONST = "obj_transheart";
static const char btStory[] BT_R6_CONST = "obj_battlerstory";
static const char btEarly[] BT_R6_CONST = "obj_earlystorybattle";
static const char btStoryRoom[] BT_R6_CONST = "room_storybattle";

static BT_R6 void btNames(const DataWin* dw) {
    if (bt.namesOf == dw) return;
    bt.namesOf = dw;
    bt.flier = bt.story = bt.early = bt.storyRoom = -1;
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        const char* name = dw->objt.objects[i].name;
        if (name == nullptr) continue;
        if (strcmp(name, btFlier) == 0) bt.flier = (int16_t) i;
        if (strcmp(name, btStory) == 0) bt.story = (int16_t) i;
        if (strcmp(name, btEarly) == 0) bt.early = (int16_t) i;
    }
    for (uint32_t i = 0; dw->room.count > i; i++) {
        const char* name = dw->room.rooms[i].name;
        if (name != nullptr && strcmp(name, btStoryRoom) == 0) bt.storyRoom = (int16_t) i;
    }
}

static BT_R6 Instance** btList(const Runner* runner, int32_t o) {
    return 0 <= o && runner->instancesByExactObject != nullptr ? runner->instancesByExactObject[o] : nullptr;
}

static BT_R6 bool btMoving(const Runner* runner) {
    btNames(runner->dataWin);
    Instance** list = btList(runner, bt.flier);
    for (ptrdiff_t i = 0; arrlen(list) > i; i++) {
        if (list[i]->speed != 0.0f) return true;
    }
    return false;
}

// The scene to read ahead, or null: a story battle's room's own scene (group -1), else btlentry's battle group scene.
static BT_R6 const uint8_t* btTarget(Runner* runner, const uint8_t* plan, int32_t* group) {
    bool battle = btStarting(runner); // looks up the alert (bt.objects[1])
    btNames(runner->dataWin);
    if (arrlen(btList(runner, bt.story)) > 0 || (battle && arrlen(btList(runner, bt.early)) > 0 && arrlen(btList(runner, bt.objects[1])) > 0)) {
        *group = -1;
        RoomPlanHeader h;
        memcpy(&h, plan, sizeof(h));
        size_t at = sizeof(h);
        for (uint32_t k = 0; h.sceneCount > k; k++) {
            RoomPlanScene scene;
            memcpy(&scene, plan + at, sizeof(scene));
            if (scene.sub == ROOMPLAN_NO_SUB && 0 <= bt.storyRoom && scene.room == (uint32_t) bt.storyRoom) return plan + at;
            at += RoomPlan_sceneSize(&scene);
        }
        return nullptr;
    }
    if (!battle) return nullptr;
    *group = RoomPlan_utstallGroup(runner);
    return 0 <= *group ? btScene(plan, *group) : nullptr;
}

BT_TEXT void __wrap_CtrFlowstream_collect(CtrTextureCache* cache, bool drop) {
    __real_CtrFlowstream_collect(cache, drop);
    Runner* runner = cache != nullptr ? cache->runner : nullptr;
    if (runner == nullptr || cache->reader == nullptr) return;
    if (bt.cache != cache || bt.roomStart != cache->roomStartFrame) {
        // A room change: the cache released our reads with the room's. Evicted tiles resident again and last drawn
        // before the battle room's first frame were loaded again in the encounter room.
        if (bt.scene != nullptr && bt.cache == cache) btReloads(cache, cache->roomStartFrame);
        if (bt.codeRead != nullptr && bt.cache == cache) {
            BTD(btdFault(BT_READS, 9);)
            CtrTileReader_release(cache->reader, bt.codeRead); // the room load reads the rest
        }
        if (bt.scene != nullptr)
            logInfo(btLog, (long) bt.group, (unsigned) bt.installed, (unsigned) bt.missing, (unsigned) bt.frames, (unsigned) bt.untabled,
                    (unsigned) bt.reads, (double) bt.installTicks / (double) (SYSCLOCK_ARM11 / 1000u),
                    (double) bt.maxTicks / (double) (SYSCLOCK_ARM11 / 1000u), (unsigned) bt.evicted, (unsigned) (bt.evictedBytes / 1024u),
                    (unsigned) bt.reloads, (unsigned) bt.codePlaced, (unsigned) ((bt.codeBytes + 1023u) / 1024u), (unsigned) bt.codeReads,
                    (unsigned) bt.codeLeft);
        BTD(btdMark(bt.scene != nullptr ? "the encounter animation and the battle room load" : nullptr));
        memset((void*) &bt.cache, 0, sizeof(bt) - offsetof(__typeof__(bt), cache));
        bt.cache = cache;
        bt.roomStart = cache->roomStartFrame;
    }
    BTD(btdMark(cache->frame - bt.roomStart == 300u ? "the room's first 300 frames" : nullptr));
    btReloads(cache, UINT32_MAX);
    // The reads in: their tiles installed while there is room and time, else dropped; a read is released once all its
    // tiles are, and one cut short by the time keeps the page until the next frame.
    if (btMoving(runner)) goto look; // round 6: not while the soul flies (the reads stay in flight or wait)
    uint64_t start = svcGetSystemTick();
    uint64_t budget = (uint64_t) BT_INSTALL_MS * (SYSCLOCK_ARM11 / 1000u);
    uint32_t done = 0;
    btPlaceCode(cache);
    for (uint32_t k = 0; BT_READS > k; k++) {
        CtrTileRead* read = bt.read[k];
        if (read == nullptr) continue;
        CtrPage* page = &cache->pages[bt.page[k]];
        if (page->read != read) { // the cache took it (drawn, or released)
            bt.read[k] = nullptr;
            continue;
        }
        if (!CtrTileReader_finished(cache->reader, read)) continue;
        const CtrPageLayout* layout = &read->view.layout;
        bool allowed = CtrMemory_allowRelief(false);
        if (read->headerOk && !page->layoutKnown && (page->tiles = calloc((size_t) (layout->tilesX * layout->tilesY), sizeof(CtrTile))) != nullptr) {
            // A page not used yet this session: its grid from the read's header (ctr_textures.c's setLayout).
            page->width = layout->width;
            page->height = layout->height;
            page->tileW = layout->tileW;
            page->tileH = layout->tileH;
            page->tilesX = layout->tilesX;
            page->tilesY = layout->tilesY;
            page->layoutKnown = true;
        }
        CtrMemory_allowRelief(allowed);
        if (read->headerOk && page->layoutKnown && !page->packInfo && page->width == layout->width && page->height == layout->height &&
            page->tileW == layout->tileW && page->tileH == layout->tileH && page->tilesX == layout->tilesX && page->tilesY == layout->tilesY) {
            // Its tile table, from the same header (ctr_textures.c's applyView, as takeView does on a page's first load).
            for (int32_t i = 0; page->tilesX * page->tilesY > i; i++) {
                CtrTile* each = &page->tiles[i];
                each->format = layout->formats[i];
                each->offset = read->view.tileOffset[i];
                each->size = read->view.tileSize[i];
                each->checksum = read->view.tileChecksum[i];
            }
            page->formatsKnown = true;
            page->packInfo = true;
            page->cacheFile = CTR_CACHE_FILE_VALID;
        }
        bool more = false;
        for (uint32_t t = bt.next[k]; CTR_PAGE_MAX_TILES > t; t++) {
            if (!read->want[t]) continue;
            if (done > 0 && svcGetSystemTick() - start + bt.tileTicks > budget) {
                bt.next[k] = (uint16_t) t;
                more = true;
                break;
            }
            CtrTile* tile = page->packInfo && t < (uint32_t) (page->tilesX * page->tilesY) ? &page->tiles[t] : nullptr;
            if (tile != nullptr && tile->valid) continue;
            bool usable = read->headerOk && tile != nullptr && read->data != nullptr && read->tileAt[t] != CTR_READ_NOT_READ &&
                          tile->offset == read->view.tileOffset[t] && tile->size == read->view.tileSize[t];
            if (usable && !btRoomFor(cache, page, tile) && (bt.pool == 0 || !btMakeRoom(cache, page, tile))) {
                if (bt.pool > 0 && cache->frame <= bt.encounter + CTR_THRASH_FRAMES + 1u) { // the tiles drawn just before may be evicted soon
                    bt.next[k] = (uint16_t) t;
                    more = true;
                    break;
                }
                bt.full = true;
            } else if (usable && BTD(!btdFault(k, 3) &&) btReserve(cache, page, tile)) {
                done++;
                uint64_t before = svcGetSystemTick();
                bool ok = CtrTextures_btlentryInstall(cache, page, (int32_t) t);
                uint32_t took = (uint32_t) (svcGetSystemTick() - before);
                if (took > bt.tileTicks) bt.tileTicks = took;
                if (ok) {
                    bt.installed++;
                    continue;
                }
            }
            bt.dropped[k] = true;
        }
        if (more) break;
        BTD(if (read->data == nullptr || bt.dropped[k]) btdFault(k, read->data == nullptr ? 4 : 5);)
        CtrTextures_btlentryInstall(cache, page, -1); // released: frees a reserved texture whose install failed
        bt.read[k] = nullptr;
    }
    if (done > 0) {
        uint32_t ticks = (uint32_t) (svcGetSystemTick() - start);
        bt.installTicks += ticks;
        if (ticks > bt.maxTicks) bt.maxTicks = ticks;
    }
look:;
    const uint8_t* plan = RoomPlan_utstallPlan(cache);
    int32_t group = -1;
    const uint8_t* scene = plan != nullptr ? btTarget(runner, plan, &group) : nullptr; // round 6
    if (scene == nullptr) return;
    RoomPlanScene s;
    memcpy(&s, scene, sizeof(s));
    if (s.room == (uint32_t) runner->currentRoomIndex) return;
    bool first = bt.scene != scene;
    BTD(btdMark(first ? "the overworld visit before the encounter" : nullptr));
    if (first) bt.encounter = cache->frame - 1u; // obj_battler came in the last frame's step, before its draw
    bt.scene = scene;
    bt.group = group;
    if (first) bt.pool = btPool(cache);
    bt.frames++;
    btQueueCode(cache);
    uint32_t k = 0;
    for (uint32_t i = 0; s.tileCount > i; i++) {
        while (BT_READS > k && bt.read[k] != nullptr) k++;
        if (BT_READS == k && !first) break;
        uint32_t entry;
        memcpy(&entry, scene + sizeof(s) + (size_t) i * 4u, 4);
        CtrPage* page = nullptr;
        uint32_t t = 0;
        bool readable = btReadable(cache, entry, &page, &t);
        if (first && page != nullptr) bt.missing++, bt.untabled += !page->packInfo;
        if (!readable || BT_READS == k || cache->fillPending) continue;
        // The page's planned tiles still missing, in plan order from this one (the page's first), up to a read's limits.
        uint32_t p = (uint32_t) (page - cache->pages);
        uint8_t want[BT_READ_TILES];
        uint32_t tiles = 0, bytes = 0;
        for (uint32_t j = i; s.tileCount > j && BT_READ_TILES > tiles; j++) {
            uint32_t other;
            memcpy(&other, scene + sizeof(s) + (size_t) j * 4u, 4);
            CtrPage* same = nullptr;
            uint32_t u = 0;
            if (((other & BT_KEY_MASK) >> 8) != p || memchr(want, (int) (other & 0xFFu), tiles) != nullptr || !btReadable(cache, other, &same, &u))
                continue;
            uint32_t size = page->packInfo ? page->tiles[u].size : 0;
            if (tiles > 0 && bytes + size > BT_READ_BYTES) break;
            want[tiles++] = (uint8_t) u;
            bytes += size;
            if (page->packInfo && !btRoomFor(cache, page, &page->tiles[u]))
                bt.poolTaken += btTileBytes(page, &page->tiles[u]);
        }
        CtrTileRead* read = btHeapFor(bytes) ? btNewRead() : nullptr;
        if (read == nullptr) break;
        for (uint32_t j = 0; tiles > j; j++) read->want[want[j]] = true;
        const Texture* txtr = &cache->dataWin->txtr.textures[p];
        read->pageId = p;
        read->blobOffset = txtr->blobOffset;
        read->blobSize = txtr->blobSize;
        page->read = read;
        bt.page[k] = p;
        bt.next[k] = 0;
        bt.dropped[k] = false;
        bt.read[k] = read; // before the queue's lock hands it to the reader
        bt.reads++;
        BTD(btdQueued(k));
        CtrTileReader_queue(cache->reader, read);
    }
}

// The reader's hooks (ctr_tile_reader.c's CTR_READ_*, through flowstream's): a read of ours is admitted with the
// promotion reserve plus its bytes, and its buffer allocated with heap relief off, as flowstream's refill read is.
BT_TEXT bool __wrap_CtrFlowstream_readHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize,
                                             CtrPackView* out) {
    CtrTileRead* read = (CtrTileRead*) (void*) ((uint8_t*) out - offsetof(CtrTileRead, view));
    if (threadGetCurrent() != nullptr) {
        bt.performing = nullptr;
        for (uint32_t k = 0; BT_READS > k; k++) {
            if (read == bt.read[k] && read->pageId == bt.page[k]) bt.performing = read BTD(, btdFault(k, 0));
        }
        if (read == bt.codeRead && read->pageId == BT_CODE_PAGE) {
            // The bytecode file's header: its magic (perform checks the file's size); the view was set up when it was queued.
            bt.performing = read BTD(, btdFault(BT_READS, 0));
            uint32_t magic;
            memcpy(&magic, data, 4);
            return size >= 4u && magic == BT_CODE_MAGIC;
        }
    } BTD(else btdMain(read);)
    return __real_CtrFlowstream_readHeader(data, size, key, blobOffset, blobSize, out);
}

BT_TEXT struct mallinfo __wrap_CtrFlowstream_readMallinfo(void) {
    struct mallinfo mi = __real_CtrFlowstream_readMallinfo();
    if (bt.performing != nullptr && threadGetCurrent() != nullptr BTD(&& !btdFault(0, 1))) {
        uint32_t lift = BT_LIFT - CtrTileReader_heapReserve();
        mi.uordblks = (uint32_t) mi.uordblks > lift ? (int) ((uint32_t) mi.uordblks - lift) : 0;
    }
    return mi;
}

BT_TEXT void* __wrap_CtrFlowstream_readMalloc(size_t size) {
    if (bt.performing == nullptr || threadGetCurrent() == nullptr) return __real_CtrFlowstream_readMalloc(size);
    bt.performing = nullptr;
    bool allowed = CtrMemory_allowRelief(false);
    void* data = BTD(btdFault(0, 2) ? nullptr :) malloc(size);
    CtrMemory_allowRelief(allowed);
    return data;
}

#ifdef CTRPORT_BTLENTRY_DIAG
// LAB ONLY, never release (CTRPORT_BTLENTRY_DIAG; review decision 11-02 on the reserve): the smallest heap free
// (__ctru_heap_size less mallinfo's bytes in use, as ctr_tile_reader.c's heapFree) after every heap allocation of any
// thread (newlib's _malloc_r, _calloc_r, _realloc_r and _memalign_r are wrapped, so stdio's buffers count too), logged
// per window: the overworld visit before an encounter, the encounter animation with the battle room's load, and the
// battle room's first 300 frames. With CTRPORT_BTLENTRY_FAULT, our reads cycle through five outcomes (normal, refused
// at the reader's reserve, payload allocation failed, texture allocation failed at install, no file as a read the
// game thread took gets) to prove each falls back to the room load's own read with the same results. Our reads whose
// header the game thread read (a file of ours opened on it: c26rev r101 F1's gate) are counted; they must stay 0.
static struct {
    uint32_t minFree, queued, outcome[10]; // [9]: code reads released by the room change
    uint8_t kind[BT_READS + 1u]; // each read in flight's fault (the code read's last) (0 none, 1 refused, 2 allocation fails, 3 texture fails, 7 no file)
    uint8_t performingK;
} btd = {.minFree = UINT32_MAX};

static void btdSample(void) {
    struct mallinfo mi = mallinfo();
    uint32_t used = (uint32_t) mi.uordblks;
    uint32_t free = __ctru_heap_size > used ? __ctru_heap_size - used : 0;
    uint32_t seen = __atomic_load_n(&btd.minFree, __ATOMIC_RELAXED);
    while (seen > free && !__atomic_compare_exchange_n(&btd.minFree, &seen, free, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

void* __real__malloc_r(struct _reent* r, size_t n);
void* __real__calloc_r(struct _reent* r, size_t c, size_t n);
void* __real__realloc_r(struct _reent* r, void* p, size_t n);
void* __real__memalign_r(struct _reent* r, size_t a, size_t n);
void* __wrap__malloc_r(struct _reent* r, size_t n);
void* __wrap__calloc_r(struct _reent* r, size_t c, size_t n);
void* __wrap__realloc_r(struct _reent* r, void* p, size_t n);
void* __wrap__memalign_r(struct _reent* r, size_t a, size_t n);
void* __wrap__malloc_r(struct _reent* r, size_t n) {
    void* q = __real__malloc_r(r, n);
    if (q != nullptr) btdSample();
    return q;
}
void* __wrap__calloc_r(struct _reent* r, size_t c, size_t n) {
    void* q = __real__calloc_r(r, c, n);
    if (q != nullptr) btdSample();
    return q;
}
void* __wrap__realloc_r(struct _reent* r, void* p, size_t n) {
    void* q = __real__realloc_r(r, p, n);
    if (q != nullptr) btdSample();
    return q;
}
void* __wrap__memalign_r(struct _reent* r, size_t a, size_t n) {
    void* q = __real__memalign_r(r, a, n);
    if (q != nullptr) btdSample();
    return q;
}

static void btdMark(const char* what) {
    if (what == nullptr) return;
    uint32_t low = __atomic_exchange_n(&btd.minFree, UINT32_MAX, __ATOMIC_RELAXED);
    logInfo("Battle entry diag: %s: smallest heap free %lu KB (floor 4096 KB), %s; reads queued %lu (since boot): installed, "
            "taken or released %lu, finished empty %lu, not installed %lu, game-thread files refused %lu, read on the game thread %lu; faults: refused %lu, allocation failed %lu, texture failed %lu, no file %lu; code reads released by a room change %lu\n", what,
            (unsigned long) (low / 1024), low >= 4096u * 1024u ? "above" : "BELOW", (unsigned long) btd.queued,
            (unsigned long) (btd.queued - btd.outcome[4] - btd.outcome[5]), (unsigned long) btd.outcome[4],
            (unsigned long) btd.outcome[5], (unsigned long) btd.outcome[6], (unsigned long) btd.outcome[8], (unsigned long) btd.outcome[1], (unsigned long) btd.outcome[2],
            (unsigned long) btd.outcome[3], (unsigned long) btd.outcome[7], (unsigned long) btd.outcome[9]);
}

static void btdQueued(uint32_t k) {
#ifdef CTRPORT_BTLENTRY_FAULT
    static const uint8_t cycle[5] = {0, 1, 2, 3, 7};
    btd.kind[k] = cycle[btd.queued % 5u];
#else
    btd.kind[k] = 0;
#endif
    btd.queued++;
}

// kind 0: the reader is performing read k (remember it); 1: may its reserve be lifted? (nonzero: no, refused);
// 2: may its allocation succeed? (nonzero: no); 3: may its texture be allocated? (nonzero: no, dropped);
// 4, 5, 6: a read finished empty / not installed / its file refused to the game thread (counted).
static uint32_t btdFault(uint32_t k, uint32_t kind) {
    if (kind == 0) {
        btd.performingK = (uint8_t) k;
        return 0;
    }
    if (kind >= 4) {
        btd.outcome[kind]++;
        return 0;
    }
    uint32_t at = kind == 3 ? k : btd.performingK;
    if (btd.kind[at] != kind) return 0;
    btd.outcome[kind]++;
    return 1;
}

// A file of ours the reader thread asks for, refused under CTRPORT_BTLENTRY_FAULT for the no-file reads (fault 7).
static bool btdFile(uint32_t pageId) {
    if (pageId == BT_CODE_PAGE && btd.kind[BT_READS] == 7) return btd.outcome[7]++, true;
    if (threadGetCurrent() == nullptr || bt.cache == nullptr || pageId >= bt.cache->pageCount) return false;
    for (uint32_t k = 0; BT_READS > k; k++) {
        if (bt.read[k] != nullptr && bt.page[k] == pageId && bt.cache->pages[pageId].read == bt.read[k] && btd.kind[k] == 7) {
            btd.outcome[7]++;
            return true;
        }
    }
    return false;
}

// A header the game thread read: one of ours must never be among them.
static void btdMain(const CtrTileRead* read) {
    for (uint32_t k = 0; BT_READS > k; k++) {
        if (read == bt.read[k]) btd.outcome[8]++;
    }
    if (read == bt.codeRead) btd.outcome[8]++;
}

// With CTRPORT_BTLENTRY_FAULT, a code read with no fault is never placed: the room change releases it, in whatever
// state it is (queued, being read or finished), and the room load reads its groups.
static bool btdHold(void) {
#ifdef CTRPORT_BTLENTRY_FAULT
    return btd.kind[BT_READS] == 0;
#else
    return false;
#endif
}
#endif

// ---- AUD-16: the bytecode file read back once, on the boot that writes it.
// DataWin_moveBytecodeToFile trusts a file whose header and length match on later boots, so a write the card lost
// (a bad or fake-capacity card, a write dropped without an error) would be run as bytecode on every boot after.
// writeCodeCache's rename of its finished temporary file into place (rename's wrap below knows it by the path's
// pointer, set only during the move) first reads the payload back against the resident bytes it was written from.
// A difference fails the rename, so writeCodeCache deletes the file and the move returns false with the bytecode
// still in memory. The hook then runs the move once more, which writes a new file and reads it back again. A
// second difference leaves the bytecode in memory, the move's own "could not use" path. The payload's size is the
// one the move gives the header (the end of the last code entry), never read from a file. Damage after a good
// write stays a card-damage case. Own sections (late_reserve.ld, after lshalt); the one call is in the wrap above.
#define CV_TEXT __attribute__((noinline, section(".text_codeverify")))
#define CV_CHUNK 8192u
static struct { DataWin* dw; const char* path; uint32_t bad; } cv __attribute__((section(".bss.codeverify_late")));
static const char cvRead[] __attribute__((aligned(4), section(".rodata.codeverify_in_text"))) = "rb";

// True when the file at from holds the resident payload. Its own function, so only the bytecode's rename has its
// 8 KB frame (c26rev3 R194); every other rename is the wrap's pointer compare and the real call.
CV_TEXT static bool cvMatches(const char* from) {
    DataWin* dw = cv.dw;
    const uint8_t* buf = dw->bytecodeBuffer;
    uint32_t base = (uint32_t) dw->bytecodeBufferBase, size = 0;
    for (uint32_t i = 0; dw->code.count > i; i++) {
        CodeEntry* e = &dw->code.entries[i];
        if (e->present && e->bytecodeAbsoluteOffset + e->length - base > size) size = e->bytecodeAbsoluteOffset + e->length - base;
    }
    uint8_t chunk[CV_CHUNK]; // on the main thread's 32 KB stack, at boot
    FILE* f = fopen(from, cvRead);
    // Unbuffered (set before any other use of the stream): each chunk is one read of the card, not one per
    // stdio buffer. 20: data_win.c's CodeCacheHeader.
    bool bad = f == nullptr || setvbuf(f, nullptr, _IONBF, 0) != 0 || fseek(f, 20, SEEK_SET) != 0;
    for (uint32_t at = 0; !bad && size > at; at += CV_CHUNK) {
        uint32_t n = size - at > CV_CHUNK ? CV_CHUNK : size - at;
        bad = fread(chunk, 1, n, f) != n || memcmp(chunk, buf + at, n) != 0;
    }
    if (f != nullptr) fclose(f);
    return !bad;
}

int __real_rename(const char* from, const char* to);
CV_TEXT int __wrap_rename(const char* from, const char* to) {
    if (to == cv.path && !cvMatches(from)) {
        cv.bad = 1;
        return -1;
    }
    return __real_rename(from, to);
}

CV_TEXT bool CtrCodeVerify_move(DataWin* dw, const char* cachePath) {
    cv.dw = dw;
    cv.path = cachePath;
    cv.bad = 0;
    bool moved = __real_DataWin_moveBytecodeToFile(dw, cachePath);
    if (cv.bad) moved = __real_DataWin_moveBytecodeToFile(dw, cachePath); // the file was lost: write it once more
    cv.path = nullptr; // the caller's path is its stack: a later rename must not match it
    return moved;
}
#endif

#if defined(ENABLE_WAD16) && defined(CTRPORT_FLOWSTREAM_READ)
// ---- utfade A (change utfade, 2026-11-05; UNDERTALE Old): a door's destination read during the door's fade-out.
// On the Old models a door's first visit holds the black (z_utfade.c) 7 to 35 vblanks, against PC's 4: the room load
// reads the new room's planned tiles and its code from the SD card on the game thread (change utfade, utfade-ft1 rows:
// tiles and code are 65-78 % of the hold). The eight plain doors (uaNames: A, B, C and D, and their music-fading
// variants; touching one sets its alarm 2 to 8 steps, 13 on the music doors, and its Alarm 2 changes the room) go to
// the room the room order gives: A the next room, B the previous one, C two on, D two back. The doors whose Alarm 2
// picks the room another way (a table of rooms, or flags) are not read ahead, and where a plain door's room differs
// (one A door goes elsewhere on a flag) the reads are only released.
//
// So, once a frame (flowstream's once-a-frame call, z_utstall.c's utflPkCollect: here, then btlentry's wrap above,
// then flowstream), while such a door's alarm 2 runs, the destination's room plan scene (the set its room load takes,
// z_utstall.c item 5) is read on the reader as btlentry reads a battle's: its missing tiles a page at a time, in btlentry's
// read slots (bt.read, three in flight: btlentry's reader hooks admit and allocate them as its own, and a game thread's
// wait never performs one), and its code entries in file order (bt.codeRead, placed by btlentry's btPlaceCode). The
// reads stay ours (ua.read) until released: btlentry's once-a-frame installs never take one (r220 F1). While its
// collect runs, its calls of CtrTileReader_finished (pointed at CtrUtfadeA_finished by the build) find ours not
// finished; they are installed here, after it, BT_INSTALL_MS a frame. Nothing is evicted for them: a tile is read only
// while the texture budget and linear memory above UA_FLOOR cover it with every tile still in flight, and its texture is
// allocated only while they still cover it, then kept only if linear memory is still at UA_FLOOR or above (a page's
// first read learns the tile sizes only from its header); a tile turned away is released, and the door reads no more.
// UA_FLOOR is flowstream's floor and spare (BT_FLOOR) plus the
// 364 KB that Omega Flowey's whole set leaves after the late-code reserve (change bufexp), so the two rooms' tiles side by
// side never take linear memory below what the boss needs. A battle starting (btlentry's own test, before its collect
// can queue) or the door no longer fading to that room (another door, or none and no room change pending: r220 F2)
// releases our reads and the code read; the tiles already in stay, and the next door arms as usual.
//
// When the room load starts (ctr_textures.c's CtrTextures_roomLoadedOld calls here, then the room load) and the room is
// the door's destination, the reads still in flight are waited for (the room load would read the same bytes on the
// game thread; UA_WAIT_MS at most), the finished ones installed and their code placed with no time limit, under the same
// rules, and every read released. Then the room load finds those tiles resident and those groups in memory, and reads
// only the rest. The room change frame isn't presented, and the black (z_utfade.c) is on screen through it.
//
// Textures and code loads only, as btlentry: no game state, script, timing the game sees or RNG changes; every tile is
// the same texels and every group the same bytes whenever it loads. Its own late sections (late_reserve.ld, after
// dlgtop): it calls none of btlentry's static functions (a new caller would change how they compile, in btlentry's
// fixed slot), only its state and the external functions.
#define UA_TEXT __attribute__((section(".text_utfadea")))
#define UA_CONST __attribute__((aligned(4), section(".rodata.utfadea_in_text")))
#define UA_DOORS 8u
#define UA_FLOOR (BT_FLOOR + 364u * 1024u)
#define UA_WAIT_MS 1000u           // the most the room load waits for the reads in flight
#define UA_SLICE_NS 1000000LL      // a wait's poll
void CtrUtfadeA_collect(CtrTextureCache* cache, bool drop);
void CtrUtfadeA_roomLoad(CtrTextureCache* cache, Runner* runner);
bool uaFinished(CtrTileReader* reader, CtrTileRead* read) __asm__("CtrTileReader_finished"); // the reader's own
void CtrFlowstream_collect(CtrTextureCache* cache, bool drop);       // btlentry's wrap (--wrap)
void CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner); // z_utstall.c's wrap (--wrap)

static struct {
    const DataWin* objectsOf;  // the data.win the object indices are for
    int32_t objects[UA_DOORS]; // the doors' (or -1)
    // From here on, a room visit's (cleared at a room change):
    const CtrTextureCache* cache;
    uint32_t roomStart;
    const uint8_t* scene;      // the destination's plan scene, once a door's fade-out started
    int32_t from, dest;
    uint32_t frames, missing, reads, linearLeast; // frames armed; missing planned tiles at the first look; reads queued
    uint16_t inRam, held, noRoom; // of the missing at the first look: a packed copy in RAM; a read on the page; no room
    CtrTileRead* read[BT_READS]; // ours in btlentry's slots (bt.read[k] the same), until released; their pages
    uint32_t page[BT_READS];
    uint16_t next[BT_READS];     // the next tile of the read to install
    bool code;                   // bt.codeRead, if set, is ours (no battle in this visit queues one)
    volatile bool shield;        // btlentry's collect is running: our reads aren't finished for it
    uint32_t installed, turned, frameDone, tileTicks; // tiles in; turned away at install (no room); this frame's; costliest
} ua __attribute__((section(".bss.utfadea_late")));

// The doors, in the order A, B, C, D: the room order steps +1, -1, +2, -2.
static const char uaNames[] UA_CONST = "obj_doorA\0obj_doorB\0obj_doorC\0obj_doorD\0"
                                       "obj_doorAmusicfade\0obj_doorBmusicfade\0obj_doorCmusicfade\0obj_doorDmusicfade";
static const char uaLog[] UA_CONST =
    "Door ahead (utfade A): room %ld -> %ld: %u of %u missing planned tiles in before the load (%u at its start, "
    "waited %.1f ms, %.1f ms in all), %u reads in %u frames (at the first look %u packed in RAM, %u held by a read, "
    "%u without room; %u turned away at install); code %u groups (%u KB) placed from %u reads, %u not; linear free %u KB "
    "least\n";
static const char uaOther[] UA_CONST = "Door ahead (utfade A): room %ld -> %ld read ahead, but the room is %ld\n";
static const char uaStop[] UA_CONST = "Door ahead (utfade A): room %ld -> %ld stopped (%s), %u tiles in\n";
static const char uaStopBattle[] UA_CONST = "a battle";
static const char uaStopDoor[] UA_CONST = "the door";

// The room the fading door goes to, or -1 (no door's alarm 2 running, or a door that picks its room another way).
static UA_TEXT int32_t uaDoorDest(Runner* runner) {
    const DataWin* dw = runner->dataWin;
    if (ua.objectsOf != dw) {
        ua.objectsOf = dw;
        for (uint32_t i = 0; UA_DOORS > i; i++) ua.objects[i] = -1;
        for (uint32_t o = 0; dw->objt.count > o; o++) {
            const char* name = dw->objt.objects[o].name;
            const char* each = uaNames;
            for (uint32_t i = 0; name != nullptr && UA_DOORS > i; i++, each += strlen(each) + 1u) {
                if (strcmp(name, each) == 0) ua.objects[i] = (int32_t) o;
            }
        }
    }
    if (runner->instancesByExactObject == nullptr) return -1;
    for (uint32_t i = 0; UA_DOORS > i; i++) {
        int32_t o = ua.objects[i];
        if (0 > o) continue;
        Instance** list = runner->instancesByExactObject[o];
        for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
            if (!list[j]->active || list[j]->destroyed || 0 >= list[j]->alarm[2]) continue;
            int32_t step = (i & 2u) != 0 ? 2 : 1;
            int32_t at = -1;
            for (uint32_t r = 0; 0 > at && dw->gen8.roomOrderCount > r; r++) {
                if (dw->gen8.roomOrder[r] == runner->currentRoomIndex) at = (int32_t) r + ((i & 1u) != 0 ? -step : step);
            }
            return 0 <= at && (int32_t) dw->gen8.roomOrderCount > at ? dw->gen8.roomOrder[at] : -1;
        }
    }
    return -1;
}

// The room's own plan scene (sub ROOMPLAN_NO_SUB), or nullptr.
static UA_TEXT const uint8_t* uaScene(const uint8_t* plan, int32_t room) {
    RoomPlanHeader h;
    memcpy(&h, plan, sizeof(h));
    size_t at = sizeof(h);
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        memcpy(&scene, plan + at, sizeof(scene));
        if (scene.sub == ROOMPLAN_NO_SUB && (int32_t) scene.room == room) return plan + at;
        at += RoomPlan_sceneSize(&scene);
    }
    return nullptr;
}

static UA_TEXT uint32_t uaTileBytes(const CtrPage* page, const CtrTile* tile) {
    return (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
}

// Texture budget and linear memory above UA_FLOOR for bytes more, with nothing evicted.
static UA_TEXT bool uaRoomFor(const CtrTextureCache* cache, uint32_t bytes) {
    uint32_t linear = linearSpaceFree();
    if (ua.linearLeast > linear) ua.linearLeast = linear;
    return cache->budgetBytes >= cache->residentBytes && cache->budgetBytes - cache->residentBytes >= bytes && linear >= UA_FLOOR &&
           linear - UA_FLOOR >= bytes;
}

// The texture bytes of the tiles our reads in flight still hold.
static UA_TEXT uint32_t uaInFlight(const CtrTextureCache* cache) {
    uint32_t bytes = 0;
    for (uint32_t k = 0; BT_READS > k; k++) {
        const CtrTileRead* read = ua.read[k];
        const CtrPage* page = read != nullptr ? &cache->pages[ua.page[k]] : nullptr;
        if (page == nullptr || page->read != read || !page->packInfo) continue;
        for (int32_t t = 0; page->tilesX * page->tilesY > t; t++) {
            if (read->want[t] && !page->tiles[t].valid) bytes += uaTileBytes(page, &page->tiles[t]);
        }
    }
    return bytes;
}

// A planned tile: 0 not missing; 2 readable now (btlentry's btReadable, with the tiles in flight and those taken so far,
// extra, counted against the budget and the floor); else missing, not readable now: 3 its page's packed copy in RAM
// (the room load takes it from there, no SD read), 4 a read holds the page, 5 no room, 1 retrying after a failure.
// Its page, tile and texture bytes out.
static UA_TEXT uint32_t uaReadable(CtrTextureCache* cache, uint32_t entry, uint32_t extra, CtrPage** outPage, uint32_t* outTile,
                                   uint32_t* outBytes) {
    uint32_t p = (entry & BT_KEY_MASK) >> 8;
    uint32_t t = entry & 0xFFu;
    if (p >= cache->pageCount) return 0;
    CtrPage* page = &cache->pages[p];
    if (page->failed || page->cacheFile == CTR_CACHE_FILE_MISSING || cache->dataWin->txtr.textures[p].externalPath != nullptr) return 0;
    uint32_t bytes = 0;
    if (page->layoutKnown) {
        if (t >= (uint32_t) (page->tilesX * page->tilesY)) return 0;
        const CtrTile* tile = &page->tiles[t];
        if (tile->valid || (page->formatsKnown && tile->format == CTR_TILE_EMPTY)) return 0;
        if (page->packInfo) bytes = uaTileBytes(page, tile);
    }
    *outPage = page;
    *outTile = t;
    *outBytes = bytes;
    if (cache->frame < page->retryFrame) return 1;
    if (page->packed != nullptr) return 3;
    if (page->read != nullptr) return 4;
    return uaRoomFor(cache, extra + bytes) ? 2 : 5;
}

static UA_TEXT bool uaHeapFor(uint32_t bytes) {
    struct mallinfo mi = mallinfo();
    return __ctru_heap_size >= (uint32_t) mi.uordblks + CtrTileReader_heapReserve() + bytes;
}

static UA_TEXT CtrTileRead* uaNewRead(uint32_t bytes) {
    if (!uaHeapFor(bytes)) return nullptr;
    bool allowed = CtrMemory_allowRelief(false);
    CtrTileRead* read = CtrTileReader_newRead();
    CtrMemory_allowRelief(allowed);
    return read;
}

// The scene's missing tiles, a page's in one read, in btlentry's free read slots.
static UA_TEXT void uaQueueTiles(CtrTextureCache* cache, bool first) {
    RoomPlanScene s;
    memcpy(&s, ua.scene, sizeof(s));
    const uint8_t* entries = ua.scene + sizeof(s);
    uint32_t extra = uaInFlight(cache);
    uint32_t k = 0;
    for (uint32_t i = 0; s.tileCount > i; i++) {
        while (BT_READS > k && (bt.read[k] != nullptr || ua.read[k] != nullptr)) k++;
        if (BT_READS == k && !first) break;
        uint32_t entry;
        memcpy(&entry, entries + (size_t) i * 4u, 4);
        CtrPage* page = nullptr;
        uint32_t t = 0, bytes = 0;
        uint32_t state = uaReadable(cache, entry, extra, &page, &t, &bytes);
        if (first && state > 0) ua.missing++, ua.inRam += state == 3, ua.held += state == 4, ua.noRoom += state == 5;
        if (state != 2 || BT_READS == k || cache->fillPending) continue;
        uint32_t p = (uint32_t) (page - cache->pages);
        uint8_t want[BT_READ_TILES];
        uint32_t tiles = 0, size = 0, tex = 0;
        for (uint32_t j = i; s.tileCount > j && BT_READ_TILES > tiles; j++) {
            uint32_t other;
            memcpy(&other, entries + (size_t) j * 4u, 4);
            CtrPage* same = nullptr;
            uint32_t u = 0, b = 0;
            if (((other & BT_KEY_MASK) >> 8) != p || memchr(want, (int) (other & 0xFFu), tiles) != nullptr ||
                uaReadable(cache, other, extra + tex, &same, &u, &b) != 2)
                continue;
            uint32_t packed = page->packInfo ? page->tiles[u].size : 0;
            if (tiles > 0 && size + packed > BT_READ_BYTES) break;
            want[tiles++] = (uint8_t) u;
            size += packed;
            tex += b;
        }
        CtrTileRead* read = uaNewRead(size);
        if (read == nullptr) break;
        for (uint32_t j = 0; tiles > j; j++) read->want[want[j]] = true;
        const Texture* txtr = &cache->dataWin->txtr.textures[p];
        read->pageId = p;
        read->blobOffset = txtr->blobOffset;
        read->blobSize = txtr->blobSize;
        page->read = read;
        bt.page[k] = p;
        bt.next[k] = 0;
        bt.dropped[k] = false;
        ua.read[k] = read; // ours (the shield's), then btlentry's slot: both before the queue's lock hands it to the reader
        ua.page[k] = p;
        ua.next[k] = 0;
        bt.read[k] = read;
        ua.reads++;
        extra += tex;
        CtrTileReader_queue(cache->reader, read);
    }
}

// ---- The scene's code (btlentry round 5's, for the destination's scene).

static UA_TEXT uint32_t uaBlobOf(const DataWin* dw, uint32_t offset) {
    uint32_t low = 0, high = dw->codeBlobCount;
    while (high > low) {
        uint32_t mid = low + (high - low) / 2u;
        if (offset > dw->codeBlobs[mid].absoluteOffset) low = mid + 1u;
        else high = mid;
    }
    return low;
}

static UA_TEXT uint32_t uaGroupEnd(const DataWin* dw, uint32_t first) {
    uint32_t end = 0;
    for (uint32_t b = first; dw->codeBlobCount > b && dw->codeBlobs[b].group == first; b++) {
        uint32_t blobEnd = dw->codeBlobs[b].absoluteOffset + dw->codeBlobs[b].length;
        if (blobEnd > end) end = blobEnd;
    }
    return end;
}

static UA_TEXT uint32_t uaNextGroup(const DataWin* dw, uint32_t from) {
    RoomPlanScene s;
    memcpy(&s, ua.scene, sizeof(s));
    const uint8_t* codes = ua.scene + sizeof(s) + (size_t) s.tileCount * 4u + (((size_t) s.tileCount + 3u) & ~(size_t) 3u);
    uint32_t best = UINT32_MAX, bestAt = UINT32_MAX;
    for (uint32_t i = 0; s.codeCount > i; i++) {
        uint16_t index;
        memcpy(&index, codes + (size_t) i * 2u, 2);
        if (index >= dw->code.count) continue;
        const CodeEntry* code = &dw->code.entries[index];
        if (!code->present || code->bytecode != nullptr) continue;
        uint32_t b = uaBlobOf(dw, code->bytecodeAbsoluteOffset);
        if (b >= dw->codeBlobCount || dw->codeBlobs[b].absoluteOffset != code->bytecodeAbsoluteOffset || dw->codeBlobs[b].bytes != nullptr)
            continue;
        uint32_t g = dw->codeBlobs[b].group;
        uint32_t at = dw->codeBlobs[g].absoluteOffset;
        if (at >= from && bestAt > at) best = g, bestAt = at;
    }
    return best;
}

static UA_TEXT void uaQueueCode(CtrTextureCache* cache) {
    DataWin* dw = cache->dataWin;
    if (bt.codeRead != nullptr || bt.codeAt == UINT32_MAX || bt.codeOf != dw || dw->bytecodeFile == nullptr || dw->codeBlobs == nullptr)
        return;
    uint32_t g = uaNextGroup(dw, bt.codeAt);
    if (g == UINT32_MAX) {
        bt.codeAt = UINT32_MAX;
        return;
    }
    CtrTileRead* read = uaNewRead(BT_CODE_BYTES);
    if (read == nullptr) return;
    uint32_t base = (uint32_t) dw->bytecodeBufferBase;
    uint32_t groups = 0, total = 0, row = 0, runStart = 0, runEnd = 0;
    while (g != UINT32_MAX && CTR_PAGE_MAX_TILES > groups) {
        uint32_t start = dw->codeBlobs[g].absoluteOffset, end = uaGroupEnd(dw, g);
        bool join = groups > 0 && runEnd + BT_CODE_GAP >= start;
        uint32_t add = end - (join ? runEnd : start);
        if (total + add > BT_CODE_BYTES || (!join && groups > 0 && row + 1u == BT_CODE_SPANS)) break;
        total += add;
        if (!join && groups > 0) row++;
        if (!join) runStart = start;
        runEnd = end;
        read->want[row] = true;
        read->view.tileOffset[row] = BT_CODE_HEADER + runStart - base;
        read->view.tileSize[row] = runEnd - runStart;
        read->dstSize[groups] = g;
        read->dstFormat[groups++] = (uint8_t) row;
        g = uaNextGroup(dw, end);
    }
    bt.codeAt = groups > 0 ? runEnd : dw->codeBlobs[g].absoluteOffset + 1u;
    if (groups == 0) {
        CtrTileReader_release(cache->reader, read);
        return;
    }
    read->view.layout.tilesX = 1;
    read->view.layout.tilesY = (int32_t) row + 1;
    read->view.totalSize = BT_CODE_HEADER + dw->codeStats.totalBytes;
    read->pageId = BT_CODE_PAGE;
    bt.codeGroups = groups;
    ua.code = true;
    bt.codeRead = read;
    bt.codeReads++;
    CtrTileReader_queue(cache->reader, read);
}

// btlentry's btArena and btPlaceCode, for the code read finished when the room load starts.
static UA_TEXT uint8_t* uaArena(DataWin* dw, uint32_t size) {
    size = (size + 3u) & ~3u;
    bool own = size > BT_ARENA_BLOCK / 4u;
    uint32_t take = own ? size : size > dw->codeArenaLeft ? BT_ARENA_BLOCK : 0;
    if (take > 0) {
        stbds_array_header* list = dw->codeArenaBlocks != nullptr ? stbds_header(dw->codeArenaBlocks) : nullptr;
        uint8_t* block = list != nullptr && list->capacity > list->length && uaHeapFor(take) ? malloc(take) : nullptr;
        if (block == nullptr) return nullptr;
        dw->codeArenaBlocks[list->length++] = block;
        dw->codeStats.residentBytes += take;
        if (own) return block;
        dw->codeArena = block;
        dw->codeArenaLeft = BT_ARENA_BLOCK;
    }
    uint8_t* at = dw->codeArena;
    dw->codeArena += size;
    dw->codeArenaLeft -= size;
    return at;
}

static UA_TEXT void uaPlaceCode(CtrTextureCache* cache) {
    CtrTileRead* read = bt.codeRead;
    bt.codeRead = nullptr;
    if (read == nullptr) return;
    if (uaFinished(cache->reader, read)) {
        DataWin* dw = cache->dataWin;
        DataWinCodeBlob* blobs = dw->codeBlobs;
        uint32_t base = (uint32_t) dw->bytecodeBufferBase;
        bool allowed = CtrMemory_allowRelief(false);
        for (uint32_t i = 0; bt.codeGroups > i; i++) {
            uint32_t g = read->dstSize[i], row = read->dstFormat[i];
            if (blobs[g].bytes != nullptr) continue;
            uint32_t start = blobs[g].absoluteOffset, end = uaGroupEnd(dw, g);
            uint8_t* at = read->headerOk && read->data != nullptr && read->tileAt[row] != CTR_READ_NOT_READ ? uaArena(dw, end - start) : nullptr;
            if (at == nullptr) {
                bt.codeLeft++;
                continue;
            }
            memcpy(at, read->data + read->tileAt[row] + (BT_CODE_HEADER + start - base - read->view.tileOffset[row]), end - start);
            for (uint32_t b = g; dw->codeBlobCount > b && blobs[b].group == g; b++) blobs[b].bytes = at + (blobs[b].absoluteOffset - start);
            bt.codePlaced++;
            bt.codeBytes += end - start;
        }
        CtrMemory_allowRelief(allowed);
    }
    CtrTileReader_release(cache->reader, read); // one not finished is abandoned to the reader, as releaseReads does
}

// btlentry's btReserve.
static UA_TEXT bool uaReserve(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    uint32_t bytes = uaTileBytes(page, tile);
    if (tile->reserved || !C3D_TexInit(&tile->tex, (u16) page->tileW, (u16) page->tileH, CtrPack_gpuFormat((CtrTileFormat) tile->format)))
        return false;
    if (tile->tex.size != bytes) {
        C3D_TexDelete(&tile->tex);
        return false;
    }
    C3D_TexSetFilter(&tile->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&tile->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    tile->reserved = true;
    page->bytes += bytes;
    cache->residentBytes += bytes;
    return true;
}

// ctr_textures.c's unreserveTile, for a texture allocated above and turned away.
static UA_TEXT void uaUnreserve(CtrTextureCache* cache, CtrPage* page, CtrTile* tile) {
    uint32_t bytes = tile->tex.size;
    C3D_TexDelete(&tile->tex);
    tile->reserved = false;
    page->bytes -= bytes;
    cache->residentBytes -= bytes;
}

// Our finished read k's tiles from ua.next[k] on (btlentry's install loop, with no eviction): each tile's texture is
// allocated only while the budget and linear memory above UA_FLOOR cover it, and kept only if linear memory is still
// at UA_FLOOR or above after (a page's first read learned the tile's size only from its header, and the allocation
// may take more than its bytes); else turned away. Timed: one tile a frame, then more only while the frame's installs
// stay within BT_INSTALL_MS (by the costliest tile so far). True once the read is through.
static UA_TEXT bool uaInstall(CtrTextureCache* cache, uint32_t k, bool timed, uint64_t start) {
    CtrTileRead* read = ua.read[k];
    CtrPage* page = &cache->pages[ua.page[k]];
    const CtrPageLayout* layout = &read->view.layout;
    bool allowed = CtrMemory_allowRelief(false);
    if (read->headerOk && !page->layoutKnown && (page->tiles = calloc((size_t) (layout->tilesX * layout->tilesY), sizeof(CtrTile))) != nullptr) {
        page->width = layout->width;
        page->height = layout->height;
        page->tileW = layout->tileW;
        page->tileH = layout->tileH;
        page->tilesX = layout->tilesX;
        page->tilesY = layout->tilesY;
        page->layoutKnown = true;
    }
    CtrMemory_allowRelief(allowed);
    if (read->headerOk && page->layoutKnown && !page->packInfo && page->width == layout->width && page->height == layout->height &&
        page->tileW == layout->tileW && page->tileH == layout->tileH && page->tilesX == layout->tilesX && page->tilesY == layout->tilesY) {
        for (int32_t i = 0; page->tilesX * page->tilesY > i; i++) {
            CtrTile* each = &page->tiles[i];
            each->format = layout->formats[i];
            each->offset = read->view.tileOffset[i];
            each->size = read->view.tileSize[i];
            each->checksum = read->view.tileChecksum[i];
        }
        page->formatsKnown = true;
        page->packInfo = true;
        page->cacheFile = CTR_CACHE_FILE_VALID;
    }
    uint64_t budget = (uint64_t) BT_INSTALL_MS * (SYSCLOCK_ARM11 / 1000u);
    for (uint32_t t = ua.next[k]; CTR_PAGE_MAX_TILES > t; t++) {
        if (!read->want[t]) continue;
        if (timed && ua.frameDone > 0 && svcGetSystemTick() - start + ua.tileTicks > budget) {
            ua.next[k] = (uint16_t) t;
            return false;
        }
        CtrTile* tile = page->packInfo && t < (uint32_t) (page->tilesX * page->tilesY) ? &page->tiles[t] : nullptr;
        if (tile == nullptr || tile->valid || !read->headerOk || read->data == nullptr || read->tileAt[t] == CTR_READ_NOT_READ ||
            tile->offset != read->view.tileOffset[t] || tile->size != read->view.tileSize[t])
            continue;
        if (!uaRoomFor(cache, uaTileBytes(page, tile)) || !uaReserve(cache, page, tile)) {
            ua.turned++;
            continue;
        }
        uint32_t linear = linearSpaceFree();
        if (ua.linearLeast > linear) ua.linearLeast = linear;
        if (UA_FLOOR > linear) {
            uaUnreserve(cache, page, tile);
            ua.turned++;
            continue;
        }
        ua.frameDone++;
        uint64_t before = svcGetSystemTick();
        bool ok = CtrTextures_btlentryInstall(cache, page, (int32_t) t);
        uint32_t took = (uint32_t) (svcGetSystemTick() - before);
        if (took > ua.tileTicks) ua.tileTicks = took;
        ua.installed += ok;
    }
    ua.next[k] = CTR_PAGE_MAX_TILES;
    return true;
}

// Our read k released (with the page's, if the cache didn't take it: a texture reserved and not installed is freed),
// and btlentry's slot with it.
static UA_TEXT void uaRelease(CtrTextureCache* cache, uint32_t k) {
    CtrTileRead* read = ua.read[k];
    ua.read[k] = nullptr;
    if (read == nullptr) return;
    CtrPage* page = &cache->pages[ua.page[k]];
    if (page->read == read) CtrTextures_btlentryInstall(cache, page, -1);
    if (bt.read[k] == read) bt.read[k] = nullptr;
}

// btlentry's btStarting, read only (calling it would change how it compiles): a battle's objects in the room.
static UA_TEXT bool uaBattle(Runner* runner) {
    if (bt.objectsOf != runner->dataWin || runner->instancesByExactObject == nullptr) return false;
    for (uint32_t k = 0; 2 > k; k++) {
        int32_t o = bt.objects[k];
        if (0 <= o && arrlen(runner->instancesByExactObject[o]) > 0) return true;
    }
    return false;
}

// The door's read ahead stopped: our reads released (still queued: taken off the queue; in flight: left to the reader
// as the cache's releaseRead does), and our code read; unless a battle has them (bt.scene: btlentry's from then on),
// the code's place is reset for an armed door, even one that queued no code (its scene's code all in memory left the
// place at the end: r220b). The tiles already in stay; the next door arms as the first.
static UA_TEXT void uaCancel(CtrTextureCache* cache, const char* why) {
    for (uint32_t k = 0; BT_READS > k; k++) uaRelease(cache, k);
    if (bt.scene == nullptr) {
        if (ua.code) {
            if (bt.codeRead != nullptr) CtrTileReader_release(cache->reader, bt.codeRead);
            bt.codeRead = nullptr;
        }
        if (ua.code || ua.scene != nullptr) bt.codeAt = 0;
    }
    ua.code = false;
    if (ua.scene != nullptr) logInfo(uaStop, (long) ua.from, (long) ua.dest, why, (unsigned) ua.installed);
    ua.scene = nullptr;
    ua.frames = ua.missing = ua.reads = ua.installed = ua.turned = 0;
    ua.inRam = ua.held = ua.noRoom = 0;
}

// btlentry's calls of CtrTileReader_finished (the build points them at CtrUtfadeA_finished): while its collect runs,
// our reads aren't finished, so its installs never take one. CtrUtfadeA_finished is this function's second name, set
// in the assembler, so the compiler sees btlentry's calls as calls out of this file, as before (its slot compiles the
// same, but for those call targets).
#ifdef CtrTileReader_finished
bool CtrUtfadeA_shield(CtrTileReader* reader, CtrTileRead* read);
__attribute__((noipa, used)) UA_TEXT bool CtrUtfadeA_shield(CtrTileReader* reader, CtrTileRead* read) {
    for (uint32_t k = 0; ua.shield && BT_READS > k; k++) {
        if (read != nullptr && ua.read[k] == read) return false;
    }
    return uaFinished(reader, read);
}
__asm__(".global CtrUtfadeA_finished\n\t.type CtrUtfadeA_finished, %function\n\t.set CtrUtfadeA_finished, CtrUtfadeA_shield");
#endif

// Once a frame (z_utstall.c's utflPkCollect): collect btlentry and flowstream before the door's reads.
// A fading door keeps its reads ahead; installs wait for the hidden matched room load.
UA_TEXT void CtrUtfadeA_collect(CtrTextureCache* cache, bool drop) {
    Runner* runner = cache != nullptr ? cache->runner : nullptr;
    if (runner == nullptr || cache->reader == nullptr) {
        CtrFlowstream_collect(cache, drop);
        return;
    }
    if (ua.cache != cache || ua.roomStart != cache->roomStartFrame) {
        // A room change: the cache released our reads with the room's, and btlentry's collect clears its slots.
        memset((void*) &ua.cache, 0, sizeof(ua) - offsetof(__typeof__(ua), cache));
        ua.cache = cache;
        ua.roomStart = cache->roomStartFrame;
    }
    // A battle starting (btStarting's test, as btlentry makes it after its installs): before btlentry's collect can
    // queue a read or install by its rules, ours are gone.
    bool visit = bt.cache == cache && bt.roomStart == cache->roomStartFrame;
    if (visit && (bt.scene != nullptr || uaBattle(runner))) uaCancel(cache, uaStopBattle);
    ua.shield = true;
    CtrFlowstream_collect(cache, drop);
    ua.shield = false;
    // Not before btlentry has seen this room visit; nor once a battle starts (btlentry has the slots).
    if (bt.cache != cache || bt.roomStart != cache->roomStartFrame) return;
    if (bt.scene != nullptr || uaBattle(runner)) {
        uaCancel(cache, uaStopBattle);
        return;
    }
    // The door no longer fading to that room (another door, or none with no room change pending; a pending one is the
    // door's Alarm 2 done, and the room load checks the room it goes to): stopped before any of its tiles go in.
    int32_t dest = uaDoorDest(runner);
    if (ua.scene != nullptr && dest != ua.dest && (0 <= dest || 0 > runner->pendingRoom)) uaCancel(cache, uaStopDoor);
    ua.frameDone = 0;
    for (uint32_t k = 0; BT_READS > k; k++) {
        CtrTileRead* read = ua.read[k];
        if (read == nullptr) continue;
        if (cache->pages[ua.page[k]].read != read) { // the cache took it (drawn, or released)
            ua.read[k] = nullptr;
            if (bt.read[k] == read) bt.read[k] = nullptr;
            continue;
        }
        // Installing in the collector can miss a visible fade frame. Finished
        // reads wait for the matched load; visit every slot for ownership cleanup.
    }
    const uint8_t* plan = RoomPlan_utstallPlan(cache);
    if (plan == nullptr) return;
    bool first = ua.scene == nullptr;
    if (first) {
        const uint8_t* scene = 0 <= dest && dest != runner->currentRoomIndex ? uaScene(plan, dest) : nullptr;
        if (scene == nullptr) return;
        ua.scene = scene;
        ua.from = runner->currentRoomIndex;
        ua.dest = dest;
        ua.linearLeast = UINT32_MAX;
    }
    ua.frames++;
    uaQueueCode(cache);
    if (ua.turned == 0) uaQueueTiles(cache, first); // once a tile is turned away, no more reads for this door
}

// The room load (ctr_textures.c's CtrTextures_roomLoadedOld calls here in place of z_utstall.c's wrap): for the door's
// destination, its reads finished and put in first (no time limit, the same rules); every read of ours released.
UA_TEXT void CtrUtfadeA_roomLoad(CtrTextureCache* cache, Runner* runner) {
    if (ua.scene != nullptr && ua.cache == cache && cache->reader != nullptr && runner != nullptr) {
        bool match = runner->currentRoomIndex == ua.dest; // else (a door that went elsewhere) only released
        uint64_t start = svcGetSystemTick(), tick = SYSCLOCK_ARM11 / 1000u;
        for (;;) {
            bool waiting = bt.codeRead != nullptr && !uaFinished(cache->reader, bt.codeRead);
            for (uint32_t k = 0; BT_READS > k; k++) {
                CtrTileRead* read = ua.read[k];
                if (read != nullptr && cache->pages[ua.page[k]].read == read && !uaFinished(cache->reader, read)) waiting = true;
            }
            if (!match || !waiting || svcGetSystemTick() - start >= (uint64_t) UA_WAIT_MS * tick) break;
            svcSleepThread(UA_SLICE_NS);
        }
        uint64_t waited = svcGetSystemTick() - start;
        if (!match && bt.codeRead != nullptr) {
            CtrTileReader_release(cache->reader, bt.codeRead);
            bt.codeRead = nullptr;
        }
        uaPlaceCode(cache);
        uint32_t before = ua.installed;
        for (uint32_t k = 0; BT_READS > k; k++) {
            CtrTileRead* read = ua.read[k];
            if (read == nullptr) continue;
            if (match && cache->pages[ua.page[k]].read == read && uaFinished(cache->reader, read)) uaInstall(cache, k, false, start);
            uaRelease(cache, k);
        }
        uint32_t atLoad = ua.installed - before;
        uaRoomFor(cache, 0); // the linear memory left with both rooms' tiles in
        if (match)
            logInfo(uaLog, (long) ua.from, (long) ua.dest, (unsigned) ua.installed, (unsigned) ua.missing, (unsigned) atLoad,
                    (double) waited / (double) tick, (double) (svcGetSystemTick() - start) / (double) tick, (unsigned) ua.reads,
                    (unsigned) ua.frames, (unsigned) ua.inRam, (unsigned) ua.held, (unsigned) ua.noRoom, (unsigned) ua.turned,
                    (unsigned) bt.codePlaced, (unsigned) ((bt.codeBytes + 1023u) / 1024u), (unsigned) bt.codeReads, (unsigned) bt.codeLeft,
                    (unsigned) (ua.linearLeast / 1024u));
        else
            logInfo(uaOther, (long) ua.from, (long) ua.dest, (long) runner->currentRoomIndex);
        ua.scene = nullptr;
        ua.code = false;
    }
    CtrRoomload_prefetchRoom(cache, runner);
}
// ---- bthand (change tilemiss, 11-08; UNDERTALE Old): btlentry's reads handed to the battle room's load.
// Round 10 of the room plan (change tilemiss) plans the art a battle group's monsters create and switch to, so a group's
// whole set grew by up to 8 tiles; btlentry installs about 35-44 tiles before the room change (none while the soul
// flies, round 6), and the rest loaded on the room change frame, with the soul still on the black: 94 -> 187 ms there in
// group 32 on Azahar Old with the console's memory. Its reads keep reading during the flight, and a finished one's
// bytes waited in its slot until the room change released them; the room load then read the same tiles again from
// the SD card, on the game thread.
// So, when the room load starts (ctr_textures.c's CtrTextures_roomLoadedOld calls here, then utfade A's, then the room
// load) and the room is the one btlentry read ahead for (its plan scene's room: the battle room, or a story battle's),
// btlentry's reads still in flight are waited for (BH_WAIT_MS at most: the room load would read the same bytes on the
// game thread), and each finished one's tiles not yet in go in with no time limit, by btlentry's install rules with
// nothing evicted (texture budget, and linear memory above BT_FLOOR after the tile); then the read is released, as the
// room load's releaseReads would. The room change frame isn't presented: the black and the soul stay on screen through
// it. A tile without room is left to the room load, which evicts for its whole set as before. A read of a door (utfade
// A's, in the same slot) is left to utfade A.
// Textures only, as btlentry: no game state, script, timing the game sees or RNG changes; every tile is the same texels
// whenever it loads. Its own appended late sections (late_reserve.ld); it calls none of this file's static functions
// (a new caller would change how they compile) and reaches utfade A's room load by its assembler name.
#define BH_TEXT __attribute__((noinline, section(".text.bthand_late")))
#define BH_CONST __attribute__((aligned(4), section(".rodata.bthand_in_text")))
#define BH_WAIT_MS 1000u // the most the room load waits for btlentry's reads in flight
void CtrBtHand_roomLoad(CtrTextureCache* cache, Runner* runner);
void bhUtfadeRoomLoad(CtrTextureCache* cache, Runner* runner) __asm__("CtrUtfadeA_roomLoad");
static const char bhLog[] BH_CONST =
    "Battle entry (bthand): group %ld: %u of %u reads finished at the room load (waited %.1f ms), %u more tiles in "
    "(%u without room left to the load), %.1f ms in all\n";

BH_TEXT void CtrBtHand_roomLoad(CtrTextureCache* cache, Runner* runner) {
    RoomPlanScene s;
    if (bt.scene != nullptr && bt.cache == cache && cache->reader != nullptr && runner != nullptr &&
        (memcpy(&s, bt.scene, sizeof(s)), s.room == (uint32_t) runner->currentRoomIndex)) {
        uint64_t start = svcGetSystemTick(), tick = SYSCLOCK_ARM11 / 1000u;
        for (;;) {
            bool waiting = false;
            for (uint32_t k = 0; BT_READS > k; k++) {
                CtrTileRead* read = bt.read[k];
                if (read != nullptr && ua.read[k] != read && cache->pages[bt.page[k]].read == read && !uaFinished(cache->reader, read))
                    waiting = true;
            }
            if (!waiting || svcGetSystemTick() - start >= (uint64_t) BH_WAIT_MS * tick) break;
            svcSleepThread(UA_SLICE_NS);
        }
        uint64_t waited = svcGetSystemTick() - start;
        uint32_t reads = 0, finished = 0, in = 0, noRoom = 0;
        for (uint32_t k = 0; BT_READS > k; k++) {
            CtrTileRead* read = bt.read[k];
            if (read == nullptr || ua.read[k] == read) continue;
            CtrPage* page = &cache->pages[bt.page[k]];
            if (page->read != read) continue; // the cache took it
            reads++;
            if (!uaFinished(cache->reader, read)) continue; // left to the room load's releaseReads
            finished++;
            const CtrPageLayout* layout = &read->view.layout;
            bool allowed = CtrMemory_allowRelief(false);
            if (read->headerOk && !page->layoutKnown && (page->tiles = calloc((size_t) (layout->tilesX * layout->tilesY), sizeof(CtrTile))) != nullptr) {
                page->width = layout->width;
                page->height = layout->height;
                page->tileW = layout->tileW;
                page->tileH = layout->tileH;
                page->tilesX = layout->tilesX;
                page->tilesY = layout->tilesY;
                page->layoutKnown = true;
            }
            CtrMemory_allowRelief(allowed);
            if (read->headerOk && page->layoutKnown && !page->packInfo && page->width == layout->width && page->height == layout->height &&
                page->tileW == layout->tileW && page->tileH == layout->tileH && page->tilesX == layout->tilesX && page->tilesY == layout->tilesY) {
                for (int32_t i = 0; page->tilesX * page->tilesY > i; i++) {
                    CtrTile* each = &page->tiles[i];
                    each->format = layout->formats[i];
                    each->offset = read->view.tileOffset[i];
                    each->size = read->view.tileSize[i];
                    each->checksum = read->view.tileChecksum[i];
                }
                page->formatsKnown = true;
                page->packInfo = true;
                page->cacheFile = CTR_CACHE_FILE_VALID;
            }
            for (uint32_t t = bt.next[k]; CTR_PAGE_MAX_TILES > t; t++) {
                if (!read->want[t]) continue;
                CtrTile* tile = page->packInfo && t < (uint32_t) (page->tilesX * page->tilesY) ? &page->tiles[t] : nullptr;
                if (tile == nullptr || tile->valid || tile->reserved || !read->headerOk || read->data == nullptr ||
                    read->tileAt[t] == CTR_READ_NOT_READ || tile->offset != read->view.tileOffset[t] || tile->size != read->view.tileSize[t])
                    continue;
                uint32_t bytes = (uint32_t) page->tileW * (uint32_t) page->tileH * CtrPack_texelBytes((CtrTileFormat) tile->format);
                uint32_t linear = linearSpaceFree();
                if (cache->residentBytes + bytes > cache->budgetBytes || BT_FLOOR > linear || linear - BT_FLOOR < bytes ||
                    !C3D_TexInit(&tile->tex, (u16) page->tileW, (u16) page->tileH, CtrPack_gpuFormat((CtrTileFormat) tile->format))) {
                    noRoom++;
                    continue;
                }
                if (tile->tex.size != bytes || BT_FLOOR > linearSpaceFree()) { // utfade A's r220 F1: the floor after it too
                    C3D_TexDelete(&tile->tex);
                    noRoom++;
                    continue;
                }
                C3D_TexSetFilter(&tile->tex, GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetWrap(&tile->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
                tile->reserved = true; // btlentry's btReserve: installTile takes it as allocated
                page->bytes += bytes;
                cache->residentBytes += bytes;
                in += CtrTextures_btlentryInstall(cache, page, (int32_t) t);
            }
            CtrTextures_btlentryInstall(cache, page, -1); // released: frees a reserved texture whose install failed
            bt.read[k] = nullptr;
        }
        if (reads > 0)
            logInfo(bhLog, (long) bt.group, (unsigned) finished, (unsigned) reads, (double) waited / (double) tick, (unsigned) in,
                    (unsigned) noRoom, (double) (svcGetSystemTick() - start) / (double) tick);
    }
    bhUtfadeRoomLoad(cache, runner);
}
#endif
