#ifndef CTR_INPUT_SCHEDULE_H
#define CTR_INPUT_SCHEDULE_H

#include <stdbool.h>
#include <stdint.h>

// Protected by the HID cursor lock. The worker never changes CPU; main drains
// every frame, including the frame detecting a missed 32 ms worker window.
// Background reads pause for 1..32 s; only a current worker snapshot can ack.
typedef struct {
    uint64_t workerMs, nextMs, retryMs;
    uint32_t delayMs, seenOverruns, takeovers, returns, retryMisses;
    bool recovery, mainReads, retrying;
} CtrInputSchedule;

static inline __attribute__((always_inline)) bool CtrInputSchedule_check(
        CtrInputSchedule* s, uint64_t nowMs, uint32_t overruns) {
    if (!s->recovery) return false;
    bool lost = overruns != s->seenOverruns;
    s->seenOverruns = overruns;
    // A frame may sample its clock before waiting for a newer worker ack.
    if (s->mainReads || (!lost && (nowMs < s->workerMs || nowMs - s->workerMs <= 32))) return false;
    s->mainReads = true;
    ++s->takeovers;
    if (s->retrying) ++s->retryMisses;
    if (!s->delayMs) s->delayMs = 1000;
    s->nextMs = nowMs + s->delayMs;
    if (s->delayMs < 32000) s->delayMs *= 2;
    return true;
}

static inline __attribute__((always_inline)) bool CtrInputSchedule_due(
        const CtrInputSchedule* s, uint64_t nowMs) {
    return !s->mainReads || nowMs >= s->nextMs;
}

static inline __attribute__((always_inline)) bool CtrInputSchedule_ack(
        CtrInputSchedule* s, uint64_t nowMs, bool valid) {
    if (!valid || !CtrInputSchedule_due(s, nowMs)) return false;
    bool returned = s->mainReads;
    s->workerMs = nowMs;
    if (returned) {
        s->mainReads = false;
        s->retrying = true;
        s->retryMs = nowMs;
        ++s->returns;
    } else if (s->retrying && nowMs - s->retryMs >= 32000) {
        s->delayMs = 1000;
        s->retrying = false;
    }
    return returned;
}

static inline __attribute__((always_inline)) bool CtrInputSchedule_reject(
        CtrInputSchedule* s, uint64_t nowMs) {
    if (!s->mainReads || !CtrInputSchedule_due(s, nowMs)) return false;
    ++s->retryMisses;
    s->nextMs = nowMs + s->delayMs;
    if (s->delayMs < 32000) s->delayMs *= 2;
    return true;
}
#endif
