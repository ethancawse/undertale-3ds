#include "ctr_lz.h"

#include <stdlib.h>
#include <string.h>

#include "common.h"

#define HASH_BITS 14
#define MIN_MATCH 4
#define MAX_OFFSET 65535
#define WINDOW (MAX_OFFSET + 1)
// Matches are looked up by their first 8 bytes and are at least that long:
// two RGBA8 texels or four RGBA5551 ones. Hashing 4 bytes instead finds a few
// more short matches but makes the chains far longer to walk.
#define FIND_MIN 8
// Candidates tried per position, a match long enough to stop looking, and
// how many positions at the end of a match are added to the hash table.
// Compression runs once per page, on first run while the player waits on the
// "Preparing textures" screen, so this trades some size for speed: on
// DELTARUNE's pages, 16 candidates made files 5-9% smaller and took twice as
// long, and 32 candidates with lazy matching, every position added, several
// times as long again.
#define CHAIN_DEPTH 4
#define NICE_MATCH 64
#define MATCH_TAIL_INSERTS 4

static inline uint32_t read32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint32_t hashOf(uint32_t first, uint32_t second) {
    return ((first * 2654435761u) ^ (second * 2246822519u)) >> (32 - HASH_BITS);
}

// ===[ Compression ]===

// Hash chains over the positions that are multiples of step.
typedef struct {
    const uint8_t* src;
    uint32_t* head;  // hash -> most recent position + 1, 0 for none
    uint16_t* chain; // chainSlot(position) -> distance back to the previous position with the same hash, 0 for none
    uint32_t step;
    uint32_t stepShift; // log2(step): the ARM11 has no divide instruction
} MatchFinder;

static inline uint32_t chainSlot(const MatchFinder* mf, uint32_t pos) {
    return (pos & (WINDOW - 1)) >> mf->stepShift;
}

// Adds pos, whose first 8 bytes hash to h, and returns the most recent
// position with the same hash before it (+ 1, 0 for none).
static inline uint32_t insert(MatchFinder* mf, uint32_t pos, uint32_t h) {
    uint32_t prev = mf->head[h];
    uint32_t distance = prev == 0 ? 0 : pos - (prev - 1);
    mf->chain[chainSlot(mf, pos)] = (uint16_t) (distance > MAX_OFFSET ? 0 : distance);
    mf->head[h] = pos + 1;
    return prev;
}

// Adds pos and returns the length of the longest earlier match for it (0 if
// under FIND_MIN): a whole number of steps, unless it runs to the end.
// pos + FIND_MIN must not pass end.
static size_t findMatch(MatchFinder* mf, uint32_t pos, const uint8_t* end, uint32_t* outOffset) {
    const uint8_t* ip = mf->src + pos;
    const uint8_t* last = end - 4;
    uint32_t seq0 = read32(ip);
    uint32_t seq1 = read32(ip + 4);
    // Hashed once, both to look the position up and to add it.
    uint32_t candidate = insert(mf, pos, hashOf(seq0, seq1));
    if (candidate == 0) return 0;

    size_t best = 0;
    uint32_t c = candidate - 1;
    for (int32_t depth = 0; CHAIN_DEPTH > depth; depth++) {
        uint32_t offset = pos - c;
        if (offset > MAX_OFFSET) break;
        const uint8_t* rp = mf->src + c;
        // The byte at `best` must match for this candidate to be longer.
        if (end - ip > (ptrdiff_t) best && rp[best] == ip[best] && read32(rp) == seq0 && read32(rp + 4) == seq1) {
            const uint8_t* mp = ip + FIND_MIN;
            const uint8_t* r = rp + FIND_MIN;
            while (last >= mp && read32(mp) == read32(r)) {
                mp += 4;
                r += 4;
            }
            while (end > mp && *mp == *r) {
                mp++;
                r++;
            }
            size_t len = (size_t) (mp - ip);
            // Whole texels, so that the next position is a multiple of step too.
            if (mp != end) len &= ~(size_t) (mf->step - 1);
            if (len > best) {
                best = len;
                *outOffset = offset;
                if (mp == end || len >= NICE_MATCH) break;
            }
        }
        uint16_t distance = mf->chain[chainSlot(mf, c)];
        if (distance == 0 || distance > c) break;
        c -= distance;
    }
    return best >= FIND_MIN ? best : 0;
}

static uint8_t* writeLength(uint8_t* op, size_t len) {
    while (len >= 255) {
        *op++ = 255;
        len -= 255;
    }
    *op++ = (uint8_t) len;
    return op;
}

// Emits literals [anchor, anchor + litLen) followed by a match, or only the
// literals when matchLen is 0 (the final sequence).
static uint8_t* writeSequence(uint8_t* op, const uint8_t* anchor, size_t litLen, size_t offset, size_t matchLen) {
    uint8_t* token = op++;
    *token = (uint8_t) ((litLen >= 15 ? 15 : litLen) << 4);
    if (litLen >= 15) op = writeLength(op, litLen - 15);
    memcpy(op, anchor, litLen);
    op += litLen;
    if (matchLen == 0) return op;
    size_t ml = matchLen - MIN_MATCH;
    *op++ = (uint8_t) offset;
    *op++ = (uint8_t) (offset >> 8);
    *token |= (uint8_t) (ml >= 15 ? 15 : ml);
    if (ml >= 15) op = writeLength(op, ml - 15);
    return op;
}

size_t CtrLz_bound(size_t srcSize) {
    return srcSize + srcSize / 255 + 16;
}

size_t CtrLz_compress(const uint8_t* src, size_t srcSize, uint8_t* dst, uint32_t step) {
    if (step != 2 && step != 4) step = 1;
    uint32_t stepShift = step == 4 ? 2 : step == 2 ? 1 : 0;
    MatchFinder mf = { src, calloc((size_t) 1 << HASH_BITS, sizeof(uint32_t)), calloc(WINDOW >> stepShift, sizeof(uint16_t)), step, stepShift };
    if (mf.head == nullptr || mf.chain == nullptr) {
        free(mf.head);
        free(mf.chain);
        return 0;
    }

    const uint8_t* end = src + srcSize;
    uint8_t* op = dst;
    uint32_t pos = 0;
    uint32_t anchor = 0;
    if (srcSize >= FIND_MIN) {
        uint32_t lastPos = (uint32_t) (srcSize - FIND_MIN);
        while (lastPos >= pos) {
            uint32_t offset = 0;
            size_t len = findMatch(&mf, pos, end, &offset);
            if (len == 0) {
                pos += step;
                continue;
            }
            op = writeSequence(op, src + anchor, pos - anchor, offset, len);
            uint32_t matchStart = pos;
            pos += (uint32_t) len;
            anchor = pos;
            // Of the positions inside the match, only the last few go in the
            // hash table: a texture's matches are mostly long runs of the
            // same texels, whose positions would all be alike.
            uint32_t from = pos > MATCH_TAIL_INSERTS * step ? pos - MATCH_TAIL_INSERTS * step : 0;
            if (matchStart + step > from) from = matchStart + step;
            for (uint32_t p = from; pos > p && lastPos >= p; p += step) insert(&mf, p, hashOf(read32(src + p), read32(src + p + 4)));
        }
    }
    if (srcSize > anchor) op = writeSequence(op, src + anchor, srcSize - anchor, 0, 0);
    free(mf.head);
    free(mf.chain);
    return (size_t) (op - dst);
}

// ===[ Decompression ]===

static inline bool readLength(const uint8_t** ip, const uint8_t* iend, size_t* len) {
    uint32_t b;
    do {
        if (*ip >= iend) return false;
        b = *(*ip)++;
        *len += b;
    } while (b == 255);
    return true;
}

bool CtrLz_decompress(const uint8_t* src, size_t srcSize, uint8_t* dst, size_t dstSize) {
    const uint8_t* ip = src;
    const uint8_t* iend = src + srcSize;
    uint8_t* op = dst;
    uint8_t* oend = dst + dstSize;

    while (iend > ip) {
        uint32_t token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15 && !readLength(&ip, iend, &lit)) return false;
        if (lit > (size_t) (iend - ip) || lit > (size_t) (oend - op)) return false;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip == iend) break;

        if (2 > iend - ip) return false;
        size_t offset = (size_t) ip[0] | ((size_t) ip[1] << 8);
        ip += 2;
        size_t ml = token & 15;
        if (ml == 15 && !readLength(&ip, iend, &ml)) return false;
        ml += MIN_MATCH;
        if (offset == 0 || offset > (size_t) (op - dst) || ml > (size_t) (oend - op)) return false;

        const uint8_t* m = op - offset;
        uint8_t* cpy = op + ml;
        if (8 > oend - cpy) {
            // Near the end of the output: no room for the 8-byte overrun.
            while (cpy > op) *op++ = *m++;
            continue;
        }
        if (8 > offset) {
            // The match overlaps its own output and repeats with period
            // `offset`. Write the first 8 bytes one by one, then copy from a
            // whole number of periods back, at least 8 bytes behind.
            // offset * ceil(8 / offset), from a table: the ARM11 has no divide.
            static const uint8_t periods[8] = { 0, 8, 8, 9, 8, 10, 12, 14 };
            for (int32_t i = 0; 8 > i; i++) op[i] = m[i];
            size_t period = periods[offset];
            op += 8;
            m = op - period;
        }
        // 8 bytes at a time; may write up to 7 bytes past cpy, which later
        // sequences overwrite.
        while (cpy > op) {
            memcpy(op, m, 8);
            op += 8;
            m += 8;
        }
        op = cpy;
    }
    return op == oend;
}
