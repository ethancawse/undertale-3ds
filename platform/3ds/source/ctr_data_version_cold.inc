// Exact DR cold checker. Included after the original checker so its object layout remains frozen.
// Shares the original known-file table; the wrapper chain still visits every digest consumer.
// Receipt trust remains builder-verified and timestamp-bound, not independent per-boot proof.
// The cold path still hashes every byte and checks read/close errors, size and pre/post stat time.
#define BX_COLD_TEXT __attribute__((noinline, section(".text.late.builderxcold")))
static const char bxColdHexFormat[] __attribute__((section(".rodata.builderxcold_in_text"))) = "%08lx";
static const char bxColdReadMode[] __attribute__((section(".rodata.builderxcold_in_text"))) = "r";
static const char bxColdBinaryMode[] __attribute__((section(".rodata.builderxcold_in_text"))) = "rb";
static const char bxColdWriteMode[] __attribute__((section(".rodata.builderxcold_in_text"))) = "w";
static const char bxColdScanFormat[] __attribute__((section(".rodata.builderxcold_in_text"))) = "%u %llu %lld %64s\n";
static const char bxColdNewlines[] __attribute__((section(".rodata.builderxcold_in_text"))) = "\r\n";
static const char bxColdDigits[] __attribute__((section(".rodata.builderxcold_in_text"))) = "0123456789abcdef";
static const char bxColdWriteFormat[] __attribute__((section(".rodata.builderxcold_in_text"))) = "%u %llu %lld %s\n%s\n";
static const uint32_t bxColdK[64] __attribute__((section(".rodata.builderxcold_in_text"))) = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
    };

static inline uint32_t bxColdRotate(uint32_t x, unsigned bits) {
    return (x >> bits) | (x << (32 - bits));
}

static BX_COLD_TEXT void bxColdShaBlock(Sha256* sha, const uint8_t* data) {

    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        const uint8_t* p = data + i * 4;
        w[i] = ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t x = w[i - 15], y = w[i - 2];
        w[i] = w[i - 16] + (bxColdRotate(x, 7) ^ bxColdRotate(x, 18) ^ (x >> 3)) + w[i - 7]
             + (bxColdRotate(y, 17) ^ bxColdRotate(y, 19) ^ (y >> 10));
    }
    uint32_t a = sha->h[0], b = sha->h[1], c = sha->h[2], d = sha->h[3];
    uint32_t e = sha->h[4], f = sha->h[5], g = sha->h[6], h = sha->h[7];
// Exact SHA-256 rounds, grouped in eights to rotate variable roles without
// copying all eight working words after each round. The caller supplies the
// unchanged 64-word message schedule and constants from ctr_data_version.c.
#define BX_SHA_ROUND(a, b, c, d, e, f, g, h, i) do { \
    uint32_t t1 = (h) + (bxColdRotate((e), 6) ^ bxColdRotate((e), 11) ^ bxColdRotate((e), 25)) \
        + (((e) & (f)) ^ (~(e) & (g))) + bxColdK[(i)] + w[(i)]; \
    uint32_t t2 = (bxColdRotate((a), 2) ^ bxColdRotate((a), 13) ^ bxColdRotate((a), 22)) \
        + (((a) & (b)) ^ ((a) & (c)) ^ ((b) & (c))); \
    (d) += t1; (h) = t1 + t2; \
} while (0)
    for (size_t i = 0; i < 64; i += 8) {
        BX_SHA_ROUND(a, b, c, d, e, f, g, h, i);
        BX_SHA_ROUND(h, a, b, c, d, e, f, g, i + 1);
        BX_SHA_ROUND(g, h, a, b, c, d, e, f, i + 2);
        BX_SHA_ROUND(f, g, h, a, b, c, d, e, i + 3);
        BX_SHA_ROUND(e, f, g, h, a, b, c, d, i + 4);
        BX_SHA_ROUND(d, e, f, g, h, a, b, c, i + 5);
        BX_SHA_ROUND(c, d, e, f, g, h, a, b, i + 6);
        BX_SHA_ROUND(b, c, d, e, f, g, h, a, i + 7);
    }
#undef BX_SHA_ROUND

    sha->h[0] += a; sha->h[1] += b; sha->h[2] += c; sha->h[3] += d;
    sha->h[4] += e; sha->h[5] += f; sha->h[6] += g; sha->h[7] += h;
}

static BX_COLD_TEXT void bxColdShaUpdate(Sha256* sha, const uint8_t* data, size_t size) {
    sha->bytes += size;
    while (size > 0) {
        if (sha->used == 0 && size >= 64) {
            bxColdShaBlock(sha, data);
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
                bxColdShaBlock(sha, sha->block);
                sha->used = 0;
            }
        }
    }
}

static BX_COLD_TEXT void bxColdShaFinish(Sha256* sha, char digest[65]) {
    uint64_t bits = sha->bytes * 8;
    uint8_t tail[128] = {0x80};
    size_t padding = sha->used < 56 ? 56 - sha->used : 120 - sha->used;
    for (size_t i = 0; i < 8; i++) tail[padding + i] = (uint8_t) (bits >> (56 - i * 8));
    bxColdShaUpdate(sha, tail, padding + 8);
    for (size_t i = 0; i < 8; i++) snprintf(digest + i * 8, 9, bxColdHexFormat, (unsigned long) sha->h[i]);
}

static inline __attribute__((always_inline)) bool bxColdReadCache(const char* cachePath, const char* path, const struct stat* st, char digest[65]) {
    FILE* f = BxReceipt_open(cachePath, bxColdReadMode);
    if (f == NULL) return false;
    unsigned revision = 0;
    unsigned long long size = 0;
    long long mtime = 0;
    char savedPath[768];
    bool ok = fscanf(f, bxColdScanFormat, &revision, &size, &mtime, digest) == 4
           && fgets(savedPath, sizeof(savedPath), f) != NULL;
    fclose(f);
    if (!ok) return false;
    savedPath[strcspn(savedPath, bxColdNewlines)] = '\0';
    return revision == DATA_VERSIONS_REVISION && size == (uint64_t) st->st_size && mtime == (long long) st->st_mtime
        && strcmp(savedPath, path) == 0 && strlen(digest) == 64 && strspn(digest, bxColdDigits) == 64;
}

static inline __attribute__((always_inline)) void bxColdWriteCache(const char* cachePath, const char* path, const struct stat* st, const char* digest) {
    if (strpbrk(path, bxColdNewlines) != NULL) return;
    FILE* f = fopen(cachePath, bxColdWriteMode);
    if (f == NULL) return;
    fprintf(f, bxColdWriteFormat, DATA_VERSIONS_REVISION, (unsigned long long) st->st_size,
            (long long) st->st_mtime, digest, path);
    fclose(f); // A partial/unwritable cache is ignored next boot.
}

BX_COLD_TEXT CtrDataVersionResult BxCold_check(const char* path, const char* game, const char* part,
                                        const char* cachePath, void (*progress)(void), bool* cached, char digest[65]) {
    *cached = false;
    digest[0] = '\0';
    struct stat before, after;
    if (stat(path, &before) != 0 || before.st_size < 0) return CTR_DATA_UNREADABLE;
    *cached = bxColdReadCache(cachePath, path, &before, digest);
    if (!*cached) {
        digest[0] = '\0';
        FILE* f = fopen(path, bxColdBinaryMode);
        if (f == NULL) return CTR_DATA_UNREADABLE;
        // Bounded temporary memory; use the original chunk size if larger allocations fail.
        size_t capacity = 1048576;
        uint8_t* buffer = malloc(capacity);
        if (buffer == NULL) { capacity = 262144; buffer = malloc(capacity); }
        if (buffer == NULL) {
            fclose(f);
            // Retain the original checker and its 64 KiB allocation path. No
            // cache/identity shortcut is taken when temporary memory is tight.
            return __real_CtrDataVersion_check(path, game, part, cachePath, progress, cached, digest);
        }
        Sha256 sha = {{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, 0, {0}, 0};
        size_t size;
        while ((size = fread(buffer, 1, capacity, f)) != 0) {
            // Preserve the existing callback's byte accounting and watchdog cadence.
            for (size_t offset = 0; offset < size; ) {
                size_t take = size - offset;
                if (take > 65536) take = 65536;
                bxColdShaUpdate(&sha, buffer + offset, take);
                offset += take;
                if (progress != NULL) progress();
            }
        }
        bool ok = !ferror(f) && sha.bytes == (uint64_t) before.st_size;
        if (fclose(f) != 0) ok = false;
        free(buffer);
        if (!ok || stat(path, &after) != 0 || before.st_size != after.st_size || before.st_mtime != after.st_mtime)
            return CTR_DATA_UNREADABLE;
        bxColdShaFinish(&sha, digest);
        bxColdWriteCache(cachePath, path, &before, digest);
    }
    for (size_t i = 0; i < sizeof(knownFiles) / sizeof(knownFiles[0]); i++) {
        const KnownFile* known = &knownFiles[i];
        if (known->size == (uint64_t) before.st_size && strcmp(known->game, game) == 0
            && strcmp(known->part, part) == 0 && strcmp(known->digest, digest) == 0) return CTR_DATA_KNOWN;
    }
    return CTR_DATA_UNKNOWN;
}


#undef BX_COLD_TEXT
