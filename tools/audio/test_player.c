// Device simulation exercises the real queue, not a second queue implementation.
#include "ctr_music_clock.h"
#include "ctr_adpcm_player.h"
#include "ctr_adpcm_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool paused;
    u16 sequence, nextSequence, format;
    u32 position;
    float rate, mix[12];
    s16 coefficients[16];
    ndspWaveBuf* queue[8];
    unsigned count;
    uint64_t consumed;
} Device;
static Device device[24];
static bool inCallback;
static unsigned reads, mutations;

void ndspChnReset(int c) { memset(&device[c], 0, sizeof(device[c])); mutations++; }
void ndspChnSetPaused(int c, bool p) { device[c].paused = p; mutations++; }
void ndspChnWaveBufClear(int c) {
    for (unsigned i = 0; i < device[c].count; i++) device[c].queue[i]->status = NDSP_WBUF_DONE;
    device[c].count = 0;
    mutations++;
}
void ndspChnSetFormat(int c, u16 f) { device[c].format = f; mutations++; }
void ndspChnSetAdpcmCoefs(int c, u16* co) { memcpy(device[c].coefficients, co, 32); mutations++; }
void ndspChnSetInterp(int c, int interpolation) { (void)c; assert(interpolation == 0); mutations++; }
void ndspChnSetRate(int c, float r) { device[c].rate = r; mutations++; }
void ndspChnSetMix(int c, float mix[12]) { memcpy(device[c].mix, mix, 48); mutations++; }
void ndspChnWaveBufAdd(int c, ndspWaveBuf* w) {
    assert(inCallback); // Both stereo publications must precede the next DSP update.
    Device* d = &device[c];
    assert(d->count < 8 && w->nsamples > 0 && !w->looping);
    assert(w->data_adpcm != NULL && w->adpcm_data != NULL);
    assert(w->adpcm_data->index == w->data_adpcm[0]);
    if (++d->nextSequence == 0) d->nextSequence = 1;
    w->sequence_id = d->nextSequence;
    w->status = NDSP_WBUF_QUEUED;
    d->queue[d->count++] = w;
    mutations++;
}
u16 ndspChnGetWaveBufSeq(int c) { return device[c].sequence; }
u32 ndspChnGetSamplePos(int c) { return device[c].position; }
void DSP_FlushDataCache(const void* data, u32 bytes) { assert(data != NULL && bytes > 0); }

static void callback(CtrAdpcmPlayer* p) {
    unsigned before = reads;
    inCallback = true;
    CtrAdpcmPlayer_frame(p);
    inCallback = false;
    assert(reads == before);
}

static void advance(int c, unsigned frames) {
    Device* d = &device[c];
    if (d->paused) return;
    while (frames && d->count) {
        ndspWaveBuf* w = d->queue[0];
        if (w->status == NDSP_WBUF_QUEUED) {
            w->status = NDSP_WBUF_PLAYING;
            d->sequence = w->sequence_id;
            d->position = 0;
        }
        unsigned remaining = w->nsamples - d->position;
        unsigned n = frames < remaining ? frames : remaining;
        d->position += n;
        d->consumed += n;
        frames -= n;
        if (d->position == w->nsamples) {
            w->status = NDSP_WBUF_DONE;
            memmove(d->queue, d->queue + 1, --d->count * sizeof(*d->queue));
        }
    }
}

typedef struct { uint8_t* data; uint32_t size; unsigned failAfter; CtrAdpcmFile* file; } Source;
static bool readAt(void* owner, uint64_t at, void* out, uint32_t bytes) {
    Source* s = owner;
    assert(!inCallback);
    reads++;
    if (reads > s->failAfter || at > s->size || bytes > s->size - at) return false;
    if (s->file) return CtrAdpcmStore_readAt(s->file, at, out, bytes);
    memcpy(out, s->data + (size_t)at, bytes);
    return true;
}

static void le32(uint8_t* p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(n >> (8 * i));
}

static Source fixture(unsigned channels, uint32_t frames) {
    uint32_t blocks = (frames + 2043) / 2044;
    uint32_t last = frames - (blocks - 1) * 2044;
    uint32_t bytes = 128 + (blocks - 1) * channels * 1176 + channels * (8 + ((last + 13) / 14) * 8);
    Source s = { .data = calloc(bytes + 31, 1), .size = bytes, .failAfter = ~0u };
    assert(s.data);
    memcpy(s.data, "DTAD", 4);
    le32(s.data + 4, 1); le32(s.data + 8, 128); le32(s.data + 12, 44100);
    le32(s.data + 16, channels); le32(s.data + 20, frames);
    le32(s.data + 28, 2044); le32(s.data + 32, blocks);
    uint8_t* p = s.data + 128;
    for (uint32_t b = 0; b < blocks; b++) {
        uint32_t count = b + 1 == blocks ? last : 2044;
        uint32_t payload = ((count + 13) / 14) * 8;
        for (unsigned ch = 0; ch < channels; ch++) {
            if (b != 0) { // Predictor zero, residual +1 or -1: known histories.
                p[2] = p[4] = ch ? 255 : 1;
                p[3] = p[5] = ch ? 255 : 0;
            }
            for (uint32_t j = 0; j < payload; j += 8) memset(p + 8 + j + 1, ch ? 255 : 17, 7);
            p += 8 + payload;
        }
    }
    return s;
}

static uint8_t* align32(uint8_t* p) { return (uint8_t*)(((uintptr_t)p + 31u) & ~(uintptr_t)31u); }

static void stream(unsigned channels, bool loop) {
    Source s = fixture(channels, 2044 * 9 + 17);
    CtrAdpcmSource src = { readAt, &s, s.size, NULL };
    uint8_t storage[CTR_ADPCM_QUEUE_BLOCKS * CTR_ADPCM_STEREO_BLOCK_BYTES + 31];
    CtrAdpcmPlayer p = {0};
    const int leases[2] = {3, 17};
    reads = 0;
    assert(CtrAdpcmPlayer_start(&p, &src, align32(storage), sizeof(storage) - 31, leases, 0, loop, false));
    assert(reads == 5 && device[3].count == 0 && device[3].paused);
    for (unsigned ch = 0; ch < channels; ch++) {
        assert(device[leases[ch]].format == NDSP_FORMAT_MONO_ADPCM);
        assert(device[leases[ch]].rate == 44100 * CTR_MUSIC_RATE_SCALE); // flowpitch v2
    }
    callback(&p);
    assert(device[3].count == 4 && !device[3].paused);
    float left[12] = {.25f, .125f}, right[12] = {.75f, .5f};
    CtrAdpcmPlayer_mix(&p, left, right);
    assert(memcmp(left, device[3].mix, sizeof(left)) == 0);
    if (channels == 2) assert(memcmp(right, device[17].mix, sizeof(right)) == 0);
    CtrAdpcmPlayer_rate(&p, 22050);
    assert(device[3].rate == 22050 * CTR_MUSIC_RATE_SCALE);
    CtrAdpcmPlayer_pause(&p, true);
    callback(&p);
    advance(3, 100);
    assert(device[3].consumed == 0);
    CtrAdpcmPlayer_pause(&p, false);
    callback(&p);
    uint64_t total = (uint64_t)p.info.frames * (loop ? 50u : 1u);
    while (device[3].consumed < total) {
        uint32_t step = (uint32_t)(total - device[3].consumed);
        if (step > 137) step = 137;
        for (unsigned ch = 0; ch < channels; ch++) advance(leases[ch], step);
        uint32_t pos = CtrAdpcmPlayer_position(&p);
        uint32_t expected = (uint32_t)(device[3].consumed % p.info.frames);
        assert(pos == expected || (expected == 0 && pos == p.info.frames));
        assert(CtrAdpcmPlayer_pump(&p));
        callback(&p);
        if (channels == 2) assert(device[3].consumed == device[17].consumed);
    }
    if (!loop) {
        assert(p.state == CTR_ADPCM_FINISHED && CtrAdpcmPlayer_position(&p) == p.info.frames);
        assert(device[3].count == 0);
    }
    CtrAdpcmPlayer_stop(&p);
    assert(p.state == CTR_ADPCM_STOPPED && device[3].count == 0);
    free(s.data);
}

static void boundaries_and_failures(void) {
    Source s = fixture(2, 4097);
    CtrAdpcmSource src = {readAt, &s, s.size, NULL};
    uint8_t storage[9408 + 31];
    CtrAdpcmPlayer p = {0};
    int channels[2] = {5, 9};
    unsigned before = mutations;
    assert(!CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, channels, 1, false, false));
    assert(!CtrAdpcmPlayer_start(&p, &src, align32(storage), 9407, channels, 0, false, false));
    channels[1] = channels[0];
    assert(!CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, channels, 0, false, false));
    channels[1] = 24;
    assert(!CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, channels, 0, false, false));
    channels[1] = 9;
    assert(mutations == before); // Admission failures don't touch another voice.
    reads = 0;
    s.failAfter = 2; // Header + first block, then a short read during prefill.
    assert(!CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, channels, 0, false, false));
    assert(p.state == CTR_ADPCM_STOPPED && device[5].count == 0 && device[9].count == 0);
    s.failAfter = ~0u;
    reads = 0;
    assert(CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, channels, 2044, false, false));
    callback(&p);
    advance(5, 2044); advance(9, 2044);
    assert(CtrAdpcmPlayer_position(&p) == 4088);
    advance(5, 9); advance(9, 9);
    assert(CtrAdpcmPlayer_pump(&p) && p.state == CTR_ADPCM_FINISHED);
    CtrAdpcmPlayer_stop(&p);
    // Validate the actual resident bytes, not an unrelated reader callback.
    uint8_t* allocation = malloc(s.size + 31);
    uint8_t* resident = align32(allocation);
    memcpy(resident, s.data, s.size);
    src.resident = resident;
    reads = 0;
    assert(CtrAdpcmPlayer_start(&p, &src, storage, 2352, channels, 0, false, false));
    assert(reads == 0);
    callback(&p);
    assert(device[5].queue[0]->data_adpcm == resident + 136);
    assert(device[9].queue[0]->data_adpcm == resident + 1312);
    assert(device[5].queue[2]->nsamples == 9); // Not its 14-sample padded frame.
    CtrAdpcmPlayer_stop(&p);
    resident[136] = 0x80; // Invalid predictor in actual queued memory.
    assert(!CtrAdpcmPlayer_start(&p, &src, storage, 2352, channels, 0, false, false));
    assert(p.state == CTR_ADPCM_STOPPED);
    free(allocation);
    free(s.data);
}

static void refill_stall(void) {
    Source s = fixture(2, 2044 * 10);
    CtrAdpcmSource src = {readAt, &s, s.size, NULL};
    uint8_t storage[9408 + 31];
    CtrAdpcmPlayer p = {0};
    int channels[2] = {7, 8};
    reads = 0;
    assert(CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, channels, 0, false, false));
    callback(&p);
    advance(7, 100000); advance(8, 100000); // The producer missed its entire ring.
    assert(device[7].count == 0 && device[8].count == 0);
    assert(CtrAdpcmPlayer_pump(&p));
    assert(device[7].count == 0 && device[8].count == 0); // No half-published stereo restart.
    callback(&p);
    advance(7, 123); advance(8, 123);
    assert(device[7].consumed == device[8].consumed);
    assert(CtrAdpcmPlayer_position(&p) == 2044 * 4 + 123);
    advance(7, 100000); advance(8, 100000);
    s.failAfter = reads; // A later I/O failure preserves queued storage until stop.
    assert(!CtrAdpcmPlayer_pump(&p) && p.state == CTR_ADPCM_FAILED);
    CtrAdpcmPlayer_stop(&p);
    assert(device[7].count == 0 && device[8].count == 0);
    free(s.data);
}

static void asynchronous_cancel(void) {
    Source s = fixture(2, 2044 * 10);
    CtrAdpcmSource src = {readAt, &s, s.size, NULL};
    uint8_t firstStorage[9408 + 31], nextStorage[9408 + 31];
    CtrAdpcmPlayer p = {0};
    CtrAdpcmRequest r, other;
    int channels[2] = {11, 12};
    reads = 0;
    assert(CtrAdpcmPlayer_start(&p, &src, align32(firstStorage), 9408, channels, 0, true, false));
    callback(&p);
    advance(11, 2044);
    assert(!CtrAdpcmPlayer_request(&p, &r)); // The right channel still owns this ring slot.
    advance(12, 2044);
    assert(CtrAdpcmPlayer_request(&p, &r));
    assert(!CtrAdpcmPlayer_request(&p, &other)); // Exactly one producer request.
    unsigned before = reads;
    callback(&p); // A producer waiting on SD doesn't exclude this callback.
    assert(reads == before);
    CtrAdpcmPlayer_read(&r);
    assert(CtrAdpcmPlayer_commit(&p, &r));
    callback(&p);
    advance(11, 2044); advance(12, 2044);
    assert(CtrAdpcmPlayer_request(&p, &other));
    assert(!CtrAdpcmPlayer_commit(&p, &r)); // An old completed request cannot commit twice.
    CtrAdpcmPlayer_stop(&p);
    assert(CtrAdpcmPlayer_start(&p, &src, align32(nextStorage), 9408, channels, 0, false, false));
    CtrAdpcmPlayer_read(&other); // Its source and OLD storage stayed pinned.
    assert(!CtrAdpcmPlayer_commit(&p, &other));
    callback(&p);
    advance(11, 27); advance(12, 27);
    assert(CtrAdpcmPlayer_position(&p) == 27);
    CtrAdpcmPlayer_stop(&p);
    free(s.data);
}

static void prepared_lease(void) {
    Source s = fixture(1, 2044 * 7);
    CtrAdpcmSource src = {readAt, &s, s.size, NULL};
    uint8_t storage[4704 + 31];
    CtrAdpcmPlayer p = {0};
    int channels[2] = {15, -1};
    reads = 0;
    unsigned before = mutations;
    assert(CtrAdpcmPlayer_prepare(&p, &src, align32(storage), 4704, channels, 0, false, false));
    callback(&p);
    assert(!p.activated && mutations == before && CtrAdpcmPlayer_position(&p) == 0);
    CtrAdpcmPlayer_stop(&p); // A cancelled open must not clear a newer voice's channel.
    assert(mutations == before);
    s.failAfter = reads + 2; // Header and first block pass; prefill fails later.
    assert(!CtrAdpcmPlayer_prepare(&p, &src, align32(storage), 4704, channels, 0, false, false));
    assert(p.state == CTR_ADPCM_STOPPED && mutations == before);
    s.failAfter = ~0u;
    assert(CtrAdpcmPlayer_prepare(&p, &src, align32(storage), 4704, channels, 0, false, false));
    unsigned readBefore = reads;
    assert(CtrAdpcmPlayer_activate(&p));
    assert(reads == readBefore);
    before = mutations;
    assert(!CtrAdpcmPlayer_activate(&p) && mutations == before && reads == readBefore);
    callback(&p);
    advance(15, 193);
    assert(CtrAdpcmPlayer_position(&p) == 193);
    CtrAdpcmPlayer_stop(&p);
    free(s.data);
}

static void pcm_handoff(unsigned channels, uint32_t frames, bool loop,
                        const Source* input, const int16_t* reference) {
    Source s = input ? *input : fixture(channels, frames);
    CtrAdpcmSource src = {readAt, &s, s.size, NULL};
    uint8_t storage[9408 + 31];
    CtrAdpcmPlayer p = {0};
    int leases[2] = {19, 20};
    int16_t pcm[3][2044 * 2];
    CtrAdpcmBridgePiece pieces[3] = {{pcm[0], 2044, 0, 0},
                                   {pcm[1], 2044, 0, 0}, {pcm[2], 2044, 0, 0}};
    reads = 0;
    assert(CtrAdpcmPlayer_start(&p, &src, align32(storage), 9408, leases, 0, loop, false));
    for (unsigned step = 0; step < 500; step++) {
        CtrAdpcmPlayer snapshot = p;
        uint32_t used = 99;
        unsigned readBefore = reads, mutationsBefore = mutations;
        inCallback = true;
        assert(CtrAdpcmPlayer_bridge(&p, pieces, 3, &used));
        inCallback = false;
        assert(used > 0 && used <= 3 && reads == readBefore && mutations == mutationsBefore);
        assert(memcmp(&snapshot, &p, sizeof(p)) == 0);
        uint32_t first = (uint32_t)(device[19].consumed % frames);
        for (uint32_t i = 0; i < used; i++) {
            assert(pieces[i].first == first && pieces[i].frames > 0);
            for (uint32_t j = 0; j < pieces[i].frames; j++)
                for (uint32_t c = 0; c < channels; c++)
                    assert(pieces[i].pcm[j * channels + c] ==
                        (reference ? reference[(first + j) * channels + c] : (c ? -1 : 1)));
            first += pieces[i].frames;
            if (loop && first == frames) first = 0;
        }
        pieces[0].capacityFrames = step % 2 ? 17 : 2044;
        callback(&p);
        unsigned advanceBy = 1u + (step * 31u) % 2111u;
        if (!loop && advanceBy >= frames - device[19].consumed) break;
        for (unsigned c = 0; c < channels; c++) advance(leases[c], advanceBy);
        assert(CtrAdpcmPlayer_pump(&p));
        callback(&p);
    }
    if (channels == 2) {
        callback(&p);
        advance(19, 1); // Never hide a stereo clock mismatch in a handoff.
        uint32_t used = 99;
        assert(!CtrAdpcmPlayer_bridge(&p, pieces, 3, &used) && used == 0);
    }
    CtrAdpcmContinuation continuation;
    assert(CtrAdpcmContinuation_start(&continuation, &p, frames > 2044 ? 2041 : 0));
    CtrAdpcmPlayer_stop(&p);
    uint32_t used = 99;
    assert(!CtrAdpcmPlayer_bridge(&p, pieces, 3, &used) && used == 0);
    // Returning both native leases does not round the continuation's cursor
    // or invalidate its dedicated SD owner. Reads belong to the audio worker.
    for (unsigned step = 0; step < 100; step++) {
        uint32_t first = step ? (step * 251u) % frames : continuation.position;
        unsigned before = reads;
        assert(CtrAdpcmContinuation_seek(&continuation, first));
        assert(reads == before);
        uint32_t n = frames - first;
        if (n > 2044) n = 2044;
        assert(CtrAdpcmContinuation_read(&continuation, pcm[0], n) == (int32_t)n);
        assert(continuation.position == first + n);
        for (uint32_t j = 0; j < n; j++) for (unsigned c = 0; c < channels; c++)
            assert(pcm[0][j * channels + c] ==
                (reference ? reference[(first + j) * channels + c] : (c ? -1 : 1)));
    }
    assert(CtrAdpcmContinuation_seek(&continuation, frames));
    assert(CtrAdpcmContinuation_read(&continuation, pcm[0], 17) == 0);
    assert(!CtrAdpcmContinuation_seek(&continuation, frames + 1));
    continuation.ready = false;
    assert(CtrAdpcmContinuation_seek(&continuation, 0));
    s.failAfter = reads;
    assert(CtrAdpcmContinuation_read(&continuation, pcm[0], 17) == -1);
    assert(continuation.position == 0 && continuation.failed);
    s.failAfter = ~0u;
    assert(CtrAdpcmContinuation_seek(&continuation, 1));
    assert(CtrAdpcmContinuation_read(&continuation, pcm[0], 1) == 1 && continuation.position == 2);
    if (!input) free(s.data);
}

static uint8_t* load(const char* path, uint32_t* bytes) {
    FILE* file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    assert(size > 0 && (unsigned long)size < 0xffffffffu && fseek(file, 0, SEEK_SET) == 0);
    uint8_t* data = malloc((size_t)size);
    assert(data && fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file);
    *bytes = (uint32_t)size;
    return data;
}

int main(int argc, char** argv) {
    if (argc == 3) {
        Source source = {0};
        source.data = load(argv[1], &source.size);
        source.failAfter = ~0u;
        uint32_t pcmBytes;
        int16_t* reference = (int16_t*)load(argv[2], &pcmBytes);
        CtrAdpcmInfo info;
        assert(CtrAdpcm_parse(source.data, source.size, source.size, &info));
        assert(pcmBytes == info.frames * info.channels * 2u);
        pcm_handoff(info.channels, info.frames, true, &source, reference);
        pcm_handoff(info.channels, info.frames, false, &source, reference);
        CtrAdpcmFile file = {0};
        file.file = fopen(argv[1], "rb");
        file.bytes = source.size;
        file.info = info;
        assert(file.file);
        source.file = &file;
        pcm_handoff(info.channels, info.frames, true, &source, reference);
        pcm_handoff(info.channels, info.frames, false, &source, reference);
        CtrAdpcmStore_closeFile(&file);
        free(reference); free(source.data);
        puts("Native SD queue/PCM bridge: dedicated file reads, independent PCM, exact offsets and loop wraps passed");
        return 0;
    }
    assert(argc == 1);
    for (unsigned channels = 1; channels <= 2; channels++) {
        pcm_handoff(channels, 2044 * 9 + 17, true, NULL, NULL);
        pcm_handoff(channels, 17, true, NULL, NULL);
        pcm_handoff(channels, 4097, false, NULL, NULL);
    }
    stream(1, false); stream(2, false);
    stream(1, true); stream(2, true);
    boundaries_and_failures(); refill_stall(); asynchronous_cancel(); prepared_lease();
    puts("Native queue: mono/stereo, 50 loops, exact final frames, positions, pause, resident cache, short reads, refill stalls, cancelled I/O and bounded PCM bridges passed");
    return 0;
}
