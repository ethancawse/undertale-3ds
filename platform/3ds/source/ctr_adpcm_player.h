// Native queue shared by SD streams and pinned compressed SFX.
#ifndef CTR_ADPCM_PLAYER_H
#define CTR_ADPCM_PLAYER_H

#ifdef CTR_ADPCM_PLAYER_HOST
#include "test_ndsp.h"
#else
#include <3ds.h>
#endif
#include "ctr_adpcm_format.h"

#define CTR_ADPCM_QUEUE_BLOCKS 4u

typedef struct {
    CtrAdpcmReadAt readAt;
    void* owner;
    uint64_t bytes;
    // Optional complete DTAD in linear memory, pinned until stop(). No copy
    // of its payload is queued. With NULL, storage is the SD streaming ring.
    const uint8_t* resident;
} CtrAdpcmSource;

enum { CTR_ADPCM_STOPPED, CTR_ADPCM_PRIMED, CTR_ADPCM_RUNNING,
       CTR_ADPCM_FINISHED, CTR_ADPCM_FAILED };

typedef struct {
    ndspWaveBuf wave[2];
    ndspAdpcmData context[2];
    uint32_t first, frames;
    bool occupied, queued, reading;
} CtrAdpcmPiece;

typedef struct {
    CtrAdpcmInfo info;
    CtrAdpcmSource source;
    CtrAdpcmPiece piece[CTR_ADPCM_QUEUE_BLOCKS];
    uint8_t* storage;
    uint32_t capacity, stride, nextBlock, lastPosition, generation;
    int channel[2];
    unsigned state, ring, publish;
    bool loop, paused, appliedPaused, activated;
} CtrAdpcmPlayer;

typedef struct {
    CtrAdpcmInfo info;
    CtrAdpcmSource source;
    CtrAdpcmBlock block;
    uint8_t* storage;
    uint32_t capacity, index, slot, generation;
    bool ok;
} CtrAdpcmRequest;

typedef struct {
    int16_t* pcm; // supplied storage, disjoint from the native queue's storage
    uint32_t capacityFrames;
    uint32_t first, frames; // exact source coordinates, including loop wraps
} CtrAdpcmBridgePiece;

// Bounded SD-backed PCM continuation after returning a borrowed channel.
// The game adapter keeps this workspace in ordinary heap, not linear memory.
// The copied source/owner stays pinned after stop() and through the last read.
// The workspace itself stays at a fixed address once read() fills its block.
typedef struct {
    CtrAdpcmInfo info;
    CtrAdpcmSource source;
    CtrAdpcmBlock block;
    uint32_t position, index;
    bool ready, failed;
    uint8_t storage[CTR_ADPCM_STEREO_BLOCK_BYTES];
} CtrAdpcmContinuation;

// The caller owns the physical channel leases and serializes every API call,
// including frame(), with one lock. Source/storage/player outlive all queued
// buffers. read() is the exception: it uses only its private request and runs
// OUTSIDE that lock, so SD latency never holds the NDSP callback. A cancelled
// request's source and storage stay pinned until read() returns, even if the
// player has stopped. A zero-initialized player is stopped. This layer neither steals
// logical game voices nor substitutes a different source on failure.
//
// Streaming: aligned linear storage >= 4 * channels * 1176 B. Resident SFX:
// ordinary scratch storage >= channels * 1176 B, plus a pinned linear DTAD.
// startFrame must be a DTAD block boundary; a rejected seek does not round it.
// The game adapter must use its existing exact PCM/Vorbis path otherwise.
bool CtrAdpcmPlayer_start(CtrAdpcmPlayer* player, const CtrAdpcmSource* source,
                          void* storage, uint32_t capacity, const int channels[2],
                          uint32_t startFrame, bool loop, bool paused);
// Opening on the worker is separate from claiming the channel. prepare()
// reads/prefills without touching any DSP channel; activate() performs no I/O
// and belongs under the game stream lock after its cancellation check.
bool CtrAdpcmPlayer_prepare(CtrAdpcmPlayer* player, const CtrAdpcmSource* source,
                            void* storage, uint32_t capacity, const int channels[2],
                            uint32_t startFrame, bool loop, bool paused);
bool CtrAdpcmPlayer_activate(CtrAdpcmPlayer* player);
// Initial prefill convenience, before the caller exposes this player to the
// callback. During playback use request()/read()/commit() instead.
bool CtrAdpcmPlayer_pump(CtrAdpcmPlayer* player);
// request() and commit() hold the caller's lock; read() never does. There is
// at most one outstanding request per player. A false request means the ring
// is full or the player is done; inspect state to distinguish failure/EOF.
bool CtrAdpcmPlayer_request(CtrAdpcmPlayer* player, CtrAdpcmRequest* request);
void CtrAdpcmPlayer_read(CtrAdpcmRequest* request);
bool CtrAdpcmPlayer_commit(CtrAdpcmPlayer* player, const CtrAdpcmRequest* request);
// In the NDSP callback, before libctru publishes channel updates: paired
// queue publication and pause/unpause, with no allocation, reads or decoding.
void CtrAdpcmPlayer_frame(CtrAdpcmPlayer* player);
void CtrAdpcmPlayer_pause(CtrAdpcmPlayer* player, bool paused);
void CtrAdpcmPlayer_rate(CtrAdpcmPlayer* player, float rate);
void CtrAdpcmPlayer_mix(CtrAdpcmPlayer* player, const float left[12], const float right[12]);
uint32_t CtrAdpcmPlayer_position(CtrAdpcmPlayer* player);
// Prepare a bounded PCM continuation from buffers already in the native queue.
// This is the channel-pressure handoff building block: no allocation, I/O or
// DSP mutation. Call at a DSP frame boundary under the player's lock, then
// publish the PCM continuation in that same callback before releasing leases.
// Storage/source remain pinned until the outstanding request has returned.
// At most four pieces; a short destination stops at its exact source frame.
// Failed preparation publishes zero pieces. This API alone does not switch
// channel formats or claim click-free resampler/loop-seam equivalence.
bool CtrAdpcmPlayer_bridge(CtrAdpcmPlayer* player, CtrAdpcmBridgePiece* pieces,
                           uint32_t count, uint32_t* used);
// start()/seek() allocate nothing and perform no I/O. read() runs on the
// existing audio worker outside the NDSP callback/locks; it never opens Vorbis.
// No block rounding is visible: EOF and every seek retain exact source frames.
bool CtrAdpcmContinuation_start(CtrAdpcmContinuation* cursor,
                                const CtrAdpcmPlayer* player, uint32_t first);
bool CtrAdpcmContinuation_seek(CtrAdpcmContinuation* cursor, uint32_t first);
int32_t CtrAdpcmContinuation_read(CtrAdpcmContinuation* cursor, int16_t* out,
                                 uint32_t capacityFrames);
// Hard clear only. The game adapter must arrange the existing stop tail before
// releasing its lease; this primitive does not claim click-free game cuts.
void CtrAdpcmPlayer_stop(CtrAdpcmPlayer* player);

#endif
