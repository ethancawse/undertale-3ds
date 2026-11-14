#ifndef CTR_INPUT_STALLTAP_H
#define CTR_INPUT_STALLTAP_H

#include "ctr_input_latch.h"

// Reuses ctr_input.c's existing 12 B lastTaken record. Only main accesses it.
// The high pending bit marks a preceding live frame; mapped keys use bits 1..10.
typedef struct { uint32_t held, frameMs, pending; } CtrInputStallTap;

#ifdef __3DS__
// Keep the delivery record indirect: a global-specialized copy would access
// lastTaken before the raw latch and change GCC's BSS object ordering.
#define STALLTAP_CODE static __attribute__((noipa, unused, section(".text.late.handover.h1")))
#else
#define STALLTAP_CODE static inline
#endif

STALLTAP_CODE CtrInputEdges CtrInputStallTap_frame(CtrInputStallTap* state,
        CtrInputEdges input, uint32_t nowMs, bool live) {
    const uint32_t armed = 1u << 31;
    uint32_t tap = 0;
    if (live) {
        if ((state->pending & armed) && (uint32_t) (nowMs - state->frameMs) > 50)
            tap = input.pressed & input.released & ~input.held & ~state->held;
        // A completed stall tap is down for exactly this game frame. Keep
        // the sampler's raw state untouched; release it on the next frame.
        input.released |= state->pending & ~armed;
        input.held |= tap;
        input.released &= ~tap;
    }
    *state = (CtrInputStallTap) {input.held, nowMs, live ? armed | tap : 0};
    return input;
}

#undef STALLTAP_CODE
#endif
