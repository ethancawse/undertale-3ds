#ifndef _BS_PROFILER_H_
#define _BS_PROFILER_H_

#include <common.h>
#include <stdint.h>
#include "stdio_compat.h"

// GML script profiler.
// Tracks self-time (exclusive of nested script calls) and self-instruction-count per code name.

#define PROFILER_MAX_DEPTH 256

// What the time inside profiled calls went to, for the report's last line: each stretch of time between two of the
// profiler's clock reads counts for what was running then.
typedef enum {
    PROFILER_KIND_NONE,        // outside every profiled call (not counted)
    PROFILER_KIND_COMPILED,    // GML run as C: generated (AOT) code and native versions of scripts and events
    PROFILER_KIND_INTERPRETED, // GML run by the interpreter (executeLoop)
    PROFILER_KIND_BUILTIN,     // builtin functions
    PROFILER_KIND_RUNNER,      // the runner's own work (its zones)
    PROFILER_KIND_COUNT
} ProfilerKind;

typedef struct {
    uint64_t ticks; // accumulated self-time in profiler clock ticks (see profiler.c)
    uint64_t ops;   // accumulated self-count of VM instructions executed
    uint64_t calls; // times it was entered (for a builtin, which runs no VM instructions, the only count there is)
} ProfilerStats;

typedef struct {
    const char* key;
    ProfilerStats value;
} ProfilerEntry;

// Where a name pointer's line is in entries, so most exits skip hashing the name's text.
typedef struct {
    const char* key;
    ptrdiff_t value;
} ProfilerNameSlot;

typedef struct {
    uint64_t startTicks;
    uint64_t childTicks;
    uint64_t startOps;
    uint64_t childOps;
    const char* name;
    ProfilerKind outerKind; // what ran before this call, to carry on with after it
} ProfilerFrame;

typedef struct Profiler {
    ProfilerEntry* entries;       // stb_ds sh-map, by name text
    ProfilerNameSlot* nameSlots;  // stb_ds hm-map, by name pointer
    ProfilerFrame frameStack[PROFILER_MAX_DEPTH];
    int frameDepth;
    uint64_t instructionCount;
    uint64_t readCost; // ticks one read of the profiler clock takes, taken off each call
    ProfilerKind kind; // what is running now
    uint64_t kindSince; // when it started to
    uint64_t kindTicks[PROFILER_KIND_COUNT];
    uint64_t calls;    // profiled calls since the last reset
    uint64_t callCost; // ticks a profiled call takes besides its two clock reads (measured at creation)
} Profiler;

// Allocates and initializes a Profiler. Caller owns the returned pointer.
Profiler* Profiler_create(void);

// Frees the Profiler.
void Profiler_destroy(Profiler* p);

// Creates a Profiler at *slot when enabling, destroys it when disabling. Safe to call repeatedly.
void Profiler_setEnabled(Profiler** slot, bool enabled);

// A call of code or a script: compiled GML until the interpreter takes it (Profiler_setKind).
void Profiler_enter(Profiler* p, const char* name);
// A call of another kind (a builtin, a runner zone).
void Profiler_enterKind(Profiler* p, const char* name, ProfilerKind kind);
void Profiler_exit(Profiler* p);
// Says what runs from now on within the current call; returns what ran before, to set back afterwards.
ProfilerKind Profiler_setKind(Profiler* p, ProfilerKind kind);
// Allocates and returns a compact profiler summary (header + top N lines + footer). Caller owns the returned buffer and must free() it.
// Returns nullptr if there's nothing to report.
char* Profiler_createReport(const Profiler* p, int topN, int framesInWindow);
// Clears all accumulated per-script timings.
void Profiler_reset(Profiler* p);

// Record a single VM instruction.
static inline void Profiler_tickInstruction(Profiler* p) {
    if (p != nullptr) p->instructionCount++;
}

#endif /* _BS_PROFILER_H_ */
