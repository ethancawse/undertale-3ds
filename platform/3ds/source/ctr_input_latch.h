#ifndef CTR_INPUT_LATCH_H
#define CTR_INPUT_LATCH_H

#include <stdbool.h>
#include <stdint.h>
#include "ctr_inputlate_keys.h"

// Pure state machine, shared with the host tests. Bits are CtrGameKey, not buttons.
typedef struct { uint32_t held, pressed, released; } CtrInputEdges;

static inline INPUT_INLINE void CtrInputEdges_sample(CtrInputEdges* edges, uint32_t held) {
    edges->pressed |= held & ~edges->held;
    edges->released |= edges->held & ~held;
    edges->held = held;
}

static inline INPUT_INLINE CtrInputEdges CtrInputEdges_take(CtrInputEdges* edges) {
    CtrInputEdges result = *edges;
    edges->pressed = edges->released = 0;
    return result;
}

// These inputs come from the frame's hidScanInput, not the HID pad ring.
#define CTR_INPUT_EXTRA_MASK (CTR_MAP_ZL | CTR_MAP_ZR | (15u << 24))
#define CTR_INPUT_PAD_MASK (3u | CTR_COMBO_SELECT | CTR_MAP_START | (15u << 4) | CTR_MAP_R | CTR_MAP_L | CTR_MAP_X | CTR_MAP_Y | (15u << 28))

typedef struct {
    CtrInputMap map;
    CtrInputEdges edges;
    CtrChordGuard guard;
    uint32_t raw, extras, ignoredKeys;
    bool enabled, lAllowed, lPulse, lCandidate, lTap;
} CtrInputLatch;

// A new owner starts with no queued edges. Keys already held by any alias must
// all be released before live input can arm them again.
static inline INPUT_INLINE void CtrInputLatch_rebase(CtrInputLatch* latch, const CtrInputMap* map,
                                                    uint32_t held) {
    latch->map = *map;
    latch->edges = (CtrInputEdges) {0};
    latch->guard = (CtrChordGuard) {0};
    latch->lAllowed = latch->lPulse = latch->lCandidate = latch->lTap = false;
    latch->ignoredKeys |= CtrInputLate_keys(map, latch->raw | held);
}

static inline INPUT_INLINE void CtrInputLatch_collect(CtrInputLatch* latch) {
    uint32_t raw = latch->raw | latch->extras;
    // Suppression belongs to a KEY: another source holding that key keeps it
    // suppressed until every source releases it, including unlatched sources.
    latch->ignoredKeys &= CtrInputLate_keys(&latch->map, raw);
    if (!latch->lAllowed) raw &= ~CTR_MAP_L;
    if (latch->lPulse) raw |= CTR_MAP_L;
    uint32_t keys = CtrInputLate_keys(&latch->map, raw) & ~latch->ignoredKeys;
    CtrInputEdges_sample(&latch->edges, latch->enabled ? keys : 0);
}

static inline INPUT_INLINE void CtrInputLatch_sample(CtrInputLatch* latch, uint32_t raw) {
    raw &= CTR_INPUT_PAD_MASK;
    bool wasL = (latch->raw & CTR_MAP_L) != 0;
    bool isL = (raw & CTR_MAP_L) != 0;
    if (isL && !wasL) {
        latch->lAllowed = false; // the frame guard must approve a new L press
        latch->lCandidate = true;
    }
    if (isL && (raw & CTR_COMBO_SELECT)) {
        latch->guard.chord = true;
        latch->guard.waiting = false;
        latch->lAllowed = latch->lCandidate = false;
    }
    if (!isL && wasL) {
        if (latch->lCandidate && !latch->lAllowed && !latch->guard.chord) latch->lTap = true;
        latch->lAllowed = latch->lCandidate = false;
    }
    latch->raw = raw;
    CtrInputLatch_collect(latch);
}

// Called once per game frame under the same short handoff lock as sample().
// The mapping copy and both source groups change atomically with respect to the
// sampler. Mapping changes mid-hold release the old key and press the new key;
// a menu close additionally suppresses the newly mapped held keys.
static inline INPUT_INLINE CtrInputEdges CtrInputLatch_frame(CtrInputLatch* latch, const CtrInputMap* map,
        uint32_t held, uint32_t down, bool enabled, uint32_t suppressKeys) {
    if (enabled && !latch->enabled) CtrInputLatch_rebase(latch, map, held);
    if (suppressKeys) CtrInputLatch_rebase(latch, map, held);
    if (!CtrInputMap_equal(&latch->map, map)) {
        latch->map = *map;
        latch->guard = (CtrChordGuard) {0};
        latch->lAllowed = latch->lPulse = latch->lTap = false;
    }
    latch->extras = held & CTR_INPUT_EXTRA_MASK;
    latch->ignoredKeys |= suppressKeys;
    bool waiting = latch->guard.waiting;
    // The ring can be newer than main's hidScanInput. Use one coherent pad
    // state for both sides of the chord, so a stale scanned L cannot start a
    // second waiting pulse after its completed ring tap was already delivered.
    uint32_t chordMask = CTR_MAP_L | CTR_COMBO_SELECT;
    uint32_t chordHeld = (held & ~chordMask) | (latch->raw & chordMask);
    uint32_t chordDown = down & ~CTR_MAP_L;
    if (latch->lCandidate && !waiting && !latch->lAllowed) chordDown |= latch->raw & CTR_MAP_L;
    uint32_t filtered = CtrChordGuard_filter(&latch->guard, chordHeld, chordDown, CtrInputMap_sendsFromL(map));
    latch->lAllowed = (filtered & latch->raw & CTR_MAP_L) != 0;
    // Preserve the trunk guard's one-frame held pulse for an L press observed
    // in one frame and released in the next before its hold-back expired.
    latch->lPulse = (filtered & CTR_MAP_L) && !(latch->raw & CTR_MAP_L);
    latch->enabled = enabled;
    CtrInputLatch_collect(latch);
    if (enabled && latch->lTap && !waiting && !latch->lPulse) {
        // A complete, non-chord L tap hidden between frame scans: final held=0,
        // both edges=1. Union with other sources BEFORE collecting this pulse.
        uint32_t raw = latch->raw | latch->extras;
        uint32_t keys = CtrInputLate_keys(map, raw | CTR_MAP_L) & ~latch->ignoredKeys;
        CtrInputEdges_sample(&latch->edges, keys);
        CtrInputLatch_collect(latch);
    }
    latch->lTap = false;
    if (!enabled) {
        latch->edges = (CtrInputEdges) {0};
        return latch->edges;
    }
    return CtrInputEdges_take(&latch->edges);
}

// HID timestamps belong to index-zero writes (current and previous ring turns),
// NOT to individual slots. An index alone cannot distinguish zero from 8 samples.
typedef struct { uint64_t tick; uint32_t index; bool ready; } CtrPadCursor;
typedef struct { unsigned count; bool overrun; } CtrPadAdvance;
static inline INPUT_INLINE CtrPadAdvance CtrPadCursor_advance(CtrPadCursor* cursor, uint64_t tick,
                                                uint64_t previousTick, uint32_t index) {
    CtrPadAdvance result = {0};
    if (cursor->ready) {
        if (tick == cursor->tick && index >= cursor->index) result.count = index - cursor->index;
        else if (tick > cursor->tick && previousTick == cursor->tick) {
            result.count = 8 + index - cursor->index;
            result.overrun = result.count > 8;
        } else {
            // Multiple turns, timestamp reset, or an inconsistent producer index.
            result.overrun = true;
            result.count = 8;
        }
    }
    if (result.count > 8) result.count = 8;
    cursor->tick = tick;
    cursor->index = index;
    cursor->ready = true;
    return result;
}

#endif
