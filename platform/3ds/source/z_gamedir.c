// The game folder's names from one listing, and settings.ini's contents from one read (gamedir, smoothux).
//
// On an Old 3DS a lookup in the game folder is slow, and slower still for a name that isn't there: the FAT folder is
// read through to its end (rows utflames13: about 200 ms for a missing name's open, about 400 ms for its stat,
// 55-75 ms for a name that exists, on the test consoles). Boot looked up about 25 missing names (test switches such as
// loadshot.txt, and the game's own file_exists checks), 8-9 s in all, and opened settings.ini 16 times (about 1 s).
//
// A layer over the card's device (the one ctr_open_files tracks, so every caller is under it: stat, fopen, open, the
// game's file_exists) answers "no such file" at once for a name in the game folder that the folder's listing doesn't
// have, and calls the card for everything else. Exact:
// - The listing is made once, at the first lookup there. A listing error, a truncated read, or a name outside plain
//   printable ASCII turns the layer off for the run (every call goes to the card).
// - Only names: a path into a folder below, a relative path, or a name FAT could match another way (an 8.3 alias with
//   '~', a trailing dot or space, characters FAT refuses) is the card's to answer.
// - Our creates add their name before they return: an open that may create, a rename's new name, mkdir, link. A write
//   by a path that isn't in the plain form (it could name a file there another way) turns the layer off, as does the
//   folder itself made anew after the listing.
// - Files the lab's guard and the watchdog write directly through the FS service (crash.txt, hang.txt, labboot.txt,
//   and their .prev copies) are never answered from the listing.
// - The listing is a 32768-bit Bloom filter of the names (case folded, as FAT compares): a name it has is looked up
//   on the card, so a false match only costs the old lookup.
// - settings.ini, opened only to read, is served from one read of it while no one writes it; any write, rename or
//   removal of it drops the copy.
// - settings.ini written as newlib's fopen "w" and "a" open it, once listed: the writes go to a copy in memory, the
//   reads see it at once (the options menu's save opens the file four times, 0.2-1.2 s on the game thread in a large
//   folder, rows smuxm4), and a worker thread writes it once the writes go quiet. Never a truncated settings.ini: the
//   worker writes settings.ini.tmp, renames it settings.ini.new (whole), removes settings.ini, renames the new one in;
//   the next listing finishes a save cut short (a .new goes in, a .tmp goes). A save while one is being written is
//   written after it, the latest contents only; the exit waits for the last. Only settings.ini: every other file (the
//   game's saves) is written through at once, as before.
// Late code over the device table, installed by a wrap of CtrOpenFiles_track (main()'s first call); a wrap of
// CtrLog_openFile logs the listing's line once the log file is open. No existing object changes but those three
// calls (main.c, z_loadscreen.c, ctr_exit.c; CMakeLists.txt).
//
// Two rules for any call made here straight to the device below (gamedir2, the o3ds/o2ds crash of rows smuxm6):
// - libctru's sdmc device finds its archive through r->deviceData, which newlib's dispatch sets before every call it
//   routes. Only some of its calls (open, stat) fall back to the path when it is NULL; archive_rename reads through it
//   at once. A call made by this layer outside a dispatch (the worker, the exit) sets it first (gdSaveNow): the
//   worker's first rename read address 0x1 on every Old console. Azahar reads unmapped memory as zero, so it passed.
// - The worker never outlives the exit: the exit (atexit, which HOME -> Close also reaches by main's return) tells it
//   to stop, joins it after any write it is making, then writes what is still newer than the card on its own thread
//   (gdExit). No thread of this layer wakes after libctru unmaps the heap its stack is on.

#include <errno.h>
#include <stddef.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef GAMEDIR_HOST_TEST
// The host test defines the device table, LightLock, logInfo and the device calls first.
#else
#include <3ds.h>
#include <sys/iosupport.h>
#include <sys/syslimits.h> // NAME_MAX: readdir's name buffer (sys/dirent.h)
#include "log.h"
#include "ctr_wrap_chain.h"
#define GD_TICKS() svcGetSystemTick()
#define GD_TICKS_PER_MS 268111u
#define GD_REENT() _REENT
#endif

#ifndef GD_GAME
#define GD_GAME CTRPORT_GAME_DIR
#endif

#ifndef GD_CODE
#define GD_CODE __attribute__((noinline, section(".text.late.gamedir")))
#define GD_BSS __attribute__((section(".bss.late.gamedir")))
// Its constants: late constants (late_reserve.ld, last), but in late text on DELTARUNE Old 3DS (505 B of late constants
// were left at cand20, these are 554 B).
#if defined(ENABLE_WAD17) && defined(CTRPORT_TARGET_O3DS)
#define GD_RO __attribute__((section(".rodata.gamedir_in_text")))
#else
#define GD_RO __attribute__((section(".rodata_gamedir2_late")))
#endif
#endif

#define GD_BITS 32768u
#define GD_SETTINGS_MAX 8192u
#define GD_WRITE_MAX 65536u
#define GD_SLOTS 6
#define GD_QUIET_NS 100000000LL // the worker's wait for the writes to go quiet
#define GD_STACK 8192

void CTR_OPENFILES_AFTER_GAMEDIR(void);
void CTR_OPENFILES_GAMEDIR(void);
void __real_CtrLog_openFile(const char* path);
void __wrap_CtrLog_openFile(const char* path);

static const char gdFolder[] GD_RO = GD_GAME;          // "sdmc:/3ds/<game>-3ds"
static const char gdSettings[] GD_RO = "settings.ini";
// Retired with lab mode's borrowing of the old folder (hashfix, 2026-10-27); kept so the late constants after
// them don't move.
static const char gdOldSettings[] GD_RO __attribute__((used)) = "legacycfg.ini";
static const char gdRefused[] GD_RO = "\\:*?\"<>|";
// Written straight through the FS service (ctr_lab.c, ctr_watchdog.c, ctr_boot_trail.c), not through this layer.
static const char gdDirect[] GD_RO = "crash.txt\0hang.txt\0labboot.txt\0";
static const char gdListed[] GD_RO = "Game folder: %d names listed in %lu ms (%s); lookups of missing names answered from it\n";
static const char gdOff[] GD_RO = "Game folder: listing off for this run (%s)\n";
static const char gdWhyList[] GD_RO = "listing failed";
static const char gdWhyName[] GD_RO = "a name outside plain ASCII";
static const char gdWhyPath[] GD_RO = "a write by an unusual path";
static const char gdWhyFolder[] GD_RO = "the folder made anew";
static const char gdWhyMoved[] GD_RO __attribute__((used)) = "the old folder borrowed after the listing"; // retired
static const char gdSaved[] GD_RO = "Settings: written in %lu ms, off the game thread\n";
static const char gdNotSaved[] GD_RO = "Settings: not written (step %d, error %d)\n";
static const char gdRecovered[] GD_RO = "Settings: a write cut short finished at the listing (%d)\n";
static const char gdTmp[] GD_RO = ".tmp";
static const char gdNew[] GD_RO = ".new";
// The file as read (at boot, the first read): a row after an edit and an exit compares it with Azahar's file.
static const char gdReadLine[] GD_RO = "Settings: %lu B read (FNV-1a %08lx)\n";

typedef struct {
    struct stat st;          // the device's fstat of it
    uint32_t size, cap;
    uint32_t refs;           // opens it serves (and the worker writing it)
    char text[];
} GdCopy;

typedef struct {
    void* fd;                // the device's file state of an open served from a copy
    GdCopy* copy;
    off_t pos;
    uint8_t mode;            // 0 read, 1 write, 2 append
} GdSlot;

static struct {
    devoptab_t tab;          // this layer
    const devoptab_t* real;  // the device below (ctr_open_files' or libctru's)
    uint32_t* bits;          // the listing, once made
    LightLock lock;
    int8_t state;            // 0 not listed, 1 listed, -1 off
    uint32_t retired[17];    // lab mode's borrowed old folder, retired (hashfix, 2026-10-27); kept so nothing moves
    // settings.ini: its contents while no one writes it through, and the opens served from them
    GdCopy* copy;
    GdCopy* work;            // the contents the open "w"/"a" sessions write (the copy once the last closes)
    uint8_t writers;         // opens to write it through (no copy is made while there are any)
    uint8_t served;
    uint8_t dirty, saving;   // the copy is newer than the card; the worker is writing it
    uint8_t leftNew;         // a settings.ini.new this run left (a write stopped after its rename)
    uint32_t gen;            // copies made by sessions (the worker waits until it stops changing)
    LightLock io;            // one card write at a time
#ifndef GAMEDIR_HOST_TEST
    Thread thread;
    LightEvent wake;
    uint8_t quit;            // the exit has begun: the worker stops, a later save is written by its caller
#endif
    // what the listing found, for the log once it opens (the listing usually comes first)
    uint32_t count, ms;
    uint32_t readSize, readHash; // the last read of the file (gdFill), for the log once it opens
    const char* why;
    void* writer[2];
    GdSlot slot[GD_SLOTS];
} gd GD_BSS;

static GD_CODE int gdFold(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static GD_CODE const char* gdDir(void) { return gdFolder; }

// The name after "<sdmc:>/3ds/<game>-3ds/" (gdDir) if the path is that folder's plain form, "" if the path is plain and
// elsewhere (or the folder itself), NULL if it isn't plain: relative, "//", a part ending in a dot or space ("." and
// ".." too) or starting with a space, a trailing '/', '~', characters outside printable ASCII or that FAT refuses.
static GD_CODE const char* gdName(const char* path) {
    const char* p = path;
    if (strncmp(p, gdFolder, 5) == 0) p += 5; // "sdmc:"
    if (*p != '/') return NULL;
    const char* name = p;
    for (const char* q = p;; q++) {
        int c = (unsigned char) *q;
        if (c == '/' || c == '\0') {
            if (q != p && (q[-1] == '/' || q[-1] == '.' || q[-1] == ' ')) return NULL;
            if (c == '\0') break;
            if (q[1] == ' ') return NULL;
            name = q + 1;
        } else if (c < 0x20 || c > 0x7e || c == '~' || strchr(gdRefused, c) != NULL) {
            return NULL;
        }
    }
    const char* f = gdDir() + 5;
    size_t n = strlen(f);
    // (gdFolder's own NUL as the "": a string literal would be a mergeable constant outside the late area)
    return strncasecmp(p, f, n) == 0 && name == p + n + 1 ? name : gdFolder + sizeof(gdFolder) - 1;
}

// A plain path that is the game folder or a folder above it ("sdmc:/3ds").
static GD_CODE bool gdAbove(const char* path) {
    const char* f = gdDir();
    const char* p = strncmp(path, f, 5) == 0 ? path + 5 : path;
    size_t n = strlen(p);
    return n <= strlen(f) - 5 && strncasecmp(p, f + 5, n) == 0 && (f[5 + n] == '/' || f[5 + n] == '\0');
}

static GD_CODE uint32_t gdHash(const char* name) {
    uint32_t h = 2166136261u;
    while (*name != '\0') h = (h ^ (uint32_t) gdFold((unsigned char) *name++)) * 16777619u;
    return h;
}

// The 3 bits of a name: bit i of the result's step is set, or tested.
static GD_CODE bool gdBloom(const char* name, bool add) {
    uint32_t h = gdHash(name), step = ((h >> 17) | (h << 15)) * 0x85ebca6bu | 1u;
    bool all = true;
    for (int i = 0; i < 3; i++, h += step) {
        uint32_t bit = h & (GD_BITS - 1u);
        if (add) gd.bits[bit >> 5] |= 1u << (bit & 31u);
        else all = all && (gd.bits[bit >> 5] & (1u << (bit & 31u))) != 0;
    }
    return all;
}

static GD_CODE void gdDropSettings(void);
static GD_CODE int gdSettingsPart(const char* name);
static GD_CODE void gdRecover(struct _reent* r, int found);

// Off for the rest of the run (the caller holds the lock): every call goes to the card, settings.ini's too.
static GD_CODE void gdTurnOff(const char* why) {
    if (gd.state == -1) return;
    __atomic_store_n(&gd.state, -1, __ATOMIC_RELEASE);
    gd.why = why;
    gdDropSettings();
    logInfo(gdOff, why);
}

// The listing (the caller holds the lock). False turns the layer off.
static GD_CODE bool gdList(struct _reent* r) {
    int err = r->_errno, count = 0, found = 0;
    uint64_t t0 = GD_TICKS();
    gd.bits = calloc(GD_BITS / 32u, sizeof(uint32_t));
    DIR_ITER dir = { 0, malloc(gd.real->dirStateSize) };
    char* name = malloc(NAME_MAX + 1);
    struct stat st;
    bool ok = gd.bits != NULL && dir.dirStruct != NULL && name != NULL && gd.real->diropen_r != NULL &&
              gd.real->dirnext_r != NULL && gd.real->diropen_r(r, &dir, gdDir()) != NULL;
    const char* why = gdWhyList;
    if (ok) {
        for (;;) {
            r->_errno = 0;
            if (gd.real->dirnext_r(r, &dir, name, &st) != 0) {
                ok = r->_errno == ENOENT; // the end; anything else is an error
                break;
            }
            for (const char* c = name; *c != '\0'; c++)
                if ((unsigned char) *c < 0x20 || (unsigned char) *c > 0x7e) ok = false;
            if (!ok) {
                why = gdWhyName;
                break;
            }
            gdBloom(name, true);
            found |= 1 << gdSettingsPart(name); // bit 1 settings.ini, 2 .tmp, 3 .new
            count++;
        }
        gd.real->dirclose_r(r, &dir);
    }
    free(name);
    free(dir.dirStruct);
    r->_errno = err;
    if (!ok) {
        gdTurnOff(why);
        return false;
    }
    if ((found & 12) != 0) gdRecover(r, found);
    gd.count = (uint32_t) count;
    gd.ms = (uint32_t) ((GD_TICKS() - t0) / GD_TICKS_PER_MS);
    __atomic_store_n(&gd.state, 1, __ATOMIC_RELEASE); // the bits first
    logInfo(gdListed, count, (unsigned long) gd.ms, gdDir());
    return true;
}

// True: the name isn't in the game folder, so the card would say ENOENT.
static GD_CODE bool gdMissing(struct _reent* r, const char* name) {
    if (name == NULL || *name == '\0') return false;
    for (const char* d = gdDirect; *d != '\0'; d += strlen(d) + 1)
        if (strncasecmp(name, d, strlen(d)) == 0) return false;
    int state = __atomic_load_n(&gd.state, __ATOMIC_ACQUIRE);
    if (state == 0) {
        LightLock_Lock(&gd.lock);
        if (gd.state == 0) gdList(r);
        state = gd.state;
        LightLock_Unlock(&gd.lock);
    }
    return state == 1 && !gdBloom(name, false);
}

// A create (or a rename's new name) at this path has happened: once the folder is listed, its name joins the listing
// (before, the listing will find it on the card; a listing in progress holds the lock until it ends). A create by a
// path that isn't plain turns the layer off. So does the game folder itself, or a folder above it, made anew by a
// rename or mkdir after the listing; before it (the SD folder move on the first launch) the copy goes, if any.
static GD_CODE void gdCreated(const char* path) {
    const char* name = gdName(path);
    bool folder = name != NULL && *name == '\0';
    if (folder && !gdAbove(path)) return;
    LightLock_Lock(&gd.lock);
    if (name == NULL) gdTurnOff(gdWhyPath);
    else if (folder && gd.state == 1) gdTurnOff(gdWhyFolder);
    else if (folder) gdDropSettings();
    else if (gd.state == 1) gdBloom(name, true);
    LightLock_Unlock(&gd.lock);
}

// A write at this path is about to happen (an open to write, an unlink, a rename's old name): by an unusual path, one
// that could name a file here another way (settings.ini, say), it turns the layer off.
static GD_CODE void gdWriting(const char* path) {
    if (gdName(path) != NULL) return;
    LightLock_Lock(&gd.lock);
    gdTurnOff(gdWhyPath);
    LightLock_Unlock(&gd.lock);
}

// 1 settings.ini, 2 settings.ini.tmp, 3 settings.ini.new, else 0.
static GD_CODE int gdSettingsPart(const char* name) {
    if (name == NULL) return 0;
    const char* s = gdSettings;
    size_t n = strlen(s), m = strlen(name);
    if (strncasecmp(name, s, n) != 0) return 0;
    if (m == n) return 1;
    if (m != n + 4) return 0;
    return strcasecmp(name + n, gdTmp) == 0 ? 2 : strcasecmp(name + n, gdNew) == 0 ? 3 : 0;
}

// "<folder>/settings.ini" and a suffix (gdTmp, gdNew or ""): false if it doesn't fit.
static GD_CODE bool gdPath(char* out, size_t size, const char* suffix) {
    const char* dir = gdDir();
    const char* s = gdSettings;
    size_t a = strlen(dir), b = strlen(s), c = strlen(suffix);
    if (a + b + c + 2 > size) return false;
    memcpy(out, dir, a);
    out[a] = '/';
    memcpy(out + a + 1, s, b);
    memcpy(out + a + 1 + b, suffix, c + 1);
    return true;
}

// settings.ini's three paths: the file, its .tmp and its .new (false if they don't fit).
static GD_CODE bool gdPaths(char (*path)[96]) {
    return gdPath(path[0], sizeof(path[0]), gdFolder + sizeof(gdFolder) - 1) && gdPath(path[1], sizeof(path[1]), gdTmp) &&
           gdPath(path[2], sizeof(path[2]), gdNew);
}

// A copy is freed once it is dropped and no open it serves is left (the caller holds the lock).
static GD_CODE void gdRelease(GdCopy* copy) {
    if (copy != NULL && copy != gd.copy && copy != gd.work && copy->refs == 0) free(copy);
}

// The copy's contents are newer than the card's file (the caller holds the lock).
static GD_CODE bool gdNewer(void) {
    return gd.dirty || gd.saving || gd.work != NULL;
}

// The copy is dropped (the caller holds the lock): opens already served keep theirs. Kept while it is newer than the
// card (the worker drops it once written, if the layer is off by then).
static GD_CODE void gdDropSettings(void) {
    if (gdNewer()) return;
    GdCopy* copy = gd.copy;
    gd.copy = NULL;
    gdRelease(copy);
}

// An open served from a copy, in a free slot i (the caller holds the lock): mode 0 read, 1 write, 2 append.
static GD_CODE void gdServe(int i, void* fd, GdCopy* copy, int mode) {
    gd.slot[i].fd = fd;
    gd.slot[i].copy = copy;
    gd.slot[i].pos = 0;
    gd.slot[i].mode = (uint8_t) mode;
    copy->refs++;
    gd.served++;
}

static GD_CODE void gdDropLocked(void) {
    LightLock_Lock(&gd.lock);
    gdDropSettings();
    LightLock_Unlock(&gd.lock);
}

static GD_CODE int gdSlot(void* fd) {
    if (gd.served != 0)
        for (int i = 0; i < GD_SLOTS; i++)
            if (gd.slot[i].fd == fd) return i;
    return -1;
}

// A new copy: the text of "text" (none: empty), the fstat of "from" (none: a plain file's).
static GD_CODE GdCopy* gdNewCopy(const GdCopy* text, const GdCopy* from, uint32_t cap) {
    if (text != NULL && text->size > cap) cap = text->size;
    GdCopy* copy = malloc(sizeof(GdCopy) + cap);
    if (copy == NULL) return NULL;
    if (from != NULL) {
        copy->st = from->st;
    } else {
        memset(&copy->st, 0, sizeof(copy->st));
        copy->st.st_mode = S_IFREG;
    }
    copy->size = text != NULL ? text->size : 0;
    copy->cap = cap;
    copy->refs = 0;
    if (text != NULL) memcpy(copy->text, text->text, text->size);
    copy->st.st_size = (off_t) copy->size;
    return copy;
}

// The one read of settings.ini, through the device below, with its fstat (the caller holds the lock). 1 the copy is
// made, 0 there is no such file, -1 neither (too big, or an error: reads go to the card).
static GD_CODE int gdFill(struct _reent* r, void* fd, const char* path) {
    if (gd.real->open_r(r, fd, path, O_RDONLY, 0) == -1) return r->_errno == ENOENT ? 0 : -1;
    GdCopy* copy = malloc(sizeof(GdCopy) + GD_SETTINGS_MAX);
    ssize_t n = 1;
    if (copy != NULL) {
        copy->size = 0;
        copy->cap = GD_SETTINGS_MAX;
        copy->refs = 0;
        if (gd.real->fstat_r == NULL || gd.real->fstat_r(r, fd, &copy->st) != 0) n = -1;
        while (n > 0 && copy->size < GD_SETTINGS_MAX) {
            n = gd.real->read_r(r, fd, copy->text + copy->size, GD_SETTINGS_MAX - copy->size);
            if (n > 0) copy->size += (uint32_t) n;
        }
    }
    gd.real->close_r(r, fd);
    if (copy != NULL && n == 0) {
        gd.copy = copy;
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < copy->size; i++) h = (h ^ (uint8_t) copy->text[i]) * 16777619u;
        gd.readSize = copy->size + 1u; // (0: nothing read yet)
        gd.readHash = h;
        logInfo(gdReadLine, (unsigned long) copy->size, (unsigned long) h);
        return 1;
    }
    free(copy);
    return -1;
}

static GD_CODE void gdSaveNow(void);

// The copy written to the card if it is newer (on this thread, after any write the worker is making), before a call
// that goes to the card's own settings.ini. Not while a session writes it (on this thread, the caller's own).
static GD_CODE void gdSettle(void) {
    LightLock_Lock(&gd.lock);
    bool newer = gd.dirty || gd.saving;
    LightLock_Unlock(&gd.lock);
    if (newer) gdSaveNow();
}

static GD_CODE void gdKick(void);

// newlib's fopen "w" or "a" of settings.ini once listed: a session on the copy (0), else -2 (to the card).
static GD_CODE int gdOpenWrite(struct _reent* r, void* fd, const char* path, const char* name, int flags) {
    bool missing = gdMissing(r, name); // (the listing, if not yet made)
    int err = r->_errno, got = -2;
    LightLock_Lock(&gd.lock);
    int i = 0;
    while (i < GD_SLOTS && gd.slot[i].fd != NULL) i++;
    if (gd.state == 1 && i < GD_SLOTS && gd.writers == 0) {
        bool append = (flags & O_APPEND) != 0;
        if (gd.work == NULL) {
            int had = gd.copy != NULL ? 1 : missing ? 0 : append ? gdFill(r, fd, path) : -1;
            if (had >= 0 || !append) gd.work = gdNewCopy(append ? gd.copy : NULL, gd.copy, GD_SETTINGS_MAX);
        }
        if (gd.work != NULL) {
            if (!append) gd.work->size = 0; // O_TRUNC
            gd.work->st.st_size = (off_t) gd.work->size;
            gdServe(i, fd, gd.work, append ? 2 : 1);
            gdBloom(name, true); // created (if it wasn't there)
            got = 0;
        }
    }
    LightLock_Unlock(&gd.lock);
    r->_errno = err;
    return got;
}

static GD_CODE int gdOpen(struct _reent* r, void* fd, const char* path, int flags, int mode) {
    const char* name = gdName(path);
    if ((flags & O_CREAT) == 0 && gdMissing(r, name)) {
        r->_errno = ENOENT;
        return -1;
    }
    bool reading = (flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) == O_RDONLY;
    if (!reading) gdWriting(path);
    int part = gdSettingsPart(name);
    bool settings = part == 1;
    if (settings && reading) {
        int err = r->_errno;
        LightLock_Lock(&gd.lock);
        if (gd.copy == NULL && gd.writers == 0 && gd.state != -1 && !gdNewer()) gdFill(r, fd, path);
        if (gd.copy != NULL && gd.served < GD_SLOTS) {
            int i = 0;
            while (gd.slot[i].fd != NULL) i++;
            gdServe(i, fd, gd.copy, 0);
            LightLock_Unlock(&gd.lock);
            r->_errno = err;
            return 0;
        }
        LightLock_Unlock(&gd.lock);
        gdSettle(); // (no slot left)
        r->_errno = err;
        return gd.real->open_r(r, fd, path, flags, mode);
    }
    if (settings && (flags == (O_WRONLY | O_CREAT | O_TRUNC) || flags == (O_WRONLY | O_CREAT | O_APPEND)) &&
        gdOpenWrite(r, fd, path, name, flags) == 0)
        return 0;
    if (part != 0 && !reading) gdSettle();
    if (settings && !reading) {
        // A write through (or a create): the copy goes now, and none is made until it closes.
        LightLock_Lock(&gd.lock);
        gdDropSettings();
        gd.writers++;
        LightLock_Unlock(&gd.lock);
    }
    int got = gd.real->open_r(r, fd, path, flags, mode);
    if (settings && !reading) {
        LightLock_Lock(&gd.lock);
        if (got != -1 && gd.writer[0] == NULL) gd.writer[0] = fd;
        else if (got != -1 && gd.writer[1] == NULL) gd.writer[1] = fd;
        else if (got == -1) gd.writers--;
        // (a third open writer is never counted down: no copy again this run)
        LightLock_Unlock(&gd.lock);
    }
    if (got != -1 && (flags & O_CREAT) != 0) gdCreated(path);
    return got;
}

static GD_CODE int gdClose(struct _reent* r, void* fd) {
    LightLock_Lock(&gd.lock);
    int i = gdSlot(fd);
    if (i >= 0) {
        GdCopy* copy = gd.slot[i].copy;
        gd.slot[i].fd = NULL;
        gd.served--;
        copy->refs--;
        bool kick = copy == gd.work && copy->refs == 0;
        if (kick) {
            // the last session's close: its contents are the file's now, for every read, and the worker's to write
            GdCopy* old = gd.copy;
            gd.copy = copy;
            gd.work = NULL;
            gdRelease(old);
            gd.dirty = 1;
            __atomic_store_n(&gd.gen, gd.gen + 1u, __ATOMIC_RELEASE);
        }
        gdRelease(copy);
        LightLock_Unlock(&gd.lock);
        if (kick) gdKick();
        return 0;
    }
    for (int w = 0; w < 2; w++)
        if (gd.writer[w] == fd && fd != NULL) {
            gd.writer[w] = NULL;
            gd.writers--;
        }
    LightLock_Unlock(&gd.lock);
    return gd.real->close_r(r, fd);
}

static GD_CODE ssize_t gdRead(struct _reent* r, void* fd, char* ptr, size_t len) {
    int i = gdSlot(fd);
    if (i < 0) return gd.real->read_r(r, fd, ptr, len);
    GdSlot* s = &gd.slot[i];
    if (s->mode != 0) {
        r->_errno = EBADF; // opened to write only
        return -1;
    }
    off_t left = s->pos < (off_t) s->copy->size ? (off_t) s->copy->size - s->pos : 0;
    if ((off_t) len > left) len = (size_t) left;
    memcpy(ptr, s->copy->text + s->pos, len);
    s->pos += (off_t) len;
    return (ssize_t) len;
}

static GD_CODE ssize_t gdWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    int i = gdSlot(fd);
    if (i < 0) return gd.real->write_r(r, fd, ptr, len);
    GdSlot* s = &gd.slot[i];
    if (s->mode == 0) {
        r->_errno = EBADF; // opened to read only
        return -1;
    }
    LightLock_Lock(&gd.lock);
    GdCopy* c = s->copy;
    size_t at = s->mode == 2 ? c->size : (size_t) s->pos, end = at + len;
    if (end > c->cap) {
        // more room (only the sessions hold the work copy)
        size_t cap = c->cap;
        while (cap < end && cap < GD_WRITE_MAX) cap <<= 1;
        GdCopy* grown = cap >= end ? realloc(c, sizeof(GdCopy) + cap) : NULL;
        if (grown == NULL) {
            LightLock_Unlock(&gd.lock);
            r->_errno = ENOSPC;
            return -1;
        }
        grown->cap = (uint32_t) cap;
        for (int k = 0; k < GD_SLOTS; k++)
            if (gd.slot[k].fd != NULL && gd.slot[k].copy == c) gd.slot[k].copy = grown;
        gd.work = c = grown;
    }
    if (at > c->size) memset(c->text + c->size, 0, at - c->size);
    memcpy(c->text + at, ptr, len);
    s->pos = (off_t) end;
    if (end > c->size) c->size = (uint32_t) end;
    c->st.st_size = (off_t) c->size;
    LightLock_Unlock(&gd.lock);
    return (ssize_t) len;
}

static GD_CODE off_t gdSeek(struct _reent* r, void* fd, off_t pos, int dir) {
    int i = gdSlot(fd);
    if (i < 0) return gd.real->seek_r(r, fd, pos, dir);
    GdSlot* s = &gd.slot[i];
    off_t at = dir == SEEK_SET ? pos : dir == SEEK_CUR ? s->pos + pos : dir == SEEK_END ? (off_t) s->copy->size + pos : -1;
    if (at < 0) {
        r->_errno = EINVAL;
        return -1;
    }
    return s->pos = at;
}

static GD_CODE int gdFstat(struct _reent* r, void* fd, struct stat* st) {
    int i = gdSlot(fd);
    if (i < 0) return gd.real->fstat_r(r, fd, st);
    *st = gd.slot[i].copy->st;
    return 0;
}

static GD_CODE int gdStat(struct _reent* r, const char* path, struct stat* st) {
    const char* name = gdName(path);
    if (gdMissing(r, name)) {
        r->_errno = ENOENT;
        return -1;
    }
    if (gdSettingsPart(name) == 1) {
        // newer in memory: the copy's (a session's, if it made the file)
        LightLock_Lock(&gd.lock);
        GdCopy* c = !gdNewer() ? NULL : gd.copy != NULL ? gd.copy : gd.work;
        if (c != NULL) *st = c->st;
        LightLock_Unlock(&gd.lock);
        if (c != NULL) return 0;
    }
    return gd.real->stat_r(r, path, st);
}

static GD_CODE int gdRename(struct _reent* r, const char* from, const char* to) {
    gdWriting(from);
    bool settings = gdSettingsPart(gdName(from)) != 0 || gdSettingsPart(gdName(to)) != 0;
    if (settings) gdSettle();
    int got = gd.real->rename_r(r, from, to);
    if (settings) gdDropLocked();
    if (got == 0) gdCreated(to);
    return got;
}

static GD_CODE int gdUnlink(struct _reent* r, const char* path) {
    gdWriting(path);
    if (gdSettingsPart(gdName(path)) != 0) {
        gdSettle();
        gdDropLocked();
    }
    return gd.real->unlink_r(r, path);
}

static GD_CODE int gdMkdir(struct _reent* r, const char* path, int mode) {
    int got = gd.real->mkdir_r(r, path, mode);
    if (got == 0) gdCreated(path);
    return got;
}

static GD_CODE int gdLink(struct _reent* r, const char* existing, const char* link) {
    int got = gd.real->link_r(r, existing, link);
    if (got == 0) gdCreated(link);
    return got;
}

// ===[ The card's settings.ini from the copy: settings.ini.tmp written whole, renamed .new, the old one removed, .new
// renamed in; any step cut short leaves a settings.ini whole (the old or the new) or a .new the listing puts in ]===

// The steps through the device below; 0 done, else the step that failed.
static GD_CODE int gdWriteOut(struct _reent* r, const GdCopy* c) {
    char path[3][96], *ini = path[0], *tmp = path[1], *neu = path[2];
    if (!gdPaths(path)) return 1;
    void* fd = malloc(gd.real->structSize);
    if (fd == NULL) return 1;
    int step = 1;
    if (gd.real->open_r(r, fd, tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666) != -1) {
        gdCreated(tmp);
        step = 0;
        for (uint32_t at = 0; at < c->size && step == 0;) {
            ssize_t n = gd.real->write_r(r, fd, c->text + at, c->size - at);
            if (n <= 0) step = 2;
            else at += (uint32_t) n;
        }
        if (gd.real->close_r(r, fd) != 0 && step == 0) step = 2;
    }
    free(fd);
    if (step != 0) return step;
    if (gd.leftNew) gd.real->unlink_r(r, neu);
    if (gd.real->rename_r(r, tmp, neu) != 0) return 3;
    gd.leftNew = 1;
    gdCreated(neu);
    if (gd.real->unlink_r(r, ini) != 0 && r->_errno != ENOENT) return 4;
    if (gd.real->rename_r(r, neu, ini) != 0) return 5;
    gd.leftNew = 0;
    gdCreated(ini);
    return 0;
}

// Writes the copy while it is newer than the card, the latest each time (the worker; the exit, and a call that needs
// the card's own file, on their thread after any write the worker is making).
static GD_CODE void gdSaveNow(void) {
    if (gd.real == NULL) return;
    struct _reent* r = GD_REENT();
    int err = r->_errno;
    // Outside newlib's dispatch (the worker, the exit) r->deviceData is whatever this thread's last dispatched call
    // left there (NULL on the worker, romfs's after a romfs read): libctru's archive_rename reads through it.
    void* dev = r->deviceData;
    r->deviceData = gd.real->deviceData;
    LightLock_Lock(&gd.io);
    for (;;) {
        LightLock_Lock(&gd.lock);
        GdCopy* c = gd.dirty ? gd.copy : NULL;
        if (c != NULL) {
            c->refs++;
            gd.dirty = 0;
            gd.saving = 1;
        } else {
            gd.saving = 0;
            if (gd.state == -1) gdDropSettings();
        }
        LightLock_Unlock(&gd.lock);
        if (c == NULL) break;
        uint64_t t0 = GD_TICKS();
        r->_errno = 0;
        int step = gdWriteOut(r, c);
        if (step == 0) logInfo(gdSaved, (unsigned long) ((GD_TICKS() - t0) / GD_TICKS_PER_MS));
        else logInfo(gdNotSaved, step, r->_errno);
        LightLock_Lock(&gd.lock);
        c->refs--;
        gdRelease(c);
        LightLock_Unlock(&gd.lock);
    }
    LightLock_Unlock(&gd.io);
    r->deviceData = dev;
    r->_errno = err;
}

// A write cut short, found by the listing (the caller holds the lock): a .tmp may be partial, so it goes; a .new is
// whole, so it goes in, over settings.ini if that is still there.
static GD_CODE void gdRecover(struct _reent* r, int found) {
    char path[3][96], *ini = path[0], *tmp = path[1], *neu = path[2];
    if (!gdPaths(path)) return;
    int err = r->_errno;
    if ((found & 4) != 0) gd.real->unlink_r(r, tmp);
    if ((found & 8) != 0) {
        if ((found & 2) != 0) gd.real->unlink_r(r, ini);
        gd.real->rename_r(r, neu, ini);
        gdBloom(strrchr(ini, '/') + 1, true); // listed before it was there
    }
    r->_errno = err;
    logInfo(gdRecovered, found);
}

#ifdef GAMEDIR_HOST_TEST
static GD_CODE void gdKick(void) { GD_KICK(); }
static GD_CODE void gdExit(void) { gdSaveNow(); }
#else
// The worker: woken by a session's last close, it waits for the writes to go quiet (the menu's save closes and
// reopens the file four times), then writes the latest copy. The exit's signal (gd.quit) ends any wait at once; a
// write already under way finishes first (the exit joins the thread).
static GD_CODE void gdWorker(void* arg) {
    (void) arg;
    for (;;) {
        LightEvent_Wait(&gd.wake);
        uint32_t gen;
        do {
            if (__atomic_load_n(&gd.quit, __ATOMIC_ACQUIRE)) return;
            gen = __atomic_load_n(&gd.gen, __ATOMIC_ACQUIRE);
            LightEvent_WaitTimeout(&gd.wake, GD_QUIET_NS); // a later close, or the exit, ends it early
        } while (gen != __atomic_load_n(&gd.gen, __ATOMIC_ACQUIRE) || __atomic_load_n(&gd.work, __ATOMIC_ACQUIRE) != NULL ||
                 __atomic_load_n(&gd.quit, __ATOMIC_ACQUIRE));
        gdSaveNow();
    }
}

static GD_CODE void gdKick(void) {
    if (__atomic_load_n(&gd.quit, __ATOMIC_ACQUIRE)) {
        gdSaveNow(); // the exit has begun: written here, now
        return;
    }
    if (gd.thread == NULL) {
        // below the caller's priority, on its core: it runs while the game thread waits (for the frame, say). Not
        // detached: the exit joins it.
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        LightEvent_Init(&gd.wake, RESET_ONESHOT);
        gd.thread = threadCreate(gdWorker, NULL, GD_STACK, prio < 0x3F ? prio + 1 : prio, -2, false);
        if (gd.thread == NULL) {
            gdSaveNow(); // no worker: written here, as before
            return;
        }
    }
    LightEvent_Signal(&gd.wake);
}

// The exit (atexit; main's return after HOME -> Close, the menu's quit or SELECT+START, and exit() after an error
// screen all come here; lab mode's crash path leaves without it and keeps the card's whole old file): the worker stops
// after any write it is making, then whatever is still newer than the card is written here, before libctru's exit
// closes the FS session and unmaps the heap the worker's stack is on.
static GD_CODE void gdExit(void) {
    __atomic_store_n(&gd.quit, 1, __ATOMIC_RELEASE);
    Thread t = gd.thread;
    if (t != NULL) {
        if (t == threadGetCurrent()) return; // (never: the worker doesn't exit; a fault there leaves by lab's path)
        LightEvent_Signal(&gd.wake);
        threadJoin(t, U64_MAX);
    }
    gdSaveNow();
}
#endif

// The log file opens (ctr_exit.c): lines logged before it are lost, so the listing's line, or why it's off, again.
GD_CODE void __wrap_CtrLog_openFile(const char* path) {
    __real_CtrLog_openFile(path);
    if (gd.state == 1) logInfo(gdListed, (int) gd.count, (unsigned long) gd.ms, gdDir());
    else if (gd.state == -1) logInfo(gdOff, gd.why);
    if (gd.readSize != 0) logInfo(gdReadLine, (unsigned long) (gd.readSize - 1u), (unsigned long) gd.readHash);
}

// This layer's calls over the device's, each where the device has one; the first 7 it must have (and no symlink_r).
#define GD_CALL(f) ((uint8_t) (offsetof(devoptab_t, f) / sizeof(void*)))
static const uint8_t gdCallAt[11] GD_RO = { GD_CALL(open_r),   GD_CALL(close_r),  GD_CALL(read_r),  GD_CALL(stat_r),
                                            GD_CALL(rename_r), GD_CALL(unlink_r), GD_CALL(mkdir_r), GD_CALL(write_r),
                                            GD_CALL(seek_r),   GD_CALL(fstat_r),  GD_CALL(link_r) };
static const void* const gdCalls[11] GD_RO = { gdOpen,   gdClose,  gdRead,  gdStat,  gdRename, gdUnlink,
                                               gdMkdir,  gdWrite,  gdSeek,  gdFstat, gdLink };

GD_CODE void CTR_OPENFILES_GAMEDIR(void) {
    CTR_OPENFILES_AFTER_GAMEDIR();
    const devoptab_t* real = GetDeviceOpTab(gdFolder);
    if (real == NULL || gd.real != NULL || real->symlink_r != NULL) return;
    gd.tab = *real;
    for (int k = 0; k < 11; k++) {
        const void** at = (const void**) &gd.tab + gdCallAt[k];
        if (*at != NULL) *at = gdCalls[k];
        else if (k < 7) return;
    }
    LightLock_Init(&gd.lock);
    LightLock_Init(&gd.io);
    gd.real = real;
    // Replaces the device of the same name in place.
    if (AddDevice(&gd.tab) < 0) gd.real = NULL;
    else atexit(gdExit); // the exit waits for the last write
}
