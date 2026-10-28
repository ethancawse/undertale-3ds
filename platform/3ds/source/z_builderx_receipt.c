// Cold receipt binding. The ordinary checker still validates revision/path/size/stat time and digest syntax.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __3DS__
#include <3ds/archive.h>
// appInit already pulls archive_dev.o for archiveMountSdmc. A strong reference here would pull it
// earlier and reorder existing libctru text. Resolve from that same object without changing its order.
#pragma weak archive_getmtime
#else
// Host tests supply a deterministic archive service, never a console connection.
int archive_getmtime(const char* path, uint64_t* stamp);
#endif

#define RX __attribute__((noinline, section(".text.late.builderxreceipt")))
#define RR __attribute__((section(".rodata.late.builderxreceipt")))
static const char rxAppend[] RR = "a";
static const char rxScan[] RR = "FAT1 %llu%c", rxPrint[] RR = "FAT1 %llu\n";
static const char rxEnd[] RR = "\r\n";

FILE* BxReceipt_open(const char* cachePath, const char* mode);
RX FILE* BxReceipt_open(const char* cachePath, const char* mode) {
#ifdef __3DS__
    if (!archive_getmtime) return NULL; // An archive-less build must hash instead of trusting a receipt.
#endif
    FILE* f = fopen(cachePath, mode);
    if (!f) return NULL;
    char line[256], path[768];
    uint64_t before, after;
    unsigned long long saved;
    char end;
    if (!fgets(line, sizeof(line), f) || !fgets(path, sizeof(path), f)) goto miss;
    path[strcspn(path, rxEnd)] = 0;
    if (archive_getmtime(path, &before) != 0) goto miss;
    if (fgets(line, sizeof(line), f)) {
        // A malformed/partial binding never becomes a fresh unbound receipt.
        if (sscanf(line, rxScan, &saved, &end) != 2 || end != '\n' || saved != before) goto miss;
    } else {
        if (ferror(f)) goto miss;
        FILE* out = fopen(cachePath, rxAppend);
        if (!out) goto miss;
        int wrote = fprintf(out, rxPrint, (unsigned long long) before);
        int closed = fclose(out);
        if (wrote < 0 || closed) goto miss;
    }
    if (archive_getmtime(path, &after) != 0 || before != after) goto miss;
    if (fseek(f, 0, SEEK_SET) != 0) goto miss;
    return f;
miss:
    fclose(f);
    return NULL; // The original checker hashes current data and replaces the untrusted cache.
}
