#include "ctr_open_files.h"

#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>

#include "common.h"
#include "log.h"

#define TRACKED_MAX 96
#define TRACKED_PATH 88
#define FAILURES_LOGGED 5
#define KEEPERS_MAX 8
// Below this many files open, kept-open caches may keep one more. Each cache
// file opened again costs about 10 ms on an Old 3DS, so only a crowd sheds.
// On an Old 3DS an open failed with 18 open (Chapter 4's Titan climb, with
// 16 here): the game's own opens (music streams, sounds, the chapter's
// files) come on top of what the caches keep, so this leaves them six.
#define DEFAULT_LIMIT 12
// An open that failed waits this long at most for other threads' caches to
// close their files, trying again each time one does.
#define SHED_WAIT_NS (150ll * 1000000ll)
#define SHED_STEP_NS (2ll * 1000000ll)
#define HANDLES_BELIEVABLE 12
#define HANDLE_PROBE_MAX 4096

typedef struct {
    const void* key; // the devoptab's file or folder state
    bool folder;
    char path[TRACKED_PATH];
} OpenEntry;

typedef struct {
    void (*shed)(void* owner);
    void (*wake)(void* owner);
    void* owner;
    Thread thread; // the thread that closes its files (nullptr: the main thread)
    bool used;
    bool pending; // asked to shed at its thread's next CtrOpenFiles_poll
} Keeper;

static LightLock lock;
static OpenEntry entries[TRACKED_MAX];
static int files, folders, peak;
static int untracked; // opened while the list was full
static int failuresLogged;
static int limit = DEFAULT_LIMIT;
static int fakeLimit; // open_files_fake_max: refuse opens past this many, for testing (0: off)
static Keeper keepers[KEEPERS_MAX];
static volatile uint32_t sheds; // bumped each time a cache closes its files
static devoptab_t tracked;
static const devoptab_t* real;
static __thread int quietDepth; // inside CtrOpenFiles_beginCacheOpen

// Under lock.
static void add(const void* key, bool folder, const char* path) {
    for (int i = 0; TRACKED_MAX > i; i++) {
        if (entries[i].key != nullptr) continue;
        entries[i].key = key;
        entries[i].folder = folder;
        strncpy(entries[i].path, path, TRACKED_PATH - 1);
        entries[i].path[TRACKED_PATH - 1] = '\0';
        return;
    }
    untracked++;
}

// Under lock. False for one opened before the list was kept.
static bool removeKey(const void* key) {
    for (int i = 0; TRACKED_MAX > i; i++) {
        if (entries[i].key == key) {
            entries[i].key = nullptr;
            return true;
        }
    }
    if (0 == untracked) return false;
    untracked--;
    return true;
}

static bool isMissing(int err) {
    return err == ENOENT || err == EEXIST || err == ENOTDIR || err == EISDIR;
}

bool CtrOpenFiles_missing(int err) {
    return isMissing(err);
}

// The first few opens that fail for a reason other than a missing file, with
// what was open then.
static void noteFailure(const char* what, const char* path, int err) {
    if (isMissing(err)) return;
    LightLock_Lock(&lock);
    bool log = FAILURES_LOGGED > failuresLogged;
    if (log) failuresLogged++;
    LightLock_Unlock(&lock);
    if (!log) return;
    logWarn("Files: could not open %s %s (errno %d)\n", what, path, err);
    // The FS service's own answer, which errno doesn't keep.
    if (strncmp(path, "sdmc:/", 6) == 0) {
        FS_Archive sd = 0;
        Handle file = 0;
        Result archive = FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
        Result opened = archive;
        if (R_SUCCEEDED(archive)) {
            if (strcmp(what, "file") == 0) opened = FSUSER_OpenFile(&file, sd, fsMakePath(PATH_ASCII, path + 5), FS_OPEN_READ, 0);
            else opened = FSUSER_OpenDirectory(&file, sd, fsMakePath(PATH_ASCII, path + 5));
            if (R_SUCCEEDED(opened) && strcmp(what, "file") == 0) FSFILE_Close(file);
            else if (R_SUCCEEDED(opened)) FSDIR_Close(file);
            FSUSER_CloseArchive(sd);
        }
        logWarn("Files: opened again through FS: archive %08lX, %s %08lX\n", (unsigned long) archive, what, (unsigned long) opened);
    }
    CtrOpenFiles_log("then", true);
}

// Runs the calling thread's caches that were asked to shed. Never under lock.
static int shedOwn(void) {
    Thread self = threadGetCurrent();
    int shed = 0;
    for (int i = 0; KEEPERS_MAX > i; i++) {
        LightLock_Lock(&lock);
        Keeper k = keepers[i];
        bool run = k.used && k.pending && k.thread == self;
        if (run) keepers[i].pending = false;
        LightLock_Unlock(&lock);
        if (!run) continue;
        k.shed(k.owner);
        shed++;
    }
    if (0 < shed) __atomic_add_fetch(&sheds, 1, __ATOMIC_RELEASE);
    return shed;
}

// Asks every cache to close its kept-open files. Those of the calling thread
// close them now unless it is opening a cache file itself (its caller may
// hold one); the others at their thread's next poll. Returns whether another
// thread's cache was asked.
static bool askToShed(bool own) {
    Thread self = threadGetCurrent();
    bool others = false;
    void (*wakes[KEEPERS_MAX])(void*);
    void* owners[KEEPERS_MAX];
    int wakeCount = 0;
    LightLock_Lock(&lock);
    for (int i = 0; KEEPERS_MAX > i; i++) {
        Keeper* k = &keepers[i];
        if (!k->used) continue;
        if (k->thread == self && !own) continue;
        k->pending = true;
        if (k->thread == self) continue;
        others = true;
        if (k->wake != nullptr) {
            wakes[wakeCount] = k->wake;
            owners[wakeCount++] = k->owner;
        }
    }
    LightLock_Unlock(&lock);
    for (int i = 0; wakeCount > i; i++) wakes[i](owners[i]);
    if (own) shedOwn();
    return others;
}

// Whether the open_files_fake_max test setting refuses another open, as the
// FS service does once too many are open.
static bool fakeFull(struct _reent* r) {
    if (0 >= fakeLimit) return false;
    LightLock_Lock(&lock);
    bool full = files + folders >= fakeLimit;
    LightLock_Unlock(&lock);
    if (full) r->_errno = EMFILE;
    return full;
}

static int attemptOpen(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
    return fakeFull(r) ? -1 : real->open_r(r, fileStruct, path, flags, mode);
}

static DIR_ITER* attemptDirOpen(struct _reent* r, DIR_ITER* dirState, const char* path) {
    return fakeFull(r) ? nullptr : real->diropen_r(r, dirState, path);
}

static int openCount(void) {
    LightLock_Lock(&lock);
    int count = files + folders;
    LightLock_Unlock(&lock);
    return count;
}

// Opens through `attempt` and, should that fail with the card's files
// crowded, has the caches close theirs and tries again.
#define RETRY_OPEN(ok, attempt)                                                                   \
    do {                                                                                          \
        if (ok || isMissing(r->_errno)) break;                                                    \
        int failedErrno = r->_errno;                                                              \
        int crowd = openCount();                                                                  \
        bool own = 0 == quietDepth;                                                               \
        uint32_t seen = __atomic_load_n(&sheds, __ATOMIC_ACQUIRE);                                \
        bool others = askToShed(own);                                                             \
        if (own) attempt;                                                                         \
        int64_t waited = 0;                                                                       \
        while (!(ok) && others && SHED_WAIT_NS > waited) {                                         \
            svcSleepThread(SHED_STEP_NS);                                                         \
            waited += SHED_STEP_NS;                                                               \
            uint32_t now = __atomic_load_n(&sheds, __ATOMIC_ACQUIRE);                             \
            if (now == seen) continue;                                                            \
            seen = now;                                                                           \
            attempt;                                                                              \
        }                                                                                         \
        if (ok) logInfo("Files: opened %s once kept-open files were closed (%d were open, errno "  \
                        "%d; %.0f ms)\n", path, crowd, failedErrno, (double) waited / 1e6);       \
    } while (0)

static int trackedOpen(struct _reent* r, void* fileStruct, const char* path, int flags, int mode) {
    int fd = attemptOpen(r, fileStruct, path, flags, mode);
    RETRY_OPEN(0 <= fd, fd = attemptOpen(r, fileStruct, path, flags, mode));
    if (0 > fd) {
        // The caller tells a missing file from a crowded card by errno.
        int err = r->_errno;
        noteFailure("file", path, err);
        r->_errno = err;
        return fd;
    }
    LightLock_Lock(&lock);
    add(fileStruct, false, path);
    files++;
    if (files + folders > peak) peak = files + folders;
    LightLock_Unlock(&lock);
    return fd;
}

static int trackedClose(struct _reent* r, void* fileStruct) {
    LightLock_Lock(&lock);
    if (removeKey(fileStruct)) files--;
    LightLock_Unlock(&lock);
    return real->close_r(r, fileStruct);
}

static DIR_ITER* trackedDirOpen(struct _reent* r, DIR_ITER* dirState, const char* path) {
    DIR_ITER* dir = attemptDirOpen(r, dirState, path);
    RETRY_OPEN(dir != nullptr, dir = attemptDirOpen(r, dirState, path));
    if (dir == nullptr) {
        int err = r->_errno;
        noteFailure("folder", path, err);
        r->_errno = err;
        return dir;
    }
    LightLock_Lock(&lock);
    add(dirState, true, path);
    folders++;
    if (files + folders > peak) peak = files + folders;
    LightLock_Unlock(&lock);
    return dir;
}

static int trackedDirClose(struct _reent* r, DIR_ITER* dirState) {
    LightLock_Lock(&lock);
    if (removeKey(dirState)) folders--;
    LightLock_Unlock(&lock);
    return real->dirclose_r(r, dirState);
}

void CtrOpenFiles_track(void) {
    if (real != nullptr) return;
    const devoptab_t* sd = GetDeviceOpTab("sdmc");
    if (sd == nullptr || sd->open_r == nullptr || sd->close_r == nullptr) return;
    LightLock_Init(&lock);
    real = sd;
    tracked = *sd;
    tracked.open_r = trackedOpen;
    tracked.close_r = trackedClose;
    if (sd->diropen_r != nullptr && sd->dirclose_r != nullptr) {
        tracked.diropen_r = trackedDirOpen;
        tracked.dirclose_r = trackedDirClose;
    }
    // Replaces the device of the same name in place.
    if (0 > AddDevice(&tracked)) real = nullptr;
}

void CtrOpenFiles_setLimit(int max, int fakeMax) {
    if (0 < max) limit = max;
    fakeLimit = fakeMax;
    if (0 < fakeLimit) logInfo("Files: kept-open files close once %d are open; opens past %d refused (test)\n", limit, fakeLimit);
    else logInfo("Files: kept-open files close once %d are open\n", limit);
}

bool CtrOpenFiles_mayKeep(void) {
    if (real == nullptr) return true;
    LightLock_Lock(&lock);
    bool may = limit > files + folders;
    LightLock_Unlock(&lock);
    return may;
}

int CtrOpenFiles_addKeeper(void (*shed)(void* owner), void (*wake)(void* owner), void* owner) {
    if (real == nullptr) return -1;
    LightLock_Lock(&lock);
    int id = -1;
    for (int i = 0; KEEPERS_MAX > i && 0 > id; i++) {
        if (keepers[i].used) continue;
        keepers[i] = (Keeper) { shed, wake, owner, threadGetCurrent(), true, false };
        id = i;
    }
    LightLock_Unlock(&lock);
    return id;
}

void CtrOpenFiles_removeKeeper(int id) {
    if (0 > id || KEEPERS_MAX <= id) return;
    LightLock_Lock(&lock);
    keepers[id].used = false;
    LightLock_Unlock(&lock);
}

void CtrOpenFiles_poll(void) {
    if (real == nullptr) return;
    shedOwn();
}

void CtrOpenFiles_beginCacheOpen(void) {
    quietDepth++;
}

void CtrOpenFiles_endCacheOpen(void) {
    quietDepth--;
}

// Azahar answers svcGetSystemInfo's type 0x20000; a console doesn't.
static bool onEmulator(void) {
    s64 out = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&out, 0x20000, 0));
}

// How many more handles the process's table takes, found by filling it with
// copies of the process handle (then closed). Only where the kernel won't
// say how many are in use (Azahar): meanwhile every other thread's opens
// would fail.
static int freeHandles(void) {
    static Handle probe[HANDLE_PROBE_MAX];
    int count = 0;
    while (HANDLE_PROBE_MAX > count && R_SUCCEEDED(svcDuplicateHandle(&probe[count], CUR_PROCESS_HANDLE))) count++;
    for (int i = 0; count > i; i++) svcCloseHandle(probe[i]);
    return count;
}

void CtrOpenFiles_log(const char* where, bool list) {
    // svcGetProcessInfo 4: kernel handles in use; 5: the most at once. Azahar
    // answers with junk, so only a believable count is shown (a process holds
    // a dozen or more for its threads and services alone).
    s64 handles = -1, handlesPeak = -1;
    if (R_FAILED(svcGetProcessInfo(&handles, CUR_PROCESS_HANDLE, 4)) || HANDLES_BELIEVABLE > handles || handles > 0x10000) handles = -1;
    if (R_FAILED(svcGetProcessInfo(&handlesPeak, CUR_PROCESS_HANDLE, 5)) || HANDLES_BELIEVABLE > handlesPeak || handlesPeak > 0x10000) {
        handlesPeak = -1;
    }
    char kernel[64];
    if (0 <= handles) snprintf(kernel, sizeof(kernel), "%d kernel handles (most %d)", (int) handles, (int) handlesPeak);
    else if (onEmulator()) snprintf(kernel, sizeof(kernel), "room for %d more kernel handles", freeHandles());
    else snprintf(kernel, sizeof(kernel), "kernel handles unknown");
    if (real == nullptr) {
        logInfo("Files (%s): %s; SD card opens not tracked\n", where, kernel);
        return;
    }
    LightLock_Lock(&lock);
    int f = files, d = folders, p = peak, u = untracked;
    LightLock_Unlock(&lock);
    logInfo("Files (%s): %d files and %d folders open on the SD card (most %d), %s\n", where, f, d, p, kernel);
    if (!list) return;
    // One entry at a time: the threads that log a failure have small stacks.
    for (int i = 0; TRACKED_MAX > i; i++) {
        OpenEntry e;
        LightLock_Lock(&lock);
        e = entries[i];
        LightLock_Unlock(&lock);
        if (e.key != nullptr) logInfo("Files:   %s%s\n", e.folder ? "folder " : "", e.path);
    }
    if (0 < u) logInfo("Files:   and %d more not listed\n", u);
}
