// GPU texture cache for data.win texture pages.
//
// The PICA200 cannot sample textures larger than 1024x1024, so each page is
// split into a grid of power-of-two tiles of at most CTR_TILE_MAX on a side
// (see ctr_texture_pack.h). Tiles, not pages, are what the cache keeps in
// linear memory: a tile loads the first time something draws from it and
// stays under a byte budget, the least recently used tiles going first. A
// room that shows a few sprites from each of a dozen large pages then holds
// those parts only. Fully transparent tiles are never loaded at all.
//
// Decoding a page image takes hundreds of milliseconds, so pages are also
// kept packed (converted and each tile LZ-compressed) in RAM under a second
// budget and in cache files on the SD card, which a background worker fills
// for every page. A tile then loads from the first of: the packed copy in
// RAM, the cache file (a read of that tile alone), the image (which decodes
// the whole page, so every tile the room wants is loaded from it at once).
// The tiles a room wants are read and, on the New 3DS, decompressed ahead
// on other threads (see ctr_tile_reader.h), into texture memory reserved
// for them here.
#ifndef CTRPORT_CTR_TEXTURES_H
#define CTRPORT_CTR_TEXTURES_H

#include <3ds.h>
#include <citro3d.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ctr_gpu_queue.h"
#include "ctr_texture_pack.h"
#include "ctr_tile_reader.h"
#include "data_win.h"
#include "file_system.h"
#include "renderer.h"

typedef struct {
    C3D_Tex tex;            // allocated while valid
    uint32_t lastUsedFrame;
    uint32_t offset, size;  // of the compressed tile in the packed page, once the page's pack info is known
    uint32_t checksum;
    uint8_t format;         // CtrTileFormat, once known
    bool valid;             // resident
    bool wanted;            // used by what the current room shows from the start
    bool now;               // wanted, and drawn in the room's first frames (not just another frame of an animation)
    bool requested;         // to load in the current ensureTiles or prefetch
    bool reserved;          // tex is allocated (and counted) for the page's read to decode into, but not loaded yet
    bool formatRemembered;  // format is from the room memory, before the page's formats are known
    uint32_t seenVisit;     // the room visit it was last recorded drawn in (CtrTextureCache.recordVisit)
    C3D_Tex* mirror;        // its copy in VRAM to draw from (a CtrTileMirror's), or nullptr
    uint32_t probeHash;     // its texels' hash once loaded, with the texture probe on (CtrTextures_hashTexels)
} CtrTile;

// The texture to draw a tile with: its copy in VRAM if it has one. What reads
// a tile on the CPU reads tex, which stays in linear memory.
static inline C3D_Tex* CtrTextures_drawTex(CtrTile* tile) {
    return tile->mirror != nullptr ? tile->mirror : &tile->tex;
}

// A copy in VRAM of a tile drawn frame after frame (see "Tiles in VRAM" in
// ctr_renderer.c): the tile's texture, with the copy's data.
typedef struct {
    C3D_Tex tex;
    CtrTile* tile;          // whose copy it is, or nullptr
    // Given up in the current frame: a batch may still hold tex, whose data
    // is the tile's own again. Free for another tile from the next frame on.
    bool dead;
    // The GX copy that fills it, which dropMirror cancels if the GPU hasn't
    // been handed it yet.
    CtrGpuQueued copy;
} CtrTileMirror;

#define CTR_MIRROR_MAX 256
#define CTR_HOT_MAX 64

typedef enum {
    CTR_CACHE_FILE_UNKNOWN,
    CTR_CACHE_FILE_MISSING, // not there or unusable when last looked for
    CTR_CACHE_FILE_VALID,
} CtrCacheFileState;

typedef struct {
    bool layoutKnown;      // the size and tile grid below are set, and tiles is allocated
    bool formatsKnown;     // every tile's format is known (from the pack info or a full decode)
    bool packInfo;         // every tile's place in the packed page is known
    int32_t width, height; // page size in pixels
    int32_t tileW, tileH;  // power of two, at most CTR_TILE_MAX
    int32_t tilesX, tilesY;
    CtrTile* tiles;        // tilesX * tilesY once the layout is known
    uint32_t bytes;        // texture memory held by resident tiles
    uint32_t residentTiles;
    uint32_t lastUsedFrame;
    uint32_t retryFrame;   // after running out of memory, don't load again before this frame
    uint32_t imageFrame;   // when the page was last decoded from its image
    bool imageDecoded;
    bool failed;           // decoding failed; not retried
    CtrPacked* packed;     // packed copy in RAM, or nullptr
    uint32_t packedSize;
    uint32_t fileLoads;    // loads of some of its tiles from the cache file
    uint32_t promotionBytes; // file size for a promotion requested by Draw, serviced before a later Step/Draw
    uint32_t promotionRetryFrame;
    bool promotionPending;
    CtrCacheFileState cacheFile;
    CtrTileRead* read;     // the room's wanted tiles, being read ahead, or nullptr
    bool wantedNow;        // has tiles the current room wants
    int32_t wantDepth;     // the greatest depth they are drawn at: pages drawn first are read first
    uint32_t firstUse;     // where it came in the room's remembered tiles (UINT32_MAX if not there); read in that order
    bool fillRead;         // filling in the room's tiles has had the reader read this page
} CtrPage;

// Cache files kept open, so that loading from a few pages in turn doesn't
// reopen them each time (on the New 3DS XL an open costs 8-20 ms and a close
// 2-4 ms). DELTARUNE Chapter 5's Flowery fight loads tiles from 16 pages in
// turn: with 4 files kept open, half its loads reopened one; with 12, one in
// twenty. Each is an FS handle, as are the reader's and the audio streams',
// so no more than that (a failed open closes these and tries again).
#define CTR_OPEN_FILES 12

typedef struct {
    uint32_t count;
    uint32_t tiles;
    double msTotal;
} CtrLoadStats;

typedef struct {
    DataWin* dataWin;
    FileSystem* fileSystem; // for GameMaker's external texture files
    CtrPage* pages;
    uint32_t pageCount;
    uint32_t residentBytes;
    uint32_t budgetBytes;
    uint32_t packedBytes;  // RAM held by packed copies
    uint32_t packedBudgetBytes;
    uint32_t frame;
    uint32_t evictionCount;
    uint32_t packedReclaims, packedReclaimBytes;
    uint32_t promotions, failedPromotions;
    CtrPackKey key;
    CtrPackWorker* worker; // nullptr if not running
    bool workerKeepsPacked; // the worker hands over copies of the pages it reads or packs
    uint32_t workerPacked; // pages the worker packed (or found packed) so far
    uint32_t heapCheckFrame;
    CtrPage* busyPage;     // the page being loaded, whose packed copy may be in use
    FILE* files[CTR_OPEN_FILES]; // the cache files read from last, kept open
    uint32_t filePages[CTR_OPEN_FILES];
    uint32_t fileUsed[CTR_OPEN_FILES]; // when each was last used, for replacing the oldest
    uint32_t fileUses;
    int fileKeeper; // CtrOpenFiles_addKeeper id of the files kept open
    bool fileBusy;  // the last cacheFile failed with too many files open
    CtrTileReader* reader; // reads a room's wanted tiles ahead; nullptr if not running
    // A page's cache file the reader is reading whole, to keep packed in RAM,
    // or nullptr. Its size counts against the packed budget meanwhile.
    CtrTileRead* wholeRead;
    uint32_t wholePage;
    uint32_t wholeBytes;
    C3D_Tex clear;         // 8x8 transparent, for fully transparent tiles
    CtrLoadStats fromRam, fromFile, fromImage, fromRead;
    uint32_t decodedAhead;  // tiles the reader decoded off the game thread (installed with a flush or a copy)
    // Read ahead for the current room so far.
    uint32_t readPages, readBytes;
    double readMs;
    uint64_t frameLoadNanos; // spent loading tiles in the current frame
    // Loading the tiles the room wants a few at a time (see prefetchRoom).
    bool fillPending;
    bool filling;          // a fill load is in progress
    uint32_t fillPage;     // the next page to look at
    uint32_t fillRevisit;  // the first page passed over while its tiles were being read, or UINT32_MAX
    uint32_t fillFrom;     // the first frame to fill in
    Runner* runner;        // for the second look at the room once its first step has run
    uint32_t fillTiles;
    double fillMs;
    // The tiles each room drew in its first frames on the last visit, as
    // pageId << 8 | tile in the order first drawn (stb_ds arrays, by room
    // index), kept in a file in the cache folder (see "Room memory").
    uint32_t** roomTiles;
    uint32_t roomCount;
    int32_t recordRoom;    // the room whose tiles are being recorded, or -1
    uint32_t recordUntil;  // the frame recording stops at
    uint32_t recordVisit;  // counts room visits
    uint32_t recordFirstFrame; // the first frame the room drew in, or UINT32_MAX
    uint32_t* recording;   // stb_ds array
    uint32_t roomStartFrame; // the frame the current room was entered in
    bool roomThrashed;     // see CtrTextures_roomThrashing
    bool probeHashes;      // the texture probe is on: tiles note their texels' hash as they load (CtrTile.probeHash)
    // Tiles' copies in VRAM, and the tiles drawn in the frame before and
    // again in this one that have none (the first CTR_HOT_MAX).
    CtrTileMirror mirrors[CTR_MIRROR_MAX];
    uint32_t mirrorBytes;
    CtrTile* hot[CTR_HOT_MAX];
    uint32_t hotCount;
} CtrTextureCache;

void CtrTextures_init(CtrTextureCache* cache, DataWin* dataWin, FileSystem* fileSystem, uint32_t budgetBytes);
// Deletes the record of each room's first tiles (room_memory_reset); call before CtrTextures_init.
void CtrTextures_forgetRoomMemory(void);
// Frees everything; init may follow for another data.win (DELTARUNE's
// chapters each have their own).
void CtrTextures_destroy(CtrTextureCache* cache);

// Call once per frame after the GPU has finished the previous frame. Tiles
// not used in the current frame may be evicted from then on.
void CtrTextures_beginFrame(CtrTextureCache* cache, uint32_t frame);

// Frees linear memory for something other than tiles (a surface that VRAM
// has no room for): evicts the tiles last used in the oldest frame, never
// those used in the current one. Returns false if no tile could be evicted.
bool CtrTextures_evictOldest(CtrTextureCache* cache);
// The same, but only of tiles not drawn in the last idleFrames frames (0:
// any but the current one's). Returns false if there are none.
bool CtrTextures_evictOldestIdle(CtrTextureCache* cache, uint32_t idleFrames);
// Evicts the one least recently used tile, the current frame's too: only
// once the GPU has run everything queued, as nothing uses them then (they
// load again when drawn). Returns false if no tile is resident.
bool CtrTextures_evictLeastRecent(CtrTextureCache* cache);
// Evicts the one least recently used tile not drawn in the last idleFrames
// frames (0: any but the current one's). Returns false if there is none.
bool CtrTextures_evictLeastRecentIdle(CtrTextureCache* cache, uint32_t idleFrames);

// Returns the page with its size and tile grid known, or nullptr if the page
// does not exist or cannot be loaded. Tiles load with CtrTextures_ensureTiles.
CtrPage* CtrTextures_acquire(CtrTextureCache* cache, uint32_t pageId);

// Marks tiles [tx0, tx1] x [ty0, ty1] of an acquired page used this frame and
// loads those that aren't resident. Tiles that can't be loaded (no memory
// yet) stay invalid; so do fully transparent ones (format CTR_TILE_EMPTY).
void CtrTextures_ensureTiles(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1);
// The same, but for the tiles drawn from something else (a palette shader's
// recoloured copy): bit k of drawnElsewise is the k-th tile of the range, row
// by row (the first 64). Those count as drawn in the room's memory, so they
// load on the next visit, but aren't marked used nor loaded.
void CtrTextures_ensureTilesBut(CtrTextureCache* cache, CtrPage* page, int32_t tx0, int32_t ty0, int32_t tx1, int32_t ty1, uint64_t drawnElsewise);

// Marks the tiles the current room shows from the start (backgrounds, tiles,
// layer sprites, sprites of visible instances) as wanted. Call right after a
// room change. The first frame's draws load the tiles they use, and the other
// wanted ones load a few per frame after that, so that a room which uses
// many pages neither stalls on entry nor loads its tiles one hitch at a time
// later on. A page that isn't packed yet is decoded whole when first drawn,
// and its wanted tiles load with it. The tiles the room drew in its first
// frames on the last visit (kept in the cache folder) are wanted too, and
// those the first frame draws are read ahead in the order it draws them.
void CtrTextures_prefetchRoom(CtrTextureCache* cache, Runner* runner);

// Lends `bytes` of the budget to other linear memory the renderer allocates
// (palette shaders' recoloured tiles), evicting tiles not used in the last
// idleFrames frames until the rest fit. False, lending nothing, if they
// don't: tiles the room draws are never evicted for it, nor are those it was
// found to want (marked used when found) for a while, and nothing is lent
// once the room is thrashing (see below). Returned with
// CtrTextures_returnBudget once that memory is freed.
bool CtrTextures_lendBudget(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames);
void CtrTextures_returnBudget(CtrTextureCache* cache, uint32_t bytes);

// Whether, since the current room was entered, a tile it drew in the last
// CTR_THRASH_FRAMES frames has been evicted: what it draws doesn't fit the
// budget, and memory lent out is better given back.
#define CTR_THRASH_FRAMES 8u
bool CtrTextures_roomThrashing(const CtrTextureCache* cache);
// Lets the current room borrow again after it thrashed: what it draws has
// since settled with room to spare (the renderer judges).
void CtrTextures_forgiveThrashing(CtrTextureCache* cache);

// Takes the tiles drawn in the last frame and the one before that have no
// copy in VRAM (up to max of them), emptying the list.
uint32_t CtrTextures_takeHot(CtrTextureCache* cache, CtrTile** out, uint32_t max);
// Gives a resident tile its copy in VRAM, whose data the caller has
// allocated with vramAlloc and fills before anything draws from it. False if
// there is no slot for it (the caller frees the data).
bool CtrTextures_addMirror(CtrTextureCache* cache, CtrTile* tile, void* vram);
// Remembers the GX copy (CtrGpu_lastQueued) that fills a tile's new copy.
void CtrTextures_noteMirrorCopy(CtrTile* tile, CtrGpuQueued copy);
// Frees the copies in VRAM of the tiles not drawn in the current frame, or
// with inUse of all tiles: only once the frame so far has been handed to the
// GPU, or outside a frame. What draws from them next draws from the tiles'
// own textures. Returns the bytes freed.
uint32_t CtrTextures_releaseMirrors(CtrTextureCache* cache, bool inUse);

// Stops the background worker, waiting for the page in progress. Pages it
// hasn't packed yet are packed on a later run.
void CtrTextures_stopWorker(CtrTextureCache* cache);
void CtrTextures_logStats(CtrTextureCache* cache);
#ifdef CTRPORT_RENDER_TIMING
// Logs which pages are packed in RAM (the cache's settled set) and every page's
// loads by source so far, with `label` (timing builds only).
void CtrTextures_logPackedSet(const CtrTextureCache* cache, const char* label);
#endif
// A hash of a texture's texels as stored (tex->size bytes), for the texture probe.
uint32_t CtrTextures_hashTexels(const C3D_Tex* tex);
// CtrTextures_lendBudget counting tiles no one has wanted since the room began as idle (kawkaw).
bool CtrTextures_lendBudgetInRoom(CtrTextureCache* cache, uint32_t bytes, uint32_t idleFrames);

#endif
