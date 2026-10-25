// The SD folder move, on the first launch after builds of 2026-10-25 renamed the game's folder.
//
// Builds before then kept everything (the game's data, saves, settings, caches and logs) in the old folder, ctrSdLegacyDir/<game>.
// Each game now has its own folder, CTRPORT_GAME_DIR (sdmc:/3ds/undertale-3ds or sdmc:/3ds/deltarune-3ds), and the
// settings file is settings.ini. CtrSdMigrate_run runs once, before anything reads the game folder (the loading
// screen's C3D_Init wrap, z_loadscreen.c, calls it before its first settings.ini read; main() calls it again before it
// makes the game folder, then a no-op):
//   no old folder              nothing to do (every launch after the first one).
//   old folder, no new folder  one rename moves the whole folder; the card is one FAT volume, so nothing is copied.
//   both folders               (an older build was started after the move, or the new folder was made by hand) every
//                              entry the new folder lacks moves over; an entry it already has stays in the old folder,
//                              so nothing is overwritten or deleted.
// The old settings file (smOldIni) becomes settings.ini unless settings.ini is already there. The old folder and its parent are removed
// only when they are empty (another game's folder keeps the parent). One line in migrate.txt in the new folder says
// what happened; the log file isn't open yet at this point.
//
// Lab mode (--lab or --lab-host, ctr_lab.h) never moves anything: a test console also runs builds from before the
// rename, which read only the old folder. There, when the old folder exists and the new one holds nothing but lab
// mode's own files (labboot.txt, crash.txt and their .prev copies, which lab mode writes before this runs), the build
// reads and writes the old folder in place: every path under the new folder is turned into the same path under the
// old one, for stdio (the sdmc device's table) and for direct FS calls (fsMakePath, wrapped at link time). Lab mode's
// own files stay in the new folder, where the lab guard already opened them and where it tells the lab host to look;
// settings.ini is read and written as the old folder's settings file (smOldIni), so both builds of an A/B use the same settings.
// A later launch outside lab mode moves the old folder as above, taking the new folder's lab files along.
//
// Everything here is late code and constants (late_reserve.ld), so nothing linked before it moves.

#include <3ds/types.h>
#include <3ds/services/fs.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#define SM_LATE __attribute__((noinline, section(".text.late.sdmigrate")))
#define SM_CONST __attribute__((aligned(4), section(".rodata.sdmigrate_in_text")))
#define SM_BSS __attribute__((section(".bss.late.sdmigrate")))
#define SM_PATH 256
#define SM_NAME 64
// Moving entries out of a folder while reading it can make the read skip some, so the merge reads it again until a
// pass moves nothing; this only bounds a card that keeps failing.
#define SM_PASSES 64
// Rewritten paths handed to fsMakePath live here until the FS call that takes them: a call can take two paths
// (FSUSER_RenameFile), and the lab guard, the watchdog and the game thread can be in one at the same time.
#define SM_RING 8

int CtrSdMigrate_run(const char* legacyParent, int mode);
bool CtrLab_active(void);
FS_Path __real_fsMakePath(FS_PathType type, const void* path);
FS_Path __wrap_fsMakePath(FS_PathType type, const void* path);

#ifdef CTRPORT_SDMIGRATE_TEST_LAB
// Test builds only: Azahar can't pass launch arguments, so this takes lab mode's path without lab mode.
#define smLabMode() true
#else
#define smLabMode() CtrLab_active()
#endif

// The parent folder of every build before the move (main.c passes it: it used to make this folder).
const char ctrSdLegacyDir[] SM_CONST = "sdmc:/3ds/deltatale";
static const char smGameDir[] SM_CONST = CTRPORT_GAME_DIR;
static const char smSuffix[] SM_CONST = "-3ds";
static const char smOldIni[] SM_CONST = "deltatale.ini";
static const char smNewIni[] SM_CONST = "settings.ini";
static const char smLogName[] SM_CONST = "migrate.txt";
static const char smAppend[] SM_CONST = "a";
static const char smReadMode[] SM_CONST = "r";
static const char smWriteMode[] SM_CONST = "w";
// The data.win version cache (ctr_data_version.c): "<revision> <size> <mtime> <digest>", then the data.win path.
static const char smCacheName[] SM_CONST = "cache/data-version.txt";
static const char smCacheFormat[] SM_CONST = "%s%s%s";
static const char smDeviceColon[] SM_CONST = "sdmc:";
static const char smMoved[] SM_CONST = "Moved %s to %s.\n";
static const char smMerged[] SM_CONST = "Merged %s into %s: %d moved, %d already here and left in the old folder.\n";
static const char smMergedAll[] SM_CONST = "Merged %s into %s: %d moved; the old folder is gone.\n";
// Lab mode's own files (main.c: BOOT_TRAIL_PATH, CRASH_REPORT_PATH; ctr_boot_trail.c and ctr_lab.c add ".prev").
static const char smLab0[] SM_CONST = "labboot.txt";
static const char smLab1[] SM_CONST = "labboot.txt.prev";
static const char smLab2[] SM_CONST = "crash.txt";
static const char smLab3[] SM_CONST = "crash.txt.prev";
static const char* const smLabFiles[4] SM_CONST = { smLab0, smLab1, smLab2, smLab3 };

static char smLegacy[SM_PATH] SM_BSS;            // ctrSdLegacyDir/<game>
static volatile bool smBorrowing SM_BSS;         // lab mode reads the old folder in place
static devoptab_t smTable SM_BSS;                // the sdmc device's table, with its path functions wrapped
static const devoptab_t* smSdmc SM_BSS;          // the table it replaced
static char smRingBuf[SM_RING][SM_PATH] SM_BSS;
static unsigned smRingNext SM_BSS;

static SM_LATE bool smIsDir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static SM_LATE bool smExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

// out = dir "/" name, if it fits.
static SM_LATE bool smJoin(char* out, const char* dir, const char* name) {
    size_t a = strlen(dir), b = strlen(name);
    if (a + 1 + b + 1 > SM_PATH) return false;
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, name, b + 1);
    return true;
}

// The game's old folder: the new folder's name without "-3ds", under the old parent.
static SM_LATE bool smLegacyGameDir(char* out, const char* legacyParent) {
    const char* base = smGameDir;
    for (const char* p = smGameDir; *p != '\0'; p++) {
        if (*p == '/') base = p + 1;
    }
    size_t length = strlen(base), suffix = strlen(smSuffix);
    if (length <= suffix || length - suffix >= SM_NAME || strcmp(base + length - suffix, smSuffix) != 0) return false;
    char name[SM_NAME];
    memcpy(name, base, length - suffix);
    name[length - suffix] = '\0';
    return smJoin(out, legacyParent, name);
}

static SM_LATE bool smIsLabFile(const char* name) {
    for (int i = 0; i < 4; i++) {
        if (strcmp(name, smLabFiles[i]) == 0) return true;
    }
    return false;
}

// True when the folder is missing, empty, or holds only lab mode's own files.
static SM_LATE bool smOnlyLabFiles(const char* path) {
    DIR* dir = opendir(path);
    if (dir == NULL) return true;
    bool only = true;
    struct dirent* entry;
    while (only && (entry = readdir(dir)) != NULL) {
        const char* name = entry->d_name;
        if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        only = smIsLabFile(name);
    }
    closedir(dir);
    return only;
}

// A path under the new folder ("sdmc:/3ds/undertale-3ds/..." or "/3ds/undertale-3ds/...") as the same path under the
// old one, in buf (settings.ini as smOldIni); any other path, and lab mode's own files, as they are.
static SM_LATE const char* smRewrite(const char* path, char* buf) {
    if (!smBorrowing || path == NULL) return path;
    size_t skip = strncmp(path, smDeviceColon, 5) == 0 ? 5 : 0;
    const char* newRel = smGameDir + 5;
    const char* oldRel = smLegacy + 5;
    size_t newLength = strlen(newRel), oldLength = strlen(oldRel);
    const char* rest = path + skip + newLength;
    if (strncmp(path + skip, newRel, newLength) != 0 || (rest[0] != '/' && rest[0] != '\0')) return path;
    if (rest[0] == '/' && smIsLabFile(rest + 1)) return path;
    // The settings file keeps its old name there, so builds from before and after the rename read the same one.
    bool ini = rest[0] == '/' && strcmp(rest + 1, smNewIni) == 0;
    const char* tail = ini ? smOldIni : rest;
    size_t tailLength = strlen(tail);
    if (skip + oldLength + 1 + tailLength + 1 > SM_PATH) return path;
    memcpy(buf, path, skip);
    memcpy(buf + skip, oldRel, oldLength);
    char* at = buf + skip + oldLength;
    if (ini) *at++ = '/';
    memcpy(at, tail, tailLength + 1);
    return buf;
}

SM_LATE FS_Path __wrap_fsMakePath(FS_PathType type, const void* path) {
    if (smBorrowing && type == PATH_ASCII) {
        unsigned slot = __atomic_fetch_add(&smRingNext, 1, __ATOMIC_RELAXED) % SM_RING;
        path = smRewrite((const char*) path, smRingBuf[slot]);
    }
    return __real_fsMakePath(type, path);
}

static SM_LATE int smOpen(struct _reent* r, void* file, const char* path, int flags, int mode) {
    char buf[SM_PATH];
    return smSdmc->open_r(r, file, smRewrite(path, buf), flags, mode);
}

static SM_LATE int smStat(struct _reent* r, const char* path, struct stat* st) {
    char buf[SM_PATH];
    return smSdmc->stat_r(r, smRewrite(path, buf), st);
}

static SM_LATE int smUnlink(struct _reent* r, const char* path) {
    char buf[SM_PATH];
    return smSdmc->unlink_r(r, smRewrite(path, buf));
}

static SM_LATE int smRmdir(struct _reent* r, const char* path) {
    char buf[SM_PATH];
    return smSdmc->rmdir_r(r, smRewrite(path, buf));
}

static SM_LATE int smMkdir(struct _reent* r, const char* path, int mode) {
    char buf[SM_PATH];
    return smSdmc->mkdir_r(r, smRewrite(path, buf), mode);
}

static SM_LATE int smRename(struct _reent* r, const char* from, const char* to) {
    char buf[SM_PATH], buf2[SM_PATH];
    return smSdmc->rename_r(r, smRewrite(from, buf), smRewrite(to, buf2));
}

static SM_LATE DIR_ITER* smDiropen(struct _reent* r, DIR_ITER* state, const char* path) {
    char buf[SM_PATH];
    return smSdmc->diropen_r(r, state, smRewrite(path, buf));
}

// From here on, paths under the new folder go to the old one (lab mode only; see the top of this file). The game's
// paths only reach these entries (access() is a stat); chdir, chmod, utimes and statvfs keep the device's own.
static SM_LATE void smBorrow(void) {
    int device = FindDevice(smDeviceColon);
    const devoptab_t* sdmc = device >= 0 ? devoptab_list[device] : NULL;
    if (sdmc == NULL) return;
    smSdmc = sdmc;
    smTable = *sdmc;
    if (sdmc->open_r) smTable.open_r = smOpen;
    if (sdmc->stat_r) smTable.stat_r = smStat;
    if (sdmc->unlink_r) smTable.unlink_r = smUnlink;
    if (sdmc->rmdir_r) smTable.rmdir_r = smRmdir;
    if (sdmc->mkdir_r) smTable.mkdir_r = smMkdir;
    if (sdmc->rename_r) smTable.rename_r = smRename;
    if (sdmc->diropen_r) smTable.diropen_r = smDiropen;
    smBorrowing = true;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    devoptab_list[device] = &smTable;
}

// Moves each entry of legacy that the game folder lacks; returns how many moved, and in *left how many stayed.
static SM_LATE int smMerge(const char* legacy, int* left) {
    static char from[SM_PATH] SM_BSS;
    static char to[SM_PATH] SM_BSS;
    int moved = 0;
    *left = 0;
    for (int pass = 0; pass < SM_PASSES; pass++) {
        DIR* dir = opendir(legacy);
        if (dir == NULL) break;
        int movedNow = 0, leftNow = 0;
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            const char* name = entry->d_name;
            if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
            const char* target = strcmp(name, smOldIni) == 0 ? smNewIni : name;
            if (smJoin(from, legacy, name) && smJoin(to, smGameDir, target) && !smExists(to) && rename(from, to) == 0) {
                movedNow++;
            } else {
                leftNow++;
            }
        }
        closedir(dir);
        moved += movedNow;
        *left = leftNow;
        if (movedNow == 0) break;
    }
    return moved;
}

// A moved data-version cache in dir (the game folder, or one of its folders: DELTARUNE's chapters) still names the
// data.win by its old path, which the cache must match: point it at the new folder, or the next start hashes the
// whole data.win again. A failed write only costs that hash (the cache is checked line by line).
static SM_LATE void smFixCache(const char* dir) {
    static char file[SM_PATH] SM_BSS;
    static char head[128] SM_BSS;
    static char saved[SM_PATH] SM_BSS;
    size_t n = strlen(smLegacy);
    if (!smJoin(file, dir, smCacheName)) return;
    FILE* f = fopen(file, smReadMode);
    if (f == NULL) return;
    bool ok = fgets(head, sizeof(head), f) != NULL && fgets(saved, sizeof(saved), f) != NULL;
    fclose(f);
    if (!ok || strncmp(saved, smLegacy, n) != 0 || saved[n] != '/') return;
    f = fopen(file, smWriteMode);
    if (f == NULL) return;
    fprintf(f, smCacheFormat, head, smGameDir, saved + n);
    fclose(f);
}

static SM_LATE void smFixCaches(void) {
    static char sub[SM_PATH] SM_BSS;
    smFixCache(smGameDir);
    DIR* dir = opendir(smGameDir);
    if (dir == NULL) return;
    for (struct dirent* e = readdir(dir); e != NULL; e = readdir(dir)) {
        if (e->d_name[0] != '.' && smJoin(sub, smGameDir, e->d_name)) smFixCache(sub);
    }
    closedir(dir);
}

// 0: no old folder; 1: the whole folder moved; 2: merged into an existing game folder (also when nothing more could
// move: entries the game folder already has stay in the old one, which then costs one read of it per launch);
// 3: lab mode reads the old folder in place.
SM_LATE int CtrSdMigrate_run(const char* legacyParent, int mode) {
    static char path[SM_PATH] SM_BSS;
    static char path2[SM_PATH] SM_BSS;
    static bool done SM_BSS;
    const char* legacy = smLegacy;
    // Once per launch: the loading screen's C3D_Init wrap calls it before anything reads settings.ini, main() after.
    if (done) return 0;
    done = true;
    if (!smLegacyGameDir(smLegacy, legacyParent) || !smIsDir(legacy)) return 0;
    if (smLabMode()) {
        if (!smOnlyLabFiles(smGameDir)) return 0;
        smBorrow();
        return smBorrowing ? 3 : 0;
    }
    // A new folder that only lab mode made: its files go to the old folder (replacing older copies there), so the
    // whole folder can move in one rename.
    if (smIsDir(smGameDir) && smOnlyLabFiles(smGameDir)) {
        for (int i = 0; i < 4; i++) {
            if (smJoin(path, smGameDir, smLabFiles[i]) && smJoin(path2, legacy, smLabFiles[i]) && smExists(path)) {
                remove(path2);
                rename(path, path2);
            }
        }
        rmdir(smGameDir);
    }
    int result, moved = 0, left = 0;
    if (!smExists(smGameDir) && rename(legacy, smGameDir) == 0) {
        result = 1;
        if (smJoin(path, smGameDir, smOldIni) && smJoin(path2, smGameDir, smNewIni) && smExists(path) &&
            !smExists(path2)) {
            rename(path, path2);
        }
    } else {
        result = 2;
        mkdir(smGameDir, mode);
        moved = smMerge(legacy, &left);
        if (left == 0) rmdir(legacy);
    }
    rmdir(legacyParent); // only if no other game's folder (or anything else) is left in it
    if (result == 1 || moved > 0) smFixCaches();
    // A merge that moved nothing (entries left from an earlier merge) runs again at every launch: no line for it.
    if ((result == 1 || moved > 0) && smJoin(path, smGameDir, smLogName)) {
        FILE* f = fopen(path, smAppend);
        if (f != NULL) {
            if (result == 1) {
                fprintf(f, smMoved, legacy, smGameDir);
            } else if (left == 0) {
                fprintf(f, smMergedAll, legacy, smGameDir, moved);
            } else {
                fprintf(f, smMerged, legacy, smGameDir, moved, left);
            }
            fclose(f);
        }
    }
    return result;
}
