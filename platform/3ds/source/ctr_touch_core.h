// battletouch's pure logic: the stylus state machine for UNDERTALE battles and the merge of its keys with the
// buttons'. No libctru, engine or game reads here: z_battletouch.c reads
// the game into a TouchMenu each frame and applies a TouchOut; a host test
// compiles this file as it is.
//
// Key bits are CtrGameKey bits (ctr_input_map.h): 1 << GAME_KEY_Z and so on. Room coordinates are whole room pixels.
#ifndef CTR_TOUCH_CORE_H
#define CTR_TOUCH_CORE_H

#include <stdbool.h>
#include <stdint.h>

#ifndef TOUCH_INLINE
#define TOUCH_INLINE
#endif

// CtrGameKey's numbers (ctr_input_map.h; z_battletouch.c checks they agree).
#define TOUCH_KEY_Z (1u << 1)
#define TOUCH_KEY_X (1u << 2)
#define TOUCH_KEY_ENTER (1u << 4)
#define TOUCH_KEY_UP (1u << 5)
#define TOUCH_KEY_DOWN (1u << 6)
#define TOUCH_KEY_LEFT (1u << 7)
#define TOUCH_KEY_RIGHT (1u << 8)
#define TOUCH_KEY_SHIFT (1u << 9)
#define TOUCH_KEY_ARROWS (TOUCH_KEY_UP | TOUCH_KEY_DOWN | TOUCH_KEY_LEFT | TOUCH_KEY_RIGHT)
// A physical press of one of these abandons a stylus press in progress: confirm, cancel or a move.
#define TOUCH_KEY_ABANDON (TOUCH_KEY_Z | TOUCH_KEY_X | TOUCH_KEY_ENTER | TOUCH_KEY_SHIFT | TOUCH_KEY_ARROWS)

// A held press's area grows by this many room pixels on each side (4 bottom-screen pixels at 2 room px per pixel), so
// a pen's jitter on an edge can't flicker the highlight.
#define TOUCH_GROW 8

typedef struct { uint32_t held, pressed, released; } TouchEdges; // CtrInputEdges' layout

// ===[ Merge: the buttons' edges and the stylus's, as one keyboard ]===
//
// A key is down while either side holds it. A side's press is a press only if the other side wasn't already holding
// the key; a side's release is a release only if the other side isn't holding it at the end of the frame. ePrev and
// tPrev are each side's held keys at the end of the previous frame. With the stylus idle (t all 0, tPrev 0) the
// result is e, bit for bit.
static inline TOUCH_INLINE TouchEdges Touch_merge(TouchEdges e, TouchEdges t, uint32_t ePrev, uint32_t tPrev) {
    TouchEdges r;
    r.held = e.held | t.held;
    r.pressed = (e.pressed & ~tPrev) | (t.pressed & ~(e.held | ePrev));
    r.released = (e.released & ~t.held) | (t.released & ~r.held);
    return r;
}

// The stylus's own edges this frame. A tap holds its key for this one frame and lets go on the next, as the quickest
// D-pad tap does: UNDERTALE reads Z and X as held keys (control_update compares this step's hold with the last), so
// a press and release in the same frame would never reach it. tPrev: last frame's held (the result's held, kept).
static inline TOUCH_INLINE TouchEdges Touch_edges(uint32_t pulse, uint32_t hold, uint32_t tPrev) {
    uint32_t held = hold | pulse;
    return (TouchEdges) {held, pulse | (held & ~tPrev), tPrev & ~held};
}

// ===[ The game as the state machine sees it, filled in each frame the pen is down or a press is in progress ]===

enum { TOUCH_CTX_OFF, TOUCH_CTX_MENU, TOUCH_CTX_TAP, TOUCH_CTX_DEFENCE };

typedef struct { int16_t x0, y0, x1, y1; } TouchRect; // room px, x0 <= x < x1, y0 <= y < y1

#define TOUCH_ITEMS 6
typedef struct {
    uint8_t ctx;                     // TOUCH_CTX_*
    bool atButtons;                  // MENU: the FIGHT/ACT/ITEM/MERCY row (bmenuno 0); items are the buttons
    bool pageValid;                  // MENU: an ITEM page label to tap (a second page exists)
    int8_t cursor;                   // MENU: the game's cursor for this menu now
    int8_t openButton;               // MENU in a submenu: the lit button (bmenucoord[0])
    uint8_t count;                   // MENU: items
    uint8_t valid;                   // MENU: bit k set if item k can be chosen (the arrows' own rules)
    uint8_t buttonValid;             // MENU in a submenu: bit k set if button k exists and can be chosen
    uint32_t id;                     // MENU: which menu this is (z_battletouch.c: bmenuno, myfight, mnfight, active)
    union {
        struct {
            TouchRect item[TOUCH_ITEMS];
            TouchRect button[4];     // MENU in a submenu: the buttons
            TouchRect page;          // MENU: the ITEM page label
        };
        // DEFENCE, purple: the web's lines, in the menu rectangles' bytes (a defence has none), so TouchMenu keeps
        // its size and z_battletouch.c's first slot its code.
        struct {
            int32_t line0, lineSpace; // line 1's y and the lines' spacing, 1/16 room px
            int8_t line, lines;       // the SOUL's line (1 = top) and how many
            bool lineBusy;            // the SOUL is moving between lines (a press now is ignored)
        };
    };
    uint8_t pageSlot;                // MENU: the right-column slot of the cursor's row, where a page turn starts
    uint8_t soul;                    // DEFENCE: TOUCH_SOUL_*, how the stylus moves this SOUL (NONE: it doesn't)
    int32_t soulX, soulY;            // DEFENCE: the SOUL's centre, 1/16 room px
    int32_t soulStep;                // DEFENCE: one arrow's move per frame (global.sp), 1/16 room px
} TouchMenu;

// The SOULs the stylus moves (design section 4). Every other heart, mode or state is NONE: buttons only.
enum { TOUCH_SOUL_NONE, TOUCH_SOUL_RED, TOUCH_SOUL_BLUE, TOUCH_SOUL_GREEN, TOUCH_SOUL_PURPLE, TOUCH_SOUL_YELLOW };

// Blue: Up (the jump) is held while the pen is more than one SOUL (16 room px) above the centre.
#define TOUCH_BLUE_ABOVE (16 * 16)

// Green: no arrow while the pen is within one SOUL (16 room px) of the centre on both axes; within 4 room px of a
// diagonal the axis held last frame keeps it.
#define TOUCH_GREEN_DEAD (16 * 16)
#define TOUCH_GREEN_BAND (4 * 16)

// Yellow (11-08): a pen-down within this distance of the SOUL's centre fires one shot (a Z press); a
// pen-down farther away only steers. Nothing else fires: not a drag, not a held pen, not a mode change under the pen.
// Shots hit bombs and Mettaton's hearts, so steering must never shoot.
#define TOUCH_YELLOW_FIRE (24 * 16)

// What z_battletouch.c does with this frame.
typedef struct {
    uint32_t pulse;  // keys tapped: held this frame only, released the next (Touch_edges)
    uint32_t hold;   // keys held this frame
    int8_t write;    // the menu's cursor to write before the game's Step, or -1
    bool sound;      // with the write: the arrows' move sound (the cursor changed)
    bool used;       // a stylus action reached the game (the first-time hint stops)
} TouchOut;

enum {
    TOUCH_IDLE,      // the pen is up, nothing in progress
    TOUCH_DEAD,      // the pen does nothing until it is lifted
    TOUCH_PRESS,     // the pen went down on a menu item: lit while inside it
    TOUCH_BUTTON,    // the pen went down on a button while a submenu is open: acts on release
    TOUCH_PAGE,      // the pen went down on the ITEM page label: acts on release
    TOUCH_CASCADE,   // a button released while a submenu was open: cancels every other frame, then the button
    TOUCH_FOLLOW,    // the pen went down in a defence: the SOUL follows it until the lift
};

typedef struct {
    uint8_t mode;
    uint8_t ctx;      // the context the pen went down in
    int8_t item;      // the pressed item or button
    int8_t saved;     // TOUCH_PRESS: the cursor before the press
    int8_t target;    // TOUCH_CASCADE: the button to open at the buttons (-1: just go back)
    uint8_t steps;    // TOUCH_CASCADE: cancels sent
    bool lit;         // TOUCH_PRESS: the item is written as the cursor
    bool pen;         // the pen was down last frame
    bool rest;        // TOUCH_CASCADE: last frame's tap is being released, nothing this frame
    int8_t pCur;      // TOUCH_PRESS: the cursor, the item and lit before this sample (the lift can go back to them);
    int8_t pItem;     // in the padding before id, so TouchState keeps its size (20 B) and every later field its place
    bool pLit;
    uint32_t id;      // the menu the pen went down in (TOUCH_CASCADE: the menu the last cancel was sent from)
    uint8_t soul;     // TOUCH_FOLLOW: the SOUL kind followed last frame
    uint8_t locked;   // TOUCH_FOLLOW: axes the buttons took until the lift (1: left/right, 2: up/down)
    uint8_t face;     // TOUCH_FOLLOW, green: the axis held last frame (0 none, 1 left/right, 2 up/down)
    bool lineKey;     // TOUCH_FOLLOW, purple: a line key was held last frame (the next must be a fresh press); in
                      // the padding, so TouchState keeps its size (20 B) and z_battletouch.c's first slot its code
} TouchState;

_Static_assert(sizeof(TouchState) == 20 && __builtin_offsetof(TouchState, id) == 12, "TouchState's layout");

// The lift (11-08). A resistive panel can report a drifted last sample as the stylus's pressure drops,
// so the pen is read through its last three samples (bottom-screen px, one per game frame):
// - a release uses the newest STILL sample: the newest, unless it moved more than TOUCH_STILL px (on either axis) from
//   a still one before it (or from the pen-down), which is used instead; a sample still waiting (below) is never used;
// - while the pen is down, a sample more than TOUCH_JUMP px from the one before, after a step that was not, waits one
//   sample: the core sees the one before for that frame, and the next sample is used whatever it is. Only the start of
//   a fast flick pays the frame; its later samples, each a jump from a jump, are used at once.
// Steady motion has no still sample to go back to, so a release while dragging uses the newest.
#define TOUCH_STILL 8
#define TOUCH_JUMP 32

typedef struct {
    int16_t x[3], y[3];  // the last samples, [0] the newest
    uint8_t n;           // samples since the pen-down (at most 3)
    bool pen;            // the pen was down last frame
    bool wait;           // [0] moved more than TOUCH_JUMP and is not used yet
} TouchPen;

static inline TOUCH_INLINE bool Touch_near(const TouchPen* p, int32_t i, int32_t d) {
    int32_t dx = p->x[i] - p->x[i + 1], dy = p->y[i] - p->y[i + 1];
    return -d <= dx && dx <= d && -d <= dy && dy <= d;
}

// Each frame, with the pen's state and (while down) its place: the sample the core sees, in *x, *y. True when it is
// the one before the newest (the newest jumped, or the lift went back from it).
static inline TOUCH_INLINE bool Touch_pen(TouchPen* p, bool pen, int32_t sx, int32_t sy, int32_t* x, int32_t* y) {
    bool back = false;
    if (pen) {
        if (!p->pen) p->n = 0;
        p->x[2] = p->x[1], p->y[2] = p->y[1];
        p->x[1] = p->x[0], p->y[1] = p->y[0];
        p->x[0] = (int16_t) sx, p->y[0] = (int16_t) sy;
        if (3 > p->n) p->n++;
        back = p->n >= 2 && !p->wait && !Touch_near(p, 0, TOUCH_JUMP) && (p->n == 2 || Touch_near(p, 1, TOUCH_JUMP));
        p->wait = back;
    } else if (p->pen) {
        back = p->n >= 2 && (p->wait || (!Touch_near(p, 0, TOUCH_STILL) &&
                                          (p->n == 2 || Touch_near(p, 1, TOUCH_STILL))));
        p->wait = false;
    }
    p->pen = pen;
    *x = back ? p->x[1] : p->x[0];
    *y = back ? p->y[1] : p->y[0];
    return back;
}

static inline TOUCH_INLINE bool Touch_inside(TouchRect r, int32_t x, int32_t y, int32_t grow) {
    return r.x0 - grow <= x && x < r.x1 + grow && r.y0 - grow <= y && y < r.y1 + grow;
}

// The first valid rectangle under the point, or -1.
static inline TOUCH_INLINE int32_t Touch_hit(const TouchRect* r, uint32_t count, uint32_t valid, int32_t x, int32_t y) {
    for (uint32_t k = 0; count > k; k++) {
        if ((valid >> k & 1u) != 0 && Touch_inside(r[k], x, y, 0)) return (int32_t) k;
    }
    return -1;
}

static inline TOUCH_INLINE void Touch_write(TouchOut* out, int32_t value, int32_t cursorNow) {
    out->write = (int8_t) value;
    out->sound = value != cursorNow;
}

static inline TOUCH_INLINE void Touch_reset(TouchState* s) {
    bool pen = s->pen;
    *s = (TouchState) {0};
    s->pen = pen;
    s->mode = pen ? TOUCH_DEAD : TOUCH_IDLE;
}

// A pen-down in MENU (the menu m as the last Step left it).
static inline TOUCH_INLINE void Touch_menuDown(TouchState* s, const TouchMenu* m, int32_t x, int32_t y, TouchOut* out) {
    int32_t k = Touch_hit(m->item, m->count, m->valid, x, y);
    if (k >= 0) {
        // maintainer: "when we hold, the icon turns yellow": the game's cursor on the item, as the arrows would leave it.
        s->mode = TOUCH_PRESS;
        s->item = (int8_t) k;
        s->saved = m->cursor;
        s->lit = true;
        Touch_write(out, k, m->cursor);
        out->used = true;
        return;
    }
    if (m->pageValid && Touch_inside(m->page, x, y, 0)) {
        s->mode = TOUCH_PAGE;
        return;
    }
    if (!m->atButtons) {
        k = Touch_hit(m->button, 4, m->buttonValid, x, y);
        if (k >= 0) {
            s->mode = TOUCH_BUTTON;
            s->item = (int8_t) k;
            return;
        }
    }
    // maintainer 11-08: off the items, the press still follows the pen: dragged onto one, it lights.
    s->mode = TOUCH_PRESS;
    s->item = -1;
    s->saved = m->cursor;
    s->lit = false;
}

// One axis of the follow law: the arrow toward the target while it is more than half a move away, else none. From
// any distance in (half, 2 * half] one move ends inside the band, so a still pen is reached and the SOUL rests.
static inline TOUCH_INLINE uint32_t Touch_axis(int32_t target, int32_t centre, int32_t half, uint32_t less, uint32_t more) {
    int32_t d = target - centre;
    return d > half ? more : -half > d ? less : 0u;
}

// FOLLOW, each frame the pen is down in a defence: the arrows the SOUL needs to move toward the pen (x, y, room px),
// held as a D-pad player would hold them. The game moves it: its Keyboard events, its speed, its border.
static inline TOUCH_INLINE void Touch_follow(TouchState* s, int32_t x, int32_t y, const TouchMenu* m, uint32_t eHeld,
                                             TouchOut* out) {
    // A button on an axis takes that axis until the lift (the player's own arrow wins); X and Z pass through.
    if ((eHeld & (TOUCH_KEY_LEFT | TOUCH_KEY_RIGHT)) != 0) s->locked |= 1u;
    if ((eHeld & (TOUCH_KEY_UP | TOUCH_KEY_DOWN)) != 0) s->locked |= 2u;
    if (m->soul != s->soul) {
        // The SOUL's mode changed under the pen: let go this frame; the new mode's law reads the pen from the next.
        s->soul = m->soul;
        s->face = 0;
        s->lineKey = false;
        return;
    }
    if (m->soul == TOUCH_SOUL_PURPLE) {
        // maintainer: Muffet "splits each line into a row hitbox (the stylus row picks the line, x tracks)". x as red at the
        // web's own 4 px (X doesn't slow it). y: the line nearest the pen. The game takes a line key only as a fresh
        // press while the SOUL isn't already moving, so the key is held for one frame and let go the next.
        if ((s->locked & 1u) == 0) out->hold |= Touch_axis(x * 16, m->soulX, m->soulStep / 2, TOUCH_KEY_LEFT, TOUCH_KEY_RIGHT);
        bool key = false;
        if ((s->locked & 2u) == 0 && m->lineSpace > 0 && m->lines > 0) {
            int32_t d = y * 16 - m->line0;
            int32_t want = 0 > d ? 1 : (d + m->lineSpace / 2) / m->lineSpace + 1;
            if (want > m->lines) want = m->lines;
            if (!m->lineBusy && !s->lineKey && want != m->line) {
                out->hold |= want < m->line ? TOUCH_KEY_UP : TOUCH_KEY_DOWN;
                key = true;
            }
        }
        s->lineKey = key;
        if (out->hold != 0) out->used = true;
        return;
    }
    if (m->soul == TOUCH_SOUL_GREEN) {
        // maintainer: "click and drag by quadrant (the quadrant held sets the facing)". The arrow toward the pen's side of
        // the SOUL is held; the shield turns to the arrow held and keeps its facing when none is. A button arrow
        // takes the facing until the lift.
        int32_t dx = x * 16 - m->soulX, dy = y * 16 - m->soulY;
        int32_t ax = 0 > dx ? -dx : dx, ay = 0 > dy ? -dy : dy;
        uint8_t axis = 0;
        if (s->locked == 0 && (ax > TOUCH_GREEN_DEAD || ay > TOUCH_GREEN_DEAD)) {
            if (ax > ay + TOUCH_GREEN_BAND) axis = 1;
            else if (ay > ax + TOUCH_GREEN_BAND) axis = 2;
            else axis = s->face != 0 ? s->face : ax >= ay ? 1 : 2;
        }
        s->face = axis;
        if (axis == 1) out->hold = 0 > dx ? TOUCH_KEY_LEFT : TOUCH_KEY_RIGHT;
        if (axis == 2) out->hold = 0 > dy ? TOUCH_KEY_UP : TOUCH_KEY_DOWN;
        if (out->hold != 0) out->used = true;
        return;
    }
    if (m->soul == TOUCH_SOUL_RED || m->soul == TOUCH_SOUL_BLUE || m->soul == TOUCH_SOUL_YELLOW) {
        // Yellow moves as red (the game's movement 1). The slow key (X) halves the move: the game adds sp, then takes sp / 2 back (the SOUL's arrow events).
        int32_t step = (eHeld & TOUCH_KEY_X) != 0 ? m->soulStep / 2 : m->soulStep;
        if (step <= 0) return;
        if ((s->locked & 1u) == 0) out->hold |= Touch_axis(x * 16, m->soulX, step / 2, TOUCH_KEY_LEFT, TOUCH_KEY_RIGHT);
        if ((s->locked & 2u) == 0) {
            if (m->soul != TOUCH_SOUL_BLUE) {
                out->hold |= Touch_axis(y * 16, m->soulY, step / 2, TOUCH_KEY_UP, TOUCH_KEY_DOWN);
            } else if (m->soulY - y * 16 > TOUCH_BLUE_ABOVE) {
                // maintainer: "press above the heart and it jumps (tries to jump) toward the stylus". Up held: the game
                // starts a jump only from the ground and cuts it when Up is let go, so the SOUL rises toward the
                // pen and a pen held high jumps again on landing, as a held Up does. Never Down.
                out->hold |= TOUCH_KEY_UP;
            }
        }
        if (out->hold != 0) out->used = true;
    }
}

// One frame. pen: the pen is down, at (x, y) in room px. back: (x, y) is the sample before the newest (Touch_pen).
// ePressed, eHeld: the buttons' presses and held keys this frame. Fills *out.
static inline TOUCH_INLINE void Touch_stepAt(TouchState* s, bool pen, int32_t x, int32_t y, bool back,
                                             const TouchMenu* m, uint32_t ePressed, uint32_t eHeld, TouchOut* out) {
    *out = (TouchOut) {0};
    out->write = -1;
    bool down = pen && !s->pen, up = !pen && s->pen;
    s->pen = pen;
    bool abandon = (ePressed & TOUCH_KEY_ABANDON) != 0;

    if (s->mode == TOUCH_CASCADE) {
        // A cancel every other frame (each tap's key is let go in between) until the buttons, each checked to have
        // taken (the menu changed); then the button. A button press, another context or a cancel that did nothing
        // ends it: the game is then the player's.
        if (abandon || m->ctx != TOUCH_CTX_MENU) {
            s->mode = pen ? TOUCH_DEAD : TOUCH_IDLE;
        } else if (s->rest) {
            s->rest = false;
        } else if (!m->atButtons && (m->id == s->id || s->steps >= 4)) {
            s->mode = pen ? TOUCH_DEAD : TOUCH_IDLE;
        } else if (m->atButtons) {
            if (s->target >= 0 && (m->valid >> s->target & 1u) != 0) {
                Touch_write(out, s->target, m->cursor);
                out->pulse = TOUCH_KEY_Z;
            }
            s->mode = pen ? TOUCH_DEAD : TOUCH_IDLE;
        } else {
            out->pulse = TOUCH_KEY_X;
            s->id = m->id;
            s->steps++;
            s->rest = true;
        }
        // A pen put down during the cancels belongs to nothing: it waits for its lift.
        return;
    }

    if (s->mode == TOUCH_FOLLOW) {
        // The defence ended (or the pen lifted): touch's arrows go with it, and a pen still down waits for its lift.
        if (!pen || m->ctx != TOUCH_CTX_DEFENCE) {
            s->mode = pen ? TOUCH_DEAD : TOUCH_IDLE;
            return;
        }
        Touch_follow(s, x, y, m, eHeld, out);
        return;
    }

    if (down) {
        s->ctx = m->ctx;
        s->id = m->id;
        if (m->ctx == TOUCH_CTX_MENU && !abandon) {
            Touch_menuDown(s, m, x, y, out);
        } else if (m->ctx == TOUCH_CTX_TAP && !abandon) {
            // maintainer: "if you tap anywhere on the screen while its up, it should trigger a press": the FIGHT bar, and
            // text that waits for Z. One press, on this frame.
            out->pulse = TOUCH_KEY_Z;
            out->used = true;
            s->mode = TOUCH_DEAD;
        } else if (m->ctx == TOUCH_CTX_DEFENCE) {
            // maintainer: "the heart should follow the stylus centered in the middle": grabbed on the pen-down, from this
            // frame. A SOUL the stylus doesn't move (NONE) is held anyway, so a mode change to one it does starts.
            s->mode = TOUCH_FOLLOW;
            s->soul = m->soul;
            s->locked = 0;
            s->face = 0;
            s->lineKey = false;
            Touch_follow(s, x, y, m, eHeld, out);
            if (m->soul == TOUCH_SOUL_YELLOW) {
                // Yellow: a pen-down on the SOUL also fires, once (the game's own cooldown decides if a shot leaves).
                int32_t dx = x * 16 - m->soulX, dy = y * 16 - m->soulY;
                if (TOUCH_YELLOW_FIRE * TOUCH_YELLOW_FIRE >= dx * dx + dy * dy) {
                    out->pulse = TOUCH_KEY_Z;
                    out->used = true;
                }
            }
        } else {
            s->mode = TOUCH_DEAD;
        }
        return;
    }

    if (s->mode == TOUCH_PRESS || s->mode == TOUCH_BUTTON || s->mode == TOUCH_PAGE) {
        // The menu changed under the pen, or a button was pressed: the press is over, nothing restored.
        if (abandon || m->ctx != s->ctx || m->id != s->id) {
            s->mode = pen ? TOUCH_DEAD : TOUCH_IDLE;
            return;
        }
    }

    if (s->mode == TOUCH_PRESS) {
        // maintainer 11-08 (playtest1): the cursor follows the pen over the items, live (the item under the pen, its edge
        // grown while lit); a release on one confirms it ("when we release it opens the submenu": the game's own
        // Confirm reads the cursor this frame); a release anywhere else confirms nothing, and the cursor stays on the
        // last item the pen was over.
        if (up && back) {
            // The lift went back a sample (Touch_pen): the state that sample left, so a stray last sample's item never
            // sticks.
            s->item = s->pItem;
            s->lit = s->pLit;
        } else {
            s->pCur = m->cursor;
            s->pItem = s->item;
            s->pLit = s->lit;
        }
        bool in = s->lit && Touch_inside(m->item[s->item], x, y, TOUCH_GROW);
        if (!in) {
            int32_t k = Touch_hit(m->item, m->count, m->valid, x, y);
            if (k >= 0) {
                s->item = (int8_t) k;
                Touch_write(out, k, m->cursor);
            }
            in = k >= 0;
            s->lit = in;
        }
        if (up) {
            // Gone back: the cursor as that sample left it (its item, or where it was when it was on nothing).
            int32_t c = in ? s->item : s->pCur;
            if (back && c >= 0 && c != m->cursor) Touch_write(out, c, m->cursor);
            if (in) out->pulse = TOUCH_KEY_Z;
            s->mode = TOUCH_IDLE;
        }
        return;
    }

    if (s->mode == TOUCH_PAGE) {
        if (up) {
            // The right-hand slot of the cursor's row, then the game's own page turn from there (Right).
            if (Touch_inside(m->page, x, y, TOUCH_GROW)) {
                out->write = (int8_t) m->pageSlot;
                out->pulse = TOUCH_KEY_RIGHT;
                out->used = true;
            }
            s->mode = TOUCH_IDLE;
        }
        return;
    }

    if (s->mode == TOUCH_BUTTON) {
        if (up) {
            s->mode = TOUCH_IDLE;
            if (Touch_inside(m->button[s->item], x, y, TOUCH_GROW)) {
                // Back to the buttons one cancel at a time; then the released button, unless it is the one open.
                s->mode = TOUCH_CASCADE;
                s->target = s->item == m->openButton ? (int8_t) -1 : s->item;
                s->steps = 1;
                s->rest = true;
                out->pulse = TOUCH_KEY_X;
                out->used = true;
            }
        }
        return;
    }

    if (up) s->mode = TOUCH_IDLE;
}

// One frame, the newest sample (the host tests' and any caller without Touch_pen).
static inline TOUCH_INLINE void Touch_step(TouchState* s, bool pen, int32_t x, int32_t y, const TouchMenu* m,
                                           uint32_t ePressed, uint32_t eHeld, TouchOut* out) {
    Touch_stepAt(s, pen, x, y, false, m, ePressed, eHeld, out);
}

#endif
