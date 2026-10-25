#include "ctr_png.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ctr_inflate.h"
#include "ctr_memory.h"
#include "gettime.h"

#define PNG_SIGNATURE_SIZE 8
#define CHUNK_OVERHEAD 12 // length, type, CRC
#define IHDR_SIZE 13
#define BYTES_PER_PIXEL 4
#define MAX_SIZE 16384

// Time this thread has spent inflating, for CtrPng_takeInflateNanos.
static __thread uint64_t inflateNanos;

typedef struct {
    const uint8_t* png;
    size_t pngSize;
    size_t offset;   // where to look for the next IDAT chunk
    CtrPngStatus status;
    uint64_t consumeNanos;
    int32_t width, height;
    size_t stride;   // bytes per row of pixels
    uint8_t* line;   // the scanline being inflated: filter type byte, then stride bytes
    size_t linePos;
    uint8_t* prev;   // the previous row, unfiltered (zeros above the first)
    uint8_t* strip;  // stripRows rows, unfiltered
    int32_t stripRows;
    int32_t stripFill;
    int32_t y;       // rows finished
    CtrPngStripFn fn;
    void* user;
} Decoder;

static uint32_t load32be(const uint8_t* p) {
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
}

bool CtrPng_info(const uint8_t* png, size_t size, int32_t* width, int32_t* height) {
    if (PNG_SIGNATURE_SIZE + CHUNK_OVERHEAD + IHDR_SIZE > size) return false;
    if (memcmp(png, "\x89PNG\r\n\x1a\n", PNG_SIGNATURE_SIZE) != 0) return false;
    const uint8_t* ihdr = png + PNG_SIGNATURE_SIZE;
    if (load32be(ihdr) != IHDR_SIZE || memcmp(ihdr + 4, "IHDR", 4) != 0) return false;
    const uint8_t* fields = ihdr + 8;
    uint32_t w = load32be(fields);
    uint32_t h = load32be(fields + 4);
    // Bit depth 8, color type 6 (RGBA), compression 0, filter method 0, no interlacing.
    if (fields[8] != 8 || fields[9] != 6 || fields[10] != 0 || fields[11] != 0 || fields[12] != 0) return false;
    if (w == 0 || h == 0 || w > MAX_SIZE || h > MAX_SIZE) return false;
    *width = (int32_t) w;
    *height = (int32_t) h;
    return true;
}

// The Paeth predictor (a left, b above, c above left), in stb_image's
// branch-free form, which is equivalent to the PNG specification's.
static inline uint8_t paeth(int32_t a, int32_t b, int32_t c) {
    int32_t thresh = c * 3 - (a + b);
    int32_t lo = a < b ? a : b;
    int32_t hi = a < b ? b : a;
    int32_t t0 = hi <= thresh ? lo : c;
    return (uint8_t) (thresh <= lo ? hi : t0);
}

// Undoes a scanline's filter. The leftmost pixel has no neighbor on the left,
// which the filters treat as zero.
static bool unfilter(uint8_t* restrict out, const uint8_t* restrict raw, const uint8_t* restrict prev, size_t stride, uint8_t filter) {
    const size_t bpp = BYTES_PER_PIXEL;
    switch (filter) {
        case 0:
            // GameMaker writes every row unfiltered. The scanline starts one
            // byte past a filter type byte, so it is rarely word aligned,
            // which turns memcpy into a byte loop; rows are whole pixels, so
            // copy a word at a time with unaligned loads.
            for (size_t i = 0; stride > i; i += 4) {
                uint32_t px;
                memcpy(&px, raw + i, 4);
                memcpy(out + i, &px, 4);
            }
            return true;
        case 1:
            memcpy(out, raw, bpp);
            for (size_t i = bpp; stride > i; i++) out[i] = (uint8_t) (raw[i] + out[i - bpp]);
            return true;
        case 2:
            for (size_t i = 0; stride > i; i++) out[i] = (uint8_t) (raw[i] + prev[i]);
            return true;
        case 3:
            for (size_t i = 0; bpp > i; i++) out[i] = (uint8_t) (raw[i] + (prev[i] >> 1));
            for (size_t i = bpp; stride > i; i++) out[i] = (uint8_t) (raw[i] + ((out[i - bpp] + prev[i]) >> 1));
            return true;
        case 4:
            for (size_t i = 0; bpp > i; i++) out[i] = (uint8_t) (raw[i] + prev[i]);
            for (size_t i = bpp; stride > i; i++) out[i] = (uint8_t) (raw[i] + paeth(out[i - bpp], prev[i], prev[i - bpp]));
            return true;
        default:
            return false;
    }
}

// Takes inflated bytes, finishing scanlines and handing over full strips.
static CtrPngStatus consume(Decoder* d, const uint8_t* data, size_t size) {
    size_t lineSize = 1 + d->stride;
    while (size > 0 && d->height > d->y) {
        // Scanlines that arrive whole are unfiltered where they are; only
        // those split between two batches of output are put together first.
        const uint8_t* line = data;
        if (d->linePos == 0 && size >= lineSize) {
            data += lineSize;
            size -= lineSize;
        } else {
            size_t n = lineSize - d->linePos < size ? lineSize - d->linePos : size;
            memcpy(d->line + d->linePos, data, n);
            d->linePos += n;
            data += n;
            size -= n;
            if (lineSize > d->linePos) break;
            d->linePos = 0;
            line = d->line;
        }

        uint8_t* row = d->strip + (size_t) d->stripFill * d->stride;
        const uint8_t* above = d->stripFill > 0 ? row - d->stride : d->prev;
        if (!unfilter(row, line + 1, above, d->stride, line[0])) return CTR_PNG_BAD_DATA;
        d->stripFill++;
        d->y++;
        if (d->stripFill == d->stripRows || d->y == d->height) {
            if (!d->fn(d->user, d->strip, d->y - d->stripFill, d->stripFill)) return CTR_PNG_STOPPED;
            memcpy(d->prev, row, d->stride);
            d->stripFill = 0;
        }
    }
    return CTR_PNG_OK;
}

// Finds the next IDAT chunk at or after *offset. IDAT chunks are consecutive,
// so only the first search skips other chunks.
static bool nextIdat(const uint8_t* png, size_t size, size_t* offset, const uint8_t** data, size_t* length, bool skipOthers) {
    while (size >= *offset + CHUNK_OVERHEAD) {
        uint32_t len = load32be(png + *offset);
        const uint8_t* type = png + *offset + 4;
        if (len > size - *offset - CHUNK_OVERHEAD) return false;
        size_t chunk = *offset;
        *offset += CHUNK_OVERHEAD + len;
        if (memcmp(type, "IDAT", 4) == 0) {
            *data = png + chunk + 8;
            *length = len;
            return true;
        }
        if (!skipOthers || memcmp(type, "IEND", 4) == 0) return false;
    }
    return false;
}

// The image data continues in the next IDAT chunk.
static bool nextInput(void* user, const uint8_t** data, size_t* length) {
    Decoder* d = user;
    return nextIdat(d->png, d->pngSize, &d->offset, data, length, false);
}

static bool takeOutput(void* user, const uint8_t* data, size_t size) {
    Decoder* d = user;
    uint64_t start = nowNanos();
    d->status = consume(d, data, size);
    d->consumeNanos += nowNanos() - start;
    return d->status == CTR_PNG_OK;
}

static CtrPngStatus inflateImage(Decoder* d, void* work) {
    d->offset = PNG_SIGNATURE_SIZE;
    const uint8_t* in = nullptr;
    size_t inSize = 0;
    if (!nextIdat(d->png, d->pngSize, &d->offset, &in, &inSize, true)) return CTR_PNG_BAD_DATA;

    // The zlib header, checked and skipped here; the Adler-32 checksum at
    // the end goes unchecked (stb_image ignores it too).
    if (2 > inSize) return CTR_PNG_UNSUPPORTED;
    uint8_t cmf = in[0];
    uint8_t flg = in[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((uint32_t) cmf * 256 + flg) % 31 != 0 || (flg & 0x20) != 0) return CTR_PNG_BAD_DATA;

    // Every scanline: its filter type byte, then the pixels.
    size_t want = (size_t) d->height * (1 + d->stride);
    d->status = CTR_PNG_OK;
    d->consumeNanos = 0;
    uint64_t start = nowNanos();
    CtrInflateStatus status = CtrInflate_decode(in + 2, inSize - 2, nextInput, d, want, takeOutput, d, work);
    inflateNanos += nowNanos() - start - d->consumeNanos;
    if (status == CTR_INFLATE_STOPPED) return d->status;
    // Otherwise the stream ended, or failed, before the last row.
    return status == CTR_INFLATE_OK && d->y == d->height ? CTR_PNG_OK : CTR_PNG_BAD_DATA;
}

CtrPngStatus CtrPng_decode(const uint8_t* png, size_t size, int32_t stripRows, CtrPngStripFn fn, void* user) {
    int32_t width = 0;
    int32_t height = 0;
    if (!CtrPng_info(png, size, &width, &height)) return CTR_PNG_UNSUPPORTED;
    if (0 >= stripRows) return CTR_PNG_UNSUPPORTED;
    if (stripRows > height) stripRows = height;

    Decoder* d = malloc(sizeof(Decoder));
    if (d == nullptr) return CTR_PNG_NO_MEMORY;
    d->width = width;
    d->height = height;
    d->stride = (size_t) width * BYTES_PER_PIXEL;
    d->line = malloc(1 + d->stride);
    d->linePos = 0;
    d->prev = calloc(1, d->stride);
    d->strip = CtrMemory_allocWork((size_t) stripRows * d->stride);
    d->stripRows = stripRows;
    d->stripFill = 0;
    d->y = 0;
    d->fn = fn;
    d->user = user;
    d->png = png;
    d->pngSize = size;
    void* work = malloc(CtrInflate_workBytes());

    CtrPngStatus status = CTR_PNG_NO_MEMORY;
    if (d->line != nullptr && d->prev != nullptr && d->strip != nullptr && work != nullptr) status = inflateImage(d, work);
    free(work);
    free(d->line);
    free(d->prev);
    if (d->strip != nullptr) CtrMemory_freeWork(d->strip);
    free(d);
    return status;
}

uint64_t CtrPng_takeInflateNanos(void) {
    uint64_t nanos = inflateNanos;
    inflateNanos = 0;
    return nanos;
}
