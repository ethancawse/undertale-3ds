#include "ctr_adpcm_player.h"
#include "ctr_music_clock.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

#ifdef CTR_ADPCM_PLAYER_HOST
#define AP_CODE(name)
#define AP_INIT(name)
#else
#define AP_CODE(name) __attribute__((noinline, noipa, section(".text.late.adpcm.player." #name)))
#define AP_INIT(name) AP_CODE(name) __attribute__((optimize("Os")))
#endif

static inline __attribute__((always_inline)) bool live(const CtrAdpcmPlayer* p) {
    return p != NULL && (p->state == CTR_ADPCM_PRIMED || p->state == CTR_ADPCM_RUNNING);
}

static inline __attribute__((always_inline)) bool pending(const CtrAdpcmPlayer* p, unsigned slot) {
    const CtrAdpcmPiece* b = &p->piece[slot];
    if (!b->occupied) return false;
    if (!b->queued) return true;
    for (uint32_t ch = 0; ch < p->info.channels; ch++)
        if (b->wave[ch].status != NDSP_WBUF_DONE) return true;
    return false;
}

AP_CODE(source) static bool readSource(void* owner, uint64_t offset, void* out, uint32_t bytes) {
    const CtrAdpcmSource* source = owner;
    if (offset > source->bytes || bytes > source->bytes - offset) return false;
    if (source->resident) {
        memcpy(out, source->resident + (size_t)offset, bytes);
        return true;
    }
    return source->readAt(source->owner, offset, out, bytes);
}

AP_CODE(stop) void CtrAdpcmPlayer_stop(CtrAdpcmPlayer* p) {
    if (p == NULL || p->state == CTR_ADPCM_STOPPED) return;
    // Clear synchronously detaches libctru's pointers before storage is freed.
    for (uint32_t ch = 0; p->activated && ch < p->info.channels; ch++) {
        ndspChnSetPaused(p->channel[ch], true);
        ndspChnWaveBufClear(p->channel[ch]);
    }
    uint32_t generation = p->generation + 1u;
    memset(p, 0, sizeof(*p));
    p->generation = generation;
}

AP_CODE(request) bool CtrAdpcmPlayer_request(CtrAdpcmPlayer* p, CtrAdpcmRequest* r) {
    if (!live(p) || r == NULL) return false;
    if (p->nextBlock == p->info.blocks && !p->loop) {
        bool any = false;
        for (unsigned slot = 0; slot < CTR_ADPCM_QUEUE_BLOCKS; slot++)
            any |= pending(p, slot) || p->piece[slot].reading;
        if (!any) {
            p->lastPosition = p->info.frames;
            p->state = CTR_ADPCM_FINISHED;
        }
        return false;
    }
    unsigned slot = p->ring;
    CtrAdpcmPiece* b = &p->piece[slot];
    if (pending(p, slot) || b->reading) return false;
    b->occupied = false;
    b->reading = true;
    memset(r, 0, sizeof(*r));
    r->info = p->info;
    r->source = p->source;
    r->index = p->nextBlock == p->info.blocks ? 0 : p->nextBlock;
    r->storage = p->storage + (p->source.resident ? 0 : slot * p->stride);
    r->capacity = p->stride;
    r->slot = slot;
    r->generation = ++p->generation;
    return true;
}

AP_CODE(read) void CtrAdpcmPlayer_read(CtrAdpcmRequest* r) {
    if (r != NULL) r->ok = CtrAdpcm_readBlock(&r->info, readSource, &r->source,
        r->index, r->storage, r->capacity, &r->block);
}

AP_CODE(commit) bool CtrAdpcmPlayer_commit(CtrAdpcmPlayer* p, const CtrAdpcmRequest* r) {
    if (!live(p) || r == NULL || r->generation != p->generation ||
        r->slot != p->ring || !p->piece[p->ring].reading) return false;
    CtrAdpcmPiece* b = &p->piece[p->ring];
    b->reading = false;
    if (!r->ok) {
        p->state = CTR_ADPCM_FAILED;
        return false;
    }
    const CtrAdpcmBlock* block = &r->block;
    b->first = r->index * CTR_ADPCM_BLOCK_FRAMES;
    b->frames = block->frames;
    memset(b->wave, 0, sizeof(b->wave));
    for (uint32_t ch = 0; ch < p->info.channels; ch++) {
        ndspWaveBuf* w = &b->wave[ch];
        b->context[ch].index = block->history[ch].index;
        b->context[ch].history0 = block->history[ch].history0;
        b->context[ch].history1 = block->history[ch].history1;
        w->data_adpcm = (uint8_t*)block->channel[ch];
        if (p->source.resident) {
            uint64_t at;
            uint32_t bytes, frames;
            if (!CtrAdpcm_blockRange(&p->info, r->index, &at, &bytes, &frames)) {
                p->state = CTR_ADPCM_FAILED;
                return false;
            }
            w->data_adpcm = (uint8_t*)p->source.resident + (size_t)at +
                ch * (block->payloadBytes + 8u) + 8u;
        }
        w->nsamples = block->frames; // final DSP-frame padding is never audible
        w->adpcm_data = &b->context[ch];
        DSP_FlushDataCache(w->data_adpcm, block->payloadBytes);
    }
    // Publication belongs to frame(), so an I/O stall cannot make the two
    // channels of a stereo underrun restart in different DSP updates.
    b->occupied = true;
    b->queued = false;
    p->nextBlock = r->index + 1u;
    p->ring = (p->ring + 1u) % CTR_ADPCM_QUEUE_BLOCKS;
    return true;
}

AP_CODE(pump) bool CtrAdpcmPlayer_pump(CtrAdpcmPlayer* p) {
    CtrAdpcmRequest r;
    for (unsigned n = 0; n < CTR_ADPCM_QUEUE_BLOCKS; n++) {
        if (!CtrAdpcmPlayer_request(p, &r)) break;
        CtrAdpcmPlayer_read(&r);
        if (!CtrAdpcmPlayer_commit(p, &r)) return false;
    }
    return p != NULL && p->state != CTR_ADPCM_FAILED && p->state != CTR_ADPCM_STOPPED;
}

AP_INIT(prepare) bool CtrAdpcmPlayer_prepare(CtrAdpcmPlayer* p, const CtrAdpcmSource* source,
                                 void* storage, uint32_t capacity, const int channels[2],
                                 uint32_t first, bool loop, bool paused) {
    if (p == NULL || p->state != CTR_ADPCM_STOPPED || source == NULL ||
        (source->readAt == NULL && source->resident == NULL) ||
        (source->resident && source->bytes > SIZE_MAX) ||
        channels == NULL || storage == NULL || first % CTR_ADPCM_BLOCK_FRAMES != 0) return false;
    uint8_t header[CTR_ADPCM_HEADER_BYTES];
    CtrAdpcmInfo info;
    if (!readSource((void*)source, 0, header, sizeof(header)) ||
        !CtrAdpcm_parse(header, sizeof(header), source->bytes, &info) || first >= info.frames) return false;
    uint32_t stride = info.channels * CTR_ADPCM_BLOCK_BYTES;
    uint32_t need = stride * (source->resident ? 1u : CTR_ADPCM_QUEUE_BLOCKS);
    if (capacity < need || (!source->resident && ((uintptr_t)storage & 31u) != 0) ||
        (source->resident && ((uintptr_t)source->resident & 31u) != 0)) return false;
    for (uint32_t ch = 0; ch < info.channels; ch++)
        if (channels[ch] < 0 || channels[ch] >= 24 || (ch != 0 && channels[ch] == channels[0])) return false;
    uint32_t generation = p->generation;
    memset(p, 0, sizeof(*p));
    p->generation = generation;
    p->info = info;
    p->source = *source;
    p->storage = storage;
    p->capacity = capacity;
    p->stride = stride;
    p->nextBlock = first / CTR_ADPCM_BLOCK_FRAMES;
    p->lastPosition = first;
    p->loop = loop;
    p->paused = paused;
    p->appliedPaused = true;
    p->state = CTR_ADPCM_PRIMED;
    for (uint32_t ch = 0; ch < info.channels; ch++) p->channel[ch] = channels[ch];
    if (!CtrAdpcmPlayer_pump(p)) {
        CtrAdpcmPlayer_stop(p);
        return false;
    }
    return true;
}

AP_INIT(activate) bool CtrAdpcmPlayer_activate(CtrAdpcmPlayer* p) {
    if (!live(p) || p->activated) return false;
    for (uint32_t ch = 0; ch < p->info.channels; ch++) {
        int channel = p->channel[ch];
        ndspChnReset(channel);
        ndspChnSetPaused(channel, true);
        ndspChnSetFormat(channel, NDSP_FORMAT_MONO_ADPCM);
        ndspChnSetAdpcmCoefs(channel, (u16*)p->info.coefficients[ch]);
        ndspChnSetInterp(channel, NDSP_INTERP_POLYPHASE);
        // flowpitch v2: every voice at the game's frame clock (ctr_music_clock.h); this file sets the DSP rate itself.
        ndspChnSetRate(channel, (float)p->info.rate * CTR_MUSIC_RATE_SCALE);
    }
    p->activated = true;
    return true;
}

AP_CODE(start) bool CtrAdpcmPlayer_start(CtrAdpcmPlayer* p, const CtrAdpcmSource* source,
    void* storage, uint32_t capacity, const int channels[2], uint32_t first, bool loop, bool paused) {
    return CtrAdpcmPlayer_prepare(p, source, storage, capacity, channels, first, loop, paused) &&
        CtrAdpcmPlayer_activate(p);
}

AP_CODE(frame) void CtrAdpcmPlayer_frame(CtrAdpcmPlayer* p) {
    if (!live(p) || !p->activated) return;
    for (unsigned n = 0; n < CTR_ADPCM_QUEUE_BLOCKS; n++) {
        CtrAdpcmPiece* b = &p->piece[p->publish];
        if (!b->occupied || b->queued) break;
        for (uint32_t ch = 0; ch < p->info.channels; ch++)
            ndspChnWaveBufAdd(p->channel[ch], &b->wave[ch]);
        b->queued = true;
        p->publish = (p->publish + 1u) % CTR_ADPCM_QUEUE_BLOCKS;
    }
    // In particular, don't resume an empty queue after an opening failure.
    if (p->appliedPaused != p->paused) {
        for (uint32_t ch = 0; ch < p->info.channels; ch++) ndspChnSetPaused(p->channel[ch], p->paused);
        p->appliedPaused = p->paused;
    }
    p->state = CTR_ADPCM_RUNNING;
}

AP_CODE(pause) void CtrAdpcmPlayer_pause(CtrAdpcmPlayer* p, bool paused) {
    if (live(p)) p->paused = paused;
}

AP_CODE(rate) void CtrAdpcmPlayer_rate(CtrAdpcmPlayer* p, float rate) {
    if (!live(p) || !p->activated || !isfinite(rate) || rate <= 0.0f) return;
    for (uint32_t ch = 0; ch < p->info.channels; ch++) ndspChnSetRate(p->channel[ch], rate * CTR_MUSIC_RATE_SCALE);
}

AP_CODE(mix) void CtrAdpcmPlayer_mix(CtrAdpcmPlayer* p, const float left[12], const float right[12]) {
    if (!live(p) || !p->activated || left == NULL || (p->info.channels == 2 && right == NULL)) return;
    ndspChnSetMix(p->channel[0], (float*)left);
    if (p->info.channels == 2) ndspChnSetMix(p->channel[1], (float*)right);
}

AP_CODE(position) uint32_t CtrAdpcmPlayer_position(CtrAdpcmPlayer* p) {
    if (p == NULL) return 0;
    if (!live(p) || !p->activated) return p->lastPosition;
    u16 seq = ndspChnGetWaveBufSeq(p->channel[0]);
    u32 pos = ndspChnGetSamplePos(p->channel[0]);
    for (unsigned slot = 0; slot < CTR_ADPCM_QUEUE_BLOCKS; slot++) {
        const CtrAdpcmPiece* b = &p->piece[slot];
        if (b->occupied && b->wave[0].sequence_id == seq) {
            if (pos > b->frames) pos = b->frames;
            p->lastPosition = b->first + pos;
            break;
        }
    }
    return p->lastPosition;
}

AP_CODE(bridge) bool CtrAdpcmPlayer_bridge(CtrAdpcmPlayer* p, CtrAdpcmBridgePiece* out,
                                  uint32_t count, uint32_t* used) {
    if (used == NULL) return false;
    *used = 0;
    if (!live(p) || !p->activated || out == NULL || count == 0 || count > CTR_ADPCM_QUEUE_BLOCKS) return false;
    for (uint32_t i = 0; i < count; i++) {
        out[i].frames = 0;
        if (out[i].pcm == NULL || out[i].capacityFrames == 0) return false;
    }
    u16 sequence = ndspChnGetWaveBufSeq(p->channel[0]);
    uint32_t position = ndspChnGetSamplePos(p->channel[0]);
    unsigned first = CTR_ADPCM_QUEUE_BLOCKS;
    uint32_t offset = 0;
    for (unsigned i = 0; i < CTR_ADPCM_QUEUE_BLOCKS; i++) {
        const CtrAdpcmPiece* b = &p->piece[i];
        if (!sequence || !b->occupied || !b->queued || b->wave[0].sequence_id != sequence) continue;
        if (position > b->frames) return false;
        if (p->info.channels == 2 &&
            (ndspChnGetWaveBufSeq(p->channel[1]) != b->wave[1].sequence_id ||
             ndspChnGetSamplePos(p->channel[1]) != position)) return false;
        first = i;
        offset = position;
        break;
    }
    if (first == CTR_ADPCM_QUEUE_BLOCKS) {
        // Before first publication, or after a drained queue was refilled.
        // ring is the next producer slot; pending slots after it are oldest.
        for (unsigned n = 0; n < CTR_ADPCM_QUEUE_BLOCKS; n++) {
            unsigned i = (p->ring + n) % CTR_ADPCM_QUEUE_BLOCKS;
            if (pending(p, i) && !p->piece[i].reading) { first = i; break; }
        }
        if (first == CTR_ADPCM_QUEUE_BLOCKS) return false;
    }
    uint32_t written = 0;
    for (unsigned n = 0; n < CTR_ADPCM_QUEUE_BLOCKS && written < count; n++) {
        unsigned slot = (first + n) % CTR_ADPCM_QUEUE_BLOCKS;
        const CtrAdpcmPiece* b = &p->piece[slot];
        if (!b->occupied || b->reading || offset > b->frames) break;
        if (offset == b->frames) { offset = 0; continue; }
        if (!pending(p, slot)) break;
        CtrAdpcmBlock block = {0};
        block.frames = b->frames;
        block.payloadBytes = ((b->frames + 13u) / 14u) * 8u;
        for (uint32_t ch = 0; ch < p->info.channels; ch++) {
            block.channel[ch] = b->wave[ch].data_adpcm;
            block.history[ch].index = b->context[ch].index;
            block.history[ch].history0 = b->context[ch].history0;
            block.history[ch].history1 = b->context[ch].history1;
        }
        CtrAdpcmBridgePiece* piece = &out[written];
        uint32_t frames = b->frames - offset;
        if (frames > piece->capacityFrames) frames = piece->capacityFrames;
        if (!CtrAdpcm_decodeSpan(&p->info, &block, offset, frames, piece->pcm, piece->capacityFrames)) {
            for (uint32_t i = 0; i < count; i++) out[i].frames = 0;
            return false;
        }
        piece->first = b->first + offset;
        piece->frames = frames;
        written++;
        if (frames < b->frames - offset) break;
        offset = 0;
    }
    *used = written;
    return written != 0;
}

AP_CODE(continuationStart) bool CtrAdpcmContinuation_start(CtrAdpcmContinuation* cursor,
                                       const CtrAdpcmPlayer* p, uint32_t first) {
    if (cursor == NULL || !live(p) || first > p->info.frames) return false;
    memset(cursor, 0, sizeof(*cursor));
    cursor->info = p->info;
    cursor->source = p->source;
    cursor->position = first;
    return true;
}

AP_CODE(continuationSeek) bool CtrAdpcmContinuation_seek(CtrAdpcmContinuation* cursor, uint32_t first) {
    if (cursor == NULL || cursor->info.frames == 0 || first > cursor->info.frames) return false;
    cursor->position = first;
    cursor->failed = false;
    return true;
}

AP_CODE(continuationRead) int32_t CtrAdpcmContinuation_read(CtrAdpcmContinuation* cursor, int16_t* out, uint32_t capacity) {
    if (cursor == NULL || out == NULL || cursor->info.frames == 0 || cursor->failed ||
        cursor->position > cursor->info.frames || capacity > INT32_MAX) return -1;
    uint32_t done = 0;
    while (done < capacity && cursor->position < cursor->info.frames) {
        uint32_t block = cursor->position / CTR_ADPCM_BLOCK_FRAMES;
        if (!cursor->ready || cursor->index != block) {
            cursor->ready = false;
            if (!CtrAdpcm_readBlock(&cursor->info, readSource, &cursor->source, block,
                cursor->storage, sizeof(cursor->storage), &cursor->block)) {
                cursor->failed = true;
                return done ? (int32_t)done : -1;
            }
            cursor->ready = true;
            cursor->index = block;
        }
        uint32_t at = cursor->position % CTR_ADPCM_BLOCK_FRAMES;
        uint32_t n = cursor->block.frames - at;
        if (n > capacity - done) n = capacity - done;
        if (!CtrAdpcm_decodeSpan(&cursor->info, &cursor->block, at, n,
            out + done * cursor->info.channels, n)) {
            cursor->failed = true;
            return done ? (int32_t)done : -1;
        }
        cursor->position += n;
        done += n;
    }
    return (int32_t)done;
}
