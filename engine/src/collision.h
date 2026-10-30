#ifndef _BS_COLLISION_H_
#define _BS_COLLISION_H_

#include "common.h"
#include "data_win.h"
#include "instance.h"
#include "runner.h"
#include "vm.h"

#include "math_compat.h"

#include <string.h>

// Checks if an instance matches a collision target.
// target >= INSTANCE_ID_BASE: instance ID (match specific instance)
// target == INSTANCE_ALL (-3): match any instance
// target >= 0 && < INSTANCE_ID_BASE: object index (match via parent chain)
static inline bool Collision_matchesTarget(DataWin* dataWin, Instance* inst, int32_t target) {
    if (target >= INSTANCE_ID_BASE) return inst->instanceId == (uint32_t) target;
    if (target == INSTANCE_ALL) return true;
    return VM_isObjectOrDescendant(dataWin, inst->objectIndex, target);
}

typedef struct {
    GMLReal left, right, top, bottom;
    bool valid;
} InstanceBBox;

#if defined(ENABLE_WAD16)
__attribute__((pure)) InstanceBBox Colexact_unrotatedBBox(Sprite* spr, Instance* inst);
InstanceBBox Colexact_sharedBBox(Runner* runner, Instance* inst);
#endif
#line 28

// Returns the collision sprite for an instance (mask sprite if set, else display sprite)
static inline Sprite* Collision_getSprite(DataWin* dataWin, Instance* inst) {
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;
    if (0 > sprIdx || (uint32_t) sprIdx >= dataWin->sprt.count) return nullptr;
    return &dataWin->sprt.sprites[sprIdx];
}

// The cosine and sine of an image_angle (degrees), exactly as GMLReal_cos and
// GMLReal_sin give them for its radians. Bullet hells keep hundreds of
// rotating instances, each needing them for its bounding box once or twice a
// frame (Outside Room, then the collision pass), and their angles step by
// whole degrees, so the same few values come round again and again: a slot
// keyed on the angle's exact bits keeps the pair. UNDERTALE's Photoshop
// Flowey ends with about 300 such instances: by frame 2700 of its sweep it
// fills 975 of the 1024 slots, with 404,422 hits to 51,716 misses (89%). A
// miss costs what the pair cost before plus the slot write; an instance
// that isn't rotated (|image_angle| <= 0.0001) never comes here, so scenes
// without rotation pay nothing but the table's 32 KB.
#define COLLISION_TRIG_SLOTS 1024
typedef struct {
    GMLReal angle;
    GMLReal cs, sn;
    bool filled;
} CollisionTrigSlot;
extern CollisionTrigSlot Collision_trigSlots[COLLISION_TRIG_SLOTS];

static inline void Collision_angleTrig(GMLReal angle, GMLReal* cs, GMLReal* sn) {
    uint64_t bits = 0;
    memcpy(&bits, &angle, sizeof(angle));
    CollisionTrigSlot* slot = &Collision_trigSlots[(bits * 0x9E3779B97F4A7C15ull) >> 54];
    // The bits, not ==: -0 and 0 are equal but their sines aren't.
    if (!slot->filled || memcmp(&slot->angle, &angle, sizeof(angle)) != 0) {
        GMLReal rad = angle * M_PI / 180.0;
        slot->cs = GMLReal_cos(rad);
        slot->sn = GMLReal_sin(rad);
        slot->angle = angle;
        slot->filled = true;
    }
    *cs = slot->cs;
    *sn = slot->sn;
}

// Collision_computeBBox's edge rounding (simplefix, 2026-10-28). Where CTRPORT_BBOX_ROUND is defined (runner.c and
// vm_builtins.c on the 3DS, CMakeLists.txt), z_bboxround.c's Collision_bboxRound gives GMLReal_bankersRound's bits
// without newlib's floor, and Collision_computeBBox stays out of line there, as GCC already emitted it (its callers
// compile as before; the slot is kept at each file's end). Everywhere else nothing changes.
#if defined(__3DS__) && !defined(USE_FLOAT_REALS) && defined(CTRPORT_BBOX_ROUND)
__attribute__((const)) GMLReal Collision_bboxRound(GMLReal v);
#define COLLISION_BBOX_STORAGE static __attribute__((noinline, unused))
#else
#define Collision_bboxRound GMLReal_bankersRound
#define COLLISION_BBOX_STORAGE static inline
#endif

#if defined(ENABLE_WAD16) && defined(__3DS__) && !defined(USE_FLOAT_REALS) && defined(CTRPORT_BBOX_ROUND)
#include "colexact_bbox.h"
#endif
#line 83
// Computes the axis-aligned bounding box for an instance using its collision sprite
COLLISION_BBOX_STORAGE InstanceBBox Collision_computeBBox(Runner* runner, Instance* inst) {
    InstanceBBox ret;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (spr == nullptr) {
        ZERO_STRUCT(ret);
        return ret;
    }

#if defined(ENABLE_WAD16)
    if (runner->collisionCompatibilityMode && runner->dataWin->gen8.wadVersion <= 16 && (float) inst->imageAngle == 0.0f) {
#if defined(__3DS__) && !defined(USE_FLOAT_REALS) && defined(CTRPORT_BBOX_ROUND)
        return Colexact_inlineBBox(spr, inst);
#else
        return Colexact_unrotatedBBox(spr, inst);
#endif
    }
#endif
#line 91

    GMLReal marginL = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginLeft;
    GMLReal marginR = (spr->bboxMode == 1) ? (GMLReal) spr->width : (GMLReal) (spr->marginRight + 1);
    GMLReal marginT = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginTop;
    GMLReal marginB = (spr->bboxMode == 1) ? (GMLReal) spr->height : (GMLReal) (spr->marginBottom + 1);
    GMLReal originX = (GMLReal) spr->originX;
    GMLReal originY = (GMLReal) spr->originY;

    GMLReal left, right, top, bottom;
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        // Compute rotated AABB: transform the 4 corners of the unrotated bbox
        GMLReal cs, sn;
        Collision_angleTrig(inst->imageAngle, &cs, &sn);

        // Local-space corners relative to origin, scaled
        GMLReal lx0 = inst->imageXscale * (marginL - originX);
        GMLReal ly0 = inst->imageYscale * (marginT - originY);
        GMLReal lx1 = inst->imageXscale * (marginR - originX);
        GMLReal ly1 = inst->imageYscale * (marginB - originY);

        // Rotate all 4 corners (CW rotation matching renderer's negated angle for Y-down screen coords)
        GMLReal cx[4], cy[4];
        cx[0] = cs * lx0 + sn * ly0;  cy[0] = -sn * lx0 + cs * ly0;
        cx[1] = cs * lx1 + sn * ly0;  cy[1] = -sn * lx1 + cs * ly0;
        cx[2] = cs * lx0 + sn * ly1;  cy[2] = -sn * lx0 + cs * ly1;
        cx[3] = cs * lx1 + sn * ly1;  cy[3] = -sn * lx1 + cs * ly1;

        GMLReal minX = cx[0], maxX = cx[0], minY = cy[0], maxY = cy[0];
        for (int c = 1; 4 > c; c++) {
            if (minX > cx[c]) minX = cx[c];
            if (cx[c] > maxX) maxX = cx[c];
            if (minY > cy[c]) minY = cy[c];
            if (cy[c] > maxY) maxY = cy[c];
        }

        left   = inst->x + minX;
        right  = inst->x + maxX;
        top    = inst->y + minY;
        bottom = inst->y + maxY;
    } else {
        // No rotation fast path
        left   = inst->x + inst->imageXscale * (marginL - originX);
        right  = inst->x + inst->imageXscale * (marginR - originX);
        top    = inst->y + inst->imageYscale * (marginT - originY);
        bottom = inst->y + inst->imageYscale * (marginB - originY);

        // Normalize if negative scale
        if (left > right) { GMLReal tmp = left; left = right; right = tmp; }
        if (top > bottom) { GMLReal tmp = top; top = bottom; bottom = tmp; }
    }

    if (runner->collisionCompatibilityMode) {
        left   = Collision_bboxRound(left);
        top    = Collision_bboxRound(top);
        right  = Collision_bboxRound(right);
        bottom = Collision_bboxRound(bottom);
    }

    ret.left = left;
    ret.right = right;
    ret.top = top;
    ret.bottom = bottom;
    ret.valid = true;
    return ret;
}

static inline bool Collision_hasFrameMasks(Sprite* sprite) {
    return sprite != nullptr && sprite->sepMasks == 1 && sprite->masks != nullptr && sprite->maskCount > 0;
}

// Oriented bounding box for a sprite-bearing instance.
// Local rect is the sprite's collision-margin rectangle, scaled by image_xscale/yscale; (cs, sn) is the rotation that takes local-space points to world-space relative to the instance origin (matches Collision_computeBBox: world = inst.pos + (cs*lx + sn*ly, -sn*lx + cs*ly)).
typedef struct {
    GMLReal x, y; // World position (instance origin).
    GMLReal lx0, lx1; // Local rect X extents (lx0 <= lx1).
    GMLReal ly0, ly1; // Local rect Y extents (ly0 <= ly1).
    GMLReal cs, sn; // cos/sin of imageAngle (in radians).
    bool rotated; // true if abs(imageAngle) > epsilon.
} InstanceOBB;

static inline InstanceOBB Collision_instanceOBB(Sprite* spr, Instance* inst) {
    InstanceOBB obb;
    obb.x = inst->x;
    obb.y = inst->y;
    GMLReal marginL = spr->bboxMode == 1 ? 0.0 : (GMLReal) spr->marginLeft;
    GMLReal marginR = spr->bboxMode == 1 ? (GMLReal) spr->width : (GMLReal) (spr->marginRight + 1);
    GMLReal marginT = spr->bboxMode == 1 ? 0.0 : (GMLReal) spr->marginTop;
    GMLReal marginB = spr->bboxMode == 1 ? (GMLReal) spr->height : (GMLReal) (spr->marginBottom + 1);
    GMLReal originX = (GMLReal) spr->originX;
    GMLReal originY = (GMLReal) spr->originY;
    obb.lx0 = inst->imageXscale * (marginL - originX);
    obb.lx1 = inst->imageXscale * (marginR - originX);
    obb.ly0 = inst->imageYscale * (marginT - originY);
    obb.ly1 = inst->imageYscale * (marginB - originY);
    if (obb.lx0 > obb.lx1) { GMLReal t = obb.lx0; obb.lx0 = obb.lx1; obb.lx1 = t; }
    if (obb.ly0 > obb.ly1) { GMLReal t = obb.ly0; obb.ly0 = obb.ly1; obb.ly1 = t; }
    obb.rotated = GMLReal_fabs(inst->imageAngle) > 0.0001;
    if (obb.rotated) {
        Collision_angleTrig(inst->imageAngle, &obb.cs, &obb.sn);
    } else {
        obb.cs = 1.0;
        obb.sn = 0.0;
    }
    return obb;
}

// Inverse-transforms a world point into OBB local coordinates.
static inline void Collision_obbWorldToLocal(const InstanceOBB* obb, GMLReal wx, GMLReal wy, GMLReal* outLx, GMLReal* outLy) {
    GMLReal dx = wx - obb->x;
    GMLReal dy = wy - obb->y;
    *outLx = dx * obb->cs - dy * obb->sn;
    *outLy = dx * obb->sn + dy * obb->cs;
}

// Returns true iff the OBB needs SAT-style testing rather than AABB. Only sepMasks == 2 sprites that are actually rotated qualify; everything else (axis-aligned, or precise sprites which fall through to per-pixel scans) is handled correctly by AABB.
static inline bool Collision_obbNeedsSAT(Sprite* spr, Instance* inst) {
    return spr != nullptr && spr->sepMasks == 2 && GMLReal_fabs(inst->imageAngle) > 0.0001;
}

static inline bool Collision_rectOverlapsInstance(Runner* runner, Instance* inst, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;

    if (rx1 >= bbox.right || bbox.left > rx2 || ry1 >= bbox.bottom || bbox.top > ry2) return false;

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) return true;

    // OBB-vs-AABB SAT for sepMasks==2 with rotation. Native uses SeparatingAxisCollisionBox here.
    InstanceOBB obb = Collision_instanceOBB(spr, inst);

    // Project the 4 world-rect corners onto the OBB's local axes; if they don't overlap the local rect on either axis, no collision.
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

// Tests whether a world point lies inside the instance's collision rect (margins, rotated/scaled). Cheaper and more correct than Collision_pointInInstance for sepMasks != 1, since point_in_instance bounds-checks against the full sprite texture rather than the bbox margins.
static inline bool Collision_pointInsideInstanceBox(Runner* runner, Instance* inst, GMLReal px, GMLReal py) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;
    if (bbox.left > px || px >= bbox.right || bbox.top > py || py >= bbox.bottom) return false;

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) return true;

    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lx, ly;
    Collision_obbWorldToLocal(&obb, px, py, &lx, &ly);
    return lx >= obb.lx0 && obb.lx1 > lx && ly >= obb.ly0 && obb.ly1 > ly;
}

// Circle (cx, cy, radius) vs instance collision rect. Falls back to circle-vs-AABB when the instance isn't a rotated sepMasks==2 sprite.
static inline bool Collision_circleOverlapsInstance(Runner* runner, Instance* inst, GMLReal cx, GMLReal cy, GMLReal radius) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;
    GMLReal rSq = radius * radius;

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) {
        // Closest point on AABB to circle center.
        GMLReal closestX = cx;
        if (bbox.left > closestX) closestX = bbox.left;
        if (closestX > bbox.right) closestX = bbox.right;
        GMLReal closestY = cy;
        if (bbox.top > closestY) closestY = bbox.top;
        if (closestY > bbox.bottom) closestY = bbox.bottom;
        GMLReal dx = closestX - cx, dy = closestY - cy;
        return dx * dx + dy * dy <= rSq;
    }

    // Loose AABB pre-pass.
    GMLReal qx1 = cx - radius, qy1 = cy - radius, qx2 = cx + radius, qy2 = cy + radius;
    if (qx1 >= bbox.right || bbox.left >= qx2 || qy1 >= bbox.bottom || bbox.top >= qy2) return false;

    // Transform circle center into OBB local frame; clamp to local rect; squared distance.
    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lx, ly;
    Collision_obbWorldToLocal(&obb, cx, cy, &lx, &ly);
    GMLReal closestX = lx;
    if (obb.lx0 > closestX) closestX = obb.lx0;
    if (closestX > obb.lx1) closestX = obb.lx1;
    GMLReal closestY = ly;
    if (obb.ly0 > closestY) closestY = obb.ly0;
    if (closestY > obb.ly1) closestY = obb.ly1;
    GMLReal dx = closestX - lx, dy = closestY - ly;
    return dx * dx + dy * dy <= rSq;
}

// Tests whether world point (px, py) lies inside the ellipse defined by opposite corners (x1,y1)-(x2,y2).
static inline bool Collision_pointInEllipse(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, GMLReal px, GMLReal py) {
    GMLReal cx = (x1 + x2) * 0.5;
    GMLReal cy = (y1 + y2) * 0.5;
    GMLReal rx = (x2 - x1) * 0.5;
    GMLReal ry = (y2 - y1) * 0.5;
    GMLReal a = (px - cx) / rx;
    GMLReal b = (py - cy) / ry;
    return a * a + b * b <= 1.0;
}

// Ellipse (defined by opposite corners) vs instance collision shape.
// Mirrors GameMaker-HTML5's yyInstance.Collision_Ellipse flow: AABB rejection, degenerate
// (zero width/height) fallback to the rectangle test, nearest-corner PtInEllipse rejection,
// and ellipse-vs-OBB SAT for rotated sepMasks==2 sprites.
static inline bool Collision_ellipseOverlapsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;

    // Native rounds the ellipse corners in compatibility mode, before computing the centre/radii.
    if (runner->collisionCompatibilityMode) {
        x1 = GMLReal_bankersRound(x1);
        y1 = GMLReal_bankersRound(y1);
        x2 = GMLReal_bankersRound(x2);
        y2 = GMLReal_bankersRound(y2);
    }

    GMLReal minX = GMLReal_fmin(x1, x2);
    GMLReal maxX = GMLReal_fmax(x1, x2);
    GMLReal minY = GMLReal_fmin(y1, y2);
    GMLReal maxY = GMLReal_fmax(y1, y2);

    // Reject when the ellipse box and the instance bbox are separated.
    if (minX >= bbox.right || maxX < bbox.left || minY >= bbox.bottom || maxY < bbox.top) return false;

    // Degenerate ellipse (a line)
    if (minX == maxX || minY == maxY) return Collision_rectOverlapsInstance(runner, inst, minX, minY, maxX, maxY);

    GMLReal cx = (x1 + x2) * 0.5;
    GMLReal cy = (y1 + y2) * 0.5;
    GMLReal rx = (x2 - x1) * 0.5;
    GMLReal ry = (y2 - y1) * 0.5;

    if (!(bbox.left <= cx && bbox.right >= cx) &&
        !(bbox.top <= cy && bbox.bottom >= cy)) {
        GMLReal px = (bbox.right <= cx) ? bbox.right : bbox.left;
        GMLReal py = (bbox.bottom <= cy) ? bbox.bottom : bbox.top;
        if (!Collision_pointInEllipse(x1, y1, x2, y2, px, py)) return false;
    }

    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) return true;

    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lcx, lcy;
    Collision_obbWorldToLocal(&obb, cx, cy, &lcx, &lcy);

    GMLReal axes[4][2] = {
        { 1.0, 0.0 },
        { 0.0, 1.0 },
        { obb.cs, obb.sn },
        { -obb.sn, obb.cs }
    };
    GMLReal corners[4][2] = {
        { obb.lx0, obb.ly0 }, { obb.lx1, obb.ly0 },
        { obb.lx0, obb.ly1 }, { obb.lx1, obb.ly1 }
    };

    repeat(4, axIdx) {
        GMLReal nx = axes[axIdx][0], ny = axes[axIdx][1];
        GMLReal rMin = corners[0][0] * nx + corners[0][1] * ny;
        GMLReal rMax = rMin;
        {
        for (int c = 1; 4 > c; c++) {
            GMLReal p = corners[c][0] * nx + corners[c][1] * ny;
            if (p < rMin) rMin = p;
            else if (p > rMax) rMax = p;
        }
        }
        GMLReal du = nx * obb.cs + ny * obb.sn;
        GMLReal dv = -nx * obb.sn + ny * obb.cs;
        GMLReal halfLen = GMLReal_sqrt(rx * rx * du * du + ry * ry * dv * dv);
        GMLReal c = lcx * nx + lcy * ny;
        if (c + halfLen <= rMin || c - halfLen >= rMax) return false;
    }
    return true;
}

// Liang-Barsky clip of a parametric line p(t) = p1 + t*(p2-p1), t in [0,1], against an axis-aligned rect [rx1,rx2] x [ry1,ry2].
// Returns true if the segment intersects the rect, and writes the clipped parametric range to *outTEnter/*outTExit.
static inline bool Collision_segmentVsAARectClip(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2, GMLReal* outTEnter, GMLReal* outTExit) {
    GMLReal tEnter = 0.0, tExit = 1.0;
    GMLReal dx = x2 - x1, dy = y2 - y1;
    GMLReal p[4] = { -dx, dx, -dy, dy };
    GMLReal q[4] = { x1 - rx1, rx2 - x1, y1 - ry1, ry2 - y1 };
    for (int i = 0; 4 > i; i++) {
        if (GMLReal_fabs(p[i]) < 1e-9) {
            if (q[i] < 0) return false;
            continue;
        }
        GMLReal t = q[i] / p[i];
        if (p[i] < 0) {
            if (t > tEnter) tEnter = t;
        } else {
            if (t < tExit) tExit = t;
        }
        if (tEnter > tExit) return false;
    }
    *outTEnter = tEnter;
    *outTExit = tExit;
    return true;
}

static inline bool Collision_segmentVsAARect(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2) {
    GMLReal tEnter, tExit;
    return Collision_segmentVsAARectClip(x1, y1, x2, y2, rx1, ry1, rx2, ry2, &tEnter, &tExit);
}

// Line segment (x1,y1)-(x2,y2) vs instance collision rect.
static inline bool Collision_lineOverlapsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;

    // Epsilon from GameMaker-HTML5, we apply it because the BBox is "exclusive" (outside of the sprite) but we need to put the right/bottom INSIDE of the sprite
    // So we nudge it to be inside on it
    GMLReal eps = 1e-5;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (!Collision_obbNeedsSAT(spr, inst)) {
        return Collision_segmentVsAARect(x1, y1, x2, y2, bbox.left, bbox.top, bbox.right - eps, bbox.bottom - eps);
    }

    // For rotated OBB: transform line endpoints into local frame and clip against local rect.
    InstanceOBB obb = Collision_instanceOBB(spr, inst);
    GMLReal lx1, ly1, lx2, ly2;
    Collision_obbWorldToLocal(&obb, x1, y1, &lx1, &ly1);
    Collision_obbWorldToLocal(&obb, x2, y2, &lx2, &ly2);
    return Collision_segmentVsAARect(lx1, ly1, lx2, ly2, obb.lx0, obb.ly0, obb.lx1 - eps, obb.ly1 - eps);
}

// Tests if world point (px, py) is inside the given instance's collision shape.
// The point is inverse-transformed into sprite-local coords (translation, rotation, inverse scale, origin) and bounds-checked against the full sprite texture [0, spr.width) x [0, spr.height).
// Precise sprites (sepMasks == 1) additionally require the mask bit at the resulting local pixel to be set.
static inline bool Collision_pointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py) {
    if (spr == nullptr) return false;

    // Reject degenerate scales to avoid divide-by-zero.
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;

    // Transform world coords to sprite-local coords
    GMLReal dx = px - inst->x;
    GMLReal dy = py - inst->y;

    // Inverse of CW rotation is standard CCW rotation (positive angle)
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        GMLReal cs, sn;
        Collision_angleTrig(inst->imageAngle, &cs, &sn);
        GMLReal rx = cs * dx - sn * dy;
        GMLReal ry = sn * dx + cs * dy;
        dx = rx;
        dy = ry;
    }

    // Inverse scale + add origin
    GMLReal localX = dx / inst->imageXscale + (GMLReal) spr->originX;
    GMLReal localY = dy / inst->imageYscale + (GMLReal) spr->originY;

    int32_t ix = (int32_t) localX;
    int32_t iy = (int32_t) localY;

    // Bounds check
    if (0 > ix || 0 > iy || ix >= (int32_t) spr->width || iy >= (int32_t) spr->height) return false;

    if (Collision_hasFrameMasks(spr)) {
        // Pick mask for current frame
        uint32_t frameIdx = ((uint32_t) inst->imageIndex) % spr->maskCount;
        uint8_t* mask = spr->masks[frameIdx];
        // Masks are stored at maskWidth x maskHeight starting at (maskOffsetX, maskOffsetY) in sprite-local space.
        // Pre-2024.6 this is the full sprite with zero offset; 2024.6+ it is the bounding box.
        int32_t mx = ix - spr->maskOffsetX;
        int32_t my = iy - spr->maskOffsetY;
        if (0 > mx || 0 > my || mx >= (int32_t) spr->maskWidth || my >= (int32_t) spr->maskHeight) return false;
        uint32_t bytesPerRow = (spr->maskWidth + 7) / 8;
        return (mask[my * bytesPerRow + (mx >> 3)] & (1 << (7 - (mx & 7)))) != 0;
    }

    return true;
}

// One instance as Collision_pointInInstance tests it, worked out once for a whole pixel scan.
typedef struct {
    Sprite* spr;
    const uint8_t* mask; // the frame's collision mask, nullptr for a sprite without masks
    uint32_t rowBytes;
    GMLReal x, y, xscale, yscale;
    GMLReal cs, sn; // the rotation, when rotated
    bool rotated;
} CollisionScanShape;

// Fills *out for inst. False when Collision_pointInInstance fails every point for it: a (near) zero scale.
static inline bool Collision_scanShape(Sprite* spr, Instance* inst, CollisionScanShape* out) {
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;
    out->spr = spr;
    out->mask = Collision_hasFrameMasks(spr) ? spr->masks[((uint32_t) inst->imageIndex) % spr->maskCount] : nullptr;
    out->rowBytes = (spr->maskWidth + 7) / 8;
    out->x = inst->x;
    out->y = inst->y;
    out->xscale = inst->imageXscale;
    out->yscale = inst->imageYscale;
    out->rotated = GMLReal_fabs(inst->imageAngle) > 0.0001;
    out->cs = 1.0;
    out->sn = 0.0;
    if (out->rotated) {
        Collision_angleTrig(inst->imageAngle, &out->cs, &out->sn);
    }
    return true;
}

// Collision_pointInInstance along one axis of an unrotated shape: the mask column (or row) that world coordinate w
// falls in (the sprite's, for a sprite without masks), or -1 when the point is outside. pos, scale, origin and size are
// the instance's and sprite's along that axis, maskOffset and maskSize the mask's.
static inline int32_t Collision_scanCell(GMLReal w, GMLReal pos, GMLReal scale, int32_t origin, uint32_t size, bool masked, int32_t maskOffset,
                                         uint32_t maskSize) {
    GMLReal d = w - pos;
    GMLReal local = d / scale + (GMLReal) origin;
    int32_t i = (int32_t) local;
    if (0 > i || i >= (int32_t) size) return -1;
    if (!masked) return i;
    int32_t m = i - maskOffset;
    if (0 > m || m >= (int32_t) maskSize) return -1;
    return m;
}

// Collision_pointInInstance for a rotated shape, with its rotation worked out once.
static inline bool Collision_scanShapeHas(const CollisionScanShape* s, GMLReal px, GMLReal py) {
    Sprite* spr = s->spr;
    GMLReal dx = px - s->x;
    GMLReal dy = py - s->y;
    GMLReal rx = s->cs * dx - s->sn * dy;
    GMLReal ry = s->sn * dx + s->cs * dy;
    GMLReal localX = rx / s->xscale + (GMLReal) spr->originX;
    GMLReal localY = ry / s->yscale + (GMLReal) spr->originY;
    int32_t ix = (int32_t) localX;
    int32_t iy = (int32_t) localY;
    if (0 > ix || 0 > iy || ix >= (int32_t) spr->width || iy >= (int32_t) spr->height) return false;
    if (s->mask == nullptr) return true;
    int32_t mx = ix - spr->maskOffsetX;
    int32_t my = iy - spr->maskOffsetY;
    if (0 > mx || 0 > my || mx >= (int32_t) spr->maskWidth || my >= (int32_t) spr->maskHeight) return false;
    return (s->mask[(uint32_t) my * s->rowBytes + (uint32_t) (mx >> 3)] & (1 << (7 - (mx & 7)))) != 0;
}

// Columns Collision_scanPixels works out at a time.
#define COLLISION_SCAN_CHUNK 64

// Whether any pixel of cols x rows from (startX, startY), sampled at +offset, is in both instances' shapes, as
// Collision_pointInInstance tests each: the pixel scan of Collision_instancesOverlapPrecise. An unrotated shape's
// mask column depends only on a pixel's x and its row only on its y, so those are worked out once per column (a
// chunk of columns at a time) and once per row, leaving only the mask bits to test per pixel; a rotated shape's sine
// and cosine are worked out once. The same arithmetic as Collision_pointInInstance, so the same pixels hit, and which
// pixel hits first doesn't change the answer.
MAYBE_UNUSED static NOINLINE bool Collision_scanPixels(Sprite* sprA, Instance* a, Sprite* sprB, Instance* b, int32_t startX, int64_t cols, int32_t startY,
                                                       int64_t rows, GMLReal offset) {
    CollisionScanShape sa, sb;
    if (!Collision_scanShape(sprA, a, &sa) || !Collision_scanShape(sprB, b, &sb)) return false;
    bool maskedA = sa.mask != nullptr, maskedB = sb.mask != nullptr;
    GMLReal wx[COLLISION_SCAN_CHUNK];
    int32_t colA[COLLISION_SCAN_CHUNK], colB[COLLISION_SCAN_CHUNK];
    for (int64_t c0 = 0; cols > c0; c0 += COLLISION_SCAN_CHUNK) {
        int32_t n = cols - c0 > COLLISION_SCAN_CHUNK ? COLLISION_SCAN_CHUNK : (int32_t) (cols - c0);
        bool anyColumn = false;
        repeat(n, i) {
            wx[i] = (GMLReal) ((int64_t) startX + c0 + i) + offset;
            colA[i] = sa.rotated ? 0 : Collision_scanCell(wx[i], sa.x, sa.xscale, sprA->originX, sprA->width, maskedA, sprA->maskOffsetX, sprA->maskWidth);
            colB[i] = sb.rotated ? 0 : Collision_scanCell(wx[i], sb.x, sb.xscale, sprB->originX, sprB->width, maskedB, sprB->maskOffsetX, sprB->maskWidth);
            if (colA[i] >= 0 && colB[i] >= 0) anyColumn = true;
        }
        if (!anyColumn) continue;
        repeat(rows, j) {
            GMLReal wy = (GMLReal) ((int64_t) startY + j) + offset;
            const uint8_t* lineA = nullptr;
            const uint8_t* lineB = nullptr;
            if (!sa.rotated) {
                int32_t row = Collision_scanCell(wy, sa.y, sa.yscale, sprA->originY, sprA->height, maskedA, sprA->maskOffsetY, sprA->maskHeight);
                if (0 > row) continue;
                if (maskedA) lineA = sa.mask + (uint32_t) row * sa.rowBytes;
            }
            if (!sb.rotated) {
                int32_t row = Collision_scanCell(wy, sb.y, sb.yscale, sprB->originY, sprB->height, maskedB, sprB->maskOffsetY, sprB->maskHeight);
                if (0 > row) continue;
                if (maskedB) lineB = sb.mask + (uint32_t) row * sb.rowBytes;
            }
            repeat(n, i) {
                int32_t ca = colA[i], cb = colB[i];
                if (0 > ca || 0 > cb) continue;
                if (lineA != nullptr && (lineA[ca >> 3] & (1 << (7 - (ca & 7)))) == 0) continue;
                if (lineB != nullptr && (lineB[cb >> 3] & (1 << (7 - (cb & 7)))) == 0) continue;
                if (sa.rotated && !Collision_scanShapeHas(&sa, wx[i], wy)) continue;
                if (sb.rotated && !Collision_scanShapeHas(&sb, wx[i], wy)) continue;
                return true;
            }
        }
    }
    return false;
}

// ===[ Compatibility-mode precise collisions (colflip) ]===
//
// GameMaker's compatibility-mode rule, measured against the official runner (docs/vm-warnings.md, "Precise collisions
// in compatibility mode"). These paths live in late code (platform/3ds/late_reserve.ld, .text.late.colflip) so that
// the warm text keeps its addresses; the modern-mode paths above are unchanged.
// - Collision_instancesOverlapPrecise reaches the scan through Collision_scanPixelsLate, a call of the same size as
//   the Collision_scanPixels call it replaced.
// - collision_point, instance_position, position_meeting, collision_rectangle and collision_rectangle_list get late
//   builtins (collision_compat.c), registered over the originals, which still answer modern mode.
// The functions are defined once, in the translation unit that defines COLLISION_COMPAT_DEFINE (collision_compat.c,
// all of it late code). Defining them in an existing file, even unused, reorders GCC's output of that file's functions.
#define COLFLIP_LATE __attribute__((noinline, section(".text.late.colflip")))
// Inlines every call the function makes (except noinline functions), so that no out-of-line copy of a header
// function such as Collision_computeBBox lands outside the late section.
#define COLFLIP_FLAT __attribute__((flatten))

// collision_point's test of one instance: the bbox with the point unrounded, then, for a precise sprite when prec, the
// mask: the point and the instance's position rounded half to even, the pixel's centre sampled, the cell floored.
bool Collision_compatPointHits(Runner* runner, Instance* inst, GMLReal px, GMLReal py, bool prec);
// The mask half of Collision_compatPointHits (spr has frame masks).
bool Collision_compatPointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py);
// collision_rectangle's test of one instance. (x1, y1)-(x2, y2) is the caller's rect, normalised but not rounded. It
// misses when it lies wholly outside the bbox taken with an inclusive right and bottom edge (right - 1, bottom - 1).
// A precise test then walks the integer pixels from the rounded (half to even) clipped left/top to the rounded clipped
// right/bottom, both inclusive, and tests each as collision_point does.
bool Collision_compatRectHitsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, bool prec);
// The precise instance-against-instance scan: cols x rows pixels from (startX, startY), upper bound exclusive, each
// sampled at its corner (+1 on an axis where that instance's scale is negative). Neither instance rotated: the cell
// is truncated, and the positions are rounded half to even only when all four scales are 1. Either rotated: both go
// GameMaker's general way, positions unrounded, trig and arithmetic in float32, the cell floored. One pointer argument,
// so that Collision_scanPixelsLate can call it from inline asm (Colflip_callScan).
typedef struct {
    Sprite* sprA;
    Instance* a;
    Sprite* sprB;
    Instance* b;
    int32_t startX, startY;
    int64_t cols, rows;
} ColflipScan;
bool Collision_compatScanPixels(const ColflipScan* scan);

#ifdef COLLISION_COMPAT_DEFINE

// x / y, kept a division. Under -ffast-math (-freciprocal-math, which the engine is built with) GCC turns a division by
// a loop-invariant y into a multiplication by 1 / y worked out before the loop, which rounds differently and moves
// cells. The volatile empty asm gives y a new value on every use, so each division stays one (and costs no
// instruction).
static inline __attribute__((always_inline)) double Colflip_div(double x, double y) {
#if defined(__arm__)
    __asm__ volatile("" : "+w"(y));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(y));
#else
    __asm__ volatile("" : "+m"(y));
#endif
    return x / y;
}

// x / y in float32, kept a division (Colflip_div).
static inline __attribute__((always_inline)) float Colflip_divf(float x, float y) {
#if defined(__arm__)
    __asm__ volatile("" : "+w"(y));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(y));
#else
    __asm__ volatile("" : "+m"(y));
#endif
    return x / y;
}

// (int32_t) floorf(x) and (int32_t) floor(x) without the library call: ARMv6's VFP has no rounding instruction, so
// floorf and floor are calls, two for each pixel of a rotated shape. Truncate, then step down when that went up. Exact
// wherever the library form is defined (|x| < 2^31; (float) i is exact there), and the same as its conversion of a
// NaN or out-of-range value (the conversion's own result, kept at INT32_MIN).
static inline __attribute__((always_inline)) int32_t Colflip_floorf(float x) {
    int32_t i = (int32_t) x;
    return (float) i > x && i != INT32_MIN ? i - 1 : i;
}

static inline __attribute__((always_inline)) int32_t Colflip_floor(double x) {
    int32_t i = (int32_t) x;
    return (double) i > x && i != INT32_MIN ? i - 1 : i;
}

// A scale to divide by. A power of two's reciprocal is exact, and x / y and x * (1 / y) are then the same number, so
// they round the same (to double, and on to float): the multiplication replaces a double division, which ARMv6's VFP
// takes about 30 cycles for. Any other scale is divided by.
//
// The float32 forms (Colflip_divByF) take a float x: GameMaker divides it by the scale in double and rounds the
// quotient to float. The scale is a float (Instance's imageXscale), and for two floats that is the same number as
// the float division x / y, as double has more than twice float's precision plus two bits (a double rounding of +, -,
// * and / is then innocuous); the float division takes half the cycles and no conversions.
typedef struct {
    double y, inverse;
    float yf, inversef;
    bool powerOfTwo;
} ColflipDivisor;

static inline __attribute__((always_inline)) ColflipDivisor Colflip_divisor(double y) {
    uint64_t bits = 0;
    memcpy(&bits, &y, sizeof(y));
    uint32_t exponent = (uint32_t) (bits >> 52) & 0x7ff;
    ColflipDivisor d = { y, 1.0, (float) y, 1.0f, false };
    // A normal number with an all-zero mantissa, whose reciprocal is normal too (in float as well: a scale is at least
    // 0.0001 and at most a float).
    if ((bits & 0xfffffffffffffull) == 0 && exponent > 1 && 0x7fe > exponent) {
        d.powerOfTwo = true;
        d.inverse = 1.0 / y;
        d.inversef = (float) d.inverse;
    }
    return d;
}

static inline __attribute__((always_inline)) double Colflip_divBy(double x, const ColflipDivisor* d) {
    return d->powerOfTwo ? x * d->inverse : Colflip_div(x, d->y);
}

static inline __attribute__((always_inline)) float Colflip_divByF(float x, const ColflipDivisor* d) {
    return d->powerOfTwo ? x * d->inversef : Colflip_divf(x, d->yf);
}

// The mask column (or row) of sprite-local cell i, or -1 outside the sprite or (masked) its mask.
static inline __attribute__((always_inline)) int32_t Colflip_index(int32_t i, uint32_t size, bool masked, int32_t maskOffset, uint32_t maskSize) {
    if (0 > i || i >= (int32_t) size) return -1;
    if (!masked) return i;
    int32_t m = i - maskOffset;
    if (0 > m || m >= (int32_t) maskSize) return -1;
    return m;
}

static inline __attribute__((always_inline)) bool Colflip_maskBit(const uint8_t* mask, uint32_t rowBytes, int32_t c, int32_t r) {
    return (mask[(uint32_t) r * rowBytes + (uint32_t) (c >> 3)] & (1 << (7 - (c & 7)))) != 0;
}

// Colflip_index with its limits worked out once: cell i is in when (uint32_t) i - lo < span, and its mask column (or
// row) is then (uint32_t) i - lo + add. [lo, lo + span) is [0, size) and, masked, [maskOffset, maskOffset + maskSize)
// intersected, with the sizes taken as int32_t as Colflip_index compares them, so the same cells are in and they get
// the same columns, with one comparison.
typedef struct {
    uint32_t lo, span, add;
} ColflipAxis;

static inline __attribute__((always_inline)) ColflipAxis Colflip_axis(uint32_t size, bool masked, int32_t maskOffset, uint32_t maskSize) {
    int64_t lo = 0, hi = (int32_t) size;
    if (masked) {
        if (maskOffset > lo) lo = maskOffset;
        if ((int64_t) maskOffset + (int32_t) maskSize < hi) hi = (int64_t) maskOffset + (int32_t) maskSize;
    }
    ColflipAxis axis = { (uint32_t) lo, hi > lo ? (uint32_t) (hi - lo) : 0u, (uint32_t) (lo - (masked ? maskOffset : 0)) };
    return axis;
}

static inline __attribute__((always_inline)) int32_t Colflip_axisIndex(const ColflipAxis* axis, uint32_t i) {
    uint32_t u = i - axis->lo;
    return axis->span > u ? (int32_t) (u + axis->add) : -1;
}

// (GMLReal) x for an int64_t x, through int32_t when it fits: the same value, without the library's 64-bit conversion.
static inline __attribute__((always_inline)) GMLReal Colflip_real64(int64_t x) {
    return x == (int32_t) x ? (GMLReal) (int32_t) x : (GMLReal) x;
}

COLFLIP_LATE COLFLIP_FLAT bool Collision_compatPointInInstance(Sprite* spr, Instance* inst, GMLReal px, GMLReal py) {
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;
    GMLReal dx = GMLReal_bankersRound(px) + 0.5 - GMLReal_bankersRound(inst->x);
    GMLReal dy = GMLReal_bankersRound(py) + 0.5 - GMLReal_bankersRound(inst->y);
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        GMLReal cs, sn;
        Collision_angleTrig(inst->imageAngle, &cs, &sn);
        GMLReal rx = cs * dx - sn * dy;
        GMLReal ry = sn * dx + cs * dy;
        dx = rx;
        dy = ry;
    }
    int32_t c = Colflip_index(Colflip_floor(Colflip_div(dx, inst->imageXscale) + (GMLReal) spr->originX), spr->width, true, spr->maskOffsetX, spr->maskWidth);
    int32_t r = Colflip_index(Colflip_floor(Colflip_div(dy, inst->imageYscale) + (GMLReal) spr->originY), spr->height, true, spr->maskOffsetY, spr->maskHeight);
    if (0 > c || 0 > r) return false;
    return Colflip_maskBit(spr->masks[((uint32_t) inst->imageIndex) % spr->maskCount], (spr->maskWidth + 7) / 8, c, r);
}

COLFLIP_LATE COLFLIP_FLAT bool Collision_compatPointHits(Runner* runner, Instance* inst, GMLReal px, GMLReal py, bool prec) {
    if (!Collision_pointInsideInstanceBox(runner, inst, px, py)) return false;
    if (!prec) return true;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    return !Collision_hasFrameMasks(spr) || Collision_compatPointInInstance(spr, inst, px, py);
}

// Columns the rectangle walk and the scan work out at a time.
#define COLFLIP_CHUNK 64

// The rectangle walk: Collision_compatPointInInstance's arithmetic with the per-instance parts worked out once (the
// rounded position, the frame's mask, the trig). For an unrotated instance a pixel's column depends only on its x and
// its row only on its y, so columns are worked out once per chunk and rows once per row. Each column and row is the
// same expression on the same values, so the same pixels hit.
static inline __attribute__((always_inline)) bool Colflip_rectWalk(Sprite* spr, Instance* inst, int32_t u0, int32_t u1, int32_t v0, int32_t v1) {
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;
    GMLReal ix = GMLReal_bankersRound(inst->x), iy = GMLReal_bankersRound(inst->y);
    ColflipDivisor xs = Colflip_divisor(inst->imageXscale), ys = Colflip_divisor(inst->imageYscale);
    GMLReal ox = (GMLReal) spr->originX, oy = (GMLReal) spr->originY;
    const uint8_t* mask = spr->masks[((uint32_t) inst->imageIndex) % spr->maskCount];
    uint32_t rowBytes = (spr->maskWidth + 7) / 8;
    if (GMLReal_fabs(inst->imageAngle) > 0.0001) {
        GMLReal cs, sn;
        Collision_angleTrig(inst->imageAngle, &cs, &sn);
        for (int32_t v = v0; v1 >= v; v++) {
            GMLReal dy = (GMLReal) v + 0.5 - iy;
            for (int32_t u = u0; u1 >= u; u++) {
                GMLReal dx = (GMLReal) u + 0.5 - ix;
                GMLReal rx = cs * dx - sn * dy;
                GMLReal ry = sn * dx + cs * dy;
                int32_t c = Colflip_index(Colflip_floor(Colflip_divBy(rx, &xs) + ox), spr->width, true, spr->maskOffsetX, spr->maskWidth);
                int32_t r = Colflip_index(Colflip_floor(Colflip_divBy(ry, &ys) + oy), spr->height, true, spr->maskOffsetY, spr->maskHeight);
                if (0 > c || 0 > r) continue;
                if (Colflip_maskBit(mask, rowBytes, c, r)) return true;
            }
        }
        return false;
    }
    int32_t col[COLFLIP_CHUNK];
    for (int64_t c0 = u0; u1 >= c0; c0 += COLFLIP_CHUNK) {
        int32_t n = (int64_t) u1 - c0 + 1 > COLFLIP_CHUNK ? COLFLIP_CHUNK : (int32_t) ((int64_t) u1 - c0 + 1);
        bool anyColumn = false;
        repeat(n, i) {
            GMLReal dx = (GMLReal) (int32_t) (c0 + i) + 0.5 - ix;
            col[i] = Colflip_index(Colflip_floor(Colflip_divBy(dx, &xs) + ox), spr->width, true, spr->maskOffsetX, spr->maskWidth);
            if (col[i] >= 0) anyColumn = true;
        }
        if (!anyColumn) continue;
        for (int32_t v = v0; v1 >= v; v++) {
            GMLReal dy = (GMLReal) v + 0.5 - iy;
            int32_t r = Colflip_index(Colflip_floor(Colflip_divBy(dy, &ys) + oy), spr->height, true, spr->maskOffsetY, spr->maskHeight);
            if (0 > r) continue;
            const uint8_t* line = mask + (uint32_t) r * rowBytes;
            repeat(n, i) {
                int32_t c = col[i];
                if (c >= 0 && (line[c >> 3] & (1 << (7 - (c & 7)))) != 0) return true;
            }
        }
    }
    return false;
}

// The separating-axis test of a rotated non-precise box (sepMasks 2) against the rect, as Collision_rectOverlapsInstance
// does it.
static inline __attribute__((always_inline)) bool Colflip_rectOverlapsOBB(Sprite* spr, Instance* inst, GMLReal rx1, GMLReal ry1, GMLReal rx2, GMLReal ry2) {
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

COLFLIP_LATE COLFLIP_FLAT bool Collision_compatRectHitsInstance(Runner* runner, Instance* inst, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, bool prec) {
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (!bbox.valid) return false;
    GMLReal right = bbox.right - 1.0, bottom = bbox.bottom - 1.0;
    if (x1 > right || bbox.left > x2 || y1 > bottom || bbox.top > y2) return false;
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (prec && Collision_hasFrameMasks(spr)) {
        int32_t u0 = (int32_t) GMLReal_bankersRound(GMLReal_fmax(x1, bbox.left));
        int32_t u1 = (int32_t) GMLReal_bankersRound(GMLReal_fmin(x2, right));
        int32_t v0 = (int32_t) GMLReal_bankersRound(GMLReal_fmax(y1, bbox.top));
        int32_t v1 = (int32_t) GMLReal_bankersRound(GMLReal_fmin(y2, bottom));
        if (u0 > u1 || v0 > v1) return false;
        return Colflip_rectWalk(spr, inst, u0, u1, v0, v1);
    }
    if (!Collision_obbNeedsSAT(spr, inst)) return true;
    return Colflip_rectOverlapsOBB(spr, inst, x1, y1, x2, y2);
}

// GameMaker's general scan takes its rotation in float32: the angle converted to radians in double, rounded to float,
// and its cosine and sine rounded to float. Cached by angle like Collision_angleTrig's slots, in late statics.
typedef struct {
    GMLReal angle;
    float csf, snf;
    bool filled;
} ColflipTrigSlot;
#define COLFLIP_TRIG_SLOTS 256
static __attribute__((section(".bss.late.colflip"))) ColflipTrigSlot Colflip_trigSlots[COLFLIP_TRIG_SLOTS];

static inline __attribute__((always_inline)) void Colflip_floatTrig(GMLReal angle, float* csf, float* snf) {
    uint64_t bits = 0;
    memcpy(&bits, &angle, sizeof(angle));
    ColflipTrigSlot* slot = &Colflip_trigSlots[(bits * 0x9E3779B97F4A7C15ull) >> 56];
    if (!slot->filled || memcmp(&slot->angle, &angle, sizeof(angle)) != 0) {
        float radf = (float) ((double) angle * M_PI / 180.0);
        slot->csf = (float) cos((double) radf);
        slot->snf = (float) sin((double) radf);
        slot->angle = angle;
        slot->filled = true;
    }
    *csf = slot->csf;
    *snf = slot->snf;
}

// One instance as the compatibility scan tests it, worked out once per scan.
typedef struct {
    Sprite* spr;
    const uint8_t* mask; // the frame's collision mask, nullptr for a sprite without masks
    uint32_t rowBytes;
    GMLReal x, y, xscale, yscale;
    GMLReal sx, sy;  // the sample's shift from the pixel corner: 1 on an axis with a negative scale
    ColflipDivisor dx, dy; // xscale and yscale
    ColflipAxis ax, ay; // the sprite's (and mask's) columns and rows
    uint32_t gx, gy;    // originX - ax.lo and originY - ay.lo: a floored cell plus these is (uint32_t) i - lo
    float csf, snf;  // the rotation in float32
    bool rotated;
} ColflipShape;

static inline __attribute__((always_inline)) bool Colflip_shape(Sprite* spr, Instance* inst, ColflipShape* out) {
    if (0.0001 > GMLReal_fabs(inst->imageXscale)) return false;
    if (0.0001 > GMLReal_fabs(inst->imageYscale)) return false;
    out->spr = spr;
    out->mask = Collision_hasFrameMasks(spr) ? spr->masks[((uint32_t) inst->imageIndex) % spr->maskCount] : nullptr;
    out->rowBytes = (spr->maskWidth + 7) / 8;
    out->x = inst->x;
    out->y = inst->y;
    out->xscale = inst->imageXscale;
    out->yscale = inst->imageYscale;
    out->sx = 0.0 > inst->imageXscale ? 1.0 : 0.0;
    out->sy = 0.0 > inst->imageYscale ? 1.0 : 0.0;
    out->dx = Colflip_divisor(inst->imageXscale);
    out->dy = Colflip_divisor(inst->imageYscale);
    out->ax = Colflip_axis(spr->width, out->mask != nullptr, spr->maskOffsetX, spr->maskWidth);
    out->ay = Colflip_axis(spr->height, out->mask != nullptr, spr->maskOffsetY, spr->maskHeight);
    out->gx = (uint32_t) spr->originX - out->ax.lo;
    out->gy = (uint32_t) spr->originY - out->ay.lo;
    out->rotated = GMLReal_fabs(inst->imageAngle) > 0.0001;
    out->csf = 1.0f;
    out->snf = 0.0f;
    if (out->rotated) Colflip_floatTrig(inst->imageAngle, &out->csf, &out->snf);
    return true;
}

// One axis of an unrotated shape: the mask column (or row) that world coordinate w falls in, or -1. general floors the
// cell in float32, as GameMaker does for an unrotated instance beside a rotated one; otherwise it is truncated.
static inline __attribute__((always_inline)) int32_t Colflip_cell(GMLReal w, GMLReal pos, const ColflipDivisor* scale, int32_t origin, const ColflipAxis* axis,
                                                                 bool general) {
    uint32_t i;
    if (general) {
        float d = (float) (w - pos);
        i = (uint32_t) Colflip_floorf(Colflip_divByF(d, scale)) + (uint32_t) origin;
    } else {
        i = (uint32_t) (int32_t) (Colflip_divBy(w - pos, scale) + (GMLReal) origin);
    }
    return Colflip_axisIndex(axis, i);
}

// A rotated shape's test of world point (px, py), the general way (float32, floored cell).
// The column is tested before the row is worked out.
static inline __attribute__((always_inline)) bool Colflip_shapeHas(const ColflipShape* s, GMLReal px, GMLReal py) {
    float dx = (float) (px - s->x);
    float dy = (float) (py - s->y);
    float rx = s->csf * dx - s->snf * dy;
    uint32_t u = (uint32_t) Colflip_floorf(Colflip_divByF(rx, &s->dx)) + s->gx;
    if (u >= s->ax.span) return false;
    float ry = s->snf * dx + s->csf * dy;
    uint32_t v = (uint32_t) Colflip_floorf(Colflip_divByF(ry, &s->dy)) + s->gy;
    if (v >= s->ay.span) return false;
    return s->mask == nullptr || Colflip_maskBit(s->mask, s->rowBytes, (int32_t) (u + s->ax.add), (int32_t) (v + s->ay.add));
}

COLFLIP_LATE COLFLIP_FLAT bool Collision_compatScanPixels(const ColflipScan* scan) {
    Sprite* sprA = scan->sprA;
    Sprite* sprB = scan->sprB;
    Instance* a = scan->a;
    Instance* b = scan->b;
    int32_t startX = scan->startX, startY = scan->startY;
    int64_t cols = scan->cols, rows = scan->rows;
    if (0 >= cols || 0 >= rows) return false;
    ColflipShape sa, sb;
    if (!Colflip_shape(sprA, a, &sa) || !Colflip_shape(sprB, b, &sb)) return false;
    bool general = sa.rotated || sb.rotated;
    if (!general && sa.xscale == 1.0 && sa.yscale == 1.0 && sb.xscale == 1.0 && sb.yscale == 1.0) {
        sa.x = GMLReal_bankersRound(sa.x);
        sa.y = GMLReal_bankersRound(sa.y);
        sb.x = GMLReal_bankersRound(sb.x);
        sb.y = GMLReal_bankersRound(sb.y);
    }
    bool maskedA = sa.mask != nullptr, maskedB = sb.mask != nullptr;
    GMLReal wx[COLFLIP_CHUNK];
    int32_t colA[COLFLIP_CHUNK], colB[COLFLIP_CHUNK];
    for (int64_t c0 = 0; cols > c0; c0 += COLFLIP_CHUNK) {
        int32_t n = cols - c0 > COLFLIP_CHUNK ? COLFLIP_CHUNK : (int32_t) (cols - c0);
        bool anyColumn = false;
        // The columns, once for each value of general, so that neither loop tests it.
        if (general) {
            repeat(n, i) {
                wx[i] = Colflip_real64((int64_t) startX + c0 + i);
                colA[i] = sa.rotated ? 0 : Colflip_cell(wx[i] + sa.sx, sa.x, &sa.dx, sprA->originX, &sa.ax, true);
                colB[i] = sb.rotated ? 0 : Colflip_cell(wx[i] + sb.sx, sb.x, &sb.dx, sprB->originX, &sb.ax, true);
                if (colA[i] >= 0 && colB[i] >= 0) anyColumn = true;
            }
        } else {
            repeat(n, i) {
                wx[i] = Colflip_real64((int64_t) startX + c0 + i);
                colA[i] = Colflip_cell(wx[i] + sa.sx, sa.x, &sa.dx, sprA->originX, &sa.ax, false);
                colB[i] = Colflip_cell(wx[i] + sb.sx, sb.x, &sb.dx, sprB->originX, &sb.ax, false);
                if (colA[i] >= 0 && colB[i] >= 0) anyColumn = true;
            }
        }
        if (!anyColumn) continue;
        repeat(rows, j) {
            GMLReal wy = Colflip_real64((int64_t) startY + j);
            const uint8_t* lineA = nullptr;
            const uint8_t* lineB = nullptr;
            if (!sa.rotated) {
                int32_t row = Colflip_cell(wy + sa.sy, sa.y, &sa.dy, sprA->originY, &sa.ay, general);
                if (0 > row) continue;
                if (maskedA) lineA = sa.mask + (uint32_t) row * sa.rowBytes;
            }
            if (!sb.rotated) {
                int32_t row = Colflip_cell(wy + sb.sy, sb.y, &sb.dy, sprB->originY, &sb.ay, general);
                if (0 > row) continue;
                if (maskedB) lineB = sb.mask + (uint32_t) row * sb.rowBytes;
            }
            repeat(n, i) {
                int32_t ca = colA[i], cb = colB[i];
                if (0 > ca || 0 > cb) continue;
                if (lineA != nullptr && (lineA[ca >> 3] & (1 << (7 - (ca & 7)))) == 0) continue;
                if (lineB != nullptr && (lineB[cb >> 3] & (1 << (7 - (cb & 7)))) == 0) continue;
                if (sa.rotated && !Colflip_shapeHas(&sa, wx[i] + sa.sx, wy + sa.sy)) continue;
                if (sb.rotated && !Colflip_shapeHas(&sb, wx[i] + sb.sx, wy + sb.sy)) continue;
                return true;
            }
        }
    }
    return false;
}

#endif /* COLLISION_COMPAT_DEFINE */

// Colflip_callScan's target (collision_gm14.c): UNDERTALE's scan for GameMaker 1.4's data, otherwise a tail call of
// Collision_compatScanPixels. collision_compat.c never reaches the scan through Colflip_callScan, and any added
// declaration changes its debug info (var-tracking follows declaration numbering), so it keeps the old target.
#ifndef COLLISION_COMPAT_DEFINE
bool Colflip14_scanPixels(const ColflipScan* scan);
#define COLFLIP_SCAN_TARGET Colflip14_scanPixels
#else
#define COLFLIP_SCAN_TARGET Collision_compatScanPixels
#endif

// Calls Collision_compatScanPixels (through Colflip14_scanPixels) so that GCC sees no more than the original scan
// does. A plain call adds a call graph edge from Collision_instancesOverlapPrecise down to new code, and GCC's
// function output order (callers walked up from each externally visible function, the compatibility scan being one)
// then moves dozens of existing functions; it also widens what the call may write, which changes the code of callers
// such as mpTestFree. The asm call has no call graph edge, reads only the fields the original scan reads, writes no
// memory (the scan writes only its own statics), and clobbers what the ABI lets a call clobber, which the original
// scan uses in full. Other targets call it plainly: their layout is not tracked.
static inline __attribute__((always_inline)) bool Colflip_callScan(const ColflipScan* scan) {
#if defined(__arm__) && !defined(__thumb__)
    register uintptr_t r0 __asm__("r0") = (uintptr_t) scan;
    Instance* a = scan->a;
    Instance* b = scan->b;
    __asm__("blx %[fn]"
            : "+r"(r0)
            : [fn] "r"(COLFLIP_SCAN_TARGET), "m"(*scan), "m"(a->x), "m"(a->y), "m"(a->imageXscale), "m"(a->imageYscale), "m"(a->imageAngle),
              "m"(a->imageIndex), "m"(b->x), "m"(b->y), "m"(b->imageXscale), "m"(b->imageYscale), "m"(b->imageAngle), "m"(b->imageIndex)
            : "r1", "r2", "r3", "ip", "lr", "cc", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7");
    return (r0 & 0xff) != 0;
#else
    return COLFLIP_SCAN_TARGET(scan);
#endif
}

// Collision_instancesOverlapPrecise's scan. Compatibility mode is the only caller that samples at offset 0 (modern
// mode samples pixel centres, 0.5), and it passes an inclusive range (cols and rows one larger): that goes to the
// compatibility scan with the exclusive range GameMaker walks. Modern mode goes to Collision_scanPixels unchanged.
MAYBE_UNUSED static COLFLIP_LATE bool Collision_scanPixelsLate(Sprite* sprA, Instance* a, Sprite* sprB, Instance* b, int32_t startX, int64_t cols, int32_t startY,
                                                                int64_t rows, GMLReal offset) {
    if (offset != 0.0) return Collision_scanPixels(sprA, a, sprB, b, startX, cols, startY, rows, offset);
    ColflipScan scan = { sprA, a, sprB, b, startX, startY, cols - 1, rows - 1 };
    return Colflip_callScan(&scan);
}

// Returns true if the two instances' collision shapes overlap.
//
// Matches the native GMS 1.4 runner's flow in FUN_0043fde0:
//   1. AABB overlap test on the two precomputed bboxes.
//   2. If neither sprite is precise (sepMasks == 1), the AABB overlap is enough.
//   3. Otherwise walk the pixel intersection and test BOTH instances on every
//      pixel via Collision_pointInInstance. Both sides get inverse-transformed
//      regardless of whether they're individually precise, so a rotated
//      non-precise sprite collides as an OBB as long as its partner is precise.
static inline bool Collision_instancesOverlapPrecise(Runner* runner, Instance* a, Instance* b, InstanceBBox bboxA, InstanceBBox bboxB) {
    bool compatMode = runner->collisionCompatibilityMode;
    DataWin* dataWin = runner->dataWin;
    // Compute world-space intersection of the two AABBs
    GMLReal iLeft   = GMLReal_fmax(bboxA.left, bboxB.left);
    GMLReal iRight  = GMLReal_fmin(bboxA.right, bboxB.right);
    GMLReal iTop    = GMLReal_fmax(bboxA.top, bboxB.top);
    GMLReal iBottom = GMLReal_fmin(bboxA.bottom, bboxB.bottom);

    // AABB overlap test. Native uses identical semantics in both modern and compat for axis-aligned integer-coordinate cases (compat shifts bbox.right/bottom by -1 *and* the test by +1, which cancel).
    if (iLeft >= iRight || iTop >= iBottom) return false;

    Sprite* sprA = Collision_getSprite(dataWin, a);
    Sprite* sprB = Collision_getSprite(dataWin, b);
    if (sprA == nullptr || sprB == nullptr) return false;

    bool preciseA = Collision_hasFrameMasks(sprA);
    bool preciseB = Collision_hasFrameMasks(sprB);

    // Neither sprite precise? Then we need to check if either side is a rotated sepMasks==2 sprite, in which case the loose AABB engulfs space the rotated rect doesn't, and we need OBB-vs-OBB SAT to match native SeparatingAxisCollisionBox.
    if (!preciseA && !preciseB) {
        bool needSatA = Collision_obbNeedsSAT(sprA, a);
        bool needSatB = Collision_obbNeedsSAT(sprB, b);
        if (!needSatA && !needSatB) return true;

        InstanceOBB obbA = Collision_instanceOBB(sprA, a);
        InstanceOBB obbB = Collision_instanceOBB(sprB, b);

        // Compute the 4 world corners of each OBB. Indices: 0=(lx0,ly0), 1=(lx1,ly0), 2=(lx0,ly1), 3=(lx1,ly1).
        GMLReal ax[4], ay[4], bx[4], by[4];
        GMLReal lxA[2] = {obbA.lx0, obbA.lx1}, lyA[2] = {obbA.ly0, obbA.ly1};
        GMLReal lxB[2] = {obbB.lx0, obbB.lx1}, lyB[2] = {obbB.ly0, obbB.ly1};
        repeat(2, i) {
            repeat(2, j) {
                int k = j * 2 + i;
                ax[k] = obbA.x + obbA.cs * lxA[i] + obbA.sn * lyA[j];
                ay[k] = obbA.y - obbA.sn * lxA[i] + obbA.cs * lyA[j];
                bx[k] = obbB.x + obbB.cs * lxB[i] + obbB.sn * lyB[j];
                by[k] = obbB.y - obbB.sn * lxB[i] + obbB.cs * lyB[j];
            }
        }

        // SAT: 4 axes (each OBB has 2 unique edge normals). axes[0]/[1] are A's local-x/local-y in world space, axes[2]/[3] are B's.
        GMLReal axes[4][2] = {
            { obbA.cs, -obbA.sn },
            { obbA.sn,  obbA.cs },
            { obbB.cs, -obbB.sn },
            { obbB.sn,  obbB.cs }
        };
        repeat(4, axIdx) {
            GMLReal nx = axes[axIdx][0], ny = axes[axIdx][1];
            GMLReal aMin = ax[0]*nx + ay[0]*ny, aMax = aMin;
            GMLReal bMin = bx[0]*nx + by[0]*ny, bMax = bMin;
            for (int j = 1; 4 > j; j++) {
                GMLReal pa = ax[j]*nx + ay[j]*ny;
                GMLReal pb = bx[j]*nx + by[j]*ny;
                if (pa < aMin) aMin = pa; else if (pa > aMax) aMax = pa;
                if (pb < bMin) bMin = pb; else if (pb > bMax) bMax = pb;
            }
            if (aMax <= bMin || bMax <= aMin) return false;
        }
        return true;
    }

    // Pixel scan over the AABB intersection.
    // Modern: floor..ceil with exclusive upper bound, sample pixel centers (+0.5).
    // Compatibility: truncated int range with inclusive upper bound, sample pixel corners (no +0.5).
    int32_t startX, endX, startY, endY;
    GMLReal sampleOffset;
    if (compatMode) {
        startX = (int32_t) iLeft;
        endX   = (int32_t) iRight;
        startY = (int32_t) iTop;
        endY   = (int32_t) iBottom;
        sampleOffset = 0.0;
    } else {
        startX = (int32_t) GMLReal_floor(iLeft);
        endX   = (int32_t) GMLReal_ceil(iRight);
        startY = (int32_t) GMLReal_floor(iTop);
        endY   = (int32_t) GMLReal_ceil(iBottom);
        sampleOffset = 0.5;
    }

    int64_t cols = (int64_t) endX - startX + (compatMode ? 1 : 0);
    int64_t rows = (int64_t) endY - startY + (compatMode ? 1 : 0);
    if (0 >= cols || 0 >= rows) return false;
    return Collision_scanPixelsLate(sprA, a, sprB, b, startX, cols, startY, rows, sampleOffset);
}

#endif /* _BS_COLLISION_H_ */
