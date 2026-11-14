// Read-only cursor graphs for the popup families. The shared Tu_step emits
// arrows and Z; the native Draw handler owns all cursor and history writes.
#ifndef CTR_TOUCHMENUS_CORE_H
#define CTR_TOUCHMENUS_CORE_H
#include "ctr_touchui_core.h"

#ifndef TM_INLINE
#define TM_INLINE
#endif

static inline TM_INLINE bool Tm_panel(TuMenu* m, uint32_t owner, int column, int row,
                                      unsigned visible, int x, int y) {
    if (!owner || x < -32768 || x > 32466 || y < -32768 || y > 32695
        || column < 0 || column > 1 || row < 0 || row > 2 || visible & ~63u
        || !(visible & (1u << (row * 2 + column)))) return false;
    *m = (TuMenu) {.id = owner, .ctx = TU_CHOICE, .count = 6,
                    .cursor = (int8_t) (row * 2 + column)};
    for (int i = 0; i < 6; ++i) {
        int col = i % 2, r = i / 2;
        // Partition the native list into its two columns and three text rows.
        // Hidden floors have neither a target nor an incoming arrow edge.
        m->item[i] = (visible & (1u << i))
            ? (TouchRect) {(int16_t) (x + 20 + col * 110), (int16_t) (y + 12 + r * 20),
                           (int16_t) (x + (col ? 301 : 130)), (int16_t) (y + 32 + r * 20)}
            : (TouchRect) {0, 0, 0, 0};
        int to[4] = {r ? i - 2 : -1, r < 2 ? i + 2 : -1,
                     col ? i - 1 : -1, col ? -1 : i + 1};
        for (int d = 0; d < 4; ++d)
            m->next[i][d] = (int8_t) ((visible & (1u << i)) && to[d] >= 0
                && (visible & (1u << to[d])) ? to[d] : -1);
    }
    return true;
}

static inline TM_INLINE bool Tm_storage(TuMenu* m, uint32_t owner, int column,
                                        int inventoryRow, int storageRow, int x, int y) {
    if (!owner || x < -32768 || x > 32458 || y < -32768 || y > 32571
        || column < 0 || column > 1 || inventoryRow < 0 || inventoryRow >= 8
        || storageRow < 0 || storageRow >= 10) return false;
    *m = (TuMenu) {.id = owner, .ctx = TU_CHOICE, .count = 18,
                    .cursor = (int8_t) (column ? 8 + storageRow : inventoryRow)};
    for (int i = 0; i < 18; ++i) {
        int col = i >= 8, row = col ? i - 8 : i;
        int first = col ? 8 : 0, last = col ? 17 : 7;
        m->item[i] = (TouchRect) {(int16_t) (x + (col ? 163 : 11)), (int16_t) (y + 36 + row * 16),
                                  (int16_t) (x + (col ? 309 : 159)), (int16_t) (y + 52 + row * 16)};
        m->next[i][0] = (int8_t) (i > first ? i - 1 : -1);
        m->next[i][1] = (int8_t) (i < last ? i + 1 : -1);
        // Crossing columns copies the source row; inventory clamps rows 8/9.
        m->next[i][2] = (int8_t) (col ? (row < 8 ? row : 7) : -1);
        m->next[i][3] = (int8_t) (col ? -1 : 8 + row);
    }
    return true;
}
#endif
