#include "ctr_file_system.h"

#include <3ds.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "ctr_log.h"
#include "ctr_safe_save.h"
#include "ctr_watchdog.h"
#include "gettime.h"
#include "overlay_file_system.h"
#include "stb_ds.h"
#include "utils.h"

#define WRITER_STACK_SIZE (32 * 1024)

// Save files kept in memory: each at most this big, all of them together at
// most CACHE_TOTAL_MAX. DELTARUNE's whole save folder is about 300 KB.
#define CACHE_FILE_MAX (128 * 1024)
#define CACHE_TOTAL_MAX (1024 * 1024)

// The writer waits this long after a write is queued before it starts. Games
// save on a room change, the moment the new room's music starts, and a slow
// card can take seconds over a write, holding up every read meanwhile. The
// delay lets the stream open and its read-ahead fill first.
#define WRITE_DELAY_NS (500ull * 1000000ull)

typedef struct {
    char* key;
    uint8_t* data; // text writes keep a terminating NUL past size
    int32_t size;
    uint64_t queuedAt;
} WriteJob;

typedef struct {
    char* key;
    bool value;
} ExistsEntry;

// A save file's contents. data has a NUL past size, so it also serves text
// reads.
typedef struct {
    char* key; // lowercase name in the save folder
    uint8_t* data;
    int32_t size;
} CachedFile;

typedef struct {
    FileSystem base;
    FileSystem* inner;
    char* saveDir; // ends with '/'
    char* bundleDir; // ends with '/'

    // The queue in order. The writer leaves a job at the front while it
    // writes it, so reads still find the contents until they are on the card.
    LightLock lock;
    CondVar wake; // a job was queued, or stop was set
    CondVar idle; // the queue became empty
    WriteJob* queue;
    bool stop;
    int hurry; // threads waiting in CtrFileSystem_flush: no write delay
    bool writeFailed; // the card may not hold what the game wrote: always write
    Thread thread;
    // Save files the writer reads into memory when it has nothing to write,
    // and what it has read, for the game thread to take.
    char** preloadNames;
    ptrdiff_t preloadNext;
    CachedFile* preloaded;

    // Only touched by the game thread.
    ExistsEntry* exists;
    // What the save folder holds (lowercase names of its files and folders),
    // listed once at start and then kept up to date, so checking for a save
    // file or reading one doesn't first ask the SD card whether it's there.
    // false when the folder couldn't be listed: every call then asks the
    // overlay file system as before.
    bool indexed;
    ExistsEntry* saveNames;
    CachedFile* contents;
    int32_t contentBytes;
    ExistsEntry* touched; // changed by the game since start: preloads of these are stale
} CtrFileSystem;

// Reads retain the overlay's streaming handle. Writable binary files are
// buffered until close and then use the same safe queue as whole-file saves.
typedef struct {
    void* inner;
    char* key;
    uint8_t* data;
    int32_t size, position, capacity;
    bool failed;
} CtrBinaryHandle;

static FileSystemVtable ctrFileSystemVtable;

// The same file is always found under the same key however the game spells
// the path: forward slashes and no leading "./".
static char* makeKey(const char* path) {
    while (path[0] == '.' && (path[1] == '/' || path[1] == '\\')) path += 2;
    char* key = safeStrdup(path);
    for (char* c = key; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }
    return key;
}

static char* lowerDup(const char* s) {
    char* out = safeStrdup(s);
    for (char* c = out; *c != '\0'; c++) *c = (char) tolower((unsigned char) *c);
    return out;
}

static char* joinDir(const char* dir, const char* name) {
    size_t dirLen = strlen(dir), nameLen = strlen(name);
    char* out = safeMalloc(dirLen + nameLen + 1);
    memcpy(out, dir, dirLen);
    memcpy(out + dirLen, name, nameLen + 1);
    return out;
}

static char* withSlash(const char* dir) {
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1] == '/') return safeStrdup(dir);
    return joinDir(dir, "/");
}

// The key relative to the save folder when it names a file in it by its full
// path, else the key itself.
static const char* saveRelative(const CtrFileSystem* cfs, const char* key) {
    size_t len = strlen(cfs->saveDir);
    if (strncasecmp(key, cfs->saveDir, len) == 0) return key + len;
    return key;
}

// A plain name, which the overlay looks for in the save folder and then in
// the bundle folder.
static bool isTopLevel(const char* rel) {
    return rel[0] != '\0' && strchr(rel, '/') == nullptr && strchr(rel, ':') == nullptr && strcmp(rel, ".") != 0 &&
           strcmp(rel, "..") != 0;
}

static bool hasName(ExistsEntry* set, const char* lkey) {
    return shgeti(set, lkey) >= 0;
}

// Reads a whole file into memory with a NUL after it. Fails when the file is
// missing or bigger than maxSize.
static bool readWhole(const char* fullPath, int32_t maxSize, uint8_t** outData, int32_t* outSize) {
    FILE* f = fopen(fullPath, "rb");
    if (f == nullptr) return false;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    long size = ftell(f);
    if (0 > size || size > maxSize || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }
    uint8_t* data = safeMalloc((size_t) size + 1);
    size_t bytesRead = fread(data, 1, (size_t) size, f);
    bool ok = bytesRead == (size_t) size && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) { free(data); return false; }
    data[bytesRead] = '\0';
    *outData = data;
    *outSize = (int32_t) bytesRead;
    return true;
}

// ===[ Save file contents ]===

static void cacheDrop(CtrFileSystem* cfs, const char* lkey) {
    ptrdiff_t i = shgeti(cfs->contents, lkey);
    if (0 > i) return;
    cfs->contentBytes -= cfs->contents[i].size;
    free(cfs->contents[i].data);
    (void) shdel(cfs->contents, lkey);
}

// Takes ownership of data (size bytes and a NUL).
static void cachePut(CtrFileSystem* cfs, const char* lkey, uint8_t* data, int32_t size) {
    cacheDrop(cfs, lkey);
    if (size > CACHE_FILE_MAX || cfs->contentBytes + size > CACHE_TOTAL_MAX) {
        free(data);
        return;
    }
    CachedFile entry = { (char*) lkey, data, size };
    shputs(cfs->contents, entry);
    cfs->contentBytes += size;
}

static uint8_t* copyWithNul(const uint8_t* data, int32_t size) {
    uint8_t* copy = safeMalloc((size_t) size + 1);
    memcpy(copy, data, (size_t) size);
    copy[size] = '\0';
    return copy;
}

// Takes what the writer has read ahead. Contents of files the game has
// changed since are out of date and dropped.
static void adoptPreloaded(CtrFileSystem* cfs) {
    if (cfs->thread == nullptr) return;
    LightLock_Lock(&cfs->lock);
    CachedFile* list = cfs->preloaded;
    cfs->preloaded = nullptr;
    LightLock_Unlock(&cfs->lock);
    for (ptrdiff_t i = 0; arrlen(list) > i; i++) {
        if (hasName(cfs->touched, list[i].key) || shgeti(cfs->contents, list[i].key) >= 0) {
            free(list[i].data);
        } else {
            cachePut(cfs, list[i].key, list[i].data, list[i].size);
        }
        free(list[i].key);
    }
    arrfree(list);
}

// Copies a save file's contents from memory, or reads it straight from the
// save folder (it is known to be there) and keeps it.
static bool readSaveFile(CtrFileSystem* cfs, const char* rel, const char* lkey, uint8_t** outData, int32_t* outSize) {
    ptrdiff_t i = shgeti(cfs->contents, lkey);
    if (i >= 0) {
        *outData = copyWithNul(cfs->contents[i].data, cfs->contents[i].size);
        *outSize = cfs->contents[i].size;
        return true;
    }
    char* full = joinDir(cfs->saveDir, rel);
    bool ok = readWhole(full, INT32_MAX, outData, outSize);
    free(full);
    if (ok) cachePut(cfs, lkey, copyWithNul(*outData, *outSize), *outSize);
    return ok;
}

// Records that the game created or replaced a file. contents may be null.
static void noteWritten(CtrFileSystem* cfs, const char* key, const uint8_t* contents, int32_t size) {
    if (!cfs->indexed) return;
    const char* rel = saveRelative(cfs, key);
    if (isTopLevel(rel)) {
        char* lkey = lowerDup(rel);
        shput(cfs->saveNames, lkey, true);
        shput(cfs->touched, lkey, true);
        if (contents != nullptr) cachePut(cfs, lkey, copyWithNul(contents, size), size);
        else cacheDrop(cfs, lkey);
        free(lkey);
    } else if (rel != key || strchr(rel, ':') == nullptr) {
        // A file in a subfolder: writing it creates the subfolder.
        const char* slash = strchr(rel, '/');
        if (slash == nullptr || slash == rel) return;
        char* first = lowerDup(rel);
        first[slash - rel] = '\0';
        if (isTopLevel(first)) shput(cfs->saveNames, first, true);
        free(first);
    }
}

// Records that a save folder entry is gone.
static void noteRemoved(CtrFileSystem* cfs, const char* key) {
    if (!cfs->indexed) return;
    const char* rel = saveRelative(cfs, key);
    if (!isTopLevel(rel)) return;
    char* lkey = lowerDup(rel);
    (void) shdel(cfs->saveNames, lkey);
    shput(cfs->touched, lkey, true);
    cacheDrop(cfs, lkey);
    free(lkey);
}

// Lists the save folder. Returns false when it can't be read.
static bool entryIsDirectory(const CtrFileSystem* cfs, const struct dirent* entry) {
#ifdef DT_DIR
    (void) cfs;
    return entry->d_type == DT_DIR;
#else
    char* path = joinDir(cfs->saveDir, entry->d_name);
    struct stat st;
    bool directory = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    free(path);
    return directory;
#endif
}

static bool indexSaveFolder(CtrFileSystem* cfs) {
    size_t len = strlen(cfs->saveDir);
    char* path = safeStrdup(cfs->saveDir);
    if (len > 1) path[len - 1] = '\0';
    DIR* dir = opendir(path);
    if (dir == nullptr) {
        // No save folder yet means no saves.
        struct stat st;
        bool missing = stat(path, &st) != 0 && errno == ENOENT;
        free(path);
        return missing;
    }
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char* lkey = lowerDup(ent->d_name);
        shput(cfs->saveNames, lkey, true);
        free(lkey);
        if (!entryIsDirectory(cfs, ent)) arrput(cfs->preloadNames, safeStrdup(ent->d_name));
    }
    closedir(dir);
    free(path);
    return true;
}

// ===[ Write queue ]===

static void freeJob(WriteJob* job) {
    free(job->key);
    free(job->data);
}

static void noteSaveFailure(CtrFileSystem* cfs, const char* key) {
    CtrSafeSave_failed();
    LightLock_Lock(&cfs->lock);
    cfs->writeFailed = true;
    LightLock_Unlock(&cfs->lock);
    logWarn("Files: could not save %s; previous data retained at the save path or .bak\n", key);
}

// Match the overlay's write-path rules, without its truncating writer.
static char* saveWritePath(const CtrFileSystem* cfs, const char* key) {
    if (key[0] == '/' || strncmp(key, cfs->saveDir, strlen(cfs->saveDir)) == 0
        || strncmp(key, cfs->bundleDir, strlen(cfs->bundleDir)) == 0) return safeStrdup(key);
    return joinDir(cfs->saveDir, key);
}

static void recoverSaveFolder(CtrFileSystem* cfs) {
    DIR* dir = opendir(cfs->saveDir);
    if (dir == nullptr) return;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        size_t length = strlen(entry->d_name);
        if (length <= 4 || strcmp(entry->d_name + length - 4, ".bak") != 0 || entryIsDirectory(cfs, entry)) continue;
        char* original = joinDir(cfs->saveDir, entry->d_name);
        original[strlen(original) - 4] = '\0';
        if (!CtrSafeSave_recover(original)) noteSaveFailure(cfs, original);
        free(original);
    }
    closedir(dir);
}

static void runJob(CtrFileSystem* cfs, const WriteJob* job) {
    uint64_t start = nowNanos();
    char* path = saveWritePath(cfs, job->key);
    bool ok = CtrSafeSave_write(path, job->data, (size_t) job->size);
    free(path);
    double ms = (double) (nowNanos() - start) / 1000000.0;
    if (ok) {
        logInfo("Files: saved %s (%d bytes) in %.0f ms (staged)\n", job->key, (int) job->size, ms);
    } else {
        noteSaveFailure(cfs, job->key);
    }
}

static void writerMain(void* arg) {
    CtrFileSystem* cfs = arg;
    uint64_t preloadStart = nowNanos();
    int preloadedFiles = 0;
    long preloadedBytes = 0;
    CtrWatchdog_nameThread("save writer");
    LightLock_Lock(&cfs->lock);
    while (true) {
        while (arrlen(cfs->queue) == 0 && cfs->preloadNext >= arrlen(cfs->preloadNames) && !cfs->stop) {
            CtrWatchdog_note("waiting for work");
            CondVar_Wait(&cfs->wake, &cfs->lock);
        }
        CtrWatchdog_note("taking a job");
        if (arrlen(cfs->queue) > 0) {
            uint64_t age = nowNanos() - cfs->queue[0].queuedAt;
            if (cfs->hurry == 0 && !cfs->stop && WRITE_DELAY_NS > age) {
                CondVar_WaitTimeout(&cfs->wake, &cfs->lock, (s64) (WRITE_DELAY_NS - age));
                continue;
            }
            WriteJob job = cfs->queue[0];
            LightLock_Unlock(&cfs->lock);
            CtrWatchdog_note("writing a save file");
            runJob(cfs, &job);
            LightLock_Lock(&cfs->lock);
            freeJob(&cfs->queue[0]);
            arrdel(cfs->queue, 0);
            if (arrlen(cfs->queue) == 0) CondVar_Broadcast(&cfs->idle);
            continue;
        }
        if (cfs->stop) break;

        // Nothing to write: read the next save file ahead.
        const char* name = cfs->preloadNames[cfs->preloadNext++];
        bool last = cfs->preloadNext == arrlen(cfs->preloadNames);
        LightLock_Unlock(&cfs->lock);
        CtrWatchdog_note("reading a save file ahead");
        char* full = joinDir(cfs->saveDir, name);
        uint8_t* data = nullptr;
        int32_t size = 0;
        bool ok = readWhole(full, CACHE_FILE_MAX, &data, &size);
        free(full);
        if (ok) {
            preloadedFiles++;
            preloadedBytes += size;
        }
        if (last) {
            logInfo("Files: read %d save files (%ld KB) ahead in %.0f ms\n", preloadedFiles, (preloadedBytes + 1023) / 1024,
                    (double) (nowNanos() - preloadStart) / 1000000.0);
        }
        LightLock_Lock(&cfs->lock);
        if (ok) {
            CachedFile entry = { lowerDup(name), data, size };
            arrput(cfs->preloaded, entry);
        }
    }
    LightLock_Unlock(&cfs->lock);
}

void CtrFileSystem_flush(FileSystem* fs) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    LightLock_Lock(&cfs->lock);
    if (arrlen(cfs->queue) > 0) {
        cfs->hurry++;
        CondVar_Signal(&cfs->wake);
        while (arrlen(cfs->queue) > 0) CondVar_Wait(&cfs->idle, &cfs->lock);
        cfs->hurry--;
    }
    LightLock_Unlock(&cfs->lock);
}

// True when the save folder file key names is known to hold exactly these
// bytes already (on the card, or queued to be), so writing it changes nothing.
// Undertale writes config.ini whenever its settings menu closes, changed or
// not, and Deltarune rewrites a chapter's filech*_9 with the same bytes.
static bool alreadyHolds(CtrFileSystem* cfs, const char* key, const uint8_t* data, int32_t size) {
    if (!cfs->indexed) return false;
    const char* rel = saveRelative(cfs, key);
    if (!isTopLevel(rel)) return false;
    LightLock_Lock(&cfs->lock);
    bool failed = cfs->writeFailed;
    LightLock_Unlock(&cfs->lock);
    if (failed) return false;
    adoptPreloaded(cfs);
    char* lkey = lowerDup(rel);
    bool same = false;
    if (hasName(cfs->saveNames, lkey)) {
        ptrdiff_t i = shgeti(cfs->contents, lkey);
        same = i >= 0 && cfs->contents[i].size == size && memcmp(cfs->contents[i].data, data, (size_t) size) == 0;
    }
    free(lkey);
    return same;
}

// Takes ownership of key and data.
static void queueWrite(CtrFileSystem* cfs, char* key, uint8_t* data, int32_t size) {
    if (alreadyHolds(cfs, key, data, size)) {
        logInfo("Files: %s unchanged (%d bytes), not written\n", key, (int) size);
        free(key);
        free(data);
        return;
    }
    shput(cfs->exists, key, true);
    noteWritten(cfs, key, data, size);
    WriteJob job = { key, data, size, nowNanos() };
    if (cfs->thread == nullptr) {
        runJob(cfs, &job);
        freeJob(&job);
        return;
    }
    LightLock_Lock(&cfs->lock);
    // An older write of the same file that hasn't started is replaced. The
    // front job may be being written right now, so it stays.
    for (ptrdiff_t i = arrlen(cfs->queue) - 1; i >= 1; i--) {
        if (strcmp(cfs->queue[i].key, key) == 0) {
            freeJob(&cfs->queue[i]);
            arrdel(cfs->queue, i);
        }
    }
    arrput(cfs->queue, job);
    CondVar_Signal(&cfs->wake);
    LightLock_Unlock(&cfs->lock);
}

// Copies the newest queued contents of key. Returns false when the file has
// no queued write.
static bool readQueued(CtrFileSystem* cfs, const char* key, uint8_t** outData, int32_t* outSize) {
    bool found = false;
    LightLock_Lock(&cfs->lock);
    for (ptrdiff_t i = arrlen(cfs->queue) - 1; i >= 0; i--) {
        const WriteJob* job = &cfs->queue[i];
        if (strcmp(job->key, key) != 0) continue;
        *outData = safeMalloc((size_t) job->size + 1);
        memcpy(*outData, job->data, (size_t) job->size);
        (*outData)[job->size] = '\0';
        *outSize = job->size;
        found = true;
        break;
    }
    LightLock_Unlock(&cfs->lock);
    return found;
}

// True when an earlier fileExists or write showed the file isn't there, so a
// read can fail without touching the card.
static bool knownMissing(CtrFileSystem* cfs, const char* key) {
    ptrdiff_t i = shgeti(cfs->exists, key);
    return i >= 0 && !cfs->exists[i].value;
}

static void forget(CtrFileSystem* cfs, const char* path) {
    char* key = makeKey(path);
    (void) shdel(cfs->exists, key);
    free(key);
}

static void forgetAll(CtrFileSystem* cfs) {
    shfree(cfs->exists);
    sh_new_strdup(cfs->exists);
}

// How a path is looked up once the save folder is indexed.
typedef enum {
    LOOKUP_OVERLAY, // not a plain name: ask the overlay file system
    LOOKUP_SAVE, // in the save folder
    LOOKUP_BUNDLE, // not in the save folder, so only the bundle folder can have it
    LOOKUP_MISSING, // a full path into the save folder that isn't there
} Lookup;

static Lookup lookup(CtrFileSystem* cfs, const char* key, const char** outRel, char** outLkey) {
    *outLkey = nullptr;
    if (!cfs->indexed) return LOOKUP_OVERLAY;
    const char* rel = saveRelative(cfs, key);
    if (!isTopLevel(rel)) return LOOKUP_OVERLAY;
    *outRel = rel;
    *outLkey = lowerDup(rel);
    if (hasName(cfs->saveNames, *outLkey)) return LOOKUP_SAVE;
    return rel == key ? LOOKUP_BUNDLE : LOOKUP_MISSING;
}

// Reads a file through the save folder index. Returns false when the overlay
// has to answer.
static bool readIndexed(CtrFileSystem* cfs, const char* key, bool* outOk, uint8_t** outData, int32_t* outSize) {
    const char* rel = nullptr;
    char* lkey = nullptr;
    Lookup where = lookup(cfs, key, &rel, &lkey);
    if (where == LOOKUP_OVERLAY) return false;
    adoptPreloaded(cfs);
    bool ok = false;
    if (where == LOOKUP_SAVE) {
        ok = readSaveFile(cfs, rel, lkey, outData, outSize);
    } else if (where == LOOKUP_BUNDLE && !knownMissing(cfs, key)) {
        char* full = joinDir(cfs->bundleDir, rel);
        ok = readWhole(full, INT32_MAX, outData, outSize);
        free(full);
        shput(cfs->exists, key, ok);
    }
    free(lkey);
    *outOk = ok;
    return true;
}

// ===[ Vtable ]===

static char* ctrResolvePath(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    return cfs->inner->vtable->resolvePath(cfs->inner, relativePath);
}

static bool ctrFileExists(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    char* key = makeKey(relativePath);
    const char* rel = nullptr;
    char* lkey = nullptr;
    Lookup where = lookup(cfs, key, &rel, &lkey);
    free(lkey);
    bool exists;
    if (where == LOOKUP_SAVE || where == LOOKUP_MISSING) {
        exists = where == LOOKUP_SAVE;
    } else {
        ptrdiff_t i = shgeti(cfs->exists, key);
        if (i >= 0) {
            exists = cfs->exists[i].value;
        } else {
            if (where == LOOKUP_BUNDLE) {
                // What the overlay would find after missing in the save folder.
                char* full = joinDir(cfs->bundleDir, rel);
                struct stat st;
                exists = stat(full, &st) == 0;
                free(full);
            } else {
                exists = cfs->inner->vtable->fileExists(cfs->inner, key);
            }
            shput(cfs->exists, key, exists);
        }
    }
    free(key);
    return exists;
}

static char* ctrReadFileText(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    char* key = makeKey(relativePath);
    uint8_t* data = nullptr;
    int32_t size = 0;
    char* text = nullptr;
    bool ok = false;
    if (readQueued(cfs, key, &data, &size)) {
        text = (char*) data;
    } else if (readIndexed(cfs, key, &ok, &data, &size)) {
        text = ok ? (char*) data : nullptr;
    } else if (!knownMissing(cfs, key)) {
        text = cfs->inner->vtable->readFileText(cfs->inner, key);
        shput(cfs->exists, key, text != nullptr);
    }
    free(key);
    return text;
}

static bool ctrReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    char* key = makeKey(relativePath);
    bool ok = readQueued(cfs, key, outData, outSize);
    if (!ok && !readIndexed(cfs, key, &ok, outData, outSize) && !knownMissing(cfs, key)) {
        ok = cfs->inner->vtable->readFileBinary(cfs->inner, key, outData, outSize);
        shput(cfs->exists, key, ok);
    }
    free(key);
    return ok;
}

static bool ctrWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    size_t len = strlen(contents);
    uint8_t* data = safeMalloc(len + 1);
    memcpy(data, contents, len + 1);
    queueWrite((CtrFileSystem*) fs, makeKey(relativePath), data, (int32_t) len);
    return true;
}

static bool ctrWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    uint8_t* copy = safeMalloc((size_t) size + 1);
    memcpy(copy, data, (size_t) size);
    queueWrite((CtrFileSystem*) fs, makeKey(relativePath), copy, size);
    return true;
}

static bool ctrDeleteFile(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    char* key = makeKey(relativePath);
    char* full = saveWritePath(cfs, key);
    free(key);
    bool forgotten = CtrSafeSave_forgetBackup(full);
    free(full);
    if (!forgotten) { noteSaveFailure(cfs, relativePath); return false; }
    forget(cfs, relativePath); // a copy in the bundle folder may still be there
    bool ok = cfs->inner->vtable->deleteFile(cfs->inner, relativePath);
    if (ok) {
        char* key = makeKey(relativePath);
        noteRemoved(cfs, key);
        free(key);
    }
    return ok;
}

static bool ctrRenameFile(FileSystem* fs, const char* oldRelativePath, const char* newRelativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    char* key = makeKey(oldRelativePath);
    char* full = saveWritePath(cfs, key);
    free(key);
    bool forgotten = CtrSafeSave_forgetBackup(full);
    free(full);
    if (!forgotten) { noteSaveFailure(cfs, oldRelativePath); return false; }
    forget(cfs, oldRelativePath);
    forget(cfs, newRelativePath);
    bool ok = cfs->inner->vtable->renameFile(cfs->inner, oldRelativePath, newRelativePath);
    if (ok) {
        char* oldKey = makeKey(oldRelativePath);
        char* newKey = makeKey(newRelativePath);
        noteRemoved(cfs, oldKey);
        noteWritten(cfs, newKey, nullptr, 0);
        free(oldKey);
        free(newKey);
    }
    return ok;
}

static char* binaryWriteKey(CtrFileSystem* cfs, const char* key) {
    char* target = saveWritePath(cfs, key);
    struct stat st;
    if (stat(target, &st) != 0 && errno == ENOENT) {
        char* bundle = cfs->inner->vtable->resolvePath(cfs->inner, key);
        if (bundle != nullptr && stat(bundle, &st) == 0) { free(target); return bundle; }
        free(bundle);
    }
    return target;
}

static void* ctrBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    CtrBinaryHandle* h = safeCalloc(1, sizeof(*h));
    h->key = makeKey(relativePath);
    if (mode == GML_FILE_BIN_READ) {
        h->inner = cfs->inner->vtable->binaryOpen(cfs->inner, relativePath, mode);
        if (h->inner == nullptr) { free(h->key); free(h); return nullptr; }
        return h;
    }
    if (mode == GML_FILE_BIN_READWRITE) {
        if (!ctrReadFileBinary(fs, h->key, &h->data, &h->size) && ctrFileExists(fs, h->key)) {
            noteSaveFailure(cfs, h->key);
            free(h->key);
            free(h);
            return nullptr;
        }
        // Preserve the overlay's read-write rule: an existing bundle file
        // is modified there when no save-folder file overrides it.
        char* target = binaryWriteKey(cfs, h->key);
        free(h->key);
        h->key = target;
    }
    h->data = safeRealloc(h->data, (size_t) h->size + 1);
    h->data[h->size] = '\0';
    h->capacity = h->size;
    return h;
}

static void ctrBinaryClose(FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrBinaryHandle* h = handle;
    if (h->inner != nullptr) { cfs->inner->vtable->binaryClose(cfs->inner, h->inner); free(h->key); }
    else if (!h->failed) queueWrite(cfs, h->key, h->data, h->size);
    else { free(h->key); free(h->data); }
    free(h);
}

static int32_t ctrBinaryRead(FileSystem* fs, void* handle, void* dst, int32_t n) {
    if (handle == nullptr || n <= 0) return 0;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrBinaryHandle* h = handle;
    if (h->inner != nullptr) return cfs->inner->vtable->binaryRead(cfs->inner, h->inner, dst, n);
    if (h->position >= h->size) return 0;
    if (n > h->size - h->position) n = h->size - h->position;
    memcpy(dst, h->data + h->position, (size_t) n);
    h->position += n;
    return n;
}

static int32_t ctrBinaryWrite(FileSystem* fs, void* handle, const void* src, int32_t n) {
    if (handle == nullptr || n <= 0) return 0;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrBinaryHandle* h = handle;
    if (h->inner != nullptr) {
        int32_t written = cfs->inner->vtable->binaryWrite(cfs->inner, h->inner, src, n);
        if (written != n) noteSaveFailure(cfs, h->key);
        return written;
    }
    if (h->failed || n > INT32_MAX - h->position) {
        if (!h->failed) noteSaveFailure(cfs, h->key);
        h->failed = true;
        return 0;
    }
    int32_t end = h->position + n;
    if (end > h->capacity) {
        int32_t capacity = h->capacity <= (INT32_MAX - 256) / 2 ? h->capacity * 2 + 256 : INT32_MAX;
        if (capacity < end) capacity = end;
        h->data = safeRealloc(h->data, (size_t) capacity + 1);
        h->capacity = capacity;
    }
    if (h->position > h->size) memset(h->data + h->size, 0, (size_t) (h->position - h->size));
    memcpy(h->data + h->position, src, (size_t) n);
    h->position = end;
    if (end > h->size) h->size = end;
    h->data[h->size] = '\0';
    return n;
}

static int32_t ctrBinaryTell(FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrBinaryHandle* h = handle;
    return h->inner != nullptr ? cfs->inner->vtable->binaryTell(cfs->inner, h->inner) : h->position;
}

static bool ctrBinarySeek(FileSystem* fs, void* handle, int32_t pos) {
    if (handle == nullptr || pos < 0) return false;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrBinaryHandle* h = handle;
    if (h->inner != nullptr) return cfs->inner->vtable->binarySeek(cfs->inner, h->inner, pos);
    h->position = pos;
    return true;
}

static int32_t ctrBinarySize(FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrBinaryHandle* h = handle;
    return h->inner != nullptr ? cfs->inner->vtable->binarySize(cfs->inner, h->inner) : h->size;
}

static void ctrBinaryRewrite(FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    CtrBinaryHandle* h = handle;
    if (h->inner != nullptr) {
        CtrFileSystem* cfs = (CtrFileSystem*) fs;
        char* target = binaryWriteKey(cfs, h->key);
        cfs->inner->vtable->binaryClose(cfs->inner, h->inner);
        h->inner = nullptr;
        free(h->key);
        h->key = target;
        h->data = safeCalloc(1, 1);
    }
    h->size = h->position = 0;
    h->data[0] = '\0';
}

static bool ctrDirectoryExists(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    return cfs->inner->vtable->directoryExists(cfs->inner, relativePath);
}

static bool ctrCreateDirectory(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    forgetAll(cfs);
    bool ok = cfs->inner->vtable->createDirectory(cfs->inner, relativePath);
    if (ok) {
        char* key = makeKey(relativePath);
        noteWritten(cfs, key, nullptr, 0);
        free(key);
    }
    return ok;
}

static bool ctrDeleteDirectory(FileSystem* fs, const char* relativePath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    forgetAll(cfs);
    bool ok = cfs->inner->vtable->deleteDirectory(cfs->inner, relativePath);
    if (ok) {
        char* key = makeKey(relativePath);
        noteRemoved(cfs, key);
        free(key);
    }
    return ok;
}

static FileSystemDirEntry* ctrListDirectory(FileSystem* fs, const char* relativeDirPath) {
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    CtrFileSystem_flush(fs);
    return cfs->inner->vtable->listDirectory(cfs->inner, relativeDirPath);
}

// ===[ Lifecycle ]===

FileSystem* CtrFileSystem_create(const char* bundlePath, const char* savePath) {
    CtrFileSystem* cfs = safeCalloc(1, sizeof(CtrFileSystem));
    ctrFileSystemVtable = (FileSystemVtable) {
        .resolvePath = ctrResolvePath,
        .fileExists = ctrFileExists,
        .readFileText = ctrReadFileText,
        .writeFileText = ctrWriteFileText,
        .deleteFile = ctrDeleteFile,
        .readFileBinary = ctrReadFileBinary,
        .writeFileBinary = ctrWriteFileBinary,
        .renameFile = ctrRenameFile,
        .binaryOpen = ctrBinaryOpen,
        .binaryClose = ctrBinaryClose,
        .binaryRead = ctrBinaryRead,
        .binaryWrite = ctrBinaryWrite,
        .binaryTell = ctrBinaryTell,
        .binarySeek = ctrBinarySeek,
        .binarySize = ctrBinarySize,
        .binaryRewrite = ctrBinaryRewrite,
        .directoryExists = ctrDirectoryExists,
        .createDirectory = ctrCreateDirectory,
        .deleteDirectory = ctrDeleteDirectory,
        .listDirectory = ctrListDirectory,
    };
    cfs->base.vtable = &ctrFileSystemVtable;
    cfs->inner = (FileSystem*) OverlayFileSystem_create(bundlePath, savePath);
    cfs->saveDir = withSlash(savePath);
    cfs->bundleDir = withSlash(bundlePath);
    sh_new_strdup(cfs->exists);
    sh_new_strdup(cfs->saveNames);
    sh_new_strdup(cfs->contents);
    sh_new_strdup(cfs->touched);
    LightLock_Init(&cfs->lock);
    CondVar_Init(&cfs->wake);
    CondVar_Init(&cfs->idle);
    recoverSaveFolder(cfs);

    uint64_t start = nowNanos();
    cfs->indexed = indexSaveFolder(cfs);
    if (cfs->indexed) {
        logInfo("Files: save folder has %d entries (listed in %.0f ms)\n", (int) shlen(cfs->saveNames),
                (double) (nowNanos() - start) / 1000000.0);
    } else {
        logWarn("Files: could not list %s\n", cfs->saveDir);
    }

    // Below the game thread on its core: the writer mostly waits on the SD
    // card, and the game should win whenever both want the CPU.
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    priority += 1;
    if (priority > 0x3F) priority = 0x3F;
    cfs->thread = threadCreate(writerMain, cfs, WRITER_STACK_SIZE, priority, -2, false);
    if (cfs->thread == nullptr) logWarn("Files: no writer thread, saving on the game thread\n");
    return &cfs->base;
}

void CtrFileSystem_destroy(FileSystem* fs) {
    if (fs == nullptr) return;
    CtrFileSystem* cfs = (CtrFileSystem*) fs;
    if (cfs->thread != nullptr) {
        LightLock_Lock(&cfs->lock);
        cfs->stop = true;
        CondVar_Signal(&cfs->wake);
        LightLock_Unlock(&cfs->lock);
        // The writer empties the queue before it sees stop.
        threadJoin(cfs->thread, U64_MAX);
        threadFree(cfs->thread);
    }
    arrfree(cfs->queue);
    for (ptrdiff_t i = 0; arrlen(cfs->preloadNames) > i; i++) free(cfs->preloadNames[i]);
    arrfree(cfs->preloadNames);
    for (ptrdiff_t i = 0; arrlen(cfs->preloaded) > i; i++) {
        free(cfs->preloaded[i].key);
        free(cfs->preloaded[i].data);
    }
    arrfree(cfs->preloaded);
    for (ptrdiff_t i = 0; shlen(cfs->contents) > i; i++) free(cfs->contents[i].data);
    shfree(cfs->contents);
    shfree(cfs->exists);
    shfree(cfs->saveNames);
    shfree(cfs->touched);
    free(cfs->saveDir);
    free(cfs->bundleDir);
    OverlayFileSystem_destroy((OverlayFileSystem*) cfs->inner);
    free(cfs);
}
