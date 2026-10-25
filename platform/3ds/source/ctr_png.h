// Streaming decoder for the PNGs GameMaker stores texture pages as.
//
// stb_image inflates a whole PNG into one buffer and then unfilters it into a
// second, so decoding a 2048x2048 page briefly needs 32 MB. This decoder
// inflates through a 256 KB buffer (ctr_inflate.h) and hands the image over a
// strip of rows at a time, so it needs memory for one strip only. It handles
// 8-bit RGBA, non-interlaced PNGs, which is what GameMaker writes; anything
// else is reported as unsupported for the caller to decode some other way.
#ifndef CTRPORT_CTR_PNG_H
#define CTRPORT_CTR_PNG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CTR_PNG_OK,
    CTR_PNG_UNSUPPORTED, // a valid PNG, perhaps, but not 8-bit RGBA non-interlaced
    CTR_PNG_BAD_DATA,
    CTR_PNG_NO_MEMORY,
    CTR_PNG_STOPPED,     // the strip callback returned false
} CtrPngStatus;

// Receives `count` rows of RGBA8 pixels starting at row y, `width * 4` bytes
// each. Returns false to stop decoding.
typedef bool (*CtrPngStripFn)(void* user, const uint8_t* rows, int32_t y, int32_t count);

// Reads the image size from the header. Returns false if this is not a PNG
// this decoder can stream.
bool CtrPng_info(const uint8_t* png, size_t size, int32_t* width, int32_t* height);

// Decodes the PNG, calling fn with strips of stripRows rows (fewer for the
// last strip), top to bottom.
CtrPngStatus CtrPng_decode(const uint8_t* png, size_t size, int32_t stripRows, CtrPngStripFn fn, void* user);

// Time the calling thread has spent inflating since the last call.
uint64_t CtrPng_takeInflateNanos(void);

#endif
