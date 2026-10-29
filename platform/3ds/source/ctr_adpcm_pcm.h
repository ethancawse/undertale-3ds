// Bounded PCM pieces from a pinned, immutable resident DTAD cache entry.
#ifndef CTR_ADPCM_PCM_H
#define CTR_ADPCM_PCM_H

#include "ctr_adpcm_format.h"

typedef struct {
    CtrAdpcmInfo info;
    const uint8_t* data;
    uint32_t bytes, position, blockFirst, blockFrames;
    const uint8_t* channel[2];
    int32_t history[2][2];
    bool ready;
} CtrAdpcmPcm;

// Validates all block descriptors/predictors before exposing the resident
// source. No allocation or copies of the payload. Data outlives this cursor;
// its owner serializes access (the existing feeder already has its own lock).
bool CtrAdpcmPcm_open(CtrAdpcmPcm* cursor, const void* data, uint32_t bytes);
// No I/O, allocation, wrapping, padding or gain changes. Consecutive pieces
// continue their histories; seeks restart from the nearest stored block.
// count <= capacityFrames, output has count * info.channels PCM16 samples.
// decodedFrames includes discarded seek samples as well as delivered frames;
// a caller measures its own ticks/core around this call. On failure the cursor
// and output may have advanced; the owner must retire it rather than queue it.
bool CtrAdpcmPcm_read(CtrAdpcmPcm* cursor, uint32_t first, uint32_t count,
                      int16_t* out, uint32_t capacityFrames, uint32_t* decodedFrames);

#endif
