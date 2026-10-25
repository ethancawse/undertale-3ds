#include "ctr_inflate.h"

#include <string.h>

#include "common.h"

// How far back a match can reach, and how long it can be.
#define WINDOW_SIZE 32768u
#define MAX_MATCH 258u
// Output is decoded into a buffer this big. When it fills, the new bytes are
// passed on and the last WINDOW_SIZE bytes move to the front.
#define BUFFER_SIZE (256u * 1024u)
#define FLUSH_AT (BUFFER_SIZE - MAX_MATCH)
// Match copies may write this far past the end of the match.
#define COPY_SLACK 16u
#define MAX_BITS 15
// Codes of up to LUT_BITS bits decode with one lookup in a table of
// (symbol << 4) | length entries; 0 marks the prefix of a longer code.
#define LUT_BITS 10
#define LUT_SIZE (1u << LUT_BITS)
#define MAX_LIT_CODES 288
#define MAX_DIST_CODES 30
// A block decoded to its end; decoding goes on with the next one.
#define BLOCK_DONE (-1)

typedef struct {
    uint16_t lut[LUT_SIZE];
    uint16_t count[MAX_BITS + 1]; // codes of each length
    uint16_t symbol[MAX_LIT_CODES]; // symbols in code order
} Huffman;

typedef struct {
    const uint8_t* in;
    const uint8_t* inEnd;
    CtrInflateInputFn input; // nullptr once the input has run out
    void* inputUser;
    uint32_t bitBuf;   // the next bits of the stream, from the low bit up
    uint32_t bitCount; // how many of bitBuf's bits are the stream's
    uint32_t overrun;  // zero bytes added to bitBuf past the end of the input

    uint8_t* buffer;   // BUFFER_SIZE + COPY_SLACK bytes
    uint32_t pos;      // bytes in buffer
    uint32_t handed;   // bytes of buffer passed on
    size_t left;       // bytes still to pass on
    CtrInflateOutputFn output;
    void* outputUser;

    Huffman lit;  // literal/length codes (and the code length codes)
    Huffman dist;
} Inflater;

#define INFLATER_BYTES ((sizeof(Inflater) + 7) & ~(size_t) 7)

static const uint16_t lengthBase[29] = { 3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t lengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t distBase[30] = { 1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t distExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

// ===[ Bits ]===

// The next input byte, moving on to the next piece of input as needed; past
// the end, a zero, counted.
static uint32_t slowByte(Inflater* z) {
    while (z->in == z->inEnd) {
        const uint8_t* data = nullptr;
        size_t size = 0;
        if (z->input == nullptr || !z->input(z->inputUser, &data, &size) || data == nullptr) {
            z->input = nullptr;
            z->overrun++;
            return 0;
        }
        z->in = data;
        z->inEnd = data + size;
    }
    return *z->in++;
}

// Tops bitBuf up to at least 25 bits.
static void refill(Inflater* z) {
    while (24 >= z->bitCount) {
        uint32_t byte = z->inEnd > z->in ? *z->in++ : slowByte(z);
        z->bitBuf |= byte << z->bitCount;
        z->bitCount += 8;
    }
}

// n is 0 to 24.
static uint32_t getBits(Inflater* z, uint32_t n) {
    refill(z);
    uint32_t v = z->bitBuf & ((1u << n) - 1);
    z->bitBuf >>= n;
    z->bitCount -= n;
    return v;
}

// Whether any of the bits taken so far were past the end of the input.
static bool truncated(const Inflater* z) {
    return z->overrun * 8 > z->bitCount;
}

// ===[ Codes ]===

// Builds the decoding tables for codes of the given lengths. Returns false
// for lengths that give more codes than there is room for; leaving codes
// unused is allowed (decoding one fails).
static bool buildHuffman(Huffman* h, const uint8_t* lengths, uint32_t n) {
    memset(h->count, 0, sizeof(h->count));
    for (uint32_t i = 0; n > i; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int32_t room = 1;
    for (uint32_t len = 1; MAX_BITS >= len; len++) {
        room = room * 2 - h->count[len];
        if (0 > room) return false;
    }
    uint16_t offset[MAX_BITS + 2];
    offset[1] = 0;
    for (uint32_t len = 1; MAX_BITS >= len; len++) offset[len + 1] = (uint16_t) (offset[len] + h->count[len]);
    for (uint32_t i = 0; n > i; i++) {
        if (lengths[i] != 0) h->symbol[offset[lengths[i]]++] = (uint16_t) i;
    }

    // Codes are canonical: those of one length are consecutive, in symbol
    // order. The stream sends them from the top bit down, so the table is
    // indexed by the code's bits reversed.
    memset(h->lut, 0, sizeof(h->lut));
    uint32_t code = 0;
    uint32_t index = 0;
    for (uint32_t len = 1; LUT_BITS >= len; len++) {
        for (uint32_t k = 0; h->count[len] > k; k++, code++) {
            uint32_t reversed = 0;
            for (uint32_t b = 0; len > b; b++) reversed |= ((code >> b) & 1) << (len - 1 - b);
            uint16_t entry = (uint16_t) ((h->symbol[index++] << 4) | len);
            for (uint32_t at = reversed; LUT_SIZE > at; at += 1u << len) h->lut[at] = entry;
        }
        code <<= 1;
    }
    return true;
}

// Decodes a code too long for the table (or not in it) from bits, which
// holds at least MAX_BITS bits. Returns the symbol and sets *length, or
// returns -1 for a code that isn't there.
static int32_t decodeSlow(const Huffman* h, uint32_t bits, uint32_t* length) {
    int32_t code = 0;
    int32_t first = 0;
    int32_t index = 0;
    for (uint32_t len = 1; MAX_BITS >= len; len++) {
        code |= (int32_t) (bits & 1);
        bits >>= 1;
        int32_t count = h->count[len];
        if (count > code - first) {
            *length = len;
            return h->symbol[index + code - first];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int32_t decodeSymbol(Inflater* z, const Huffman* h) {
    refill(z);
    uint32_t entry = h->lut[z->bitBuf & (LUT_SIZE - 1)];
    uint32_t len = entry & 15;
    int32_t sym = (int32_t) (entry >> 4);
    if (entry == 0) sym = decodeSlow(h, z->bitBuf, &len);
    if (0 > sym) return -1;
    z->bitBuf >>= len;
    z->bitCount -= len;
    return sym;
}

static void buildFixed(Inflater* z) {
    uint8_t lengths[MAX_LIT_CODES];
    memset(lengths, 8, 144);
    memset(lengths + 144, 9, 256 - 144);
    memset(lengths + 256, 7, 280 - 256);
    memset(lengths + 280, 8, MAX_LIT_CODES - 280);
    buildHuffman(&z->lit, lengths, MAX_LIT_CODES);
    memset(lengths, 5, MAX_DIST_CODES);
    buildHuffman(&z->dist, lengths, MAX_DIST_CODES);
}

static int32_t buildDynamic(Inflater* z) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    uint32_t nLit = getBits(z, 5) + 257;
    uint32_t nDist = getBits(z, 5) + 1;
    uint32_t nCode = getBits(z, 4) + 4;
    if (nLit > 286 || nDist > MAX_DIST_CODES) return CTR_INFLATE_BAD_DATA;

    uint8_t lengths[MAX_LIT_CODES + MAX_DIST_CODES];
    memset(lengths, 0, 19);
    for (uint32_t i = 0; nCode > i; i++) lengths[order[i]] = (uint8_t) getBits(z, 3);
    if (!buildHuffman(&z->lit, lengths, 19)) return CTR_INFLATE_BAD_DATA;
    uint32_t total = nLit + nDist;
    for (uint32_t i = 0; total > i;) {
        int32_t sym = decodeSymbol(z, &z->lit);
        if (0 > sym) return truncated(z) ? CTR_INFLATE_SHORT : CTR_INFLATE_BAD_DATA;
        if (16 > sym) {
            lengths[i++] = (uint8_t) sym;
            continue;
        }
        uint8_t value = 0;
        uint32_t repeat = 0;
        if (sym == 16) {
            if (i == 0) return CTR_INFLATE_BAD_DATA;
            value = lengths[i - 1];
            repeat = 3 + getBits(z, 2);
        } else if (sym == 17) {
            repeat = 3 + getBits(z, 3);
        } else {
            repeat = 11 + getBits(z, 7);
        }
        if (repeat > total - i) return CTR_INFLATE_BAD_DATA;
        memset(lengths + i, value, repeat);
        i += repeat;
    }
    if (truncated(z)) return CTR_INFLATE_SHORT;
    // A block has to be able to end.
    if (lengths[256] == 0) return CTR_INFLATE_BAD_DATA;
    if (!buildHuffman(&z->lit, lengths, nLit) || !buildHuffman(&z->dist, lengths + nLit, nDist)) return CTR_INFLATE_BAD_DATA;
    return BLOCK_DONE;
}

// ===[ Output ]===

// Where in the buffer decoding has to stop to pass bytes on: when it's
// nearly full, or has every byte wanted.
static uint32_t stopFor(const Inflater* z) {
    return z->left < (size_t) (FLUSH_AT - z->handed) ? z->handed + (uint32_t) z->left : FLUSH_AT;
}

// Passes on the new bytes and, when the buffer is nearly full, moves the
// window to its front. Returns a status to stop with, or BLOCK_DONE to go on.
static int32_t flush(Inflater* z) {
    size_t n = z->pos - z->handed;
    if (n > z->left) n = z->left;
    if (n > 0 && !z->output(z->outputUser, z->buffer + z->handed, n)) return CTR_INFLATE_STOPPED;
    z->left -= n;
    z->handed = z->pos;
    // Bytes decoded from past the end of the input aren't the stream's.
    if (truncated(z)) return CTR_INFLATE_SHORT;
    if (z->left == 0) return CTR_INFLATE_OK;
    if (z->pos >= FLUSH_AT) {
        memmove(z->buffer, z->buffer + z->pos - WINDOW_SIZE, WINDOW_SIZE);
        z->pos = WINDOW_SIZE;
        z->handed = WINDOW_SIZE;
    }
    return BLOCK_DONE;
}

// Copies a match of length bytes (at least 3) from dist bytes back. A word
// at a time when dist allows, which may write up to COPY_SLACK - 1 bytes
// past the end; the next output overwrites them. In every loop each load
// comes after the store before it, so a match that overlaps itself repeats
// as it should.
static inline void copyMatch(uint8_t* dst, uint32_t dist, uint32_t length) {
    const uint8_t* src = dst - dist;
    uint8_t* end = dst + length;
    if (dist == 1) {
        // A run of one byte (most often a transparent area): aligned words.
        uint32_t w = *src * 0x01010101u;
        while (((uintptr_t) dst & 3) != 0) *dst++ = (uint8_t) w;
        uint32_t* d = (uint32_t*) dst;
        while (end > (uint8_t*) d) {
            d[0] = w;
            d[1] = w;
            d[2] = w;
            d[3] = w;
            d += 4;
        }
    } else if ((dist & 3) == 0) {
        // A run of one pixel, or a copy of the row above: source and
        // destination are aligned alike, so after a few bytes, aligned words.
        while (((uintptr_t) dst & 3) != 0) *dst++ = *src++;
        uint32_t* d = (uint32_t*) dst;
        const uint32_t* s = (const uint32_t*) src;
        while (end > (uint8_t*) d) {
            d[0] = s[0];
            d[1] = s[1];
            d += 2;
            s += 2;
        }
    } else if (dist > 4) {
        do {
            uint32_t w;
            memcpy(&w, src, 4);
            memcpy(dst, &w, 4);
            memcpy(&w, src + 4, 4);
            memcpy(dst + 4, &w, 4);
            src += 8;
            dst += 8;
        } while (end > dst);
    } else {
        for (uint32_t i = 0; length > i; i++) dst[i] = src[i];
    }
}

// ===[ Blocks ]===

static int32_t storedBlock(Inflater* z) {
    uint32_t drop = z->bitCount & 7;
    z->bitBuf >>= drop;
    z->bitCount -= drop;
    uint32_t length = getBits(z, 16);
    uint32_t check = getBits(z, 16);
    if (truncated(z)) return CTR_INFLATE_SHORT;
    if ((length ^ 0xFFFFu) != check) return CTR_INFLATE_BAD_DATA;
    uint32_t stop = stopFor(z);
    while (length > 0) {
        if (z->pos >= stop) {
            int32_t status = flush(z);
            if (status != BLOCK_DONE) return status;
            stop = stopFor(z);
        }
        z->buffer[z->pos++] = (uint8_t) getBits(z, 8);
        length--;
    }
    return truncated(z) ? CTR_INFLATE_SHORT : BLOCK_DONE;
}

// Decodes a block's symbols with the tables in z->lit and z->dist.
static int32_t codedBlock(Inflater* z) {
    // The bit reader and output position live in locals here: stores to the
    // buffer could alias the struct's fields, which would keep them in memory.
    uint32_t bitBuf = z->bitBuf;
    uint32_t bitCount = z->bitCount;
    const uint8_t* in = z->in;
    const uint8_t* inEnd = z->inEnd;
    uint8_t* buffer = z->buffer;
    uint32_t pos = z->pos;
    uint32_t stop = stopFor(z);
    const uint16_t* litLut = z->lit.lut;
    const uint16_t* distLut = z->dist.lut;
    int32_t result;
#define SAVE()                    \
    do {                          \
        z->bitBuf = bitBuf;       \
        z->bitCount = bitCount;   \
        z->in = in;               \
        z->pos = pos;             \
    } while (0)
#define LOAD()                    \
    do {                          \
        bitBuf = z->bitBuf;       \
        bitCount = z->bitCount;   \
        in = z->in;               \
        inEnd = z->inEnd;         \
        pos = z->pos;             \
    } while (0)
// Tops bitBuf up to at least 24 bits. With 4 bytes of input at hand, in one
// go: a (little-endian) word goes in above the bits there, and as many of its bytes as fit
// count as taken. The bits of the rest are the stream's next bits, so a
// later refill putting them in again changes nothing (bits above bitCount
// are always either those or zeros).
#define REFILL()                                                  \
    do {                                                          \
        if (24 >= bitCount) {                                     \
            if (inEnd - in >= 4) {                                \
                uint32_t word_;                                   \
                memcpy(&word_, in, 4);                            \
                bitBuf |= word_ << bitCount;                      \
                in += (31 - bitCount) >> 3;                       \
                bitCount |= 24;                                   \
            } else {                                              \
                while (24 >= bitCount) {                          \
                    uint32_t byte_;                               \
                    if (inEnd > in) {                             \
                        byte_ = *in++;                            \
                    } else {                                      \
                        z->in = in;                               \
                        byte_ = slowByte(z);                      \
                        in = z->in;                               \
                        inEnd = z->inEnd;                         \
                    }                                             \
                    bitBuf |= byte_ << bitCount;                  \
                    bitCount += 8;                                \
                }                                                 \
            }                                                     \
        }                                                         \
    } while (0)
#define FINISH(status)  \
    do {                \
        result = status; \
        goto finish;    \
    } while (0)

    for (;;) {
        if (pos >= stop) {
            SAVE();
            result = flush(z);
            if (result != BLOCK_DONE) return result;
            LOAD();
            stop = stopFor(z);
        }

        REFILL();
        uint32_t entry = litLut[bitBuf & (LUT_SIZE - 1)];
        uint32_t len = entry & 15;
        int32_t sym = (int32_t) (entry >> 4);
        if (entry == 0) {
            sym = decodeSlow(&z->lit, bitBuf, &len);
            if (0 > sym) FINISH(CTR_INFLATE_BAD_DATA);
        }
        bitBuf >>= len;
        bitCount -= len;
        if (256 > sym) {
            buffer[pos++] = (uint8_t) sym;
            continue;
        }
        if (sym == 256) FINISH(BLOCK_DONE);
        sym -= 257;
        if (sym >= 29) FINISH(CTR_INFLATE_BAD_DATA);
        uint32_t length = lengthBase[sym];
        uint32_t extra = lengthExtra[sym];
        length += bitBuf & ((1u << extra) - 1);
        bitBuf >>= extra;
        bitCount -= extra;

        REFILL();
        entry = distLut[bitBuf & (LUT_SIZE - 1)];
        len = entry & 15;
        sym = (int32_t) (entry >> 4);
        if (entry == 0) {
            sym = decodeSlow(&z->dist, bitBuf, &len);
            if (0 > sym) FINISH(CTR_INFLATE_BAD_DATA);
        }
        bitBuf >>= len;
        bitCount -= len;
        if (sym >= MAX_DIST_CODES) FINISH(CTR_INFLATE_BAD_DATA);
        uint32_t dist = distBase[sym];
        extra = distExtra[sym];
        if (extra > bitCount) REFILL();
        dist += bitBuf & ((1u << extra) - 1);
        bitBuf >>= extra;
        bitCount -= extra;
        // The buffer holds all the output so far, or at least the window.
        if (dist > pos) FINISH(CTR_INFLATE_BAD_DATA);
        copyMatch(buffer + pos, dist, length);
        pos += length;
    }

finish:
    SAVE();
    if (result == CTR_INFLATE_BAD_DATA && truncated(z)) return CTR_INFLATE_SHORT;
    return result;
#undef SAVE
#undef LOAD
#undef REFILL
#undef FINISH
}

// ===[ Interface ]===

size_t CtrInflate_workBytes(void) {
    return INFLATER_BYTES + BUFFER_SIZE + COPY_SLACK;
}

CtrInflateStatus CtrInflate_decode(const uint8_t* first, size_t firstSize, CtrInflateInputFn input, void* inputUser, size_t want,
                                   CtrInflateOutputFn output, void* outputUser, void* work) {
    Inflater* z = work;
    memset(z, 0, sizeof(*z));
    z->in = first;
    z->inEnd = first + firstSize;
    z->input = input;
    z->inputUser = inputUser;
    z->buffer = (uint8_t*) work + INFLATER_BYTES;
    z->left = want;
    z->output = output;
    z->outputUser = outputUser;
    if (want == 0) return CTR_INFLATE_OK;

    bool last = false;
    while (!last) {
        if (truncated(z)) return CTR_INFLATE_SHORT;
        last = getBits(z, 1) != 0;
        uint32_t type = getBits(z, 2);
        int32_t status = CTR_INFLATE_BAD_DATA;
        if (type == 0) {
            status = storedBlock(z);
        } else if (type == 1) {
            buildFixed(z);
            status = codedBlock(z);
        } else if (type == 2) {
            status = buildDynamic(z);
            if (status == BLOCK_DONE) status = codedBlock(z);
        }
        if (status != BLOCK_DONE) return (CtrInflateStatus) status;
    }
    // The stream ended: pass on what is left.
    int32_t status = flush(z);
    return status == BLOCK_DONE ? CTR_INFLATE_SHORT : (CtrInflateStatus) status;
}
