#ifndef CTR_ADPCM_CATALOG_H
#define CTR_ADPCM_CATALOG_H

#include <stdbool.h>
#include <stdint.h>

#define CTR_ADPCM_INDEX_HEADER 128u
#define CTR_ADPCM_INDEX_RECORD 128u
#define CTR_ADPCM_INDEX_LIMIT (4u * 1024u * 1024u)
enum { CTR_ADPCM_ASSET_VALID = 1, CTR_ADPCM_ASSET_EMBEDDED = 2, CTR_ADPCM_ASSET_DUAL_MONO = 4,
       CTR_ADPCM_ASSET_PROJECTED_MONO = 8 };

typedef struct {
    uint32_t flags, frames, rate, channels, bytes, sourceBytes, sourceChannels;
    uint64_t sourceOffset;
    const char* path; // original source/container path, relative to the game root
    const uint8_t* key; // 32 B source/policy identity; objects/<hex key>.dtad
    const uint8_t* payloadSha1;
    const uint8_t* sourceSha1; // exact original bytes, at sourceOffset for sourceBytes
    uint64_t sourceFileBytes; // projected embedded source's complete container size
    uint32_t sourceBits; // projected source PCM width, 8 or 16; zero for legacy records
    uint32_t seamMaxStep; // projected mono render's largest adjacent PCM16 step, prepared on PC
} CtrAdpcmAsset;

typedef struct {
    const uint8_t* records;
    const char* strings;
    uint32_t sounds, files, stringBytes;
} CtrAdpcmCatalog;

// Borrowed immutable memory, pinned for the catalog/asset lifetime. All records
// and paths are validated before exposing any lookup. The SHA-256 is the
// existing boot check's data.win identity; the body SHA-1 detects index damage.
// It does not replace source/payload validation by the playback adapter.
bool CtrAdpcmCatalog_parse(const void* data, uint32_t bytes, const uint8_t dataSha256[32],
                           uint32_t soundCount, CtrAdpcmCatalog* out);
bool CtrAdpcmCatalog_sound(const CtrAdpcmCatalog* catalog, uint32_t sound, CtrAdpcmAsset* out);
bool CtrAdpcmCatalog_file(const CtrAdpcmCatalog* catalog, const char* relativePath, CtrAdpcmAsset* out);

#endif
