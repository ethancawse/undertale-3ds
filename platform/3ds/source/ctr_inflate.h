// Deflate decompressor for the PNG texture pages of older GameMaker games
// (UNDERTALE's, see ctr_png.h).
//
// GameMaker writes pages unfiltered, so nearly all of a page inflates from
// long matches: runs of one byte (transparent areas) and of one pixel, and
// copies of the row above. miniz's tinfl copies matches three bytes at a time
// on the ARM11, which made inflating its biggest cost. This decoder copies
// them a word at a time (or with memset for runs of one byte), reads codes
// through lookup tables and keeps its state in registers. It decodes the
// same streams to the same bytes, and checks every bound on corrupt input.
#ifndef CTRPORT_CTR_INFLATE_H
#define CTRPORT_CTR_INFLATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CTR_INFLATE_OK,       // `want` bytes were produced
    CTR_INFLATE_SHORT,    // the stream ended, or its input ran out, first
    CTR_INFLATE_BAD_DATA, // corrupt
    CTR_INFLATE_STOPPED,  // the output callback returned false
} CtrInflateStatus;

// Gives the next piece of the stream's input in *data and *size. Returns
// false when there is no more.
typedef bool (*CtrInflateInputFn)(void* user, const uint8_t** data, size_t* size);

// Takes the next `size` bytes of output, which stay readable only during the
// call. Returns false to stop.
typedef bool (*CtrInflateOutputFn)(void* user, const uint8_t* data, size_t size);

// Memory CtrInflate_decode needs: its tables and a buffer of output, which
// holds the last 32 KB the stream can refer back to.
size_t CtrInflate_workBytes(void);

// Decodes a raw deflate stream (no zlib header) until `want` bytes have come
// out, handing them to `output` in batches. The input starts with first and
// continues with what `input` gives (which may be nullptr). Decoding stops
// once `want` bytes have come out, and nothing past them is passed on, so
// what follows in the stream doesn't matter. work is CtrInflate_workBytes
// bytes, 4-byte aligned.
CtrInflateStatus CtrInflate_decode(const uint8_t* first, size_t firstSize, CtrInflateInputFn input, void* inputUser, size_t want,
                                   CtrInflateOutputFn output, void* outputUser, void* work);

#endif
