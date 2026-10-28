// Opt-in New DR code residency acquisition. Warm engine and renderer untouched.
#ifdef CTRPORT_FLOWCODE
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_textures.h"
#include "ctr_wrap_chain.h"
#include "log.h"
#include "runner.h"

#define FC_TEXT __attribute__((noinline, section(".text.late.flowfullcode")))
#define FC_RO __attribute__((section(".rodata.late.flowfullcode")))
#define FC_BSS __attribute__((section(".bss.late.flowfullcode")))

static const char fcPlanPath[] FC_RO = "romfs:/roomplan/chapter5-flowcode.bin";
static const char fcCacheName[] FC_RO = "flowcode-chapter5.bin";
static const char fcReadMode[] FC_RO = "rb";
static const char fcIdentity[] FC_RO = "[flowcode] identity sha256=%s\n";
static const char fcDisabled[] FC_RO = "[flowcode] disabled: plan or exact file identity unavailable\n";
static const char fcMoved[] FC_RO = "[flowcode] moved=%u freed=%lu B linear_before=%lu B linear_after=%lu B\n";
static const char fcRead[] FC_RO = "[flowcode] code read room=%ld index=%lu reads=%lu bytes=%lu B ms=%.3f hidden=1\n";
static const char fcMiss[] FC_RO = "[flowcode] code miss room=%ld index=%lu reads=%lu bytes=%lu B ms=%.3f hidden=0\n";
static const char fcWarm[] FC_RO = "[flowcode] preload room=%ld entries=%lu reads=%lu bytes=%lu B ms=%.3f linear_free=%lu B\n";
static const char fcSummary[] FC_RO = "[flowcode] summary room=%ld frame=%ld misses=%lu reads bytes=%lu B ms=%.3f resident=%lu B linear_free=%lu B tile_budget=%lu B\n";
static const char fcEnd[] FC_RO = "[flowcode] end\n";

typedef struct { uint32_t room, first, count; } FlowcodeRoom;
static struct {
    DataWin* dw;
    Runner* runner;
    uint8_t* plan;
    FlowcodeRoom* rooms;
    uint32_t* indices;
    uint32_t count;
    uint32_t hidden;
    uint32_t misses, missBytes;
    uint64_t missTicks;
    int32_t room;
    bool visible;
    char digest[65];
    bool known;
    CtrTextureCache* cache;
    uint32_t tileBudget;
} fc FC_BSS;

Runner* __real_Runner_create(DataWin*, VMContext*, Renderer*, FileSystem*, AudioSystem*, uint32_t);
Runner* __wrap_Runner_create(DataWin*, VMContext*, Renderer*, FileSystem*, AudioSystem*, uint32_t);
void __real_Runner_step(Runner*);
void __wrap_Runner_step(Runner*);
void __real_Runner_free(Runner*);
void __wrap_Runner_free(Runner*);
void __real_CtrBench_handleRoomChange(Runner*);
void __wrap_CtrBench_handleRoomChange(Runner*);
void __real_CtrRoomload_prefetchRoom(CtrTextureCache*, Runner*);
void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache*, Runner*);
const uint8_t* __real_DataWin_loadCodeBytes(DataWin*, CodeEntry*);
const uint8_t* __wrap_DataWin_loadCodeBytes(DataWin*, CodeEntry*);

static FC_TEXT uint32_t fcWord(const uint8_t* p) {
    uint32_t value;
    memcpy(&value, p, sizeof(value));
    return value;
}

static FC_TEXT uint32_t fcChecksum(const uint8_t* p, uint32_t size) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < size; i++) hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

FC_TEXT CtrDataVersionResult CtrDataVersion_chainFlowcode(CTR_DATAVERSION_CHAIN_PARAMS) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_FLOWCODE(path, game, part, cachePath, progress, cached, digest);
    fc.known = result == CTR_DATA_KNOWN && digest != NULL;
    if (fc.known) memcpy(fc.digest, digest, sizeof(fc.digest));
    return result;
}

static FC_TEXT bool fcLoadPlan(DataWin* dw) {
    if (!fc.known || dw->bytecodeBuffer == NULL || dw->bytecodeFile != NULL) return false;
    FILE* file = fopen(fcPlanPath, fcReadMode);
    if (file == NULL) return false;
    uint8_t h[72] = {0};
    bool valid = fread(h, 1, sizeof(h), file) == sizeof(h);
    uint64_t timestamp = 0;
    memcpy(&timestamp, h + 16, sizeof(timestamp));
    valid = valid && fcWord(h) == 0x31444346u && fcWord(h + 4) == 1u &&
        fcWord(h + 8) == dw->fileSize && fcWord(h + 12) == dw->gen8.gameID && timestamp == dw->gen8.timestamp &&
        fcWord(h + 24) == dw->room.count && fcWord(h + 28) == dw->code.count;
    static const char hex[] FC_RO = "0123456789abcdef";
    for (uint32_t i = 0; valid && i < 32; i++)
        valid = fc.digest[2 * i] == hex[h[40 + i] >> 4] && fc.digest[2 * i + 1] == hex[h[40 + i] & 15];
    uint32_t size = valid ? fcWord(h + 32) : 0;
    valid = valid && size >= 4 && size <= 1024u * 1024u && size % 4 == 0;
    uint8_t* body = valid ? malloc(size) : NULL;
    // fgetc would extract two newlib members into ordinary text (320 B).
    // These primitives are already linked; retain exact trailing-byte rejection.
    valid = valid && body != NULL && fread(body, 1, size, file) == size &&
        fseek(file, 0, SEEK_END) == 0 && ftell(file) == (long) (sizeof(h) + size);
    fclose(file);
    if (valid) valid = fcChecksum(body, size) == fcWord(h + 36);
    uint32_t count = valid ? fcWord(body) : 0;
    valid = valid && count <= dw->room.count && count <= (size - 4) / 12;
    FlowcodeRoom* rooms = (FlowcodeRoom*) (body != NULL ? body + 4 : NULL);
    uint32_t indexCount = valid ? (size - 4 - count * 12) / 4 : 0;
    uint32_t* indices = valid ? (uint32_t*) (body + 4 + count * 12) : NULL;
    for (uint32_t i = 0; valid && i < count; i++) {
        FlowcodeRoom* r = rooms + i;
        valid = r->room < dw->room.count && (i == 0 || r->room > rooms[i - 1].room) &&
            r->first <= indexCount && r->count <= indexCount - r->first;
    }
    for (uint32_t i = 0; valid && i < indexCount; i++) valid = indices[i] < dw->code.count;
    if (!valid) { free(body); return false; }
    fc.plan = body;
    fc.rooms = rooms;
    fc.indices = indices;
    fc.count = count;
    return true;
}

static FC_TEXT void fcPreload(int32_t room) {
    if (fc.dw == NULL || room < 0) return;
    fc.room = room;
    uint32_t before = fc.dw->codeStats.loads, bytes = fc.dw->codeStats.loadedBytes;
    uint64_t tick = svcGetSystemTick();
    fc.hidden++;
    uint32_t entries = 0;
    for (uint32_t i = 0; i < fc.count; i++) if (fc.rooms[i].room == (uint32_t) room) {
        FlowcodeRoom* r = fc.rooms + i;
        entries = r->count;
        for (uint32_t j = 0; j < r->count; j++) {
            CodeEntry* code = &fc.dw->code.entries[fc.indices[r->first + j]];
            if (code->present && code->bytecode == NULL) __wrap_DataWin_loadCodeBytes(fc.dw, code);
        }
        break;
    }
    fc.hidden--;
    logInfo(fcWarm, (long) room, (unsigned long) entries, (unsigned long) (fc.dw->codeStats.loads - before),
        (unsigned long) (fc.dw->codeStats.loadedBytes - bytes),
        (double) (svcGetSystemTick() - tick) / CPU_TICKS_PER_MSEC, (unsigned long) linearSpaceFree());
}

static FC_TEXT void fcReport(void) {
    if (fc.dw == NULL) return;
    logInfo(fcSummary, (long) fc.room, (long) (fc.runner != NULL ? fc.runner->frameCount : -1),
        (unsigned long) fc.misses, (unsigned long) fc.missBytes, (double) fc.missTicks / CPU_TICKS_PER_MSEC,
        (unsigned long) fc.dw->codeStats.residentBytes, (unsigned long) linearSpaceFree(),
        (unsigned long) fc.tileBudget);
}

FC_TEXT Runner* __wrap_Runner_create(DataWin* dw, VMContext* vm, Renderer* renderer,
                                     FileSystem* fs, AudioSystem* audio, uint32_t seed) {
    free(fc.plan);
    fc.plan = NULL;
    fc.dw = NULL;
    fc.runner = NULL;
    fc.cache = NULL;
    fc.tileBudget = 0;
    fc.visible = false;
    fc.hidden = 1;
    fc.room = -1;
    fc.misses = fc.missBytes = 0;
    fc.missTicks = 0;
    if (fcLoadPlan(dw)) {
        CtrPack_makeCacheDir();
        char path[1024];
        CtrPack_cachePath(path, sizeof(path), fcCacheName);
        uint32_t before = linearSpaceFree();
        bool moved = DataWin_moveBytecodeToFile(dw, path);
        uint32_t after = linearSpaceFree();
        if (moved) {
            fc.dw = dw;
            logInfo(fcIdentity, fc.digest);
        }
        logInfo(fcMoved, moved ? 1u : 0u, (unsigned long) (after >= before ? after - before : 0),
            (unsigned long) before, (unsigned long) after);
    } else logInfo(fcDisabled);
    Runner* runner = __real_Runner_create(dw, vm, renderer, fs, audio, seed);
    fc.runner = runner;
    if (fc.dw != NULL && dw->gen8.roomOrderCount > 0) fcPreload(dw->gen8.roomOrder[0]);
    fc.hidden = 0;
    return runner;
}

FC_TEXT const uint8_t* __wrap_DataWin_loadCodeBytes(DataWin* dw, CodeEntry* code) {
    uint32_t before = dw->codeStats.loads, bytes = dw->codeStats.loadedBytes;
    uint64_t tick = svcGetSystemTick();
    const uint8_t* result = __real_DataWin_loadCodeBytes(dw, code);
    uint64_t took = svcGetSystemTick() - tick;
    uint32_t reads = dw->codeStats.loads - before;
    if (dw == fc.dw && reads > 0) {
        uint32_t loaded = dw->codeStats.loadedBytes - bytes;
        bool miss = fc.visible && fc.hidden == 0;
        if (miss) { fc.misses += reads; fc.missBytes += loaded; fc.missTicks += took; }
        // Warnings cannot drop from the log queue: acquisition indices stay complete.
        logWarn(miss ? fcMiss : fcRead, (long) fc.room, (unsigned long) (code - dw->code.entries),
            (unsigned long) reads, (unsigned long) loaded, (double) took / CPU_TICKS_PER_MSEC);
    }
    return result;
}

FC_TEXT void __wrap_CtrBench_handleRoomChange(Runner* runner) {
    bool change = runner->dataWin == fc.dw && runner->pendingRoom != -1;
    if (change) {
        fcReport();
        fc.hidden++;
        fcPreload(runner->pendingRoom >= 0 ? runner->pendingRoom :
            (runner->dataWin->gen8.roomOrderCount > 0 ? runner->dataWin->gen8.roomOrder[0] : -1));
    }
    __real_CtrBench_handleRoomChange(runner);
    if (change) { fc.room = runner->currentRoomIndex; fc.hidden--; }
}

FC_TEXT void __wrap_CtrRoomload_prefetchRoom(CtrTextureCache* cache, Runner* runner) {
    bool active = runner != NULL && runner->dataWin == fc.dw;
    if (active) {
        fc.cache = cache;
        fc.hidden++;
        fcPreload(runner->currentRoomIndex);
    }
    __real_CtrRoomload_prefetchRoom(cache, runner);
    if (active) { fc.tileBudget = cache->budgetBytes; fc.hidden--; }
}

FC_TEXT void __wrap_Runner_step(Runner* runner) {
    if (runner->dataWin == fc.dw) {
        fc.visible = true;
        fc.room = runner->currentRoomIndex;
        if (fc.cache != NULL) fc.tileBudget = fc.cache->budgetBytes;
    }
    __real_Runner_step(runner);
    if (runner->dataWin == fc.dw && runner->frameCount % 150 == 0) fcReport();
}

FC_TEXT void __wrap_Runner_free(Runner* runner) {
    if (runner == fc.runner) { fc.cache = NULL; fcReport(); fc.visible = false; fc.hidden++; }
    __real_Runner_free(runner);
    if (runner == fc.runner) {
        logInfo(fcEnd);
        free(fc.plan);
        fc.plan = NULL;
        fc.dw = NULL;
        fc.runner = NULL;
        fc.cache = NULL;
    }
}
#endif
