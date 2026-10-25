// RomFS supplement stays out of the existing benchmark compiler unit.
#include <3ds.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench_script.h"
#include "ctr_benchromfs.h"
#include "log.h"
#include "stb_ds.h"

const char* coverageGameDir(void);

static const char coverageMount[] __attribute__((section(".rodata.late.coverage"))) = "romfs";
static const char coveragePath[] __attribute__((section(".rodata.late.coverage"))) = "romfs:/bench-supplement.txt";
static const char coverageOverridePath[] __attribute__((section(".rodata.late.coverage"))) = "%s/benchscenes.txt";
static const char coverageReadMode[] __attribute__((section(".rodata.late.coverage"))) = "rb";
static const char coverageLoaded[] __attribute__((section(".rodata.late.coverage"))) = "Bench: RomFS coverage supplement loaded (%lu bytes)\n";

// Same signature as BenchScene_parseList: the existing caller only changes its
// call target. Preserve the SD replacement and expand includes against the
// primary list before discarding duplicate IDs (primary always wins).
__attribute__((noinline, noclone, section(".text.late.coverage")))
BenchScene* coverageParseScenes(const char* primary) {
    char overridePath[256];
    snprintf(overridePath, sizeof(overridePath), coverageOverridePath, coverageGameDir());
    FILE* override = fopen(overridePath, coverageReadMode);
    if (override != nullptr) {
        bool replaces = fseek(override, 0, SEEK_END) == 0 && ftell(override) > 0;
        fclose(override);
        if (replaces) return BenchScene_parseList(primary);
    }
    // The bench's own RomFS mount, shared and cached (ctr_romfs.c): the supplement sits next to bench_scenes.txt.
    if (R_FAILED(CtrRomfs_init(coverageMount))) return BenchScene_parseList(primary);
    FILE* f = fopen(coveragePath, coverageReadMode);
    if (f == nullptr) return BenchScene_parseList(primary);
    long size = 0;
    if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
    if (size <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return BenchScene_parseList(primary);
    }
    size_t primarySize = strlen(primary);
    char* text = malloc(primarySize + (size_t) size + 2);
    if (text == nullptr) {
        fclose(f);
        return BenchScene_parseList(primary);
    }
    memcpy(text, primary, primarySize);
    text[primarySize] = '\n';
    size_t got = fread(text + primarySize + 1, 1, (size_t) size, f);
    bool complete = got == (size_t) size && !ferror(f);
    fclose(f);
    if (!complete) {
        free(text);
        return BenchScene_parseList(primary);
    }
    text[primarySize + 1 + got] = '\0';
    BenchScene* all = BenchScene_parseList(text);
    free(text);
    for (ptrdiff_t i = 0; arrlen(all) > i; i++) {
        for (ptrdiff_t j = 0; i > j; j++) {
            if (strcmp(all[i].id, all[j].id) != 0) continue;
            free(all[i].id);
            free(all[i].title);
            free(all[i].script);
            arrdel(all, i);
            i--;
            break;
        }
    }
    logInfo(coverageLoaded, (unsigned long) got);
    return all;
}
