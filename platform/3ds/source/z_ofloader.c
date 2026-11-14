// ofloader (change omegaflowey, GO 11-02; UNDERTALE only): the audio loader thread keeps the few files it reads
// sounds from (data.win, adpcm-sfx.dtsp) open, instead of closing one and opening the other each time a request's
// file differs from the last. On the SD card an open is a lookup in the game folder's directory, and the console's FS
// serves it ahead of a read the game thread issued at the same moment: on the o2DS (11-01 on) each such reopen of
// adpcm-sfx.dtsp took 64 ms and held the game thread's read for as long (docs/3ds-hardware.md, "o2DS card, 11-01").
// ctr_audio_system.c's loader (loaderThreadMain) and flowend's decode-time reads (__wrap_CtrDecoder_read) call these
// in place of open and close (ctr_audio_system.h's CTR_LOADER_* names), so their code keeps its size and only the
// call targets change. Everything here runs on the loader thread.
//
// The files are a kept-open cache (ctr_open_files.h): it keeps a file only while CtrOpenFiles_mayKeep, and an open
// elsewhere that finds the card's files crowded has it close them (the shed runs on this thread: at its next open, or
// once its wait for work returns, which the shed's wake signals). The file the loader holds between requests
// (loaderThreadMain's own fd, the newest it opened) stays open through a shed, as it did before this cache: closing
// it would leave the loader reading a closed descriptor. Reads go through each caller's lseek and read, as before,
// so every sound's bytes are the same.
#include <3ds.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "ctr_audio_system.h"
#include "ctr_open_files.h"
#include "log.h"

// dofl (change titancpu, GO 11-03, docs/perf/sd-open-design.md B1): DELTARUNE runs the same code. Its loader reads
// sounds from data.win and the chapter's audiogroup in turn, and reopened each about 7-10 times a Flowery minute
// (~32 ms an open on the console, a main-thread read queued behind it). Its code, constants and statics go in their
// own sections, appended after flowerhw stfix (late_reserve.ld), so nothing before them moves; UNDERTALE's stay where they
// were. A DELTARUNE chapter switch stops the audio system's loader thread and the next chapter starts a new one
// (stopLoaderThread, startLoaderThread), which UNDERTALE never does: see CtrOfLoader_join and olAdopt.
#if defined(__3DS__) && !defined(ENABLE_WAD16)
#define OL_CODE __attribute__((noinline, section(".titancpu_dofl")))
#define OL_RO __attribute__((aligned(4), section(".titancpu_dofl_ro")))
#define OL_BSS __attribute__((section(".bss_titancpu_dofl,\"aw\",%nobits @")))
#define OL_ADOPT 1
#endif
#if defined(ENABLE_WAD16) && defined(__3DS__)
#define OL_CODE __attribute__((noinline, section(".omegaflowey_ofloader")))
#define OL_RO __attribute__((aligned(4), section(".omegaflowey_ofloader_ro")))
// Zero-initialised, outside .bss.* so the main .bss line leaves it to late_reserve.ld's (flags spelled out, as
// bubbleclip's: '@' starts the assembler's comment, ending GCC's own).
#define OL_BSS __attribute__((section(".bss_omegaflowey_ofloader,\"aw\",%nobits @")))
#endif
#if defined(__3DS__)
#define OL_FILES 3
#define OL_PATH 192 // a longer path is opened and closed as before

typedef struct {
    int fd; // -1: free
    uint32_t used; // the request count when last returned (the oldest is replaced first)
    char path[OL_PATH];
} OlFile;
static struct {
    OlFile files[OL_FILES];
    LightEvent* event; // the loader's wait-for-work event while it waits on it (the shed's wake), else NULL
    int keeper;        // CtrOpenFiles keeper id + 1 (0: not registered)
    int held;          // the fd loaderThreadMain holds (never shed), or -1
    uint32_t requests, opens, sheds, hits; // hits: requests served by a kept file (each an open saved)
    bool ready;
#ifdef OL_ADOPT
    u32 tid; // the kernel thread id of the loader these files belong to (ids are never reused)
#endif
} ol OL_BSS;

static const char olLogOpen[] OL_RO = "[ofloader] keeps %s open (%lu opens, %lu requests, %lu sheds)\n";
static const char olLogShed[] OL_RO = "[ofloader] shed: closed %d kept-open files (%lu sheds)\n";
static const char olLogHits[] OL_RO = "[ofloader] %lu opens saved: %lu requests, %lu kept opens, %lu sheds\n";
#ifdef OL_ADOPT
static const char olLogRetire[] OL_RO = "[ofloader] retired %d\n";
#endif

static OL_CODE void olShed(void* owner) {
    (void) owner;
    int closed = 0;
    for (int i = 0; OL_FILES > i; i++) {
        OlFile* f = &ol.files[i];
        if (0 > f->fd || f->fd == ol.held) continue;
        close(f->fd);
        f->fd = -1;
        closed++;
    }
    ol.sheds++;
    logInfo(olLogShed, closed, (unsigned long) ol.sheds);
}

// Called under the open-files lock by the thread that asks for the shed.
static OL_CODE void olWake(void* owner) {
    (void) owner;
    LightEvent* event = __atomic_load_n(&ol.event, __ATOMIC_SEQ_CST);
    if (event != NULL) LightEvent_Signal(event);
}

static OL_CODE void olInit(void) {
    ol.ready = true;
    ol.held = -1;
    for (int i = 0; OL_FILES > i; i++) ol.files[i].fd = -1;
}

#ifdef OL_ADOPT
// DELTARUNE (c26rev R150 F1): retires an ended loader's cache, on a thread that knows the loader is gone (its join, or
// the next loader). The wake has no event; the keeper is removed under the open-files lock, so no shed or wake is asked
// of it again and a later thread given the old Thread's allocation never matches it. Every kept file is closed except
// the one the loader held: its own last close (loaderThreadMain's end, newlib's close) ended that descriptor already,
// whether or not it was a kept file, and its number may belong to another file by now, so it is dropped, never closed
// again (-1 when it held none). The cache is left empty: the next loader starts afresh.
static OL_CODE void olRetire(void) {
    __atomic_store_n(&ol.event, NULL, __ATOMIC_SEQ_CST);
    if (ol.keeper != 0) CtrOpenFiles_removeKeeper(ol.keeper - 1);
    ol.keeper = 0;
    int closed = 0;
    for (int i = 0; OL_FILES > i; i++) {
        OlFile* f = &ol.files[i];
        if (0 <= f->fd && f->fd != ol.held) {
            close(f->fd);
            closed++;
        }
        f->fd = -1;
    }
    ol.held = -1;
    ol.ready = false;
    logInfo(olLogRetire, closed);
}

// DELTARUNE: the first call from a new loader thread (the next chapter's audio system) takes over. The old loader's
// cache was retired at its join (CtrOfLoader_join), so this only starts afresh; one still here (a join that failed)
// is retired now. The thread is told apart by its kernel id, not by its Thread pointer, which the next thread's
// allocation can reuse.
static OL_CODE void olAdopt(void) {
    u32 tid = 0;
    svcGetThreadId(&tid, CUR_THREAD_HANDLE);
    if (ol.ready && ol.tid == tid) return;
    if (ol.ready) olRetire();
    olInit();
    ol.tid = tid;
}
#endif

// The file's descriptor: kept open from an earlier request, or opened now (and kept while the card has room).
static OL_CODE int olOpen(const char* path, int flags, bool hold) {
#ifdef OL_ADOPT
    olAdopt();
#endif
    if (!ol.ready) olInit();
    if (ol.keeper == 0) ol.keeper = CtrOpenFiles_addKeeper(olShed, olWake, NULL) + 1;
    CtrOpenFiles_poll(); // a shed asked for since the last request (no kept file is in use here)
    ol.requests++;
    size_t n = path != NULL ? strlen(path) : OL_PATH;
    OlFile* slot = NULL;
    for (int i = 0; OL_PATH > n && OL_FILES > i; i++) {
        OlFile* f = &ol.files[i];
        if (0 <= f->fd && strcmp(f->path, path) == 0) {
            f->used = ol.requests;
            if (hold) ol.held = f->fd;
            ol.hits++;
            if ((ol.hits & (ol.hits - 1)) == 0) // the proof count, at 1, 2, 4, 8, ... opens saved
                logInfo(olLogHits, (unsigned long) ol.hits, (unsigned long) ol.requests, (unsigned long) ol.opens,
                        (unsigned long) ol.sheds);
            return f->fd;
        }
        if (0 > f->fd) {
            if (slot == NULL || 0 <= slot->fd) slot = f;
        } else if (f->fd != ol.held && (slot == NULL || (0 <= slot->fd && slot->used > f->used))) {
            slot = f;
        }
    }
    int fd = open(path, flags);
    if (hold) ol.held = fd;
    if (0 > fd || slot == NULL || OL_PATH <= n || ol.keeper == 0 || !CtrOpenFiles_mayKeep()) return fd;
    if (0 <= slot->fd) close(slot->fd); // the oldest kept file the loader doesn't hold
    slot->fd = fd;
    slot->used = ol.requests;
    memcpy(slot->path, path, n + 1);
    ol.opens++;
    logInfo(olLogOpen, path, (unsigned long) ol.opens, (unsigned long) ol.requests, (unsigned long) ol.sheds);
    return fd;
}

static OL_CODE bool olKept(int fd) {
    for (int i = 0; 0 <= fd && OL_FILES > i; i++)
        if (ol.files[i].fd == fd) return true;
    return false;
}

// loaderThreadMain's open: the file it then holds until its next.
OL_CODE int CtrOfLoader_open(const char* path, int flags) {
    return olOpen(path, flags, true);
}

// flowend's decode-time read: opened, read and closed within one request.
OL_CODE int CtrOfLoader_openOnce(const char* path, int flags) {
    return olOpen(path, flags, false);
}

// loaderThreadMain's close of the file it held: a kept file stays open.
OL_CODE int CtrOfLoader_close(int fd) {
    if (fd == ol.held) ol.held = -1;
    return olKept(fd) ? 0 : close(fd);
}

// flowend's close: a kept file stays open, and one the loader holds (the same file, one descriptor) stays held.
OL_CODE int CtrOfLoader_closeOnce(int fd) {
    return olKept(fd) ? 0 : close(fd);
}

// loaderThreadMain's wait for work. A shed asked for while it waits signals this event (olWake), and it sheds once the
// wait returns; one asked for just before is done before it waits. The wake signals the event only while the loader
// waits on it: after the loader's last wait (as it stops) nothing signals an event that may be gone. The loader's
// last close, of the file it held, stays newlib's (so loaderThreadMain keeps its code); on DELTARUNE the other kept
// files and the keeper are retired at the join (CtrOfLoader_join). UNDERTALE's loader runs until the process ends.
OL_CODE void CtrOfLoader_wait(LightEvent* event) {
#ifdef OL_ADOPT
    olAdopt(); // the loader's first call: before its keeper's wake can name this event
#endif
    __atomic_store_n(&ol.event, event, __ATOMIC_SEQ_CST);
    CtrOpenFiles_poll();
    LightEvent_Wait(event);
    __atomic_store_n(&ol.event, NULL, __ATOMIC_SEQ_CST);
    CtrOpenFiles_poll();
}

#ifdef OL_ADOPT
// DELTARUNE (c26rev R150 F1): stopLoaderThread's join. Once the loader has ended (joined, so nothing can be reading
// its files), the thread that stops it retires the cache (olRetire), so a chapter whose loader never starts keeps no
// file or keeper of the last one's. A failed join retires nothing: the loader may still run.
OL_CODE Result CtrOfLoader_join(Thread thread, u64 timeoutNs) {
    Result rc = threadJoin(thread, timeoutNs);
    if (R_SUCCEEDED(rc) && ol.ready) olRetire();
    return rc;
}
#endif
#endif
