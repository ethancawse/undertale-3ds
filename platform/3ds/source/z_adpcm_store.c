#include "ctr_adpcm_store.h"
#include "sha1.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef __3DS__
#include <3ds/archive.h>
#pragma weak archive_getmtime
#endif
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME && defined(__3DS__)
#include <3ds/svc.h>
#include <3ds/result.h>
#endif

#ifdef CTR_ADPCM_HOST
#define AST_CODE(name)
#define AST_RO
#else
#define AST_CODE(name) __attribute__((noinline, noipa, optimize("Os"), section(".text.late.adpcm.store." #name)))
#define AST_RO __attribute__((section(".rodata.late.adpcm")))
#endif

static const char storeIndex[] AST_RO = "%s/audio-adpcm/index/%s.dtai";
static const char storeObject[] AST_RO = "%s/audio-adpcm/objects/%s.dtad";
static const char storeHex[] AST_RO = "0123456789abcdef";
static const char storeRead[] AST_RO = "rb";
static const char storeReceipt[] AST_RO = "%s/audio-adpcm/receipts/%s.dtar";
static const char storeSource[] AST_RO = "%s/%s";

static inline __attribute__((always_inline)) uint32_t receiptWord(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline __attribute__((always_inline)) uint64_t receiptStamp(const uint8_t* p) {
    return receiptWord(p) | (uint64_t)receiptWord(p + 4) << 32;
}

#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
static inline __attribute__((always_inline)) bool testEmulator(void) {
#ifdef CTR_ADPCM_HOST
    extern int testStore_emulator(void);
    return testStore_emulator() != 0;
#else
    int64_t detected = 0;
    return R_SUCCEEDED(svcGetSystemInfo(&detected, 0x20000, 0)) && detected != 0;
#endif
}

AST_CODE(expectedStamp) static uint64_t expectedStamp(const CtrAdpcmStore* s, const uint8_t* record) {
    return s->stampMode == 2 ? 946684800u : receiptStamp(record + s->stampMode * 8);
}
#endif

// libctru stat/fstat deliberately leave st_mtime zero. ControlArchive supplies
// the actual FAT calendar timestamp. A receipt-file probe selects the checked
// FAT-local or emulator-UTC representation; no guessed timezone adjustment.
AST_CODE(stamp) static bool fileStamp(const char* path, uint64_t* stamp) {
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
#ifdef CTR_ADPCM_HOST
    extern int testStore_timestamp(uint64_t*);
    if (!testStore_timestamp(stamp)) {
        struct stat st;
        if (stat(path, &st) || st.st_mtime <= 0) return false;
        *stamp = (uint64_t)st.st_mtime;
    }
#else
    if (!archive_getmtime || archive_getmtime(path, stamp) != 0) return false;
#endif
    // Keep libctru's epoch-adjusted value. Only receipt admission can enable
    // the emulator-only unknown-time mode, after explicit emulator detection.
    return true;
#else
#ifdef __3DS__
    return archive_getmtime && archive_getmtime(path, stamp) == 0;
#else
    struct stat st;
    if (stat(path, &st) || st.st_mtime <= 0) return false;
    *stamp = (uint64_t)st.st_mtime;
    return true;
#endif
#endif
}

AST_CODE(checksum) static bool checksum(const uint8_t* data, uint32_t bytes, const uint8_t* expected) {
    SHA1_CTX hash;
    uint8_t digest[20];
    SHA1Init(&hash); SHA1Update(&hash, data, bytes); SHA1Final(digest, &hash);
    return memcmp(digest, expected, sizeof(digest)) == 0;
}

AST_CODE(hex) static void toHex(char out[65], const uint8_t in[32]) {
    for (unsigned i = 0; i < 32; i++) {
        out[i * 2] = storeHex[in[i] >> 4];
        out[i * 2 + 1] = storeHex[in[i] & 15];
    }
    out[64] = 0;
}

AST_CODE(length) static long length(FILE* file) {
    if (fseek(file, 0, SEEK_END)) return -1;
    long bytes = ftell(file);
    if (bytes < 0 || fseek(file, 0, SEEK_SET)) return -1;
    return bytes;
}

// Only bounded metadata is read here, once at audio initialization. A missing
// receipt does not invalidate the catalog; each converted play logs fallback.
AST_CODE(receipt) static void loadReceipt(CtrAdpcmStore* s, const char* hex,
    uint32_t indexBytes, void* (*allocate)(size_t)) {
    s->receiptResult = CTR_ADPCM_STORE_RECEIPT;
    char path[CTR_ADPCM_PATH_BYTES + 96];
    int n = snprintf(path, sizeof(path), storeReceipt, s->root, hex);
    if (n < 0 || (unsigned)n >= sizeof(path)) return;
    FILE* file = fopen(path, storeRead);
    if (!file) return;
    uint32_t count = s->catalog.sounds + s->catalog.files;
    uint32_t bytes = 128u + count * 32u;
    bool ok = length(file) == (long)bytes;
    uint8_t* r = ok ? allocate(bytes) : NULL;
    if (ok && !r) s->receiptResult = CTR_ADPCM_STORE_HEAP;
    ok = r && fread(r, 1, bytes, file) == bytes;
    fclose(file);
    uint64_t stamp;
    if (ok) ok = receiptWord(r) == 0x52415444u && receiptWord(r + 4) == 1 &&
        receiptWord(r + 8) == 128 && receiptWord(r + 12) == 32 &&
        receiptWord(r + 16) == count && receiptWord(r + 20) == bytes &&
        receiptWord(r + 24) == 1 && receiptWord(r + 28) == 0 &&
        receiptStamp(r + 120) == 0 && memcmp(r + 88, s->index + 32, 32) == 0 &&
        checksum(s->index, indexBytes, r + 32) && checksum(r + 128, bytes - 128, r + 52) &&
        receiptStamp(r + 72) == 981173106u && fileStamp(path, &stamp);
    if (ok) {
        if (stamp == receiptStamp(r + 72)) s->stampMode = 0;
        else if (stamp == receiptStamp(r + 80)) s->stampMode = 1;
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
        else if (stamp == 946684800u && testEmulator()) s->stampMode = 2; // diagnostic only
#endif
        else ok = false;
    }
    if (!ok) { free(r); return; }
    s->receipt = r; s->receiptBytes = bytes;
    s->receiptResult = CTR_ADPCM_STORE_OK;
}

AST_CODE(close) void CtrAdpcmStore_close(CtrAdpcmStore* s) {
    if (!s) return;
    free(s->index);
    free(s->receipt);
    memset(s, 0, sizeof(*s));
}

// Verify the complete on-SD catalog and receipt first. Projected SFX keep
// their original SOND slots for direct admission; music-only catalogs retain
// just their file records in heap.
// Source/payload identities, timestamps, and the original slot pairing stay
// exact. No receipt is written and no borrowed asset exists during this move.
#ifdef CTR_ADPCM_STORE_TEST_RESIZE
extern void* testStore_resize(void*, size_t);
#define storeResize testStore_resize
#elif defined(__3DS__)
extern void* __real_realloc(void*, size_t);
#define storeResize __real_realloc
#else
#define storeResize realloc
#endif
AST_CODE(compact) static bool compact(CtrAdpcmStore* s) {
    uint32_t sounds = s->catalog.sounds, files = s->catalog.files;
    if (!sounds) return true;
    for (uint32_t i = 0; i < sounds; i++)
        if (receiptWord(s->catalog.records + i * CTR_ADPCM_INDEX_RECORD) &
            CTR_ADPCM_ASSET_PROJECTED_MONO) return true;
    if (s->receipt) {
        uint32_t bytes = 128u + files * 32u;
        memmove(s->receipt + 128, s->receipt + 128u + sounds * 32u, files * 32u);
        void* moved = storeResize(s->receipt, bytes);
        if (!moved) return false;
        s->receipt = moved;
        s->receiptBytes = bytes;
    }
    uint32_t records = files * CTR_ADPCM_INDEX_RECORD;
    uint32_t body = records + s->catalog.stringBytes;
    memmove(s->index + CTR_ADPCM_INDEX_HEADER,
        s->catalog.records + sounds * CTR_ADPCM_INDEX_RECORD, body);
    void* moved = storeResize(s->index, CTR_ADPCM_INDEX_HEADER + body);
    if (!moved) return false;
    s->index = moved;
    s->catalog.records = s->index + CTR_ADPCM_INDEX_HEADER;
    s->catalog.strings = (const char*)s->catalog.records + records;
    s->catalog.sounds = 0;
    return true;
}
#undef storeResize

AST_CODE(open) CtrAdpcmStoreResult CtrAdpcmStore_open(CtrAdpcmStore* s, const char* dataWin,
    const uint8_t sha[32], uint32_t sounds, void* (*allocate)(size_t)) {
    if (!s || s->index || !dataWin || !sha || !allocate || sounds > 65535 ||
        strlen(dataWin) >= sizeof(s->dataDir)) return CTR_ADPCM_STORE_ARGUMENT;
    CtrAdpcmStore candidate = {0};
    memcpy(candidate.dataDir, dataWin, strlen(dataWin) + 1);
    char* slash = strrchr(candidate.dataDir, '/');
    if (!slash || slash == candidate.dataDir) return CTR_ADPCM_STORE_ARGUMENT;
    *slash = 0;
    memcpy(candidate.root, candidate.dataDir, strlen(candidate.dataDir) + 1);
    char hex[65], path[CTR_ADPCM_PATH_BYTES + 96];
    toHex(hex, sha);
    FILE* file = NULL;
    for (unsigned depth = 0; depth < 2; depth++) {
        int n = snprintf(path, sizeof(path), storeIndex, candidate.root, hex);
        if (n < 0 || (unsigned)n >= sizeof(path)) return CTR_ADPCM_STORE_ARGUMENT;
        file = fopen(path, storeRead);
        if (file) break;
        slash = strrchr(candidate.root, '/');
        if (depth || !slash || slash == candidate.root) break;
        *slash = 0;
    }
    if (!file) return CTR_ADPCM_STORE_INDEX_MISSING;
    long bytes = length(file);
    CtrAdpcmStoreResult result = CTR_ADPCM_STORE_INDEX_INVALID;
    if (bytes >= (long)CTR_ADPCM_INDEX_HEADER && bytes <= (long)CTR_ADPCM_INDEX_LIMIT) {
        candidate.index = allocate((size_t)bytes);
        if (!candidate.index) result = CTR_ADPCM_STORE_HEAP;
        else if (fread(candidate.index, 1, (size_t)bytes, file) == (size_t)bytes &&
            CtrAdpcmCatalog_parse(candidate.index, (uint32_t)bytes, sha, sounds, &candidate.catalog))
            result = CTR_ADPCM_STORE_OK;
    }
    fclose(file);
    if (result != CTR_ADPCM_STORE_OK) CtrAdpcmStore_close(&candidate);
    else {
        loadReceipt(&candidate, hex, (uint32_t)bytes, allocate);
        if (!compact(&candidate)) {
            CtrAdpcmStore_close(&candidate);
            result = CTR_ADPCM_STORE_HEAP;
        } else *s = candidate;
    }
    return result;
}

static inline __attribute__((always_inline)) const char* relative(const char* path, const char* root) {
    size_t n = strlen(root);
    return strncmp(path, root, n) == 0 && path[n] == '/' ? path + n + 1 : NULL;
}

// Compare lexical paths, never open another file to canonicalize them. GM's
// chapter music paths contain ../mus; keep every component inside this root.
AST_CODE(normalize) static bool normalize(const char* path, char out[CTR_ADPCM_PATH_BYTES]) {
    size_t used = 0, begin = 0;
    for (;;) {
        unsigned char ch = (unsigned char)*path++;
        if (ch && ch != '/') {
            if (ch < 32 || ch == 127 || ch == ':' || ch == '\\') return false;
            if (used >= CTR_ADPCM_PATH_BYTES - 1) return false;
            out[used++] = (char)ch;
        } else {
            size_t size = used - begin;
            if (size == 1 && out[begin] == '.') used = begin;
            else if (size == 2 && out[begin] == '.' && out[begin + 1] == '.') {
                if (!begin) return false;
                used = begin - 1;
                while (used && out[used - 1] != '/') used--;
            } else if (size) out[used++] = '/';
            begin = used;
            if (!ch) break;
        }
    }
    if (used) used--; // Each retained component ends in '/', including the last.
    out[used] = 0;
    return used != 0;
}

AST_CODE(lookupFile) CtrAdpcmStoreResult CtrAdpcmStore_lookupFile(const CtrAdpcmStore* s,
    const char* path, CtrAdpcmAsset* asset) {
    if (!s || !s->index || !path || !asset) return CTR_ADPCM_STORE_ARGUMENT;
    CtrAdpcmAsset a;
    const char* name = relative(path, s->root);
    char canonical[CTR_ADPCM_PATH_BYTES];
    bool found = name && normalize(name, canonical) && CtrAdpcmCatalog_file(&s->catalog, canonical, &a);
    if (!found || (a.flags & CTR_ADPCM_ASSET_EMBEDDED)) return CTR_ADPCM_STORE_UNCONVERTED;
    *asset = a;
    return CTR_ADPCM_STORE_OK;
}

AST_CODE(openAsset) static CtrAdpcmStoreResult openAsset(const CtrAdpcmStore* s,
    const char* path, CtrAdpcmAsset a, CtrAdpcmFile* out, CtrAdpcmAsset* asset) {
    if (s->receiptResult) return s->receiptResult;
    if (!s->receipt) return CTR_ADPCM_STORE_RECEIPT;
    size_t slot = (size_t)(a.key - s->catalog.records - 40) / CTR_ADPCM_INDEX_RECORD;
    const uint8_t* record = s->receipt + 128 + slot * 32;
    // libctru's stdio seek takes a signed 32-bit long. Reject unsupported
    // sizes before a cast, including on a 64-bit host with wider long.
    uint64_t sourceBytes = a.flags & CTR_ADPCM_ASSET_PROJECTED_MONO ? a.sourceFileBytes : a.sourceBytes;
    if (a.bytes > INT32_MAX || sourceBytes > INT32_MAX) return CTR_ADPCM_STORE_SOURCE;
    struct stat st;
    uint64_t stamp;
    if (stat(path, &st) || !fileStamp(path, &stamp)) return CTR_ADPCM_STORE_SOURCE;
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
    if (st.st_size != (int64_t)sourceBytes || stamp != expectedStamp(s, record))
#else
    if (st.st_size != (int64_t)sourceBytes || stamp != receiptStamp(record + s->stampMode * 8))
#endif
        return CTR_ADPCM_STORE_STALE_SOURCE;
    char key[65], payload[CTR_ADPCM_PATH_BYTES + 96];
    toHex(key, a.key);
    int n = snprintf(payload, sizeof(payload), storeObject, s->root, key);
    if (n < 0 || (unsigned)n >= sizeof(payload)) return CTR_ADPCM_STORE_ARGUMENT;
    FILE* file = fopen(payload, storeRead);
    if (!file) return CTR_ADPCM_STORE_PAYLOAD;
    uint8_t header[CTR_ADPCM_HEADER_BYTES];
    CtrAdpcmInfo info;
#if CTRPORT_ADPCM_TEST_UNKNOWN_MTIME
    if (!fileStamp(payload, &stamp) || stamp != expectedStamp(s, record + 16)) {
#else
    if (!fileStamp(payload, &stamp) || stamp != receiptStamp(record + 16 + s->stampMode * 8)) {
#endif
        fclose(file); return CTR_ADPCM_STORE_STALE_PAYLOAD;
    }
    bool ok = length(file) == (long)a.bytes &&
        fread(header, 1, sizeof(header), file) == sizeof(header) &&
        CtrAdpcm_parse(header, sizeof(header), a.bytes, &info) &&
        info.frames == a.frames && info.rate == a.rate && info.channels == a.channels;
    if (!ok) { fclose(file); return CTR_ADPCM_STORE_PAYLOAD; }
    out->file = file;
    out->bytes = out->sourceBytes = a.bytes;
    out->info = info;
    out->mono = false;
    memcpy(out->header, header, sizeof(header));
    *asset = a;
    return CTR_ADPCM_STORE_OK;
}

AST_CODE(openFile) CtrAdpcmStoreResult CtrAdpcmStore_openFile(const CtrAdpcmStore* s,
    const char* path, CtrAdpcmFile* out, CtrAdpcmAsset* asset) {
    if (!out || out->file || !asset) return CTR_ADPCM_STORE_ARGUMENT;
    CtrAdpcmAsset a;
    CtrAdpcmStoreResult result = CtrAdpcmStore_lookupFile(s, path, &a);
    return result ? result : openAsset(s, path, a, out, asset);
}

AST_CODE(openSound) CtrAdpcmStoreResult CtrAdpcmStore_openSound(const CtrAdpcmStore* s,
    uint32_t sound, CtrAdpcmFile* out, CtrAdpcmAsset* asset) {
    if (!s || !s->index || !out || out->file || !asset) return CTR_ADPCM_STORE_ARGUMENT;
    CtrAdpcmAsset a;
    if (!CtrAdpcmCatalog_sound(&s->catalog, sound, &a) ||
        !(a.flags & CTR_ADPCM_ASSET_PROJECTED_MONO)) return CTR_ADPCM_STORE_UNCONVERTED;
    char path[CTR_ADPCM_PATH_BYTES + 96];
    int n = snprintf(path, sizeof(path), storeSource, s->root, a.path);
    if (n < 0 || (unsigned)n >= sizeof(path)) return CTR_ADPCM_STORE_ARGUMENT;
    return openAsset(s, path, a, out, asset);
}

AST_CODE(mono) bool CtrAdpcmStore_mono(CtrAdpcmFile* file, const CtrAdpcmAsset* a) {
    if (!file || !file->file || !a || file->mono || file->info.channels != 2 ||
        !(a->flags & CTR_ADPCM_ASSET_DUAL_MONO) || a->channels != 2 ||
        a->bytes != file->bytes || a->frames != file->info.frames || a->rate != file->info.rate) return false;
    file->sourceBytes = 128u + (file->bytes - 128u) / 2u;
    file->header[16] = 1;
    memset(file->header + 80, 0, 48);
    file->info.channels = 1;
    file->info.fileBytes = file->sourceBytes;
    memset(file->info.coefficients[1], 0, sizeof(file->info.coefficients[1]));
    file->mono = true;
    return true;
}

AST_CODE(readAt) bool CtrAdpcmStore_readAt(void* owner, uint64_t offset, void* out, uint32_t bytes) {
    CtrAdpcmFile* file = owner;
    if (!file || !file->file || (!out && bytes)) return false;
    uint32_t size = file->mono ? file->sourceBytes : file->bytes;
    if (offset > size || bytes > size - offset || offset > INT32_MAX) return false;
    if (!bytes) return true;
    if (file->mono) {
        uint8_t* dst = out;
        if (offset < 128) {
            uint32_t n = 128u - (uint32_t)offset;
            if (n > bytes) n = bytes;
            memcpy(dst, file->header + (size_t)offset, n);
            offset += n; dst += n; bytes -= n;
        }
        while (bytes) {
            uint32_t relative = (uint32_t)offset - 128u;
            uint32_t block = relative / CTR_ADPCM_BLOCK_BYTES, within = relative % CTR_ADPCM_BLOCK_BYTES;
            uint32_t n = CTR_ADPCM_BLOCK_BYTES - within;
            if (n > bytes) n = bytes;
            uint64_t physical = 128u + (uint64_t)block * CTR_ADPCM_STEREO_BLOCK_BYTES + within;
            if (physical > INT32_MAX || fseek(file->file, (long)physical, SEEK_SET) ||
                fread(dst, 1, n, file->file) != n) return false;
            dst += n; offset += n; bytes -= n;
        }
        return true;
    }
    return fseek(file->file, (long)offset, SEEK_SET) == 0 && fread(out, 1, bytes, file->file) == bytes;
}

AST_CODE(closeFile) void CtrAdpcmStore_closeFile(CtrAdpcmFile* file) {
    if (!file) return;
    if (file->file) fclose(file->file);
    memset(file, 0, sizeof(*file));
}
