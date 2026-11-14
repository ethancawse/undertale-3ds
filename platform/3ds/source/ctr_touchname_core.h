// Naming uses the same pen state machine with a separate larger graph.
// Include in its isolated translation unit, before any 64-node touch reader.
#ifndef CTR_TOUCHNAME_CORE_H
#define CTR_TOUCHNAME_CORE_H
#ifdef ENABLE_WAD16
#include <stdlib.h>
#ifdef CTR_TOUCHUI_CORE_H
#error Naming core must precede other touch readers in its isolated unit
#endif
#define NN_ITEMS 126
#ifndef NN_CODE
#define NN_CODE static __attribute__((unused, noinline, section(".touchname_text")))
#endif
#define TU_ITEMS NN_ITEMS
#define TuMenu NnMenu
#define TuState NnState
#define Tu_reset Nn_reset
#define Tu_direction Nn_direction
#define Tu_step Nn_step
#define TU_INLINE __attribute__((always_inline))
#define TU_STEP NN_CODE
#include "ctr_touchui_core.h"
#undef TU_STEP
#undef TU_INLINE
#undef Tu_step
#undef Tu_direction
#undef Tu_reset
#undef TuState
#undef TuMenu
#undef TU_ITEMS
typedef struct {
    int rows, cols;
    bool japanese;
    int16_t x[15], y[8], menuX[3], charsetX[3];
    int16_t menuY, charsetY;
    uint16_t filled[8];
} NnGrid;

NN_CODE int Nn_nearest(const NnGrid* g, int x) {
    int best = 0, distance = abs(g->x[0] - x);
    for (int c = 1; c < g->cols; ++c) {
        int d = abs(g->x[c] - x);
        if (d < distance) { best = c; distance = d; }
    }
    return best;
}

NN_CODE int Nn_id(const NnGrid* g, int r, int c) {
    if (c < 0 || c >= g->cols) return -1;
    if (r >= 0 && r < g->rows) return r * g->cols + c;
    if (r == -1 && (g->japanese || c < 3)) return g->rows * g->cols + c;
    if (r == -2 && g->japanese) return (g->rows + 1) * g->cols + c;
    return -1;
}

NN_CODE int Nn_next(const NnGrid* g, int r, int c, unsigned direction) {
    int originalCol = c;
    /* Empty cells repeat the same native pressed-key branch. A malformed
     * cycle is rejected before injecting any key. Right at the final cell
     * exits early, restoring the original column without an empty-cell test. */
    for (unsigned steps = 0; steps < 126; ++steps) {
        if (direction == 3) {
            ++c;
            if (r == -1) { if (c > 2) c = 0; }
            else if (c >= g->cols) {
                if (r == g->rows - 1) return Nn_id(g, r, originalCol);
                c = 0; ++r;
            }
        } else if (direction == 2) {
            if (--c < 0) {
                if (r == 0) c = 0;
                else if (r > 0) { c = g->cols - 1; --r; }
                else c = 2;
            }
        } else if (direction == 1) {
            if (r == -1) {
                r = 0; c = Nn_nearest(g, g->menuX[c == 1 ? 1 : c == 2 ? 2 : 0]);
            } else if (++r >= g->rows) {
                r = g->japanese ? -2 : -1;
                const int16_t* x = g->japanese ? g->charsetX : g->menuX;
                c = g->x[c] >= x[2] - 10 ? 2 : g->x[c] >= x[1] - 10 ? 1 : 0;
            }
        } else if (direction == 0) {
            if (r == -2 || (r == -1 && !g->japanese)) {
                const int16_t* x = r == -2 ? g->charsetX : g->menuX;
                r = g->rows - 1;
                c = c > 0 ? Nn_nearest(g, x[c == 2 ? 2 : 1]) : 0;
            } else if (--r == -1) {
                c = g->x[c] >= g->menuX[2] - 10 ? 2 : g->x[c] >= g->menuX[1] - 10 ? 1 : 0;
            }
        } else return -1;
        int id = Nn_id(g, r, c);
        if (id < 0) return -1;
        if (r < 0 || (g->filled[r] & (1u << c))) return id;
    }
    return -1;
}

NN_CODE bool Nn_graph(NnMenu* menu, const NnGrid* g, uint32_t owner, int row, int col) {
    if (!owner || g->rows < 1 || g->rows > 8 || g->cols < 3 || g->cols > 15) return false;
    int count = g->rows * g->cols + (g->japanese ? 2 * g->cols : 3);
    int cursor = Nn_id(g, row, col);
    if (count > NN_ITEMS || cursor < 0) return false;
    for (int c = 0; c < g->cols; ++c)
        if (g->x[c] < 0 || g->x[c] > 304 || (c && g->x[c] - g->x[c-1] < 4)) return false;
    for (int r = 0; r < g->rows; ++r)
        if (g->y[r] < 0 || g->y[r] > 224 || (r && g->y[r] - g->y[r-1] < 4) || g->filled[r] >> g->cols) return false;
    if (g->menuY < 0 || g->menuY > 224 || (g->japanese && (g->charsetY < 0 || g->charsetY > 224))) return false;
    for (int c = 0; c < 3; ++c)
        if (g->menuX[c] < 0 || g->menuX[c] > 296 || (c && g->menuX[c] - g->menuX[c-1] < 8)
            || (g->japanese && (g->charsetX[c] < 0 || g->charsetX[c] > 296
                || (c && g->charsetX[c] - g->charsetX[c-1] < 8)))) return false;
    *menu = (NnMenu) {.id = owner, .ctx = TU_CHOICE, .count = (uint8_t) count, .cursor = (int8_t) cursor};
    for (int i = 0; i < count; ++i) {
        int r = i < g->rows * g->cols ? i / g->cols : i < (g->rows + 1) * g->cols ? -1 : -2;
        int c = i < g->rows * g->cols ? i % g->cols : r == -1 ? i - g->rows * g->cols : i - (g->rows + 1) * g->cols;
        if ((r >= 0 && (g->filled[r] & (1u << c))) || (r < 0 && c < 3)) {
            int x = r >= 0 ? g->x[c] : r == -1 ? g->menuX[c] : g->charsetX[c];
            int y = r >= 0 ? g->y[r] : r == -1 ? g->menuY : g->charsetY;
            int right = r >= 0 ? x + 18 : c < 2 ? (r == -1 ? g->menuX[c+1] : g->charsetX[c+1]) - 4 : 300;
            int bottom = y + 18;
            if (r >= 0 && c + 1 < g->cols && right > g->x[c+1] - 2) right = g->x[c+1] - 2;
            if (r >= 0 && r + 1 < g->rows && bottom > g->y[r+1] - 2) bottom = g->y[r+1] - 2;
            menu->item[i] = (TouchRect) {x - 2, y - 2, right, bottom};
        }
        for (unsigned d = 0; d < 4; ++d) menu->next[i][d] = (int8_t) Nn_next(g, r, c, d);
    }
    return true;
}

#endif
#endif
