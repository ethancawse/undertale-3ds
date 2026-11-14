// Nonbattle stylus navigation. No game writes: each action is one ordinary game-key pulse.
// Geometry and arrow successors come from the live menu reader. Pen semantics/edges are shared with battletouch.
#ifndef CTR_TOUCHUI_CORE_H
#define CTR_TOUCHUI_CORE_H
#include "ctr_touch_core.h"

#ifndef TU_INLINE
#define TU_INLINE
#endif
#ifndef TU_STEP
#define TU_STEP static inline TU_INLINE
#endif
#ifndef TU_ITEMS
#define TU_ITEMS 64
#endif
enum { TU_OFF, TU_TEXT, TU_CHOICE };
enum { TU_IDLE, TU_DEAD, TU_PRESS, TU_CONFIRM, TU_RETURN };

typedef struct {
    uint32_t id;
    uint8_t ctx, count;
    int8_t cursor;
    uint8_t textKey; // Key at pen-down: X while typing, Z when complete; 0 outside the bottom panel.
    TouchRect box;
    TouchRect item[TU_ITEMS];
    // -1: no move; directions are Up, Down, Left, Right, using the game's own arrow rules.
    int8_t next[TU_ITEMS][4];
} TuMenu;

typedef struct {
    uint32_t id;
    uint8_t mode, ctx;
    int8_t target, saved, expected;
    bool pen, lit, rest, check;
} TuState;

static inline TU_INLINE void Tu_reset(TuState* s, bool pen) {
    *s = (TuState) {0};
    s->pen = pen;
    s->mode = pen ? TU_DEAD : TU_IDLE;
}

// Shortest legal sequence through the reader's graph; bounded even for a malformed/wrapping graph.
// Returns the first direction, with its expected cursor in *next. No reachable goal: -1.
static inline TU_INLINE int Tu_direction(const TuMenu* m, int goal, int* next) {
    if (m->count > TU_ITEMS || m->cursor < 0 || m->cursor >= m->count || goal < 0 || goal >= m->count) return -1;
    uint8_t queue[TU_ITEMS], first[TU_ITEMS];
    for (unsigned k = 0; k < TU_ITEMS; ++k) first[k] = 255;
    unsigned head = 0, tail = 0;
    queue[tail++] = (uint8_t) m->cursor;
    first[(unsigned) m->cursor] = 254;
    while (head < tail) {
        int from = queue[head++];
        for (unsigned d = 0; d < 4; ++d) {
            int to = m->next[from][d];
            if (to < 0 || to >= m->count || first[to] != 255) continue;
            first[to] = from == m->cursor ? (uint8_t) d : first[from];
            if (to == goal) {
                *next = m->next[(unsigned) m->cursor][first[to]];
                return first[to];
            }
            queue[tail++] = (uint8_t) to;
        }
    }
    return -1;
}

// Return one key held for this sample. Touch_edges/Touch_merge in the shared caller release it next sample.
// A short tap on a distant choice finishes its arrow sequence, then confirms; never confirms another item.
TU_STEP uint32_t Tu_step(TuState* s, const TuMenu* m, bool pen, int x, int y,
                                       uint32_t pressed, uint32_t held, bool reset) {
    bool down = pen && !s->pen, up = !pen && s->pen;
    s->pen = pen;
    if (reset || m->ctx == TU_OFF || ((pressed | held) & TOUCH_KEY_ABANDON) != 0
        || (m->ctx == TU_CHOICE && (m->count > TU_ITEMS || m->cursor < 0 || m->cursor >= m->count))
        || (s->mode >= TU_PRESS && (s->id != m->id || (s->ctx != m->ctx && !(s->ctx == TU_TEXT && m->ctx == TU_CHOICE))
            || (s->ctx == TU_CHOICE && (s->target < 0 || s->target >= m->count || s->saved >= m->count))))) {
        Tu_reset(s, pen);
        return 0;
    }
    if (s->mode == TU_DEAD) {
        if (!pen) s->mode = TU_IDLE;
        return 0;
    }
    if (s->check) {
        s->check = false;
        if (m->cursor != s->expected) {
            Tu_reset(s, pen); // an ignored arrow or an unexpected game move: abandon, never send a late Confirm
            return 0;
        }
    }
    if (down) {
        int item = -1, ctx = m->ctx;
        if (m->ctx == TU_TEXT) {
            // Text acts on press, as the ordinary keys do. Keep this stroke
            // dead until lift, including if X finishes the line immediately.
            Tu_reset(s, true);
            return m->textKey;
        } else if (m->ctx == TU_CHOICE && m->count <= TU_ITEMS) {
            for (unsigned k = 0; k < m->count; ++k) {
                if (Touch_inside(m->item[k], x, y, 0)) { item = (int) k; break; }
            }
            if (item < 0) {
                Tu_reset(s, true); // A choice background never commits an answer.
                return 0;
            }
        } else { Tu_reset(s, true); return 0; }
        *s = (TuState) {.id = m->id, .mode = TU_PRESS, .ctx = (uint8_t) ctx, .target = (int8_t) item,
                        .saved = m->cursor, .pen = true, .lit = true};
    }
    if (s->mode == TU_IDLE) return 0;
    if (s->mode == TU_PRESS) {
        // On lift x/y are the retained last position, as in battletouch's live sampler.
        s->lit = s->ctx == TU_TEXT ? m->textKey != 0
            : Touch_inside(m->item[(unsigned) s->target], x, y, s->lit ? TOUCH_GROW : 0);
        if (up) s->mode = s->lit ? TU_CONFIRM : TU_RETURN;
    }
    int goal = s->mode == TU_RETURN ? s->saved : s->target;
    if (s->ctx == TU_TEXT || m->cursor == goal) {
        bool confirm = s->mode == TU_CONFIRM;
        if (s->mode == TU_CONFIRM || s->mode == TU_RETURN) s->mode = TU_IDLE;
        s->rest = false;
        return confirm ? (s->ctx == TU_TEXT ? m->textKey : TOUCH_KEY_Z) : 0;
    }
    if (s->rest) { s->rest = false; return 0; }
    int next = -1;
    int direction = Tu_direction(m, goal, &next);
    if (direction < 0) { Tu_reset(s, pen); return 0; }
    s->expected = (int8_t) next;
    s->check = true;
    s->rest = true;
    return TOUCH_KEY_UP << direction;
}
#endif
