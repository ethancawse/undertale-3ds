#include "ctr_link_files.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common.h"

typedef struct {
    char* path;
    char name[CTR_LINK_FILES_NAME_MAX];
    uint32_t size;
    uint8_t rank;
    bool send;
} LinkFile;

static bool enabled;
static uint32_t capBytes;
static LinkFile* list; // in the order noted
static int count;
static int capacity;
static int lost;       // notes past CTR_LINK_FILES_MAX, or with no memory for them
static bool prepared;  // CtrLinkFiles_prepare ran and CtrLinkFiles_send hasn't yet
static volatile uint32_t progress;
static volatile bool sending;

void CtrLinkFiles_enable(uint32_t cap) {
    enabled = true;
    capBytes = cap;
}

bool CtrLinkFiles_enabled(void) {
    return enabled;
}

void CtrLinkFiles_note(const char* path) {
    if (!enabled || path == nullptr || path[0] == '\0') return;
    for (int i = 0; count > i; i++) {
        if (strcmp(list[i].path, path) == 0) return;
    }
    if (count >= CTR_LINK_FILES_MAX) {
        lost++;
        return;
    }
    if (count == capacity) {
        int grown = capacity == 0 ? 16 : capacity * 2;
        LinkFile* bigger = realloc(list, (size_t) grown * sizeof(LinkFile));
        if (bigger == nullptr) {
            lost++;
            return;
        }
        list = bigger;
        capacity = grown;
    }
    size_t length = strlen(path);
    char* copy = malloc(length + 1);
    if (copy == nullptr) {
        lost++;
        return;
    }
    memcpy(copy, path, length + 1);
    memset(&list[count], 0, sizeof(LinkFile));
    list[count++].path = copy;
}

// The order files go in: what the tests compare most first, in case the cap or the connection runs out.
static uint8_t rankOf(const char* name) {
    if (strcmp(name, "state_hash.log") == 0) return 0;
    if (strncmp(name, "top_", 4) == 0 || strncmp(name, "right_", 6) == 0) return 1;
    if (strncmp(name, "shot_", 5) == 0) return 2;
    if (strncmp(name, "bottom_", 7) == 0) return 3;
    return 4;
}

// The file's name for the PC: its base name with anything outside [A-Za-z0-9._-] made '_', at most
// CTR_LINK_FILES_NAME_MAX - 1 characters, not starting with a dot, and not one an earlier file already has (then
// numbered: 2-top_00834.bmp); and its rank, from the base name.
static void nameFor(int index) {
    const char* path = list[index].path;
    const char* base = strrchr(path, '/');
    base = base != nullptr ? base + 1 : path;
    char clean[CTR_LINK_FILES_NAME_MAX];
    size_t n = 0;
    for (; base[n] != '\0' && sizeof(clean) - 1 > n; n++) {
        char c = base[n];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        clean[n] = ok ? c : '_';
    }
    clean[n] = '\0';
    if (n == 0) snprintf(clean, sizeof(clean), "file");
    if (clean[0] == '.') clean[0] = '_';
    list[index].rank = rankOf(clean);
    char* name = list[index].name;
    snprintf(name, CTR_LINK_FILES_NAME_MAX, "%s", clean);
    for (int k = 2;; k++) {
        bool taken = false;
        for (int i = 0; index > i && !taken; i++) taken = strcmp(list[i].name, name) == 0;
        if (!taken) return;
        char numbered[CTR_LINK_FILES_NAME_MAX + 16];
        int length = snprintf(numbered, sizeof(numbered), "%d-%s", k, clean);
        if (length > CTR_LINK_FILES_NAME_MAX - 1) length = CTR_LINK_FILES_NAME_MAX - 1;
        memcpy(name, numbered, (size_t) length);
        name[length] = '\0';
    }
}

static void reportf(void (*report)(void*, bool, const char*), void* ctx, bool warning, const char* format, ...)
    __attribute__((format(printf, 4, 5)));
static void reportf(void (*report)(void*, bool, const char*), void* ctx, bool warning, const char* format, ...) {
    if (report == nullptr) return;
    char line[256];
    va_list va;
    va_start(va, format);
    vsnprintf(line, sizeof(line), format, va);
    va_end(va);
    report(ctx, warning, line);
}

int CtrLinkFiles_prepare(void (*report)(void* ctx, bool warning, const char* line), void* ctx) {
    if (!enabled || prepared) return 0;
    prepared = true;
    for (int i = 0; count > i; i++) {
        nameFor(i);
        struct stat st;
        if (stat(list[i].path, &st) != 0) {
            reportf(report, ctx, true, "Remote: not sending %s to the PC: it is gone", list[i].name);
            list[i].rank = 0xFF;
        } else {
            list[i].size = (uint64_t) st.st_size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t) st.st_size;
        }
    }
    int sent = 0;
    uint32_t total = 0;
    for (uint8_t rank = 0; 4 >= rank; rank++) {
        for (int i = 0; count > i; i++) {
            if (list[i].rank != rank) continue;
            if (list[i].size > capBytes - total) {
                reportf(report, ctx, true, "Remote: not sending %s (%lu bytes) to the PC: the run's files are capped at %lu MB "
                        "(--send-files=<MB>); it stays on the SD card", list[i].name, (unsigned long) list[i].size,
                        (unsigned long) (capBytes >> 20));
                continue;
            }
            list[i].send = true;
            total += list[i].size;
            sent++;
        }
    }
    if (lost > 0) {
        reportf(report, ctx, true, "Remote: %d more files were written than can be sent to the PC (the first %d were noted)", lost,
                CTR_LINK_FILES_MAX);
    }
    if (sent > 0) {
        reportf(report, ctx, false, "Remote: sending %d files (%lu bytes) to the PC after the log", sent, (unsigned long) total);
    } else {
        reportf(report, ctx, false, "Remote: no files to send to the PC");
    }
    return sent;
}

bool CtrLinkFiles_pending(void) {
    if (!prepared) return false;
    for (int i = 0; count > i; i++) {
        if (list[i].send) return true;
    }
    return false;
}

uint32_t CtrLinkFiles_progress(void) {
    return progress;
}

bool CtrLinkFiles_sending(void) {
    return sending;
}

bool CtrLinkFiles_busy(void) {
    return sending || CtrLinkFiles_pending();
}

static bool sendText(const CtrLinkFilesSink* sink, const char* format, ...) __attribute__((format(printf, 2, 3)));
static bool sendText(const CtrLinkFilesSink* sink, const char* format, ...) {
    char line[256];
    va_list va;
    va_start(va, format);
    int n = vsnprintf(line, sizeof(line), format, va);
    va_end(va);
    if (n < 0) return true;
    if ((size_t) n >= sizeof(line)) {
        n = (int) sizeof(line) - 1;
        line[n - 1] = '\n';
    }
    return sink->send(sink->ctx, line, (size_t) n);
}

#define DATA_PREFIX "[link] data "

// Sends one file: begin, data and end lines, or a skipped line. Returns false when the connection is gone. *sentBytes
// is what went, -1 when it was skipped.
static bool sendOne(const CtrLinkFilesSink* sink, const LinkFile* file, uint8_t* raw, char* line, long* sentBytes) {
    *sentBytes = -1;
    FILE* f = fopen(file->path, "rb");
    if (f == nullptr) return sendText(sink, "[link] file skipped %s: it could not be opened\n", file->name);
    // First the CRC for the header, then the data; the end line carries what was actually sent.
    uint32_t size = 0, crc = 0;
    size_t n;
    while ((n = fread(raw, 1, CTR_LINK_FILES_CHUNK, f)) > 0) {
        crc = CtrLinkFiles_crc32(crc, raw, n);
        size += (uint32_t) n;
    }
    if (ferror(f) || size > file->size || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return sendText(sink, "[link] file skipped %s: it could not be read, or grew after the run (%lu bytes, %lu expected)\n",
                        file->name, (unsigned long) size, (unsigned long) file->size);
    }
    bool up = sendText(sink, "[link] file begin %s %lu %08lx\n", file->name, (unsigned long) size, (unsigned long) crc);
    uint32_t sentSize = 0, sentCrc = 0;
    memcpy(line, DATA_PREFIX, sizeof(DATA_PREFIX) - 1);
    while (up && (n = fread(raw, 1, CTR_LINK_FILES_CHUNK, f)) > 0) {
        sentCrc = CtrLinkFiles_crc32(sentCrc, raw, n);
        sentSize += (uint32_t) n;
        size_t length = sizeof(DATA_PREFIX) - 1;
        length += CtrLinkFiles_base64(line + length, raw, n);
        line[length++] = '\n';
        up = sink->send(sink->ctx, line, length);
        progress += (uint32_t) n;
    }
    fclose(f);
    if (!up) return false;
    *sentBytes = (long) sentSize;
    return sendText(sink, "[link] file end %s %lu %08lx\n", file->name, (unsigned long) sentSize, (unsigned long) sentCrc);
}

bool CtrLinkFiles_send(const CtrLinkFilesSink* sink) {
    if (!CtrLinkFiles_pending()) {
        prepared = false;
        return true;
    }
    sending = true; // before prepared goes, so CtrLinkFiles_busy stays true throughout
    prepared = false;
    uint64_t startMs = sink->nowMs != nullptr ? sink->nowMs() : 0;
    int planned = 0;
    uint32_t plannedBytes = 0;
    for (int i = 0; count > i; i++) {
        if (!list[i].send) continue;
        planned++;
        plannedBytes += list[i].size;
    }
    uint8_t* raw = malloc(CTR_LINK_FILES_CHUNK);
    char* line = malloc(sizeof(DATA_PREFIX) - 1 + (CTR_LINK_FILES_CHUNK + 2) / 3 * 4 + 2);
    bool up = raw != nullptr && line != nullptr
                  ? sendText(sink, "[link] files v1: %d files, %lu bytes\n", planned, (unsigned long) plannedBytes)
                  : sendText(sink, "[link] files v1: 0 files, 0 bytes (no memory to send %d)\n", planned);
    int done = 0;
    uint32_t doneBytes = 0;
    // In the order CtrLinkFiles_prepare ranked them.
    for (uint8_t rank = 0; 4 >= rank && up && raw != nullptr && line != nullptr; rank++) {
        for (int i = 0; count > i && up; i++) {
            if (!list[i].send || list[i].rank != rank) continue;
            list[i].send = false;
            long sentBytes;
            up = sendOne(sink, &list[i], raw, line, &sentBytes);
            if (sentBytes >= 0) {
                done++;
                doneBytes += (uint32_t) sentBytes;
            }
        }
    }
    if (up) {
        unsigned long ms = sink->nowMs != nullptr ? (unsigned long) (sink->nowMs() - startMs) : 0ul;
        up = sendText(sink, "[link] files done: %d of %d files, %lu bytes in %lu ms\n", done, planned, (unsigned long) doneBytes, ms);
    }
    free(raw);
    free(line);
    for (int i = 0; count > i; i++) list[i].send = false;
    sending = false;
    return up;
}

void CtrLinkFiles_reset(void) {
    for (int i = 0; count > i; i++) free(list[i].path);
    free(list);
    list = nullptr;
    count = capacity = lost = 0;
    enabled = prepared = sending = false;
    capBytes = 0;
    progress = 0;
}

// zlib's CRC-32 half a byte at a time: a 64-byte table instead of 1 KB.
static const uint32_t crcNibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};

uint32_t CtrLinkFiles_crc32(uint32_t crc, const void* data, size_t length) {
    const uint8_t* p = data;
    crc = ~crc;
    while (length-- > 0) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crcNibble[crc & 15];
        crc = (crc >> 4) ^ crcNibble[crc & 15];
    }
    return ~crc;
}

size_t CtrLinkFiles_base64(char* out, const uint8_t* data, size_t length) {
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    size_t i = 0;
    for (; length >= i + 3; i += 3) {
        uint32_t v = (uint32_t) data[i] << 16 | (uint32_t) data[i + 1] << 8 | data[i + 2];
        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = digits[(v >> 6) & 63];
        out[o++] = digits[v & 63];
    }
    if (length > i) {
        uint32_t v = (uint32_t) data[i] << 16 | (length > i + 1 ? (uint32_t) data[i + 1] << 8 : 0);
        out[o++] = digits[v >> 18];
        out[o++] = digits[(v >> 12) & 63];
        out[o++] = length > i + 1 ? digits[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}
