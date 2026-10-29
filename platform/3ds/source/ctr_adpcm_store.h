// Receipt-verified, bounded SD sources for music and projected mono SFX. No DSP calls.
#ifndef CTR_ADPCM_STORE_H
#define CTR_ADPCM_STORE_H

#include "ctr_adpcm_catalog.h"
#include "ctr_adpcm_format.h"
#include <stddef.h>
#include <stdio.h>

#define CTR_ADPCM_PATH_BYTES 512u

typedef enum {
    CTR_ADPCM_STORE_OK,
    CTR_ADPCM_STORE_ARGUMENT,
    CTR_ADPCM_STORE_INDEX_MISSING,
    CTR_ADPCM_STORE_INDEX_INVALID,
    CTR_ADPCM_STORE_HEAP,
    CTR_ADPCM_STORE_UNCONVERTED,
    CTR_ADPCM_STORE_SOURCE,
    CTR_ADPCM_STORE_PAYLOAD,
    CTR_ADPCM_STORE_RECEIPT = 12,
    CTR_ADPCM_STORE_STALE_SOURCE,
    CTR_ADPCM_STORE_STALE_PAYLOAD,
} CtrAdpcmStoreResult;

typedef struct {
    uint8_t* index;
    CtrAdpcmCatalog catalog;
    char root[CTR_ADPCM_PATH_BYTES], dataDir[CTR_ADPCM_PATH_BYTES];
    uint8_t* receipt;
    uint32_t receiptBytes, stampMode;
    CtrAdpcmStoreResult receiptResult;
} CtrAdpcmStore;

typedef struct {
    FILE* file;
    uint32_t bytes, sourceBytes; // physical file and exposed source lengths
    CtrAdpcmInfo info;
    bool mono;
    uint8_t header[CTR_ADPCM_HEADER_BYTES];
} CtrAdpcmFile;

// Zero-initialize store/file. One owner serializes all calls. The allocator
// must return free()/realloc()-compatible ordinary heap, declining rather than spilling
// into linear memory. No stream/payload-sized allocation is made by openFile.
// open() tries the data.win directory and its parent for the index, matching
// flat UT and chapter-based DR layouts. After full validation, music-only
// catalogs drop embedded records. A projected-mono catalog retains SOND slots
// for direct sound-ID lookups. The on-SD files stay unchanged.
// A shrink failure closes the store and falls back. Retained
// records and strings stay pinned until close(). No asset may precede open().
CtrAdpcmStoreResult CtrAdpcmStore_open(CtrAdpcmStore* store, const char* dataWin,
    const uint8_t dataSha256[32], uint32_t sounds, void* (*allocate)(size_t));
void CtrAdpcmStore_close(CtrAdpcmStore* store);

// Metadata preflight only: no source or payload I/O. This is not verification
// and cannot authorize native playback; openFile must still succeed.
// Resolve lexical . and .. components relative to the installation root;
// reject paths that leave it, even if a later component would re-enter it.
CtrAdpcmStoreResult CtrAdpcmStore_lookupFile(const CtrAdpcmStore* store,
    const char* originalPath, CtrAdpcmAsset* asset);

// External complete files; embedded admission uses openSound below.
// Preparation hashes source/payload once, after copying, and publishes a DTAR
// receipt. open() pins and checks it once. openFile checks current sizes/real
// SD timestamps and the bounded header; no whole-track hash in playback.
// Metadata-preserving edits require re-preparation (FAT has 2 s precision).
// Header rate/channels/frames must equal the catalog. The
// returned asset borrows the store, whereas the file can outlive that store.
CtrAdpcmStoreResult CtrAdpcmStore_openFile(const CtrAdpcmStore* store,
    const char* originalPath, CtrAdpcmFile* file, CtrAdpcmAsset* asset);
// Only the explicit build-time exact-dual-mono projection is eligible.
// Check containing source size/timestamp plus payload metadata/header, without
// opening or scanning the source WAV. Original sourceChannels preserves pan.
// Missing/stale receipts return the same fallback statuses as openFile.
CtrAdpcmStoreResult CtrAdpcmStore_openSound(const CtrAdpcmStore* store,
    uint32_t sound, CtrAdpcmFile* file, CtrAdpcmAsset* asset);
// A verified identical-channel payload may expose only its left channel.
// This changes no encoded bytes or source-frame count. The game retains the
// asset's original channel count for its existing spatial/panning policy.
bool CtrAdpcmStore_mono(CtrAdpcmFile* file, const CtrAdpcmAsset* asset);

// CtrAdpcmSource.readAt-compatible. Worker only, outside the player lock and
// DSP callback. Owner/file remains pinned through every outstanding read,
// including cancelled requests. closeFile() is legal only after they return.
bool CtrAdpcmStore_readAt(void* owner, uint64_t offset, void* out, uint32_t bytes);
void CtrAdpcmStore_closeFile(CtrAdpcmFile* file);

#endif
