#ifndef CTR_INPUTLATE_KEYS_H
#define CTR_INPUTLATE_KEYS_H
#include "ctr_input_map.h"
#ifdef __3DS__
#define INPUT_INLINE __attribute__((section(".text.late.inputlate")))
#else
#define INPUT_INLINE
#endif

// Same mapping as CtrInputMap_keys, using immediates rather than new copies of
// its static pad/arrow tables in the almost-full late rodata reserve.
static inline INPUT_INLINE uint32_t CtrInputLate_keys(const CtrInputMap* map, uint32_t held) {
    uint32_t keys = 0;
    for (int32_t row = 0; MAP_ROW_DPAD > row; row++) {
        uint8_t key = map->keys[row];
        if (key != GAME_KEY_NONE && GAME_KEY_ARROWS > key && (held & CtrInputMap_rowButton((CtrMapRow) row))) keys |= 1u << key;
    }
    for (int32_t pad = 0; 3 > pad; pad++) {
        if (map->keys[MAP_ROW_DPAD + pad] != GAME_KEY_ARROWS) continue;
        uint32_t directions = (held >> (pad == 0 ? 4 : pad == 1 ? 28 : 24)) & 15u;
        if (directions & 1u) keys |= 1u << GAME_KEY_RIGHT;
        if (directions & 2u) keys |= 1u << GAME_KEY_LEFT;
        if (directions & 4u) keys |= 1u << GAME_KEY_UP;
        if (directions & 8u) keys |= 1u << GAME_KEY_DOWN;
    }
    return keys;
}
#endif
