#include "load_trace.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define MAX_PHASES 128
#define MAX_REQUESTS 8192

typedef struct {
    char name[32];
    uint64_t start, elapsed, readTicks, seekTicks, bytes;
    unsigned reads, seeks;
} TracePhase;

typedef struct {
    uint64_t start, elapsed;
    long offset;
    size_t requested, actual;
    int32_t codeId;
    unsigned phase;
    int origin;
    char kind;
} TraceRequest;

typedef struct {
    TracePhase phases[MAX_PHASES];
    TraceRequest requests[MAX_REQUESTS];
    unsigned phaseCount, requestCount, dropped;
    int32_t codeId;
    const char* scope;
    uint64_t (*clock)(void);
    double ticksPerSecond;
} Trace;

static Trace* trace;

#if defined(PLATFORM_3DS)
// Use the already-linked sink directly: retaining the otherwise unused
// vLogInfo trampoline would add ordinary text ahead of the warm engine.
void platformLog(logType type, const char* format, va_list args);
// Keep the caller's format and ABI unchanged so this verbosity change does not
// move the runner's warm text or its existing literal pool. No new frame work.
void LoadTrace_logGlobalInit(const char* format, ...) {
#if defined(NDEBUG)
    if (trace == NULL) return;
#endif
    va_list args;
    va_start(args, format);
    platformLog(LOG_TYPE_NORMAL, format, args);
    va_end(args);
}
#endif

void LoadTrace_begin(const char* scope, uint64_t (*clock)(void), double ticksPerSecond) {
    if (trace != NULL || clock == NULL || ticksPerSecond <= 0) return;
    trace = calloc(1, sizeof(*trace));
    if (trace == NULL) {
        logWarn("Load trace: allocation failed\n");
        return;
    }
    trace->scope = scope;
    trace->clock = clock;
    trace->ticksPerSecond = ticksPerSecond;
    trace->codeId = -1;
    LoadTrace_phase("setup");
}

void LoadTrace_phase(const char* phase) {
    if (trace == NULL) return;
    uint64_t now = trace->clock();
    if (trace->phaseCount > 0) {
        TracePhase* previous = &trace->phases[trace->phaseCount - 1];
        previous->elapsed = now - previous->start;
    }
    if (trace->phaseCount == MAX_PHASES) {
        trace->dropped++;
        return;
    }
    TracePhase* next = &trace->phases[trace->phaseCount++];
    snprintf(next->name, sizeof(next->name), "%s", phase);
    next->start = now;
}

static void request(char kind, uint64_t start, uint64_t end, long offset, int origin, size_t requested, size_t actual) {
    if (trace->requestCount == MAX_REQUESTS) {
        trace->dropped++;
        return;
    }
    TraceRequest* r = &trace->requests[trace->requestCount++];
    *r = (TraceRequest) {.start = start, .elapsed = end - start, .offset = offset, .origin = origin,
                         .requested = requested, .actual = actual, .codeId = trace->codeId,
                         .phase = trace->phaseCount - 1, .kind = kind};
}

size_t LoadTrace_read(void* destination, size_t size, size_t count, FILE* file) {
    if (trace == NULL) return fread(destination, size, count, file);
    long offset = ftell(file);
    uint64_t start = trace->clock();
    size_t result = fread(destination, size, count, file);
    uint64_t end = trace->clock();
    TracePhase* p = &trace->phases[trace->phaseCount - 1];
    p->reads++;
    p->bytes += size * result;
    p->readTicks += end - start;
    request('r', start, end, offset, SEEK_SET, size * count, size * result);
    return result;
}

int LoadTrace_seek(FILE* file, long offset, int origin) {
    if (trace == NULL) return fseek(file, offset, origin);
    uint64_t start = trace->clock();
    int result = fseek(file, offset, origin);
    uint64_t end = trace->clock();
    TracePhase* p = &trace->phases[trace->phaseCount - 1];
    p->seeks++;
    p->seekTicks += end - start;
    request('s', start, end, offset, origin, 0, result == 0 ? 1 : 0);
    return result;
}

uint64_t LoadTrace_scriptBegin(int32_t codeId) {
    if (trace == NULL) return 0;
    trace->codeId = codeId;
    return trace->clock();
}

void LoadTrace_scriptEnd(uint64_t start) {
    if (trace == NULL) return;
    request('g', start, trace->clock(), 0, 0, 0, 0);
    trace->codeId = -1;
}

void LoadTrace_end(void (*flush)(void)) {
    if (trace == NULL) return;
    Trace* finished = trace;
    TracePhase* last = &trace->phases[trace->phaseCount - 1];
    last->elapsed = trace->clock() - last->start;
    trace = NULL; // Printing cannot become part of a measured request or phase.
    double ms = 1000.0 / finished->ticksPerSecond;
    logInfo("Load trace: %s begin (%u phases, %u requests, %u dropped; stdio calls, not physical FS requests)\n",
            finished->scope, finished->phaseCount, finished->requestCount, finished->dropped);
    for (unsigned i = 0; finished->phaseCount > i; i++) {
        TracePhase* p = &finished->phases[i];
        logInfo("Load phase: %s %u %s %.3f ms; read %u/%llu bytes/%.3f ms; seek %u/%.3f ms\n",
                finished->scope, i, p->name, p->elapsed * ms, p->reads, (unsigned long long) p->bytes,
                p->readTicks * ms, p->seeks, p->seekTicks * ms);
    }
    for (unsigned i = 0; finished->requestCount > i; i++) {
        TraceRequest* r = &finished->requests[i];
        logInfo("Load request: %s %u %c phase=%u code=%ld offset=%ld origin=%d want=%zu got=%zu start=%.3f ms took=%.3f ms\n",
                finished->scope, i, r->kind, r->phase, (long) r->codeId, r->offset, r->origin, r->requested, r->actual,
                (r->start - finished->phases[0].start) * ms, r->elapsed * ms);
        if (flush != NULL && i % 32 == 31) flush();
    }
    logInfo("Load trace: %s end\n", finished->scope);
    if (flush != NULL) flush();
    free(finished);
}
