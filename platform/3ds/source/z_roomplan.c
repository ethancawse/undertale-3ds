// roomplan: the room plan, phase 1 (UNDERTALE). Late code, constants and statics only
// (platform/3ds/late_reserve.ld): nothing here moves existing code or data, and nothing runs per frame or per draw.
//
// A room's first visit has no record in rooms.dtm (ctr_textures.c, "Room memory"), so its tiles load while it plays.
// The builder (tools/roomplan/build_plan.py) ships romfs:/roomplan/<part>.dtp: for each room, the tiles it draws in
// its first frames, recorded on the emulator and resolved against the player's own data.win, in rooms.dtm's own
// encoding (late | page << 8 | tile). After CtrTextures_init has read rooms.dtm, every room without a record gets the
// plan's, so a first visit reads ahead like a second one. Nothing else changes: markRemembered, readAhead and the
// fill take the tiles as they would take remembered ones, and a visit records the room as before, replacing the plan.
//
// The plan is used only when it was made from this exact data.win (the SHA-256 the boot check computed: its call is
// wrapped to keep the digest, so nothing is hashed again), for this data.win's texture key, tile size and tile grid
// rule, and its room count; otherwise it is ignored and logged once. rooms.dtm keeps only the player's own records: a
// room whose tiles still equal the plan's is left out when the record is written (the reader's write is wrapped), so a
// later plan still reaches rooms not visited yet.
//
// Hooks (CMakeLists.txt): CtrDataVersion_check (a link of its chain, ctr_wrap_chain.h), CtrTextures_init and
// CtrTileReader_writeFile are wrapped, so their callers keep their
// code. DELTARUNE (one plan per chapter, romfs:/roomplan/<chapter folder>.dtp): pixpress wraps
// CtrTextures_init there, so the plan is seeded at CtrTextures_init's call of CtrMemory_setHeapReliever, which comes
// right after it read rooms.dtm (the same point as UNDERTALE's seeding), the first one after each chapter's data.win
// check (main.c checks each data.win once, at each load of a chapter or of the launcher).
//
// CTRPORT_ROOMPLAN_RECORDER (build.sh ... roomplan-rec; never a release build) adds the recorder: per room visit, the
// texture page items drawn (sprites, backgrounds and tiles, fonts), the tiles the cache was asked for, and the code
// entries read, each with its frame since the room change, written to roomplan-rec.txt in the game folder. Indices
// only, no names or content. tools/roomplan/records.py turns them into the committed records.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roomplan_format.h"

#if defined(ENABLE_WAD16) || defined(ENABLE_WAD17) || defined(ROOMPLAN_HOST_TEST)

#ifndef ROOMPLAN_HOST_TEST
#include "ctr_array.h"
#line 40
#include <3ds.h>

#include "common.h"
#include "ctr_benchromfs.h"
#include "ctr_data_version.h"
#include "ctr_textures.h"
#include "ctr_tile_reader.h"
#include "data_win.h"
#include "log.h"
#include "stb_ds.h"
#ifndef ENABLE_WAD16
#include "ctr_memory.h"
#endif
#define ROOMPLAN_TEXT __attribute__((noinline, section(".text.late.roomplan")))
#define ROOMPLAN_CONST __attribute__((aligned(4), section(".rodata.roomplan_in_text")))
#define ROOMPLAN_BSS __attribute__((section(".bss.late.roomplan")))
#else
#define ROOMPLAN_TEXT
#define ROOMPLAN_CONST
#define ROOMPLAN_BSS
#endif

// ===[ The file ]===

ROOMPLAN_TEXT uint32_t RoomPlan_fnv1a(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; size > i; i++) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

#if defined(ENABLE_WAD16) && !defined(ROOMPLAN_HOST_TEST) // freebytes: z_utsfx.c calls it too (UNDERTALE)
#define hexNibble RoomPlan_hexNibble
int hexNibble(char c);
#define ROOMPLAN_NIBBLE_LINKAGE
#else
#define ROOMPLAN_NIBBLE_LINKAGE static
#endif
#line 72
ROOMPLAN_TEXT ROOMPLAN_NIBBLE_LINKAGE int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Checks a plan file against the running data.win and build. digest: the boot check's lowercase hex SHA-256.
// Returns ROOMPLAN_OK or why it can't be used.
ROOMPLAN_TEXT RoomPlanStatus RoomPlan_check(const uint8_t* data, size_t size, const char* digest, uint32_t dataWinSize,
                                            uint32_t gameId, uint64_t timestamp, uint32_t roomCount) {
    RoomPlanHeader h;
    if (sizeof(h) > size) return ROOMPLAN_BAD_FILE;
    memcpy(&h, data, sizeof(h));
    if (h.magic != ROOMPLAN_MAGIC) return ROOMPLAN_BAD_FILE;
    if (h.version != ROOMPLAN_VERSION) return ROOMPLAN_OTHER_VERSION;
    if (digest == nullptr || strlen(digest) != 64) return ROOMPLAN_NO_DIGEST;
    for (int i = 0; 32 > i; i++) {
        int hi = hexNibble(digest[2 * i]), lo = hexNibble(digest[2 * i + 1]);
        if (0 > hi || 0 > lo || h.sha256[i] != (uint8_t) (hi << 4 | lo)) return ROOMPLAN_OTHER_DATA;
    }
    if (h.dataWinSize != dataWinSize || h.gameId != gameId || h.timestamp != timestamp) return ROOMPLAN_OTHER_DATA;
    if (h.tileMax != ROOMPLAN_TILE_MAX || h.gridVersion != ROOMPLAN_GRID_VERSION) return ROOMPLAN_OTHER_VERSION;
    if (h.roomCount != roomCount) return ROOMPLAN_OTHER_DATA;
    if (h.bodySize != size - sizeof(h) || h.checksum != RoomPlan_fnv1a(data + sizeof(h), h.bodySize)) return ROOMPLAN_BAD_FILE;
    // Every scene must fit the body.
    size_t at = sizeof(h);
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        if (sizeof(scene) > size - at) return ROOMPLAN_BAD_FILE;
        memcpy(&scene, data + at, sizeof(scene));
        size_t need = RoomPlan_sceneSize(&scene);
        if (scene.room >= roomCount || scene.tileCount > ROOMPLAN_TILES_MAX || need > size - at) return ROOMPLAN_BAD_FILE;
        at += need;
    }
    return at == size ? ROOMPLAN_OK : ROOMPLAN_BAD_FILE;
}

// Calls take(context, room, tiles, count) for each room-level scene (sub-key ROOMPLAN_NO_SUB) of a checked plan.
ROOMPLAN_TEXT void RoomPlan_forEachRoom(const uint8_t* data, void (*take)(void*, uint32_t, const uint32_t*, uint32_t), void* context) {
    RoomPlanHeader h;
    memcpy(&h, data, sizeof(h));
    size_t at = sizeof(h);
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        memcpy(&scene, data + at, sizeof(scene));
        // rooms.dtm's record holds the first ROOMPLAN_SEED_FRAMES frames; the tiles are in first-use order, so those
        // are a prefix.
        const uint8_t* buckets = data + at + sizeof(scene) + (size_t) scene.tileCount * 4;
        uint32_t count = 0;
        while (scene.tileCount > count && ROOMPLAN_SEED_FRAMES >= buckets[count]) count++;
        if (scene.sub == ROOMPLAN_NO_SUB && count > 0) take(context, scene.room, (const uint32_t*) (data + at + sizeof(scene)), count);
        at += RoomPlan_sceneSize(&scene);
    }
}

// Removes, from a rooms.dtm image, every room whose tiles equal the plan's (same entries, same order). Returns the new
// size; the header's body size and checksum follow. same(context, room, tiles, count) says whether they equal.
ROOMPLAN_TEXT size_t RoomPlan_dropPlanned(uint8_t* data, size_t size, bool (*same)(void*, uint32_t, const uint32_t*, uint32_t), void* context,
                                          uint32_t* dropped) {
    *dropped = 0;
    if (ROOMPLAN_DTM_HEADER_SIZE > size) return size;
    uint32_t magic;
    memcpy(&magic, data, 4);
    if (magic != ROOMPLAN_DTM_MAGIC) return size;
    uint8_t* body = data + ROOMPLAN_DTM_HEADER_SIZE;
    size_t bodySize = size - ROOMPLAN_DTM_HEADER_SIZE;
    size_t in = 0, out = 0;
    while (bodySize >= in + 4) {
        uint16_t room, count;
        memcpy(&room, body + in, 2);
        memcpy(&count, body + in + 2, 2);
        size_t record = 4 + (size_t) count * 4;
        if (record > bodySize - in) break;
        // Aligned copy: the body's entries are only 2-byte aligned relative to the start (room and count are u16).
        bool drop = false;
        uint32_t* tiles = malloc(count > 0 ? (size_t) count * 4 : 4);
        if (tiles != nullptr) {
            memcpy(tiles, body + in + 4, (size_t) count * 4);
            drop = same(context, room, tiles, count);
            free(tiles);
        }
        if (drop) {
            (*dropped)++;
        } else {
            if (out != in) memmove(body + out, body + in, record);
            out += record;
        }
        in += record;
    }
    if (*dropped == 0) return size;
    // Whatever followed the last whole record is kept as it was (loadRoomMemory stops there too).
    if (bodySize > in) {
        memmove(body + out, body + in, bodySize - in);
        out += bodySize - in;
    }
    uint32_t newBody = (uint32_t) out;
    uint32_t checksum = RoomPlan_fnv1a(body, out);
    memcpy(data + ROOMPLAN_DTM_BODY_SIZE_AT, &newBody, 4);
    memcpy(data + ROOMPLAN_DTM_CHECKSUM_AT, &checksum, 4);
    return ROOMPLAN_DTM_HEADER_SIZE + out;
}

// The code entries of a checked plan's scene for a room: its battle group's scene if the plan has one (group(context)
// gives the group, asked only when the room has group scenes), else the room's. Returns them (unaligned u16) and their
// count, or nullptr and 0; *sub is the group taken, or -1.
ROOMPLAN_TEXT const uint8_t* RoomPlan_sceneCode(const uint8_t* data, uint32_t room, int32_t (*group)(void*), void* context, uint32_t* count,
                                                int32_t* sub) {
    RoomPlanHeader h;
    memcpy(&h, data, sizeof(h));
    size_t at = sizeof(h);
    const uint8_t* whole = nullptr;
    uint32_t wholeCount = 0;
    int32_t want = -2;
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        RoomPlanScene scene;
        memcpy(&scene, data + at, sizeof(scene));
        if (scene.room == room) {
            const uint8_t* code = data + at + sizeof(scene) + (size_t) scene.tileCount * 4 + (((size_t) scene.tileCount + 3) & ~(size_t) 3);
            if (scene.sub == ROOMPLAN_NO_SUB) {
                whole = code;
                wholeCount = scene.codeCount;
            } else {
                if (want == -2) want = group(context);
                if (0 <= want && scene.sub == (uint32_t) want) {
                    *count = scene.codeCount;
                    *sub = want;
                    return code;
                }
            }
        }
        at += RoomPlan_sceneSize(&scene);
    }
    *count = whole != nullptr ? wholeCount : 0;
    *sub = -1;
    return whole;
}

#ifndef ROOMPLAN_HOST_TEST

// ===[ The runtime ]===

#ifdef ENABLE_WAD16
static const char rpPath[] ROOMPLAN_CONST = "romfs:/roomplan/undertale.dtp";
#else
// DELTARUNE: one plan per chapter, named after its folder (the boot check's part).
static const char rpPathFormat[] ROOMPLAN_CONST = "romfs:/roomplan/%s.dtp";
#endif
static const char rpRomfs[] ROOMPLAN_CONST = "romfs";
static const char rpDtm[] ROOMPLAN_CONST = "rooms.dtm";
static const char rpRead[] ROOMPLAN_CONST = "rb";
static const char rpSeeded[] ROOMPLAN_CONST = "Room plan: %u rooms without a record take the plan's first tiles (%u tiles, %u rooms planned, %.1f ms)\n";
static const char rpIgnored[] ROOMPLAN_CONST = "Room plan: %s not used: %s\n";
static const char rpKept[] ROOMPLAN_CONST = "Room plan: rooms.dtm leaves out %u rooms still as planned\n";
static const char rpReason0[] ROOMPLAN_CONST = "no file";
static const char rpReason1[] ROOMPLAN_CONST = "damaged or not a plan";
static const char rpReason2[] ROOMPLAN_CONST = "made for another data.win";
static const char rpReason3[] ROOMPLAN_CONST = "made for another plan format, tile size or tile grid";
static const char rpReason4[] ROOMPLAN_CONST = "the data.win check gave no digest";
static const char rpReason5[] ROOMPLAN_CONST = "out of memory";

__attribute__((no_reorder, section(".bss.late.roomplan"))) static struct {
    char digest[65];     // the boot check's SHA-256 of this data.win, "" if none
    uint8_t* plan;       // the checked plan, kept while the cache lives (rooms.dtm writes compare against it)
    uint32_t planSize;
    CtrTextureCache* cache;
    uint32_t seededRooms;
    uint32_t seededTiles;
    uint32_t plannedRooms;
#ifndef ENABLE_WAD16
    bool pending;  // a data.win check has run and its texture cache isn't seeded yet
    char part[32]; // the checked data.win's folder ("chapter1_windows"; "root" for the launcher)
    char path[64];
#endif
} rp;

// A link of CtrDataVersion_check's wrap chain (its order: ctr_wrap_chain.h).
#include "ctr_wrap_chain.h"

ROOMPLAN_TEXT CtrDataVersionResult CTR_DATAVERSION_ROOMPLAN(const char* path, const char* game, const char* part, const char* cachePath,
                                                            void (*progress)(void), bool* cached, char digest[65]) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_ROOMPLAN(path, game, part, cachePath, progress, cached, digest);
    memcpy(rp.digest, digest, sizeof(rp.digest));
    rp.digest[64] = '\0';
#ifndef ENABLE_WAD16
    // A chapter starts: the last one's texture cache is gone (its rooms.dtm was written when it stopped), so is its plan.
    free(rp.plan);
    rp.plan = nullptr;
    rp.cache = nullptr;
    // No string literals here: they would go to the plain .rodata's string pool and move what follows it.
    size_t length = part != nullptr ? strlen(part) : 0;
    if (length >= sizeof(rp.part)) length = sizeof(rp.part) - 1;
    if (length > 0) memcpy(rp.part, part, length);
    rp.part[length] = '\0';
    rp.pending = true;
#endif
    return result;
}

ROOMPLAN_TEXT static const char* reasonText(RoomPlanStatus status) {
    switch (status) {
        case ROOMPLAN_BAD_FILE: return rpReason1;
        case ROOMPLAN_OTHER_DATA: return rpReason2;
        case ROOMPLAN_OTHER_VERSION: return rpReason3;
        case ROOMPLAN_NO_DIGEST: return rpReason4;
        case ROOMPLAN_NO_MEMORY: return rpReason5;
        default: return rpReason0;
    }
}

#line 288

// The plan's tiles of a room, or count 0.
typedef struct {
    uint32_t room;
    const uint32_t* tiles;
    uint32_t count;
} RoomPlanFind;

ROOMPLAN_TEXT static void findRoom(void* context, uint32_t room, const uint32_t* tiles, uint32_t count) {
    RoomPlanFind* find = context;
    if (room == find->room && find->count == 0) {
        find->tiles = tiles;
        find->count = count;
    }
}

ROOMPLAN_TEXT static bool samePlanned(void* context, uint32_t room, const uint32_t* tiles, uint32_t count) {
    (void) context;
    RoomPlanFind find = {room, nullptr, 0};
    RoomPlan_forEachRoom(rp.plan, findRoom, &find);
    return find.count == count && count > 0 && memcmp(find.tiles, tiles, (size_t) count * 4) == 0;
}

static void oomSeedRoom(void* context, uint32_t room, const uint32_t* tiles, uint32_t count);
// Load-only callback: retain its 176 B slot and tail-call the checked body in
// oomport's section. All four Release baselines have this same extent.
ROOMPLAN_TEXT __attribute__((naked, noipa)) static void seedRoom(
    void* context __attribute__((unused)), uint32_t room __attribute__((unused)),
    const uint32_t* tiles __attribute__((unused)), uint32_t count __attribute__((unused))) {
    __asm__("b oomSeedRoom\n"
            "oomport_tail_seedRoom:\n"
            ".org seedRoom+176\n");
}
#line 310

ROOMPLAN_TEXT static void seedFromPlan(CtrTextureCache* cache) {
    uint64_t start = svcGetSystemTick();
#ifdef ENABLE_WAD16
    const char* path = rpPath;
#else
    snprintf(rp.path, sizeof(rp.path), rpPathFormat, rp.part);
    const char* path = rp.path;
#endif
    free(rp.plan);
    rp.plan = nullptr;
    rp.cache = nullptr;
    rp.seededRooms = rp.seededTiles = rp.plannedRooms = 0;
    CtrRomfs_init(rpRomfs);
    FILE* f = fopen(path, rpRead);
    RoomPlanStatus status = ROOMPLAN_NO_FILE;
    uint8_t* data = nullptr;
    long size = 0;
    if (f != nullptr) {
        status = ROOMPLAN_BAD_FILE;
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        if (size > 0 && ROOMPLAN_MAX_SIZE >= (unsigned long) size && fseek(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t) size);
            if (data == nullptr) status = ROOMPLAN_NO_MEMORY;
            else if (fread(data, 1, (size_t) size, f) != (size_t) size) {
                free(data);
                data = nullptr;
            }
        }
        fclose(f);
    }
    if (data != nullptr) {
        status = RoomPlan_check(data, (size_t) size, rp.digest, cache->key.dataWinSize, cache->key.gameId, cache->key.timestamp,
                                cache->roomCount);
    }
    if (status != ROOMPLAN_OK) {
        free(data);
        logInfo(rpIgnored, path, reasonText(status));
        return;
    }
    rp.plan = data;
    rp.planSize = (uint32_t) size;
    rp.cache = cache;
    RoomPlan_forEachRoom(data, seedRoom, cache);
    logInfo(rpSeeded, (unsigned) rp.seededRooms, (unsigned) rp.seededTiles, (unsigned) rp.plannedRooms,
            (double) (svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
}

#ifdef ENABLE_WAD16
void __real_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);
void __wrap_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);

ROOMPLAN_TEXT void __wrap_CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes) {
    __real_CtrTextures_init(cache, dataWin, fileSystem, budgetBytes);
    seedFromPlan(cache);
}
#else
// DELTARUNE: CtrTextures_init gives the heap reliever its cache right after reading rooms.dtm (ctr_textures.c); the
// first such call after a data.win check is that chapter's cache. CtrTextures_destroy's call passes none.
void __real_CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context);
void __wrap_CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context);

ROOMPLAN_TEXT void __wrap_CtrMemory_setHeapReliever(CtrHeapReliever reliever, void* context) {
    __real_CtrMemory_setHeapReliever(reliever, context);
    if (!rp.pending || context == nullptr) return;
    rp.pending = false;
    seedFromPlan(context);
}
#endif

void __real_CtrTileReader_writeFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size);
void RoomPlan_retiredWriteFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size);

// Retired by rpfix (its walk of the plan per record cost ~145 ms in play on the Old models): kept unchanged and
// uncalled, so the late code after it stays put. __wrap_CtrTileReader_writeFile is at the end of this file.
ROOMPLAN_TEXT void RoomPlan_retiredWriteFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size) {
    size_t length = strlen(path);
    if (rp.plan != nullptr && data != nullptr && length >= sizeof(rpDtm) - 1 && strcmp(path + length - (sizeof(rpDtm) - 1), rpDtm) == 0) {
        uint32_t dropped = 0;
        size_t kept = RoomPlan_dropPlanned(data, size, samePlanned, nullptr, &dropped);
        if (dropped > 0) logInfo(rpKept, (unsigned) dropped);
        size = kept;
    }
    __real_CtrTileReader_writeFile(reader, path, data, size);
}

// ===[ Phase 2: the scene's code entries ]===
//
// Where the bytecode lives on file (the Old models: DataWin_moveBytecodeToFile), each code entry is read back the first
// time it runs, in play. At each room change, before the tiles' read-ahead starts (a bytecode read would wait behind
// the tile reader's), the entries the plan's scene first used after the room change and not in memory yet are read
// back, in first-use order, as their first run would (DataWin_codeBytes: the same reads, the same code memory, only
// earlier). The frame that changes rooms isn't shown, so the reads lengthen the transition instead of a shown frame.
// A battle room's scene is its battle group's (global.battlegroup), else the room's. The New models keep the bytecode
// in memory: nothing to do there.

#include "renderer.h"
#include "runner.h"
#include "vm.h"
#include "int_rvalue_hashmap.h"

// Read ahead per room change at most (the lazy path would read the same, a group at a time).
#define ROOMPLAN_CODE_BYTES (256u * 1024u)
#define ROOMPLAN_CODE_MS 250u

static const char rpBattleVar[] ROOMPLAN_CONST = "battlegroup";

__attribute__((section(".bss.late.roomplan"))) static struct {
    VMContext* battleFor; // the VM the variable id below belongs to
    int32_t battleVar;    // varNameMap id of battlegroup, -1 none
} rpb;

// global.battlegroup, or -1.
ROOMPLAN_TEXT static int32_t battleGroup(Runner* runner) {
    VMContext* ctx = runner->vmContext;
    if (ctx == nullptr || ctx->globalScopeInstance == nullptr) return -1;
    if (rpb.battleFor != ctx) {
        ptrdiff_t slot = shgeti(ctx->varNameMap, (char*) rpBattleVar);
        rpb.battleVar = 0 > slot ? -1 : (int32_t) ctx->varNameMap[slot].value;
        rpb.battleFor = ctx;
    }
    if (0 > rpb.battleVar) return -1;
    RValue* value = IntRValueHashMap_findSlot(&ctx->globalScopeInstance->selfVars, rpb.battleVar);
    // Not RValue_toInt32: its out-of-line helper is a static in rvalue.h and would land in .text here, moving all
    // that follows. A battle group is a number.
    if (value == nullptr) return -1;
    if (value->type == RVALUE_REAL) return (int32_t) value->real;
    if (value->type == RVALUE_INT32) return value->int32;
    return -1;
}

ROOMPLAN_TEXT static int32_t battleGroupOf(void* runner) {
    return battleGroup(runner);
}

#if !defined(CTRPORT_ROOMPLAN_RECORDER) && (!defined(ROOMPLAN_OWN_CODE_READ) || ROOMPLAN_OWN_CODE_READ)
static const char rpCodeRead[] ROOMPLAN_CONST = "Room plan: room %ld scene %ld read %u code groups ahead (%u KB, %u of %u entries) in %.1f ms%s\n";
static const char rpCut[] ROOMPLAN_CONST = ", cut short";
static const char rpNone[] ROOMPLAN_CONST = "";

ROOMPLAN_TEXT static void readPlannedCode(Runner* runner) {
    if (rp.plan == nullptr || runner == nullptr || rp.cache == nullptr) return;
    DataWin* dw = runner->dataWin;
    if (dw == nullptr || dw != rp.cache->dataWin || dw->bytecodeFile == nullptr || 0 > runner->currentRoomIndex) return;
    uint32_t count = 0;
    int32_t sub = -1;
    const uint8_t* code = RoomPlan_sceneCode(rp.plan, (uint32_t) runner->currentRoomIndex, battleGroupOf, runner, &count, &sub);
    if (code == nullptr || count == 0) return;
    uint64_t start = svcGetSystemTick();
    uint64_t limit = (uint64_t) ROOMPLAN_CODE_MS * (uint64_t) CPU_TICKS_PER_MSEC;
    uint32_t loads = dw->codeStats.loads, bytes = dw->codeStats.loadedBytes, taken = 0;
    bool cut = false;
    for (uint32_t i = 0; count > i; i++) {
        uint16_t index;
        memcpy(&index, code + (size_t) i * 2, 2);
        if (index >= dw->code.count) continue;
        CodeEntry* entry = &dw->code.entries[index];
        if (!entry->present || entry->bytecode != nullptr) continue;
        if (dw->codeStats.loadedBytes - bytes >= ROOMPLAN_CODE_BYTES || svcGetSystemTick() - start >= limit) {
            cut = true;
            break;
        }
        DataWin_codeBytes(dw, entry);
        taken++;
    }
    if (dw->codeStats.loads != loads) {
        logInfo(rpCodeRead, (long) runner->currentRoomIndex, (long) sub, (unsigned) (dw->codeStats.loads - loads),
                (unsigned) ((dw->codeStats.loadedBytes - bytes + 1023) / 1024), (unsigned) taken, (unsigned) count,
                (double) (svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC, cut ? rpCut : rpNone);
    }
}
#endif

// For roomload's room change: roomload reads the room's object code in file
// order, neighbouring groups in one read, within one shared budget (z_roomload.c, rlReadCode over rl.codeIds). Its wrap
// appends the plan's entries to rl.codeIds with this, after its own, and the merge sets ROOMPLAN_OWN_CODE_READ to 0 so
// the read below stands down. Writes at most max code entry indices of the runner's room's scene (its battle group's
// if planned) to out, in first-use order; returns how many. Entries in memory are left in: rlReadCode skips them.
uint32_t RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max);
ROOMPLAN_TEXT uint32_t RoomPlan_roomCode(Runner* runner, int32_t* out, uint32_t max) {
    if (rp.plan == nullptr || runner == nullptr || rp.cache == nullptr || runner->dataWin != rp.cache->dataWin ||
        0 > runner->currentRoomIndex) {
        return 0;
    }
    uint32_t count = 0;
    int32_t sub = -1;
    const uint8_t* code = RoomPlan_sceneCode(rp.plan, (uint32_t) runner->currentRoomIndex, battleGroupOf, runner, &count, &sub);
    uint32_t n = 0;
    for (uint32_t i = 0; code != nullptr && count > i && max > n; i++) {
        uint16_t index;
        memcpy(&index, code + (size_t) i * 2, 2);
        if (index < runner->dataWin->code.count) out[n++] = index;
    }
    return n;
}

#ifndef ROOMPLAN_OWN_CODE_READ
#define ROOMPLAN_OWN_CODE_READ 1
#endif

void __real_CtrRenderer_prefetchRoom(Renderer* renderer);
void __wrap_CtrRenderer_prefetchRoom(Renderer* renderer);

ROOMPLAN_TEXT void __wrap_CtrRenderer_prefetchRoom(Renderer* renderer) {
#if !defined(CTRPORT_ROOMPLAN_RECORDER) && ROOMPLAN_OWN_CODE_READ
    readPlannedCode(renderer->runner);
#endif
    CTR_PREFETCHROOM_AFTER_ROOMPLAN(renderer);
}

#ifdef CTRPORT_ROOMPLAN_RECORDER
#include "rp_recorder.inc"
#endif

__attribute__((noipa, used, section(".text.late.oomport")))
static void oomSeedRoom(void* context, uint32_t room, const uint32_t* tiles, uint32_t count) {
    CtrTextureCache* cache = context;
    rp.plannedRooms++;
    if (room >= cache->roomCount || arrlen(cache->roomTiles[room]) > 0) return;
    if (!CtrArray_resize(&cache->roomTiles[room], sizeof(uint32_t), count)) return;
    if (count != 0) memcpy(cache->roomTiles[room], tiles, (size_t) count * 4);
    rp.seededRooms++;
    rp.seededTiles += count;
}

#endif // ROOMPLAN_HOST_TEST

// ===[ rpfix: the rooms.dtm filter by a room index ]===
//
// RoomPlan_retiredWriteFile filtered each rooms.dtm write with RoomPlan_dropPlanned and samePlanned: a malloc'd copy
// of every record (a seeded cache's image holds ~312) and a walk of the whole plan for each, O(records x plan) on the
// main thread, ~145 ms on the Old models 45 frames into each first visit (c0r145-148; 65 ms on Azahar Old).
// The filter below indexes the plan's rooms once per write (one walk, as forEachRoom gives them: a room's first
// room-level scene) and compares each record where it lies: the same records leave and the same bytes stay. Its code
// and constants are in rpfix's late sections, after every earlier feature's, so no late code before them moves.

#ifndef ROOMPLAN_HOST_TEST
#define RPFIX_TEXT __attribute__((noinline, section(".text.late.rpfix")))
#define RPFIX_CONST __attribute__((aligned(4), section(".rodata.rpfix_in_text")))
#else
#define RPFIX_TEXT
#define RPFIX_CONST
#endif

// A checked plan's room-level tiles of one room, count 0 if none.
typedef struct {
    const uint32_t* tiles;
    uint32_t count;
} RoomPlanRoom;

typedef struct {
    RoomPlanRoom* rooms;
    uint32_t roomCount;
} RoomPlanIndexing;

RPFIX_TEXT static void indexRoom(void* context, uint32_t room, const uint32_t* tiles, uint32_t count) {
    RoomPlanIndexing* indexing = context;
    if (indexing->roomCount > room && indexing->rooms[room].count == 0) {
        indexing->rooms[room].tiles = tiles;
        indexing->rooms[room].count = count;
    }
}

// Fills rooms[0..roomCount) from a checked plan.
void RoomPlan_indexRooms(const uint8_t* data, RoomPlanRoom* rooms, uint32_t roomCount);
RPFIX_TEXT void RoomPlan_indexRooms(const uint8_t* data, RoomPlanRoom* rooms, uint32_t roomCount) {
    memset(rooms, 0, (size_t) roomCount * sizeof(*rooms));
    RoomPlanIndexing indexing = {rooms, roomCount};
    RoomPlan_forEachRoom(data, indexRoom, &indexing);
}

// RoomPlan_dropPlanned with the index: removes every room whose tiles equal the plan's (same entries, same order) and
// returns the new size, the header's body size and checksum redone. Records are compared in place (2-byte aligned).
size_t RoomPlan_dropIndexed(uint8_t* data, size_t size, const RoomPlanRoom* rooms, uint32_t roomCount, uint32_t* dropped);
RPFIX_TEXT size_t RoomPlan_dropIndexed(uint8_t* data, size_t size, const RoomPlanRoom* rooms, uint32_t roomCount, uint32_t* dropped) {
    *dropped = 0;
    if (ROOMPLAN_DTM_HEADER_SIZE > size) return size;
    uint32_t magic;
    memcpy(&magic, data, 4);
    if (magic != ROOMPLAN_DTM_MAGIC) return size;
    uint8_t* body = data + ROOMPLAN_DTM_HEADER_SIZE;
    size_t bodySize = size - ROOMPLAN_DTM_HEADER_SIZE;
    size_t in = 0, out = 0;
    while (bodySize >= in + 4) {
        uint16_t room, count;
        memcpy(&room, body + in, 2);
        memcpy(&count, body + in + 2, 2);
        size_t record = 4 + (size_t) count * 4;
        if (record > bodySize - in) break;
        if (roomCount > room && count > 0 && rooms[room].count == count && memcmp(rooms[room].tiles, body + in + 4, record - 4) == 0) {
            (*dropped)++;
        } else {
            if (out != in) memmove(body + out, body + in, record);
            out += record;
        }
        in += record;
    }
    if (*dropped == 0) return size;
    // Whatever followed the last whole record is kept as it was (loadRoomMemory stops there too).
    if (bodySize > in) {
        memmove(body + out, body + in, bodySize - in);
        out += bodySize - in;
    }
    uint32_t newBody = (uint32_t) out;
    uint32_t checksum = RoomPlan_fnv1a(body, out);
    memcpy(data + ROOMPLAN_DTM_BODY_SIZE_AT, &newBody, 4);
    memcpy(data + ROOMPLAN_DTM_CHECKSUM_AT, &checksum, 4);
    return ROOMPLAN_DTM_HEADER_SIZE + out;
}

#ifndef ROOMPLAN_HOST_TEST
static const char rpfixKept[] RPFIX_CONST = "Room plan: rooms.dtm leaves out %u rooms still as planned (%.2f ms)\n";

void __wrap_CtrTileReader_writeFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size);

// rooms.dtm leaves out the rooms still as planned. The reader writes it later (and frees data), so it is changed here,
// in place, before it is handed over. The index lives only for this write: no memory is kept between writes.
RPFIX_TEXT void __wrap_CtrTileReader_writeFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size) {
    size_t length = strlen(path);
    if (rp.plan != nullptr && data != nullptr && length >= sizeof(rpDtm) - 1 && strcmp(path + length - (sizeof(rpDtm) - 1), rpDtm) == 0) {
        uint64_t start = svcGetSystemTick();
        RoomPlanHeader h;
        memcpy(&h, rp.plan, sizeof(h));
        RoomPlanRoom* rooms = malloc(h.roomCount > 0 ? (size_t) h.roomCount * sizeof(RoomPlanRoom) : sizeof(RoomPlanRoom));
        if (rooms == nullptr) {
            // No memory for the index: main's walk (slow; a record whose small copy fails is kept; it hands over too).
            RoomPlan_retiredWriteFile(reader, path, data, size);
            return;
        }
        RoomPlan_indexRooms(rp.plan, rooms, h.roomCount);
        uint32_t dropped = 0;
        size = RoomPlan_dropIndexed(data, size, rooms, h.roomCount, &dropped);
        free(rooms);
        if (dropped > 0) logInfo(rpfixKept, (unsigned) dropped, (double) (svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
    }
    __real_CtrTileReader_writeFile(reader, path, data, size);
}
#endif

#if defined(ENABLE_WAD16) && !defined(ROOMPLAN_HOST_TEST)
// utstall (z_utstall.c): the checked plan, if it is this cache's, and the running battle group. Its late code.
const uint8_t* RoomPlan_utstallPlan(const CtrTextureCache* cache);
int32_t RoomPlan_utstallGroup(Runner* runner);

__attribute__((noinline, section(".text.late.utstall"))) const uint8_t* RoomPlan_utstallPlan(const CtrTextureCache* cache) {
    return rp.plan != nullptr && rp.cache == cache ? rp.plan : nullptr;
}

__attribute__((noinline, section(".text.late.utstall"))) int32_t RoomPlan_utstallGroup(Runner* runner) {
    return battleGroup(runner);
}
#endif

#endif // ENABLE_WAD16 || ROOMPLAN_HOST_TEST
