// UNDERTALE's collision rules (colflip): GameMaker: Studio 1.4's runner, bytecode 16 and older, read from
// UNDERTALE.exe. DELTARUNE's runner differs in the
// instance scan, the point test and the rectangle test. GMS 1.4:
// - samples every pixel at its corner, with no shift for a negative scale;
// - always rounds the instance positions in the scan, whatever the scales;
// - turns a rotated offset with the float32 cosine and sine and truncates the scan's cells;
// - floors the point and rectangle cells after rounding them to float32;
// - misses a rectangle only from right + 1 on.
//
// All of it is late code of its own (.text.late.colflip_gm14, .bss.late.colflip_gm14 and .rodata.late.colflip_gm14,
// listed last in platform/3ds/late_reserve.ld), so collision_compat.c and every late function before it keep their
// bytes and addresses. Two calls are retargeted: VM_create calls Colflip14_registerAll in place of
// Colflip_registerAll, and Collision_scanPixelsLate calls Colflip14_scanPixels in place of Collision_compatScanPixels.
// Both wrappers go on to the old functions, which still answer DELTARUNE, when the data is not GMS 1.4's.
#ifndef _BS_COLLISION_GM14_H_
#define _BS_COLLISION_GM14_H_

#include "collision.h"

// Set by Colflip14_registerAll for bytecode 16 or older (UNDERTALE 16, DELTARUNE 17).
extern bool Colflip_gm14;
// collision_point's test of one instance in GMS 1.4: the bbox as Collision_compatPointHits takes it, then the mask.
bool Colflip14_pointHits(Runner* runner, Instance* inst, GMLReal px, GMLReal py, bool prec);
// The mask half (spr has frame masks): the point and the position rounded, the pixel's corner sampled, the offset
// rotated with the float32 trig, divided by the scale plus the origin, rounded to float32 and floored.
bool Colflip14_pointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py);
// collision_rectangle's test of one instance in GMS 1.4 (UNDERTALE.exe 0x43ed20, 0x48b9f0).
bool Colflip14_rectHitsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, bool prec);
// The instance scan in GMS 1.4 (UNDERTALE.exe 0x43fde0, 0x48afc0), over the same pixels as Collision_compatScanPixels.
bool Colflip14_scan(const ColflipScan* scan);

#ifdef COLLISION_GM14_DEFINE

#define COLFLIP14_LATE __attribute__((noinline, section(".text.late.colflip_gm14")))
// As COLFLIP_FLAT: header functions are inlined, so that no out-of-line copy lands outside the late section.
#define COLFLIP14_FLAT __attribute__((flatten))

bool Colflip_gm14 __attribute__((section(".bss.late.colflip_gm14")));

// x / y, kept a division under -freciprocal-math (collision.h's Colflip_div).
static inline __attribute__((always_inline)) double Colflip14_div(double x, double y) {
#if defined(__arm__)
    __asm__ volatile("" : "+w"(y));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(y));
#else
    __asm__ volatile("" : "+m"(y));
#endif
    return x / y;
}

// (int32_t) floorf(x) without the library call (collision.h's Colflip_floorf).
static inline __attribute__((always_inline)) int32_t Colflip14_floorf(float x) {
    int32_t i = (int32_t) x;
    return (float) i > x && i != INT32_MIN ? i - 1 : i;
}

// The mask column (or row) of sprite-local cell i, or -1 outside the sprite or (masked) its mask.
static inline __attribute__((always_inline)) int32_t Colflip14_index(int32_t i, uint32_t size, bool masked, int32_t maskOffset, uint32_t maskSize) {
    if (0 > i || i >= (int32_t) size) return -1;
    if (!masked) return i;
    int32_t m = i - maskOffset;
    if (0 > m || m >= (int32_t) maskSize) return -1;
    return m;
}

static inline __attribute__((always_inline)) bool Colflip14_maskBit(const uint8_t* mask, uint32_t rowBytes, int32_t c, int32_t r) {
    return (mask[(uint32_t) r * rowBytes + (uint32_t) (c >> 3)] & (1 << (7 - (c & 7)))) != 0;
}

// The rotation in float32: the angle converted to radians in double and rounded to float, its cosine and sine rounded
// to float (collision.h's Colflip_floatTrig, without its cache).
static inline __attribute__((always_inline)) void Colflip14_floatTrig(GMLReal angle, float* csf, float* snf) {
    float radf = (float) ((double) angle * M_PI / 180.0);
    *csf = (float) cos((double) radf);
    *snf = (float) sin((double) radf);
}

// One instance's point arithmetic, worked out once: the rounded position, the rotation and the frame's mask.
typedef struct {
    Sprite* spr;
    const uint8_t* mask;
    uint32_t rowBytes;
    GMLReal ix, iy, cs, sn;
    bool rotated;
} Colflip14Point;

static inline __attribute__((always_inline)) bool Colflip14_pointSetup(Sprite* spr, Instance* inst, Colflip14Point* p) {
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;
    p->spr = spr;
    p->mask = spr->masks[((uint32_t) inst->imageIndex) % spr->maskCount];
    p->rowBytes = (spr->maskWidth + 7) / 8;
    p->ix = GMLReal_bankersRound(inst->x);
    p->iy = GMLReal_bankersRound(inst->y);
    p->rotated = GMLReal_fabs(inst->imageAngle) > 0.0001;
    p->cs = 1.0;
    p->sn = 0.0;
    if (p->rotated) {
        float csf, snf;
        Colflip14_floatTrig(inst->imageAngle, &csf, &snf);
        p->cs = csf;
        p->sn = snf;
    }
    return true;
}

// Whether whole pixel (u, v) is set: its corner, (u - ix, v - iy), rotated, divided by the scale, plus the origin,
// rounded to float32 and floored.
static inline __attribute__((always_inline)) bool Colflip14_pointHas(const Colflip14Point* p, Instance* inst, GMLReal u, GMLReal v) {
    Sprite* spr = p->spr;
    GMLReal dx = u - p->ix;
    GMLReal dy = v - p->iy;
    if (p->rotated) {
        GMLReal rx = p->cs * dx - p->sn * dy;
        GMLReal ry = p->sn * dx + p->cs * dy;
        dx = rx;
        dy = ry;
    }
    int32_t c = Colflip14_index(Colflip14_floorf((float) (Colflip14_div(dx, inst->imageXscale) + (GMLReal) spr->originX)), spr->width, true, spr->maskOffsetX, spr->maskWidth);
    if (0 > c) return false;
    int32_t r = Colflip14_index(Colflip14_floorf((float) (Colflip14_div(dy, inst->imageYscale) + (GMLReal) spr->originY)), spr->height, true, spr->maskOffsetY, spr->maskHeight);
    if (0 > r) return false;
    return Colflip14_maskBit(p->mask, p->rowBytes, c, r);
}

COLFLIP14_LATE COLFLIP14_FLAT bool Colflip14_pointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py) {
    Colflip14Point p;
    if (!Colflip14_pointSetup(spr, inst, &p)) return false;
    return Colflip14_pointHas(&p, inst, GMLReal_bankersRound(px), GMLReal_bankersRound(py));
}

COLFLIP14_LATE COLFLIP14_FLAT bool Colflip14_pointHits(Runner* runner, Instance* inst, GMLReal px, GMLReal py, bool prec) {
    if (!Collision_pointInsideInstanceBox(runner, inst, px, py)) return false;
    if (!prec) return true;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    return !Collision_hasFrameMasks(spr) || Colflip14_pointInInstance(spr, inst, px, py);
}

// The separating-axis test of a rotated non-precise box against the rect (collision.h's Colflip_rectOverlapsOBB).
// GMS 1.4 has none (an open item); it is kept so that this answer stays the one DELTARUNE's rule gives.
static inline __attribute__((always_inline)) bool Colflip14_rectOverlapsOBB(Sprite* spr, Instance* inst, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2) {
    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal corners[4][2] = { {rx1, ry1}, {rx2, ry1}, {rx1, ry2}, {rx2, ry2} };
    GMLReal uMin = 0, uMax = 0, vMin = 0, vMax = 0;
    repeat(4, c) {
        GMLReal pu, pv;
        Collision_obbWorldToLocal(&obb, corners[c][0], corners[c][1], &pu, &pv);
        if (c == 0) { uMin = uMax = pu; vMin = vMax = pv; }
        else {
            if (pu < uMin) uMin = pu; else if (pu > uMax) uMax = pu;
            if (pv < vMin) vMin = pv; else if (pv > vMax) vMax = pv;
        }
    }
    if (uMin >= obb.lx1 || obb.lx0 >= uMax) return false;
    if (vMin >= obb.ly1 || obb.ly0 >= vMax) return false;
    return true;
}

COLFLIP14_LATE COLFLIP14_FLAT bool Colflip14_rectHitsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, bool prec) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;
    // A miss only from right + 1 (bbox.right) on, the rect taken in float32.
    if ((float) x1 >= (float) bbox.right || (float) bbox.left > (float) x2 || (float) y1 >= (float) bbox.bottom || (float) bbox.top > (float) y2) return false;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (prec && Collision_hasFrameMasks(spr)) {
        GMLReal right = bbox.right - 1.0, bottom = bbox.bottom - 1.0;
        int32_t u0 = (int32_t) GMLReal_bankersRound(GMLReal_fmax(x1, bbox.left));
        int32_t u1 = (int32_t) GMLReal_bankersRound(GMLReal_fmin(x2, right));
        int32_t v0 = (int32_t) GMLReal_bankersRound(GMLReal_fmax(y1, bbox.top));
        int32_t v1 = (int32_t) GMLReal_bankersRound(GMLReal_fmin(y2, bottom));
        if (u0 > u1 || v0 > v1) return false;
        Colflip14Point p;
        if (!Colflip14_pointSetup(spr, inst, &p)) return false;
        for (int32_t v = v0; v1 >= v; v++) {
            for (int32_t u = u0; u1 >= u; u++) {
                if (Colflip14_pointHas(&p, inst, (GMLReal) u, (GMLReal) v)) return true;
            }
        }
        return false;
    }
    if (!Collision_obbNeedsSAT(spr, inst)) return true;
    return Colflip14_rectOverlapsOBB(spr, inst, x1, y1, x2, y2);
}

// Cell i is in when (uint32_t) i - lo < span; its mask column (or row) is then (uint32_t) i - lo + add (collision.h's
// ColflipAxis).
typedef struct {
    uint32_t lo, span, add;
} Colflip14Axis;

static inline __attribute__((always_inline)) Colflip14Axis Colflip14_axis(uint32_t size, bool masked, int32_t maskOffset, uint32_t maskSize) {
    int64_t lo = 0, hi = (int32_t) size;
    if (masked) {
        if (maskOffset > lo) lo = maskOffset;
        if ((int64_t) maskOffset + (int32_t) maskSize < hi) hi = (int64_t) maskOffset + (int32_t) maskSize;
    }
    Colflip14Axis axis = { (uint32_t) lo, hi > lo ? (uint32_t) (hi - lo) : 0u, (uint32_t) (lo - (masked ? maskOffset : 0)) };
    return axis;
}

static inline __attribute__((always_inline)) int32_t Colflip14_axisIndex(const Colflip14Axis* axis, uint32_t i) {
    uint32_t u = i - axis->lo;
    return axis->span > u ? (int32_t) (u + axis->add) : -1;
}

// One instance as the GMS 1.4 scan tests it: the position always rounded, the scale's reciprocal rounded to float32,
// and the float32 cosine and sine of -angle (1 and 0 unrotated).
typedef struct {
    const uint8_t* mask; // the frame's collision mask, nullptr for a sprite without masks
    uint32_t rowBytes;
    double ix, iy, rx, ry, ox, oy, cs, sn;
    Colflip14Axis ax, ay;
} Colflip14Shape;

static inline __attribute__((always_inline)) bool Colflip14_shape(Sprite* spr, Instance* inst, Colflip14Shape* out) {
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;
    bool masked = Collision_hasFrameMasks(spr);
    out->mask = masked ? spr->masks[((uint32_t) inst->imageIndex) % spr->maskCount] : nullptr;
    out->rowBytes = (spr->maskWidth + 7) / 8;
    out->ix = GMLReal_bankersRound(inst->x);
    out->iy = GMLReal_bankersRound(inst->y);
    out->rx = (double) (float) Colflip14_div(1.0, (double) inst->imageXscale);
    out->ry = (double) (float) Colflip14_div(1.0, (double) inst->imageYscale);
    out->ox = (double) spr->originX;
    out->oy = (double) spr->originY;
    out->cs = 1.0;
    out->sn = 0.0;
    if ((float) inst->imageAngle != 0.0f) {
        float csf, snf;
        Colflip14_floatTrig(inst->imageAngle, &csf, &snf);
        out->cs = csf;
        out->sn = -(double) snf;
    }
    out->ax = Colflip14_axis(spr->width, masked, spr->maskOffsetX, spr->maskWidth);
    out->ay = Colflip14_axis(spr->height, masked, spr->maskOffsetY, spr->maskHeight);
    return true;
}

// Whether shape s has world pixel (x, y): the offset from its rounded position, rotated, times the reciprocal, plus
// the origin, truncated. Unrotated (cs 1, sn 0) that is trunc(float(d) * float(1 / scale) + origin), the value of
// UNDERTALE.exe's unit and scaled paths.
static inline __attribute__((always_inline)) bool Colflip14_shapeHas(const Colflip14Shape* s, double x, double y) {
    double dx = x - s->ix, dy = y - s->iy;
    int32_t c = Colflip14_axisIndex(&s->ax, (uint32_t) (int32_t) ((dy * s->sn + dx * s->cs) * s->rx + s->ox));
    if (0 > c) return false;
    int32_t r = Colflip14_axisIndex(&s->ay, (uint32_t) (int32_t) ((dy * s->cs - dx * s->sn) * s->ry + s->oy));
    if (0 > r) return false;
    return s->mask == nullptr || Colflip14_maskBit(s->mask, s->rowBytes, c, r);
}

static inline __attribute__((always_inline)) double Colflip14_real64(int64_t x) {
    return x == (int32_t) x ? (double) (int32_t) x : (double) x;
}

// Every pixel of the range (the two bboxes' intersection, both ends inclusive), at its corner.
COLFLIP14_LATE COLFLIP14_FLAT bool Colflip14_scan(const ColflipScan* scan) {
    if (0 >= scan->cols || 0 >= scan->rows) return false;
    Colflip14Shape sa, sb;
    if (!Colflip14_shape(scan->sprA, scan->a, &sa) || !Colflip14_shape(scan->sprB, scan->b, &sb)) return false;
    for (int64_t i = 0; scan->cols > i; i++) {
        double x = Colflip14_real64((int64_t) scan->startX + i);
        for (int64_t j = 0; scan->rows > j; j++) {
            double y = Colflip14_real64((int64_t) scan->startY + j);
            if (Colflip14_shapeHas(&sa, x, y) && Colflip14_shapeHas(&sb, x, y)) return true;
        }
    }
    return false;
}

// Collision_scanPixelsLate's scan (collision.h's Colflip_callScan): GMS 1.4's, or DELTARUNE's unchanged.
COLFLIP14_LATE bool Colflip14_scanPixels(const ColflipScan* scan) {
    if (Colflip_gm14) return Colflip14_scan(scan);
    WAD17_ONLY(return Collision_compatScanPixels(scan);) WAD17_NONE(return Colflip14_scan(scan);)
}

#endif /* COLLISION_GM14_DEFINE */

#endif /* _BS_COLLISION_GM14_H_ */
