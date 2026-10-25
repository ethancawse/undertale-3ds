#include "profiler.h"

#include <stdlib.h>
#include "string_compat.h"
#include "stdio_compat.h"

#include "utils.h"
#include "stb_ds.h"
#include "string_builder.h"
#include "gettime.h"

#ifdef __3DS__
#include <3ds/svc.h>
#endif

// The profiler's clock. Enter and exit read it around every profiled call, builtins included, so on the 3DS it is the
// raw system tick (an SVC), not nowNanos's clock_gettime with its 64-bit division, which cost a few microseconds a
// call there and swelled the times it was measuring. Ticks become milliseconds only in the report.
#ifdef __3DS__
#define PROFILER_TICKS_PER_SECOND 268111856.0
static inline uint64_t profilerTicks(void) {
    return svcGetSystemTick();
}
#else
#define PROFILER_TICKS_PER_SECOND 1000000000.0
static inline uint64_t profilerTicks(void) {
    return nowNanos();
}
#endif

Profiler* Profiler_create(void) {
    Profiler* p = (Profiler *)safeMalloc(sizeof(Profiler));
    p->entries = nullptr;
    p->nameSlots = nullptr;
    p->frameDepth = 0;
    p->instructionCount = 0;
    // What one clock read costs (1.3 us on an emulated Old 3DS), so each call's own reads come off the times.
    uint64_t t0 = profilerTicks();
    repeat(100, i) (void) profilerTicks();
    p->readCost = (profilerTicks() - t0) / 101;
    p->kind = PROFILER_KIND_NONE;
    p->kindSince = profilerTicks();
    memset(p->kindTicks, 0, sizeof(p->kindTicks));
    p->calls = 0;
    p->callCost = 0;
    // What a profiled call adds besides its clock reads (its bookkeeping), for the report to say how much of the
    // times that is.
    t0 = profilerTicks();
    repeat(64, i) {
        Profiler_enterKind(p, "<profiler calibration>", PROFILER_KIND_NONE);
        Profiler_exit(p);
    }
    uint64_t pairCost = (profilerTicks() - t0) / 64;
    p->callCost = pairCost > 2 * p->readCost ? pairCost - 2 * p->readCost : 0;
    Profiler_reset(p);
    return p;
}

// The time since the last change of kind counts for the kind running then, less about one clock read (the end of the
// read that began it and the start of the one that ends it).
static inline void chargeKind(Profiler* p, uint64_t now) {
    uint64_t span = now - p->kindSince;
    p->kindTicks[p->kind] += span > p->readCost ? span - p->readCost : 0;
    p->kindSince = now;
}

ProfilerKind Profiler_setKind(Profiler* p, ProfilerKind kind) {
    if (p == nullptr) return PROFILER_KIND_NONE;
    ProfilerKind outer = p->kind;
    if (outer == kind) return outer;
    chargeKind(p, profilerTicks());
    p->kind = kind;
    return outer;
}

void Profiler_destroy(Profiler* p) {
    if (p == nullptr) return;
    shfree(p->entries);
    hmfree(p->nameSlots);
    free(p);
}

void Profiler_setEnabled(Profiler** slot, bool enabled) {
    if (enabled) {
        if (*slot == nullptr) *slot = Profiler_create();
    } else {
        if (*slot != nullptr) {
            Profiler_destroy(*slot);
            *slot = nullptr;
        }
    }
}

void Profiler_enterKind(Profiler* p, const char* name, ProfilerKind kind) {
    if (p == nullptr) return;
    if (p->frameDepth >= PROFILER_MAX_DEPTH) return;
    ProfilerFrame* f = &p->frameStack[p->frameDepth];
    uint64_t now = profilerTicks();
    chargeKind(p, now);
    p->calls++;
    f->outerKind = p->kind;
    p->kind = kind;
    f->startTicks = now;
    f->childTicks = 0;
    f->startOps = p->instructionCount;
    f->childOps = 0;
    f->name = name != nullptr ? name : "<unknown>";
    p->frameDepth++;
}

void Profiler_enter(Profiler* p, const char* name) {
    Profiler_enterKind(p, name, PROFILER_KIND_COMPILED);
}

void Profiler_exit(Profiler* p) {
    if (p == nullptr) return;
    if (0 >= p->frameDepth) return;
    p->frameDepth--;
    ProfilerFrame* f = &p->frameStack[p->frameDepth];
    uint64_t now = profilerTicks();
    chargeKind(p, now);
    p->kind = f->outerKind;
    uint64_t elapsed = now - f->startTicks;
    // About one clock read falls inside a call's span (the end of the first, the start of the second), and the
    // caller's span holds both.
    uint64_t own = elapsed > p->readCost ? elapsed - p->readCost : 0;
    uint64_t selfTicks = own > f->childTicks ? own - f->childTicks : 0;
    uint64_t totalOps = p->instructionCount - f->startOps;
    uint64_t selfOps = totalOps > f->childOps ? totalOps - f->childOps : 0;

    // Names are code and function names that live as long as the game, so the pointer finds the line; a name met
    // through a new pointer is looked up by its text once.
    ptrdiff_t slot = hmgeti(p->nameSlots, f->name);
    ptrdiff_t i;
    if (0 > slot) {
        i = shgeti(p->entries, f->name);
        if (0 > i) {
            ProfilerStats stats = {0};
            shput(p->entries, f->name, stats);
            i = shgeti(p->entries, f->name);
        }
        hmput(p->nameSlots, f->name, i);
    } else {
        i = p->nameSlots[slot].value;
    }
    p->entries[i].value.ticks += selfTicks;
    p->entries[i].value.ops += selfOps;
    p->entries[i].value.calls++;

    if (p->frameDepth > 0) {
        p->frameStack[p->frameDepth - 1].childTicks += elapsed + p->readCost;
        p->frameStack[p->frameDepth - 1].childOps += totalOps;
    }
}

static int compareEntriesDesc(const void* a, const void* b) {
    uint64_t va = ((const ProfilerEntry*) a)->value.ticks;
    uint64_t vb = ((const ProfilerEntry*) b)->value.ticks;
    if (vb > va) return 1;
    if (va > vb) return -1;
    return 0;
}

// Sort entries into a caller-owned buffer. Returns entry count; 0 if nothing to report.
// Also computes the grand total (across all entries, not just topN) in *outTotal.
static size_t collectSorted(const Profiler* p, ProfilerEntry* outSorted, size_t outCap, ProfilerStats* outTotal) {
    size_t count = shlen(p->entries);
    if (count == 0) return 0;
    if (count > outCap) count = outCap;
    memcpy(outSorted, p->entries, count * sizeof(ProfilerEntry));
    qsort(outSorted, count, sizeof(ProfilerEntry), compareEntriesDesc);

    ProfilerStats total = { 0 };
    size_t fullCount = shlen(p->entries);
    repeat(fullCount, i) {
        total.ticks += p->entries[i].value.ticks;
        total.ops += p->entries[i].value.ops;
    }
    *outTotal = total;
    return count;
}

void Profiler_reset(Profiler* p) {
    if (p == nullptr) return;
    shfree(p->entries);
    p->entries = nullptr;
    hmfree(p->nameSlots);
    p->nameSlots = nullptr;
    chargeKind(p, profilerTicks());
    memset(p->kindTicks, 0, sizeof(p->kindTicks));
    p->calls = 0;
}

char* Profiler_createReport(const Profiler* p, int topN, int framesInWindow) {
    if (p == nullptr) return nullptr;
    size_t count = shlen(p->entries);
    if (count == 0) return nullptr;
    if (0 >= framesInWindow) framesInWindow = 1;

    ProfilerEntry* sorted = (ProfilerEntry*) malloc(count * sizeof(ProfilerEntry));
    if (sorted == nullptr) return nullptr;
    ProfilerStats total = { 0 };
    size_t sortedEntriesCount = collectSorted(p, sorted, count, &total);

    size_t limit = sortedEntriesCount;
    if (topN > 0 && (size_t) topN < limit)
        limit = (size_t) topN;

    StringBuilder stringBuilder = StringBuilder_create(64);

    double frames = (double) framesInWindow;
    const double ticksPerMs = PROFILER_TICKS_PER_SECOND / 1000.0;
    double totalMs = (double)(int64_t)total.ticks / ticksPerMs;
    double totalOpsPerFrame = (double)(int64_t)total.ops / frames;

    // The same time by what ran: GML as C, GML in the interpreter, builtins, and the runner's own work. On the first
    // line, which a platform's log keeps when it cuts a long report short.
    double kindMs[PROFILER_KIND_COUNT];
    double kindsMs = 0;
    repeat(PROFILER_KIND_COUNT, k) {
        kindMs[k] = (double)(int64_t) p->kindTicks[k] / ticksPerMs / frames;
        if (k != PROFILER_KIND_NONE) kindsMs += kindMs[k];
    }
    double share = kindsMs > 0 ? 100.0 / kindsMs : 0;
    StringBuilder_appendFormat(&stringBuilder,
                               "GML Profiler (avg %d frames): compiled GML %.2fms (%.0f%%), interpreted GML %.2fms (%.0f%%), builtins "
                               "%.2fms (%.0f%%), runner %.2fms (%.0f%%) a frame; %.0f profiled calls a frame, whose bookkeeping is "
                               "about %.2fms of that\n",
                               framesInWindow, kindMs[PROFILER_KIND_COMPILED], kindMs[PROFILER_KIND_COMPILED] * share,
                               kindMs[PROFILER_KIND_INTERPRETED], kindMs[PROFILER_KIND_INTERPRETED] * share,
                               kindMs[PROFILER_KIND_BUILTIN], kindMs[PROFILER_KIND_BUILTIN] * share,
                               kindMs[PROFILER_KIND_RUNNER], kindMs[PROFILER_KIND_RUNNER] * share, (double)(int64_t) p->calls / frames,
                               (double)(int64_t) (p->calls * p->callCost) / ticksPerMs / frames);
    repeat(limit, i) {
        double ms = (double)(int64_t)sorted[i].value.ticks / ticksPerMs;
        double perFrameMs = ms / frames;
        double opsPerFrame = (double)(int64_t)sorted[i].value.ops / frames;
        double nsPerOp = sorted[i].value.ops > 0 ? ms * 1000000.0 / (double)(int64_t)sorted[i].value.ops : (double) 0;
        double callsPerFrame = (double)(int64_t)sorted[i].value.calls / frames;
        double usPerCall = sorted[i].value.calls > 0 ? ms * 1000.0 / (double)(int64_t)sorted[i].value.calls : (double) 0;
        // The calls go after the name: tools/aot/gmlaot.py reads the line up to it.
        StringBuilder_appendFormat(&stringBuilder, "%.2fms %.0f ops (%.0f ns/op) %s %.1f calls (%.1f us/call)\n", perFrameMs, opsPerFrame, nsPerOp, sorted[i].key, callsPerFrame, usPerCall);
    }
    StringBuilder_appendFormat(&stringBuilder, "total %.2fms/frame, %.0f ops/frame (%zu scripts)", totalMs / frames, totalOpsPerFrame, sortedEntriesCount);
    char* result = StringBuilder_toString(&stringBuilder);
    StringBuilder_free(&stringBuilder);
    free(sorted);
    return result;
}
