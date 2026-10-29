// DTAD v1 reader shared by native queueing and the bounded PCM seek fallback.
#ifndef CTR_ADPCM_FORMAT_H
#define CTR_ADPCM_FORMAT_H

#include <stdbool.h>
#include <stdint.h>

#define CTR_ADPCM_HEADER_BYTES 128u
#define CTR_ADPCM_BLOCK_FRAMES 2044u
#define CTR_ADPCM_BLOCK_BYTES 1176u
#define CTR_ADPCM_STEREO_BLOCK_BYTES (2u * CTR_ADPCM_BLOCK_BYTES)

typedef struct {
    uint32_t rate, channels, frames, blocks;
    uint64_t fileBytes;
    int16_t coefficients[2][16];
} CtrAdpcmInfo;

typedef struct {
    uint16_t index;
    int16_t history0, history1;
} CtrAdpcmHistory;

typedef struct {
    const uint8_t* channel[2]; // payload, excluding the stored history
    CtrAdpcmHistory history[2];
    uint32_t frames, payloadBytes;
} CtrAdpcmBlock;

// The owner supplies a dedicated SD descriptor or a memory-backed read.
// A short read is failure; this layer never shares a descriptor's cursor.
typedef bool (*CtrAdpcmReadAt)(void* owner, uint64_t offset, void* out, uint32_t bytes);

bool CtrAdpcm_parse(const uint8_t* header, uint32_t headerBytes,
                    uint64_t fileBytes, CtrAdpcmInfo* out);
bool CtrAdpcm_blockRange(const CtrAdpcmInfo* info, uint32_t block,
                         uint64_t* offset, uint32_t* bytes, uint32_t* frames);
bool CtrAdpcm_readBlock(const CtrAdpcmInfo* info, CtrAdpcmReadAt readAt, void* owner,
                        uint32_t block, void* storage, uint32_t capacity, CtrAdpcmBlock* out);
// Decode only a bounded span inside one block, preserving source channel order.
// Native music queues the validated payload directly. The compressed SFX
// feeder uses CtrAdpcmPcm to continue histories across successive PCM pieces.
bool CtrAdpcm_decodeSpan(const CtrAdpcmInfo* info, const CtrAdpcmBlock* block,
                         uint32_t first, uint32_t count, int16_t* out, uint32_t capacityFrames);

#endif
