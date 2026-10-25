#include "ctr_read_ahead.h"
#include "ctr_watchdog.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Two rings: the music and a crossfade or a second track. At 512 KB each a
// ring holds 45 s of a typical Undertale track (90 kbps) and 20 s of all but
// the biggest Deltarune ones (196 kbps at the 90th percentile).
#define RA_SLOTS 2
#define RA_SLOT_BYTES (512u * 1024u)
// One read of the card. Small enough not to hold it long when the game wants
// it too (texture tiles come from the SD card cache).
#define RA_CHUNK (32u * 1024u)
#define RA_STACK_SIZE (16 * 1024)

struct CtrReadAhead {
    // All fields under gLock.
    bool used;
    bool closing; // detached while a read was in progress
    bool inflight;
    bool failed;
    bool filled; // was read ahead as far as it goes at least once
    FILE* file;
    void* fileBuffer;
    int fd;
    uint8_t* buf; // RA_SLOT_BYTES of linear memory; file offset o is at buf[o % RA_SLOT_BYTES]
    int64_t size;
    // The ring holds the file's bytes [start, end). The decoder reads at pos;
    // bytes before pos stay until the space is needed (cheap backward seeks,
    // and a file that fits loops without touching the card).
    int64_t start, end, pos;
    uint32_t gen; // bumped when the decoder moves outside the ring
};

static LightLock gLock;
static CondVar gWake; // work for the thread
static CondVar gData; // a read finished
static Thread gThread;
static volatile bool gQuit;
static bool gReady;
static bool gThreadWaiting;
static CtrReadAhead gSlots[RA_SLOTS];
static CtrReadAheadStats gStats;

static void resetStats(void) {
    memset(&gStats, 0, sizeof(gStats));
    gStats.lowestAheadBytes = UINT32_MAX;
}

static void wakeThread(void) {
    if (gThreadWaiting) CondVar_Signal(&gWake);
}

static int64_t aheadOf(const CtrReadAhead* ra) {
    return ra->end - ra->pos;
}

// Bytes that can be read into the ring without dropping any at or after pos.
static int64_t roomIn(const CtrReadAhead* ra) {
    return (int64_t) RA_SLOT_BYTES - aheadOf(ra);
}

static bool wantsRead(const CtrReadAhead* ra) {
    if (!ra->used || ra->closing || ra->failed || ra->end >= ra->size) return false;
    if (ra->start > ra->pos || ra->pos > ra->end) return false; // the decoder seeked away and hasn't read yet
    int64_t room = roomIn(ra);
    return room >= (int64_t) RA_CHUNK || ra->end + room >= ra->size;
}

// Takes the slot's file out and frees the slot. Under gLock.
static void releaseSlot(CtrReadAhead* ra, FILE** outFile, void** outBuffer) {
    *outFile = ra->file;
    *outBuffer = ra->fileBuffer;
    uint8_t* buf = ra->buf;
    memset(ra, 0, sizeof(*ra));
    ra->buf = buf;
    ra->fd = -1;
}

static void closeFile(FILE* file, void* buffer) {
    if (file != NULL) fclose(file);
    free(buffer);
}

static void threadMain(void* arg) {
    (void) arg;
    CtrWatchdog_nameThread("stream read-ahead");
    LightLock_Lock(&gLock);
    while (!gQuit) {
        // The decoder closest to running dry goes first.
        CtrReadAhead* ra = NULL;
        for (int i = 0; i < RA_SLOTS; i++) {
            CtrReadAhead* s = &gSlots[i];
            if (wantsRead(s) && (ra == NULL || aheadOf(ra) > aheadOf(s))) ra = s;
        }
        if (ra == NULL) {
            gThreadWaiting = true;
            CtrWatchdog_note("waiting for work");
            CondVar_Wait(&gWake, &gLock);
            CtrWatchdog_note("taking a job");
            gThreadWaiting = false;
            continue;
        }

        int64_t offset = ra->end;
        uint32_t at = (uint32_t) (offset % RA_SLOT_BYTES);
        int64_t chunk = RA_CHUNK;
        if (chunk > roomIn(ra)) chunk = roomIn(ra);
        if (chunk > ra->size - offset) chunk = ra->size - offset;
        if (chunk > (int64_t) (RA_SLOT_BYTES - at)) chunk = RA_SLOT_BYTES - at;
        // The bytes about to be overwritten leave the ring.
        if (offset + chunk - ra->start > (int64_t) RA_SLOT_BYTES) ra->start = offset + chunk - RA_SLOT_BYTES;
        ra->inflight = true;
        uint32_t gen = ra->gen;
        int fd = ra->fd;
        uint8_t* dst = ra->buf + at;
        LightLock_Unlock(&gLock);

        CtrWatchdog_note("reading a stream");
        u64 t0 = svcGetSystemTick();
        ssize_t got = -1;
        if (lseek(fd, (off_t) offset, SEEK_SET) == (off_t) offset) got = read(fd, dst, (size_t) chunk);
        u64 ticks = svcGetSystemTick() - t0;

        FILE* closeMe = NULL;
        void* closeBuffer = NULL;
        CtrWatchdog_note("taking a job");
        LightLock_Lock(&gLock);
        ra->inflight = false;
        if (got > 0) gStats.bytesRead += (uint32_t) got;
        if (ticks > gStats.longestReadTicks) gStats.longestReadTicks = ticks;
        if (ra->closing) {
            releaseSlot(ra, &closeMe, &closeBuffer);
        } else if (ra->gen == gen) {
            if (got > 0) {
                ra->end += got;
                if (ra->end >= ra->size || roomIn(ra) < (int64_t) RA_CHUNK) ra->filled = true;
            } else {
                ra->failed = true;
            }
        }
        CondVar_Broadcast(&gData);
        if (closeMe != NULL || closeBuffer != NULL) {
            LightLock_Unlock(&gLock);
            closeFile(closeMe, closeBuffer);
            LightLock_Lock(&gLock);
        }
    }
    LightLock_Unlock(&gLock);
}

bool CtrReadAhead_init(int core, s32 priority) {
    if (gReady) return true;
    LightLock_Init(&gLock);
    CondVar_Init(&gWake);
    CondVar_Init(&gData);
    resetStats();
    for (int i = 0; i < RA_SLOTS; i++) {
        memset(&gSlots[i], 0, sizeof(gSlots[i]));
        gSlots[i].fd = -1;
        gSlots[i].buf = (uint8_t*) linearAlloc(RA_SLOT_BYTES);
        if (gSlots[i].buf == NULL) {
            for (int j = 0; j < i; j++) {
                linearFree(gSlots[j].buf);
                gSlots[j].buf = NULL;
            }
            return false;
        }
    }
    gQuit = false;
    gThread = threadCreate(threadMain, NULL, RA_STACK_SIZE, priority, core, false);
    if (gThread == NULL) {
        for (int i = 0; i < RA_SLOTS; i++) {
            linearFree(gSlots[i].buf);
            gSlots[i].buf = NULL;
        }
        return false;
    }
    gReady = true;
    return true;
}

void CtrReadAhead_shutdown(void) {
    if (!gReady) return;
    LightLock_Lock(&gLock);
    gQuit = true;
    CondVar_Signal(&gWake);
    LightLock_Unlock(&gLock);
    threadJoin(gThread, U64_MAX);
    threadFree(gThread);
    gThread = NULL;
    for (int i = 0; i < RA_SLOTS; i++) {
        FILE* file = NULL;
        void* buffer = NULL;
        if (gSlots[i].used) releaseSlot(&gSlots[i], &file, &buffer);
        closeFile(file, buffer);
        linearFree(gSlots[i].buf);
        gSlots[i].buf = NULL;
    }
    gReady = false;
}

uint32_t CtrReadAhead_slotBytes(void) {
    return RA_SLOT_BYTES;
}

int CtrReadAhead_slotCount(void) {
    return RA_SLOTS;
}

CtrReadAhead* CtrReadAhead_attach(FILE* file, void* fileBuffer, long pos) {
    if (!gReady || file == NULL || pos < 0) return NULL;
    int fd = fileno(file);
    if (fd < 0) return NULL;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0 || pos > (long) size) return NULL;

    CtrReadAhead* ra = NULL;
    LightLock_Lock(&gLock);
    for (int i = 0; i < RA_SLOTS; i++) {
        if (!gSlots[i].used) {
            ra = &gSlots[i];
            break;
        }
    }
    if (ra != NULL) {
        ra->used = true;
        ra->closing = ra->inflight = ra->failed = ra->filled = false;
        ra->file = file;
        ra->fileBuffer = fileBuffer;
        ra->fd = fd;
        ra->size = (int64_t) size;
        ra->start = ra->end = ra->pos = pos;
        ra->gen++;
        wakeThread();
    }
    LightLock_Unlock(&gLock);
    return ra;
}

size_t CtrReadAhead_read(CtrReadAhead* ra, void* dst, size_t n) {
    uint8_t* out = (uint8_t*) dst;
    size_t done = 0;
    u64 waitStart = 0;
    LightLock_Lock(&gLock);
    while (done < n && ra->pos < ra->size) {
        if (ra->pos >= ra->start && ra->pos < ra->end) {
            if (ra->filled && ra->end < ra->size && (uint64_t) aheadOf(ra) < gStats.lowestAheadBytes) {
                gStats.lowestAheadBytes = (uint32_t) aheadOf(ra);
            }
            uint32_t at = (uint32_t) (ra->pos % RA_SLOT_BYTES);
            size_t k = n - done;
            if ((int64_t) k > aheadOf(ra)) k = (size_t) aheadOf(ra);
            if (k > RA_SLOT_BYTES - at) k = RA_SLOT_BYTES - at;
            memcpy(out + done, ra->buf + at, k);
            ra->pos += (int64_t) k;
            done += k;
            continue;
        }
        if (ra->failed) break;
        // Outside the ring: start it over here. At its end: wait for the read.
        if (ra->pos != ra->end || ra->pos < ra->start) {
            ra->start = ra->end = ra->pos;
            ra->filled = false;
            ra->gen++;
        }
        if (waitStart == 0) waitStart = svcGetSystemTick();
        wakeThread();
        CondVar_Wait(&gData, &gLock);
    }
    if (waitStart != 0) {
        u64 waited = svcGetSystemTick() - waitStart;
        gStats.waits++;
        gStats.waitTicks += waited;
        if (waited > gStats.longestWaitTicks) gStats.longestWaitTicks = waited;
    }
    if (wantsRead(ra)) wakeThread();
    LightLock_Unlock(&gLock);
    return done;
}

bool CtrReadAhead_seek(CtrReadAhead* ra, long offset, int whence) {
    LightLock_Lock(&gLock);
    int64_t base = 0;
    if (whence == SEEK_CUR) base = ra->pos;
    else if (whence == SEEK_END) base = ra->size;
    int64_t target = base + offset;
    bool ok = target >= 0 && target <= ra->size;
    if (ok) ra->pos = target;
    LightLock_Unlock(&gLock);
    return ok;
}

long CtrReadAhead_tell(CtrReadAhead* ra) {
    LightLock_Lock(&gLock);
    long pos = (long) ra->pos;
    LightLock_Unlock(&gLock);
    return pos;
}

void CtrReadAhead_detach(CtrReadAhead* ra) {
    if (ra == NULL) return;
    FILE* file = NULL;
    void* buffer = NULL;
    LightLock_Lock(&gLock);
    if (ra->inflight) ra->closing = true;
    else releaseSlot(ra, &file, &buffer);
    LightLock_Unlock(&gLock);
    closeFile(file, buffer);
}

void CtrReadAhead_takeStats(CtrReadAheadStats* out) {
    if (!gReady) {
        memset(out, 0, sizeof(*out));
        out->lowestAheadBytes = UINT32_MAX;
        return;
    }
    LightLock_Lock(&gLock);
    *out = gStats;
    resetStats();
    LightLock_Unlock(&gLock);
}
