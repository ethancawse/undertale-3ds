// Reading a room's tiles from the SD card ahead of need, and decoding them.
//
// A tile that isn't packed in RAM comes from its page's cache file, and on
// the SD card opening that file costs tens of milliseconds and reading it
// about 0.2 ms per KB. A room entered from a fade wants dozens of tiles from
// a dozen pages, which the first frame used to read one after another. The
// texture cache knows which tiles the room wants as soon as the room is
// loaded; this thread reads them while the game runs the room's first step
// (scripts the game thread spends hundreds of milliseconds in), and the
// cache installs what has arrived instead of reading it itself.
//
// Decompressing a tile costs about 11 ms per MB of texels besides (a 256x256
// tile 1-3 ms), wherever its compressed bytes come from. On the New 3DS a
// second thread on the third core decodes the tiles read, and those of pages
// packed in RAM, into texture memory the texture cache reserved for them
// (when it knew their format), and the game thread then only flushes them.
// Tiles with no memory reserved stay compressed for the game thread.
//
// Only reading and decoding happen here: the threads open the page's cache
// file, check its header, read the runs of wanted tiles and decompress them
// into memory they are handed. Pages, tiles and textures stay the game
// thread's, and nothing here allocates linear or GPU memory. The reader also
// writes the texture cache's record of what rooms draw on entry, so that the
// game thread doesn't wait for the SD card for that either.
#ifndef CTRPORT_CTR_TILE_READER_H
#define CTRPORT_CTR_TILE_READER_H

#include <stdbool.h>
#include <stdint.h>

#include "ctr_texture_pack.h"

// Where a tile that wasn't read is in CtrTileRead.tileAt.
#define CTR_READ_NOT_READ UINT32_MAX

typedef struct CtrTileRead {
    // Filled in by the caller before queueing.
    uint32_t pageId;
    uint32_t blobOffset, blobSize; // of the page's image in data.win, for checking the header
    bool want[CTR_PAGE_MAX_TILES];
    // Texture memory to decode each tile into, of dstSize bytes in the format
    // dstFormat (CtrTileFormat), or nullptr. Never touched once the read is
    // released.
    uint8_t* dst[CTR_PAGE_MAX_TILES];
    uint32_t dstSize[CTR_PAGE_MAX_TILES];
    uint8_t dstFormat[CTR_PAGE_MAX_TILES];
    // For a decode of a page packed in RAM (CtrTileReader_queueDecode): the
    // packed page, which must stay until the read is released. The caller
    // then also fills in headerOk, view and tileAt (offsets into src).
    const CtrPacked* src;
    // Read the whole file instead (a page to keep packed in RAM), a piece at
    // a time so that other reads of the SD card get in between: packed then
    // holds the packed page and bytes its size, and tileAt isn't set. A
    // thread waiting for a read never performs one of these itself, and
    // nothing is decoded.
    bool whole;
    // Filled in by the threads, for the caller once finished.
    bool performed;        // the reader got to it (it wasn't taken back)
    bool headerOk;         // view holds the page's header and tile table
    CtrPackView view;
    uint8_t* data;         // the tiles read, or nullptr (no memory to spare)
    CtrPacked* packed;     // a whole read's page, or nullptr; the caller may take it
    uint32_t bytes;        // read in all
    uint32_t tileAt[CTR_PAGE_MAX_TILES]; // offset of each tile's compressed data in data (or in src), or CTR_READ_NOT_READ
    bool decoded[CTR_PAGE_MAX_TILES];    // dst holds the tile's texels
    uint32_t decodedTiles;
    float ms;              // from starting the read to finishing it
    float decodeMs;
    // The threads'.
    uint8_t state;
    bool abandoned;
    bool cancel;
    struct CtrTileRead* next;
} CtrTileRead;

// The compressed data of tile i that the read read (or has in src), for a
// tile whose tileAt is set.
const uint8_t* CtrTileReader_tileData(const CtrTileRead* read, int32_t i);

typedef struct CtrTileReader CtrTileReader;

// Starts the threads. Returns nullptr if it can't run; tiles are then read
// by the game thread as they are needed.
CtrTileReader* CtrTileReader_start(const CtrPackKey* key);
// Abandons what is queued, waits for the read in progress, and frees
// everything. Reads the caller still holds must have been released.
void CtrTileReader_stop(CtrTileReader* reader);
// Whether tiles are decoded off the game thread (the New 3DS).
bool CtrTileReader_decodes(const CtrTileReader* reader);
// Heap retained in addition to a read's compressed bytes. Promotion admission
// uses the same reserve as the worker; the worker checks again before allocating.
uint32_t CtrTileReader_heapReserve(void);

// A read to fill in and queue; all tiles start not read.
CtrTileRead* CtrTileReader_newRead(void);
// Queues a read; reads are done in the order queued.
void CtrTileReader_queue(CtrTileReader* reader, CtrTileRead* read);
// Queues the decoding of tiles already in RAM (src). Returns false, queueing
// nothing, if the reader doesn't decode.
bool CtrTileReader_queueDecode(CtrTileReader* reader, CtrTileRead* read);
// Whether the read is finished (successfully or not). Never blocks.
bool CtrTileReader_finished(CtrTileReader* reader, CtrTileRead* read);
// Adds a tile, to be decoded into dst if that isn't nullptr, to a read the
// thread hasn't started. Returns false if it has.
bool CtrTileReader_addTile(CtrTileReader* reader, CtrTileRead* read, int32_t tile, uint8_t* dst, uint32_t dstSize, uint8_t dstFormat);
// Takes a read the thread hasn't started back from the queue; the caller
// then reads the tiles itself. Returns false if it has started (or finished).
bool CtrTileReader_unqueue(CtrTileReader* reader, CtrTileRead* read);
// Waits until the read is finished, meanwhile performing reads queued after
// it (but not whole-file ones) on the calling thread. A read waiting to be
// decoded is finished at once with its tiles left compressed: the caller
// decodes them as quickly as the decoding thread would. Returns the
// milliseconds waited.
double CtrTileReader_wait(CtrTileReader* reader, CtrTileRead* read);
// Frees the read and its data, or, if it is being read, leaves that to the
// thread once it finishes. Once this returns, nothing writes to the read's
// dst memory any more (a decode in progress is stopped and waited for).
void CtrTileReader_release(CtrTileReader* reader, CtrTileRead* read);
// Writes data to the file at path once no read is queued, or when the
// thread is stopped, replacing a write still waiting. Takes data over (it
// is freed once written).
void CtrTileReader_writeFile(CtrTileReader* reader, const char* path, uint8_t* data, size_t size);

#endif
