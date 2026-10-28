// Opt-in Azahar SD elapsed-time floors. Ordinary Release does not link this file.
#ifdef CTRPORT_RENDER_TIMING
#include <3ds.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>

#include "ctr_benchmark.h"
#include "ctr_frame_pacer.h"
#include "log.h"
#include "ctr_wrap_chain.h"

#define CODE __attribute__((noinline, section(".text.late.sdlat")))
#define RO __attribute__((section(".rodata.late.sdlat")))
#define BSS __attribute__((section(".bss.late.sdlat")))

typedef struct {
    char name[5];
    uint32_t openUs, readUs, kibUs;
} SdlatModel;

// Observed elapsed-time fits, not isolated service times. See results/sdlat.md.
// Old: a Neutral playthrough; Old 2DS: utstall10; New: median of four flowergpu5 fits.
static const SdlatModel sdlatModels[] RO = {
    { "o3ds", 62010, 8361, 138 },
    { "o2ds", 71550, 21579, 19 },
    { "n3ds", 15000, 8454, 172 },
};
static const char sdlatDeviceName[] RO = "sdmc";
static const char sdlatControlPath[] RO = CTRPORT_GAME_DIR "/benchrun.txt";
static const char sdlatKeyPrefix[] RO = "sdlat=";
static const char sdlatOffWord[] RO = "off";
static const char sdlatReadMode[] RO = "rb";
static const char sdlatMainName[] RO = "main";
static const char sdlatWorkerName[] RO = "worker";
static const char sdlatOnFormat[] RO = "SD latency: %s elapsed floors, open %.3f ms, read %.3f ms + %.3f ms/KiB; existing elapsed subtracted; no card queue\n";
static const char sdlatOffMessage[] RO = "SD latency: off (Azahar profile benchrun control)\n";
static const char sdlatFailedMessage[] RO = "SD latency: disabled: invalid key, truncated control, or SD hook unavailable\n";
static const char sdlatStatsFormat[] RO = "[sdlat] %s opens %lu reads %lu KiB %.3f io %.3f ms added %.3f ms failed %lu overlap %lu\n";

typedef struct {
    uint32_t opens, reads, failed, overlap;
    uint64_t bytes, ioTicks, addedTicks;
} SdlatStats;

static struct {
    const devoptab_t* real;
    devoptab_t hooked;
    const SdlatModel* model;
    Thread mainThread;
    LightLock lock;
    SdlatStats stats[2];
    uint32_t active[2];
    bool initialized, keyPresent, invalid;
} sdlat BSS;

void CTR_OPENFILES_AFTER_SDLAT(void);
void CTR_OPENFILES_SDLAT(void);
bool __real_CtrBench_autorun(const char* path, PrintConsole* console, const CtrDisplaySettings* display);
bool __wrap_CtrBench_autorun(const char* path, PrintConsole* console, const CtrDisplaySettings* display);
void __real_CtrPacer_logStats(void);
void __wrap_CtrPacer_logStats(void);

static bool CODE sdlatSpace(unsigned char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

// Parse and remove keys in place; duplicate/unknown values disable the shim.
// Bootstrap and autorun both use this, so the normal scene parser sees no key.
static void CODE sdlatStripKeys(char* words, bool select) {
    char* p = words;
    while (*p != '\0') {
        while (sdlatSpace((unsigned char) *p)) p++;
        char* end = p;
        while (*end != '\0' && !sdlatSpace((unsigned char) *end)) end++;
        size_t size = (size_t) (end - p);
        if (size >= sizeof(sdlatKeyPrefix) - 1 && memcmp(p, sdlatKeyPrefix, sizeof(sdlatKeyPrefix) - 1) == 0) {
            if (select) {
                bool found = size == 9 && memcmp(p + 6, sdlatOffWord, 3) == 0;
                if (sdlat.keyPresent) sdlat.invalid = true;
                sdlat.keyPresent = true;
                for (unsigned i = 0; i < sizeof(sdlatModels) / sizeof(sdlatModels[0]); i++) {
                    if (size == 10 && memcmp(p + 6, sdlatModels[i].name, 4) == 0) {
                        sdlat.model = &sdlatModels[i];
                        found = true;
                    }
                }
                if (!found) sdlat.invalid = true;
            }
            memmove(p, end, strlen(end) + 1);
        } else {
            p = end;
        }
    }
}

static uint64_t CODE sdlatNsToTicks(uint64_t ns) {
    return ns / 1000000000 * SYSCLOCK_ARM11 + (ns % 1000000000 * SYSCLOCK_ARM11 + 999999999) / 1000000000;
}

static uint64_t CODE sdlatTicksToNs(uint64_t ticks) {
    return ticks / SYSCLOCK_ARM11 * 1000000000 + (ticks % SYSCLOCK_ARM11 * 1000000000 + SYSCLOCK_ARM11 - 1) / SYSCLOCK_ARM11;
}

static unsigned CODE sdlatBeginRequest(void) {
    unsigned who = threadGetCurrent() == sdlat.mainThread ? 0 : 1;
    LightLock_Lock(&sdlat.lock);
    if (sdlat.active[1 - who] != 0) sdlat.stats[who].overlap++;
    sdlat.active[who]++;
    LightLock_Unlock(&sdlat.lock);
    return who;
}

// No lock is held during either the real I/O or the added sleep. It stalls
// precisely this caller, with no artificial serialization against other files.
static void CODE sdlatFinishRequest(unsigned who, uint64_t start, uint64_t targetNs, size_t requested, bool open, bool failed) {
    uint64_t returned = svcGetSystemTick();
    uint64_t targetTicks = sdlatNsToTicks(targetNs);
    uint64_t elapsed = returned - start;
    while (elapsed < targetTicks) {
        svcSleepThread((int64_t) sdlatTicksToNs(targetTicks - elapsed));
        elapsed = svcGetSystemTick() - start;
    }
    LightLock_Lock(&sdlat.lock);
    SdlatStats* stats = &sdlat.stats[who];
    if (open) stats->opens++;
    else stats->reads++;
    if (failed) stats->failed++;
    stats->bytes += requested;
    stats->ioTicks += returned - start;
    stats->addedTicks += elapsed - (returned - start);
    sdlat.active[who]--;
    LightLock_Unlock(&sdlat.lock);
}

static int CODE sdlatDelayedOpen(struct _reent* r, void* file, const char* path, int flags, int mode) {
    unsigned who = sdlatBeginRequest();
    uint64_t start = svcGetSystemTick();
    int result = sdlat.real->open_r(r, file, path, flags, mode);
    int err = r->_errno;
    sdlatFinishRequest(who, start, (uint64_t) sdlat.model->openUs * 1000, 0, true, result < 0);
    r->_errno = err;
    return result;
}

static DIR_ITER* CODE sdlatDelayedDirOpen(struct _reent* r, DIR_ITER* dir, const char* path) {
    unsigned who = sdlatBeginRequest();
    uint64_t start = svcGetSystemTick();
    DIR_ITER* result = sdlat.real->diropen_r(r, dir, path);
    int err = r->_errno;
    sdlatFinishRequest(who, start, (uint64_t) sdlat.model->openUs * 1000, 0, true, result == NULL);
    r->_errno = err;
    return result;
}

static ssize_t CODE sdlatDelayedRead(struct _reent* r, void* file, char* data, size_t size) {
    unsigned who = sdlatBeginRequest();
    uint64_t start = svcGetSystemTick();
    ssize_t result = sdlat.real->read_r(r, file, data, size);
    int err = r->_errno;
    uint64_t targetNs = size == 0 ? 0 : (uint64_t) sdlat.model->readUs * 1000
        + ((uint64_t) size * sdlat.model->kibUs * 1000 + 1023) / 1024;
    sdlatFinishRequest(who, start, targetNs, size, false, result < 0);
    r->_errno = err;
    return result;
}

void CODE CTR_OPENFILES_SDLAT(void) {
    CTR_OPENFILES_AFTER_SDLAT();
    if (sdlat.initialized) return;
    sdlat.initialized = true;
    int64_t emulator = 0;
    if (R_FAILED(svcGetSystemInfo(&emulator, 0x20000, 0))) return;
    // This sole control read precedes the shim: decide before any game data,
    // sounds or code are read, without allocating or disturbing stdio's slots.
    Handle file = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
            fsMakePath(PATH_ASCII, sdlatControlPath + 5), FS_OPEN_READ, 0))) return;
    char words[4096];
    uint32_t got = 0;
    Result result = FSFILE_Read(file, &got, 0, words, sizeof(words) - 1);
    FSFILE_Close(file);
    if (R_FAILED(result)) return;
    words[got] = '\0';
    sdlatStripKeys(words, true);
    if (got == sizeof(words) - 1) sdlat.invalid = true;
    if (sdlat.invalid) sdlat.model = NULL;
    if (sdlat.model == NULL) return;
    const devoptab_t* real = GetDeviceOpTab(sdlatDeviceName);
    if (real == NULL || real->open_r == NULL || real->read_r == NULL) {
        sdlat.invalid = true;
        sdlat.model = NULL;
        return;
    }
    LightLock_Init(&sdlat.lock);
    sdlat.mainThread = threadGetCurrent();
    sdlat.real = real;
    sdlat.hooked = *real;
    sdlat.hooked.open_r = sdlatDelayedOpen;
    sdlat.hooked.read_r = sdlatDelayedRead;
    if (real->diropen_r != NULL) sdlat.hooked.diropen_r = sdlatDelayedDirOpen;
    if (AddDevice(&sdlat.hooked) < 0) {
        sdlat.invalid = true;
        sdlat.model = NULL;
    }
}

bool CODE __wrap_CtrBench_autorun(const char* path, PrintConsole* console, const CtrDisplaySettings* display) {
    if (!sdlat.keyPresent) return __real_CtrBench_autorun(path, console, display);
    if (sdlat.invalid) logWarn(sdlatFailedMessage);
    else if (sdlat.model == NULL) logInfo(sdlatOffMessage);
    else logInfo(sdlatOnFormat, sdlat.model->name, sdlat.model->openUs / 1000.0, sdlat.model->readUs / 1000.0, sdlat.model->kibUs / 1000.0);
    FILE* f = fopen(path, sdlatReadMode);
    if (f == NULL) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
    char* words = malloc((size_t) size + 1);
    if (words == NULL) { fclose(f); return false; }
    size_t got = fread(words, 1, (size_t) size, f);
    words[got] = '\0';
    fclose(f);
    sdlatStripKeys(words, false);
    bool started = CtrBench_autorunList(words, path, console, display);
    free(words);
    return started;
}

void CODE __wrap_CtrPacer_logStats(void) {
    __real_CtrPacer_logStats();
    if (sdlat.model == NULL) return;
    SdlatStats snapshot[2];
    LightLock_Lock(&sdlat.lock);
    memcpy(snapshot, sdlat.stats, sizeof(snapshot));
    memset(sdlat.stats, 0, sizeof(sdlat.stats));
    LightLock_Unlock(&sdlat.lock);
    for (unsigned who = 0; who < 2; who++) {
        SdlatStats* s = &snapshot[who];
        logInfo(sdlatStatsFormat, who == 0 ? sdlatMainName : sdlatWorkerName, (unsigned long) s->opens, (unsigned long) s->reads,
            s->bytes / 1024.0, (double) s->ioTicks * 1000.0 / SYSCLOCK_ARM11,
            (double) s->addedTicks * 1000.0 / SYSCLOCK_ARM11, (unsigned long) s->failed, (unsigned long) s->overlap);
    }
}
#endif
