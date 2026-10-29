#include "ctr_adpcm_catalog.h"
#include "sha1.h"
#include <stddef.h>
#include <string.h>

#if defined(CTR_ADPCM_HOST) && defined(_WIN32)
#define AC_CODE(name)
#define AC_EXPORT(name) __declspec(dllexport)
#elif defined(CTR_ADPCM_HOST)
#define AC_CODE(name)
#define AC_EXPORT(name)
#else
#define AC_CODE(name) __attribute__((noinline, noipa, section(".text.late.adpcm." #name)))
#define AC_EXPORT(name) AC_CODE(name)
#endif

static inline __attribute__((always_inline)) uint32_t word(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

AC_CODE(path) static bool pathValid(const char* p, uint32_t available) {
    uint32_t component = 0;
    for (uint32_t i = 0; i < available; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c == 0 || c == '/') {
            if (component == 0 || (component == 1 && p[i - 1] == '.') ||
                (component == 2 && p[i - 1] == '.' && p[i - 2] == '.')) return false;
            if (c == 0) return true;
            component = 0;
        } else {
            if (c < 32 || c == 127 || c == ':' || c == '\\') return false;
            component++;
        }
    }
    return false;
}

AC_CODE(asset) static bool assetAt(const CtrAdpcmCatalog* c, uint32_t index, CtrAdpcmAsset* out) {
    if (c == NULL || out == NULL || index >= c->sounds + c->files) return false;
    const uint8_t* r = c->records + (size_t)index * CTR_ADPCM_INDEX_RECORD;
    if (!(word(r) & CTR_ADPCM_ASSET_VALID)) return false;
    CtrAdpcmAsset asset = {0};
    asset.flags = word(r);
    asset.frames = word(r + 4);
    asset.rate = word(r + 8);
    asset.channels = word(r + 12);
    asset.bytes = word(r + 16);
    asset.path = c->strings + word(r + 20);
    asset.sourceOffset = word(r + 24) | (uint64_t)word(r + 28) << 32;
    asset.sourceBytes = word(r + 32);
    asset.sourceChannels = word(r + 36);
    asset.key = r + 40;
    asset.payloadSha1 = r + 72;
    asset.sourceSha1 = r + 92;
    asset.sourceFileBytes = word(r + 112) | (uint64_t)word(r + 116) << 32;
    asset.sourceBits = word(r + 120);
    asset.seamMaxStep = asset.flags & CTR_ADPCM_ASSET_PROJECTED_MONO ? word(r + 124) - 1u : 0;
    *out = asset;
    return true;
}

AC_EXPORT(catalog) bool CtrAdpcmCatalog_parse(const void* data, uint32_t bytes, const uint8_t sha[32],
                                  uint32_t soundCount, CtrAdpcmCatalog* out) {
    if (data == NULL || sha == NULL || out == NULL || bytes < CTR_ADPCM_INDEX_HEADER ||
        bytes > CTR_ADPCM_INDEX_LIMIT) return false;
    const uint8_t* h = data;
    if (word(h) != 0x49415444u || word(h + 4) != 1 || word(h + 8) != CTR_ADPCM_INDEX_HEADER ||
        word(h + 12) != CTR_ADPCM_INDEX_RECORD || word(h + 16) != soundCount ||
        soundCount > 65535 || word(h + 20) > 65535 || word(h + 24) != bytes - CTR_ADPCM_INDEX_HEADER ||
        memcmp(h + 32, sha, 32) != 0) return false;
    for (unsigned i = 84; i < CTR_ADPCM_INDEX_HEADER; i++) if (h[i] != 0) return false;
    uint32_t records = (soundCount + word(h + 20)) * CTR_ADPCM_INDEX_RECORD;
    if (records > word(h + 24) || word(h + 28) != word(h + 24) - records) return false;
    SHA1_CTX hash;
    unsigned char actual[20];
    SHA1Init(&hash);
    SHA1Update(&hash, h + CTR_ADPCM_INDEX_HEADER, bytes - CTR_ADPCM_INDEX_HEADER);
    SHA1Final(actual, &hash);
    if (memcmp(actual, h + 64, sizeof(actual)) != 0) return false;
    CtrAdpcmCatalog catalog = {h + CTR_ADPCM_INDEX_HEADER,
        (const char*)h + CTR_ADPCM_INDEX_HEADER + records, soundCount, word(h + 20), word(h + 28)};
    for (uint32_t i = 0; i < catalog.sounds + catalog.files; i++) {
        const uint8_t* r = catalog.records + (size_t)i * CTR_ADPCM_INDEX_RECORD;
        uint32_t flags = word(r);
        if (flags == 0) {
            for (unsigned j = 0; j < CTR_ADPCM_INDEX_RECORD; j++) if (r[j] != 0) return false;
            continue;
        }
        if (!(flags & CTR_ADPCM_ASSET_VALID) || (flags & ~15u) ||
            ((flags & CTR_ADPCM_ASSET_EMBEDDED) && i >= catalog.sounds)) return false;
        bool projected = (flags & CTR_ADPCM_ASSET_PROJECTED_MONO) != 0;
        if (!projected) for (unsigned j = 112; j < CTR_ADPCM_INDEX_RECORD; j++) if (r[j] != 0) return false;
        uint32_t offset = word(r + 20);
        if (offset >= catalog.stringBytes || !pathValid(catalog.strings + offset, catalog.stringBytes - offset)) return false;
        CtrAdpcmAsset a;
        if (!assetAt(&catalog, i, &a) || a.frames == 0 || a.rate == 0 || a.rate > 192000 ||
            a.channels < 1 || a.channels > 2 || a.sourceBytes == 0 ||
            a.sourceOffset > UINT64_MAX - a.sourceBytes ||
            (!(flags & CTR_ADPCM_ASSET_EMBEDDED) && a.sourceOffset != 0) ||
            ((flags & CTR_ADPCM_ASSET_DUAL_MONO) && a.channels != 2)) return false;
        if (projected) {
            if (!(flags & CTR_ADPCM_ASSET_EMBEDDED) || a.channels != 1 || a.sourceChannels != 2 ||
                (a.sourceBits != 8 && a.sourceBits != 16) || a.seamMaxStep > 65535u ||
                a.sourceFileBytes < a.sourceOffset + a.sourceBytes) return false;
        } else if (a.sourceChannels != a.channels) return false;
        uint32_t blocks = 1u + (a.frames - 1u) / 2044u;
        uint32_t last = a.frames - (blocks - 1u) * 2044u;
        uint64_t expected = 128u + (uint64_t)(blocks - 1u) * a.channels * 1176u +
            (uint64_t)a.channels * (8u + ((last + 13u) / 14u) * 8u);
        if (expected != a.bytes) return false;
    }
    *out = catalog;
    return true;
}

AC_EXPORT(sound) bool CtrAdpcmCatalog_sound(const CtrAdpcmCatalog* c, uint32_t sound, CtrAdpcmAsset* out) {
    return c != NULL && sound < c->sounds && assetAt(c, sound, out);
}

AC_EXPORT(file) bool CtrAdpcmCatalog_file(const CtrAdpcmCatalog* c, const char* path, CtrAdpcmAsset* out) {
    if (c == NULL || path == NULL || out == NULL) return false;
    for (uint32_t i = c->sounds; i < c->sounds + c->files; i++) {
        CtrAdpcmAsset asset;
        if (assetAt(c, i, &asset) && strcmp(path, asset.path) == 0) {
            *out = asset;
            return true;
        }
    }
    return false;
}
