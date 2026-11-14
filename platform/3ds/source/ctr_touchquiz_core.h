// Quiz taps: only ordinary held arrows; collision confirmation belongs to the game.
#ifndef TOUCHUI_QUIZ_CORE_H
#define TOUCHUI_QUIZ_CORE_H
#include "ctr_touch_core.h"

#ifndef TQ_INLINE
#define TQ_INLINE
#endif
#ifndef TQ_STEP
#define TQ_STEP static inline TQ_INLINE
#endif

enum { TQ_IDLE, TQ_DEAD, TQ_PRESS, TQ_STEER };
typedef struct {
    uint32_t id;
    TouchRect target;
    int32_t x, y; // centre of the collision sprite, 1/16 room px
} TqAnswer;
typedef struct {
    uint32_t owner, heart;
    int32_t x, y; // live SOUL centre, 1/16 room px
    uint8_t count;
    TqAnswer answer[4];
} TqMenu;
typedef struct {
    uint32_t owner, heart, answer, lastHold;
    int32_t lastX, lastY;
    uint8_t mode;
    bool pen;
} TqState;

static inline TQ_INLINE void Tq_reset(TqState* s, bool pen) {
    *s = (TqState) {.mode = pen ? TQ_DEAD : TQ_IDLE, .pen = pen};
}
static inline TQ_INLINE int Tq_at(const TqMenu* m, int x, int y, uint32_t previous) {
    // Keep jitter tolerance only for the previously highlighted answer.
    for (unsigned i = 0; i < m->count; ++i)
        if (Touch_inside(m->answer[i].target, x, y, 0)) return (int) i;
    for (unsigned i = 0; i < m->count; ++i)
        if (m->answer[i].id == previous && Touch_inside(m->answer[i].target, x, y, TOUCH_GROW)) return (int) i;
    return -1;
}
TQ_STEP uint32_t Tq_step(TqState* s, const TqMenu* m, bool pen, int x, int y,
                               uint32_t physical, bool reset) {
    bool down = pen && !s->pen, up = !pen && s->pen;
    s->pen = pen;
    if (reset || physical || !m->owner || !m->heart || !m->count || m->count > 4
        || (s->mode >= TQ_PRESS && (s->owner != m->owner || s->heart != m->heart))) {
        Tq_reset(s, pen);
        return 0;
    }
    if (s->mode == TQ_DEAD) {
        if (!pen) s->mode = TQ_IDLE;
        return 0;
    }
    if (down) {
        // A new stroke cancels steering immediately. It can arm a fresh release.
        *s = (TqState) {.owner = m->owner, .heart = m->heart, .mode = TQ_PRESS, .pen = true};
    }
    if (s->mode == TQ_IDLE) return 0;
    int target = -1;
    if (s->mode == TQ_PRESS) {
        target = Tq_at(m, x, y, s->answer);
        s->answer = target >= 0 ? m->answer[target].id : 0;
        if (!up) return 0;
        if (target < 0) { Tq_reset(s, false); return 0; }
        s->mode = TQ_STEER;
    } else {
        for (unsigned i = 0; i < m->count; ++i)
            if (m->answer[i].id == s->answer) target = (int) i;
        if (target < 0) { Tq_reset(s, pen); return 0; }
        // Any requested axis that made no progress is blocked or overridden.
        if (((s->lastHold & TOUCH_KEY_RIGHT) && m->x <= s->lastX)
            || ((s->lastHold & TOUCH_KEY_LEFT) && m->x >= s->lastX)
            || ((s->lastHold & TOUCH_KEY_DOWN) && m->y <= s->lastY)
            || ((s->lastHold & TOUCH_KEY_UP) && m->y >= s->lastY)) {
            Tq_reset(s, pen);
            return 0;
        }
    }
    // No speed, position, timer or collision writes: the normal key handlers
    // perform the move. The centre may be a half pixel; keep the same precision.
    const TqAnswer* answer = &m->answer[target];
    uint32_t keys = (m->x < answer->x ? TOUCH_KEY_RIGHT : m->x > answer->x ? TOUCH_KEY_LEFT : 0)
        | (m->y < answer->y ? TOUCH_KEY_DOWN : m->y > answer->y ? TOUCH_KEY_UP : 0);
    // Once at the centre, the collision has had an ordinary frame to run.
    // If it did not confirm, stop cleanly rather than hold a synthetic key.
    if (!keys) { Tq_reset(s, pen); return 0; }
    s->lastX = m->x;
    s->lastY = m->y;
    s->lastHold = keys;
    return keys;
}
#endif
