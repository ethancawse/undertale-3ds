#include "ctr_cache_publish.h"

#include <3ds.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static FS_Archive archive;
static bool opened;
static bool initialized;
static bool ready;

static const char* sdPath(const char* path) {
    return strncmp(path, "sdmc:/", 6) == 0 ? path + 5 : NULL;
}

static float elapsed(u64 start) {
    return (float) ((double) (svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11);
}

static bool deleteIfPresent(const char* path) {
    if (opened) {
        const char* relative = sdPath(path);
        if (relative == NULL) return false;
        Result r = FSUSER_DeleteFile(archive, fsMakePath(PATH_ASCII, relative));
        return R_SUCCEEDED(r) || R_SUMMARY(r) == RS_NOTFOUND || R_DESCRIPTION(r) == RD_NOT_FOUND;
    }
    return remove(path) == 0 || errno == ENOENT;
}

bool CtrCachePublish_begin(const char* marker, bool direct) {
    if (initialized) return ready;
    initialized = true;
    // Fall back only before publication begins. A failed rename must not be
    // retried through a second API with ambiguous partial results.
    if (direct && sdPath(marker) != NULL)
        opened = R_SUCCEEDED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
    ready = deleteIfPresent(marker);
    return ready;
}

bool CtrCachePublish_run(const char* temporary, const char* destination, CtrCachePublishStats* stats) {
    *stats = (CtrCachePublishStats) { .direct = opened };
    if (!ready) return false;
    const char* from = sdPath(temporary);
    const char* to = sdPath(destination);
    if (opened && (from == NULL || to == NULL)) return false;
    u64 start = svcGetSystemTick();
    bool ok = deleteIfPresent(destination);
    stats->deleteMs = elapsed(start);
    start = svcGetSystemTick();
    if (ok) {
        ok = opened ? R_SUCCEEDED(FSUSER_RenameFile(archive, fsMakePath(PATH_ASCII, from), archive, fsMakePath(PATH_ASCII, to)))
                    : rename(temporary, destination) == 0;
    }
    stats->renameMs = elapsed(start);
    if (!ok) deleteIfPresent(temporary);
    return ok;
}

void CtrCachePublish_close(void) {
    if (opened) FSUSER_CloseArchive(archive);
    opened = initialized = ready = false;
}
