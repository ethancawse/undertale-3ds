#include "ctr_benchromfs.h"

#include <stdio.h>
#include <stdlib.h>

#include "log.h"

#define LATE __attribute__((noinline, section(".text.late.benchromfs")))
#define LATE_CONST __attribute__((section(".rodata.late.benchromfs")))
static const char mountName[] LATE_CONST = "romfs";
static const char scenePath[] LATE_CONST = "romfs:/bench_scenes.txt";
static const char readMode[] LATE_CONST = "rb";
static const char loadedFormat[] LATE_CONST = "Bench: scenes from %s\n";
static const char mountError[] LATE_CONST = "Bench: ERROR romfs mount failed (%08lX); no scenes loaded\n";
static const char readError[] LATE_CONST = "Bench: ERROR cannot read %s; no scenes loaded\n";

LATE char* CtrBench_readRomfs(void) {
    Result result = CtrRomfs_init(mountName);
    if (R_FAILED(result)) {
        logError(mountError, result);
        return NULL;
    }
    FILE* file = fopen(scenePath, readMode);
    char* text = NULL;
    if (file != NULL) {
        if (fseek(file, 0, SEEK_END) == 0) {
            long size = ftell(file);
            if (size >= 0 && fseek(file, 0, SEEK_SET) == 0) {
                // malloc avoids introducing utils.h's __FILE__ string into ordinary rodata.
                text = malloc((size_t) size + 1);
                if (text != NULL) {
                    size_t got = fread(text, 1, (size_t) size, file);
                    if (got == (size_t) size) text[got] = '\0';
                    else { free(text); text = NULL; }
                }
            }
        }
        fclose(file);
    }
    if (text != NULL) logInfo(loadedFormat, scenePath);
    else logError(readError, scenePath);
    return text;
}
