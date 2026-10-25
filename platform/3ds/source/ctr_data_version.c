#include "ctr_data_version.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct KnownFile {
    const char* game;
    const char* part;
    uint64_t size;
    const char* digest;
} KnownFile;
#define DATA_VERSIONS_BASE_ONLY // revision 1 rows here; later rows in the table at the end of the file
#define DATA_VERSION(game, part, size, digest) {game, part, size, digest},
static const KnownFile knownFiles[] = {
#include "../data_versions.inc"
};
#undef DATA_VERSION

typedef struct Sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} Sha256;

static uint32_t rotate(uint32_t x, unsigned bits) {
    return (x >> bits) | (x << (32 - bits));
}

static void shaBlock(Sha256* sha, const uint8_t* data) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        const uint8_t* p = data + i * 4;
        w[i] = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) + w[i - 7]
             + (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
    }
    uint32_t a = sha->h[0], b = sha->h[1], c = sha->h[2], d = sha->h[3];
    uint32_t e = sha->h[4], f = sha->h[5], g = sha->h[6], h = sha->h[7];
    for (size_t i = 0; i < 64; i++) {
        uint32_t t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    sha->h[0] += a; sha->h[1] += b; sha->h[2] += c; sha->h[3] += d;
    sha->h[4] += e; sha->h[5] += f; sha->h[6] += g; sha->h[7] += h;
}

static void shaUpdate(Sha256* sha, const uint8_t* data, size_t size) {
    sha->bytes += size;
    while (size > 0) {
        if (sha->used == 0 && size >= 64) {
            shaBlock(sha, data);
            data += 64;
            size -= 64;
        } else {
            size_t take = 64 - sha->used;
            if (take > size) take = size;
            memcpy(sha->block + sha->used, data, take);
            sha->used += take;
            data += take;
            size -= take;
            if (sha->used == 64) {
                shaBlock(sha, sha->block);
                sha->used = 0;
            }
        }
    }
}

static void shaFinish(Sha256* sha, char digest[65]) {
    uint64_t bits = sha->bytes * 8;
    uint8_t tail[128] = {0x80};
    size_t padding = sha->used < 56 ? 56 - sha->used : 120 - sha->used;
    for (size_t i = 0; i < 8; i++) tail[padding + i] = (uint8_t) (bits >> (56 - i * 8));
    shaUpdate(sha, tail, padding + 8);
    for (size_t i = 0; i < 8; i++) snprintf(digest + i * 8, 9, "%08lx", (unsigned long) sha->h[i]);
}

static bool readCache(const char* cachePath, const char* path, const struct stat* st, char digest[65]) {
    FILE* f = fopen(cachePath, "r");
    if (f == NULL) return false;
    unsigned revision = 0;
    unsigned long long size = 0;
    long long mtime = 0;
    char savedPath[768];
    bool ok = fscanf(f, "%u %llu %lld %64s\n", &revision, &size, &mtime, digest) == 4
           && fgets(savedPath, sizeof(savedPath), f) != NULL;
    fclose(f);
    if (!ok) return false;
    savedPath[strcspn(savedPath, "\r\n")] = '\0';
    return revision == DATA_VERSIONS_REVISION && size == (uint64_t) st->st_size && mtime == (long long) st->st_mtime
        && strcmp(savedPath, path) == 0 && strlen(digest) == 64 && strspn(digest, "0123456789abcdef") == 64;
}

static void writeCache(const char* cachePath, const char* path, const struct stat* st, const char* digest) {
    if (strpbrk(path, "\r\n") != NULL) return;
    FILE* f = fopen(cachePath, "w");
    if (f == NULL) return;
    fprintf(f, "%u %llu %lld %s\n%s\n", DATA_VERSIONS_REVISION, (unsigned long long) st->st_size,
            (long long) st->st_mtime, digest, path);
    fclose(f); // A partial/unwritable cache is ignored next boot.
}

CtrDataVersionResult CtrDataVersion_check(const char* path, const char* game, const char* part,
                                        const char* cachePath, void (*progress)(void), bool* cached, char digest[65]) {
    *cached = false;
    digest[0] = '\0';
    struct stat before, after;
    if (stat(path, &before) != 0 || before.st_size < 0) return CTR_DATA_UNREADABLE;
    *cached = readCache(cachePath, path, &before, digest);
    if (!*cached) {
        digest[0] = '\0';
        FILE* f = fopen(path, "rb");
        if (f == NULL) return CTR_DATA_UNREADABLE;
        uint8_t* buffer = malloc(65536);
        if (buffer == NULL) { fclose(f); return CTR_DATA_UNREADABLE; }
        Sha256 sha = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
        size_t size;
        while ((size = fread(buffer, 1, 65536, f)) != 0) {
            shaUpdate(&sha, buffer, size);
            if (progress != NULL) progress();
        }
        bool ok = !ferror(f) && sha.bytes == (uint64_t) before.st_size;
        if (fclose(f) != 0) ok = false;
        free(buffer);
        if (!ok || stat(path, &after) != 0 || before.st_size != after.st_size || before.st_mtime != after.st_mtime)
            return CTR_DATA_UNREADABLE;
        shaFinish(&sha, digest);
        writeCache(cachePath, path, &before, digest);
    }
    for (size_t i = 0; i < sizeof(knownFiles) / sizeof(knownFiles[0]); i++) {
        const KnownFile* known = &knownFiles[i];
        if (known->size == (uint64_t) before.st_size && strcmp(known->game, game) == 0
            && strcmp(known->part, part) == 0 && strcmp(known->digest, digest) == 0) return CTR_DATA_KNOWN;
    }
    return CTR_DATA_UNKNOWN;
}

// ===[ nativemac: rows added after revision 1 (late code and data; platform/3ds/late_reserve.ld) ]===
//
// knownFiles above keeps revision 1's rows, so its size and every constant after it stay where main has them. Rows
// added later (UNDERTALE's macOS file, revision 2) live here, in the late text block, with their strings in place, and
// a wrapper of CtrDataVersion_check (--wrap, CMakeLists.txt) looks them up when the first table doesn't know the file.

#undef DATA_VERSIONS_BASE_ONLY
#define DATA_VERSIONS_LATE_ONLY
typedef struct LateKnownFile {
    uint64_t size;
    char game[12];
    char part[20];
    char digest[65];
} LateKnownFile;

#define DATA_VERSION(game, part, size, digest) {size, game, part, digest},
static const LateKnownFile lateKnownFiles[] __attribute__((aligned(8), section(".rodata.nativemac_in_text"))) = {
#include "../data_versions.inc"
};
#undef DATA_VERSION

// A link of CtrDataVersion_check's wrap chain (its order: ctr_wrap_chain.h).
#include "ctr_wrap_chain.h"

__attribute__((noinline, section(".text.late.nativemac"))) CtrDataVersionResult CTR_DATAVERSION_NATIVEMAC(
    const char* path, const char* game, const char* part, const char* cachePath, void (*progress)(void), bool* cached,
    char digest[65]) {
    CtrDataVersionResult result = CTR_DATAVERSION_AFTER_NATIVEMAC(path, game, part, cachePath, progress, cached, digest);
    struct stat st;
    if (result != CTR_DATA_UNKNOWN || stat(path, &st) != 0) return result;
    for (size_t i = 0; i < sizeof(lateKnownFiles) / sizeof(lateKnownFiles[0]); i++) {
        const LateKnownFile* known = &lateKnownFiles[i];
        if (known->size == (uint64_t) st.st_size && strcmp(known->game, game) == 0 && strcmp(known->part, part) == 0
            && strcmp(known->digest, digest) == 0) return CTR_DATA_KNOWN;
    }
    return result;
}
