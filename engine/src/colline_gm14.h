// UNDERTALE's line, circle and distance rules and its make_color_rgb (colflip, the follow-up check): GameMaker: Studio
// 1.4's runner, read from UNDERTALE.exe. The instance's
// bounding box is the integer one GMS 1.4 keeps, right and bottom inclusive: Collision_computeBBox's in compatibility
// mode, less one on the right and bottom (collision_gm14.h's rule).
// - collision_line (0x4f2800 -> 0x43f6d0 -> 0x48c1f0): the end points stay float32 through the bbox test and the
//   clip to [left, right + 1]; the non-precise answer is the clipped box test alone (no rotated box); the precise walk
//   rounds the clipped end points, hands an axis-aligned line to the rectangle walk, and otherwise samples one pixel
//   corner per column (or row) of the inclusive bbox, its minor coordinate continuous, against the rounded position,
//   with the float32 trig of float pi.
// - collision_circle (0x4f2710 -> 0x43f120): the float32 box (x - r, y - r, x + r, y + r) against the inclusive bbox,
//   the ellipse of its rounded corners (integer centre and radii) at the bbox corners; not precise only (UT never asks).
// - distance_to_point and distance_to_object (0x4f19b0, 0x4f1b50 -> 0x4f1a80): the inclusive bbox; a sprite-less
//   instance is the point (trunc x, trunc y); distance_to_object rounds each distance to float32 and gives 1000000
//   when no instance is found.
// - make_color_rgb (0x4f7d00): (b << 16) + (g << 8) + r, the channels truncated (YYGetInt32 is cvttsd2si).
//
// The code is late code of its own (colline_gm14.c: .text.late.colline_gm14 and .rodata.late.colline_gm14, listed last
// in platform/3ds/late_reserve.ld), compiled for UNDERTALE builds only (ENABLE_WAD16): a DELTARUNE build links none of
// it and keeps its bytes. VM_create reaches it through COLFLIP_REGISTER_ALL (vm_builtins.h).
#ifndef _BS_COLLINE_GM14_H_
#define _BS_COLLINE_GM14_H_

#include "collision_gm14.h"
#include "vm_builtins.h"

// collision_line's test of one instance: the end points as float32, x1..y2 in argument order.
bool Colline14_lineHits(Runner* runner, Instance* inst, float x1, float y1, float x2, float y2, bool prec);
// collision_circle's test of one instance, not precise (UT's two collision_circle calls pass prec 0; a precise call
// goes to the builtin this one replaced): the box of the circle as float32 (x - r, y - r, x + r, y + r).
bool Colline14_ellipseHits(Runner* runner, Instance* inst, float x1, float y1, float x2, float y2);
// distance_to_point from inst.
double Colline14_distanceToPoint(Runner* runner, Instance* inst, double px, double py);
// The distance between two instances' boxes, as distance_to_object takes it (1000000 for self or an inactive one).
float Colline14_distancePair(Runner* runner, Instance* other, Instance* self);

#ifdef COLLINE14_DEFINE

#define COLLINE14_LATE __attribute__((noinline, section(".text.late.colline_gm14")))
#define COLLINE14_FLAT __attribute__((flatten))

// pi as UNDERTALE.exe multiplies by it (0x6d8800): the float32 pi widened to double.
#define COLLINE14_PI_F 3.1415927410125732

// x itself, after which the compiler knows nothing of it: no reassociation, folding or hoisting across it (the engine
// builds with -ffast-math).
static inline __attribute__((always_inline)) double Colline14_keep(double x) {
#if defined(__arm__)
    __asm__ volatile("" : "+w"(x));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(x));
#else
    __asm__ volatile("" : "+m"(x));
#endif
    return x;
}

// x / y, kept a division (collision_gm14.h's Colflip14_div).
static inline __attribute__((always_inline)) double Colline14_div(double x, double y) {
    return x / Colline14_keep(y);
}

// fistp of a float32 value: round to nearest, ties to even. Out of line: ten callers.
static COLLINE14_LATE int32_t Colline14_round(float x) {
    return (int32_t) GMLReal_bankersRound((GMLReal) x);
}

// (int32_t) floor(x) (UNDERTALE.exe's 2^52 floor, then cvttss2si).
static inline __attribute__((always_inline)) int32_t Colline14_floor(float x) {
    int32_t i = (int32_t) x;
    return (float) i > x && i != INT32_MIN ? i - 1 : i;
}

// GMS 1.4's integer box, right and bottom inclusive. A sprite-less instance has none here (collision_gm14.h's tests
// skip it too); the distances take it as a point.
typedef struct {
    int32_t l, t, r, b;
} Colline14Box;

// The bbox_left, bbox_top, bbox_right and bbox_bottom that VMBuiltins_getVariable reads (Collision_computeBBox, and in
// compatibility mode right and bottom less one), so that no copy of Collision_computeBBox (1.6 KB) is linked here.
// Collision_computeBBox has a box exactly when the instance has a collision sprite.
static inline __attribute__((always_inline)) int32_t Colline14_boxSide(Runner* runner, Instance* inst, int16_t side) {
    return (int32_t) VMBuiltins_getVariable(runner->vmContext, inst, side, nullptr, -1).real;
}

static COLLINE14_LATE bool Colline14_box(Runner* runner, Instance* inst, Colline14Box* box) {
    if (Collision_getSprite(runner->dataWin, inst) == nullptr) return false;
    box->l = Colline14_boxSide(runner, inst, BUILTIN_VAR_BBOX_LEFT);
    box->t = Colline14_boxSide(runner, inst, BUILTIN_VAR_BBOX_TOP);
    box->r = Colline14_boxSide(runner, inst, BUILTIN_VAR_BBOX_RIGHT);
    box->b = Colline14_boxSide(runner, inst, BUILTIN_VAR_BBOX_BOTTOM);
    return true;
}

// One instance's mask arithmetic (the Sprite::Precise_* functions' arguments): the frame's mask, the rounded position,
// the scales, the origin and the float32 cosine and sine of -angle. fast: the scales are 1 and |angle| < 0.0001, where
// the rectangle and ellipse walks (not the line's) skip the rotation.
typedef struct {
    Sprite* spr;
    const uint8_t* mask;
    uint32_t rowBytes;
    int32_t iix, iiy;
    double xs, ys, ox, oy, cs, sn;
    bool fast;
} Colline14Shape;

static inline __attribute__((always_inline)) bool Colline14_shape(Sprite* spr, Instance* inst, Colline14Shape* s) {
    float xs = inst->imageXscale, ys = inst->imageYscale;
    // A zero scale makes every cell infinite or NaN, which cvttss2si turns into 0x80000000: no pixel is in.
    if (xs == 0.0f || ys == 0.0f) return false;
    int32_t n = (int32_t) spr->maskCount;
    int32_t frame = (int32_t) inst->imageIndex % n;
    if (0 > frame) frame += n;
    s->spr = spr;
    s->mask = spr->masks[frame];
    s->rowBytes = (spr->maskWidth + 7) / 8;
    s->iix = Colline14_round(inst->x);
    s->iiy = Colline14_round(inst->y);
    s->xs = (double) xs;
    s->ys = (double) ys;
    s->ox = (double) spr->originX;
    s->oy = (double) spr->originY;
    float angle = (float) inst->imageAngle;
    float rad = (float) Colline14_div(Colline14_keep(-(double) angle * COLLINE14_PI_F), 180.0);
    double r = Colline14_keep((double) rad);
    s->sn = (double) (float) sin(r);
    s->cs = (double) (float) cos(r);
    s->fast = xs == 1.0f && ys == 1.0f && 0.0001f > fabsf(angle);
    return true;
}

// Whether the mask has sprite-local cell (c, r).
static inline __attribute__((always_inline)) bool Colline14_cell(const Colline14Shape* s, int32_t c, int32_t r) {
    Sprite* spr = s->spr;
    if (0 > c || c >= (int32_t) spr->width || 0 > r || r >= (int32_t) spr->height) return false;
    c -= spr->maskOffsetX;
    r -= spr->maskOffsetY;
    if (0 > c || c >= (int32_t) spr->maskWidth || 0 > r || r >= (int32_t) spr->maskHeight) return false;
    return (s->mask[(uint32_t) r * s->rowBytes + ((uint32_t) c >> 3)] & (1 << (7 - (c & 7)))) != 0;
}

// The cell under offset (dx, dy) from the rounded position, with the cosine and sine given (1 and 0 on the fast path,
// where the cell is the offset plus the origin): (dx cos + dy sin) / xscale + ox and (dy cos - dx sin) / yscale + oy,
// in double, rounded to float32 and floored. Out of line: the rectangle and line walks share it.
static COLLINE14_LATE bool Colline14_has(const Colline14Shape* s, double cs, double sn, double dx, double dy) {
    int32_t c = Colline14_floor((float) (Colline14_div(dx * cs + dy * sn, s->xs) + s->ox));
    int32_t r = Colline14_floor((float) (Colline14_div(dy * cs - dx * sn, s->ys) + s->oy));
    return Colline14_cell(s, c, r);
}

// Sprite::Precise_Rectangle (0x48b9f0): every pixel corner of the rect within the bbox, both ends inclusive.
static inline __attribute__((always_inline)) bool Colline14_preciseRect(const Colline14Box* box, const Colline14Shape* s, int32_t l, int32_t t, int32_t r, int32_t b) {
    double cs = s->fast ? 1.0 : s->cs, sn = s->fast ? 0.0 : s->sn;
    if (box->l > l) l = box->l;
    if (r > box->r) r = box->r;
    if (box->t > t) t = box->t;
    if (b > box->b) b = box->b;
    for (int32_t u = l; r >= u; u++) {
        for (int32_t v = t; b >= v; v++) {
            if (Colline14_has(s, cs, sn, (double) (float) (u - s->iix), (double) (float) (v - s->iiy))) return true;
        }
    }
    return false;
}

// Sprite::Precise_Line (0x48c1f0), from the rounded clipped end points.
static inline __attribute__((always_inline)) bool Colline14_preciseLine(const Colline14Box* box, const Colline14Shape* s, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    if (x1 == x2 || y1 == y2) return Colline14_preciseRect(box, s, x1, y1 < y2 ? y1 : y2, x2, y1 < y2 ? y2 : y1);
    int32_t adx = x2 - x1, ady = y2 - y1;
    if (0 > adx) adx = -adx;
    if (0 > ady) ady = -ady;
    bool xMajor = adx >= ady;
    // Walk the major axis upward: a is the major coordinate, m the minor one.
    int32_t a1 = xMajor ? x1 : y1, m1 = xMajor ? y1 : x1, a2 = xMajor ? x2 : y2, m2 = xMajor ? y2 : x2;
    if (a1 > a2) {
        int32_t tmp = a1; a1 = a2; a2 = tmp;
        tmp = m1; m1 = m2; m2 = tmp;
    }
    float slope = (float) Colline14_div((double) (float) (m2 - m1), (double) (float) (a2 - a1));
    int32_t lo = xMajor ? box->l : box->t, hi = xMajor ? box->r : box->b;
    int32_t aOrigin = xMajor ? s->iix : s->iiy, mOrigin = xMajor ? s->iiy : s->iix;
    if (a1 > lo) lo = a1;
    if (hi > a2) hi = a2;
    for (int32_t a = lo; hi >= a; a++) {
        // The minor offset: (a - a1) * slope + m1, less the rounded position, in double.
        double dm = Colline14_keep((double) (float) (a - a1) * (double) slope + (double) (float) m1) - (double) (float) mOrigin;
        double da = (double) (float) (a - aOrigin);
        if (Colline14_has(s, s->cs, s->sn, xMajor ? da : dm, xMajor ? dm : da)) return true;
    }
    return false;
}

COLLINE14_LATE COLLINE14_FLAT bool Colline14_lineHits(Runner* runner, Instance* inst, float x1, float y1, float x2, float y2, bool prec) {
    Colline14Box box;
    if (!Colline14_box(runner, inst, &box)) return false;
    float left = (float) box.l, top = (float) box.t, right1 = (float) (box.r + 1), bottom1 = (float) (box.b + 1);
    if ((x2 > x1 ? x1 : x2) >= right1 || left > (x1 > x2 ? x1 : x2)) return false;
    if ((y2 > y1 ? y1 : y2) >= bottom1 || top > (y1 > y2 ? y1 : y2)) return false;
    if (x1 > x2) {
        float tmp = x1; x1 = x2; x2 = tmp;
        tmp = y1; y1 = y2; y2 = tmp;
    }
    // The clip to [left, right + 1], in double from the float32 values, each result rounded to float32.
    if (left > x1) {
        y1 = (float) (Colline14_div(Colline14_keep(((double) left - x1) * ((double) y2 - y1)), (double) x2 - x1) + y1);
        x1 = left;
    }
    if (x2 > right1) {
        y2 = (float) (Colline14_div(Colline14_keep(((double) right1 - x2) * ((double) y2 - y1)), (double) x2 - x1) + y2);
        x2 = (float) ((double) (float) box.r + 1.0);
    }
    if (top > y1 && top > y2) return false;
    if (y1 >= bottom1 && y2 >= bottom1) return false;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!prec || !Collision_hasFrameMasks(spr)) return true;
    Colline14Shape s;
    if (!Colline14_shape(spr, inst, &s)) return false;
    return Colline14_preciseLine(&box, &s, Colline14_round(x1), Colline14_round(y1), Colline14_round(x2), Colline14_round(y2));
}

// PtInEllipse (0x43f090): whether integer point (x, y) is in the ellipse of the integer rect, its centre and radii
// halved with truncation. Out of line: the circle test asks it for four corners.
static COLLINE14_LATE bool Colline14_ptInEllipse(const int32_t* rc, int32_t x, int32_t y) {
    double a = Colline14_div((double) x - (double) ((rc[0] + rc[2]) / 2), (double) ((rc[2] - rc[0]) / 2));
    double b = Colline14_div((double) y - (double) ((rc[1] + rc[3]) / 2), (double) ((rc[3] - rc[1]) / 2));
    return 1.0 >= b * b + a * a;
}

COLLINE14_LATE COLLINE14_FLAT bool Colline14_ellipseHits(Runner* runner, Instance* inst, float x1, float y1, float x2, float y2) {
    Colline14Box box;
    if (!Colline14_box(runner, inst, &box)) return false;
    float minX = x2 > x1 ? x1 : x2, maxX = x1 > x2 ? x1 : x2, minY = y2 > y1 ? y1 : y2, maxY = y1 > y2 ? y1 : y2;
    if (minX >= (float) (box.r + 1) || (float) box.l > maxX) return false;
    if (minY >= (float) (box.b + 1) || (float) box.t > maxY) return false;
    // A box rounding to no width or height: Instance::Collision_Rectangle.
    // Rounding keeps the order, so the rounded box is the rounded bounds, sorted.
    int32_t rx1 = Colline14_round(x1), rx2 = Colline14_round(x2), ry1 = Colline14_round(y1), ry2 = Colline14_round(y2);
    if (rx1 == rx2 || ry1 == ry2) return Colflip14_rectHitsInstance(runner, inst, minX, minY, maxX, maxY, false);
    int32_t rc[4] = { rx2 > rx1 ? rx1 : rx2, ry2 > ry1 ? ry1 : ry2, rx1 > rx2 ? rx1 : rx2, ry1 > ry2 ? ry1 : ry2 };
    // Centre outside the bbox on both axes: one of the bbox corners must be in the ellipse.
    double cx = ((double) x1 + (double) x2) * 0.5, cy = ((double) y1 + (double) y2) * 0.5;
    if (!(cx >= (double) (float) box.l && (double) (float) box.r >= cx) && !(cy >= (double) (float) box.t && (double) (float) box.b >= cy)) {
        if (!Colline14_ptInEllipse(rc, box.l, box.t) && !Colline14_ptInEllipse(rc, box.l, box.b) && !Colline14_ptInEllipse(rc, box.r, box.t) && !Colline14_ptInEllipse(rc, box.r, box.b)) return false;
    }
    // Not precise: a hit. (Sprite::Precise_Ellipse, 0x48bd80, walks the rectangle walk's pixel corners inside the
    // ellipse; UT never calls it.)
    return true;
}

// The box distance_to_point and distance_to_object measure: the instance's, or a sprite-less one's point.
static COLLINE14_LATE void Colline14_distanceBox(Runner* runner, Instance* inst, Colline14Box* box) {
    if (Colline14_box(runner, inst, box)) return;
    box->l = box->r = (int32_t) inst->x;
    box->t = box->b = (int32_t) inst->y;
}

COLLINE14_LATE COLLINE14_FLAT double Colline14_distanceToPoint(Runner* runner, Instance* inst, double px, double py) {
    Colline14Box box;
    Colline14_distanceBox(runner, inst, &box);
    double xd = 0.0, yd = 0.0;
    if (px > (double) box.r) xd = px - (double) box.r;
    if ((double) box.l > px) xd = px - (double) box.l;
    if (py > (double) box.b) yd = py - (double) box.b;
    if ((double) box.t > py) yd = py - (double) box.t;
    return sqrt(yd * yd + xd * xd);
}

COLLINE14_LATE COLLINE14_FLAT float Colline14_distancePair(Runner* runner, Instance* other, Instance* self) {
    if (other == nullptr || other == self || !other->active) return 1000000.0f;
    Colline14Box o, m;
    Colline14_distanceBox(runner, other, &o);
    Colline14_distanceBox(runner, self, &m);
    uint32_t xd = 0, yd = 0;
    if (o.l > m.r) xd = (uint32_t) o.l - (uint32_t) m.r;
    if (m.l > o.r) xd = (uint32_t) o.r - (uint32_t) m.l;
    if (o.t > m.b) yd = (uint32_t) o.t - (uint32_t) m.b;
    if (m.t > o.b) yd = (uint32_t) o.b - (uint32_t) m.t;
    return (float) sqrt(Colline14_keep((double) (int32_t) (yd * yd + xd * xd)));
}

// make_color_rgb's colour (0x4f7d00): ((b << 8) + g) << 8 + r with adds, in 32 bits, no channel masked or clamped.
static inline __attribute__((always_inline)) int32_t Colline14_rgb(int32_t r, int32_t g, int32_t b) {
    return (int32_t) (((((uint32_t) b << 8) + (uint32_t) g) << 8) + (uint32_t) r);
}

#endif /* COLLINE14_DEFINE */

#endif /* _BS_COLLINE_GM14_H_ */
