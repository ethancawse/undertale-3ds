#ifndef CTR_INPUT_H
#define CTR_INPUT_H

#include "ctr_input_latch.h"
#include "ctr_input_stalltap.h"

typedef struct {
    uint32_t drains, samples, overruns, retries, handoffMisses;
    uint32_t mainTakeovers, workerReturns, retryMisses, mainDrains;
    uint64_t busyTicks, maxGapTicks, elapsedTicks;
    int core;
} CtrInputStats;

// Call after audio initialization, so its chosen core-1 time cap is established.
bool CtrInput_start(bool isNew3ds, const CtrInputMap* map);
void CtrInput_stop(void);
void CtrInput_getStats(CtrInputStats* stats);
// Safe in lab recovery: just an atomic store, with no allocation or waiting.
void CtrInput_requestStop(void);
// After argcrash's lab veto or labguard's lab forward: arming is not an exit.
void CtrInput_stopForTarget(const char* path, const char* const* args, int count);
// The handoff contains only bounded mapping work (at most eight pad samples),
// with no SVC, HID read, log or engine call while the lock is held.
CtrInputEdges CtrInput_take(const CtrInputMap* map, uint32_t held, uint32_t down,
                          bool enabled, bool suppressHeld, CtrInputStats* stats);

#endif
