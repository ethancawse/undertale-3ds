// UNDERTALE Legal reader only. The retained painter consumes identical prewrapped .pages records.
// Only routed reader edges receive this private token; it is never passed to a libc FILE consumer.
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../engine/vendor/miniz/miniz.h"
#include "../../../engine/src/log.h"
#include "ctr_memory.h"

#ifndef NSCACHE_CODE
#define NSCACHE_CODE __attribute__((noinline, noipa, optimize("O2"), section(".text.late.noticescroll_cache")))
#endif
#ifndef NSCACHE_DATA
#define NSCACHE_DATA __attribute__((section(".data.late.noticescroll")))
#endif
#define NSCACHE_CAPACITY 8192u
#ifndef NSCACHE_RODATA
#define NSCACHE_RODATA __attribute__((section(".rodata.late.noticescroll_pack")))
#endif
static const char nsPackPath[] NSCACHE_RODATA = "romfs:/legal/READER.lgrz";
static const char nsFallbackMessage[] NSCACHE_RODATA = "Legal cache: resident records unavailable; bounded FILE fallback\n";

static struct {
    FILE* file;
    unsigned char* bytes;
    uint32_t position, filePosition, start, length, size;
    int32_t document;
    union { unsigned char header[8]; unsigned char* packed; };
    uint32_t headerLength;
    bool primed, hooked;
    unsigned char registeredExit; // bit 0: exit hook; bit 1: fallback already logged
    aptHookCookie cookie;
} nsDocument NSCACHE_DATA = {.document = -1};

// A late four-byte branch reaches the original local luiOpen without changing GCC's original C call graph.
FILE* CtrNoticescroll_openOriginal(int32_t document);
FILE* CtrNoticescroll_open(int32_t document);
size_t CtrNoticescroll_read(void* ptr, size_t size, size_t count, FILE* file);
int CtrNoticescroll_seek(FILE* file, long offset, int whence);
int CtrNoticescroll_close(FILE* file);
void CtrNoticescroll_update(CtrMenu* menu, u32 down);
void __real_CtrMenu_close(void* menu);
void CtrNoticescroll_menuClose(void* menu);
void __real_CtrExit_begin(int seconds);
void CtrNoticescroll_beginExit(int seconds);

static NSCACHE_CODE void nsDocumentRelease(void) {
    if (nsDocument.file != NULL) fclose(nsDocument.file);
    if (nsDocument.headerLength == UINT32_MAX) free(nsDocument.packed);
    if (nsDocument.bytes != NULL) free(nsDocument.bytes);
    nsDocument.file = NULL;
    nsDocument.bytes = NULL;
    memset(nsDocument.header, 0, sizeof(nsDocument.header));
    nsDocument.position = nsDocument.filePosition = nsDocument.start = nsDocument.length = 0;
    nsDocument.size = nsDocument.headerLength = 0;
    nsDocument.document = -1;
    nsDocument.primed = false;
}

static NSCACHE_CODE void nsDocumentOnApt(APT_HookType hook, void* arg) {
    (void) arg;
    // libctru invokes these hooks on the aptMainLoop thread before handing over to HOME or sleep.
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP || hook == APTHOOK_ONEXIT) nsDocumentRelease();
}

static NSCACHE_CODE void nsDocumentExit(void) {
    nsDocumentRelease();
    if (nsDocument.hooked) {
        aptUnhook(&nsDocument.cookie);
        nsDocument.hooked = false;
    }
}

static NSCACHE_CODE uint32_t nsPackWord(const unsigned char* p) {
    return (uint32_t) p[0] | (uint32_t) p[1] << 8 | (uint32_t) p[2] << 16 | (uint32_t) p[3] << 24;
}

static NSCACHE_CODE void nsDocumentFallbackLog(void) {
    if (!(nsDocument.registeredExit & 2u)) {
        nsDocument.registeredExit |= 2u;
        logInfo(nsFallbackMessage);
    }
}

// Packed allocation includes the inflater workspace, aligned to eight bytes. No large stack frame,
// third allocation, new static state, or per-scroll allocation; tinfl_decompress is already linked.
static NSCACHE_CODE size_t nsPackDecode(uint32_t position) {
    unsigned char* packed = nsDocument.packed;
    uint32_t block = position / NSCACHE_CAPACITY;
    uint32_t start = block * NSCACHE_CAPACITY;
    if (start >= nsDocument.size) return 0;
    if (nsDocument.document == 1) block += (nsPackWord(packed + 12) + NSCACHE_CAPACITY - 1u) / NSCACHE_CAPACITY;
    uint32_t first = nsPackWord(packed + 24u + block * 4u);
    uint32_t last = nsPackWord(packed + 28u + block * 4u);
    size_t input = last - first;
    size_t output = nsDocument.size - start;
    if (output > NSCACHE_CAPACITY) output = NSCACHE_CAPACITY;
    size_t expected = output;
    tinfl_decompressor* decoder = (tinfl_decompressor*) (void*) (packed + ((nsDocument.filePosition + 7u) & ~7u));
    tinfl_init(decoder);
    tinfl_status status = tinfl_decompress(decoder, packed + first, &input, nsDocument.bytes,
        nsDocument.bytes, &output, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (status != TINFL_STATUS_DONE || output != expected || input != last - first) return 0;
    nsDocument.start = start;
    nsDocument.length = (uint32_t) output;
    return output;
}

static NSCACHE_CODE bool nsPackLoad(int32_t document) {
    if ((uint32_t) document > 1u) return false;
    FILE* file = fopen(nsPackPath, "rb");
    if (file == NULL) return false;
    unsigned char header[24];
    unsigned char* packed = NULL;
    unsigned char* bytes = NULL;
    bool valid = fread(header, 1, sizeof(header), file) == sizeof(header);
    uint32_t size = valid ? nsPackWord(header + 4) : 0;
    uint32_t count = 0;
    if (valid) valid = nsPackWord(header) == 0x3150534eu && size >= 36u && size <= 262144u &&
                       nsPackWord(header + 8) == NSCACHE_CAPACITY;
    for (unsigned doc = 0; valid && doc < 2; doc++) {
        uint32_t n = nsPackWord(header + 12 + doc * 4);
        uint32_t pageBytes = CTR_RELEASE_LEGAL_LINES * (CTR_RELEASE_LEGAL_COLUMNS + 1u);
        valid = n >= 8u + pageBytes && n <= 8u + 65535u * pageBytes && (n - 8u) % pageBytes == 0;
        count += (n + NSCACHE_CAPACITY - 1u) / NSCACHE_CAPACITY;
    }
    if (valid) valid = 24u + (count + 1u) * 4u < size;
    if (valid) {
        // This optional cache has a FILE fallback. Do not evict gameplay textures or spend linear
        // memory to make its heap allocation succeed, especially on Old during Omega Flowey.
        bool allowed = CtrMemory_allowRelief(false);
        packed = malloc(((size + 7u) & ~7u) + sizeof(tinfl_decompressor));
        if (packed != NULL) bytes = malloc(NSCACHE_CAPACITY);
        CtrMemory_allowRelief(allowed);
        valid = packed != NULL && bytes != NULL;
    }
    if (valid) {
        memcpy(packed, header, sizeof(header));
        valid = fread(packed + sizeof(header), 1, size - sizeof(header), file) == size - sizeof(header);
    }
    fclose(file); // no SD handle remains on the resident path, including all error paths
    if (valid) {
        uint32_t checksum = 2166136261u;
        for (uint32_t i = 0; i < size; i++) if (i < 20u || i >= 24u) checksum = (checksum ^ packed[i]) * 16777619u;
        valid = checksum == nsPackWord(header + 20);
        uint32_t previous = 24u + (count + 1u) * 4u;
        valid = valid && nsPackWord(packed + 24) == previous;
        for (uint32_t i = 1; valid && i <= count; i++) {
            uint32_t next = nsPackWord(packed + 24u + i * 4u);
            valid = next > previous && next <= size;
            previous = next;
        }
        valid = valid && previous == size;
    }
    if (!valid) { free(packed); free(bytes); return false; }
    nsDocument.packed = packed;
    nsDocument.bytes = bytes;
    nsDocument.document = document;
    nsDocument.filePosition = size;
    nsDocument.size = nsPackWord(header + 12 + (unsigned) document * 4);
    nsDocument.headerLength = UINT32_MAX;
    valid = nsPackDecode(0) >= 8u && nsPackWord(bytes) == 0x314c474cu &&
        nsPackWord(bytes + 4) == (nsDocument.size - 8u) / (CTR_RELEASE_LEGAL_LINES * (CTR_RELEASE_LEGAL_COLUMNS + 1u));
    if (!valid) nsDocumentRelease();
    return valid;
}

NSCACHE_CODE FILE* CtrNoticescroll_open(int32_t document) {
    if ((nsDocument.file != NULL || nsDocument.headerLength == UINT32_MAX) && nsDocument.document == document) {
        nsDocument.position = 0;
        return (FILE*) (void*) &nsDocument;
    }
    nsDocumentRelease();
    if (!nsPackLoad(document)) {
        nsDocumentFallbackLog();
        FILE* file = CtrNoticescroll_openOriginal(document);
        if (file == NULL) return NULL;
        bool allowed = CtrMemory_allowRelief(false);
        unsigned char* bytes = malloc(NSCACHE_CAPACITY);
        CtrMemory_allowRelief(allowed);
        if (bytes == NULL) return file; // ordinary reader, ordinary close and unavailable/error behavior
        nsDocument.file = file;
        nsDocument.bytes = bytes;
        nsDocument.document = document;
    }
    if (!nsDocument.hooked) {
        aptHook(&nsDocument.cookie, nsDocumentOnApt, NULL);
        nsDocument.hooked = true;
    }
    if (!(nsDocument.registeredExit & 1u) && atexit(nsDocumentExit) == 0) nsDocument.registeredExit |= 1u;
    return (FILE*) (void*) &nsDocument;
}

static NSCACHE_CODE size_t nsDocumentFill(uint32_t position, size_t wanted) {
    if (nsDocument.headerLength == UINT32_MAX) {
        if (position >= nsDocument.size) return 0;
        size_t got = nsPackDecode(position);
        if (got != 0) return got;
        // Invalid block: retain the logical position and decode cache, resume the original byte reader.
        // This correctness fallback may touch SD; qualified generated assets never take this path.
        free(nsDocument.packed);
        memset(nsDocument.header, 0, sizeof(nsDocument.header));
        nsDocument.headerLength = nsDocument.filePosition = nsDocument.length = 0;
        nsDocument.file = CtrNoticescroll_openOriginal(nsDocument.document);
        nsDocumentFallbackLog();
        if (nsDocument.file == NULL) return 0;
    }
    uint32_t start = position;
    size_t length = wanted < NSCACHE_CAPACITY ? wanted : NSCACHE_CAPACITY;
    // First paint reads exactly the original header, trim page and visible records. Never bulk-read on open.
    // Subsequent misses fill ahead, or behind when scrolling backwards, to avoid a miss on every upward move.
    if (nsDocument.primed && nsDocument.size > position && position >= 8u) {
        if (position < nsDocument.start) {
            uint32_t behind = NSCACHE_CAPACITY - (uint32_t) length;
            start = position - 8u > behind ? position - behind : 8u;
        }
        length = nsDocument.size - start;
        if (length > NSCACHE_CAPACITY) length = NSCACHE_CAPACITY;
    }
    nsDocument.length = 0;
    if (nsDocument.filePosition != start && fseek(nsDocument.file, (long) start, SEEK_SET) != 0) return 0;
    size_t got = fread(nsDocument.bytes, 1, length, nsDocument.file);
    nsDocument.filePosition = start + (uint32_t) got;
    nsDocument.start = start;
    nsDocument.length = (uint32_t) got;
    return got;
}

NSCACHE_CODE size_t CtrNoticescroll_read(void* ptr, size_t size, size_t count, FILE* file) {
    if (file != (FILE*) (void*) &nsDocument) return fread(ptr, size, count, file);
    if (size == 0 || count == 0 || count > SIZE_MAX / size ||
        (nsDocument.file == NULL && nsDocument.headerLength != UINT32_MAX)) return 0;
    size_t wanted = size * count, copied = 0;
    unsigned char* out = ptr;
    while (copied < wanted) {
        uint32_t position = nsDocument.position;
        if (position < 8u && nsDocument.headerLength != UINT32_MAX) {
            if (nsDocument.headerLength == 0) {
                if (nsDocument.filePosition != 0 && fseek(nsDocument.file, 0, SEEK_SET) != 0) break;
                nsDocument.headerLength = (uint32_t) fread(nsDocument.header, 1, 8, nsDocument.file);
                nsDocument.filePosition = nsDocument.headerLength;
                uint32_t header[2];
                memcpy(header, nsDocument.header, sizeof(header));
                if (nsDocument.headerLength == 8 && header[0] == 0x314c474cu && header[1] - 1u < 65535u)
                    nsDocument.size = 8u + header[1] * CTR_RELEASE_LEGAL_LINES * (CTR_RELEASE_LEGAL_COLUMNS + 1u);
            }
            if (position >= nsDocument.headerLength) break;
            size_t n = nsDocument.headerLength - position;
            if (n > wanted - copied) n = wanted - copied;
            memcpy(out + copied, nsDocument.header + position, n);
            nsDocument.position += (uint32_t) n;
            copied += n;
            continue;
        }
        size_t remaining = wanted - copied;
        size_t part = remaining < NSCACHE_CAPACITY ? remaining : NSCACHE_CAPACITY;
        uint32_t offset = position - nsDocument.start;
        if (position < nsDocument.start || offset >= nsDocument.length ||
            (nsDocument.headerLength != UINT32_MAX && part > nsDocument.length - offset)) {
            if (nsDocumentFill(position, part) == 0) break;
            offset = position - nsDocument.start;
        }
        if (offset >= nsDocument.length) break;
        size_t n = nsDocument.length - offset;
        if (n > remaining) n = remaining;
        memcpy(out + copied, nsDocument.bytes + offset, n);
        nsDocument.position += (uint32_t) n;
        copied += n;
    }
    return copied / size;
}

NSCACHE_CODE int CtrNoticescroll_seek(FILE* file, long offset, int whence) {
    if (file != (FILE*) (void*) &nsDocument) return fseek(file, offset, whence);
    int64_t start = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t) nsDocument.position :
                    whence == SEEK_END && nsDocument.size != 0 ? (int64_t) nsDocument.size : -1;
    if (start < 0 || offset < -start || offset > (int64_t) UINT32_MAX - start) return -1;
    int64_t position = start + offset;
    if (position > LONG_MAX) return -1;
    nsDocument.position = (uint32_t) position;
    return 0;
}

NSCACHE_CODE int CtrNoticescroll_close(FILE* file) {
    if (file != (FILE*) (void*) &nsDocument) return fclose(file);
    nsDocument.primed = true;
    return 0; // end of one paint, not reader exit
}

NSCACHE_CODE void CtrNoticescroll_update(CtrMenu* menu, u32 down) {
    CtrCredroll_update(menu, down);
    if (nsDocument.bytes != NULL && (!CtrLegal_shown() || lui.view != LUI_DOC)) nsDocumentRelease();
}

NSCACHE_CODE void CtrNoticescroll_menuClose(void* menu) {
    nsDocumentRelease();
    __real_CtrMenu_close(menu); // called inside menupause's existing wrapper, before its audio resume
}

NSCACHE_CODE void CtrNoticescroll_beginExit(int seconds) {
    __real_CtrExit_begin(seconds);
    nsDocumentRelease(); // existing bench exit wrapper arms the guard here, then flushes its rows on return
}
