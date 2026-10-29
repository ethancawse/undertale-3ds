// Public-format DSP-ADPCM; no platform decoder or SDK implementation is used.
#include "ctr_adpcm_format.h"
#include <stddef.h>

#if defined(CTR_ADPCM_HOST) && defined(_WIN32)
#define AD_CODE(name) __declspec(dllexport)
#elif defined(CTR_ADPCM_HOST)
#define AD_CODE(name)
#else
#define AD_CODE(name) __attribute__((noinline, noipa, section(".text.late.adpcm." #name)))
#endif

static inline __attribute__((always_inline)) uint16_t le16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline __attribute__((always_inline)) uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

AD_CODE(parse) bool CtrAdpcm_parse(const uint8_t* h, uint32_t have, uint64_t bytes, CtrAdpcmInfo* out) {
    if (h == NULL || out == NULL || have < CTR_ADPCM_HEADER_BYTES || bytes < CTR_ADPCM_HEADER_BYTES) return false;
    if (le32(h) != 0x44415444u || le32(h + 4) != 1 || le32(h + 8) != CTR_ADPCM_HEADER_BYTES ||
        le32(h + 24) != 0 || le32(h + 28) != CTR_ADPCM_BLOCK_FRAMES || le32(h + 36) != 0) return false;
    CtrAdpcmInfo info = {0};
    info.rate = le32(h + 12);
    info.channels = le32(h + 16);
    info.frames = le32(h + 20);
    info.blocks = le32(h + 32);
    info.fileBytes = bytes;
    if (info.rate == 0 || info.rate > 192000u || info.channels == 0 || info.channels > 2 || info.frames == 0 ||
        info.blocks != 1u + (info.frames - 1u) / CTR_ADPCM_BLOCK_FRAMES) return false;
    uint32_t last = info.frames - (info.blocks - 1u) * CTR_ADPCM_BLOCK_FRAMES;
    uint64_t expected = CTR_ADPCM_HEADER_BYTES + (uint64_t)(info.blocks - 1u) * info.channels * CTR_ADPCM_BLOCK_BYTES +
        (uint64_t)info.channels * (8u + ((last + 13u) / 14u) * 8u);
    if (expected != bytes) return false;
    for (uint32_t ch = 0; ch < info.channels; ch++) {
        for (uint32_t i = 0; i < 16; i++) info.coefficients[ch][i] = (int16_t)le16(h + 40u + ch * 40u + i * 2u);
        for (uint32_t i = 72u + ch * 40u; i < 80u + ch * 40u; i++) if (h[i] != 0) return false;
    }
    // An unused channel descriptor and the header's final bytes are reserved.
    for (uint32_t i = 40u + info.channels * 40u; i < CTR_ADPCM_HEADER_BYTES; i++) if (h[i] != 0) return false;
    *out = info;
    return true;
}

AD_CODE(range) bool CtrAdpcm_blockRange(const CtrAdpcmInfo* info, uint32_t block,
                                uint64_t* offset, uint32_t* bytes, uint32_t* frames) {
    if (info == NULL || block >= info->blocks || offset == NULL || bytes == NULL || frames == NULL) return false;
    uint32_t left = info->frames - block * CTR_ADPCM_BLOCK_FRAMES;
    *frames = left < CTR_ADPCM_BLOCK_FRAMES ? left : CTR_ADPCM_BLOCK_FRAMES;
    *bytes = info->channels * (8u + ((*frames + 13u) / 14u) * 8u);
    *offset = CTR_ADPCM_HEADER_BYTES + (uint64_t)block * info->channels * CTR_ADPCM_BLOCK_BYTES;
    return *offset <= info->fileBytes && *bytes <= info->fileBytes - *offset;
}

AD_CODE(block) bool CtrAdpcm_readBlock(const CtrAdpcmInfo* info, CtrAdpcmReadAt readAt, void* owner,
                               uint32_t index, void* storage, uint32_t capacity, CtrAdpcmBlock* out) {
    uint64_t offset;
    uint32_t bytes, frames;
    if (readAt == NULL || storage == NULL || out == NULL ||
        !CtrAdpcm_blockRange(info, index, &offset, &bytes, &frames) || bytes > capacity ||
        !readAt(owner, offset, storage, bytes)) return false;
    CtrAdpcmBlock block = {0};
    block.frames = frames;
    block.payloadBytes = ((frames + 13u) / 14u) * 8u;
    const uint8_t* p = (const uint8_t*)storage;
    for (uint32_t ch = 0; ch < info->channels; ch++, p += block.payloadBytes + 8u) {
        if (le16(p + 6) != 0 || le16(p) != p[8]) return false;
        if (index == 0 && (le16(p + 2) != 0 || le16(p + 4) != 0)) return false;
        for (uint32_t i = 0; i < block.payloadBytes; i += 8u) if (p[8u + i] >= 0x80u) return false;
        block.channel[ch] = p + 8;
        block.history[ch].index = le16(p);
        block.history[ch].history0 = (int16_t)le16(p + 2);
        block.history[ch].history1 = (int16_t)le16(p + 4);
    }
    *out = block;
    return true;
}

AD_CODE(span) bool CtrAdpcm_decodeSpan(const CtrAdpcmInfo* info, const CtrAdpcmBlock* block,
                                uint32_t first, uint32_t count, int16_t* out, uint32_t capacity) {
    if (info == NULL || block == NULL || info->channels == 0 || info->channels > 2 ||
        block->frames == 0 || block->frames > CTR_ADPCM_BLOCK_FRAMES || first > block->frames ||
        count > block->frames - first || count > capacity || (count != 0 && out == NULL)) return false;
    for (uint32_t ch = 0; ch < info->channels; ch++) {
        const uint8_t* p = block->channel[ch];
        if (p == NULL || block->payloadBytes != ((block->frames + 13u) / 14u) * 8u) return false;
        int32_t h0 = block->history[ch].history0, h1 = block->history[ch].history1;
        for (uint32_t at = 0; at < first + count; at++) {
            uint32_t frame = at / 14u, nibble = at % 14u;
            uint32_t predictor = p[frame * 8u] >> 4, scale = p[frame * 8u] & 15u;
            if (predictor >= 8) return false;
            uint8_t packed = p[frame * 8u + 1u + nibble / 2u];
            int32_t residual = (packed >> ((nibble & 1u) ? 0 : 4)) & 15;
            if (residual >= 8) residual -= 16;
            int64_t sum = (int64_t)residual * ((uint32_t)1 << scale) * 2048 +
                (int64_t)info->coefficients[ch][predictor * 2u] * h0 +
                (int64_t)info->coefficients[ch][predictor * 2u + 1u] * h1 + 1024;
            // Explicit floor division matches an arithmetic right shift for negative samples.
            int64_t value = sum >= 0 ? sum / 2048 : -((-sum + 2047) / 2048);
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            h1 = h0;
            h0 = (int32_t)value;
            if (at >= first) out[(at - first) * info->channels + ch] = (int16_t)h0;
        }
    }
    return true;
}
