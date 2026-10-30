// Production bodies are inserted by test_sfx.py at the marker below.
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_adpcm_pcm.h"
#define VOICE_COUNT 24
#define UTA_RING 8
#define UTA_TAIL_MAX 160
#define UTA_SEAM_MAX 320
#define UTA_TAIL_SECONDS .003f
#define UTA_SEAM_SECONDS_X1000 3
#define UTA_DSP_RATE 32728.498
#define UTA_AHEAD_FRAMES 3
// PRODUCTION_CONSTANTS
#define AS_CODE
#define UTA_LATE
#define __real_calloc calloc
typedef unsigned int u32;
typedef unsigned short u16;
typedef int LightLock;
typedef struct {
    const void* data_vaddr;
    uint32_t nsamples;
    uint16_t sequence_id;
    unsigned status;
    bool looping;
} ndspWaveBuf;
enum { NDSP_WBUF_FREE, NDSP_WBUF_QUEUED, NDSP_WBUF_PLAYING, NDSP_WBUF_DONE };
enum { UTA_IDLE, UTA_FEED, UTA_PENDING, UTA_TAILING };
enum { UTA_PIECE_SOUND, UTA_PIECE_SEAM, UTA_PIECE_TAIL };
typedef struct { uint8_t* pcm; uint32_t bytes; } CachedSfx;
typedef struct { CachedSfx* sfx; ndspWaveBuf wb[2]; uint32_t wbStart[2]; } Voice;
// PRODUCTION_TYPES
static UtaChan utaChan[VOICE_COUNT];
static ndspWaveBuf utaWb[VOICE_COUNT][UTA_RING];
static uint8_t tail[VOICE_COUNT * UTA_TAIL_MAX * 4], seam[VOICE_COUNT * UTA_SEAM_MAX * 4];
static uint8_t* utaTailPcm = tail;
static uint8_t* utaSeamPcm = seam;
// PRODUCTION_VIEW
static struct {
    AdSfxMeta* metas;
    AdSfxPhysical physical[VOICE_COUNT];
    uint8_t* pool;
    AdSfxRing* rings;
    uint64_t ticks, calls, decodedFrames, maxTicks;
    uint32_t cores;
    uint32_t ringBytes, ringPeak, ringFailures;
    bool coreUnavailable;
} adSfx;
static uint64_t tick;
static int processor = 1;
static uint32_t processorCalls;
static uint32_t ringAllocations, ringFrees;
static bool denyRingAllocation;
static void* linearAlloc(uint32_t bytes) {
    if (denyRingAllocation) return NULL;
    ringAllocations++;
    return malloc(bytes);
}
// e848ef9da: release UTA_FAULT_ALLOC delegates to linearAlloc. Keep the
// fixture's denyRingAllocation control on that path.
#define UTA_FAULT_ALLOC(kind, bytes) linearAlloc(bytes)
static void linearFree(void* p) { ringFrees++; free(p); }
static void LightLock_Lock(LightLock* p) { (void)p; }
static void LightLock_Unlock(LightLock* p) { (void)p; }
static uint64_t svcGetSystemTick(void) { return ++tick; }
static int svcGetProcessorID(void) { processorCalls++; return processor; }
static void DSP_FlushDataCache(const void* p, uint32_t n) { (void)p; (void)n; }
// 2daeffa4a moved the platform cache flush behind adFlush. Cache maintenance
// is a platform service in this simulated DSP; PCM/lease/stop bodies stay exact.
static void adFlush(const void* p, uint32_t n) { DSP_FlushDataCache(p, n); }
static unsigned utaUnstarted, utaTails, utaPieces;
static void utaClearPhys(int p, UtaChan* c) { (void)p; c->state = UTA_IDLE; }
static int16_t queued[UTA_TAIL_MAX * 2];
static uint32_t queuedFrames;
static uint32_t queueOrder[UTA_RING], orderCounter, physicalPosition;
static uint16_t physicalSequence;
#define UTA_PHYS_SEQ(p) ((void)(p), physicalSequence)
#define UTA_PHYS_POS(p) ((void)(p), physicalPosition)
static void fakeAdd(int p, ndspWaveBuf* w) {
    if (w->nsamples <= UTA_TAIL_MAX) memcpy(queued, w->data_vaddr, w->nsamples * utaChan[p].bytes);
    queuedFrames = w->nsamples;
    w->status = NDSP_WBUF_QUEUED;
    w->sequence_id = (uint16_t)++orderCounter;
    queueOrder[w - utaWb[p]] = orderCounter;
}
#define UTA_PHYS_ADD(p, w) fakeAdd(p, w)
// PRODUCTION_FUNCTIONS

static void consume(uint32_t count, const int16_t* pcm, unsigned channels) {
    while (count) {
        int first = -1;
        for (unsigned k = 0; k < UTA_RING; k++) if (utaLive(&utaWb[0][k]) &&
            (first < 0 || queueOrder[k] < queueOrder[first])) first = (int)k;
        assert(first >= 0); // no starvation before a deliberately late callback
        ndspWaveBuf* w = &utaWb[0][first];
        if (w->status != NDSP_WBUF_PLAYING) {
            w->status = NDSP_WBUF_PLAYING;
            physicalPosition = 0;
            physicalSequence = w->sequence_id;
        }
        uint32_t n = w->nsamples - physicalPosition;
        if (n > count) n = count;
        uint32_t source = utaChan[0].piece[first].off + physicalPosition;
        assert(!memcmp((const int16_t*)w->data_vaddr + physicalPosition * channels,
            pcm + source * channels, n * channels * 2));
        physicalPosition += n;
        count -= n;
        if (physicalPosition == w->nsamples) w->status = NDSP_WBUF_DONE;
    }
}

static void checkTail(AdSfxMeta* meta, const UtaChan* input) {
    UtaChan saved = utaChan[0];
    ndspWaveBuf savedBuffers[UTA_RING];
    memcpy(savedBuffers, utaWb[0], sizeof(savedBuffers));
    utaChan[0] = *input;
    adSfx.physical[0].meta = NULL;
    adSfx.physical[0].tailReady = false;
    utaStop(0, &utaChan[0], 3);
    UtaChan old = utaChan[0];
    int16_t expected[UTA_TAIL_MAX * 2];
    uint32_t n = queuedFrames;
    memcpy(expected, queued, n * input->bytes);
    adSfx.physical[0].meta = meta;
    adSfx.physical[0].cursor = meta->initial;
    utaChan[0] = *input;
    memcpy(utaWb[0], savedBuffers, sizeof(savedBuffers));
    uint64_t calls = adSfx.calls;
    adSfxCaptureTail(0, &utaChan[0]);
    // A tail crosses at most one of these deliberately long segments.
    assert(adSfx.calls > calls && adSfx.calls - calls <= 2);
    utaStop(0, &utaChan[0], 3);
    assert(n == queuedFrames && memcmp(expected, queued, n * input->bytes) == 0);
    assert(memcmp(&old, &utaChan[0], sizeof(old)) == 0);
    utaChan[0] = saved;
    memcpy(utaWb[0], savedBuffers, sizeof(savedBuffers));
}

static void* load(const char* path, uint32_t* bytes) {
    FILE* f = fopen(path, "rb");
    assert(f && fseek(f, 0, SEEK_END) == 0);
    long size = ftell(f);
    assert(size > 0 && (unsigned long)size <= UINT32_MAX && fseek(f, 0, SEEK_SET) == 0);
    void* data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, f) == (size_t)size);
    fclose(f);
    *bytes = (uint32_t)size;
    return data;
}

int main(int argc, char** argv) {
    assert(argc == 3);
    uint32_t bytes, pcmBytes;
    uint8_t* data = load(argv[1], &bytes);
    int16_t* pcm = load(argv[2], &pcmBytes);
    CachedSfx cache = {.pcm = data, .bytes = bytes};
    AdSfxMeta meta = {0};
    meta.cache = &cache; meta.data = data;
    assert(CtrAdpcmPcm_open(&meta.initial, data, bytes));
    unsigned channels = meta.initial.info.channels;
    uint32_t frames = meta.initial.info.frames;
    assert(pcmBytes == frames * channels * 2 && frames > 6 * CTR_ADPCM_BLOCK_FRAMES);
    for (unsigned ch = 0; ch < channels; ch++) {
        meta.first[ch] = pcm[ch]; meta.prev[ch] = pcm[(frames - 2) * channels + ch];
        meta.last[ch] = pcm[(frames - 1) * channels + ch];
    }
    adSfx.metas = &meta;
    adSfx.pool = calloc(1, AS_POOL_BYTES);
    assert(adSfx.pool);
    Voice voice = {0}; voice.sfx = &cache;
    UtaChan* c = &utaChan[0];
    c->state = UTA_FEED; c->chans = (uint8_t)channels; c->bytes = (uint8_t)(channels * 2);
    c->rate = 44100; c->started = 1;
    adSfxSegment(0, c, &voice, &voice.wb[0]);
    c->seg[0].data = data; c->seg[0].frames = frames; c->segs = 1;
    for (uint32_t at = 0; at < frames;) {
        c->off = at;
        uint32_t n = frames - at < 654 ? frames - at : 654;
        const uint8_t* out = adSfxPiece(0, c, &c->seg[0], &n);
        assert(out && n && n <= AS_PIECE_FRAMES);
        assert(memcmp(out, (uint8_t*)pcm + at * c->bytes, n * c->bytes) == 0);
        at += n;
    }
    // Every live feeder slot retains its exact PCM through another decode.
    uint8_t borrowed[UTA_RING][AS_PIECE_FRAMES * 4];
    for (unsigned i = 0; i < UTA_RING; i++) {
        c->off = i * AS_PIECE_FRAMES;
        uint32_t n = 217;
        const uint8_t* out = adSfxPiece(0, c, &c->seg[0], &n);
        assert(out);
        utaWb[0][i].status = NDSP_WBUF_QUEUED;
        utaWb[0][i].data_vaddr = out;
        memcpy(borrowed[i], out, n * c->bytes);
    }
    c->off = UTA_RING * AS_PIECE_FRAMES;
    uint32_t n = 217;
    assert(!adSfxPiece(0, c, &c->seg[0], &n));
    for (unsigned i = 0; i < UTA_RING; i++)
        assert(!memcmp(borrowed[i], utaWb[0][i].data_vaddr, 217 * c->bytes));
    utaWb[0][2].status = NDSP_WBUF_DONE;
    assert(adSfxPiece(0, c, &c->seg[0], &n));
    for (unsigned i = 0; i < UTA_RING; i++) utaWb[0][i].status = NDSP_WBUF_DONE;
    // A high-pitch rate change grows only on the game thread. A later grow
    // cannot free the generation still referenced by a live wave buffer.
    assert(AS_POOL_BYTES == VOICE_COUNT * UTA_RING * AS_PIECE_FRAMES * AS_FRAME_BYTES && !adSfx.rings);
    adSfxRate(0, 96000);
    AdSfxRing* firstRing = adSfx.physical[0].ring;
    assert(firstRing && firstRing->frames >= 470u);
    c->off = 17;
    n = 469;
    const uint8_t* out = adSfxPiece(0, c, &c->seg[0], &n);
    assert(out && n == 469 && !memcmp(out, pcm + 17 * channels, n * c->bytes));
    utaWb[0][0].status = NDSP_WBUF_PLAYING;
    utaWb[0][0].data_vaddr = out;
    adSfxRate(0, 384000);
    AdSfxRing* secondRing = adSfx.physical[0].ring;
    assert(secondRing && secondRing != firstRing && secondRing->frames >= 1878u);
    adSfxReap();
    assert(!ringFrees && !memcmp(out, pcm + 17 * channels, 469 * c->bytes));
    uint32_t allocations = ringAllocations;
    c->off = 2041; n = 1878;
    out = adSfxPiece(0, c, &c->seg[0], &n);
    assert(out && n == 1878 && !memcmp(out, pcm + 2041 * channels, n * c->bytes));
    assert(ringAllocations == allocations); // callback allocation-free
    utaWb[0][0].status = NDSP_WBUF_DONE;
    adSfxReap();
    assert(ringFrees == 1 && adSfx.rings == secondRing);
    adSfxForget(0);
    adSfxReap();
    assert(!adSfx.rings && adSfx.ringBytes == 0 && ringFrees == 2);
    // Allocation failure leaves existing leases and the base pool intact;
    // it is observable in the diagnostic counter, never an invalid pointer.
    adSfx.physical[0].meta = &meta;
    denyRingAllocation = true;
    adSfxRate(0, 384000);
    assert(adSfx.ringFailures == 1 && !adSfx.physical[0].ring);
    denyRingAllocation = false;
    // Exercise the actual feeder's target/piece/ring logic at the full
    // admitted pitch range, including callbacks delayed by one DSP frame.
    const float feedRates[] = {8000, 44100, 48000, 96000, 384000};
    for (unsigned r = 0; r < sizeof(feedRates) / sizeof(feedRates[0]); r++) {
        memset(utaWb, 0, sizeof(utaWb));
        memset(c, 0, sizeof(*c));
        adSfxForget(0); adSfxReap();
        c->state = UTA_FEED; c->started = 1; c->rate = feedRates[r];
        c->chans = (uint8_t)channels; c->bytes = (uint8_t)(channels * 2);
        voice.wbStart[0] = 0;
        adSfxSegment(0, c, &voice, &voice.wb[0]);
        c->seg[0].data = (const uint8_t*)pcm;
        c->seg[0].frames = frames; c->seg[0].looping = true; c->segs = 1;
        physicalPosition = physicalSequence = 0;
        utaFeed(0, c);
        allocations = ringAllocations;
        uint32_t step = (uint32_t)(160.0 * (double)c->rate / UTA_DSP_RATE);
        for (unsigned frame = 0; frame < 1000; frame++) {
            consume(step * (frame % 11 == 10 ? 2u : 1u), pcm, channels);
            utaFeed(0, c);
        }
        assert(ringAllocations == allocations); // all allocation preceded callbacks
    }
    memset(utaWb, 0, sizeof(utaWb));
    adSfxForget(0); adSfxReap();
    c->state = UTA_FEED; c->rate = 44100;
    c->chans = (uint8_t)channels; c->bytes = (uint8_t)(channels * 2);
    c->segs = 0;
    adSfxSegment(0, c, &voice, &voice.wb[0]);
    // Arbitrary seek, a partial first segment, then a whole looping segment.
    voice.wbStart[0] = 2039; voice.wbStart[1] = 0;
    c->segs = 0;
    adSfxSegment(0, c, &voice, &voice.wb[0]);
    c->seg[0].frames = frames - 2039; c->segs = 1;
    adSfxSegment(0, c, &voice, &voice.wb[1]);
    c->seg[1].data = data; c->seg[1].frames = frames; c->seg[1].looping = true; c->segs = 2;
    for (unsigned loop = 0; loop < 50; loop++) {
        c->cur = loop == 0 ? 0 : 1;
        c->off = 0;
        n = 654;
        const uint8_t* out = adSfxPiece(0, c, &c->seg[c->cur], &n);
        uint32_t at = loop == 0 ? 2039 : 0;
        assert(out && memcmp(out, pcm + at * channels, n * c->bytes) == 0);
    }
    // Batched raw continuation must preserve the original tail at a codec
    // boundary, physical EOF, a loop, and a partial-to-whole transition.
    const float rates[] = {8000, 44100, 48000, 384000};
    const uint32_t positions[] = {17, 2039, frames - 7};
    adSfx.physical[0].start[0] = adSfx.physical[0].start[1] = 0;
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++)
        for (unsigned at = 0; at < sizeof(positions) / sizeof(positions[0]); at++)
            for (unsigned mode = 0; mode < 3; mode++) {
                UtaChan input = {0};
                input.state = UTA_FEED; input.started = 1; input.rate = rates[r];
                input.chans = (uint8_t)channels; input.bytes = (uint8_t)(channels * 2);
                input.trigger[0] = input.trigger[1] = 1;
                input.off = positions[at]; input.segs = mode == 2 ? 2 : 1;
                input.seg[0].data = (const uint8_t*)pcm;
                input.seg[0].frames = frames; input.seg[0].looping = mode == 1;
                input.seg[1] = input.seg[0]; input.seg[1].looping = true;
                checkTail(&meta, &input);
            }
    // The channel-projection path still matches independently decoded left
    // samples; production admits it only for verified identical channels.
    if (channels == 2) {
        int16_t projected[521];
        adSfx.physical[0].cursor = meta.initial;
        assert(adRead(&adSfx.physical[0], 2039, 521, projected, 1));
        for (unsigned j = 0; j < 521; j++) assert(projected[j] == pcm[(2039 + j) * 2]);
    }
    // Unsupported processor-ID service stays explicitly unknown and is
    // queried only once, rather than flooding the emulator's host log.
    processor = -1;
    uint32_t callsBefore = processorCalls;
    int16_t temporary[4];
    assert(adRead(&adSfx.physical[0], 0, 2, temporary, channels));
    assert(adRead(&adSfx.physical[0], 2, 2, temporary, channels));
    assert(adSfx.coreUnavailable && processorCalls == callsBefore + 1);
    // Original stop output against decoded PCM. Loop crossing is included.
    c->cur = 1; c->off = frames - 7; c->trigger[0] = c->trigger[1] = 1;
    c->seg[1].data = (const uint8_t*)pcm;
    adSfx.physical[0].meta = NULL;
    utaBuildSeam(0, c, &c->seg[1], 217);
    int16_t expectedSeam[UTA_SEAM_MAX * 2];
    memcpy(expectedSeam, utaSeamPcm, c->seamN * c->bytes);
    adSfx.physical[0].meta = &meta;
    utaBuildSeam(0, c, &c->seg[1], 217);
    assert(memcmp(expectedSeam, utaSeamPcm, c->seamN * c->bytes) == 0);
    UtaChan before = *c;
    ndspWaveBuf beforeBuffers[UTA_RING];
    memcpy(beforeBuffers, utaWb[0], sizeof(beforeBuffers));
    adSfx.physical[0].meta = NULL;
    adSfx.physical[0].tailReady = false;
    utaStop(0, c, 3);
    int16_t expected[UTA_TAIL_MAX * 2];
    memcpy(expected, queued, queuedFrames * c->bytes);
    *c = before;
    memcpy(utaWb[0], beforeBuffers, sizeof(beforeBuffers));
    adSfx.physical[0].meta = &meta;
    adSfxCaptureTail(0, c);
    assert(adSfx.physical[0].tailReady);
    // Eviction after clear cannot invalidate a deferred tail or its offsets.
    free(data); meta.data = NULL; cache.pcm = NULL;
    memset(&adSfx.physical[0].cursor, 0, sizeof(CtrAdpcmPcm));
    utaStop(0, c, 3);
    assert(memcmp(expected, queued, queuedFrames * c->bytes) == 0);
    adSfxForget(0);
    assert(!adSfx.physical[0].meta && !adSfx.physical[0].tailReady);
    assert(adSfx.cores == 2 && adSfx.calls && adSfx.decodedFrames);
    free(pcm); free(adSfx.pool);
    puts("SFX production pieces: leased generations, 5000 feeder callbacks through pitch 8, delayed callbacks, seeks, loops, 36 batched tails and channel projection passed");
    return 0;
}
