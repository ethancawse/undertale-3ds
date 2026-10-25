#include "ctr_qoi.h"

#include <3ds.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "ctr_bz2.h"
#include "ctr_memory.h"
#include "gettime.h"

#define QOI_HEADER_SIZE 12
#define BZ2_QOI_HEADER_SIZE_OLD 8
#define BZ2_QOI_HEADER_SIZE_NEW 12
#define MAX_SIZE 16384
// Inflated QOI bytes are taken from BZip2 this many at a time.
#define INFLATE_BUFFER_SIZE (64 * 1024)
// The longest QOI op: a colour op with all four channels.
#define MAX_OP_SIZE 5
// The BZip2 decoder (ctr_bz2.h) needs 4 bytes per byte of block, blocks
// being 100 KB times the level in the stream header, plus its tables.
#define BZ2_BYTES_PER_LEVEL (4u * 100000u)
#define BZ2_TABLE_BYTES (64u * 1024u)

// The shared workspace: a strip of a 2048-pixel-wide page as tall as a
// texture tile (CTR_TILE_MAX in ctr_texture_pack.h), the inflate buffer and
// BZip2's state at level 9.
#define WORK_STRIP_BYTES (2048u * 256u * 4u)
#define WORK_BZ2_BYTES (9u * BZ2_BYTES_PER_LEVEL + BZ2_TABLE_BYTES)
#define WORK_BYTES (WORK_STRIP_BYTES + INFLATE_BUFFER_SIZE + WORK_BZ2_BYTES)

// A decode needs several MB in a few large blocks, and the 3DS heap fragments
// over a long run (pages, blobs and game data coming and going) until blocks
// that large can't be had even with half the heap free. So one workspace is
// allocated on first use, while the heap is still in one piece, and kept
// until no more pages are waiting to be packed (see ctr_textures.c). One
// decode at a time uses it; the game thread, which is waiting for its page,
// takes it from the background worker, which stops its decode and retries.
static LightLock workLock;
static uint8_t* workspace;
static volatile bool yieldWanted;
// Time this thread has spent in BZip2, for CtrQoi_takeInflateNanos.
static __thread uint64_t inflateNanos;

typedef struct {
    const uint8_t* in;  // QOI bytes ready to decode
    const uint8_t* end;
    bool compressed;
    size_t left;        // compressed: QOI bytes of the stream not inflated yet
    bool streamEnded;   // compressed: BZip2 has delivered everything
    CtrBz2* bz;
    uint8_t* buffer;    // compressed: INFLATE_BUFFER_SIZE bytes
    uint32_t* strip;    // stripRows rows of pixels
    bool mayYield;      // stop if the game thread wants the workspace
    CtrQoiStatus error;
} Source;

static uint32_t load16le(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static uint32_t load32le(const uint8_t* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

// Where the BZip2 stream of a 2zoq page starts: GameMaker 2022.5 added the
// inflated length to the header. Trust the stream's "BZh" signature over the
// version when they disagree.
static size_t bz2HeaderSize(const uint8_t* blob, size_t size, bool gm2022_5) {
    size_t preferred = gm2022_5 ? BZ2_QOI_HEADER_SIZE_NEW : BZ2_QOI_HEADER_SIZE_OLD;
    size_t other = gm2022_5 ? BZ2_QOI_HEADER_SIZE_OLD : BZ2_QOI_HEADER_SIZE_NEW;
    if (size >= preferred + 4 && memcmp(blob + preferred, "BZh", 3) == 0) return preferred;
    if (size >= other + 4 && memcmp(blob + other, "BZh", 3) == 0) return other;
    return preferred;
}

bool CtrQoi_info(const uint8_t* blob, size_t size, int32_t* width, int32_t* height, size_t* workBytes) {
    if (8 > size) return false;
    bool plain = memcmp(blob, "fioq", 4) == 0;
    if (!plain && memcmp(blob, "2zoq", 4) != 0) return false;
    uint32_t w = load16le(blob + 4);
    uint32_t h = load16le(blob + 6);
    if (w == 0 || h == 0 || w > MAX_SIZE || h > MAX_SIZE) return false;
    *width = (int32_t) w;
    *height = (int32_t) h;
    *workBytes = 0;
    if (!plain) {
        // The level is the digit after "BZh"; assume the largest if it can't be read.
        uint32_t level = 9;
        size_t at = bz2HeaderSize(blob, size, true);
        if (size > at + 3 && blob[at + 3] >= '1' && blob[at + 3] <= '9') level = (uint32_t) (blob[at + 3] - '0');
        *workBytes = (size_t) level * BZ2_BYTES_PER_LEVEL + BZ2_TABLE_BYTES + INFLATE_BUFFER_SIZE;
    }
    return true;
}

// Makes at least `want` bytes available in [in, end), if the stream has that
// many left. Returns false on a BZip2 error (see error).
static bool fill(Source* s, size_t want) {
    size_t have = (size_t) (s->end - s->in);
    if (have >= want || !s->compressed || s->streamEnded || s->left == 0) return true;
    memmove(s->buffer, s->in, have);
    size_t space = INFLATE_BUFFER_SIZE - have;
    if (space > s->left) space = s->left;
    uint64_t start = nowNanos();
    CtrBz2Status status = CTR_BZ2_OK;
    size_t produced = CtrBz2_read(s->bz, s->buffer + have, space, &status);
    inflateNanos += nowNanos() - start;
    if (status == CTR_BZ2_BAD_DATA) {
        s->error = CTR_QOI_BAD_DATA;
        return false;
    }
    // The end of the stream, or of the input if it was cut short.
    if (status == CTR_BZ2_END) s->streamEnded = true;
    s->left -= produced;
    s->in = s->buffer;
    s->end = s->buffer + have + produced;
    return true;
}

static inline uint8_t signExtend(uint32_t value, int bits) {
    uint32_t mask = 1u << (bits - 1);
    return (uint8_t) ((value ^ mask) - mask);
}

// The pixel's slot in the colour index (UndertaleModTool's hash, not QOI's).
static inline uint32_t slotOf(uint32_t px) {
    px ^= px >> 16;
    px ^= px >> 8;
    return px & 63;
}

// Decodes the pixels, mirroring the engine's decodeQoi (UndertaleModTool's
// QoiConverter) op for op, including what it does when the data runs out
// early: the last colour fills the rest of the page.
static CtrQoiStatus decodePixels(Source* s, int32_t width, int32_t height, int32_t stripRows, CtrQoiStripFn fn, void* user) {
    uint32_t* strip = s->strip;

    // Pixels are words, R in the low byte, as they go into the strip.
    uint32_t index[64];
    memset(index, 0, sizeof(index));
    uint8_t r = 0, g = 0, b = 0, a = 255;
    uint32_t px = (uint32_t) a << 24;
    uint32_t run = 0;
    CtrQoiStatus status = CTR_QOI_OK;

    for (int32_t y = 0; height > y && status == CTR_QOI_OK; y += stripRows) {
        int32_t rows = height - y < stripRows ? height - y : stripRows;
        uint32_t* out = strip;
        uint32_t* outEnd = strip + (size_t) rows * (size_t) width;
        while (outEnd > out) {
            if (run > 0) {
                size_t n = (size_t) (outEnd - out);
                if (n > run) n = run;
                for (size_t i = 0; n > i; i++) out[i] = px;
                out += n;
                run -= (uint32_t) n;
                continue;
            }
            if (MAX_OP_SIZE > (size_t) (s->end - s->in) && !fill(s, MAX_OP_SIZE)) {
                status = s->error;
                break;
            }
            const uint8_t* in = s->in;
            size_t avail = (size_t) (s->end - in);
            if (avail >= MAX_OP_SIZE) {
                // The same ops without the length checks, while every op fits
                // in what is left. A run ends the loop, to be written above.
                const uint8_t* safe = s->end - MAX_OP_SIZE;
                while (safe >= in && outEnd > out) {
                    uint32_t op = *in++;
                    if (0x40 > op) {
                        px = index[op];
                        r = (uint8_t) px;
                        g = (uint8_t) (px >> 8);
                        b = (uint8_t) (px >> 16);
                        a = (uint8_t) (px >> 24);
                    } else if (0x80 > op) {
                        run = op & 0x1F;
                        if (op >= 0x60) run = ((run << 8) | *in++) + 32;
                        index[slotOf(px)] = px;
                        *out++ = px;
                        break;
                    } else if (0xC0 > op) {
                        r += signExtend((op >> 4) & 3, 2);
                        g += signExtend((op >> 2) & 3, 2);
                        b += signExtend(op & 3, 2);
                        px = (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | ((uint32_t) a << 24);
                    } else if (0xE0 > op) {
                        uint32_t merged = (op << 8) | *in++;
                        r += signExtend((merged >> 8) & 0x1F, 5);
                        g += signExtend((merged >> 4) & 0x0F, 4);
                        b += signExtend(merged & 0x0F, 4);
                        px = (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | ((uint32_t) a << 24);
                    } else if (0xF0 > op) {
                        uint32_t merged = (op << 16) | ((uint32_t) in[0] << 8) | in[1];
                        in += 2;
                        r += signExtend((merged >> 15) & 0x1F, 5);
                        g += signExtend((merged >> 10) & 0x1F, 5);
                        b += signExtend((merged >> 5) & 0x1F, 5);
                        a += signExtend(merged & 0x1F, 5);
                        px = (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | ((uint32_t) a << 24);
                    } else {
                        if (op & 8) r = *in++;
                        if (op & 4) g = *in++;
                        if (op & 2) b = *in++;
                        if (op & 1) a = *in++;
                        px = (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | ((uint32_t) a << 24);
                    }
                    index[slotOf(px)] = px;
                    *out++ = px;
                }
                s->in = in;
                continue;
            }
            if (avail == 0) {
                // Out of data: the reference keeps the last colour.
                run = UINT32_MAX;
                continue;
            }
            // The last few bytes, one op at a time with the length checks.
            uint8_t b1 = *in++;
            if ((b1 & 0xC0) == 0x00) {
                uint32_t entry = index[b1 & 0x3F];
                r = (uint8_t) entry;
                g = (uint8_t) (entry >> 8);
                b = (uint8_t) (entry >> 16);
                a = (uint8_t) (entry >> 24);
            } else if ((b1 & 0xE0) == 0x40) {
                run = b1 & 0x1F;
            } else if ((b1 & 0xE0) == 0x60) {
                if (2 > avail) goto bad;
                run = ((((uint32_t) b1 & 0x1F) << 8) | *in++) + 32;
            } else if ((b1 & 0xC0) == 0x80) {
                r += signExtend((b1 >> 4) & 3, 2);
                g += signExtend((b1 >> 2) & 3, 2);
                b += signExtend(b1 & 3, 2);
            } else if ((b1 & 0xE0) == 0xC0) {
                if (2 > avail) goto bad;
                uint32_t merged = ((uint32_t) b1 << 8) | *in++;
                r += signExtend((merged >> 8) & 0x1F, 5);
                g += signExtend((merged >> 4) & 0x0F, 4);
                b += signExtend(merged & 0x0F, 4);
            } else if ((b1 & 0xF0) == 0xE0) {
                if (3 > avail) goto bad;
                uint32_t merged = ((uint32_t) b1 << 16) | ((uint32_t) in[0] << 8) | in[1];
                in += 2;
                r += signExtend((merged >> 15) & 0x1F, 5);
                g += signExtend((merged >> 10) & 0x1F, 5);
                b += signExtend((merged >> 5) & 0x1F, 5);
                a += signExtend(merged & 0x1F, 5);
            } else {
                size_t need = 1 + (size_t) ((b1 >> 3) & 1) + ((b1 >> 2) & 1) + ((b1 >> 1) & 1) + (b1 & 1);
                if (need > avail) goto bad;
                if (b1 & 8) r = *in++;
                if (b1 & 4) g = *in++;
                if (b1 & 2) b = *in++;
                if (b1 & 1) a = *in++;
            }
            s->in = in;
            px = (uint32_t) r | ((uint32_t) g << 8) | ((uint32_t) b << 16) | ((uint32_t) a << 24);
            index[slotOf(px)] = px;
            *out++ = px;
            continue;
        bad:
            status = CTR_QOI_BAD_DATA;
            break;
        }
        if (status == CTR_QOI_OK && !fn(user, (const uint8_t*) strip, y, rows)) status = CTR_QOI_STOPPED;
        if (status == CTR_QOI_OK && s->mayYield && yieldWanted && height > y + rows) status = CTR_QOI_YIELDED;
    }
    return status;
}

void CtrQoi_init(void) {
    static bool initialized = false;
    if (initialized) return;
    LightLock_Init(&workLock);
    initialized = true;
}

void CtrQoi_releaseWorkspace(void) {
    LightLock_Lock(&workLock);
    if (workspace != nullptr) CtrMemory_freeWork(workspace);
    workspace = nullptr;
    LightLock_Unlock(&workLock);
}

size_t CtrQoi_releaseWorkspaceIfIdle(void) {
    if (workspace == nullptr || LightLock_TryLock(&workLock) != 0) return 0;
    size_t freed = 0;
    if (workspace != nullptr) {
        CtrMemory_freeWork(workspace);
        freed = WORK_BYTES;
    }
    workspace = nullptr;
    LightLock_Unlock(&workLock);
    return freed;
}

bool CtrQoi_yieldWanted(void) {
    return yieldWanted;
}

uint64_t CtrQoi_takeInflateNanos(void) {
    uint64_t nanos = inflateNanos;
    inflateNanos = 0;
    return nanos;
}

static CtrQoiStatus decodeWith(Source s, uint8_t* bzWork, size_t bzWorkSize, const uint8_t* blob, size_t size, bool gm2022_5, int32_t width,
                               int32_t height, int32_t stripRows, CtrQoiStripFn fn, void* user);

CtrQoiStatus CtrQoi_decode(const uint8_t* blob, size_t size, bool gm2022_5, int32_t stripRows, bool urgent, CtrQoiStripFn fn, void* user) {
    int32_t width = 0;
    int32_t height = 0;
    size_t workBytes = 0;
    if (!CtrQoi_info(blob, size, &width, &height, &workBytes)) return CTR_QOI_BAD_DATA;
    if (0 >= stripRows) stripRows = 1;
    if (stripRows > height) stripRows = height;
    size_t stripBytes = (size_t) stripRows * (size_t) width * 4;

    bool locked = LightLock_TryLock(&workLock) == 0;
    if (!locked && urgent) {
        yieldWanted = true;
        LightLock_Lock(&workLock);
        yieldWanted = false;
        locked = true;
    }
    if (locked && workspace == nullptr) workspace = CtrMemory_allocWork(WORK_BYTES);

    Source s;
    memset(&s, 0, sizeof(s));
    uint8_t* bzWork = nullptr;
    size_t bzWorkSize = 0;
    if (locked && workspace != nullptr && WORK_STRIP_BYTES >= stripBytes) {
        s.strip = (uint32_t*) workspace;
        s.buffer = workspace + WORK_STRIP_BYTES;
        bzWork = workspace + WORK_STRIP_BYTES + INFLATE_BUFFER_SIZE;
        bzWorkSize = WORK_BZ2_BYTES;
        s.mayYield = !urgent;
    } else {
        // Another decode has the workspace (or the page is too big for it).
        if (locked) LightLock_Unlock(&workLock);
        locked = false;
        s.strip = CtrMemory_allocWork(stripBytes);
        s.buffer = CtrMemory_allocWork(INFLATE_BUFFER_SIZE);
    }
    CtrQoiStatus status = CTR_QOI_NO_MEMORY;
    if (s.strip != nullptr && s.buffer != nullptr) {
        status = decodeWith(s, bzWork, bzWorkSize, blob, size, gm2022_5, width, height, stripRows, fn, user);
    }
    if (locked) {
        LightLock_Unlock(&workLock);
    } else {
        if (s.strip != nullptr) CtrMemory_freeWork(s.strip);
        if (s.buffer != nullptr) CtrMemory_freeWork(s.buffer);
    }
    return status;
}

// bzWork is the workspace's room for the BZip2 decoder, or nullptr to
// allocate it.
static CtrQoiStatus decodeWith(Source s, uint8_t* bzWork, size_t bzWorkSize, const uint8_t* blob, size_t size, bool gm2022_5, int32_t width,
                               int32_t height, int32_t stripRows, CtrQoiStripFn fn, void* user) {
    if (blob[0] == 'f') {
        if (QOI_HEADER_SIZE > size) return CTR_QOI_BAD_DATA;
        size_t length = load32le(blob + 8);
        if (length > size - QOI_HEADER_SIZE) return CTR_QOI_BAD_DATA;
        s.in = blob + QOI_HEADER_SIZE;
        s.end = s.in + length;
        return decodePixels(&s, width, height, stripRows, fn, user);
    }

    size_t headerSize = bz2HeaderSize(blob, size, gm2022_5);
    if (headerSize >= size) return CTR_QOI_BAD_DATA;
    const uint8_t* stream = blob + headerSize;
    size_t streamSize = size - headerSize;
    size_t need = CtrBz2_workBytes(stream, streamSize);
    if (need == 0) return CTR_QOI_BAD_DATA;
    uint8_t* allocated = nullptr;
    if (bzWork == nullptr || need > bzWorkSize) {
        allocated = CtrMemory_allocWork(need);
        if (allocated == nullptr) return CTR_QOI_NO_MEMORY;
        bzWork = allocated;
        bzWorkSize = need;
    }
    s.compressed = true;
    s.bz = CtrBz2_open(stream, streamSize, bzWork, bzWorkSize);
    s.in = s.end = s.buffer;
    s.left = SIZE_MAX;

    // The inflated stream is a whole fioq page: its header, then the pixels.
    CtrQoiStatus status = CTR_QOI_OK;
    if (s.bz == nullptr) {
        status = CTR_QOI_BAD_DATA;
    } else if (!fill(&s, QOI_HEADER_SIZE)) {
        status = s.error;
    } else if (QOI_HEADER_SIZE > (size_t) (s.end - s.in) || memcmp(s.in, "fioq", 4) != 0 || (int32_t) load16le(s.in + 4) != width ||
               (int32_t) load16le(s.in + 6) != height) {
        status = CTR_QOI_BAD_DATA;
    } else {
        size_t length = load32le(s.in + 8);
        s.in += QOI_HEADER_SIZE;
        size_t have = (size_t) (s.end - s.in);
        if (have > length) {
            s.end = s.in + length;
            s.left = 0;
        } else {
            s.left = length - have;
        }
        status = decodePixels(&s, width, height, stripRows, fn, user);
    }
    if (allocated != nullptr) CtrMemory_freeWork(allocated);
    return status;
}
