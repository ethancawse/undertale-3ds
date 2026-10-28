// Run-only short-window option. Keep parsing, reporting and wrapper logic out
// of the original display and benchmark translation units and string pools.
#include <stdio.h>
#include <string.h>

#include "ctr_benchmark.h"
#include "ctr_benchmark_rows_sink.h"
#line 7
#include "ctr_benchfid_window.h"
#include "ctr_log.h"

#define LATE __attribute__((noinline, noclone, section(".text.late.benchfid")))
#define RODATA __attribute__((section(".rodata.late.benchfid")))
static bool noSettle __attribute__((section(".bss.late.benchfid")));
static const char settleKey[] RODATA = "bench_settle_frames";
static const char settleParse[] RODATA = " %63[^= ] = %d";
static const char benchfidReadMode[] RODATA = "r";
static const char linkPrefix[] RODATA = "[result] ";

void __real_CtrDisplay_load(CtrDisplaySettings*, const char*, CtrDisplayCaps);
bool __real_CtrDisplay_override(CtrDisplaySettings*, const char*, CtrDisplayCaps);

// 0: another setting; 1: accepted; -1: our setting with an invalid value.
static int LATE settleLine(const char* line) {
    char key[64];
    int value;
    if (sscanf(line, settleParse, key, &value) != 2 || strcmp(key, settleKey) != 0) return 0;
    if (value != 0 && value != 90) return -1;
    noSettle = value == 0;
    return 1;
}

void LATE benchfidLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    __real_CtrDisplay_load(settings, path, caps);
    noSettle = false;
    FILE* f = fopen(path, benchfidReadMode);
    if (f != NULL) {
        char line[128];
        while (fgets(line, sizeof(line), f) != NULL) settleLine(line);
        fclose(f);
    }
}

bool LATE benchfidOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    int kind = settleLine(line);
    return kind != 0 ? kind > 0 : __real_CtrDisplay_override(settings, line, caps);
}

CtrBenchFrame LATE benchfidBeginFrame(u32 down, double frameMs) {
    CtrBenchFrame result = CtrBench_beginFrame(down, frameMs);
    if (result == BENCH_FRAME_RUN && noSettle) {
        BenchfidWindowHook hook = benchfidWindowHook();
        if (hook.ready) hook.open();
    }
    return result;
}

void LATE benchfidEmitWindow(FILE* f, const char* format, int frames, int historicalSettle) {
    char line[512];
    snprintf(line, sizeof(line), format, frames, noSettle ? 0 : historicalSettle);
    if (f != NULL) CtrBench_resultPuts(line, f);
    if (CtrLog_linkOpen()) {
        CtrLog_linkWrite(linkPrefix);
        CtrLog_linkWrite(line);
    }
}
