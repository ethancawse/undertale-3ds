#ifndef CTR_INPUT_COMBO_H
#define CTR_INPUT_COMBO_H

#include <stdbool.h>
#include <stdint.h>

// libctru key bits, kept independent of 3ds.h for host tests.
enum { CTR_COMBO_SELECT = 1u << 2, CTR_COMBO_START = 1u << 3, CTR_COMBO_L = 1u << 9 };
typedef struct { bool selectAlone; } CtrInputCombo;
typedef struct { bool screenshot, menu, quit; } CtrInputComboAction;

static inline CtrInputComboAction CtrInputCombo_step(CtrInputCombo* state, uint32_t held, uint32_t down, uint32_t up) {
    CtrInputComboAction action = {0};
    const uint32_t select = CTR_COMBO_SELECT, start = CTR_COMBO_START, l = CTR_COMBO_L;
    // Directional input (including the circle pad and C-stick) does not consume SELECT.
    const uint32_t directions = (15u << 4) | (255u << 24);
    const uint32_t partners = ~(directions | select);
    if (down & select) state->selectAlone = true;
    if (((held | down | up) & select) && ((held | down | up) & partners)) state->selectAlone = false;
    action.quit = (held & (select | start)) == (select | start);
    action.screenshot = !action.quit && (held & (select | l)) == (select | l) && (down & (select | l)) != 0;
    action.menu = (up & select) != 0 && state->selectAlone;
    if (up & select) state->selectAlone = false;
    return action;
}

#endif
