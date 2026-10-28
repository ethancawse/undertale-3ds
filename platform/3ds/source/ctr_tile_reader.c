#include "ctr_tile_reader.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctr_lz.h"
#include "ctr_open_files.h"
#include "ctr_watchdog.h"
#include "gettime.h"

#define READER_STACK_SIZE (32 * 1024)
#define DECODER_STACK_SIZE (16 * 1024)
// The tiles' bytes are only read while this much heap stays free besides
// them: the game's own allocations matter more than a quicker room entry.
#define READER_HEAP_RESERVE (8u * 1024u * 1024u)
// A whole-file read (a promotion) was admitted with its size and this reserve free (ctr_textures.c, makePromotionRoom,
// which reclaims only just enough). UNDERTALE on the Old models sets it lower (CMakeLists.txt, packed-tier fix).
#ifndef READER_PROMOTION_RESERVE
#define READER_PROMOTION_RESERVE READER_HEAP_RESERVE
#endif
// It starts if 1 MB of that has gone meanwhile (a room's first frames took that much on the Old models; roomload 6).
#define READER_WHOLE_START_RESERVE (READER_PROMOTION_RESERVE - 1024u * 1024u)

uint32_t CtrTileReader_heapReserve(void) {
    return READER_PROMOTION_RESERVE;
}
// How often a wait for a read in progress looks again.
#define WAIT_SLICE_NS 500000
// Cache files the thread keeps open while reads are queued, and files the
// game thread read from waiting for the thread to close them.
#define READER_OPEN_FILES 6
// A whole file is read this much at a time. Reads of the SD card take turns,
// so a tile the game thread reads meanwhile waits for one piece at most
// (about 12 ms on an Old 3DS) rather than for the whole file (a 1.3 MB page
// takes 250 ms there).
#define WHOLE_READ_PIECE (64u * 1024u)

extern u32 __ctru_heap_size;

typedef enum {
    READ_QUEUED,
    READ_READING,
    READ_DECODE_QUEUED,
    READ_DECODING,
    READ_FINISHED,
} ReadState;

typedef struct {
    CtrTileRead* head; // oldest first
    CtrTileRead* tail;
} ReadQueue;

struct CtrTileReader {
    Thread thread;
    Thread decoder;        // nullptr if tiles aren't decoded off the game thread
    LightLock lock;
    LightEvent wake;
    LightEvent decodeWake;
    CtrPackKey key;
    // The thread's:
    FILE* open[READER_OPEN_FILES];
    uint32_t openPages[READER_OPEN_FILES];
    uint32_t openCount;
    // Guarded by lock:
    ReadQueue reads;
    ReadQueue decodes;
    bool quit;
    FILE* toClose[READER_OPEN_FILES];
    uint32_t toCloseCount;
    // A file to write once nothing is queued (see CtrTileReader_writeFile).
    char writePath[CTR_PACK_PATH_MAX];
    uint8_t* writeData;
    size_t writeSize;
};

// ===[ Queues (the lock must be held) ]===

static void push(ReadQueue* q, CtrTileRead* read) {
    read->next = nullptr;
    if (q->tail != nullptr) q->tail->next = read;
    else q->head = read;
    q->tail = read;
}

static CtrTileRead* pop(ReadQueue* q) {
    CtrTileRead* read = q->head;
    if (read == nullptr) return nullptr;
    q->head = read->next;
    if (q->head == nullptr) q->tail = nullptr;
    read->next = nullptr;
    return read;
}

static bool removeFrom(ReadQueue* q, CtrTileRead* read) {
    CtrTileRead* prev = nullptr;
    for (CtrTileRead* r = q->head; r != nullptr; prev = r, r = r->next) {
        if (r != read) continue;
        if (prev != nullptr) prev->next = r->next;
        else q->head = r->next;
        if (q->tail == r) q->tail = prev;
        r->next = nullptr;
        return true;
    }
    return false;
}

// ===[ Writing ]===

// Writes to a temporary file renamed into place, so the file is either
// whole or as it was, even if the console is switched off meanwhile.
static void writeWhole(const char* path, const uint8_t* data, size_t size) {
    char temp[CTR_PACK_PATH_MAX + 8];
    snprintf(temp, sizeof(temp), "%s.tmp", path);
    CtrPack_makeCacheDir();
    FILE* f = fopen(temp, "wb");
    if (f == nullptr) return;
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fclose(f) == 0 && ok;
    // The SD card's rename does not replace an existing file.
    if (ok) remove(path);
    if (!ok || rename(temp, path) != 0) remove(temp);
}

// Takes the file waiting to be written, if any. The lock must be held.
static uint8_t* takeWrite(CtrTileReader* reader, char* path, size_t* size) {
    uint8_t* data = reader->writeData;
    if (data == nullptr) return nullptr;
    memcpy(path, reader->writePath, sizeof(reader->writePath));
    *size = reader->writeSize;
    reader->writeData = nullptr;
    return data;
}

static uint32_t heapFree(void) {
    struct mallinfo mi = mallinfo();
    return __ctru_heap_size > (u32) mi.uordblks ? __ctru_heap_size - (u32) mi.uordblks : 0;
}

static void freeRead(CtrTileRead* read) {
    free(read->data);
    CtrPacked_free(read->packed);
    free(read);
}

const uint8_t* CtrTileReader_tileData(const CtrTileRead* read, int32_t i) {
    return read->data != nullptr ? read->data + read->tileAt[i] : CtrPacked_at(read->src, read->tileAt[i]);
}

// ===[ Reading ]===

// Reads the header from the page's open cache file f (nullptr if there is
// none), then each tile row's run of wanted tiles (they are stored one
// after another, so one read covers a run, along with any unwanted tiles
// between).
static void perform(CtrTileReader* reader, CtrTileRead* read, FILE* f) {
    read->performed = true;
    if (f == nullptr) return;
    long size = CtrPack_fileSize(f);
    if (size > 0) {
        uint8_t head[CTR_PACK_HEADER_MAX];
        size_t headSize = (size_t) size < sizeof(head) ? (size_t) size : sizeof(head);
        read->headerOk = fread(head, 1, headSize, f) == headSize &&
                         CtrPack_parseHeader(head, headSize, &reader->key, read->blobOffset, read->blobSize, &read->view) &&
                         read->view.totalSize == (uint32_t) size;
    }
    if (!read->headerOk) return;
    if (read->whole) {
        uint32_t size = read->view.totalSize;
        if (size + READER_WHOLE_START_RESERVE > CTR_PKLIN_HEAP_FREE() || (read->packed = CtrPacked_new(&read->view)) == nullptr) return;
        if (CtrPacked_read(read->packed, f, WHOLE_READ_PIECE)) {
            read->bytes = size;
        } else {
            CtrPacked_free(read->packed);
            read->packed = nullptr;
        }
        return;
    }
    const CtrPageLayout* layout = &read->view.layout;
    uint32_t total = 0;
    for (int32_t ty = 0; layout->tilesY > ty; ty++) {
        int32_t first = -1;
        int32_t last = -1;
        for (int32_t tx = 0; layout->tilesX > tx; tx++) {
            int32_t i = ty * layout->tilesX + tx;
            if (!read->want[i] || read->view.tileSize[i] == 0) continue;
            if (0 > first) first = i;
            last = i;
        }
        if (0 <= first) total += read->view.tileOffset[last] + read->view.tileSize[last] - read->view.tileOffset[first];
    }
    if (total == 0 || total + READER_HEAP_RESERVE > heapFree() || (read->data = malloc(total)) == nullptr) return;
    uint32_t at = 0;
    for (int32_t ty = 0; layout->tilesY > ty; ty++) {
        int32_t first = -1;
        int32_t last = -1;
        for (int32_t tx = 0; layout->tilesX > tx; tx++) {
            int32_t i = ty * layout->tilesX + tx;
            if (!read->want[i] || read->view.tileSize[i] == 0) continue;
            if (0 > first) first = i;
            last = i;
        }
        if (0 > first) continue;
        uint32_t start = read->view.tileOffset[first];
        uint32_t runSize = read->view.tileOffset[last] + read->view.tileSize[last] - start;
        if (fseek(f, (long) start, SEEK_SET) != 0 || fread(read->data + at, 1, runSize, f) != runSize) break;
        for (int32_t i = first; last >= i; i++) {
            if (read->view.tileSize[i] > 0) read->tileAt[i] = at + (read->view.tileOffset[i] - start);
        }
        at += runSize;
    }
    read->bytes = at;
}

static void closeFiles(CtrTileReader* reader);

// The page's cache file, from the thread's open files or opened (in place
// of the one opened first, if they are all in use or many files are open).
static FILE* readerFile(CtrTileReader* reader, uint32_t pageId) {
    for (uint32_t i = 0; reader->openCount > i; i++) {
        if (reader->openPages[i] == pageId) return reader->open[i];
    }
    bool busy = false;
    CTR_OPEN_SITE('R');
    FILE* f = CtrPack_openFile(pageId, &busy);
    if (f == nullptr && busy) {
        // Too many files open: this thread's kept-open ones go first.
        closeFiles(reader);
        f = CtrPack_openFile(pageId, nullptr);
    }
    if (f == nullptr) return nullptr;
    if (reader->openCount == READER_OPEN_FILES || (0 < reader->openCount && !CtrOpenFiles_mayKeep())) {
        CTR_TRACE_CLOSE(reader->open[0], 'R', reader->openPages[0]);
        reader->openCount--;
        memmove(&reader->open[0], &reader->open[1], reader->openCount * sizeof(reader->open[0]));
        memmove(&reader->openPages[0], &reader->openPages[1], reader->openCount * sizeof(reader->openPages[0]));
    }
    reader->open[reader->openCount] = f;
    reader->openPages[reader->openCount] = pageId;
    reader->openCount++;
    return f;
}

// Once nothing is queued: closes the thread's files and those the game
// thread read from.
static void closeFiles(CtrTileReader* reader) {
    for (uint32_t i = 0; reader->openCount > i; i++) CTR_TRACE_CLOSE(reader->open[i], 'C', reader->openPages[i]);
    reader->openCount = 0;
    FILE* toClose[READER_OPEN_FILES];
    LightLock_Lock(&reader->lock);
    uint32_t count = reader->toCloseCount;
    memcpy(toClose, reader->toClose, count * sizeof(toClose[0]));
    reader->toCloseCount = 0;
    LightLock_Unlock(&reader->lock);
    for (uint32_t i = 0; count > i; i++) CTR_TRACE_CLOSE(toClose[i], 'C', UINT32_MAX);
}

// Whether the read has texture memory to decode into. Only the caller sets
// dst, before queueing or (with the lock held) while queued.
static bool hasDst(const CtrTileRead* read) {
    for (int32_t i = 0; CTR_PAGE_MAX_TILES > i; i++) {
        if (read->dst[i] != nullptr) return true;
    }
    return false;
}

// After reading: has the tiles decoded if there is a decoder and something
// was read, else marks the read finished. Frees it if it was released
// meanwhile.
static void handOn(CtrTileReader* reader, CtrTileRead* read, uint64_t start) {
    read->ms = (float) ((double) (nowNanos() - start) / 1e6);
    LightLock_Lock(&reader->lock);
    bool abandoned = read->abandoned;
    bool decode = !abandoned && reader->decoder != nullptr && read->headerOk && read->data != nullptr && hasDst(read);
    if (decode) push(&reader->decodes, read);
    read->state = decode ? READ_DECODE_QUEUED : READ_FINISHED;
    LightLock_Unlock(&reader->lock);
    if (abandoned) freeRead(read);
    if (decode) LightEvent_Signal(&reader->decodeWake);
}

static void shedFiles(void* owner) {
    closeFiles((CtrTileReader*) owner);
}

static void wakeReader(void* owner) {
    LightEvent_Signal(&((CtrTileReader*) owner)->wake);
}

static void readerMain(void* arg) {
    CtrTileReader* reader = arg;
    CtrWatchdog_nameThread("tile reader");
    int keeper = CtrOpenFiles_addKeeper(shedFiles, wakeReader, reader);
    for (;;) {
        CtrWatchdog_note("waiting for work");
        LightEvent_Wait(&reader->wake);
        for (;;) {
            CtrWatchdog_note("taking a job");
            CtrOpenFiles_poll();
            LightLock_Lock(&reader->lock);
            if (reader->quit) {
                LightLock_Unlock(&reader->lock);
                CtrOpenFiles_removeKeeper(keeper);
                closeFiles(reader);
                return;
            }
            CtrTileRead* read = pop(&reader->reads);
            char path[CTR_PACK_PATH_MAX];
            size_t size = 0;
            uint8_t* data = nullptr;
            if (read != nullptr) read->state = READ_READING;
            else data = takeWrite(reader, path, &size);
            LightLock_Unlock(&reader->lock);
            if (data != nullptr) {
                CtrWatchdog_note("writing a file");
                writeWhole(path, data, size);
                free(data);
                continue;
            }
            if (read == nullptr) {
                // Closing costs about as much as opening: it waits until
                // the reading is done.
                CtrWatchdog_note("closing files");
                closeFiles(reader);
                break;
            }

            CtrWatchdog_note("reading tiles");
            uint64_t start = nowNanos();
            perform(reader, read, readerFile(reader, read->pageId));
            handOn(reader, read, start);
        }
    }
}

// ===[ Decoding ]===

// Decodes the read's wanted tiles that the caller reserved texture memory of
// the right format for. Tiles that fail their checksum are left for the game
// thread, which finds them damaged itself. Decoding into the heap instead,
// for the game thread to copy in, took about 5 MB more heap at the peak (of
// DELTARUNE Chapter 5's 44) for 15-25 ms off a room's entry, so the others
// are left compressed.
static void decode(CtrTileRead* read) {
    uint64_t start = nowNanos();
    const CtrPackView* view = &read->view;
    uint32_t texels = (uint32_t) view->layout.tileW * (uint32_t) view->layout.tileH;
    int32_t tiles = view->layout.tilesX * view->layout.tilesY;
    for (int32_t i = 0; tiles > i && CTR_PAGE_MAX_TILES > i; i++) {
        if (read->dst[i] == nullptr || !read->want[i] || read->tileAt[i] == CTR_READ_NOT_READ || view->tileSize[i] == 0) continue;
        if (__atomic_load_n(&read->cancel, __ATOMIC_ACQUIRE)) break;
        uint8_t format = view->layout.formats[i];
        uint32_t size = texels * CtrPack_texelBytes((CtrTileFormat) format);
        if (format == CTR_TILE_EMPTY || read->dstFormat[i] != format || read->dstSize[i] != size) continue;
        const uint8_t* in = CtrTileReader_tileData(read, i);
        if (!CtrPack_checkTile(in, view->tileSize[i], view->tileChecksum[i])) continue;
        read->decoded[i] = CtrLz_decompress(in, view->tileSize[i], read->dst[i], size);
        if (read->decoded[i]) read->decodedTiles++;
    }
    read->decodeMs = (float) ((double) (nowNanos() - start) / 1e6);
}

static void decoderMain(void* arg) {
    CtrTileReader* reader = arg;
    CtrWatchdog_nameThread("tile decoder");
    for (;;) {
        CtrWatchdog_note("waiting for work");
        LightEvent_Wait(&reader->decodeWake);
        CtrWatchdog_note("decoding tiles");
        for (;;) {
            LightLock_Lock(&reader->lock);
            if (reader->quit) {
                LightLock_Unlock(&reader->lock);
                return;
            }
            CtrTileRead* read = pop(&reader->decodes);
            if (read != nullptr) read->state = READ_DECODING;
            LightLock_Unlock(&reader->lock);
            if (read == nullptr) break;
            decode(read);
            // Released reads are waited for while decoding, never abandoned.
            LightLock_Lock(&reader->lock);
            read->state = READ_FINISHED;
            LightLock_Unlock(&reader->lock);
        }
    }
}

// ===[ Interface ]===

static void freeList(CtrTileRead* read) {
    while (read != nullptr) {
        CtrTileRead* next = read->next;
        freeRead(read);
        read = next;
    }
}

CtrTileReader* CtrTileReader_start(const CtrPackKey* key) {
    CtrTileReader* reader = calloc(1, sizeof(CtrTileReader));
    if (reader == nullptr) return nullptr;
    reader->key = *key;
    LightLock_Init(&reader->lock);
    LightEvent_Init(&reader->wake, RESET_ONESHOT);
    LightEvent_Init(&reader->decodeWake, RESET_ONESHOT);
    // Just above the game thread, on its core: the thread mostly waits for
    // the SD card, and when a read comes back it only starts the next one.
    // Below the game thread it would wait for the game's step to finish,
    // which is the time it is meant to use.
    s32 gamePriority = 0x30;
    svcGetThreadPriority(&gamePriority, CUR_THREAD_HANDLE);
    s32 priority = gamePriority > 0x18 ? gamePriority - 1 : gamePriority;
    reader->thread = threadCreate(readerMain, reader, READER_STACK_SIZE, priority, -2, false);
    if (reader->thread == nullptr) {
        free(reader);
        return nullptr;
    }
    // The New 3DS's third core is the app's. The audio threads there run
    // above the game thread's priority (they must never be late); decoding
    // runs below them and above the background texture packer.
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    if (isNew3ds) {
        s32 decodePriority = gamePriority + 2 > 0x3E ? 0x3E : gamePriority + 2;
        reader->decoder = threadCreate(decoderMain, reader, DECODER_STACK_SIZE, decodePriority, 2, false);
    }
    return reader;
}

void CtrTileReader_stop(CtrTileReader* reader) {
    if (reader == nullptr) return;
    LightLock_Lock(&reader->lock);
    reader->quit = true;
    CtrTileRead* reads = reader->reads.head;
    CtrTileRead* decodes = reader->decodes.head;
    reader->reads = (ReadQueue) { nullptr, nullptr };
    reader->decodes = (ReadQueue) { nullptr, nullptr };
    LightLock_Unlock(&reader->lock);
    freeList(reads);
    freeList(decodes);
    LightEvent_Signal(&reader->wake);
    threadJoin(reader->thread, U64_MAX);
    threadFree(reader->thread);
    if (reader->decoder != nullptr) {
        LightEvent_Signal(&reader->decodeWake);
        threadJoin(reader->decoder, U64_MAX);
        threadFree(reader->decoder);
    }
    // A file not written yet is written now.
    char path[CTR_PACK_PATH_MAX];
    size_t size = 0;
    uint8_t* data = takeWrite(reader, path, &size);
    if (data != nullptr) {
        writeWhole(path, data, size);
        free(data);
    }
    free(reader);
}

bool CtrTileReader_decodes(const CtrTileReader* reader) {
    return reader != nullptr && reader->decoder != nullptr;
}

void CtrTileReader_writeFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size) {
    LightLock_Lock(&reader->lock);
    uint8_t* replaced = reader->writeData;
    snprintf(reader->writePath, sizeof(reader->writePath), "%s", path);
    reader->writeData = data;
    reader->writeSize = size;
    LightLock_Unlock(&reader->lock);
    free(replaced);
    LightEvent_Signal(&reader->wake);
}

CtrTileRead* CtrTileReader_newRead(void) {
    CtrTileRead* read = calloc(1, sizeof(CtrTileRead));
    if (read == nullptr) return nullptr;
    for (int32_t i = 0; CTR_PAGE_MAX_TILES > i; i++) read->tileAt[i] = CTR_READ_NOT_READ;
    return read;
}

void CtrTileReader_queue(CtrTileReader* reader, CtrTileRead* read) {
    LightLock_Lock(&reader->lock);
    read->state = READ_QUEUED;
    push(&reader->reads, read);
    LightLock_Unlock(&reader->lock);
    LightEvent_Signal(&reader->wake);
}

bool CtrTileReader_queueDecode(CtrTileReader* reader, CtrTileRead* read) {
    if (reader->decoder == nullptr) return false;
    read->performed = true;
    LightLock_Lock(&reader->lock);
    read->state = READ_DECODE_QUEUED;
    push(&reader->decodes, read);
    LightLock_Unlock(&reader->lock);
    LightEvent_Signal(&reader->decodeWake);
    return true;
}

bool CtrTileReader_finished(CtrTileReader* reader, CtrTileRead* read) {
    LightLock_Lock(&reader->lock);
    bool finished = read->state == READ_FINISHED;
    LightLock_Unlock(&reader->lock);
    return finished;
}

bool CtrTileReader_addTile(CtrTileReader* reader, CtrTileRead* read, int32_t tile, uint8_t* dst, uint32_t dstSize, uint8_t dstFormat) {
    LightLock_Lock(&reader->lock);
    bool queued = read->state == READ_QUEUED;
    if (queued) {
        read->want[tile] = true;
        read->dst[tile] = dst;
        read->dstSize[tile] = dstSize;
        read->dstFormat[tile] = dstFormat;
    }
    LightLock_Unlock(&reader->lock);
    return queued;
}

bool CtrTileReader_unqueue(CtrTileReader* reader, CtrTileRead* read) {
    LightLock_Lock(&reader->lock);
    bool taken = read->state == READ_QUEUED && removeFrom(&reader->reads, read);
    // Finished as far as the caller is concerned: nothing was read.
    if (taken) read->state = READ_FINISHED;
    LightLock_Unlock(&reader->lock);
    return taken;
}

// Performs the next queued read on the calling (game) thread, if there is
// one; the thread closes the file later. Returns false if none was queued.
static bool performNext(CtrTileReader* reader) {
    LightLock_Lock(&reader->lock);
    CtrTileRead* next = reader->reads.head;
    while (next != nullptr && next->whole) next = next->next;
    if (next != nullptr) {
        removeFrom(&reader->reads, next);
        next->state = READ_READING;
    }
    LightLock_Unlock(&reader->lock);
    if (next == nullptr) return false;
    uint64_t start = nowNanos();
    CTR_OPEN_SITE('P');
    FILE* f = CtrPack_openFile(next->pageId, nullptr);
    perform(reader, next, f);
    if (f != nullptr) {
        LightLock_Lock(&reader->lock);
        bool handed = READER_OPEN_FILES > reader->toCloseCount && CtrOpenFiles_mayKeep();
        if (handed) reader->toClose[reader->toCloseCount++] = f;
        LightLock_Unlock(&reader->lock);
        if (handed) LightEvent_Signal(&reader->wake);
        else CTR_TRACE_CLOSE(f, 'P', next->pageId);
    }
    handOn(reader, next, start);
    return true;
}

double CtrTileReader_wait(CtrTileReader* reader, CtrTileRead* read) {
    uint64_t start = nowNanos();
    for (;;) {
        LightLock_Lock(&reader->lock);
        uint8_t state = read->state;
        // Not decoded yet: the caller decodes the tiles it needs itself.
        if (state == READ_DECODE_QUEUED && removeFrom(&reader->decodes, read)) state = read->state = READ_FINISHED;
        LightLock_Unlock(&reader->lock);
        if (state == READ_FINISHED) break;
        // Instead of only waiting, read the next page meanwhile: the game
        // wants the pages in the order queued, and two reads at a time
        // finish sooner than one after the other.
        if (state == READ_DECODING || !performNext(reader)) svcSleepThread(WAIT_SLICE_NS);
    }
    return (double) (nowNanos() - start) / 1e6;
}

void CtrTileReader_release(CtrTileReader* reader, CtrTileRead* read) {
    if (read == nullptr) return;
    LightLock_Lock(&reader->lock);
    if (read->state == READ_QUEUED) removeFrom(&reader->reads, read);
    if (read->state == READ_DECODE_QUEUED) removeFrom(&reader->decodes, read);
    bool reading = read->state == READ_READING;
    bool decoding = read->state == READ_DECODING;
    // Not decoded once read: the thread frees it.
    if (reading) read->abandoned = true;
    if (decoding) __atomic_store_n(&read->cancel, true, __ATOMIC_RELEASE);
    LightLock_Unlock(&reader->lock);
    if (reading) return;
    // Stopped after the tile in progress.
    while (decoding && !CtrTileReader_finished(reader, read)) svcSleepThread(WAIT_SLICE_NS);
    freeRead(read);
}

// ===[ Reads done on the game thread use its open files (utstall, item 8 step 1) ]===

// Late code, the --wrap of CtrTileReader_wait (CMakeLists.txt): CtrTileReader_wait above, except that the read it
// performs meanwhile takes the page's file from the game thread's own open cache files (ctr_textures.c's cacheFile)
// instead of opening one and handing it to this thread to close once its queue drains. That is an open and a close
// fewer each time, and no file held past the read beyond the game thread's own, which count against the same limit.
#define UTSTALL8_TEXT __attribute__((noinline, section(".text.utstall8_late")))

FILE* CtrTextures_readerGameFile(uint32_t pageId);
bool CtrTextures_readerUsesGameFiles(void);
double __real_CtrTileReader_wait(CtrTileReader* reader, CtrTileRead* read);

UTSTALL8_TEXT static bool performNextOnGameFile(CtrTileReader* reader) {
    LightLock_Lock(&reader->lock);
    CtrTileRead* next = reader->reads.head;
    while (next != nullptr && next->whole) next = next->next;
    if (next != nullptr) {
        removeFrom(&reader->reads, next);
        next->state = READ_READING;
    }
    LightLock_Unlock(&reader->lock);
    if (next == nullptr) return false;
    uint64_t start = nowNanos();
    // The file stays the game thread's: it is open for the reads it makes itself, which are made one at a time.
    perform(reader, next, CtrTextures_readerGameFile(next->pageId));
    handOn(reader, next, start);
    return true;
}

UTSTALL8_TEXT double __wrap_CtrTileReader_wait(CtrTileReader* reader, CtrTileRead* read) {
    // Only if no texture cache started the reader (CtrTextures_init always does): the original, which stays linked.
    if (!CtrTextures_readerUsesGameFiles()) return __real_CtrTileReader_wait(reader, read);
    uint64_t start = nowNanos();
    for (;;) {
        LightLock_Lock(&reader->lock);
        uint8_t state = read->state;
        if (state == READ_DECODE_QUEUED && removeFrom(&reader->decodes, read)) state = read->state = READ_FINISHED;
        LightLock_Unlock(&reader->lock);
        if (state == READ_FINISHED) break;
        if (state == READ_DECODING || !performNextOnGameFile(reader)) svcSleepThread(WAIT_SLICE_NS);
    }
    return (double) (nowNanos() - start) / 1e6;
}
