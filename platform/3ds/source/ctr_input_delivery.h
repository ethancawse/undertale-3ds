#ifndef CTR_INPUT_DELIVERY_H
#define CTR_INPUT_DELIVERY_H
#include "ctr_input_latch.h"
// ===[ Input ]===


// Edges are already mapped and unioned by key, including the frame-polled
// sources. Delivery order preserves PC's independent press/release flags.
static inline INPUT_INLINE void sendInput(Runner* runner, CtrInputEdges input, u32* keysMask) {
    u32 previous = *keysMask;
    *keysMask = input.held;
    for (int32_t k = GAME_KEY_NONE + 1; GAME_KEY_ARROWS > k; k++) {
        u32 bit = 1u << k;
        bool held = (input.held & bit) != 0;
        bool wasHeld = (previous & bit) != 0;
        bool press = (input.pressed & bit) != 0 || (held && !wasHeld);
        bool release = (input.released & bit) != 0 || (!held && wasHeld);
        int32_t code = CtrInputMap_keyCode((CtrGameKey) k);
        if (held && release) RunnerKeyboard_onKeyUp(runner->keyboard, code);
        if (press) RunnerKeyboard_onKeyDown(runner->keyboard, code);
        if (!held && release) RunnerKeyboard_onKeyUp(runner->keyboard, code);
    }
}

// ===[ Status output ]===
#endif
