// BZip2 decompressor for streams held whole in memory: the BZip2+QOI texture
// pages of GameMaker 2022+ games ("2zoq", see ctr_qoi.h).
//
// libbzip2 takes its input a byte at a time through a resumable state
// machine and decodes Huffman codes a bit at a time; on the ARM11 that made
// inflating half the time of preparing DELTARUNE's pages. With the whole
// stream at hand, this one reads codes through lookup tables and keeps its
// state in registers. The output is the same, byte for byte, and so are the
// checks: block and stream CRCs, and every bound on corrupt input.
#ifndef CTRPORT_CTR_BZ2_H
#define CTRPORT_CTR_BZ2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CTR_BZ2_OK,
    CTR_BZ2_END,      // the stream ended (or was cut short) and every byte has been read
    CTR_BZ2_BAD_DATA, // corrupt
} CtrBz2Status;

typedef struct CtrBz2 CtrBz2;

// Memory a decoder needs for a stream (from the "BZh" header; 64 bytes are
// enough): the decoder itself, then its block buffer. 0 if this isn't BZip2.
size_t CtrBz2_workBytes(const uint8_t* head, size_t size);

// Starts decoding the stream src (kept until done) in `work`, a block of
// CtrBz2_workBytes, 4-byte aligned. Returns nullptr if src isn't BZip2 or
// work is too small.
CtrBz2* CtrBz2_open(const uint8_t* src, size_t size, void* work, size_t workSize);

// Decodes up to `want` bytes into dst and returns how many it wrote; fewer
// only at the end of the stream or on corrupt data, which *status tells.
size_t CtrBz2_read(CtrBz2* d, uint8_t* dst, size_t want, CtrBz2Status* status);

#endif
