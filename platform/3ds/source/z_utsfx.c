// utsfx: the sound plan (UNDERTALE). Late code, constants and statics only (platform/3ds/late_reserve.ld):
// nothing here moves existing code or data, and nothing runs per frame or per draw.
//
// A WAV's first play copies it into linear memory on the game thread (ctr_audio_system.c, buildSfx): 6-11 ms for the
// 150-425 KB sounds of the boss fights on the Old models, in the middle of the fight, and the play waits for a
// background read of its data before that, so it starts 10-90 ms late. The builder (tools/roomplan/build_sfxplan.py)
// ships romfs:/roomplan/undertale-sfx.dtp: for each scene (a room; a battle room's battle group), the small PCM WAVs it
// played in our recorded playthroughs (tools/roomplan/records/sounds/), in first-play order, within a byte cap. At each
// room change, in the frame that isn't shown (main.c: CtrRenderer_skipPresent, just before the new room's events run),
// the new scene's sounds not in the cache yet are copied into it, the same copy their first play would make: those the
// scene plays in its first frames right there (CtrAudioSystem_preloadSfx, the same read and copy, within a time
// budget, keeping the texture cache's floor free), the rest by a worker thread from the next frame, once the room's
// textures are prefetched (CtrAudioSystem_queueSfx, keeping 2 MiB free; the texture cache then lets idle tiles go to
// get its floor back, as after a first play's copy). Never pushing another sound out of the cache. A sound the plan
// misses, one not ready yet, or one shed since, loads at its play as before.
//
// The plan is used only when it was made from this exact data.win (the SHA-256 the boot check computed: a link of its
// wrap chain keeps the digest) and its sound and room counts; otherwise it is ignored and logged once.
//
// Hooks (CMakeLists.txt): CtrDataVersion_check (a link of its chain, ctr_wrap_chain.h) and CtrRenderer_skipPresent
// are wrapped, so their callers keep their code.
//
// File (little-endian): a 64-byte header (UtSfxHeader), then per scene a UtSfxScene and count entries of
// u16 sound index, u16 copy size in KiB (rounded up), in first-play order; the scene's first `first` entries are
// those it played in its first frames (build_sfxplan.py FIRST_FRAMES).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ENABLE_WAD16) || defined(UTSFX_HOST_TEST)

#define UTSFX_MAGIC 0x50535444u // "DTSP"
#define UTSFX_VERSION 2u
#define UTSFX_NO_SUB 0xFFFFu
#define UTSFX_MAX_SIZE (256u * 1024u)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t sha256[32];
    uint32_t soundCount;
    uint32_t roomCount;
    uint32_t sceneCount;
    uint32_t bodySize;
    uint32_t checksum; // FNV-1a of the body
    uint32_t reserved;
} UtSfxHeader;

typedef struct {
    uint16_t room;
    uint16_t sub; // battle group, UTSFX_NO_SUB for the room itself
    uint16_t count;
    uint16_t first; // the leading entries played in the scene's first frames
} UtSfxScene;

typedef enum {
    UTSFX_OK,
    UTSFX_NO_FILE,
    UTSFX_BAD_FILE,
    UTSFX_OTHER_DATA,
    UTSFX_OTHER_VERSION,
    UTSFX_NO_DIGEST,
} UtSfxStatus;

#ifndef UTSFX_HOST_TEST
#include <3ds.h>

#include "ctr_benchromfs.h"
#include "ctr_data_version.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#define UTSFX_TEXT __attribute__((noinline, section(".text.late.utsfx")))
#define UTSFX_CONST __attribute__((aligned(4), section(".rodata.utsfx_in_text")))
#define UTSFX_BSS __attribute__((section(".bss.late.utsfx")))
#else
#define UTSFX_TEXT
#define UTSFX_CONST
#define UTSFX_BSS
#endif

// ===[ The file ]===

UTSFX_TEXT static uint32_t utsfxFnv1a(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; size > i; i++) hash = (hash ^ data[i]) * 16777619u;
    return hash;
}

UTSFX_TEXT static int utsfxNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Checks a plan against the running data.win. digest: the boot check's lowercase hex SHA-256.
UTSFX_TEXT UtSfxStatus UtSfx_check(const uint8_t* data, size_t size, const char* digest, uint32_t soundCount, uint32_t roomCount) {
    UtSfxHeader h;
    if (sizeof(h) > size) return UTSFX_BAD_FILE;
    memcpy(&h, data, sizeof(h));
    if (h.magic != UTSFX_MAGIC) return UTSFX_BAD_FILE;
    if (h.version != UTSFX_VERSION) return UTSFX_OTHER_VERSION;
    if (digest == NULL || strlen(digest) != 64) return UTSFX_NO_DIGEST;
    for (int i = 0; 32 > i; i++) {
        int hi = utsfxNibble(digest[2 * i]), lo = utsfxNibble(digest[2 * i + 1]);
        if (0 > hi || 0 > lo || h.sha256[i] != (uint8_t) (hi << 4 | lo)) return UTSFX_OTHER_DATA;
    }
    if (h.soundCount != soundCount || h.roomCount != roomCount) return UTSFX_OTHER_DATA;
    if (h.bodySize != size - sizeof(h) || h.checksum != utsfxFnv1a(data + sizeof(h), h.bodySize)) return UTSFX_BAD_FILE;
    size_t at = sizeof(h);
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        UtSfxScene scene;
        if (sizeof(scene) > size - at) return UTSFX_BAD_FILE;
        memcpy(&scene, data + at, sizeof(scene));
        size_t need = sizeof(scene) + (size_t) scene.count * 4;
        if (scene.room >= roomCount || scene.first > scene.count || need > size - at) return UTSFX_BAD_FILE;
        for (uint32_t i = 0; scene.count > i; i++) {
            uint16_t sound;
            memcpy(&sound, data + at + sizeof(scene) + (size_t) i * 4, 2);
            if (sound >= soundCount) return UTSFX_BAD_FILE;
        }
        at += need;
    }
    return at == size ? UTSFX_OK : UTSFX_BAD_FILE;
}

// The entries (u16 sound, u16 KiB) of a checked plan's scene for room and battle group (-1: none), or NULL: the
// group's own scene if planned, else the room's. *first: how many lead the scene's first frames.
UTSFX_TEXT const uint8_t* UtSfx_scene(const uint8_t* data, uint32_t room, int32_t group, uint32_t* count, int32_t* sub,
                                      uint32_t* first) {
    UtSfxHeader h;
    memcpy(&h, data, sizeof(h));
    size_t at = sizeof(h);
    const uint8_t* whole = NULL;
    uint32_t wholeCount = 0, wholeFirst = 0;
    for (uint32_t s = 0; h.sceneCount > s; s++) {
        UtSfxScene scene;
        memcpy(&scene, data + at, sizeof(scene));
        if (scene.room == room) {
            if (scene.sub == UTSFX_NO_SUB) {
                whole = data + at + sizeof(scene);
                wholeCount = scene.count;
                wholeFirst = scene.first;
            } else if (0 <= group && scene.sub == (uint32_t) group) {
                *count = scene.count;
                *first = scene.first;
                *sub = group;
                return data + at + sizeof(scene);
            }
        }
        at += sizeof(scene) + (size_t) scene.count * 4;
    }
    *count = wholeCount;
    *first = wholeFirst;
    *sub = -1;
    return whole;
}

#ifndef UTSFX_HOST_TEST

// ===[ The runtime ]===

// The first frames' copies per room change at most: the transition's frame isn't shown, and the room load after it
// has its own budget. Whatever is left goes to the worker.
#define UTSFX_MS 120u
// Linear memory left free after a copy in the room change's frame, before the room's texture prefetch: the texture
// cache's floor (ctr_textures.c, LINEAR_FLOOR_BYTES 3 MB).
#define UTSFX_FLOOR_BYTES (3u * 1024u * 1024u)
// The worker's floor: the same 3 MB, so linear memory stays about 3 MB free at peak. It starts
// the frame after the room change, once the room's textures are prefetched; what doesn't fit stays a first play.
#define UTSFX_WORK_FLOOR_BYTES (3u * 1024u * 1024u)

int CtrAudioSystem_preloadSfx(int32_t soundIndex, uint32_t bytes, uint32_t floorBytes);
int CtrAudioSystem_queueSfx(int32_t soundIndex, uint32_t kib, uint32_t floorBytes);
void CtrAudioSystem_beginSfxPlan(void);
void CtrAudioSystem_sfxPlanStats(uint32_t out[4], double* ms);

static const char usPath[] UTSFX_CONST = "romfs:/roomplan/undertale-sfx.dtp";
static const char usRomfs[] UTSFX_CONST = "romfs";
static const char usRead[] UTSFX_CONST = "rb";
static const char usBattleVar[] UTSFX_CONST = "battlegroup";
static const char usLoaded[] UTSFX_CONST = "Sound plan: %u scenes (%u bytes)\n";
static const char usIgnored[] UTSFX_CONST = "Sound plan: %s not used: %s\n";
static const char usReason0[] UTSFX_CONST = "no file";
static const char usReason1[] UTSFX_CONST = "damaged or not a sound plan";
static const char usReason2[] UTSFX_CONST = "made for another data.win";
static const char usReason3[] UTSFX_CONST = "made for another plan format";
static const char usReason4[] UTSFX_CONST = "the data.win check gave no digest";
static const char usDone[] UTSFX_CONST =
    "Sound plan: room %ld scene %ld: %u copied (%u KB), %u to the worker, %u cached, %u not now, of %u, in %.1f ms\n";
static const char usWork[] UTSFX_CONST = "Sound plan: worker since the last room change: %u copied (%u KB), %u dropped, %u failed, %.1f ms\n";

UTSFX_BSS static struct {
    char digest[65];
    bool tried;         // the plan was looked for
    uint8_t* plan;      // the checked plan, or NULL
    VMContext* battleFor;
    int32_t battleVar;  // varNameMap id of battlegroup, -1 none
} us;

// A link of CtrDataVersion_check's wrap chain (its order: ctr_wrap_chain.h).
#include "ctr_wrap_chain.h"

UTSFX_TEXT CtrDataVersionResult CTR_DATAVERSION_UTSFX(const char* path, const char* game, const char* part, const char* cachePath,
                                                      void (*progress)(void), bool* cached, char digest[65]) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_UTSFX(path, game, part, cachePath, progress, cached, digest);
    memcpy(us.digest, digest, sizeof(us.digest));
    us.digest[64] = '\0';
    return result;
}

UTSFX_TEXT static const char* usReason(UtSfxStatus status) {
    if (status == UTSFX_BAD_FILE) return usReason1;
    if (status == UTSFX_OTHER_DATA) return usReason2;
    if (status == UTSFX_OTHER_VERSION) return usReason3;
    if (status == UTSFX_NO_DIGEST) return usReason4;
    return usReason0;
}

UTSFX_TEXT static void usLoad(DataWin* dw) {
    us.tried = true;
    CtrRomfs_init(usRomfs);
    FILE* f = fopen(usPath, usRead);
    UtSfxStatus status = UTSFX_NO_FILE;
    uint8_t* data = NULL;
    long size = 0;
    if (f != NULL) {
        status = UTSFX_BAD_FILE;
        if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
        if (size > 0 && UTSFX_MAX_SIZE >= (unsigned long) size && fseek(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t) size);
            if (data != NULL && fread(data, 1, (size_t) size, f) != (size_t) size) {
                free(data);
                data = NULL;
            }
        }
        fclose(f);
    }
    if (data != NULL) status = UtSfx_check(data, (size_t) size, us.digest, dw->sond.count, dw->room.count);
    if (status != UTSFX_OK) {
        free(data);
        logInfo(usIgnored, usPath, usReason(status));
        return;
    }
    us.plan = data;
    UtSfxHeader h;
    memcpy(&h, data, sizeof(h));
    logInfo(usLoaded, (unsigned) h.sceneCount, (unsigned) size);
}

// global.battlegroup, or -1 (as z_roomplan.c's battleGroup).
UTSFX_TEXT static int32_t usBattleGroup(Runner* runner) {
    VMContext* ctx = runner->vmContext;
    if (ctx == NULL || ctx->globalScopeInstance == NULL) return -1;
    if (us.battleFor != ctx) {
        ptrdiff_t slot = shgeti(ctx->varNameMap, (char*) usBattleVar);
        us.battleVar = 0 > slot ? -1 : (int32_t) ctx->varNameMap[slot].value;
        us.battleFor = ctx;
    }
    if (0 > us.battleVar) return -1;
    RValue* value = IntRValueHashMap_findSlot(&ctx->globalScopeInstance->selfVars, us.battleVar);
    if (value == NULL) return -1;
    if (value->type == RVALUE_REAL) return (int32_t) value->real;
    if (value->type == RVALUE_INT32) return value->int32;
    return -1;
}

UTSFX_TEXT static void usPreload(Runner* runner) {
    DataWin* dw = runner->dataWin;
    int32_t room = runner->pendingRoom;
    if (dw == NULL || 0 > room || (uint32_t) room >= dw->room.count) return;
    if (!us.tried) usLoad(dw);
    if (us.plan == NULL) return;
    uint32_t count = 0, first = 0;
    int32_t sub = -1;
    const uint8_t* entries = UtSfx_scene(us.plan, (uint32_t) room, usBattleGroup(runner), &count, &sub, &first);
    // The worker's results so far go in first, and what it hadn't started for the last room is dropped.
    CtrAudioSystem_beginSfxPlan();
    uint32_t work[4];
    double workMs = 0.0;
    CtrAudioSystem_sfxPlanStats(work, &workMs);
    if (work[0] > 0 || work[2] > 0 || work[3] > 0) {
        logInfo(usWork, (unsigned) work[0], (unsigned) work[1], (unsigned) work[2], (unsigned) work[3], workMs);
    }
    if (entries == NULL || count == 0) return;
    // The sounds of the scene's first frames here, the rest by the worker while the room loads and starts.
    uint64_t start = svcGetSystemTick();
    uint64_t limit = (uint64_t) UTSFX_MS * (uint64_t) CPU_TICKS_PER_MSEC;
    uint32_t copied = 0, queued = 0, cached = 0, notNow = 0, kb = 0;
    for (uint32_t i = 0; count > i; i++) {
        uint16_t sound, size;
        memcpy(&sound, entries + (size_t) i * 4, 2);
        memcpy(&size, entries + (size_t) i * 4 + 2, 2);
        int got = i >= first || svcGetSystemTick() - start >= limit ? -2 : CtrAudioSystem_preloadSfx(sound, (uint32_t) size * 1024u, UTSFX_FLOOR_BYTES);
        if (got == -2) {
            got = CtrAudioSystem_queueSfx(sound, size, UTSFX_WORK_FLOOR_BYTES);
            if (got > 0) {
                queued++;
                continue;
            }
        }
        if (got > 0) {
            copied++;
            kb += size;
        } else if (got == 0) {
            cached++;
        } else {
            notNow++;
        }
    }
    if (copied > 0 || queued > 0 || notNow > 0) {
        logInfo(usDone, (long) room, (long) sub, (unsigned) copied, (unsigned) kb, (unsigned) queued, (unsigned) cached, (unsigned) notNow,
                (unsigned) count, (double) (svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
    }
}

void __real_CtrRenderer_skipPresent(Renderer* renderer);
void __wrap_CtrRenderer_skipPresent(Renderer* renderer);

// main.c calls it in the frame that changes rooms, after the audio update and before the room change runs the new
// room's events (CtrBench_handleRoomChange).
UTSFX_TEXT void __wrap_CtrRenderer_skipPresent(Renderer* renderer) {
    __real_CtrRenderer_skipPresent(renderer);
    if (renderer->runner != NULL) usPreload(renderer->runner);
}

#endif // UTSFX_HOST_TEST

#endif // ENABLE_WAD16 || UTSFX_HOST_TEST
