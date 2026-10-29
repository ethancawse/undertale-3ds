#include "ctr_adpcm_pcm.h"
#include <stddef.h>

#if defined(CTR_ADPCM_HOST) && defined(_WIN32)
#define PC_CODE
#define PC_EXPORT __declspec(dllexport)
#elif defined(CTR_ADPCM_HOST)
#define PC_CODE
#define PC_EXPORT
#else
#define PC_CODE __attribute__((noinline, noipa, section(".text.late.adpcm")))
#define PC_EXPORT PC_CODE
#endif

static inline __attribute__((always_inline)) uint16_t pc16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

// Direct immutable views avoid a block-sized copy on the DSP callback.
PC_CODE static bool pcBlock(CtrAdpcmPcm* c, uint32_t index, bool validate) {
    uint64_t offset;
    uint32_t bytes, frames;
    if (!CtrAdpcm_blockRange(&c->info, index, &offset, &bytes, &frames) ||
        offset > c->bytes || bytes > c->bytes - offset) return false;
    uint32_t payload = ((frames + 13u) / 14u) * 8u;
    const uint8_t* p = c->data + (size_t)offset;
    for (uint32_t ch = 0; ch < c->info.channels; ch++, p += payload + 8u) {
        if (validate) {
            if (pc16(p + 6) || pc16(p) != p[8] ||
                (index == 0 && (pc16(p + 2) || pc16(p + 4)))) return false;
            for (uint32_t i = 0; i < payload; i += 8u) if (p[8u + i] >= 0x80u) return false;
        }
        c->channel[ch] = p + 8;
        c->history[ch][0] = (int16_t)pc16(p + 2);
        c->history[ch][1] = (int16_t)pc16(p + 4);
    }
    c->blockFirst = index * CTR_ADPCM_BLOCK_FRAMES;
    c->blockFrames = frames;
    c->position = c->blockFirst;
    return true;
}

PC_EXPORT bool CtrAdpcmPcm_open(CtrAdpcmPcm* out, const void* data, uint32_t bytes) {
    if (out == NULL || data == NULL) return false;
    CtrAdpcmPcm c = {0};
    if (!CtrAdpcm_parse(data, bytes, bytes, &c.info)) return false;
    c.data = data;
    c.bytes = bytes;
    for (uint32_t index = 0; index < c.info.blocks; index++)
        if (!pcBlock(&c, index, true)) return false;
    if (!pcBlock(&c, 0, false)) return false;
    c.ready = true;
    *out = c;
    return true;
}

PC_CODE static void pcFrames(CtrAdpcmPcm* c, uint32_t count, int16_t* out) {
    uint32_t first = c->position - c->blockFirst;
    for (uint32_t ch = 0; ch < c->info.channels; ch++) {
        const uint8_t* p = c->channel[ch];
        int32_t h0 = c->history[ch][0], h1 = c->history[ch][1];
        for (uint32_t at = first; at < first + count; at++) {
            uint32_t frame = at / 14u, nibble = at % 14u;
            uint8_t header = p[frame * 8u];
            uint32_t predictor = header >> 4, scale = header & 15u;
            uint8_t packed = p[frame * 8u + 1u + nibble / 2u];
            int32_t residual = (packed >> ((nibble & 1u) ? 0 : 4)) & 15;
            if (residual >= 8) residual -= 16;
            int64_t sum = (int64_t)residual * ((uint32_t)1 << scale) * 2048 +
                (int64_t)c->info.coefficients[ch][predictor * 2u] * h0 +
                (int64_t)c->info.coefficients[ch][predictor * 2u + 1u] * h1 + 1024;
            int64_t value = sum >= 0 ? sum / 2048 : -((-sum + 2047) / 2048);
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            h1 = h0;
            h0 = (int32_t)value;
            if (out != NULL) out[(at - first) * c->info.channels + ch] = (int16_t)h0;
        }
        c->history[ch][0] = h0;
        c->history[ch][1] = h1;
    }
    c->position += count;
}

PC_EXPORT bool CtrAdpcmPcm_read(CtrAdpcmPcm* c, uint32_t first, uint32_t count,
                               int16_t* out, uint32_t capacity, uint32_t* decoded) {
    if (decoded != NULL) *decoded = 0;
    if (c == NULL || !c->ready || first > c->info.frames || count > c->info.frames - first ||
        count > capacity || (count && out == NULL)) return false;
    if (count == 0) return true;
    uint32_t index = first / CTR_ADPCM_BLOCK_FRAMES;
    if (c->position > first || c->blockFirst / CTR_ADPCM_BLOCK_FRAMES != index)
        if (!pcBlock(c, index, false)) return false;
    uint32_t work = first - c->position;
    if (work) pcFrames(c, work, NULL);
    while (count) {
        if (c->position == c->blockFirst + c->blockFrames)
            if (!pcBlock(c, c->position / CTR_ADPCM_BLOCK_FRAMES, false)) return false;
        uint32_t n = c->blockFirst + c->blockFrames - c->position;
        if (n > count) n = count;
        pcFrames(c, n, out);
        out += (size_t)n * c->info.channels;
        count -= n;
        work += n;
    }
    if (decoded != NULL) *decoded = work;
    return true;
}
