// Texture page conversion and packed pages.
//
// A page is converted to the PICA200's tiled texel layout, split into tiles of
// at most CTR_TILE_MAX on a side, each in the smallest format that holds it
// exactly (see CtrTileFormat), or empty when every pixel is transparent.
// Tiles are also the unit the texture cache keeps resident, so a room that
// uses part of a page only holds that part.
//
// A packed page is that converted result, each tile LZ-compressed (ctr_lz.h)
// on its own behind a header and a tile table. Packed pages are stored on the
// SD card in a "cache" folder next to data.win and kept in RAM when there is
// room, so a tile loads with one small read (or none) and a fast decompress
// instead of an image decode and a conversion.
//
// A background worker packs every page of data.win ahead of need, from its
// own data.win handle, and tells the game thread as pages are done.
#ifndef CTRPORT_CTR_TEXTURE_PACK_H
#define CTRPORT_CTR_TEXTURE_PACK_H

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "data_win.h"
#include "ctr_cache_publish.h"

// Smaller tiles hold less of the pages a room only partly uses, so more of
// what it draws stays resident, but each change of tile breaks the draw
// batch. 512 once halved the frame rate in DELTARUNE's castle town against
// 256; today the castle town draws in 4.8 ms either way, and 256 cuts the
// texture loads where the working set outgrows the budget: on the New 3DS
// in Azahar, the Titan fight's 1% low went from 6.2 to 22.4 fps (79 loads,
// 2.4 s of loading in 20 s, down to 14 and 0.1 s), and the Roaring Knight
// and Chapter 5's flower fights from about 8 to 14. The Old 3DS's heap was
// also too small for the 4 MB strips 512 takes to decode a 2048 pixel wide
// page alongside the game.
#define CTR_TILE_MAX 256
// Enough for a 4096x4096 page.
#define CTR_PAGE_MAX_TILES ((4096 / CTR_TILE_MAX) * (4096 / CTR_TILE_MAX))

// A tile is stored as RGBA5551 when every texel survives it (red, green and
// blue on the 5-bit grid the GPU expands from, alpha 0 or 255, clear texels
// included, as filtering may blend their colour in), else, if
// fully opaque, as RGB565 when its colours survive that, else as RGB8, and
// otherwise as RGBA8. With rounded packing (see CtrPack_lossy), mostly
// opaque tiles are RGBA5551 regardless, their colours rounded to 5 bits and
// their alpha to 0 or 255, and the rest RGBA8.
typedef enum {
    CTR_TILE_RGBA5551,
    CTR_TILE_RGBA8,
    CTR_TILE_EMPTY, // fully transparent: no texels stored or loaded
    CTR_TILE_RGB8,
    CTR_TILE_RGB565,
    CTR_TILE_FORMAT_COUNT,
} CtrTileFormat;

// Whether exact tiles can be chosen (the exact_colours setting). Tiles are
// rounded by default: exact ones are mostly RGBA8, which DELTARUNE Chapter
// 5's Kawkaw dash draws at 19.9 fps on a New 3DS XL against 26.7 rounded
// (the GPU's texel reads), and take 61% more texture memory and a third
// more SD card space. On the Old 3DS,
// with a third of the New 3DS's texture budget, exact tiles thrash in
// Chapter 5's heavy rooms.
#ifndef CTR_PACK_EXACT_AVAILABLE
#ifdef CTRPORT_TARGET_O3DS
#define CTR_PACK_EXACT_AVAILABLE 0
#else
#define CTR_PACK_EXACT_AVAILABLE 1
#endif
#endif

typedef struct {
    int32_t width, height; // page size in pixels
    int32_t tileW, tileH;  // power of two, at most CTR_TILE_MAX
    int32_t tilesX, tilesY;
    uint8_t formats[CTR_PAGE_MAX_TILES]; // CtrTileFormat
} CtrPageLayout;

// Identifies the data.win that packed pages were made from.
typedef struct {
    uint32_t dataWinSize;
    uint32_t gameId;
    uint64_t timestamp;
} CtrPackKey;

// The data.win being run (default <game dir>/data.win). The worker reads
// that file and cache files go in a "cache" folder beside it, so DELTARUNE
// chapters, which switch data.win with game_change, each keep their own.
// Call while no worker runs, before the renderer is created.
void CtrPack_setDataWinPath(const char* path);
// Exact tiles (see CtrTileFormat) or rounded ones, the default. Each kind
// has its own cache files, in a "cache_exact" folder beside the "cache" one,
// so switching back and forth doesn't prepare either again. Ignored where
// CTR_PACK_EXACT_AVAILABLE is 0. Call while no worker runs, before the
// renderer is created; the folder follows CtrPack_setDataWinPath.
void CtrPack_setExact(bool exact);
// Whether tiles are rounded to fit 16 bits (see CtrTileFormat).
bool CtrPack_lossy(void);
// Diagnostic only: use a separate cache[_exact]_lt_<run> beside data.win.
// Zero selects the normal cache. Never deletes the player's cache. Call only
// while no worker runs. direct=false selects stdio publication for a matched A/B.
void CtrPack_setTestRun(unsigned run, bool direct);

// ===[ Conversion ]===

void CtrPack_init(void);
CtrPackKey CtrPack_keyFor(const DataWin* dataWin);
GPU_TEXCOLOR CtrPack_gpuFormat(CtrTileFormat format);
// Bytes a texel of the format takes (0 for empty tiles).
uint32_t CtrPack_texelBytes(CtrTileFormat format);
// Texture memory for tile `tile` of the layout (0 for an empty tile).
uint32_t CtrPack_tileBytes(const CtrPageLayout* layout, int32_t tile);
// Sets the layout's size and tile grid for a width x height page.
void CtrPack_layoutGrid(CtrPageLayout* layout, int32_t width, int32_t height);

// A page is decoded and converted one row of tiles at a time, so the whole
// page never has to be in RAM as RGBA8. `rows` holds the pixels of tile row
// ty: tileH rows of the page (fewer at the bottom), width * 4 bytes each.
// The layout's formats for that row are chosen before fn is called. Returns
// false to stop decoding.
typedef bool (*CtrPackRowFn)(void* user, const CtrPageLayout* layout, int32_t ty, const uint8_t* rows);

typedef enum {
    CTR_DECODE_OK,
    CTR_DECODE_BAD_DATA,
    CTR_DECODE_NO_MEMORY,
    CTR_DECODE_STOPPED, // fn returned false
} CtrDecodeStatus;

// Reads the page size from the start of a page image (PNG, QOI or BZip2+QOI;
// 64 bytes are enough) and estimates the RAM decoding it takes, besides the
// image itself. Returns false if the format is unknown.
bool CtrPack_imageInfo(const uint8_t* head, size_t size, int32_t* width, int32_t* height, size_t* decodeBytes);
// Decodes a page image and calls fn for each tile row, top to bottom. Fills
// in the layout as it goes. An urgent decode is one the game waits for: it
// may stop a background decode to take its workspace (ctr_qoi.h), which
// then fails with CTR_DECODE_NO_MEMORY.
CtrDecodeStatus CtrPack_decodeRows(const uint8_t* blob, size_t size, bool gm2022_5, bool urgent, CtrPageLayout* layout, CtrPackRowFn fn,
                                   void* user);
// Writes tile (tx, ty) as tiled texels to dst (tileW * tileH texels), from
// the pixels of its tile row. Not for empty tiles.
void CtrPack_convertTile(void* dst, const CtrPageLayout* layout, int32_t tx, int32_t ty, const uint8_t* rows);

// ===[ Packed pages ]===

typedef struct {
    CtrPageLayout layout;
    uint32_t tileOffset[CTR_PAGE_MAX_TILES]; // of the compressed tile, from the start of the packed page
    uint32_t tileSize[CTR_PAGE_MAX_TILES];   // 0 for empty tiles
    uint32_t tileChecksum[CTR_PAGE_MAX_TILES];
    uint32_t totalSize;                      // of the whole packed page
} CtrPackView;

// The start of a packed page that holds its header and tile table is at most
// this long; CtrPack_parseHeader needs that much (or the whole page, if shorter).
#define CTR_PACK_HEADER_MAX (64 + CTR_PAGE_MAX_TILES * 12)

// Checks the header and tile table of a packed page against the data.win and
// the page's blob, and finds its tiles. `size` is how much of the page `data`
// holds. Never trusts the input: anything inconsistent fails.
bool CtrPack_parseHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize, CtrPackView* out);
// Checks a compressed tile against its checksum from the tile table.
bool CtrPack_checkTile(const uint8_t* data, uint32_t size, uint32_t checksum);

// A packed page in RAM, in pieces of about CTR_PACKED_PIECE_BYTES that each
// hold whole tiles; the first starts with the header and tile table. Once a
// game has run a while the heap's free space is scattered: on the Old 3DS,
// 11.4 MB free had no block larger than 2.6 MB, so a 2.7 MB page couldn't be
// kept in one piece and its tiles came from the SD card one by one instead.
#define CTR_PACKED_PIECE_BYTES (256u * 1024u)
typedef struct {
    uint32_t size;       // of the whole packed page
    uint32_t pieceCount;
    uint32_t* starts;    // where in the page each piece starts, ascending (the first at 0)
    uint8_t** pieces;
} CtrPacked;

// A packed page of the view's size, its pieces allocated (from the heap) but
// not filled in; nullptr if they can't all be had.
CtrPacked* CtrPacked_new(const CtrPackView* view);
// Takes over pieces[0..count) that start at starts[0..count) (each holding
// whole tiles) as a packed page of `size` bytes. Returns nullptr, freeing
// nothing, if the small block that keeps track of them can't be had.
CtrPacked* CtrPacked_adopt(uint8_t* const* pieces, const uint32_t* starts, uint32_t count, uint32_t size);
void CtrPacked_free(CtrPacked* packed);
// The packed page's bytes from `offset` to the end of the piece that holds
// it, which holds the whole tile starting there.
const uint8_t* CtrPacked_at(const CtrPacked* packed, uint32_t offset);
// Piece i's size.
uint32_t CtrPacked_pieceSize(const CtrPacked* packed, uint32_t i);
// Reads an open cache file into a packed page made with CtrPacked_new, from
// the start, `chunk` bytes per read. Returns false if a read fails.
bool CtrPacked_read(CtrPacked* packed, FILE* f, uint32_t chunk);

// Opens a page's cache file for reading, unbuffered: tiles are read with one
// read each. Returns nullptr if there is none, or if it couldn't be opened
// with too many files open; then *busy (if not null) is set, else cleared.
FILE* CtrPack_openFile(uint32_t pageId, bool* busy);
// Reads an open cache file whole, checking its header against the data.win
// and the page's blob. Returns nullptr if it is unreadable, not a valid
// packed page or there is no memory for it.
CtrPacked* CtrPack_readFile(FILE* f, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize);
// The size of an open cache file, or -1.
long CtrPack_fileSize(FILE* f);
// Deletes a page's cache file that turned out to be damaged, so that the
// worker builds it again on the next run.
void CtrPack_removeFile(uint32_t pageId);
// Deletes every page's cache file and the completion marker, so that they
// are all packed again (the repack_textures launch setting).
void CtrPack_forgetCache(uint32_t pageCount);
// Writes a 1 MB test file in the cache folder several ways and logs how fast
// each went (the sd_write_test launch setting).
void CtrPack_sdWriteTest(void);
// Whether every page of the data.win has a cache file, as of the last time
// the worker finished (a quick check that opens no page files).
bool CtrPack_isComplete(const CtrPackKey* key, uint32_t pageCount);
// The path of the file `name` in the cache folder.
#define CTR_PACK_PATH_MAX 300
void CtrPack_cachePath(char* buf, size_t bufSize, const char* name);
// Makes the cache folder if it is missing.
void CtrPack_makeCacheDir(void);

// ===[ Background worker ]===

typedef struct CtrPackWorker CtrPackWorker;

// How writing a packed page's cache file went. Normally the page is written
// whole once packed; streamed means the heap ran short and it went a tile at
// a time.
typedef struct {
    uint32_t calls; // write calls
    bool streamed;
    bool staged; // whole, through the aligned staging buffer
    float callMinMs, callMaxMs; // the quickest and slowest write call
    // Waiting for another thread to finish writing, then opening the file,
    // sizing it, writing the data, flushing, closing and renaming it.
    float waitMs, openMs, sizeMs, dataMs, flushMs, closeMs, renameMs;
    CtrCachePublishStats publication;
} CtrPackWriteStats;

typedef struct {
    uint32_t pageId;
    CtrPacked* packed; // the packed page, owned by the receiver; nullptr unless kept in RAM (see setKeepPacked)
    size_t size;
    bool built;      // packed now (else a valid cache file already existed)
    // Time spent on a page packed now: decodeMs is reading and decoding the
    // image, packMs converting, compressing and writing it. In more detail:
    // reading the image from data.win, inflating it (BZip2 or deflate),
    // converting it to texels, compressing them, and writing the cache file.
    float decodeMs, packMs;
    float readMs, inflateMs, convertMs, compressMs, writeMs;
    // The worker thread that packed it, and the core it finished on.
    int8_t thread, core;
    CtrPackWriteStats io;
} CtrPackResult;

// Starts packing every page that has no valid cache file yet. Pages that
// don't fit in the free RAM at the time are tried again later, until the
// worker is stopped. In the foreground (nothing else running) the worker
// uses every core it can, as many threads at once as memory allows, takes
// its decoding buffers from linear memory first and keeps no packed pages in
// RAM; a page that doesn't fit even alone is left to the background packing.
// Returns nullptr if the worker can't run (no data.win at the expected path,
// no thread).
CtrPackWorker* CtrPackWorker_start(const DataWin* dataWin, const CtrPackKey* key, bool foreground);
// Whether foreground packing may borrow the system core for a thread (on by
// default). Takes effect at the next start.
void CtrPackWorker_setUseSyscore(bool use);
// Pages done (packed, found packed, or found unpackable) out of all to pack.
void CtrPackWorker_progress(CtrPackWorker* worker, uint32_t* done, uint32_t* total);
// How often a page was put back for want of memory, and how many pages the
// foreground left to the background packing.
void CtrPackWorker_memoryStats(CtrPackWorker* worker, uint32_t* waits, uint32_t* deferred);
// How many threads are packing.
uint32_t CtrPackWorker_threadCount(const CtrPackWorker* worker);
// Takes one finished page, if any. Never blocks.
bool CtrPackWorker_poll(CtrPackWorker* worker, CtrPackResult* out);
// Asks for this page to be packed next, ahead of the others (the game is
// waiting for it). Never blocks.
void CtrPackWorker_hurry(CtrPackWorker* worker, uint32_t pageId);
// Whether results should carry the packed data (false once RAM is full).
void CtrPackWorker_setKeepPacked(CtrPackWorker* worker, bool keep);
// Stops the worker, waiting for the page in progress, and frees everything.
void CtrPackWorker_stop(CtrPackWorker* worker);

#endif
