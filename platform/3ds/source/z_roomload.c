// roomload: the room change finishes the new room's known reads before its first frame is shown
// The frame that changes rooms isn't presented (main.c), so what it spends here
// lengthens the black gap of the transition instead of making the room's first shown frames stutter, which is
// the better trade (a slightly longer room load buys a smooth room).
//
// CtrRoomload_prefetchRoom runs inside argui's wrapper of CtrTextures_prefetchRoom (ctr_argui.c), within a shared budget:
//   1. the bytecode of the room's objects' events is read back from its file (Old models, where it lives there), in
//      file order, neighbouring groups in one read, before the tile reader starts;
//   then the original marks the room's tiles and starts their read-ahead, unchanged;
//   2. sprites those objects' code assigns as constants to sprite variables (sprite_index, Toriel's usprite...) are
//      marked wanted;
//   3. the room's tiles are loaded, within the texture budget: those its first frame draws (making room as its draw
//      would), then the other wanted ones that are in RAM, read, or in cache files that fit the time left (ctr_textures.c,
//      CtrTextures_roomLoadFinish).
//
// All of it is late code, constants and statics (platform/3ds/late_reserve.ld): this file adds nothing to .text,
// .rodata or .bss, so nothing that existed before moves.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_textures.h"
#include "data_win.h"
#include "gettime.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#define RL_CODE __attribute__((noinline, section(".text.late.roomload")))
#define RL_RODATA __attribute__((section(".rodata.roomload_in_text")))
#define RL_BSS __attribute__((section(".bss.late.roomload")))

// The added work's budget, the code reads before the original marking and the rest after it: measured on the Old
// 2DS.
#define ROOMLOAD_BUDGET_NANOS (200ull * 1000000ull)
// Bytecode reads stop this far in, leaving the rest for the tiles.
#define ROOMLOAD_CODE_NANOS (120ull * 1000000ull)
// Event code entries and statically assigned sprites taken per room at most.
#define ROOMLOAD_CODE_MAX 1024
#define ROOMLOAD_SPRITES_MAX 64
// One read covers neighbouring groups up to this many bytes, and skips gaps of at most this many between them.
#define ROOMLOAD_SPAN_BYTES (64u * 1024u)
#define ROOMLOAD_GAP_BYTES 256u
// Bytecode read for one room at most (the lazy path would read the same, a group at a time).
#define ROOMLOAD_CODE_BYTES (256u * 1024u)
#define ROOMLOAD_PARENT_DEPTH 32
// 0: a read's groups get their code memory first, then the transient buffer is taken and given back with nothing
// allocated in between, so it leaves no hole (malloc/free in newlib coalesce it back).
#define ROOMLOAD_TRANSIENT_FIRST 0

// OBJT event types (GameMaker's numbering) skipped: Create has run, the others run at most once, at the end.
#define EVENT_CREATE 0
#define EVENT_DESTROY 1
#define EVENT_CLEANUP 12
#define EVENT_PRECREATE 14

void __real_CtrTextures_prefetchRoom(CtrTextureCache* cache, Runner* runner);
void CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner);
uint32_t CtrTextures_roomLoadSpriteRoom(CtrTextureCache* cache);
uint32_t CtrTextures_roomLoadSprite(CtrTextureCache* cache, int32_t spriteIndex, uint32_t room);
void CtrTextures_roomLoadFinish(CtrTextureCache* cache, uint64_t deadline, uint32_t* nowTiles, uint32_t* otherTiles);

static struct {
    int32_t codeIds[ROOMLOAD_CODE_MAX];
    uint32_t groups[ROOMLOAD_CODE_MAX];
    int32_t sprites[ROOMLOAD_SPRITES_MAX];
    const DataWin* headerFor; // the data.win headerBytes was found for
    long headerBytes;         // the bytecode file's header, before the bytecode
    uint64_t copyNanos;       // this room load's copying from the transient buffers
} rl RL_BSS;

static const char rlLogFormat[] RL_RODATA =
    "Room load: %s code %u groups in %u reads, %u KB in %.0f ms (copy %.1f); %u sprites; tiles %u first, %u more; %.0f ms in all%s; %u tiles evicted for the first frame\n";
static const char rlCutShort[] RL_RODATA = " (budget reached)";
static const char rlNothing[] RL_RODATA = "";
static const char rlSprite[] RL_RODATA = "sprite";

static RL_CODE uint64_t rlNanosSince(uint64_t start) {
    uint64_t now = nowNanos();
    return now > start ? now - start : 0;
}

static RL_CODE int rlCompareU32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*) a;
    uint32_t y = *(const uint32_t*) b;
    return (x > y) - (y > x);
}

// Every event's code entries of the object and its parents, Create, Destroy, CleanUp and PreCreate aside (those
// are taken too with withCreate, for the sprite scan only), added once each.
static RL_CODE uint32_t rlAddObjectCode(const DataWin* dw, int32_t objectIndex, uint8_t* seen, uint32_t count) {
    for (int32_t depth = 0; ROOMLOAD_PARENT_DEPTH > depth && 0 <= objectIndex && dw->objt.count > (uint32_t) objectIndex; depth++) {
        if (seen[objectIndex >> 3] & (1u << (objectIndex & 7))) return count;
        seen[objectIndex >> 3] |= (uint8_t) (1u << (objectIndex & 7));
        const GameObject* object = &dw->objt.objects[objectIndex];
        for (int32_t type = 0; OBJT_EVENT_TYPE_COUNT > type; type++) {
            if (type == EVENT_DESTROY || type == EVENT_CLEANUP || type == EVENT_PRECREATE) continue;
            const ObjectEventList* list = &object->eventLists[type];
            for (uint32_t e = 0; list->events != nullptr && list->eventCount > e; e++) {
                const ObjectEvent* event = &list->events[e];
                for (uint32_t a = 0; event->actions != nullptr && event->actionCount > a; a++) {
                    int32_t codeId = event->actions[a].codeId;
                    if (0 > codeId || dw->code.count <= (uint32_t) codeId) continue;
                    if (ROOMLOAD_CODE_MAX <= count) return count;
                    // Create code goes last in the list's meaning: marked by a negative entry (-1 - id).
                    rl.codeIds[count++] = type == EVENT_CREATE ? -1 - codeId : codeId;
                }
            }
        }
        objectIndex = object->parentId;
    }
    return count;
}

static RL_CODE uint32_t rlBlobOf(const DataWin* dw, uint32_t offset) {
    uint32_t low = 0;
    uint32_t high = dw->codeBlobCount;
    while (high > low) {
        uint32_t mid = low + (high - low) / 2;
        if (offset > dw->codeBlobs[mid].absoluteOffset) low = mid + 1;
        else high = mid;
    }
    return low;
}

// The end of a group (by its first blob), and the index after its last blob.
static RL_CODE uint32_t rlGroupEnd(const DataWin* dw, uint32_t first, uint32_t* after) {
    uint32_t end = dw->codeBlobs[first].absoluteOffset;
    uint32_t i = first;
    while (dw->codeBlobCount > i && dw->codeBlobs[i].group == first) {
        uint32_t blobEnd = dw->codeBlobs[i].absoluteOffset + dw->codeBlobs[i].length;
        if (blobEnd > end) end = blobEnd;
        i++;
    }
    *after = i;
    return end;
}

// data_win.c's allocCodeArena (static there), with the same sizes: groups of up to a quarter block share 32 KB blocks,
// larger ones get their own. The room load's long-lived code memory then comes in the lazy path's blocks, not in
// blocks the size of its reads, which filled the low holes the chapter's small long-lived blocks go to and pushed one
// into the big region (DR44 Old: Chapter 5's bytecode no longer fitted).
#define RL_ARENA_BLOCK (32u * 1024u)
static RL_CODE uint8_t* rlArena(DataWin* dw, uint32_t size) {
    size = (size + 3u) & ~3u;
    if (size > RL_ARENA_BLOCK / 4) {
        uint8_t* own = (uint8_t*) malloc(size);
        if (own == nullptr) return nullptr;
        arrput(dw->codeArenaBlocks, own);
        dw->codeStats.residentBytes += size;
        return own;
    }
    if (size > dw->codeArenaLeft) {
        uint8_t* block = (uint8_t*) malloc(RL_ARENA_BLOCK);
        if (block == nullptr) return nullptr;
        dw->codeArena = block;
        arrput(dw->codeArenaBlocks, block);
        dw->codeArenaLeft = RL_ARENA_BLOCK;
        dw->codeStats.residentBytes += RL_ARENA_BLOCK;
    }
    uint8_t* at = dw->codeArena;
    dw->codeArena += size;
    dw->codeArenaLeft -= size;
    return at;
}

// Forgets the memory given to groups rl.groups[from..to) (it stays in the arena until the unload).
static RL_CODE void rlUnplace(DataWin* dw, uint32_t from, uint32_t to) {
    for (uint32_t g = from; to > g; g++) {
        uint32_t blobAfter = 0;
        rlGroupEnd(dw, rl.groups[g], &blobAfter);
        for (uint32_t b = rl.groups[g]; blobAfter > b; b++) dw->codeBlobs[b].bytes = nullptr;
    }
}

// Gives groups rl.groups[from..to) their memory as DataWin_loadCodeBytes would, a group at a time; false if out.
static RL_CODE bool rlPlace(DataWin* dw, uint32_t from, uint32_t to) {
    for (uint32_t g = from; to > g; g++) {
        uint32_t first = rl.groups[g];
        uint32_t blobAfter = 0;
        uint32_t end = rlGroupEnd(dw, first, &blobAfter);
        uint32_t groupStart = dw->codeBlobs[first].absoluteOffset;
        uint8_t* at = rlArena(dw, end - groupStart);
        if (at == nullptr) {
            rlUnplace(dw, from, g);
            return false;
        }
        for (uint32_t b = first; blobAfter > b; b++) dw->codeBlobs[b].bytes = at + (dw->codeBlobs[b].absoluteOffset - groupStart);
    }
    return true;
}

// Reads back the groups holding the listed code entries' bytecode that aren't in memory, as DataWin_loadCodeBytes
// would on their first run, but in file order and with neighbouring groups in one read.
static RL_CODE void rlReadCode(DataWin* dw, uint32_t count, uint64_t deadline, uint32_t* outGroups, uint32_t* outReads,
                               uint32_t* outBytes, uint64_t* outNanos) {
    rl.copyNanos = 0;
    if (dw->bytecodeFile == nullptr || dw->codeBlobs == nullptr || dw->codeBlobCount == 0) return;
    if (rl.headerFor != dw) {
        // The file is its header, then the bytecode (DataWin_moveBytecodeToFile).
        if (fseek(dw->bytecodeFile, 0, SEEK_END) != 0) return;
        long length = ftell(dw->bytecodeFile);
        if (0 > length || (long) dw->codeStats.totalBytes > length) return;
        rl.headerBytes = length - (long) dw->codeStats.totalBytes;
        rl.headerFor = dw;
    }
    uint32_t groups = 0;
    for (uint32_t i = 0; count > i; i++) {
        int32_t id = rl.codeIds[i];
        if (0 > id) continue;
        CodeEntry* code = &dw->code.entries[id];
        if (!code->present || code->bytecode != nullptr) continue;
        uint32_t blob = rlBlobOf(dw, code->bytecodeAbsoluteOffset);
        if (dw->codeBlobCount <= blob || dw->codeBlobs[blob].absoluteOffset != code->bytecodeAbsoluteOffset) continue;
        if (dw->codeBlobs[blob].bytes != nullptr) continue;
        rl.groups[groups++] = dw->codeBlobs[blob].group;
    }
    if (groups == 0) return;
    qsort(rl.groups, groups, sizeof(rl.groups[0]), rlCompareU32);
    uint32_t distinct = 0;
    for (uint32_t i = 0; groups > i; i++) {
        if (distinct == 0 || rl.groups[distinct - 1] != rl.groups[i]) rl.groups[distinct++] = rl.groups[i];
    }
    uint32_t bytesTotal = 0;
    uint32_t i = 0;
    while (distinct > i) {
        if (nowNanos() >= deadline || ROOMLOAD_CODE_BYTES <= bytesTotal) break;
        uint32_t first = rl.groups[i];
        uint32_t spanStart = dw->codeBlobs[first].absoluteOffset;
        uint32_t after = 0;
        uint32_t spanEnd = rlGroupEnd(dw, first, &after);
        uint32_t last = i + 1;
        while (distinct > last) {
            uint32_t next = rl.groups[last];
            uint32_t nextStart = dw->codeBlobs[next].absoluteOffset;
            uint32_t nextAfter = 0;
            uint32_t nextEnd = rlGroupEnd(dw, next, &nextAfter);
            if (nextStart < spanEnd || nextStart - spanEnd > ROOMLOAD_GAP_BYTES || nextEnd - spanStart > ROOMLOAD_SPAN_BYTES) break;
            spanEnd = nextEnd;
            last++;
        }
        uint32_t size = spanEnd - spanStart;
        // The span is read into a transient buffer, and each group copied into the lazy path's own code memory (see
        // rlPlace), so the room load leaves the heap as the lazy path would have.
#if ROOMLOAD_TRANSIENT_FIRST
        uint8_t* bytes = (uint8_t*) malloc(size);
        if (bytes == nullptr) break;
        if (!rlPlace(dw, i, last)) {
            free(bytes);
            break;
        }
#else
        if (!rlPlace(dw, i, last)) break;
        uint8_t* bytes = (uint8_t*) malloc(size);
        if (bytes == nullptr) {
            rlUnplace(dw, i, last);
            break;
        }
#endif
        uint64_t start = nowNanos();
        long at = rl.headerBytes + (long) (spanStart - (uint32_t) dw->bytecodeBufferBase);
        if (fseek(dw->bytecodeFile, at, SEEK_SET) != 0 || fread(bytes, 1, size, dw->bytecodeFile) != size) {
            free(bytes);
            rlUnplace(dw, i, last);
            break;
        }
        uint64_t took = rlNanosSince(start);
        uint64_t copyStart = nowNanos();
        for (uint32_t g = i; last > g; g++) {
            uint32_t group = rl.groups[g];
            uint32_t blobAfter = 0;
            uint32_t end = rlGroupEnd(dw, group, &blobAfter);
            uint32_t groupStart = dw->codeBlobs[group].absoluteOffset;
            // rlPlace gave the group this memory, from rlArena.
            memcpy((uint8_t*) dw->codeBlobs[group].bytes, bytes + (groupStart - spanStart), end - groupStart);
        }
        free(bytes);
        rl.copyNanos += rlNanosSince(copyStart);
        dw->codeStats.loads++;
        dw->codeStats.loadedBytes += size;
        dw->codeStats.loadNanos += took;
        *outGroups += last - i;
        *outReads += 1;
        *outBytes += size;
        *outNanos += took;
        bytesTotal += size;
        i = last;
    }
}

// The code entry's bytecode if it is in memory (never reads).
static RL_CODE const uint8_t* rlResidentBytes(const DataWin* dw, const CodeEntry* code) {
    if (!code->present) return nullptr;
    if (code->bytecode != nullptr) return code->bytecode;
    if (dw->codeBlobs == nullptr) return nullptr;
    uint32_t blob = rlBlobOf(dw, code->bytecodeAbsoluteOffset);
    if (dw->codeBlobCount <= blob || dw->codeBlobs[blob].absoluteOffset != code->bytecodeAbsoluteOffset) return nullptr;
    return dw->codeBlobs[blob].bytes;
}

static RL_CODE uint32_t rlAddSprite(uint32_t count, int32_t sprite) {
    for (uint32_t i = 0; count > i; i++) {
        if (rl.sprites[i] == sprite) return count;
    }
    if (ROOMLOAD_SPRITES_MAX <= count) return count;
    rl.sprites[count] = sprite;
    return count + 1;
}

// Sprites the code assigns as constants to a variable named like a sprite: "pushi.e <sprite>" (or push.i) right
// before "pop.v <...sprite...>".
static RL_CODE uint32_t rlScanSprites(const DataWin* dw, const uint8_t* bytes, uint32_t length, uint32_t count) {
    bool havePush = false;
    int32_t pushed = 0;
    uint32_t ip = 0;
    while (length >= ip + 4) {
        uint32_t instr = (uint32_t) bytes[ip] | ((uint32_t) bytes[ip + 1] << 8) | ((uint32_t) bytes[ip + 2] << 16) |
                         ((uint32_t) bytes[ip + 3] << 24);
        uint8_t opcode = (uint8_t) (instr >> 24);
        uint8_t type1 = (uint8_t) ((instr >> 16) & 0xF);
        uint32_t extra = 0;
        if ((instr & 0x40000000u) != 0) {
            if (type1 == GML_TYPE_DOUBLE || type1 == GML_TYPE_INT64) extra = 8;
            else if (type1 == GML_TYPE_FLOAT || type1 == GML_TYPE_INT32 || type1 == GML_TYPE_BOOL || type1 == GML_TYPE_VARIABLE ||
                     type1 == GML_TYPE_STRING) extra = 4;
        }
        if (ip + 4 + extra > length) break;
        bool push = false;
        if ((opcode == OP_PUSHI || opcode == OP_PUSH) && type1 == GML_TYPE_INT16) {
            pushed = (int16_t) (instr & 0xFFFF);
            push = true;
        } else if (opcode == OP_PUSH && type1 == GML_TYPE_INT32 && extra == 4) {
            pushed = (int32_t) ((uint32_t) bytes[ip + 4] | ((uint32_t) bytes[ip + 5] << 8) | ((uint32_t) bytes[ip + 6] << 16) |
                                ((uint32_t) bytes[ip + 7] << 24));
            push = true;
        } else if (opcode == OP_POP && havePush && extra == 4 && 0 <= pushed && dw->sprt.count > (uint32_t) pushed) {
            uint32_t ref = (uint32_t) bytes[ip + 4] | ((uint32_t) bytes[ip + 5] << 8) | ((uint32_t) bytes[ip + 6] << 16) |
                           ((uint32_t) bytes[ip + 7] << 24);
            uint32_t var = ref & 0x07FFFFFFu;
            if (dw->vari.variableCount > var && dw->vari.variables[var].name != nullptr &&
                strstr(dw->vari.variables[var].name, rlSprite) != nullptr) {
                count = rlAddSprite(count, pushed);
            }
        }
        havePush = push;
        ip += 4 + extra;
    }
    return count;
}

void CtrRenderer_loadFontVerdicts(Renderer* renderer);
// The room plan's scene code (z_roomplan.c): its entries join the room's object code in the
// same file-order read and the same budget. CMakeLists.txt builds z_roomplan.c with ROOMPLAN_OWN_CODE_READ=0.
uint32_t RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max);

RL_CODE void CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    // Fonts F2(a): the build's per-font verdicts, once per renderer, at its first room change (ctr_renderer.c).
    if (runner != nullptr) CtrRenderer_loadFontVerdicts(runner->renderer);
    if (runner == nullptr || cache->pageCount == 0 || runner->dataWin == nullptr || runner->currentRoom == nullptr) {
        __real_CtrTextures_prefetchRoom(cache, runner);
        return;
    }
    uint64_t start = nowNanos();
    DataWin* dw = runner->dataWin;

    // 1. The room's objects' event code, read before the original starts the tiles' read-ahead: on the Old models a
    //    bytecode read waits behind the tile reader's.
    uint32_t count = 0;
    uint8_t* seen = (uint8_t*) calloc((dw->objt.count + 7u) / 8u + 1u, 1);
    if (seen != nullptr) {
        for (ptrdiff_t i = 0; arrlen(runner->instances) > i; i++) {
            const Instance* instance = runner->instances[i];
            if (instance->destroyed) continue;
            count = rlAddObjectCode(dw, instance->objectIndex, seen, count);
        }
        free(seen);
    }
    uint32_t objectCount = count;
    count += RoomPlan_roomCode(runner, rl.codeIds + count, ROOMLOAD_CODE_MAX - count);
    uint32_t groups = 0, reads = 0, bytes = 0;
    uint64_t codeNanos = 0;
    uint64_t codeDeadline = start + ROOMLOAD_CODE_NANOS;
    rlReadCode(dw, count, codeDeadline, &groups, &reads, &bytes, &codeNanos);
    uint64_t codeTook = rlNanosSince(start);

    // The original: the room's tiles marked and their read-ahead started. The budget's rest runs from its end.
    __real_CtrTextures_prefetchRoom(cache, runner);
    uint64_t marked = nowNanos();
    uint64_t deadline = marked + (ROOMLOAD_BUDGET_NANOS > codeTook ? ROOMLOAD_BUDGET_NANOS - codeTook : 0);

    // 2. Sprites the code assigns.
    uint32_t sprites = 0;
    for (uint32_t i = 0; objectCount > i; i++) {
        int32_t id = 0 > rl.codeIds[i] ? -1 - rl.codeIds[i] : rl.codeIds[i];
        const CodeEntry* code = &dw->code.entries[id];
        const uint8_t* code_bytes = rlResidentBytes(dw, code);
        if (code_bytes != nullptr) sprites = rlScanSprites(dw, code_bytes, code->length, sprites);
    }
    // Only within the budget's headroom (ctr_textures.c, ROOMLOAD_SPRITE_HEADROOM_BYTES); "sprites" counts those marked.
    uint32_t room = CtrTextures_roomLoadSpriteRoom(cache);
    uint32_t spritesMarked = 0;
    for (uint32_t i = 0; sprites > i && deadline > nowNanos(); i++) {
        uint32_t adds = CtrTextures_roomLoadSprite(cache, rl.sprites[i], room);
        if (adds == UINT32_MAX) continue;
        room -= adds;
        spritesMarked++;
    }
    sprites = spritesMarked;

    // 3. The room's tiles.
    uint32_t nowTiles = 0, otherTiles = 0;
    uint32_t evictions = cache->evictionCount;
    CtrTextures_roomLoadFinish(cache, deadline, &nowTiles, &otherTiles);
    evictions = cache->evictionCount - evictions;

    uint64_t total = codeTook + rlNanosSince(marked);
    logInfo(rlLogFormat, runner->currentRoom->name, (unsigned) groups, (unsigned) reads, (unsigned) (bytes / 1024),
            (double) codeNanos / 1e6, (double) rl.copyNanos / 1e6, (unsigned) sprites, (unsigned) nowTiles, (unsigned) otherTiles, (double) total / 1e6,
            nowNanos() >= deadline ? rlCutShort : rlNothing, (unsigned) evictions);
}

// ===[ Item 4 (B): a promotion whose read gave up is asked again ]===
//
// Called once a frame by texcacheCollectWholeRead (ctr_textures.c), through RoomLoad_collect in place of its call to
// collectWholeRead, so beginFrame and the order of its work are unchanged. promotePending admits a page while the heap
// has the file's size and the reader's 8 MB reserve free, but the reader checks the heap again when it starts the read,
// and a room's first frames allocate meanwhile (the likely cause; the logs show only that the packed budget wasn't the
// limit). On the Old 2DS page 21 (Toriel's faces) was admitted at
// the first Ruins room's second frame (idle copies reclaimed for heap headroom, with the packed budget at 2.0 of 5.7 MB), then
// counted failed without being kept; nothing asked again until a draw loaded from its file, Toriel's first dialogue in
// the next Ruins room (f272: 86-105 ms against 45 ms). Such a page is asked again ROOMLOAD_RETRY_FRAMES later, at most
// ROOMLOAD_RETRIES times in a room, under promotePending's own rules; not once the room changes (prefetchRoom drops
// pending promotions too). The read seen here last frame gave up if it has been collected since (it is gone, or another
// page's has taken its place), the promotions kept didn't grow, and the failed ones did.
// Item 6: each page has its own retries (pages below ROOMLOAD_RETRY_PAGES). With one slot, a second page giving up
// took the first one's place: on the Old 2DS (rl11-o2ds-ut2B) page 21 gave up at the first Ruins room's entry, then page 24,
// and page 21 was not asked again until Toriel's first dialogue loaded from its file (f272 51-53 ms against 38-52).
#include <malloc.h>

#define ROOMLOAD_RETRY_FRAMES 30
#define ROOMLOAD_RETRIES 2
#define ROOMLOAD_RETRY_PAGES 128

extern u32 __ctru_heap_size;
static void RoomLoad_frame(CtrTextureCache* cache);
void RoomLoad_collect(CtrTextureCache* cache, bool drop);
void CtrTextures_collectWholeRead(CtrTextureCache* cache, bool drop);
void CtrTextures_roomLoadIdle(CtrTextureCache* cache); // item 7 (ctr_textures.c's end)

static struct {
    const CtrPage* pages; // the cache this is for, told apart from a new one as texcache does
    uint32_t pageCount;
    uint32_t frame;
    const CtrTileRead* whole; // the cache's whole-file read when last seen, and its page and counts then
    uint32_t wholePage;
    uint32_t promotions, failed;
    uint32_t retryFrame;  // when the pages in `pending` are asked again
    uint32_t retryRoom;   // the room (its roomStartFrame) the retries are for
    bool retryPending;
    uint8_t pending[ROOMLOAD_RETRY_PAGES / 8];
    uint8_t retries[ROOMLOAD_RETRY_PAGES]; // asks again in this room, per page
} rlFrame RL_BSS;

static const char rlRetryLog[] RL_RODATA = "Textures: page %u's promotion read gave up (%u KB heap free now)%s\n";
static const char rlRetryAgain[] RL_RODATA = "; asked again in 30 frames";

static RL_CODE void RoomLoad_frame(CtrTextureCache* cache) {
    if (rlFrame.pages != cache->pages || rlFrame.pageCount != cache->pageCount || cache->frame < rlFrame.frame) {
        // A new cache (a chapter's): nothing kept is the new one's.
        memset(&rlFrame, 0, sizeof(rlFrame));
        rlFrame.pages = cache->pages;
        rlFrame.pageCount = cache->pageCount;
    } else {
        if (rlFrame.retryRoom != cache->roomStartFrame) {
            // A new room: its own retries (prefetchRoom dropped the pending promotions).
            rlFrame.retryRoom = cache->roomStartFrame;
            rlFrame.retryPending = false;
            memset(rlFrame.pending, 0, sizeof(rlFrame.pending));
            memset(rlFrame.retries, 0, sizeof(rlFrame.retries));
        }
        if (rlFrame.whole != nullptr && (cache->wholeRead == nullptr || cache->wholePage != rlFrame.wholePage) &&
            cache->promotions == rlFrame.promotions && cache->failedPromotions > rlFrame.failed && cache->pageCount > rlFrame.wholePage) {
            uint32_t pageId = rlFrame.wholePage;
            bool again = ROOMLOAD_RETRY_PAGES > pageId && ROOMLOAD_RETRIES > rlFrame.retries[pageId] && cache->pages[pageId].packed == nullptr;
            // ctr_textures.c's heapFree, repeated.
            struct mallinfo mi = mallinfo();
            uint32_t heapLeft = __ctru_heap_size > (u32) mi.uordblks ? __ctru_heap_size - (u32) mi.uordblks : 0;
            logInfo(rlRetryLog, (unsigned) pageId, (unsigned) (heapLeft / 1024), again ? rlRetryAgain : rlNothing);
            if (again) {
                rlFrame.retries[pageId]++;
                rlFrame.pending[pageId >> 3] |= (uint8_t) (1u << (pageId & 7));
                rlFrame.retryFrame = cache->frame + ROOMLOAD_RETRY_FRAMES;
                rlFrame.retryPending = true;
            }
        }
    }
    if (rlFrame.retryPending && cache->frame >= rlFrame.retryFrame) {
        rlFrame.retryPending = false;
        for (uint32_t pageId = 0; cache->pageCount > pageId && ROOMLOAD_RETRY_PAGES > pageId; pageId++) {
            if (!((rlFrame.pending[pageId >> 3] >> (pageId & 7)) & 1u)) continue;
            CtrPage* page = &cache->pages[pageId];
            if (page->packed == nullptr && !page->failed && page->cacheFile == CTR_CACHE_FILE_VALID && page->promotionBytes > 0)
                page->promotionPending = true;
        }
        memset(rlFrame.pending, 0, sizeof(rlFrame.pending));
    }
    rlFrame.frame = cache->frame;
    rlFrame.whole = cache->wholeRead;
    rlFrame.wholePage = cache->wholePage;
    rlFrame.promotions = cache->promotions;
    rlFrame.failed = cache->failedPromotions;
    CtrTextures_roomLoadIdle(cache);
}

// Called by texcacheCollectWholeRead (ctr_textures.c) in place of collectWholeRead, once a frame before promotion.
RL_CODE void RoomLoad_collect(CtrTextureCache* cache, bool drop) {
    RoomLoad_frame(cache);
    CtrTextures_collectWholeRead(cache, drop);
}

// ===[ codeclose F1/F6 (UNDERTALE Old only): the plan's code after the tiles, coalesced and never aborting ]===
// utstall item 7 (z_utstall.c usReadCode) reads the plan scene's code that rlReadCode above left (its 120 ms) after the
// tiles, one DataWin_loadCodeBytes a group: one SD read each (about 8.5 ms on the o3DS), into the code arena through
// safeMalloc, which aborts when no block fits, while the check before it counts the heap's total free, not its largest
// block (argclosereview F1, F6). The room plan's in-play code closure (tools/roomplan/codeclose.py) lists more code
// there. RoomPlan_roomCode is wrapped (CMakeLists.txt, UNDERTALE Old): the call that fills rl.codeIds (the room load
// above) goes on as it was; usReadCode's call reads the same list here with rlReadCode (file order, neighbouring groups
// in one read, rlArena's memory, which gives up instead of aborting) within usReadCode's limits, and hands usReadCode
// nothing, so its loop and its log stay quiet. A pass of rlReadCode stops at ROOMLOAD_CODE_BYTES; another starts only
// after a pass that reached it. Only when the bytecode is on file: usReadCode returns before its call otherwise. Code
// loads only: no game state changes.
// A pass is best effort and touches nothing else (argcfreview F1-F4): the heap's reliever (packed copies) and its
// overflow into linear memory are off for it (CtrMemory_allowRelief, as CtrMemory_allocWork), so its allocations
// fail instead; rlArena's block list gets its room first, with both on, as any lazy load's would (an arrput that
// grows it inside the pass would abort on failure); and a pass starts only when one heap block holds the most it can
// take (CC_PASS_NEED, from the largest unread group: one larger than ROOMLOAD_SPAN_BYTES is a read of its own) with
// 2 MB still free after it. Its allocations then fit in that block, carved in order, the transient buffer last and
// given back each read, so none fails; one that did anyway would leave its read's code memory unused in the arena
// (rlUnplace), which is why a pass that stops short is never retried in the same call.
#if defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#include "ctr_memory.h"
#define CC_CODE __attribute__((noinline, optimize("Os"), section(".text.codeclose_late")))
#define CC_RODATA __attribute__((aligned(4), section(".rodata.codeclose_in_text")))
// z_utstall.c's UTSTALL_CODE_NANOS, UTSTALL_CODE_BYTES and UTSTALL_CODE_HEAP_FREE, repeated.
#define CC_CODE_NANOS (400ull * 1000000ull)
#define CC_CODE_BYTES (512u * 1024u)
#define CC_HEAP_FREE (2u * 1024u * 1024u)
// The most one rlReadCode pass can take, its largest read being span bytes: the code memory of its byte limit and one
// read past it (rlArena moves to a new 32 KB block only with under 8 KB of the last one free, so a third more at
// most, and one block part used), and that read's transient buffer.
#define CC_PASS_NEED(span) ((ROOMLOAD_CODE_BYTES + (span)) / 3u * 4u + RL_ARENA_BLOCK + (span))
// rlArena's blocks for one call: two passes at most, each a block per 8 KB it reads at most, and one more.
#define CC_ARENA_ENTRIES 96u

uint32_t __real_RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max);
uint32_t __wrap_RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max);

static const char ccLog[] CC_RODATA =
    "Room load (codeclose): %s: the plan's code after the tiles, %u groups in %u reads, %u KB in %.0f ms; %u entries not read\n";

CC_CODE uint32_t __wrap_RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max) {
    // The room load's call fills rl.codeIds from its objects' count on, which may be its end (max 0).
    if ((uintptr_t) out - (uintptr_t) rl.codeIds <= sizeof(rl.codeIds)) return __real_RoomPlan_roomCode(runner, out, max);
    DataWin* dw = runner->dataWin;
    uint32_t count = __real_RoomPlan_roomCode(runner, rl.codeIds, ROOMLOAD_CODE_MAX);
    uint64_t start = nowNanos();
    uint64_t deadline = start + CC_CODE_NANOS;
    uint64_t copyNanos = rl.copyNanos; // the room load's, for its log line
    uint32_t groups = 0, reads = 0, bytes = 0;
    uint64_t nanos = 0;
    uint32_t span = ROOMLOAD_SPAN_BYTES;
    for (uint32_t i = 0; count > i; i++) {
        const CodeEntry* code = &dw->code.entries[rl.codeIds[i]];
        if (!code->present || code->bytecode != nullptr) continue;
        uint32_t blob = rlBlobOf(dw, code->bytecodeAbsoluteOffset);
        if (dw->codeBlobCount <= blob || dw->codeBlobs[blob].bytes != nullptr) continue;
        uint32_t group = dw->codeBlobs[blob].group, after = 0;
        uint32_t size = rlGroupEnd(dw, group, &after) - dw->codeBlobs[group].absoluteOffset;
        if (size > span) span = size;
    }
    uint32_t need = CC_PASS_NEED(span);
    arrsetcap(dw->codeArenaBlocks, arrlen(dw->codeArenaBlocks) + CC_ARENA_ENTRIES);
    bool relief = CtrMemory_allowRelief(false);
    while (count > 0 && CC_CODE_BYTES > bytes && deadline > nowNanos()) {
        struct mallinfo mi = mallinfo();
        if ((uint32_t) mi.uordblks + CC_HEAP_FREE + need > __ctru_heap_size) break;
        void* room = malloc(need);
        if (room == nullptr) break;
        free(room);
        uint32_t before = bytes;
        rlReadCode(dw, count, deadline, &groups, &reads, &bytes, &nanos);
        if (ROOMLOAD_CODE_BYTES > bytes - before) break;
    }
    CtrMemory_allowRelief(relief);
    rl.copyNanos = copyNanos;
    uint32_t left = 0;
    for (uint32_t i = 0; count > i; i++) {
        if (rlResidentBytes(dw, &dw->code.entries[rl.codeIds[i]]) == nullptr) left++;
    }
    if (reads > 0 || left > 0) {
        logInfo(ccLog, runner->currentRoom->name, (unsigned) groups, (unsigned) reads, (unsigned) (bytes / 1024),
                (double) (nowNanos() - start) / 1e6, (unsigned) left);
    }
    return 0;
}
#endif
