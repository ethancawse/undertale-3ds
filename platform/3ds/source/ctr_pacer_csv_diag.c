// Test-only complete sweep.csv filesystem bypass. Never enabled in a release.
// Both arms use one binary; pacer_nocsv.txt is sampled once at battle entry.
#ifdef CTRPORT_PACER_CSV_DIAG
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "gettime.h"
#include "log.h"

#define CSV_LATE __attribute__((section(".text.late.pacer"), noinline))
#define CSV_RO __attribute__((section(".rodata.pacer_in_text"), aligned(4)))
static const char csvPath[] CSV_RO = "sdmc:/3ds/undertale-3ds/sweep.csv";
static const char controlPath[] CSV_RO = "sdmc:/3ds/undertale-3ds/pacer_nocsv.txt";
static const char controlMode[] CSV_RO = "r";
static const char modeLog[] CSV_RO = "Pacer CSV diag: filesystem bypass %u (sampled once)\n";
static const char rowLog[] CSV_RO = "Pacer CSV diag: filesystem bypass %u, row calls %lu us\n";
static struct {
    bool known, bypass;
    unsigned char tag;
    FILE* stream;
    uint64_t began;
// Keep __bss_end__/__end__ and their startup/heap literals unchanged.
// Zero bytes occupy the existing data reserve without changing its contents.
} csv __attribute__((section(".data.late.pacer"))) = {0};

int __real_stat(const char*, struct stat*);
FILE* __real_fopen(const char*, const char*);
int __real_fputs(const char*, FILE*);
int __real_fputc(int, FILE*);
int __real_fclose(FILE*);
int __wrap_stat(const char*, struct stat*) CSV_LATE;
FILE* __wrap_fopen(const char*, const char*) CSV_LATE;
int __wrap_fputs(const char*, FILE*) CSV_LATE;
int __wrap_fputc(int, FILE*) CSV_LATE;
int __wrap_fclose(FILE*) CSV_LATE;
static void selectMode(void) CSV_LATE;

static void selectMode(void) {
    if (csv.known) return;
    FILE* f = __real_fopen(controlPath, controlMode);
    csv.bypass = f != NULL;
    if (f != NULL) __real_fclose(f);
    csv.known = true;
    logInfo(modeLog, (unsigned) csv.bypass);
}

int __wrap_stat(const char* path, struct stat* st) {
    if (strcmp(path, csvPath) != 0) return __real_stat(path, st);
    selectMode();
    csv.began = nowNanos();
    if (!csv.bypass) return __real_stat(path, st);
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG;
    st->st_size = 1; // Header unnecessary when rows go only to the existing log.
    return 0;
}

FILE* __wrap_fopen(const char* path, const char* mode) {
    if (strcmp(path, csvPath) != 0) return __real_fopen(path, mode);
    selectMode();
    csv.stream = csv.bypass ? (FILE*) &csv.tag : __real_fopen(path, mode);
    return csv.stream;
}

int __wrap_fputs(const char* s, FILE* f) {
    if (f == (FILE*) &csv.tag) return 0;
    return __real_fputs(s, f);
}

int __wrap_fputc(int c, FILE* f) {
    if (f == (FILE*) &csv.tag) return (unsigned char) c;
    return __real_fputc(c, f);
}

int __wrap_fclose(FILE* f) {
    bool row = f != NULL && f == csv.stream;
    int result = f == (FILE*) &csv.tag ? 0 : __real_fclose(f);
    if (row) {
        csv.stream = NULL;
        uint64_t elapsed = nowNanos() - csv.began;
        logInfo(rowLog, (unsigned) csv.bypass, (unsigned long) (elapsed / 1000));
    }
    return result;
}
#endif
