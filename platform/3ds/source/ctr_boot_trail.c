// The boot trail (ctr_boot_trail.h). Lab mode starts it first thing (main.c), marks each step of the start up to the
// main loop, and ctr_lab.c adds how the run ended when it leaves. A line is
// "<ms since open> <step>". The last line says where a run that never came up stopped, even when nothing else was
// written: every mark is its own open, write (flushed) and close, straight to the FS service, so it is on the card
// as soon as the mark returns, whatever happens to the process afterwards.
#include "ctr_boot_trail.h"

#include <3ds.h>
#include <string.h>

#include "ctr_lab_proto.h"

#define TRAIL_LINE_MAX 112

static char trailPath[LAB_PATH_MAX];     // "/3ds/undertale-3ds/labboot.txt"
static char trailPrevPath[LAB_PATH_MAX]; // "<that>.prev"
static volatile bool opened;
static u64 openedAt;
static volatile u32 offset;
static const char* volatile lastStep = "not started";

static u64 ticksToMs(u64 ticks) {
    return ticks / CPU_TICKS_PER_MSEC;
}

// Makes each folder on the way to the file ("/3ds", "/3ds/the port", ...); the ones already there just fail.
static void makeFolders(FS_Archive sdmc) {
    char folder[LAB_PATH_MAX];
    size_t length = strlen(trailPath);
    for (size_t i = 1; length > i; i++) {
        if (trailPath[i] != '/') continue;
        memcpy(folder, trailPath, i);
        folder[i] = '\0';
        FSUSER_CreateDirectory(sdmc, fsMakePath(PATH_ASCII, folder), 0);
    }
}

void CtrBootTrail_open(const char* path) {
    if (opened || path == NULL) return;
    if (strncmp(path, "sdmc:", 5) == 0) path += 5;
    trailPath[0] = trailPrevPath[0] = '\0';
    LabProto_append(trailPath, sizeof(trailPath), path);
    LabProto_append(trailPrevPath, sizeof(trailPrevPath), path);
    LabProto_append(trailPrevPath, sizeof(trailPrevPath), ".prev");
    FS_Archive sdmc;
    if (R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        makeFolders(sdmc);
        FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, trailPrevPath));
        FSUSER_RenameFile(sdmc, fsMakePath(PATH_ASCII, trailPath), sdmc, fsMakePath(PATH_ASCII, trailPrevPath));
        FSUSER_CloseArchive(sdmc);
    }
    // Empty, in case the old one couldn't be moved.
    Handle file = 0;
    if (R_SUCCEEDED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, trailPath),
                                            FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        FSFILE_SetSize(file, 0);
        FSFILE_Close(file);
    }
    openedAt = svcGetSystemTick();
    offset = 0;
    opened = true;
}

void CtrBootTrail_mark(const char* step) {
    if (step == NULL) return;
    lastStep = step;
    if (!opened) return;
    // "<ms> <step>\n", without stdio.
    char line[TRAIL_LINE_MAX];
    char digits[12];
    int count = 0;
    u64 ms = ticksToMs(svcGetSystemTick() - openedAt);
    do {
        digits[count++] = (char) ('0' + ms % 10);
        ms /= 10;
    } while (ms > 0 && (int) sizeof(digits) > count);
    u32 used = 0;
    while (count > 0) line[used++] = digits[--count];
    line[used++] = ' ';
    for (const char* c = step; *c != '\0' && TRAIL_LINE_MAX - 1 > used; c++) line[used++] = *c;
    line[used++] = '\n';
    // Each line gets its own place in the file, whichever thread marks.
    u32 at = __atomic_fetch_add(&offset, used, __ATOMIC_ACQ_REL);
    Handle file = 0;
    if (R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, trailPath),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        return;
    }
    u32 written = 0;
    FSFILE_Write(file, &written, at, line, used, FS_WRITE_FLUSH);
    FSFILE_Close(file);
}

const char* CtrBootTrail_last(void) {
    return lastStep;
}
