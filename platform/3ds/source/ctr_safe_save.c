#include "ctr_safe_save.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define saveMkdir(path) _mkdir(path)
#define saveSync(file) _commit(_fileno(file))
#define saveUnlink(path) _unlink(path)
#else
#include <unistd.h>
#define saveMkdir(path) mkdir(path, 0777)
#define saveSync(file) fsync(fileno(file))
#define saveUnlink(path) unlink(path)
#endif

bool CtrSave_failed;

void CtrSafeSave_failed(void) {
    __atomic_store_n(&CtrSave_failed, true, __ATOMIC_RELEASE);
}

static char* sidePath(const char* path, const char* suffix) {
    size_t length = strlen(path), extra = strlen(suffix);
    if (length > SIZE_MAX - extra - 1) return NULL;
    char* out = malloc(length + extra + 1);
    if (out != NULL) {
        memcpy(out, path, length);
        memcpy(out + length, suffix, extra + 1);
    }
    return out;
}

static void makeParents(const char* path) {
    char* parent = sidePath(path, "");
    if (parent == NULL) return;
    for (char* p = parent + 1; *p != '\0'; p++) {
        if (*p != '/'
#ifdef _WIN32
            && *p != '\\'
#endif
        ) continue;
        *p = '\0';
        saveMkdir(parent); // Existing folders and drive roots need no work.
        *p = '/';
    }
    free(parent);
}

// 1: identical; 0: different or absent; -1: could not read the old file.
static int sameContents(const char* path, const uint8_t* data, size_t size) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return errno == ENOENT ? 0 : -1;
    int result = 1;
    uint8_t buffer[4096];
    size_t offset = 0, got;
    while ((got = fread(buffer, 1, sizeof(buffer), f)) != 0) {
        if (got > size - offset || memcmp(buffer, data + offset, got) != 0) { result = 0; break; }
        offset += got;
    }
    if (ferror(f)) result = -1;
    else if (result == 1 && offset != size) result = 0;
    if (fclose(f) != 0) result = -1;
    return result;
}

bool CtrSafeSave_recover(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return true;
    if (errno != ENOENT) return false;
    char* backup = sidePath(path, ".bak");
    if (backup == NULL) return false;
    bool ok;
    if (stat(backup, &st) == 0) ok = S_ISREG(st.st_mode) && rename(backup, path) == 0;
    else ok = errno == ENOENT;
    free(backup);
    return ok;
}

bool CtrSafeSave_forgetBackup(const char* path) {
    char* backup = sidePath(path, ".bak");
    if (backup == NULL) return false;
    bool ok = saveUnlink(backup) == 0 || errno == ENOENT;
    free(backup);
    return ok;
}

bool CtrSafeSave_write(const char* path, const uint8_t* data, size_t size) {
    if (!CtrSafeSave_recover(path)) return false;
    int same = sameContents(path, data, size);
    if (same != 0) return same == 1;
    char* temporary = sidePath(path, ".ctrport.tmp");
    char* backup = sidePath(path, ".bak");
    if (temporary == NULL || backup == NULL) { free(temporary); free(backup); return false; }
    FILE* f = fopen(temporary, "wb");
    if (f == NULL && errno == ENOENT) {
        makeParents(path);
        f = fopen(temporary, "wb");
    }
    bool ok = f != NULL;
    if (f != NULL) {
        ok = fwrite(data, 1, size, f) == size;
        if (fflush(f) != 0) ok = false;
        if (saveSync(f) != 0) ok = false;
        if (fclose(f) != 0) ok = false;
    }
    if (!ok) {
        saveUnlink(temporary);
    } else {
        struct stat st;
        bool hadOld = stat(path, &st) == 0;
        if (!hadOld && errno != ENOENT) ok = false;
        if (hadOld) {
            // Avoid assuming that rename can replace an existing destination
            // (it cannot on every platform). Live bytes still exist here.
            if (saveUnlink(backup) != 0 && errno != ENOENT) ok = false;
            if (ok && rename(path, backup) != 0) ok = false;
        }
        if (ok && rename(temporary, path) != 0) {
            ok = false;
            if (hadOld) rename(backup, path); // If rollback fails, recovery on next boot uses .bak.
        }
    }
    free(temporary);
    free(backup);
    return ok;
}
