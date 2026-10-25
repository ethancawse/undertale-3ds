// Which PC key each 3DS button sends to the game (the Controls page of the options menu, ctr_menu.c, saved in
// settings.ini), and the hold-back that keeps L from the game while it is part of the L+SELECT screenshot chord.
// SELECT is never mapped: it opens the options menu, quits with START and takes screenshots with L
// (ctr_input_combo.h). Free of 3ds.h so host tests can include it.
#ifndef CTR_INPUT_MAP_H
#define CTR_INPUT_MAP_H

#include <stdbool.h>
#include <stdint.h>

#include "ctr_input_combo.h"

// The keys a button can send. Within a frame the game gets key changes in this order, which for the default mapping
// is the order the fixed table before this one sent them in.
typedef enum {
    GAME_KEY_NONE,
    GAME_KEY_Z,     // confirm
    GAME_KEY_X,     // cancel (held: run)
    GAME_KEY_C,     // menu
    GAME_KEY_ENTER, // confirm
    GAME_KEY_UP,
    GAME_KEY_DOWN,
    GAME_KEY_LEFT,
    GAME_KEY_RIGHT,
    GAME_KEY_SHIFT, // cancel, like X
    GAME_KEY_CTRL,  // menu, like C
    GAME_KEY_ARROWS, // the directional rows only: each direction sends its arrow
    GAME_KEY_COUNT
} CtrGameKey;

typedef enum {
    MAP_ROW_A,
    MAP_ROW_B,
    MAP_ROW_X,
    MAP_ROW_Y,
    MAP_ROW_L,
    MAP_ROW_R,
    MAP_ROW_ZL, // New 3DS (and the Circle Pad Pro)
    MAP_ROW_ZR,
    MAP_ROW_START,
    MAP_ROW_DPAD, // the directional rows: GAME_KEY_ARROWS or GAME_KEY_NONE
    MAP_ROW_CPAD,
    MAP_ROW_CSTICK, // New 3DS
    MAP_ROW_COUNT
} CtrMapRow;

typedef struct {
    uint8_t keys[MAP_ROW_COUNT]; // CtrGameKey
} CtrInputMap;

// libctru key bits (3ds/services/hid.h), kept independent of 3ds.h like ctr_input_combo.h's.
enum {
    CTR_MAP_A = 1u << 0,
    CTR_MAP_B = 1u << 1,
    CTR_MAP_START = 1u << 3,
    CTR_MAP_DRIGHT = 1u << 4,
    CTR_MAP_R = 1u << 8,
    CTR_MAP_L = 1u << 9,
    CTR_MAP_X = 1u << 10,
    CTR_MAP_Y = 1u << 11,
    CTR_MAP_ZL = 1u << 14,
    CTR_MAP_ZR = 1u << 15,
    CTR_MAP_CSTICK_RIGHT = 1u << 24,
    CTR_MAP_CPAD_RIGHT = 1u << 28,
};

static inline bool CtrInputMap_isDirectional(CtrMapRow row) { return row >= MAP_ROW_DPAD; }

static inline void CtrInputMap_defaults(CtrInputMap* map) {
    for (int32_t i = 0; MAP_ROW_COUNT > i; i++) map->keys[i] = GAME_KEY_NONE;
    map->keys[MAP_ROW_A] = GAME_KEY_Z;
    map->keys[MAP_ROW_B] = GAME_KEY_X;
    map->keys[MAP_ROW_X] = GAME_KEY_C;
    map->keys[MAP_ROW_Y] = GAME_KEY_C;
    map->keys[MAP_ROW_START] = GAME_KEY_ENTER;
    map->keys[MAP_ROW_DPAD] = GAME_KEY_ARROWS;
    map->keys[MAP_ROW_CPAD] = GAME_KEY_ARROWS;
}

static inline bool CtrInputMap_equal(const CtrInputMap* a, const CtrInputMap* b) {
    for (int32_t i = 0; MAP_ROW_COUNT > i; i++) {
        if (a->keys[i] != b->keys[i]) return false;
    }
    return true;
}

static inline bool CtrInputMap_isDefault(const CtrInputMap* map) {
    CtrInputMap d;
    CtrInputMap_defaults(&d);
    return CtrInputMap_equal(map, &d);
}

// The Japanese layout: B confirms and A cancels (the confirm_with_b setting before the Controls page).
static inline void CtrInputMap_swapAB(CtrInputMap* map) {
    uint8_t a = map->keys[MAP_ROW_A];
    map->keys[MAP_ROW_A] = map->keys[MAP_ROW_B];
    map->keys[MAP_ROW_B] = a;
}

// Whether a row may hold key (what settings.ini may set it to).
static inline bool CtrInputMap_allowed(CtrMapRow row, int32_t key) {
    if ((uint32_t) row >= MAP_ROW_COUNT) return false;
    if (CtrInputMap_isDirectional(row)) return key == GAME_KEY_NONE || key == GAME_KEY_ARROWS;
    return 0 <= key && GAME_KEY_ARROWS > key;
}

// The row's next (dir 1) or previous (dir -1) choice.
static inline uint8_t CtrInputMap_step(CtrMapRow row, uint8_t key, int32_t dir) {
    if (CtrInputMap_isDirectional(row)) return key == GAME_KEY_ARROWS ? GAME_KEY_NONE : GAME_KEY_ARROWS;
    return (uint8_t) (((int32_t) key + dir + GAME_KEY_ARROWS) % GAME_KEY_ARROWS);
}

static inline bool CtrInputMap_sendsFromL(const CtrInputMap* map) { return map->keys[MAP_ROW_L] != GAME_KEY_NONE; }

// The button bit of a non-directional row.
static inline uint32_t CtrInputMap_rowButton(CtrMapRow row) {
    switch (row) {
        case MAP_ROW_A: return CTR_MAP_A;
        case MAP_ROW_B: return CTR_MAP_B;
        case MAP_ROW_X: return CTR_MAP_X;
        case MAP_ROW_Y: return CTR_MAP_Y;
        case MAP_ROW_L: return CTR_MAP_L;
        case MAP_ROW_R: return CTR_MAP_R;
        case MAP_ROW_ZL: return CTR_MAP_ZL;
        case MAP_ROW_ZR: return CTR_MAP_ZR;
        case MAP_ROW_START: return CTR_MAP_START;
        default: return 0;
    }
}

// The game keys held (bit k for CtrGameKey k) while the buttons in held are.
static inline uint32_t CtrInputMap_keys(const CtrInputMap* map, uint32_t held) {
    uint32_t keys = 0;
    for (int32_t row = 0; MAP_ROW_DPAD > row; row++) {
        uint8_t key = map->keys[row];
        if (key != GAME_KEY_NONE && GAME_KEY_ARROWS > key && (held & CtrInputMap_rowButton((CtrMapRow) row)) != 0) keys |= 1u << key;
    }
    // A pad's four direction bits are right, left, up, down from its lowest.
    static const uint32_t padLowest[3] = { CTR_MAP_DRIGHT, CTR_MAP_CPAD_RIGHT, CTR_MAP_CSTICK_RIGHT };
    static const uint8_t arrows[4] = { GAME_KEY_RIGHT, GAME_KEY_LEFT, GAME_KEY_UP, GAME_KEY_DOWN };
    for (int32_t pad = 0; 3 > pad; pad++) {
        if (map->keys[MAP_ROW_DPAD + pad] != GAME_KEY_ARROWS) continue;
        for (int32_t d = 0; 4 > d; d++) {
            if (held & (padLowest[pad] << d)) keys |= 1u << arrows[d];
        }
    }
    return keys;
}

// The virtual key code (runner_keyboard.h) the game gets for key.
static inline int32_t CtrInputMap_keyCode(CtrGameKey key) {
    switch (key) {
        case GAME_KEY_Z: return 'Z';
        case GAME_KEY_X: return 'X';
        case GAME_KEY_C: return 'C';
        case GAME_KEY_ENTER: return 13;  // VK_ENTER
        case GAME_KEY_UP: return 38;     // VK_UP
        case GAME_KEY_DOWN: return 40;   // VK_DOWN
        case GAME_KEY_LEFT: return 37;   // VK_LEFT
        case GAME_KEY_RIGHT: return 39;  // VK_RIGHT
        case GAME_KEY_SHIFT: return 16;  // VK_SHIFT
        case GAME_KEY_CTRL: return 17;   // VK_CONTROL
        default: return 0;
    }
}

static inline const char* CtrInputMap_keyName(CtrGameKey key) {
    switch (key) {
        case GAME_KEY_NONE: return "Nothing";
        case GAME_KEY_Z: return "Z (confirm)";
        case GAME_KEY_X: return "X (cancel)";
        case GAME_KEY_C: return "C (menu)";
        case GAME_KEY_ENTER: return "Enter";
        case GAME_KEY_UP: return "Up";
        case GAME_KEY_DOWN: return "Down";
        case GAME_KEY_LEFT: return "Left";
        case GAME_KEY_RIGHT: return "Right";
        case GAME_KEY_SHIFT: return "Shift (cancel)";
        case GAME_KEY_CTRL: return "Ctrl (menu)";
        case GAME_KEY_ARROWS: return "Arrows";
        default: return "?";
    }
}

static inline const char* CtrInputMap_rowName(CtrMapRow row) {
    static const char* const names[MAP_ROW_COUNT] = { "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "START", "D-pad", "Circle Pad", "C-stick" };
    return (uint32_t) row < MAP_ROW_COUNT ? names[row] : "?";
}

// settings.ini's key for a row ("map_a = 1").
static inline const char* CtrInputMap_iniKey(CtrMapRow row) {
    static const char* const keys[MAP_ROW_COUNT] = { "map_a", "map_b", "map_x", "map_y", "map_l", "map_r", "map_zl", "map_zr", "map_start", "map_dpad", "map_circle_pad", "map_c_stick" };
    return (uint32_t) row < MAP_ROW_COUNT ? keys[row] : "";
}

// L as a game button. Pressed together with SELECT (screenshot), or with SELECT already held, L's key must not reach
// the game, so a new L press waits one frame for SELECT before it is passed on. Once L has been part of the chord it
// stays held back until released. L pressed more than a frame before SELECT has reached the game by then; it is
// released when SELECT joins it. A press shorter than the frame it waited is still passed on for one frame.
typedef struct {
    bool waiting; // L went down last frame, alone
    bool chord;   // L has been held with SELECT since it went down
} CtrChordGuard;

// The buttons the game may see this frame. Changes nothing unless sendsFromL.
static inline uint32_t CtrChordGuard_filter(CtrChordGuard* guard, uint32_t held, uint32_t down, bool sendsFromL) {
    const uint32_t l = CTR_COMBO_L, select = CTR_COMBO_SELECT;
    if (!sendsFromL) {
        guard->waiting = guard->chord = false;
        return held;
    }
    if (!(held & l)) {
        bool tap = guard->waiting && !guard->chord && !(held & select);
        guard->waiting = guard->chord = false;
        return tap ? held | l : held;
    }
    if (held & select) guard->chord = true;
    if (guard->chord) {
        guard->waiting = false;
        return held & ~l;
    }
    if (down & l) {
        guard->waiting = true;
        return held & ~l;
    }
    guard->waiting = false;
    return held;
}

#endif
