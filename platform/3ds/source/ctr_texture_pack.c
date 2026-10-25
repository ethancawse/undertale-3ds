#include "ctr_texture_pack.h"

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"
#include "ctr_cache_publish.h"
#include "ctr_inflate.h"
#include "ctr_log.h"
#include "ctr_lz.h"
#include "ctr_memory.h"
#include "ctr_open_files.h"
#include "ctr_png.h"
#include "ctr_qoi.h"
#include "ctr_watchdog.h"
#include "gettime.h"
#include "image_decoder.h"
#include "log.h"

// With rounded packing, tiles whose visible pixels are at most this fraction
// partially transparent are stored as RGBA5551 with alpha thresholded at 50%.
// Tiles with more soft edges than that (anti-aliased art, glows) keep full
// RGBA8.
#define PARTIAL_ALPHA_TOLERANCE 0.02f
// A tile of mostly opaque art can still hold a glow or a translucent area,
// which the threshold turns into a hard-edged blob: DELTARUNE's
// spr_anime_bg_white_dot and Spamton NEO's electric orbs shared 512-pixel
// tiles with opaque sprites. Their partial pixels sit among other partial
// pixels, while an anti-aliased edge's have opaque or clear neighbours, so a
// tile with this many partial pixels whose four neighbours are partial too
// keeps RGBA8.
#define SOFT_AREA_MIN_PIXELS 64u

// The worker reads the data.win being run, and cache files go in a "cache"
// folder beside it, so each DELTARUNE chapter (chapterN_windows/data.win)
// keeps its own. Set by CtrPack_setDataWinPath. Exact tiles have their own
// folder beside it (CtrPack_setExact).
#define CACHE_DIR_NAME "cache"
#define EXACT_CACHE_DIR_NAME "cache_exact"
#define PACK_PATH_MAX 256
static char dataWinPath[PACK_PATH_MAX] = CTRPORT_GAME_DIR "/data.win";
static char cacheDir[PACK_PATH_MAX] = CTRPORT_GAME_DIR "/" CACHE_DIR_NAME;
static bool packExact;
static unsigned testRun;
static bool publishDirect = true;
// One thread writes cache files at a time, so the files being written don't
// interleave their clusters and each write goes at the card's pace.
static LightLock sdWriteLock;

// "DTX1". Bump PACK_VERSION whenever the conversion or the layout of packed
// pages changes, so old cache files are rebuilt instead of misread. Rounded
// packing makes version 3's files; version 4 holds every tile exactly.
#define PACK_MAGIC 0x31585444u
#define PACK_VERSION (packExact ? 4u : 3u)
// Cache files are a few MB at most; anything bigger is not ours.
#define PACK_MAX_FILE_SIZE (64u * 1024u * 1024u)

// The worker leaves this much heap free for the game: enough for the game
// thread to load a page from its image meanwhile (a 2048x2048 page's image of
// up to a few MB, a 2 MB strip of decoded rows, and for BZip2 pages 3.7 MB of
// BZip2 state). An eighth of the heap, within these bounds, so that small
// heaps (the Old 3DS) still pack.
#define WORKER_RESERVE_MIN (4u * 1024u * 1024u)
#define WORKER_RESERVE_MAX (8u * 1024u * 1024u)
// CtrLz_compress's match tables.
#define LZ_TABLE_BYTES (192u * 1024u)
// A packed page gathered in RAM is written to its file at most this much per
// write (its pieces, CTR_PACKED_PIECE_BYTES, are usually smaller).
#define WRITE_PIECE_BYTES (1024u * 1024u)
// Pages gathered whole are written through a buffer of this size, aligned
// to this (see writeStaged).
#define WRITE_STAGE_BYTES (256u * 1024u)
#define WRITE_STAGE_ALIGN 0x1000u
#define WORKER_STACK_SIZE (64 * 1024)
#define WORKER_PRIORITY 0x3F
// Pages that didn't fit in RAM are tried again after this long, doubling up
// to the maximum while they keep not fitting.
#define WORKER_RETRY_MIN_MS 250u
#define WORKER_RETRY_MAX_MS 8000u
#define WORKER_NAP_SLICE_MS 50u
// Foreground packing (nothing else running) uses this many threads at most:
// one per core the app can run on.
#define WORKER_MAX_THREADS 3
// The share of the system core borrowed for foreground packing; the .cia's
// exheader allows up to 89% (cia/o3ds.rsf), as Luma3DS does a .3dsx.
#define SYSCORE_PERCENT 80u
// The lowest share the limit goes back to: once raised it can't return to 0.
#define SYSCORE_PERCENT_MIN 5u
// In the foreground, pages that didn't fit in RAM are tried again this soon:
// another thread finishing a page is what frees the memory.
#define FOREGROUND_RETRY_MS 50u
// In the foreground, work buffers come from linear memory first (nearly all
// free before the game's textures load); this much of it is left alone.
#define FOREGROUND_LINEAR_MARGIN (2u * 1024u * 1024u)
// In the foreground, a page that runs out of memory this many times with no
// other page being packed won't fit before the game starts, so it is left to
// the background packing while the game runs.
#define FOREGROUND_MAX_SOLO_FAILURES 2u
// Written once every page has a cache file, so later runs know without
// opening each one.
#define COMPLETE_MAGIC 0x45544458u // "XDTE"

extern u32 __ctru_heap_size;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t dataWinSize;
    uint32_t gameId;
    uint64_t timestamp;
    uint32_t blobOffset;
    uint32_t blobSize;
    uint16_t width, height;
    uint16_t tileW, tileH;
    uint16_t tilesX, tilesY;
    uint32_t payloadSize;   // bytes after the header: the tile table, then each tile's LZ data in order
    uint32_t tableChecksum; // of the tile table
} PackHeader;

typedef struct {
    uint32_t format; // CtrTileFormat
    uint32_t size;   // of the LZ data; 0 for empty tiles
    uint32_t checksum;
} PackTile;

// RGBA8 channel values rounded to 5 bits and shifted into place for RGBA5551.
static uint16_t red5551[256];
static uint16_t green5551[256];
static uint16_t blue5551[256];
// Channel values the GPU's expansion from 5 bits (bit 0) and from 6 bits
// (bit 1) gives back exactly; filled by CtrPack_init.
static uint8_t exactBits[256];

void CtrPack_setDataWinPath(const char* path) {
    // Paths that don't fit would point the worker at the wrong file, so keep
    // the previous ones (the worker then refuses to run: the size won't match).
    if (path == nullptr || strlen(path) >= sizeof(dataWinPath)) return;
    const char* slash = strrchr(path, '/');
    int dirLength = slash != nullptr ? (int) (slash - path) : 0;
    char testName[48];
    const char* name = packExact ? EXACT_CACHE_DIR_NAME : CACHE_DIR_NAME;
    if (testRun > 0) {
        snprintf(testName, sizeof(testName), "%s_lt_%u", name, testRun);
        name = testName;
    }
    char dir[PACK_PATH_MAX];
    int written = dirLength > 0 ? snprintf(dir, sizeof(dir), "%.*s/%s", dirLength, path, name) : snprintf(dir, sizeof(dir), "%s", name);
    if (0 > written || written >= (int) sizeof(dir)) return;
    memcpy(dataWinPath, path, strlen(path) + 1);
    memcpy(cacheDir, dir, (size_t) written + 1);
}

void CtrPack_setExact(bool exact) {
    exact = exact && CTR_PACK_EXACT_AVAILABLE;
    if (exact == packExact) return;
    packExact = exact;
    // The same data.win, in the other folder.
    char path[PACK_PATH_MAX];
    memcpy(path, dataWinPath, sizeof(path));
    CtrPack_setDataWinPath(path);
}

bool CtrPack_lossy(void) {
    return !packExact;
}

void CtrPack_setTestRun(unsigned run, bool direct) {
    testRun = run;
    publishDirect = direct;
    char path[PACK_PATH_MAX];
    memcpy(path, dataWinPath, sizeof(path));
    CtrPack_setDataWinPath(path);
}

// ===[ Conversion ]===

void CtrPack_init(void) {
    static bool lockReady = false;
    if (!lockReady) {
        LightLock_Init(&sdWriteLock);
        lockReady = true;
    }
    CtrQoi_init();
    for (int32_t v = 0; 32 > v; v++) exactBits[(v << 3) | (v >> 2)] |= 1u;
    for (int32_t v = 0; 64 > v; v++) exactBits[(v << 2) | (v >> 4)] |= 2u;
    for (int32_t v = 0; 256 > v; v++) {
        uint16_t bits = (uint16_t) ((v * 31 + 127) / 255);
        red5551[v] = (uint16_t) (bits << 11);
        green5551[v] = (uint16_t) (bits << 6);
        blue5551[v] = (uint16_t) (bits << 1);
    }
}

CtrPackKey CtrPack_keyFor(const DataWin* dataWin) {
    CtrPackKey key = { (uint32_t) dataWin->fileSize, dataWin->gen8.gameID, dataWin->gen8.timestamp };
    return key;
}

GPU_TEXCOLOR CtrPack_gpuFormat(CtrTileFormat format) {
    switch (format) {
        case CTR_TILE_RGBA8: return GPU_RGBA8;
        case CTR_TILE_RGB8: return GPU_RGB8;
        case CTR_TILE_RGB565: return GPU_RGB565;
        default: return GPU_RGBA5551;
    }
}

uint32_t CtrPack_texelBytes(CtrTileFormat format) {
    switch (format) {
        case CTR_TILE_RGBA8: return 4;
        case CTR_TILE_RGB8: return 3;
        case CTR_TILE_RGBA5551:
        case CTR_TILE_RGB565: return 2;
        default: return 0;
    }
}

static uint32_t gpuTexelBytes(GPU_TEXCOLOR fmt) {
    return fmt == GPU_RGBA8 ? 4 : fmt == GPU_RGB8 ? 3 : 2;
}

uint32_t CtrPack_tileBytes(const CtrPageLayout* layout, int32_t tile) {
    return (uint32_t) layout->tileW * (uint32_t) layout->tileH * CtrPack_texelBytes((CtrTileFormat) layout->formats[tile]);
}

static int32_t tileSizeFor(int32_t size) {
    int32_t p = 8;
    while (size > p && CTR_TILE_MAX > p) p <<= 1;
    return p;
}

void CtrPack_layoutGrid(CtrPageLayout* layout, int32_t width, int32_t height) {
    layout->width = width;
    layout->height = height;
    layout->tileW = tileSizeFor(width);
    layout->tileH = tileSizeFor(height);
    layout->tilesX = (width + layout->tileW - 1) / layout->tileW;
    layout->tilesY = (height + layout->tileH - 1) / layout->tileH;
}

// Pixel rows of tile row ty that are inside the page.
static int32_t rowHeight(const CtrPageLayout* layout, int32_t ty) {
    int32_t y0 = ty * layout->tileH;
    return layout->height - y0 < layout->tileH ? layout->height - y0 : layout->tileH;
}

static inline bool isPartialAlpha(uint8_t a) {
    return 254 > (uint8_t) (a - 1);
}

static inline uint32_t load32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// Counts the RGBA pixel at px for chooseFormat. inner: its four neighbours
// (stride bytes per row) are inside the tile; neighbours outside it are not
// looked at.
static inline void countPixel(const uint8_t* px, bool inner, ptrdiff_t stride, uint32_t* visible, uint32_t* partial, uint32_t* softArea) {
    uint8_t a = px[3];
    if (a == 0) return;
    (*visible)++;
    if (a == 255) return;
    (*partial)++;
    if (inner && isPartialAlpha(px[3 - 4]) && isPartialAlpha(px[3 + 4]) && isPartialAlpha(px[3 - stride]) && isPartialAlpha(px[3 + stride])) {
        (*softArea)++;
    }
}

// The smallest format that holds the tile's texels exactly (see
// CtrTileFormat). A tile short of its full size (at the page's edge) is
// never opaque: the texels past the edge are transparent.
static CtrTileFormat chooseExactFormat(const uint8_t* rgba, int32_t pageW, int32_t x0, int32_t y0, int32_t w, int32_t h, bool full) {
    bool visible = false;
    bool opaque = full;
    bool binary = true; // alpha 0 or 255 throughout
    uint32_t r5 = 3, g5 = 3, b5 = 3;
    for (int32_t y = 0; h > y; y++) {
        const uint8_t* src = rgba + ((size_t) (y0 + y) * (size_t) pageW + (size_t) x0) * 4;
        for (int32_t x = 0; w > x; x++, src += 4) {
            uint8_t a = src[3];
            visible = visible || a != 0;
            opaque = opaque && a == 255;
            binary = binary && (a == 0 || a == 255);
            r5 &= exactBits[src[0]];
            g5 &= exactBits[src[1]];
            b5 &= exactBits[src[2]];
        }
        // Nothing left to learn: neither 16-bit format nor RGB8 can hold it.
        if (visible && !opaque && !(binary && (r5 & g5 & b5 & 1u))) return CTR_TILE_RGBA8;
    }
    if (!visible) return CTR_TILE_EMPTY;
    if (binary && (r5 & g5 & b5 & 1u)) return CTR_TILE_RGBA5551;
    if (opaque) return (r5 & b5 & 1u) && (g5 & 2u) ? CTR_TILE_RGB565 : CTR_TILE_RGB8;
    return CTR_TILE_RGBA8;
}

static CtrTileFormat chooseFormat(const uint8_t* rgba, int32_t pageW, int32_t x0, int32_t y0, int32_t w, int32_t h) {
    uint32_t visible = 0;
    uint32_t partial = 0;
    uint32_t softArea = 0;
    ptrdiff_t stride = (ptrdiff_t) pageW * 4;
    for (int32_t y = 0; h > y; y++) {
        const uint8_t* src = rgba + ((size_t) (y0 + y) * (size_t) pageW + (size_t) x0) * 4;
        bool innerRow = 0 < y && h - 1 > y;
        int32_t x = 0;
        // Four pixels at a time: most groups are all opaque or all clear.
        for (; w - 4 >= x; x += 4, src += 16) {
            uint32_t p0 = load32(src);
            uint32_t p1 = load32(src + 4);
            uint32_t p2 = load32(src + 8);
            uint32_t p3 = load32(src + 12);
            if ((p0 & p1 & p2 & p3) >= 0xFF000000u) {
                visible += 4;
                continue;
            }
            if ((p0 | p1 | p2 | p3) < 0x01000000u) continue;
            for (int32_t i = 0; 4 > i; i++) {
                bool inner = innerRow && 0 < x + i && w - 1 > x + i;
                countPixel(src + i * 4, inner, stride, &visible, &partial, &softArea);
            }
        }
        for (; w > x; x++, src += 4) countPixel(src, innerRow && 0 < x && w - 1 > x, stride, &visible, &partial, &softArea);
    }
    if (visible == 0) return CTR_TILE_EMPTY;
    if ((float) partial <= (float) visible * PARTIAL_ALPHA_TOLERANCE && SOFT_AREA_MIN_PIXELS > softArea) return CTR_TILE_RGBA5551;
    return CTR_TILE_RGBA8;
}

static void chooseRowFormats(CtrPageLayout* layout, int32_t ty, const uint8_t* rows) {
    int32_t h = rowHeight(layout, ty);
    for (int32_t tx = 0; layout->tilesX > tx; tx++) {
        int32_t x0 = tx * layout->tileW;
        int32_t w = layout->width - x0 < layout->tileW ? layout->width - x0 : layout->tileW;
        layout->formats[ty * layout->tilesX + tx] = packExact ? chooseExactFormat(rows, layout->width, x0, 0, w, h, w == layout->tileW && h == layout->tileH)
                                                              : chooseFormat(rows, layout->width, x0, 0, w, h);
    }
}

// Offset of pixel (x, y) inside an 8x8 tile: the PICA200 orders texels within
// a tile along a Z curve, interleaving the low three bits of x and y.
static inline uint32_t mortonOffset(uint32_t x, uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

// The Z curve keeps every aligned 2x2 quad together, in the order (0, 0),
// (1, 0), (0, 1), (1, 1). These are the offsets of the quads within an 8x8
// tile, split into the part from x ((x >> 1) & 3) and from y ((y >> 1) & 3).
static const uint8_t quadOffsetX[4] = { 0, 4, 16, 20 };
static const uint8_t quadOffsetY[4] = { 0, 8, 32, 40 };

// Source pixels are bytes R, G, B, A, read as one little-endian word.
static inline uint32_t toRgba8(uint32_t px) {
    return __builtin_bswap32(px);
}

static inline uint32_t toRgba5551(uint32_t px) {
    return red5551[px & 0xFF] | green5551[(px >> 8) & 0xFF] | blue5551[(px >> 16) & 0xFF] | (px >> 31);
}

// Only for colours RGB565 holds exactly, which dropping the low bits keeps.
static inline uint32_t toRgb565(uint32_t px) {
    return ((px & 0xF8u) << 8) | ((px >> 5) & 0x7E0u) | ((px >> 19) & 0x1Fu);
}

// Converts tile pixels [xs, xe) x [ys, ye) one at a time. Handles the edges
// that the 2x2 fast path in convertTile can't.
static void convertTileSlow(void* dst, GPU_TEXCOLOR fmt, int32_t texW, const uint8_t* rgba, int32_t pageW, int32_t x0, int32_t y0, int32_t xs,
                            int32_t xe, int32_t ys, int32_t ye) {
    size_t blockRowTexels = (size_t) (texW / 8) * 64;
    for (int32_t y = ys; ye > y; y++) {
        const uint8_t* src = rgba + ((size_t) (y0 + y) * (size_t) pageW + (size_t) (x0 + xs)) * 4;
        size_t rowBase = (size_t) (y >> 3) * blockRowTexels;
        uint32_t my = (uint32_t) y & 7;
        for (int32_t x = xs; xe > x; x++, src += 4) {
            size_t i = rowBase + (size_t) (x >> 3) * 64 + mortonOffset((uint32_t) x & 7, my);
            if (fmt == GPU_RGBA8) {
                ((uint32_t*) dst)[i] = toRgba8(load32(src));
            } else if (fmt == GPU_RGB8) {
                // Blue, green, red in memory.
                uint8_t* o = (uint8_t*) dst + i * 3;
                o[0] = src[2];
                o[1] = src[1];
                o[2] = src[0];
            } else if (fmt == GPU_RGB565) {
                ((uint16_t*) dst)[i] = (uint16_t) toRgb565(load32(src));
            } else {
                ((uint16_t*) dst)[i] = (uint16_t) toRgba5551(load32(src));
            }
        }
    }
}

// Converts the page region at (x0, y0) into tiled texels for a texW x texH
// texture of format fmt at dst. Texture row 0 (v = 0) holds the top row of
// the region, so GameMaker's top-down UVs apply unchanged. Texels past the
// page edge stay transparent.
//
// The bulk of the tile goes two rows at a time: each 2x2 quad of source
// pixels is four consecutive texels, so every 8x8 tile row pair is four
// 4-texel stores with no per-pixel address math.
static void convertTile(void* dst, GPU_TEXCOLOR fmt, int32_t texW, int32_t texH, const uint8_t* rgba, int32_t pageW, int32_t pageH, int32_t x0,
                        int32_t y0) {
    size_t blockRowTexels = (size_t) (texW / 8) * 64;
    int32_t w = pageW - x0 < texW ? pageW - x0 : texW;
    int32_t h = pageH - y0 < texH ? pageH - y0 : texH;
    int32_t fastW = w & ~7;
    int32_t fastH = h & ~1;
    if (texW > w || texH > h) memset(dst, 0, (size_t) texW * (size_t) texH * gpuTexelBytes(fmt));
    if (fmt == GPU_RGB8) {
        // Three bytes a texel suit no wide stores.
        convertTileSlow(dst, fmt, texW, rgba, pageW, x0, y0, 0, w, 0, h);
        return;
    }

    for (int32_t y = 0; fastH > y; y += 2) {
        const uint8_t* row0 = rgba + ((size_t) (y0 + y) * (size_t) pageW + (size_t) x0) * 4;
        const uint8_t* row1 = row0 + (size_t) pageW * 4;
        size_t rowBase = (size_t) (y >> 3) * blockRowTexels + quadOffsetY[(y >> 1) & 3];
        if (fmt == GPU_RGBA8) {
            uint32_t* d = (uint32_t*) dst + rowBase;
            for (int32_t x = 0; fastW > x; x += 8, row0 += 32, row1 += 32, d += 64) {
                for (int32_t q = 0; 4 > q; q++) {
                    uint32_t* o = d + quadOffsetX[q];
                    o[0] = toRgba8(load32(row0 + q * 8));
                    o[1] = toRgba8(load32(row0 + q * 8 + 4));
                    o[2] = toRgba8(load32(row1 + q * 8));
                    o[3] = toRgba8(load32(row1 + q * 8 + 4));
                }
            }
        } else if (fmt == GPU_RGB565) {
            uint32_t* d = (uint32_t*) ((uint16_t*) dst + rowBase);
            for (int32_t x = 0; fastW > x; x += 8, row0 += 32, row1 += 32, d += 32) {
                for (int32_t q = 0; 4 > q; q++) {
                    uint32_t* o = d + quadOffsetX[q] / 2;
                    o[0] = toRgb565(load32(row0 + q * 8)) | (toRgb565(load32(row0 + q * 8 + 4)) << 16);
                    o[1] = toRgb565(load32(row1 + q * 8)) | (toRgb565(load32(row1 + q * 8 + 4)) << 16);
                }
            }
        } else {
            // Two texels per 32-bit store; texel offsets here are always even.
            uint32_t* d = (uint32_t*) ((uint16_t*) dst + rowBase);
            for (int32_t x = 0; fastW > x; x += 8, row0 += 32, row1 += 32, d += 32) {
                for (int32_t q = 0; 4 > q; q++) {
                    uint32_t* o = d + quadOffsetX[q] / 2;
                    o[0] = toRgba5551(load32(row0 + q * 8)) | (toRgba5551(load32(row0 + q * 8 + 4)) << 16);
                    o[1] = toRgba5551(load32(row1 + q * 8)) | (toRgba5551(load32(row1 + q * 8 + 4)) << 16);
                }
            }
        }
    }
    if (w > fastW) convertTileSlow(dst, fmt, texW, rgba, pageW, x0, y0, fastW, w, 0, h);
    if (h > fastH) convertTileSlow(dst, fmt, texW, rgba, pageW, x0, y0, 0, fastW, fastH, h);
}

void CtrPack_convertTile(void* dst, const CtrPageLayout* layout, int32_t tx, int32_t ty, const uint8_t* rows) {
    convertTile(dst, CtrPack_gpuFormat((CtrTileFormat) layout->formats[ty * layout->tilesX + tx]), layout->tileW, layout->tileH, rows, layout->width, rowHeight(layout, ty),
                tx * layout->tileW, 0);
}

// ===[ Decoding ]===

static size_t heapFree(void) {
    struct mallinfo mi = mallinfo();
    return __ctru_heap_size > (u32) mi.uordblks ? __ctru_heap_size - (u32) mi.uordblks : 0;
}

bool CtrPack_imageInfo(const uint8_t* head, size_t size, int32_t* width, int32_t* height, size_t* decodeBytes) {
    if (CtrPng_info(head, size, width, height)) {
        // One strip of tileH rows, plus two rows and the inflate buffer.
        size_t stride = (size_t) *width * 4;
        *decodeBytes = (size_t) tileSizeFor(*height) * stride + 2 * stride + CtrInflate_workBytes() + 4 * 1024;
        return true;
    }
    size_t work = 0;
    if (CtrQoi_info(head, size, width, height, &work)) {
        if (0 >= *width || 0 >= *height || *width > 4096 || *height > 4096) return false;
        // One strip of tileH rows, plus the BZip2 state for 2zoq pages.
        *decodeBytes = (size_t) tileSizeFor(*height) * (size_t) *width * 4 + work;
        return true;
    }
    if (24 <= size && memcmp(head, "\x89PNG", 4) == 0) {
        // Another kind of PNG, which stb_image decodes whole: the inflated
        // image, then the pixels.
        uint32_t w = ((uint32_t) head[16] << 24) | ((uint32_t) head[17] << 16) | ((uint32_t) head[18] << 8) | head[19];
        uint32_t h = ((uint32_t) head[20] << 24) | ((uint32_t) head[21] << 16) | ((uint32_t) head[22] << 8) | head[23];
        if (w == 0 || h == 0 || w > 4096 || h > 4096) return false;
        *width = (int32_t) w;
        *height = (int32_t) h;
        *decodeBytes = (size_t) w * h * 9;
        return true;
    }
    return false;
}

typedef struct {
    CtrPageLayout* layout;
    int32_t ty;
    CtrPackRowFn fn;
    void* user;
} RowStream;

static bool takeStrip(void* user, const uint8_t* rows, int32_t y, int32_t count) {
    (void) y;
    (void) count;
    RowStream* stream = user;
    chooseRowFormats(stream->layout, stream->ty, rows);
    return stream->fn(stream->user, stream->layout, stream->ty++, rows);
}

CtrDecodeStatus CtrPack_decodeRows(const uint8_t* blob, size_t size, bool gm2022_5, bool urgent, CtrPageLayout* layout, CtrPackRowFn fn,
                                   void* user) {
    int32_t width = 0;
    int32_t height = 0;
    size_t decodeBytes = 0;
    if (!CtrPack_imageInfo(blob, size, &width, &height, &decodeBytes)) return CTR_DECODE_BAD_DATA;
    CtrPack_layoutGrid(layout, width, height);
    if (layout->tilesX * layout->tilesY > CTR_PAGE_MAX_TILES) return CTR_DECODE_BAD_DATA;

    RowStream stream = { layout, 0, fn, user };
    if (CtrPng_info(blob, size, &width, &height)) {
        switch (CtrPng_decode(blob, size, layout->tileH, takeStrip, &stream)) {
            case CTR_PNG_OK: return CTR_DECODE_OK;
            case CTR_PNG_NO_MEMORY: return CTR_DECODE_NO_MEMORY;
            case CTR_PNG_STOPPED: return CTR_DECODE_STOPPED;
            default: return CTR_DECODE_BAD_DATA;
        }
    }
    size_t work = 0;
    if (CtrQoi_info(blob, size, &width, &height, &work)) {
        switch (CtrQoi_decode(blob, size, gm2022_5, layout->tileH, urgent, takeStrip, &stream)) {
            case CTR_QOI_OK: return CTR_DECODE_OK;
            case CTR_QOI_NO_MEMORY:
            case CTR_QOI_YIELDED: return CTR_DECODE_NO_MEMORY;
            case CTR_QOI_STOPPED: return CTR_DECODE_STOPPED;
            default: return CTR_DECODE_BAD_DATA;
        }
    }

    // Other PNGs decode whole. Running out of memory would look like bad
    // data from here, so check first.
    if (decodeBytes > heapFree()) return CTR_DECODE_NO_MEMORY;
    int decodedW = 0;
    int decodedH = 0;
    uint8_t* rgba = ImageDecoder_decodeToRgba(blob, size, gm2022_5, &decodedW, &decodedH);
    if (rgba == nullptr || decodedW != width || decodedH != height) {
        free(rgba);
        return CTR_DECODE_BAD_DATA;
    }
    CtrDecodeStatus status = CTR_DECODE_OK;
    for (int32_t ty = 0; layout->tilesY > ty; ty++) {
        const uint8_t* rows = rgba + (size_t) ty * (size_t) layout->tileH * (size_t) width * 4;
        chooseRowFormats(layout, ty, rows);
        if (!fn(user, layout, ty, rows)) {
            status = CTR_DECODE_STOPPED;
            break;
        }
    }
    free(rgba);
    return status;
}

// ===[ Packed pages ]===

static uint32_t checksum(const uint8_t* p, size_t size) {
    uint32_t h = 2166136261u;
    size_t i = 0;
    for (; size >= i + 4; i += 4) h = (h ^ load32(p + i)) * 16777619u;
    for (; size > i; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

bool CtrPack_parseHeader(const uint8_t* data, size_t size, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize, CtrPackView* out) {
    PackHeader header;
    if (sizeof(header) > size) return false;
    memcpy(&header, data, sizeof(header));
    if (header.magic != PACK_MAGIC || header.version != PACK_VERSION) return false;
    if (header.dataWinSize != key->dataWinSize || header.gameId != key->gameId || header.timestamp != key->timestamp) return false;
    if (header.blobOffset != blobOffset || header.blobSize != blobSize) return false;
    if (header.payloadSize > PACK_MAX_FILE_SIZE) return false;

    CtrPageLayout* layout = &out->layout;
    if (header.width == 0 || header.height == 0) return false;
    CtrPack_layoutGrid(layout, header.width, header.height);
    if (layout->tileW != header.tileW || layout->tileH != header.tileH || layout->tilesX != header.tilesX || layout->tilesY != header.tilesY) {
        return false;
    }
    int32_t tileCount = layout->tilesX * layout->tilesY;
    if (tileCount > CTR_PAGE_MAX_TILES) return false;
    size_t tableSize = (size_t) tileCount * sizeof(PackTile);
    if (tableSize > header.payloadSize || sizeof(header) + tableSize > size) return false;

    const uint8_t* table = data + sizeof(header);
    if (checksum(table, tableSize) != header.tableChecksum) return false;
    uint32_t offset = (uint32_t) (sizeof(header) + tableSize);
    uint32_t remaining = header.payloadSize - (uint32_t) tableSize;
    for (int32_t i = 0; tileCount > i; i++) {
        PackTile tile;
        memcpy(&tile, table + (size_t) i * sizeof(PackTile), sizeof(tile));
        if (tile.format == CTR_TILE_EMPTY) {
            if (tile.size != 0) return false;
        } else if (tile.format >= CTR_TILE_FORMAT_COUNT || (!packExact && tile.format != CTR_TILE_RGBA5551 && tile.format != CTR_TILE_RGBA8)) {
            return false;
        } else if (tile.size == 0) {
            return false;
        }
        if (tile.size > remaining) return false;
        layout->formats[i] = (uint8_t) tile.format;
        out->tileOffset[i] = offset;
        out->tileSize[i] = tile.size;
        out->tileChecksum[i] = tile.checksum;
        offset += tile.size;
        remaining -= tile.size;
    }
    out->totalSize = offset;
    return remaining == 0;
}

bool CtrPack_checkTile(const uint8_t* data, uint32_t size, uint32_t sum) {
    return checksum(data, size) == sum;
}

static void pathFor(char* buf, size_t bufSize, uint32_t pageId, const char* extension) {
    snprintf(buf, bufSize, "%s/page%03u.%s", cacheDir, (unsigned) pageId, extension);
}

FILE* CtrPack_openFile(uint32_t pageId, bool* busy) {
    char path[PACK_PATH_MAX + 16];
    pathFor(path, sizeof(path), pageId, "dtx");
    CtrOpenFiles_beginCacheOpen();
    errno = 0;
    FILE* f = fopen(path, "rb");
    if (busy != nullptr) *busy = f == nullptr && !CtrOpenFiles_missing(errno);
    CtrOpenFiles_endCacheOpen();
    // Unbuffered: newlib then reads straight into the caller's buffer with one
    // read, instead of refilling its own buffer over and over. On the SD card
    // every read has a fixed cost on top of the bytes.
    if (f != nullptr) setvbuf(f, nullptr, _IONBF, 0);
    return f;
}

long CtrPack_fileSize(FILE* f) {
    long size = -1;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) return -1;
    return size;
}

// ===[ Packed pages in RAM ]===

// A CtrPacked and its tables in one block, the pieces not yet set.
static CtrPacked* allocPacked(uint32_t count, uint32_t size) {
    CtrPacked* packed = malloc(sizeof(CtrPacked) + (size_t) count * (sizeof(uint32_t) + sizeof(uint8_t*)));
    if (packed == nullptr) return nullptr;
    packed->size = size;
    packed->pieceCount = count;
    packed->starts = (uint32_t*) (packed + 1);
    packed->pieces = (uint8_t**) (packed->starts + count);
    return packed;
}

CtrPacked* CtrPacked_new(const CtrPackView* view) {
    // A new piece starts at a tile that would take the current one past the
    // size, unless the tile is the first in it.
    uint32_t starts[CTR_PAGE_MAX_TILES + 1];
    uint32_t count = 1;
    starts[0] = 0;
    int32_t tiles = view->layout.tilesX * view->layout.tilesY;
    for (int32_t i = 0; tiles > i && CTR_PAGE_MAX_TILES > i; i++) {
        uint32_t at = view->tileOffset[i];
        if (view->tileSize[i] == 0 || starts[count - 1] >= at) continue;
        if (at + view->tileSize[i] - starts[count - 1] > CTR_PACKED_PIECE_BYTES) starts[count++] = at;
    }
    CtrPacked* packed = allocPacked(count, view->totalSize);
    if (packed == nullptr) return nullptr;
    memcpy(packed->starts, starts, count * sizeof(uint32_t));
    for (uint32_t i = 0; count > i; i++) packed->pieces[i] = nullptr;
    for (uint32_t i = 0; count > i; i++) {
        packed->pieces[i] = malloc(CtrPacked_pieceSize(packed, i));
        if (packed->pieces[i] == nullptr) {
            CtrPacked_free(packed);
            return nullptr;
        }
    }
    return packed;
}

CtrPacked* CtrPacked_adopt(uint8_t* const* pieces, const uint32_t* starts, uint32_t count, uint32_t size) {
    CtrPacked* packed = allocPacked(count, size);
    if (packed == nullptr) return nullptr;
    memcpy(packed->starts, starts, count * sizeof(uint32_t));
    memcpy(packed->pieces, pieces, count * sizeof(uint8_t*));
    return packed;
}

void CtrPacked_free(CtrPacked* packed) {
    if (packed == nullptr) return;
    for (uint32_t i = 0; packed->pieceCount > i; i++) free(packed->pieces[i]);
    free(packed);
}

uint32_t CtrPacked_pieceSize(const CtrPacked* packed, uint32_t i) {
    uint32_t end = packed->pieceCount > i + 1 ? packed->starts[i + 1] : packed->size;
    return end - packed->starts[i];
}

const uint8_t* CtrPacked_at(const CtrPacked* packed, uint32_t offset) {
    uint32_t lo = 0;
    uint32_t hi = packed->pieceCount - 1;
    while (hi > lo) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (packed->starts[mid] > offset) hi = mid - 1;
        else lo = mid;
    }
    return packed->pieces[lo] + (offset - packed->starts[lo]);
}

bool CtrPacked_read(CtrPacked* packed, FILE* f, uint32_t chunk) {
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    for (uint32_t i = 0; packed->pieceCount > i; i++) {
        uint32_t size = CtrPacked_pieceSize(packed, i);
        for (uint32_t at = 0; size > at;) {
            uint32_t piece = size - at < chunk ? size - at : chunk;
            if (fread(packed->pieces[i] + at, 1, piece, f) != piece) return false;
            at += piece;
        }
    }
    return true;
}

CtrPacked* CtrPack_readFile(FILE* f, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize) {
    long size = CtrPack_fileSize(f);
    if (0 >= size || (unsigned long) size > PACK_MAX_FILE_SIZE) return nullptr;
    uint8_t* head = malloc(CTR_PACK_HEADER_MAX);
    CtrPackView* view = malloc(sizeof(CtrPackView));
    CtrPacked* packed = nullptr;
    if (head != nullptr && view != nullptr) {
        size_t headSize = (size_t) size < CTR_PACK_HEADER_MAX ? (size_t) size : CTR_PACK_HEADER_MAX;
        if (fread(head, 1, headSize, f) == headSize && CtrPack_parseHeader(head, headSize, key, blobOffset, blobSize, view) &&
            view->totalSize == (uint32_t) size) {
            packed = CtrPacked_new(view);
        }
    }
    free(head);
    free(view);
    if (packed != nullptr && !CtrPacked_read(packed, f, packed->size)) {
        CtrPacked_free(packed);
        packed = nullptr;
    }
    return packed;
}

// Carries PACK_VERSION, so after a bump the prepare screen runs again and
// rebuilds the cache files instead of trusting ones in the old format. It
// carries CTR_TILE_MAX too: builds that cut pages into different tiles (New
// 3DS builds before both took 256) refuse each other's files, so such a
// cache folder is prepared again before the game starts rather than decoded
// page by page in the middle of it.
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t pageCount;
    uint32_t tileMax;
    CtrPackKey key;
} CompleteMarker;

static void markerPath(char* buf, size_t bufSize) {
    snprintf(buf, bufSize, "%s/complete", cacheDir);
}

bool CtrPack_isComplete(const CtrPackKey* key, uint32_t pageCount) {
    char path[PACK_PATH_MAX + 16];
    markerPath(path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (f == nullptr) return false;
    CompleteMarker marker;
    bool read = fread(&marker, 1, sizeof(marker), f) == sizeof(marker);
    fclose(f);
    return read && marker.magic == COMPLETE_MAGIC && marker.version == PACK_VERSION && marker.pageCount == pageCount &&
           marker.tileMax == CTR_TILE_MAX && memcmp(&marker.key, key, sizeof(*key)) == 0;
}

static void writeCompleteMarker(const CtrPackKey* key, uint32_t pageCount) {
    char path[PACK_PATH_MAX + 16];
    markerPath(path, sizeof(path));
    CompleteMarker marker;
    memset(&marker, 0, sizeof(marker));
    marker.magic = COMPLETE_MAGIC;
    marker.version = PACK_VERSION;
    marker.pageCount = pageCount;
    marker.tileMax = CTR_TILE_MAX;
    marker.key = *key;
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return;
    bool written = fwrite(&marker, 1, sizeof(marker), f) == sizeof(marker);
    if (fclose(f) != 0 || !written) remove(path);
}

void CtrPack_cachePath(char* buf, size_t bufSize, const char* name) {
    snprintf(buf, bufSize, "%s/%s", cacheDir, name);
}

void CtrPack_makeCacheDir(void) {
    mkdir(cacheDir, 0777);
}

void CtrPack_removeFile(uint32_t pageId) {
    char path[PACK_PATH_MAX + 16];
    pathFor(path, sizeof(path), pageId, "dtx");
    remove(path);
    markerPath(path, sizeof(path));
    remove(path);
}

void CtrPack_forgetCache(uint32_t pageCount) {
    uint32_t removed = 0;
    char path[PACK_PATH_MAX + 16];
    for (uint32_t i = 0; pageCount > i; i++) {
        pathFor(path, sizeof(path), i, "dtx");
        if (remove(path) == 0) removed++;
    }
    markerPath(path, sizeof(path));
    remove(path);
    logInfo("Textures: repack_textures: removed %u cache files from %s\n", (unsigned) removed, cacheDir);
}

// ===[ SD card write test ]===
//
// sd_write_test=1 writes a test file in the cache folder several ways before
// the textures are prepared, and logs how fast each went, to tell on a
// console whether the card itself or the way the files are written makes
// cache files slow to write.

#define SD_TEST_BYTES (1024u * 1024u)

typedef enum {
    SD_TEST_SIZED = 1,     // ftruncate to the full size first
    SD_TEST_LINEAR = 2,    // from linear memory instead of the heap
    SD_TEST_UNALIGNED = 4, // calls start at odd offsets of the file and the buffer
    SD_TEST_INPLACE = 8,   // rewrite the file the previous test left
} SdTestFlags;

static void sdTest(const char* name, const char* path, uint8_t* heapBuf, uint8_t* linearBuf, uint32_t call, uint32_t flags) {
    uint8_t* buf = (flags & SD_TEST_LINEAR) != 0 ? linearBuf : heapBuf;
    if (buf == nullptr) {
        logInfo("SD test %-34s skipped (no buffer)\n", name);
        return;
    }
    if ((flags & SD_TEST_INPLACE) == 0) remove(path);
    uint64_t t0 = nowNanos();
    FILE* f = fopen(path, (flags & SD_TEST_INPLACE) != 0 ? "r+b" : "wb");
    if (f == nullptr) {
        logInfo("SD test %-34s couldn't open the file\n", name);
        return;
    }
    setvbuf(f, nullptr, _IONBF, 0);
    uint64_t t1 = nowNanos();
    bool ok = (flags & SD_TEST_SIZED) == 0 || ftruncate(fileno(f), (off_t) SD_TEST_BYTES) == 0;
    uint64_t t2 = nowNanos();
    uint64_t callMin = UINT64_MAX, callMax = 0;
    uint32_t calls = 0;
    // Unaligned: a first call of an odd length, then the rest from an odd address.
    uint32_t at = 0;
    if ((flags & SD_TEST_UNALIGNED) != 0) {
        uint32_t first = call - 1001u;
        ok = ok && fwrite(buf, 1, first, f) == first;
        at = first;
        calls++;
    }
    while (ok && SD_TEST_BYTES > at) {
        uint32_t n = SD_TEST_BYTES - at < call ? SD_TEST_BYTES - at : call;
        uint64_t s = nowNanos();
        ok = fwrite(buf + at, 1, n, f) == n;
        uint64_t d = nowNanos() - s;
        if (callMin > d) callMin = d;
        if (d > callMax) callMax = d;
        at += n;
        calls++;
    }
    uint64_t t3 = nowNanos();
    ok = ok && fsync(fileno(f)) == 0;
    uint64_t t4 = nowNanos();
    ok = fclose(f) == 0 && ok;
    uint64_t t5 = nowNanos();
    double total = (double) (t5 - t0) / 1e6;
    logInfo("SD test %-34s %s %.2f MB/s: %u calls (%.0f-%.0f ms each); open %.0f, size %.0f, data %.0f, flush %.0f, close %.0f ms\n", name,
            ok ? "ok" : "FAILED", total > 0 ? (double) SD_TEST_BYTES / (1024.0 * 1024.0) / (total / 1000.0) : 0.0, (unsigned) calls,
            callMin == UINT64_MAX ? 0.0 : (double) callMin / 1e6, (double) callMax / 1e6, (double) (t1 - t0) / 1e6, (double) (t2 - t1) / 1e6,
            (double) (t3 - t2) / 1e6, (double) (t4 - t3) / 1e6, (double) (t5 - t4) / 1e6);
    CtrLog_flush();
}

void CtrPack_sdWriteTest(void) {
    mkdir(cacheDir, 0777);
    char path[PACK_PATH_MAX + 16];
    CtrPack_cachePath(path, sizeof(path), "sdtest.tmp");
    uint8_t* heapBuf = memalign(WRITE_STAGE_ALIGN, SD_TEST_BYTES);
    uint8_t* linearBuf = linearAlloc(SD_TEST_BYTES);
    // Something like packed tiles: not all zeros, not compressible to nothing.
    uint32_t x = 0x12345678u;
    for (uint32_t i = 0; SD_TEST_BYTES > i; i++) {
        x = x * 1664525u + 1013904223u;
        uint8_t v = (uint8_t) (x >> 24);
        if (heapBuf != nullptr) heapBuf[i] = v;
        if (linearBuf != nullptr) linearBuf[i] = v;
    }
    if (linearBuf != nullptr) GSPGPU_FlushDataCache(linearBuf, SD_TEST_BYTES);
    logInfo("SD test: writing %u KB to %s each way (heap buffer %s, linear %s)\n", (unsigned) (SD_TEST_BYTES / 1024), path,
            heapBuf != nullptr ? "ok" : "none", linearBuf != nullptr ? "ok" : "none");
    CtrLog_flush();
    for (int32_t round = 1; 2 >= round; round++) {
        logInfo("SD test round %d\n", (int) round);
        sdTest("sized, 256 KB calls, aligned", path, heapBuf, linearBuf, 256u * 1024u, SD_TEST_SIZED);
        sdTest("sized, 256 KB calls, unaligned", path, heapBuf, linearBuf, 256u * 1024u, SD_TEST_SIZED | SD_TEST_UNALIGNED);
        sdTest("sized, 256 KB calls, from linear", path, heapBuf, linearBuf, 256u * 1024u, SD_TEST_SIZED | SD_TEST_LINEAR);
        sdTest("sized, one 1 MB call", path, heapBuf, linearBuf, SD_TEST_BYTES, SD_TEST_SIZED);
        sdTest("sized, 32 KB calls", path, heapBuf, linearBuf, 32u * 1024u, SD_TEST_SIZED);
        sdTest("growing, 256 KB calls", path, heapBuf, linearBuf, 256u * 1024u, 0);
        sdTest("rewritten in place, 256 KB calls", path, heapBuf, linearBuf, 256u * 1024u, SD_TEST_INPLACE);
    }
    // Reading it back, for comparison.
    FILE* f = fopen(path, "rb");
    if (f != nullptr && heapBuf != nullptr) {
        setvbuf(f, nullptr, _IONBF, 0);
        uint64_t s = nowNanos();
        bool ok = fread(heapBuf, 1, SD_TEST_BYTES, f) == SD_TEST_BYTES;
        double ms = (double) (nowNanos() - s) / 1e6;
        logInfo("SD test read back in one call: %s %.2f MB/s (%.0f ms)\n", ok ? "ok" : "FAILED",
                ms > 0 ? (double) SD_TEST_BYTES / (1024.0 * 1024.0) / (ms / 1000.0) : 0.0, ms);
    }
    if (f != nullptr) fclose(f);
    remove(path);
    free(heapBuf);
    if (linearBuf != nullptr) linearFree(linearBuf);
    CtrLog_flush();
}

// Builds a packed page from a page decoded in tile rows: converts and
// compresses each tile as its row arrives and gathers the packed page in RAM,
// then writes it to the SD card whole. On the console, a file that grows a
// little at a time writes very slowly (a 4 MB page took three minutes, a tile
// at a time with three threads at it), while a file sized up front and
// written in a few big pieces doesn't. Only if the heap runs short is the
// page streamed to the temporary file a tile at a time instead.
typedef struct {
    const volatile bool* quit;
    char tempPath[PACK_PATH_MAX + 16];
    FILE* file;       // the temporary file, once the page is streamed to it
    uint8_t* texels;  // one converted tile
    uint8_t* scratch; // its compressed form
    // The packed page so far, in pieces as a CtrPacked keeps it (none once
    // streamed), and how much the last piece can hold.
    uint8_t* pieces[CTR_PAGE_MAX_TILES + 1];
    uint32_t pieceStarts[CTR_PAGE_MAX_TILES + 1];
    uint32_t pieceCount;
    size_t lastCapacity;
    size_t reserve;   // heap the copy must leave free
    size_t size;      // bytes so far, header and table included
    PackTile table[CTR_PAGE_MAX_TILES];
    bool started;
    bool noMemory;
    bool writeFailed;
    uint64_t packNanos;     // time spent converting, compressing and writing tiles
    uint64_t convertNanos;
    uint64_t compressNanos;
    uint64_t writeNanos;    // gathering and writing tiles, header and table, closing and renaming the file
    // Where the SD card time went (see CtrPackWriteStats).
    uint32_t writeCalls;
    uint64_t waitNanos, openNanos, sizeNanos, dataNanos, flushNanos, closeNanos, renameNanos;
    uint64_t callMinNanos, callMaxNanos; // the quickest and slowest write call
    bool staged;                         // written through an aligned staging buffer (writeStaged)
    CtrCachePublishStats publication;
} PackBuilder;

static void lockSdWrite(PackBuilder* b) {
    uint64_t start = nowNanos();
    LightLock_Lock(&sdWriteLock);
    b->waitNanos += nowNanos() - start;
}

static void dropCopy(PackBuilder* b) {
    for (uint32_t i = 0; b->pieceCount > i; i++) free(b->pieces[i]);
    b->pieceCount = 0;
    b->lastCapacity = 0;
}

static size_t pieceUsed(const PackBuilder* b, uint32_t i) {
    return (i + 1 < b->pieceCount ? b->pieceStarts[i + 1] : b->size) - b->pieceStarts[i];
}

// Hands back the unused end of the last piece.
static void trimLastPiece(PackBuilder* b) {
    if (b->pieceCount == 0) return;
    uint32_t last = b->pieceCount - 1;
    size_t used = pieceUsed(b, last);
    if (used == 0 || used == b->lastCapacity) return;
    uint8_t* shrunk = realloc(b->pieces[last], used);
    if (shrunk != nullptr) b->pieces[last] = shrunk;
    b->lastCapacity = used;
}

// One write call, timed.
static bool timedWrite(PackBuilder* b, FILE* f, const uint8_t* data, size_t size) {
    uint64_t start = nowNanos();
    bool ok = fwrite(data, 1, size, f) == size;
    uint64_t nanos = nowNanos() - start;
    if (b->writeCalls == 0 || b->callMinNanos > nanos) b->callMinNanos = nanos;
    if (nanos > b->callMaxNanos) b->callMaxNanos = nanos;
    b->writeCalls++;
    return ok;
}

// Writes the copy's pieces to f, at most `chunk` bytes per write.
static bool writePieces(PackBuilder* b, FILE* f, size_t chunk) {
    bool ok = true;
    for (uint32_t i = 0; ok && b->pieceCount > i; i++) {
        size_t used = pieceUsed(b, i);
        for (size_t at = 0; ok && used > at; at += chunk) {
            size_t piece = used - at < chunk ? used - at : chunk;
            ok = timedWrite(b, f, b->pieces[i] + at, piece);
        }
    }
    return ok;
}

// Writes the copy's pieces to f through a page-aligned buffer, in calls of
// WRITE_STAGE_BYTES (the last one shorter), so that every call starts on a
// sector boundary of the file from an aligned address. The pieces end where
// their last tile did, so writing them as they are started most calls
// mid-sector from an unaligned address. False without writing anything if
// the buffer can't be had (*fallback is set then).
static bool writeStaged(PackBuilder* b, FILE* f, bool* fallback) {
    *fallback = false;
    uint8_t* stage = WRITE_STAGE_BYTES + b->reserve <= heapFree() ? memalign(WRITE_STAGE_ALIGN, WRITE_STAGE_BYTES) : nullptr;
    if (stage == nullptr) {
        *fallback = true;
        return false;
    }
    b->staged = true;
    bool ok = true;
    size_t filled = 0;
    for (uint32_t i = 0; ok && b->pieceCount > i; i++) {
        size_t used = pieceUsed(b, i);
        for (size_t at = 0; ok && used > at;) {
            size_t take = used - at < WRITE_STAGE_BYTES - filled ? used - at : WRITE_STAGE_BYTES - filled;
            memcpy(stage + filled, b->pieces[i] + at, take);
            filled += take;
            at += take;
            if (filled == WRITE_STAGE_BYTES) {
                ok = timedWrite(b, f, stage, filled);
                filled = 0;
            }
        }
    }
    if (ok && filled > 0) ok = timedWrite(b, f, stage, filled);
    free(stage);
    return ok;
}

// Frees everything and removes the temporary file, unless it was renamed into place.
static void freeBuilder(PackBuilder* b) {
    free(b->texels);
    free(b->scratch);
    dropCopy(b);
    b->texels = nullptr;
    b->scratch = nullptr;
    if (b->file != nullptr) {
        fclose(b->file);
        b->file = nullptr;
        remove(b->tempPath);
    }
}

// The heap can't hold the rest of the page: writes what there is to the
// temporary file, which takes the rest a tile at a time.
static bool startStreaming(PackBuilder* b) {
    lockSdWrite(b);
    uint64_t t0 = nowNanos();
    b->file = fopen(b->tempPath, "wb");
    uint64_t t1 = nowNanos();
    bool ok = b->file != nullptr;
    if (ok) {
        setvbuf(b->file, nullptr, _IONBF, 0);
        ok = writePieces(b, b->file, WRITE_PIECE_BYTES);
    }
    uint64_t t2 = nowNanos();
    LightLock_Unlock(&sdWriteLock);
    b->openNanos += t1 - t0;
    b->dataNanos += t2 - t1;
    dropCopy(b);
    if (!ok) b->writeFailed = true;
    return ok;
}

// Adds a header, table or tile: whole to the last piece, or to a new one.
static bool appendBytes(PackBuilder* b, const void* data, size_t size) {
    if (b->file == nullptr && (b->pieceCount == 0 || pieceUsed(b, b->pieceCount - 1) + size > b->lastCapacity)) {
        size_t capacity = size > CTR_PACKED_PIECE_BYTES ? size : CTR_PACKED_PIECE_BYTES;
        uint8_t* piece = b->pieceCount > CTR_PAGE_MAX_TILES || capacity + b->reserve > heapFree() ? nullptr : malloc(capacity);
        if (piece != nullptr) {
            trimLastPiece(b);
            b->pieces[b->pieceCount] = piece;
            b->pieceStarts[b->pieceCount] = (uint32_t) b->size;
            b->pieceCount++;
            b->lastCapacity = capacity;
        } else if (!startStreaming(b)) {
            return false;
        }
    }
    if (b->file != nullptr) {
        lockSdWrite(b);
        uint64_t start = nowNanos();
        bool written = timedWrite(b, b->file, data, size);
        b->dataNanos += nowNanos() - start;
        LightLock_Unlock(&sdWriteLock);
        if (!written) {
            b->writeFailed = true;
            return false;
        }
    } else {
        uint32_t last = b->pieceCount - 1;
        memcpy(b->pieces[last] + (b->size - b->pieceStarts[last]), data, size);
    }
    b->size += size;
    return true;
}

static bool startBuilder(PackBuilder* b, const CtrPageLayout* layout) {
    b->started = true;
    size_t maxTile = (size_t) layout->tileW * (size_t) layout->tileH * 4;
    b->texels = malloc(maxTile);
    b->scratch = malloc(CtrLz_bound(maxTile));
    if (b->texels == nullptr || b->scratch == nullptr) {
        b->noMemory = true;
        return false;
    }
    // The header and tile table go at the start once the tiles are known;
    // hold their place with zeros until then.
    PackHeader header = { 0 };
    size_t tableSize = (size_t) (layout->tilesX * layout->tilesY) * sizeof(PackTile);
    return appendBytes(b, &header, sizeof(header)) && appendBytes(b, b->table, tableSize);
}

static bool packRow(void* user, const CtrPageLayout* layout, int32_t ty, const uint8_t* rows) {
    PackBuilder* b = user;
    if (!b->started && !startBuilder(b, layout)) return false;
    uint64_t start = nowNanos();
    bool ok = true;
    for (int32_t tx = 0; layout->tilesX > tx; tx++) {
        // A row of tiles takes a while to pack: stop between tiles if the
        // game thread waits for the decoding workspace (retried later).
        if (*b->quit || CtrQoi_yieldWanted()) {
            ok = false;
            break;
        }
        int32_t i = ty * layout->tilesX + tx;
        b->table[i].format = layout->formats[i];
        if (layout->formats[i] == CTR_TILE_EMPTY) continue;
        uint64_t t0 = nowNanos();
        CtrPack_convertTile(b->texels, layout, tx, ty, rows);
        uint64_t t1 = nowNanos();
        uint32_t texelBytes = CtrPack_texelBytes((CtrTileFormat) layout->formats[i]);
        size_t compressed = CtrLz_compress(b->texels, CtrPack_tileBytes(layout, i), b->scratch, texelBytes);
        uint64_t t2 = nowNanos();
        b->convertNanos += t1 - t0;
        b->compressNanos += t2 - t1;
        if (compressed == 0) {
            ok = false;
            break;
        }
        b->table[i].size = (uint32_t) compressed;
        b->table[i].checksum = checksum(b->scratch, compressed);
        bool appended = appendBytes(b, b->scratch, compressed);
        b->writeNanos += nowNanos() - t2;
        if (!appended) {
            ok = false;
            break;
        }
    }
    b->packNanos += nowNanos() - start;
    return ok;
}

// Writes a page gathered whole to the temporary file: sized first, so the
// card allocates it in one go, then written a piece at a time, flushed and
// closed. The caller holds sdWriteLock.
static bool writeWhole(PackBuilder* b) {
    uint64_t t0 = nowNanos();
    FILE* f = fopen(b->tempPath, "wb");
    uint64_t t1 = nowNanos();
    if (f == nullptr) {
        b->openNanos += t1 - t0;
        return false;
    }
    setvbuf(f, nullptr, _IONBF, 0);
    bool ok = ftruncate(fileno(f), (off_t) b->size) == 0;
    uint64_t t2 = nowNanos();
    bool fallback = false;
    if (ok) ok = writeStaged(b, f, &fallback) || (fallback && writePieces(b, f, WRITE_PIECE_BYTES));
    uint64_t t3 = nowNanos();
    ok = ok && fsync(fileno(f)) == 0;
    uint64_t t4 = nowNanos();
    ok = fclose(f) == 0 && ok;
    uint64_t t5 = nowNanos();
    b->openNanos += t1 - t0;
    b->sizeNanos += t2 - t1;
    b->dataNanos += t3 - t2;
    b->flushNanos += t4 - t3;
    b->closeNanos += t5 - t4;
    return ok;
}

// The header of a packed page, cleared first. PackHeader has 4 bytes of tail padding (after tableChecksum, for the
// alignment of timestamp) that an initializer leaves unset: the console wrote whatever its stack held there, so
// the same page gave different files. Nothing reads them; zeroed, a page's file is the same on every run and
// matches the PC's (tools/texprep). Late text, so buildPage (which inlines finishBuilder) keeps its slot.
#ifdef __3DS__
#define TEXPREP_LATE __attribute__((noinline, section(".text.late.texprep")))
#else
#define TEXPREP_LATE
#endif
static TEXPREP_LATE void fillHeader(PackHeader* header, const PackBuilder* b, const CtrPackKey* key, uint32_t blobOffset,
                                    uint32_t blobSize, const CtrPageLayout* layout, size_t tableSize) {
    memset(header, 0, sizeof(*header));
    header->magic = PACK_MAGIC;
    header->version = PACK_VERSION;
    header->dataWinSize = key->dataWinSize;
    header->gameId = key->gameId;
    header->timestamp = key->timestamp;
    header->blobOffset = blobOffset;
    header->blobSize = blobSize;
    header->width = (uint16_t) layout->width;
    header->height = (uint16_t) layout->height;
    header->tileW = (uint16_t) layout->tileW;
    header->tileH = (uint16_t) layout->tileH;
    header->tilesX = (uint16_t) layout->tilesX;
    header->tilesY = (uint16_t) layout->tilesY;
    header->payloadSize = (uint32_t) (b->size - sizeof(PackHeader));
    header->tableChecksum = checksum((const uint8_t*) b->table, tableSize);
}

// Writes the header and tile table of a packed page whose tiles have all been
// added, and renames the temporary file into place, so a cache file is either
// complete or absent, even if the console is switched off mid-write.
static bool finishBuilder(PackBuilder* b, uint32_t pageId, const CtrPackKey* key, uint32_t blobOffset, uint32_t blobSize,
                          const CtrPageLayout* layout) {
    size_t tableSize = (size_t) (layout->tilesX * layout->tilesY) * sizeof(PackTile);
    PackHeader header;
    fillHeader(&header, b, key, blobOffset, blobSize, layout, tableSize);
    lockSdWrite(b);
    bool ok;
    if (b->file == nullptr) {
        // The first piece holds both: they were added to it first.
        memcpy(b->pieces[0], &header, sizeof(header));
        memcpy(b->pieces[0] + sizeof(header), b->table, tableSize);
        ok = writeWhole(b);
    } else {
        uint64_t t0 = nowNanos();
        ok = fseek(b->file, 0, SEEK_SET) == 0 && fwrite(&header, 1, sizeof(header), b->file) == sizeof(header) &&
             fwrite(b->table, 1, tableSize, b->file) == tableSize;
        b->writeCalls += 2;
        uint64_t t1 = nowNanos();
        ok = ok && fsync(fileno(b->file)) == 0;
        uint64_t t2 = nowNanos();
        ok = fclose(b->file) == 0 && ok;
        b->file = nullptr;
        uint64_t t3 = nowNanos();
        b->dataNanos += t1 - t0;
        b->flushNanos += t2 - t1;
        b->closeNanos += t3 - t2;
    }
    uint64_t renameStart = nowNanos();
    char path[PACK_PATH_MAX + 16];
    pathFor(path, sizeof(path), pageId, "dtx");
    if (ok) ok = CtrCachePublish_run(b->tempPath, path, &b->publication);
    if (!ok) remove(b->tempPath);
    b->renameNanos += nowNanos() - renameStart;
    LightLock_Unlock(&sdWriteLock);
    return ok;
}

// ===[ Background worker ]===

typedef enum {
    PAGE_NONE,    // nothing to pack (an external page)
    PAGE_PENDING,
    PAGE_CLAIMED, // a thread is packing it
    PAGE_DONE,    // packed, found packed, or not packable
} PageState;

typedef struct {
    CtrPackWorker* worker;
    Thread thread;
    FILE* dataWin; // the thread's own handle; the game thread's FILE is never touched here
    int8_t index;
    int8_t askedCore; // as threadCreate was asked (-2: the app's default core)
    int8_t core;      // where the thread ran (svcGetProcessorID), -1 until it starts
    // For the log, written by the thread itself (read once it has finished).
    uint32_t pagesBuilt;
    uint64_t startNanos, endNanos;
    uint64_t busyNanos;       // packing pages (packPage, found or built)
    uint64_t memoryWaitNanos; // sleeping until memory frees up
} WorkerThread;

struct CtrPackWorker {
    WorkerThread threads[WORKER_MAX_THREADS];
    uint32_t threadCount;
    LightLock lock;
    CtrPackKey key;
    bool gm2022_5;
    uint32_t pageCount;
    uint32_t* blobOffsets; // snapshot of the TXTR entries; size 0 = nothing to pack
    uint32_t* blobSizes;
    uint32_t* order;       // the order to pack pages in
    bool foreground;
    // Guarded by lock:
    uint8_t* pageStates;    // PageState
    uint8_t* soloFailures;  // foreground: times the page ran out of memory with no other page being packed
    uint32_t pagesTotal;    // pages to pack
    uint32_t pagesLeft;     // pending or claimed
    bool anyFailed;         // a page couldn't be packed
    // Estimated memory of the pages being packed (see admitPage).
    uint32_t inFlightPages;
    size_t inFlightHeap;
    size_t inFlightWork;
    uint32_t memoryWaits;   // pages put back for want of memory
    uint32_t deferred;      // foreground: pages left to the background packing
    CtrPackResult* results; // at most one per page
    uint32_t resultCount;
    uint32_t resultsTaken;
    size_t reserve;         // heap left free for the game
    volatile bool quit;
    volatile bool keepPacked;
    volatile int32_t hurryPage; // -1 for none
    bool borrowedSyscore;       // foreground: the app's system core time limit was raised
    u32 syscoreLimitToRestore;
    // How often a thread found lock taken, and the time such threads waited
    // in all. Updated by the thread that then holds lock.
    uint32_t lockContended;
    uint64_t lockWaitNanos;
    uint64_t startNanos;
};

static void lockWorker(CtrPackWorker* worker) {
    if (LightLock_TryLock(&worker->lock) == 0) return;
    uint64_t start = nowNanos();
    LightLock_Lock(&worker->lock);
    worker->lockContended++;
    worker->lockWaitNanos += nowNanos() - start;
}

static void pushResult(CtrPackWorker* worker, const CtrPackResult* result) {
    lockWorker(worker);
    worker->results[worker->resultCount++] = *result;
    LightLock_Unlock(&worker->lock);
}

static bool readAt(FILE* f, uint32_t offset, void* dst, size_t size) {
    return fseek(f, (long) offset, SEEK_SET) == 0 && fread(dst, 1, size, f) == size;
}

typedef enum {
    PACK_DONE,            // packed, or found packed already
    PACK_NO_MEMORY,       // try again later
    PACK_NO_MEMORY_ALONE, // the same, with no other page holding memory
    PACK_FAILED,          // not packable; never retried
} PackStatus;

static size_t linearFreeForWork(void) {
    u32 space = linearSpaceFree();
    return space > FOREGROUND_LINEAR_MARGIN ? space - FOREGROUND_LINEAR_MARGIN : 0;
}

// Whether a page fits in memory next to the pages being packed, and if so
// counts it in until releasePage. heapBytes must come from the heap (tile
// buffers, compression tables); workBytes are the image and its decoding
// buffers, which in the foreground come from linear memory first. Free
// memory already excludes what the pages in flight have allocated, while
// their whole estimates are added on top, so this errs towards packing
// fewer pages at once; with the threads racing to allocate, it is what keeps
// them from running the heap out together. *alone says that no other page
// was being packed.
static bool admitPage(CtrPackWorker* worker, size_t heapBytes, size_t workBytes, bool* alone) {
    lockWorker(worker);
    size_t heap = heapFree();
    size_t heapNeed = heapBytes + worker->inFlightHeap + worker->reserve;
    size_t workNeed = workBytes + worker->inFlightWork;
    bool fits = worker->foreground ? heap >= heapNeed && heap + linearFreeForWork() >= heapNeed + workNeed : heap >= heapNeed + workNeed;
    *alone = worker->inFlightPages == 0;
    if (fits) {
        worker->inFlightPages++;
        worker->inFlightHeap += heapBytes;
        worker->inFlightWork += workBytes;
    }
    LightLock_Unlock(&worker->lock);
    return fits;
}

// Undoes admitPage once the page's memory is freed; *alone says that no
// other page was being packed.
static void releasePage(CtrPackWorker* worker, size_t heapBytes, size_t workBytes, bool* alone) {
    lockWorker(worker);
    *alone = worker->inFlightPages == 1;
    worker->inFlightPages--;
    worker->inFlightHeap -= heapBytes;
    worker->inFlightWork -= workBytes;
    LightLock_Unlock(&worker->lock);
}

// Looks for a valid cache file for the page and reports it if there is one.
// Only the header and tile table are checked here; each tile is checked
// against its checksum when it loads.
static bool findExisting(CtrPackWorker* worker, uint32_t pageId, bool* busy) {
    FILE* f = CtrPack_openFile(pageId, busy);
    if (f == nullptr) return false;
    CtrPackResult result = { .pageId = pageId };
    CtrPackView view;
    bool valid = false;
    long size = CtrPack_fileSize(f);
    if (worker->keepPacked && size > 0 && (size_t) size + worker->reserve <= heapFree()) {
        result.packed = CtrPack_readFile(f, &worker->key, worker->blobOffsets[pageId], worker->blobSizes[pageId]);
        valid = result.packed != nullptr;
        if (valid) result.size = result.packed->size;
    } else if (size > 0) {
        uint8_t head[CTR_PACK_HEADER_MAX];
        size_t headSize = (size_t) size < sizeof(head) ? (size_t) size : sizeof(head);
        valid = fread(head, 1, headSize, f) == headSize &&
                CtrPack_parseHeader(head, headSize, &worker->key, worker->blobOffsets[pageId], worker->blobSizes[pageId], &view) &&
                view.totalSize == (uint32_t) size;
        result.size = (size_t) size;
    }
    fclose(f);
    if (valid) pushResult(worker, &result);
    return valid;
}

// Reads, decodes and packs a page that admitPage let in.
static PackStatus buildPage(WorkerThread* self, uint32_t pageId, uint64_t start) {
    CtrPackWorker* worker = self->worker;
    char marker[PACK_PATH_MAX + 16];
    markerPath(marker, sizeof(marker));
    LightLock_Lock(&sdWriteLock);
    bool canPublish = CtrCachePublish_begin(marker, publishDirect);
    LightLock_Unlock(&sdWriteLock);
    if (!canPublish) return PACK_FAILED;
    FILE* dataWin = self->dataWin;
    uint32_t blobOffset = worker->blobOffsets[pageId];
    uint32_t blobSize = worker->blobSizes[pageId];
    uint8_t* blob = CtrMemory_allocWork(blobSize);
    if (blob == nullptr) return PACK_NO_MEMORY;
    if (!readAt(dataWin, blobOffset, blob, blobSize)) {
        CtrMemory_freeWork(blob);
        return PACK_FAILED;
    }
    uint64_t readNanos = nowNanos() - start;
    CtrPageLayout layout;
    PackBuilder builder = { .quit = &worker->quit, .reserve = worker->reserve };
    pathFor(builder.tempPath, sizeof(builder.tempPath), pageId, "tmp");
    CtrQoi_takeInflateNanos();
    CtrPng_takeInflateNanos();
    CtrDecodeStatus status = CtrPack_decodeRows(blob, blobSize, worker->gm2022_5, false, &layout, packRow, &builder);
    uint64_t inflateNanos = CtrQoi_takeInflateNanos() + CtrPng_takeInflateNanos();
    CtrMemory_freeWork(blob);
    if (status != CTR_DECODE_OK) {
        freeBuilder(&builder);
        if (status == CTR_DECODE_NO_MEMORY || builder.noMemory) return PACK_NO_MEMORY;
        // A full or missing SD card: the game runs on without cache files.
        if (builder.writeFailed) return PACK_FAILED;
        // Stopped by quit (packed on a later run) or for the game thread's
        // decode (tried again soon).
        return status == CTR_DECODE_STOPPED ? PACK_NO_MEMORY : PACK_FAILED;
    }
    uint64_t finishStart = nowNanos();
    bool written = finishBuilder(&builder, pageId, &worker->key, blobOffset, blobSize, &layout);
    uint64_t finishNanos = nowNanos() - finishStart;
    builder.packNanos += finishNanos;
    builder.writeNanos += finishNanos;
    CtrPackResult result = {
        .pageId = pageId,
        .built = true,
        .size = builder.size,
        .readMs = (float) ((double) readNanos / 1e6),
        .inflateMs = (float) ((double) inflateNanos / 1e6),
        .convertMs = (float) ((double) builder.convertNanos / 1e6),
        .compressMs = (float) ((double) builder.compressNanos / 1e6),
        .writeMs = (float) ((double) builder.writeNanos / 1e6),
        .io = {
            .calls = builder.writeCalls,
            .streamed = builder.pieceCount == 0,
            .staged = builder.staged,
            .callMinMs = (float) ((double) builder.callMinNanos / 1e6),
            .callMaxMs = (float) ((double) builder.callMaxNanos / 1e6),
            .waitMs = (float) ((double) builder.waitNanos / 1e6),
            .openMs = (float) ((double) builder.openNanos / 1e6),
            .sizeMs = (float) ((double) builder.sizeNanos / 1e6),
            .dataMs = (float) ((double) builder.dataNanos / 1e6),
            .flushMs = (float) ((double) builder.flushNanos / 1e6),
            .closeMs = (float) ((double) builder.closeNanos / 1e6),
            .renameMs = (float) ((double) builder.renameNanos / 1e6),
            .publication = builder.publication,
        },
        .thread = self->index,
        .core = (int8_t) svcGetProcessorID(),
    };
    self->pagesBuilt++;
    if (written && builder.pieceCount > 0 && worker->keepPacked) {
        trimLastPiece(&builder);
        result.packed = CtrPacked_adopt(builder.pieces, builder.pieceStarts, builder.pieceCount, (uint32_t) builder.size);
        if (result.packed != nullptr) builder.pieceCount = 0;
    }
    freeBuilder(&builder);
    if (!written) return PACK_FAILED;
    double totalMs = (double) (nowNanos() - start) / 1e6;
    result.packMs = (float) ((double) builder.packNanos / 1e6);
    result.decodeMs = (float) totalMs - result.packMs;
    pushResult(worker, &result);
    return PACK_DONE;
}

#if defined(__3DS__) && defined(NDEBUG)
// Retained slot: with the header code in fillHeader, buildPage is 212 B shorter (all four Release targets, GCC
// 16.1.0); these zero bytes in its own section keep everything after it at main's address. Never executed or
// referenced (tools/analysis/check_buildpage_tail.py).
__attribute__((naked, used, section(".text.buildPage"))) static void buildPageTail(void) {
    __asm__(".space 212");
}
#endif

static PackStatus packPage(WorkerThread* self, uint32_t pageId) {
    CtrPackWorker* worker = self->worker;
    FILE* dataWin = self->dataWin;
    uint32_t blobOffset = worker->blobOffsets[pageId];
    uint32_t blobSize = worker->blobSizes[pageId];
    uint64_t start = nowNanos();
    bool busy = false;
    if (findExisting(worker, pageId, &busy)) return PACK_DONE;
    // Too many files open to tell: tried again later, as when short of
    // memory, rather than packed again.
    if (busy) return PACK_NO_MEMORY;

    // Check memory from the image header before reading the whole image.
    uint8_t head[64];
    size_t headSize = blobSize < sizeof(head) ? blobSize : sizeof(head);
    int32_t w = 0;
    int32_t h = 0;
    size_t decodeBytes = 0;
    if (!readAt(dataWin, blobOffset, head, headSize)) return PACK_FAILED;
    if (!CtrPack_imageInfo(head, headSize, &w, &h, &decodeBytes)) return PACK_FAILED;
    // A tile of texels and its compression buffer and tables; the image and
    // its decoding. The copy kept in RAM gives way when memory runs short.
    size_t maxTile = (size_t) tileSizeFor(w) * (size_t) tileSizeFor(h) * 4;
    size_t heapBytes = maxTile + CtrLz_bound(maxTile) + LZ_TABLE_BYTES;
    size_t workBytes = (size_t) blobSize + decodeBytes;
    bool alone = false;
    if (!admitPage(worker, heapBytes, workBytes, &alone)) return alone ? PACK_NO_MEMORY_ALONE : PACK_NO_MEMORY;
    PackStatus status = buildPage(self, pageId, start);
    releasePage(worker, heapBytes, workBytes, &alone);
    return status == PACK_NO_MEMORY && alone ? PACK_NO_MEMORY_ALONE : status;
}

// Sleeps for up to ms milliseconds, waking early if the worker is stopped or
// asked to hurry.
static void napUnlessQuit(CtrPackWorker* worker, uint32_t ms) {
    for (uint32_t slept = 0; ms > slept && !worker->quit && 0 > worker->hurryPage; slept += WORKER_NAP_SLICE_MS) {
        svcSleepThread((int64_t) WORKER_NAP_SLICE_MS * 1000000);
    }
}

// Takes the page to pack next: the one the game asked for if it is still
// pending, else the first pending one from *next on. Returns -1 if none is.
static int32_t claimPage(CtrPackWorker* worker, uint32_t* next) {
    lockWorker(worker);
    int32_t page = -1;
    int32_t hurried = worker->hurryPage;
    if (0 <= hurried) {
        worker->hurryPage = -1;
        if (worker->pageCount > (uint32_t) hurried && worker->pageStates[hurried] == PAGE_PENDING) page = hurried;
    }
    while (0 > page && worker->pageCount > *next) {
        uint32_t i = worker->order[(*next)++];
        if (worker->pageStates[i] == PAGE_PENDING) page = (int32_t) i;
    }
    if (0 <= page) worker->pageStates[page] = PAGE_CLAIMED;
    LightLock_Unlock(&worker->lock);
    return page;
}

// Records how packing the page went, and returns that as it stands: a page
// left to the background packing counts as failed.
static PackStatus finishPage(CtrPackWorker* worker, uint32_t page, PackStatus status) {
    lockWorker(worker);
    bool complete = false;
    if (status == PACK_NO_MEMORY_ALONE && worker->foreground && !worker->quit && ++worker->soloFailures[page] >= FOREGROUND_MAX_SOLO_FAILURES) {
        // Nothing else will free memory for it before the game starts. It
        // counts as not packed, so the game's background packing tries again.
        worker->deferred++;
        status = PACK_FAILED;
    }
    if (status == PACK_NO_MEMORY || status == PACK_NO_MEMORY_ALONE) {
        worker->pageStates[page] = PAGE_PENDING;
        if (!worker->quit) worker->memoryWaits++;
    } else {
        worker->pageStates[page] = PAGE_DONE;
        worker->pagesLeft--;
        if (status == PACK_FAILED) worker->anyFailed = true;
        complete = worker->pagesLeft == 0 && !worker->anyFailed;
    }
    LightLock_Unlock(&worker->lock);
    if (complete) writeCompleteMarker(&worker->key, worker->pageCount);
    return status;
}

static void workerMain(void* arg) {
    WorkerThread* self = arg;
    CtrPackWorker* worker = self->worker;
    static const char* const names[WORKER_MAX_THREADS] = { "texture prepare 1", "texture prepare 2", "texture prepare 3" };
    CtrWatchdog_nameThread(names[self - worker->threads]);
    // Pages that don't fit in RAM now may fit once the game frees some, so
    // go over them again, less often the longer they keep not fitting.
    uint32_t waitMs = WORKER_RETRY_MIN_MS;
    CtrMemory_preferLinearWork(worker->foreground);
    self->core = (int8_t) svcGetProcessorID();
    self->startNanos = nowNanos();
    while (!worker->quit) {
        bool waiting = false;
        bool progress = false;
        uint32_t next = 0;
        while (!worker->quit) {
            int32_t page = claimPage(worker, &next);
            if (0 > page) break;
            CtrWatchdog_note("preparing a page");
            uint64_t pageStart = nowNanos();
            PackStatus status = finishPage(worker, (uint32_t) page, packPage(self, (uint32_t) page));
            self->busyNanos += nowNanos() - pageStart;
            if (status == PACK_NO_MEMORY || status == PACK_NO_MEMORY_ALONE) {
                waiting = true;
            } else {
                progress = progress || status == PACK_DONE;
            }
        }
        if (!waiting) break;
        uint64_t napStart = nowNanos();
        if (worker->foreground) {
            CtrWatchdog_note("waiting for memory");
            svcSleepThread((int64_t) FOREGROUND_RETRY_MS * 1000000);
        } else {
            if (progress) waitMs = WORKER_RETRY_MIN_MS;
            CtrWatchdog_note("waiting for memory");
            napUnlessQuit(worker, waitMs);
            waitMs = waitMs * 2 < WORKER_RETRY_MAX_MS ? waitMs * 2 : WORKER_RETRY_MAX_MS;
        }
        self->memoryWaitNanos += nowNanos() - napStart;
    }
    CtrWatchdog_note("finished");
    self->endNanos = nowNanos();
}

// Gives back the system core borrowed for foreground packing. The share the
// app holds there slows the system modules that run on that core, GSP and FS
// among them, even while no app thread uses it: left at 80% on a New 3DS
// (hw12), GPU frames took 30-60% longer and C3D_FrameEnd 0.9 ms instead of
// 0.13 for the rest of the session. A limit that was 0 (the New 3DS, and the
// Old 3DS before its audio raises it) can't be set again, so it goes to the
// lowest share the system takes instead.
static void restoreSyscoreLimit(u32 previous) {
    Result set = APT_SetAppCpuTimeLimit(previous);
    for (u32 percent = SYSCORE_PERCENT_MIN; R_FAILED(set) && SYSCORE_PERCENT > percent; percent += SYSCORE_PERCENT_MIN) {
        if (percent > previous) set = APT_SetAppCpuTimeLimit(percent);
    }
    u32 now = 0;
    APT_GetAppCpuTimeLimit(&now);
    logInfo("Textures: system core time limit back to %lu%% (was %lu%% before packing)\n", (unsigned long) now, (unsigned long) previous);
}

// The threads must have finished.
static void freeWorker(CtrPackWorker* worker) {
    CtrCachePublish_close();
    for (uint32_t i = 0; WORKER_MAX_THREADS > i; i++) {
        if (worker->threads[i].dataWin != nullptr) fclose(worker->threads[i].dataWin);
    }
    if (worker->borrowedSyscore) restoreSyscoreLimit(worker->syscoreLimitToRestore);
    free(worker->blobOffsets);
    free(worker->blobSizes);
    free(worker->order);
    free(worker->pageStates);
    free(worker->soloFailures);
    free(worker->results);
    free(worker);
}

// The game thread's FILE belongs to the game thread, so each worker thread
// opens data.win again, and makes sure it is the same file.
static FILE* openDataWin(const CtrPackKey* key) {
    FILE* f = fopen(dataWinPath, "rb");
    if (f == nullptr) return nullptr;
    if (CtrPack_fileSize(f) != (long) key->dataWinSize) {
        fclose(f);
        return nullptr;
    }
    return f;
}

static bool useSyscore = true;

void CtrPackWorker_setUseSyscore(bool use) {
    useSyscore = use;
}

// Starts worker thread i on `core`, or on the default core if that one can't
// have it.
static bool startThread(CtrPackWorker* worker, const CtrPackKey* key, uint32_t i, int core) {
    WorkerThread* t = &worker->threads[i];
    t->worker = worker;
    t->index = (int8_t) i;
    t->askedCore = (int8_t) core;
    t->core = -1;
    if (t->dataWin == nullptr) t->dataWin = openDataWin(key);
    if (t->dataWin == nullptr) return false;
    t->thread = threadCreate(workerMain, t, WORKER_STACK_SIZE, WORKER_PRIORITY, core, false);
    if (t->thread == nullptr && core != -2) {
        logInfo("Textures: no thread on core %d, using the default core\n", core);
        t->askedCore = -2;
        t->thread = threadCreate(workerMain, t, WORKER_STACK_SIZE, WORKER_PRIORITY, -2, false);
    }
    return t->thread != nullptr;
}

CtrPackWorker* CtrPackWorker_start(const DataWin* dataWin, const CtrPackKey* key, bool foreground) {
    CtrPackWorker* worker = calloc(1, sizeof(CtrPackWorker));
    if (worker == nullptr) return nullptr;
    worker->threads[0].dataWin = openDataWin(key);
    if (worker->threads[0].dataWin == nullptr) {
        freeWorker(worker);
        return nullptr;
    }
    mkdir(cacheDir, 0777);

    uint32_t count = dataWin->txtr.count;
    worker->blobOffsets = calloc(count > 0 ? count : 1, sizeof(uint32_t));
    worker->blobSizes = calloc(count > 0 ? count : 1, sizeof(uint32_t));
    worker->order = calloc(count > 0 ? count : 1, sizeof(uint32_t));
    worker->pageStates = calloc(count > 0 ? count : 1, sizeof(uint8_t));
    worker->soloFailures = calloc(count > 0 ? count : 1, sizeof(uint8_t));
    worker->results = calloc(count > 0 ? count : 1, sizeof(CtrPackResult));
    if (worker->blobOffsets == nullptr || worker->blobSizes == nullptr || worker->order == nullptr || worker->pageStates == nullptr ||
        worker->soloFailures == nullptr || worker->results == nullptr) {
        freeWorker(worker);
        return nullptr;
    }
    for (uint32_t i = 0; count > i; i++) {
        const Texture* t = &dataWin->txtr.textures[i];
        // Pages in external files are not cached.
        if (t->externalPath != nullptr || t->blobOffset == 0 || t->blobSize == 0) continue;
        worker->blobOffsets[i] = t->blobOffset;
        worker->blobSizes[i] = t->blobSize;
        worker->pageStates[i] = PAGE_PENDING;
        worker->pagesTotal++;
    }
    worker->pagesLeft = worker->pagesTotal;
    worker->pageCount = count;
    worker->foreground = foreground;
    for (uint32_t i = 0; count > i; i++) worker->order[i] = i;
    // In the foreground the biggest pages go first, so the threads don't end
    // with one of them still packing a big page while the others sit idle.
    // In the background pages go in data.win order, which is roughly the
    // order the game first draws them in.
    if (foreground) {
        for (uint32_t i = 1; count > i; i++) {
            uint32_t page = worker->order[i];
            uint32_t j = i;
            for (; j > 0 && worker->blobSizes[page] > worker->blobSizes[worker->order[j - 1]]; j--) worker->order[j] = worker->order[j - 1];
            worker->order[j] = page;
        }
    }
    worker->key = *key;
    worker->gm2022_5 = DataWin_isVersionAtLeast(dataWin, 2022, 5, 0, 0);
    // In the foreground nobody takes the packed pages; the game reads them
    // from the cache files once it runs.
    worker->keepPacked = !foreground;
    worker->hurryPage = -1;
    size_t reserve = __ctru_heap_size / 8;
    worker->reserve = reserve < WORKER_RESERVE_MIN ? WORKER_RESERVE_MIN : reserve > WORKER_RESERVE_MAX ? WORKER_RESERVE_MAX : reserve;
    LightLock_Init(&worker->lock);

    // New 3DS: the third core is the app's to use. Otherwise share the game's
    // core at the lowest priority, which only runs while the game thread
    // waits for the GPU and vblank. In the foreground the game's core is
    // free, so it packs too, and without the third core (an Old 3DS, or a
    // New 3DS in Old 3DS mode) so does most of the system core, borrowed
    // until the worker is freed. A New 3DS doesn't borrow it: its limit
    // can't go back to 0 (see restoreSyscoreLimit).
    bool isNew3ds = false;
    APT_CheckNew3DS(&isNew3ds);
    int cores[WORKER_MAX_THREADS] = { isNew3ds ? 2 : -2, -2, -2 };
    uint32_t wanted = 1;
    if (foreground) {
        if (isNew3ds) wanted = 2;
        logInfo("Textures: prepare_syscore=%d\n", useSyscore ? 1 : 0);
    }
    worker->startNanos = nowNanos();
    for (uint32_t i = 0; wanted > i && startThread(worker, key, i, cores[i]); i++) worker->threadCount++;
    bool thirdCore = worker->threadCount > 0 && worker->threads[0].askedCore == 2;
    if (foreground && useSyscore && !thirdCore && worker->threadCount == wanted) {
        u32 previous = 0;
        Result got = APT_GetAppCpuTimeLimit(&previous);
        Result set = R_SUCCEEDED(got) ? APT_SetAppCpuTimeLimit(SYSCORE_PERCENT) : got;
        u32 now = 0;
        Result check = APT_GetAppCpuTimeLimit(&now);
        if (R_SUCCEEDED(got) && R_SUCCEEDED(set)) {
            worker->syscoreLimitToRestore = previous;
            worker->borrowedSyscore = true;
            if (startThread(worker, key, wanted, 1)) worker->threadCount++;
        }
        logInfo("Textures: system core time limit %lu%% (was %lu%%; asked %u%%: get 0x%08lX, set 0x%08lX, check 0x%08lX)\n", (unsigned long) now,
                (unsigned long) previous, SYSCORE_PERCENT, (unsigned long) got, (unsigned long) set, (unsigned long) check);
    }
    if (worker->threadCount == 0) {
        freeWorker(worker);
        return nullptr;
    }
    return worker;
}

// Where each thread ran and how it spent its time, and the waits for locks.
// The threads must have finished.
static void logThreads(CtrPackWorker* worker) {
    uint64_t end = nowNanos();
    for (uint32_t i = 0; worker->threadCount > i; i++) {
        const WorkerThread* t = &worker->threads[i];
        double life = (double) ((t->endNanos > t->startNanos ? t->endNanos : end) - worker->startNanos) / 1e9;
        double busy = (double) t->busyNanos / 1e9;
        double memoryWait = (double) t->memoryWaitNanos / 1e9;
        double other = life - busy - memoryWait;
        logInfo("Textures: thread %u on core %d (asked %d): %u pages in %.1f s busy, %.1f s waiting for memory, %.1f s other, done at %.1f s\n",
                (unsigned) i, (int) t->core, (int) t->askedCore, (unsigned) t->pagesBuilt, busy, memoryWait, other > 0 ? other : 0.0, life);
    }
    uint32_t linearContended = 0;
    uint64_t linearWaitNanos = 0;
    CtrMemory_linearLockStats(&linearContended, &linearWaitNanos);
    logInfo("Textures: worker lock waited for %u times (%.1f ms); linear memory lock %u times (%.1f ms) since the start\n",
            (unsigned) worker->lockContended, (double) worker->lockWaitNanos / 1e6, (unsigned) linearContended, (double) linearWaitNanos / 1e6);
}

void CtrPackWorker_progress(CtrPackWorker* worker, uint32_t* done, uint32_t* total) {
    lockWorker(worker);
    *total = worker->pagesTotal;
    *done = worker->pagesTotal - worker->pagesLeft;
    LightLock_Unlock(&worker->lock);
}

void CtrPackWorker_memoryStats(CtrPackWorker* worker, uint32_t* waits, uint32_t* deferred) {
    lockWorker(worker);
    *waits = worker->memoryWaits;
    *deferred = worker->deferred;
    LightLock_Unlock(&worker->lock);
}

uint32_t CtrPackWorker_threadCount(const CtrPackWorker* worker) {
    return worker->threadCount;
}

bool CtrPackWorker_poll(CtrPackWorker* worker, CtrPackResult* out) {
    bool got = false;
    lockWorker(worker);
    if (worker->resultCount > worker->resultsTaken) {
        *out = worker->results[worker->resultsTaken++];
        got = true;
    }
    LightLock_Unlock(&worker->lock);
    return got;
}

void CtrPackWorker_hurry(CtrPackWorker* worker, uint32_t pageId) {
    if (worker != nullptr && worker->pageCount > pageId) worker->hurryPage = (int32_t) pageId;
}

void CtrPackWorker_setKeepPacked(CtrPackWorker* worker, bool keep) {
    worker->keepPacked = keep;
}

void CtrPackWorker_stop(CtrPackWorker* worker) {
    if (worker == nullptr) return;
    worker->quit = true;
    for (uint32_t i = 0; worker->threadCount > i; i++) {
        threadJoin(worker->threads[i].thread, U64_MAX);
        threadFree(worker->threads[i].thread);
    }
    if (worker->foreground) logThreads(worker);
    for (uint32_t i = worker->resultsTaken; worker->resultCount > i; i++) CtrPacked_free(worker->results[i].packed);
    freeWorker(worker);
}

// The name the build gives the RomFS files of the data.win the cache serves (romfs:/fonts/<name>.dtfv, .dtst, .dtsm,
// .dtsy): its folder's name without the "-3ds" that the per-game SD folders end in. The build names them after the
// folder as it sees the data (undertale, deltarune, chapter2_windows); on the card the game's own folder is
// sdmc:/3ds/undertale-3ds (or deltarune-3ds), and lab mode still runs from the old folder (z_sdmigrate.c).
// Every RomFS file named after the data.win takes its name from here. False when the path has no folder, or the name
// doesn't fit size.
bool CtrPack_romfsName(char* name, size_t size) __attribute__((noinline, section(".text.late.roomload")));
bool CtrPack_romfsName(char* name, size_t size) {
    const char* path = dataWinPath;
    const char* end = path != nullptr ? strrchr(path, '/') : nullptr;
    if (end == nullptr) return false;
    const char* start = end;
    while (start > path && start[-1] != '/') start--;
    size_t n = (size_t) (end - start);
    if (n > 4 && end[-4] == '-' && end[-3] == '3' && end[-2] == 'd' && end[-1] == 's') n -= 4;
    if (n == 0 || n >= size) return false;
    memcpy(name, start, n);
    name[n] = '\0';
    return true;
}

// roomload item 3 (A): puts a page next in the background worker's order, if the worker hasn't looked at it yet (read its
// cache file into RAM, or packed it). A thread takes pages from order[] at its pass's place, which never lies past the
// place after the last page claimed or done: claimPage stops right after a claim, and a hurried page, taken out of order,
// only moves that place further on. So pages from there on move only further back, and the page goes in there. A page
// put back for want of memory may lie before it; that pass then ends waiting and the next one starts from the beginning,
// so no page is skipped. Returns whether the page moved. Background worker only (one thread).
bool CtrPackWorker_putFirst(CtrPackWorker* worker, uint32_t pageId) __attribute__((noinline, section(".text.late.roomload")));
bool CtrPackWorker_putFirst(CtrPackWorker* worker, uint32_t pageId) {
    if (worker == nullptr || worker->foreground || pageId >= worker->pageCount) return false;
    LightLock_Lock(&worker->lock);
    bool moved = false;
    if (worker->pageStates[pageId] == PAGE_PENDING) {
        uint32_t from = 0;
        for (uint32_t i = 0; worker->pageCount > i; i++) {
            uint8_t state = worker->pageStates[worker->order[i]];
            if (state == PAGE_CLAIMED || state == PAGE_DONE) from = i + 1;
        }
        for (uint32_t i = from; worker->pageCount > i; i++) {
            if (worker->order[i] != pageId) continue;
            for (uint32_t j = i; j > from; j--) worker->order[j] = worker->order[j - 1];
            worker->order[from] = pageId;
            moved = i != from;
            break;
        }
    }
    LightLock_Unlock(&worker->lock);
    return moved;
}

#if defined(ENABLE_WAD16)
// utstall item 4 (UNDERTALE, z_utstall.c): where the background worker is with a page (0 not looked at yet, 1 being
// read or packed, 2 done, 3 done with its packed copy not taken yet; -1 without the background worker), and in
// *reserve the heap it leaves free when it reads a cache file whole into RAM (findExisting).
int32_t CtrPackWorker_utstallState(CtrPackWorker* worker, uint32_t pageId, uint32_t* reserve) __attribute__((noinline, section(".text.late.utstall")));
int32_t CtrPackWorker_utstallState(CtrPackWorker* worker, uint32_t pageId, uint32_t* reserve) {
    if (worker == nullptr || worker->foreground || pageId >= worker->pageCount) return -1;
    LightLock_Lock(&worker->lock);
    uint8_t state = worker->pageStates[pageId];
    bool waiting = false;
    for (uint32_t i = worker->resultsTaken; worker->resultCount > i; i++) {
        if (worker->results[i].pageId == pageId && worker->results[i].packed != nullptr) waiting = true;
    }
    LightLock_Unlock(&worker->lock);
    *reserve = (uint32_t) worker->reserve;
    return state == PAGE_PENDING ? 0 : state == PAGE_CLAIMED ? 1 : waiting ? 3 : 2;
}
#endif
