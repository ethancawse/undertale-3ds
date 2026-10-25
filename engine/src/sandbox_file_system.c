#include "sandbox_file_system.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include "string_compat.h"

#include "stb_ds.h"

typedef struct {
    uint8_t* data; // malloc'd, may be nullptr when size is 0
    int32_t size;
    bool deleted; // deleted in the sandbox: hides the bundle's file of the same name
} SandboxFile;

typedef struct {
    char* key; // normalized path
    SandboxFile value;
} SandboxEntry;

typedef struct {
    FileSystem base;
    FileSystem* real;
    FileSystem* bundle;
    bool enabled;
    SandboxEntry* files; // stb_ds string hashmap
} SandboxFileSystem;

// A binaryOpen handle: the real file system's handle, or a file held in memory.
typedef struct {
    bool memory;
    FileSystem* owner; // memory == false: the file system inner belongs to
    void* inner;
    // memory == true
    char* path;
    uint8_t* data; // stb_ds array
    int32_t pos;
    bool writable;
    bool dirty;
} SandboxHandle;

// ===[ Helpers ]===

static char* normalize(const char* path) {
    char* n = safeStrdup(path);
    for (char* c = n; *c != '\0'; c++) {
        if (*c == '\\') *c = '/';
    }
    return n;
}

static SandboxFile* findFile(SandboxFileSystem* sfs, const char* path) {
    char* n = normalize(path);
    ptrdiff_t i = shgeti(sfs->files, n);
    free(n);
    return 0 > i ? nullptr : &sfs->files[i].value;
}

// Stores a copy of data under path, replacing what was there.
static void storeFile(SandboxFileSystem* sfs, const char* path, const uint8_t* data, int32_t size) {
    char* n = normalize(path);
    ptrdiff_t i = shgeti(sfs->files, n);
    if (i >= 0) {
        free(sfs->files[i].value.data);
        free(n);
    } else {
        SandboxFile empty = {0};
        shput(sfs->files, n, empty);
        i = shgeti(sfs->files, n);
        free(n); // stb_ds keeps its own copy (sh_new_strdup)
    }
    SandboxFile* f = &sfs->files[i].value;
    f->data = size > 0 ? safeMalloc((size_t) size) : nullptr;
    if (size > 0) memcpy(f->data, data, (size_t) size);
    f->size = size;
    f->deleted = false;
}

static void markDeleted(SandboxFileSystem* sfs, const char* path) {
    storeFile(sfs, path, nullptr, 0);
    findFile(sfs, path)->deleted = true;
}

static void clearFiles(SandboxFileSystem* sfs) {
    for (ptrdiff_t i = 0; shlen(sfs->files) > i; i++) free(sfs->files[i].value.data);
    shfree(sfs->files);
    sfs->files = nullptr;
    sh_new_strdup(sfs->files);
}

// Reads path as the sandbox sees it into a malloc'd buffer. Returns false if it doesn't exist.
static bool readSandboxed(SandboxFileSystem* sfs, const char* path, uint8_t** outData, int32_t* outSize) {
    SandboxFile* f = findFile(sfs, path);
    if (f != nullptr) {
        if (f->deleted) return false;
        *outData = safeMalloc((size_t) f->size + 1);
        if (f->size > 0) memcpy(*outData, f->data, (size_t) f->size);
        (*outData)[f->size] = 0;
        *outSize = f->size;
        return true;
    }
    if (sfs->bundle == nullptr) return false;
    return sfs->bundle->vtable->readFileBinary(sfs->bundle, path, outData, outSize);
}

static bool existsSandboxed(SandboxFileSystem* sfs, const char* path) {
    SandboxFile* f = findFile(sfs, path);
    if (f != nullptr) return !f->deleted;
    return sfs->bundle != nullptr && sfs->bundle->vtable->fileExists(sfs->bundle, path);
}

// ===[ Vtable ]===

static char* sbResolvePath(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    return sfs->real->vtable->resolvePath(sfs->real, relativePath);
}

static bool sbFileExists(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->fileExists(sfs->real, relativePath);
    return existsSandboxed(sfs, relativePath);
}

static char* sbReadFileText(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->readFileText(sfs->real, relativePath);
    SandboxFile* f = findFile(sfs, relativePath);
    if (f == nullptr) return sfs->bundle != nullptr ? sfs->bundle->vtable->readFileText(sfs->bundle, relativePath) : nullptr;
    if (f->deleted) return nullptr;
    char* text = safeMalloc((size_t) f->size + 1);
    if (f->size > 0) memcpy(text, f->data, (size_t) f->size);
    text[f->size] = '\0';
    return text;
}

static bool sbWriteFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->writeFileText(sfs->real, relativePath, contents);
    storeFile(sfs, relativePath, (const uint8_t*) contents, (int32_t) strlen(contents));
    return true;
}

static bool sbDeleteFile(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->deleteFile(sfs->real, relativePath);
    bool existed = existsSandboxed(sfs, relativePath);
    if (existed) markDeleted(sfs, relativePath);
    return existed;
}

static bool sbReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->readFileBinary(sfs->real, relativePath, outData, outSize);
    return readSandboxed(sfs, relativePath, outData, outSize);
}

static bool sbWriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->writeFileBinary(sfs->real, relativePath, data, size);
    storeFile(sfs, relativePath, data, size);
    return true;
}

static bool sbRenameFile(FileSystem* fs, const char* oldRelativePath, const char* newRelativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->renameFile(sfs->real, oldRelativePath, newRelativePath);
    uint8_t* data = nullptr;
    int32_t size = 0;
    if (!readSandboxed(sfs, oldRelativePath, &data, &size)) return false;
    storeFile(sfs, newRelativePath, data, size);
    markDeleted(sfs, oldRelativePath);
    free(data);
    return true;
}

static void* sbBinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) {
        void* inner = sfs->real->vtable->binaryOpen(sfs->real, relativePath, mode);
        if (inner == nullptr) return nullptr;
        SandboxHandle* h = safeCalloc(1, sizeof(SandboxHandle));
        h->owner = sfs->real;
        h->inner = inner;
        return h;
    }
    uint8_t* data = nullptr;
    int32_t size = 0;
    bool exists = mode != GML_FILE_BIN_WRITE && readSandboxed(sfs, relativePath, &data, &size);
    if (mode == GML_FILE_BIN_READ && !exists) return nullptr;
    SandboxHandle* h = safeCalloc(1, sizeof(SandboxHandle));
    h->memory = true;
    h->path = safeStrdup(relativePath);
    h->writable = mode != GML_FILE_BIN_READ;
    h->dirty = mode == GML_FILE_BIN_WRITE || (h->writable && !exists);
    if (exists) {
        arrsetlen(h->data, size);
        if (size > 0) memcpy(h->data, data, (size_t) size);
    }
    free(data);
    return h;
}

static void sbBinaryClose(FileSystem* fs, void* handle) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    SandboxHandle* h = handle;
    if (!h->memory) {
        h->owner->vtable->binaryClose(h->owner, h->inner);
    } else {
        // Written while the sandbox was on: it lands in the sandbox, even if the sandbox was switched off since.
        if (h->dirty && sfs->enabled) storeFile(sfs, h->path, h->data, (int32_t) arrlen(h->data));
        free(h->path);
        arrfree(h->data);
    }
    free(h);
}

static int32_t sbBinaryRead(MAYBE_UNUSED FileSystem* fs, void* handle, void* dst, int32_t n) {
    SandboxHandle* h = handle;
    if (!h->memory) return h->owner->vtable->binaryRead(h->owner, h->inner, dst, n);
    int32_t left = (int32_t) arrlen(h->data) - h->pos;
    if (n > left) n = left;
    if (0 >= n) return 0;
    memcpy(dst, h->data + h->pos, (size_t) n);
    h->pos += n;
    return n;
}

static int32_t sbBinaryWrite(MAYBE_UNUSED FileSystem* fs, void* handle, const void* src, int32_t n) {
    SandboxHandle* h = handle;
    if (!h->memory) return h->owner->vtable->binaryWrite(h->owner, h->inner, src, n);
    if (!h->writable || 0 >= n) return 0;
    if (h->pos + n > (int32_t) arrlen(h->data)) {
        int32_t old = (int32_t) arrlen(h->data);
        arrsetlen(h->data, h->pos + n);
        if (h->pos > old) memset(h->data + old, 0, (size_t) (h->pos - old));
    }
    memcpy(h->data + h->pos, src, (size_t) n);
    h->pos += n;
    h->dirty = true;
    return n;
}

static int32_t sbBinaryTell(MAYBE_UNUSED FileSystem* fs, void* handle) {
    SandboxHandle* h = handle;
    if (!h->memory) return h->owner->vtable->binaryTell(h->owner, h->inner);
    return h->pos;
}

static bool sbBinarySeek(MAYBE_UNUSED FileSystem* fs, void* handle, int32_t pos) {
    SandboxHandle* h = handle;
    if (!h->memory) return h->owner->vtable->binarySeek(h->owner, h->inner, pos);
    if (0 > pos) return false;
    h->pos = pos;
    return true;
}

static int32_t sbBinarySize(MAYBE_UNUSED FileSystem* fs, void* handle) {
    SandboxHandle* h = handle;
    if (!h->memory) return h->owner->vtable->binarySize(h->owner, h->inner);
    return (int32_t) arrlen(h->data);
}

static void sbBinaryRewrite(MAYBE_UNUSED FileSystem* fs, void* handle) {
    SandboxHandle* h = handle;
    if (!h->memory) {
        h->owner->vtable->binaryRewrite(h->owner, h->inner);
        return;
    }
    arrsetlen(h->data, 0);
    h->pos = 0;
    h->dirty = true;
}

static bool hasFilesUnder(SandboxFileSystem* sfs, const char* dir) {
    char* n = normalize(dir);
    size_t length = strlen(n);
    bool found = false;
    for (ptrdiff_t i = 0; shlen(sfs->files) > i && !found; i++) {
        const char* key = sfs->files[i].key;
        found = !sfs->files[i].value.deleted && strncmp(key, n, length) == 0 && key[length] == '/';
    }
    free(n);
    return found;
}

static bool sbDirectoryExists(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->directoryExists(sfs->real, relativePath);
    if (hasFilesUnder(sfs, relativePath)) return true;
    return sfs->bundle != nullptr && sfs->bundle->vtable->directoryExists(sfs->bundle, relativePath);
}

static bool sbCreateDirectory(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->createDirectory(sfs->real, relativePath);
    return true;
}

static bool sbDeleteDirectory(FileSystem* fs, const char* relativePath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->deleteDirectory(sfs->real, relativePath);
    char* n = normalize(relativePath);
    size_t length = strlen(n);
    for (ptrdiff_t i = 0; shlen(sfs->files) > i; i++) {
        SandboxEntry* e = &sfs->files[i];
        if (strncmp(e->key, n, length) == 0 && e->key[length] == '/') {
            free(e->value.data);
            e->value.data = nullptr;
            e->value.size = 0;
            e->value.deleted = true;
        }
    }
    free(n);
    return true;
}

static FileSystemDirEntry* sbListDirectory(FileSystem* fs, const char* relativeDirPath) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (!sfs->enabled) return sfs->real->vtable->listDirectory(sfs->real, relativeDirPath);
    FileSystemDirEntry* entries = sfs->bundle != nullptr ? sfs->bundle->vtable->listDirectory(sfs->bundle, relativeDirPath) : nullptr;
    char* dir = normalize(relativeDirPath);
    size_t length = strlen(dir);
    while (length > 0 && dir[length - 1] == '/') dir[--length] = '\0';
    // Drop bundle files deleted in the sandbox, then add the sandbox's own files.
    for (ptrdiff_t i = arrlen(entries) - 1; i >= 0; i--) {
        char path[512];
        snprintf(path, sizeof(path), "%s%s%s", dir, length > 0 ? "/" : "", entries[i].name);
        SandboxFile* f = findFile(sfs, path);
        if (f != nullptr) {
            free(entries[i].name);
            arrdel(entries, i);
        }
    }
    for (ptrdiff_t i = 0; shlen(sfs->files) > i; i++) {
        const char* key = sfs->files[i].key;
        if (sfs->files[i].value.deleted) continue;
        const char* name = key;
        if (length > 0) {
            if (strncmp(key, dir, length) != 0 || key[length] != '/') continue;
            name = key + length + 1;
        }
        if (strchr(name, '/') != nullptr) continue;
        FileSystemDirEntry e = { safeStrdup(name), false };
        arrput(entries, e);
    }
    free(dir);
    return entries;
}

static FileSystemVtable sandboxVtable = {
    .resolvePath = sbResolvePath,
    .fileExists = sbFileExists,
    .readFileText = sbReadFileText,
    .writeFileText = sbWriteFileText,
    .deleteFile = sbDeleteFile,
    .readFileBinary = sbReadFileBinary,
    .writeFileBinary = sbWriteFileBinary,
    .renameFile = sbRenameFile,
    .binaryOpen = sbBinaryOpen,
    .binaryClose = sbBinaryClose,
    .binaryRead = sbBinaryRead,
    .binaryWrite = sbBinaryWrite,
    .binaryTell = sbBinaryTell,
    .binarySeek = sbBinarySeek,
    .binarySize = sbBinarySize,
    .binaryRewrite = sbBinaryRewrite,
    .directoryExists = sbDirectoryExists,
    .createDirectory = sbCreateDirectory,
    .deleteDirectory = sbDeleteDirectory,
    .listDirectory = sbListDirectory,
};

// ===[ Public API ]===

FileSystem* SandboxFileSystem_create(FileSystem* real, FileSystem* bundleOnly) {
    SandboxFileSystem* sfs = safeCalloc(1, sizeof(SandboxFileSystem));
    sfs->base.vtable = &sandboxVtable;
    sfs->real = real;
    sfs->bundle = bundleOnly;
    sh_new_strdup(sfs->files);
    return (FileSystem*) sfs;
}

void SandboxFileSystem_destroy(FileSystem* fs) {
    if (fs == nullptr) return;
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    for (ptrdiff_t i = 0; shlen(sfs->files) > i; i++) free(sfs->files[i].value.data);
    shfree(sfs->files);
    free(sfs);
}

void SandboxFileSystem_setEnabled(FileSystem* fs, bool enabled) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (sfs->enabled == enabled) return;
    sfs->enabled = enabled;
    if (!enabled) clearFiles(sfs);
}

bool SandboxFileSystem_isEnabled(FileSystem* fs) {
    return ((SandboxFileSystem*) fs)->enabled;
}

FileSystem* SandboxFileSystem_real(FileSystem* fs) {
    return ((SandboxFileSystem*) fs)->real;
}

// Bench (minigames): what the sandbox holds, for the log when a bench scene start clears it
// (platform/3ds/source/ctr_bench_scene_start.h, which declares this). Late code, appended after everything else so
// nothing above it moves. Its strings are late constants for the same reason: a string
// literal would join .rodata and move what follows it there.
__attribute__((section(".rodata.late.sandbox"))) static const char lateDescribeDeleted[] = "%s%s (deleted)";
__attribute__((section(".rodata.late.sandbox"))) static const char lateDescribeSized[] = "%s%s (%ld B)";
__attribute__((section(".rodata.late.sandbox"))) static const char lateDescribeComma[] = ", ";

// Returns -1 while the sandbox is off (nothing is held then), else the number of files it holds.
__attribute__((noinline, section(".text.late.sandbox")))
int32_t SandboxFileSystem_lateDescribe(FileSystem* fs, char* out, size_t size) {
    SandboxFileSystem* sfs = (SandboxFileSystem*) fs;
    if (size > 0) out[0] = '\0';
    if (!sfs->enabled) return -1;
    ptrdiff_t count = shlen(sfs->files);
    size_t used = 0;
    for (ptrdiff_t i = 0; count > i; i++) {
        SandboxEntry* e = &sfs->files[i];
        char item[160];
        // The comma's own terminator is the empty string the first item starts with.
        const char* comma = i > 0 ? lateDescribeComma : lateDescribeComma + 2;
        if (e->value.deleted) snprintf(item, sizeof(item), lateDescribeDeleted, comma, e->key);
        else snprintf(item, sizeof(item), lateDescribeSized, comma, e->key, (long) e->value.size);
        size_t length = strlen(item);
        if (used + length + 4 >= size) {
            // Character stores, not a "..." literal, which GCC would put in .rodata.
            if (used + 4 <= size) {
                out[used] = out[used + 1] = out[used + 2] = '.';
                out[used + 3] = '\0';
            }
            break;
        }
        memcpy(out + used, item, length + 1);
        used += length;
    }
    return (int32_t) count;
}
