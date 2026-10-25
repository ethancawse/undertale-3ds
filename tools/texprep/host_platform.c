// Host implementations of the console services the texture preparation sources call (texprep). See shim/3ds.h.
//
// Threads, locks and the clock are real. Console-only services (the SD archive, APT's core time limit, linear
// memory) report failure or a neutral value so that the device code takes its plain stdio paths, which write the
// same bytes. Logging goes to stderr only with --verbose.
#include <3ds.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ctr_log.h"
#include "ctr_memory.h"
#include "ctr_open_files.h"
#include "ctr_watchdog.h"
#include "log.h"
#include "malloc.h"
#include "texprep_host.h"

// The heap size the device code divides by (the worker keeps an eighth of it free, at most 8 MB). The host never
// runs short, so admission checks always pass: a page is never deferred for want of memory.
u32 __ctru_heap_size = 0x40000000u;

bool texprepVerbose = false;

struct texprep_mallinfo texprep_mallinfo(void) {
    struct texprep_mallinfo mi = { 0, 0 };
    return mi;
}

// ===[ Locks and threads ]===

void LightLock_Init(LightLock* lock) {
    pthread_mutex_init(lock, NULL);
}

void LightLock_Lock(LightLock* lock) {
    pthread_mutex_lock(lock);
}

int LightLock_TryLock(LightLock* lock) {
    return pthread_mutex_trylock(lock) == 0 ? 0 : 1;
}

void LightLock_Unlock(LightLock* lock) {
    pthread_mutex_unlock(lock);
}

struct TexprepThread {
    pthread_t thread;
    ThreadFunc entry;
    void* arg;
};

static void* threadMain(void* p) {
    struct TexprepThread* t = p;
    t->entry(t->arg);
    return NULL;
}

Thread threadCreate(ThreadFunc entry, void* arg, size_t stackSize, int prio, int core, bool detached) {
    (void) stackSize;
    (void) prio;
    (void) core;
    (void) detached;
    struct TexprepThread* t = calloc(1, sizeof(*t));
    if (t == NULL) return NULL;
    t->entry = entry;
    t->arg = arg;
    // The device's 64 KB stack is for the ARM11; the host's default is larger and safe.
    if (pthread_create(&t->thread, NULL, threadMain, t) != 0) {
        free(t);
        return NULL;
    }
    return t;
}

Result threadJoin(Thread thread, u64 timeoutNs) {
    (void) timeoutNs;
    if (thread == NULL) return TEXPREP_NO_SERVICE;
    pthread_join(thread->thread, NULL);
    return 0;
}

void threadFree(Thread thread) {
    free(thread);
}

void svcSleepThread(s64 ns) {
    struct timespec ts = { (time_t) (ns / 1000000000), (long) (ns % 1000000000) };
    nanosleep(&ts, NULL);
}

s32 svcGetProcessorID(void) {
    return 0;
}

u64 svcGetSystemTick(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (u64) ts.tv_sec * SYSCLOCK_ARM11 + (u64) ts.tv_nsec * SYSCLOCK_ARM11 / 1000000000ull;
}

// Treated as a New 3DS: the foreground worker then starts two threads and never borrows the system core, so no
// core time limit is touched. The thread count does not change what is written (texprep_host.h).
Result APT_CheckNew3DS(bool* out) {
    *out = true;
    return 0;
}

Result APT_GetAppCpuTimeLimit(u32* percent) {
    *percent = 0;
    return TEXPREP_NO_SERVICE;
}

Result APT_SetAppCpuTimeLimit(u32 percent) {
    (void) percent;
    return TEXPREP_NO_SERVICE;
}

// ===[ Files ]===

// No SD archive on the host: ctr_cache_publish.c then publishes with stdio remove and rename, as its fallback.
FS_Path fsMakePath(FS_PathType type, const void* path) {
    FS_Path p = { type, path != NULL ? (u32) strlen(path) + 1 : 0, path };
    return p;
}

Result FSUSER_OpenArchive(FS_Archive* archive, FS_ArchiveID id, FS_Path path) {
    (void) archive;
    (void) id;
    (void) path;
    return TEXPREP_NO_SERVICE;
}

Result FSUSER_CloseArchive(FS_Archive archive) {
    (void) archive;
    return 0;
}

Result FSUSER_DeleteFile(FS_Archive archive, FS_Path path) {
    (void) archive;
    (void) path;
    return TEXPREP_NO_SERVICE;
}

Result FSUSER_RenameFile(FS_Archive srcArchive, FS_Path srcPath, FS_Archive dstArchive, FS_Path dstPath) {
    (void) srcArchive;
    (void) srcPath;
    (void) dstArchive;
    (void) dstPath;
    return TEXPREP_NO_SERVICE;
}

void CtrOpenFiles_beginCacheOpen(void) {
}

void CtrOpenFiles_endCacheOpen(void) {
}

// As ctr_open_files.c: these errors mean the file isn't there.
bool CtrOpenFiles_missing(int err) {
    return err == ENOENT || err == EEXIST || err == ENOTDIR || err == EISDIR;
}

// ===[ Memory ]===

void* linearAlloc(size_t size) {
    return malloc(size);
}

void linearFree(void* p) {
    free(p);
}

u32 linearSpaceFree(void) {
    return 0;
}

Result GSPGPU_FlushDataCache(const void* addr, u32 size) {
    (void) addr;
    (void) size;
    return 0;
}

void* CtrMemory_allocWork(size_t size) {
    return malloc(size);
}

void CtrMemory_freeWork(void* p) {
    free(p);
}

void CtrMemory_preferLinearWork(bool prefer) {
    (void) prefer;
}

void CtrMemory_linearLockStats(uint32_t* contended, uint64_t* waitNanos) {
    *contended = 0;
    *waitNanos = 0;
}

// ===[ Logging and the watchdog ]===

void CtrWatchdog_nameThread(const char* name) {
    (void) name;
}

void CtrWatchdog_note(const char* state) {
    (void) state;
}

void CtrWatchdog_mark(const char* phase) {
    (void) phase;
}

void CtrLog_flush(void) {
    fflush(stderr);
}

// engine/src/log.c's sink.
void platformLog(const logType type, const char* format, va_list va) {
    if (!texprepVerbose && type != LOG_TYPE_ERROR) return;
    vfprintf(stderr, format, va);
}
