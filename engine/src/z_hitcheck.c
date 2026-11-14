// GMS 1.4's PC box for collision and public readers. Drawing transforms and
// the shared double-precision angle cache retain their original calculations.
#include "wad_versions.h"
#if defined(ENABLE_WAD16)
#define HITCHECK_IMPLEMENTATION
#include "collision.h"
#include "colexact_bbox.h"
#define HITCHECK_LATE __attribute__((noinline, flatten, section(".text.late.hitcheck")))

typedef struct { uint32_t key; float cs, sn; } HitcheckTrigSlot;
// A failed first allocation disables caching for this runner's lifetime.
#define HITCHECK_CACHE_OFF ((void*)(uintptr_t)1)

static inline __attribute__((always_inline)) void Hitcheck_orderProducts(GMLReal* low, GMLReal* high) {
    GMLReal a=*low, b=*high;
    if (a < b) return;
    if (a > b) { *low=b; *high=a; return; }
    // Equal values (including signed zero) and unordered values keep the
    // original libm semantics. Ordinary unequal products need no library call.
    *low=GMLReal_fmin(a,b);
    *high=GMLReal_fmax(a,b);
}

// Keep the rotated body's register saves off the common zero-angle path.
static HITCHECK_LATE InstanceBBox Hitcheck_rotatedBBox(Runner* runner, Instance* inst) {
    Sprite* spr = Collision_getSprite(runner->dataWin, inst);
    if (spr == nullptr) return (InstanceBBox){0};
    // Sort inclusive stored margins BEFORE making the upper edge exclusive.
    GMLReal ml = spr->bboxMode == 1 ? 0 : (spr->marginLeft < spr->marginRight ? spr->marginLeft : spr->marginRight);
    GMLReal mr = spr->bboxMode == 1 ? spr->width : (GMLReal)(spr->marginLeft > spr->marginRight ? spr->marginLeft : spr->marginRight) + 1;
    GMLReal mt = spr->bboxMode == 1 ? 0 : (spr->marginTop < spr->marginBottom ? spr->marginTop : spr->marginBottom);
    GMLReal mb = spr->bboxMode == 1 ? spr->height : (GMLReal)(spr->marginTop > spr->marginBottom ? spr->marginTop : spr->marginBottom) + 1;
    // Match the PC's float angle, float PI, double division, float radians,
    // double libm, float result. Keep the division under -ffast-math.
    double denominator = 180.0;
#if defined(__arm__)
    __asm__ volatile("" : "+w"(denominator));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(denominator));
#else
    __asm__ volatile("" : "+m"(denominator));
#endif
    float radians = (float)((double)(float)inst->imageAngle * (double)3.14159274101257324f / denominator);
    // The entry guard excludes subnormal radians, so key 1 is initially invalid.
    // Runner_create zeroes its allocation. Retain only a heap pointer in the
    // runner's existing ARM tail padding; never add mutable late data/BSS.
    if (runner->hitcheckCache == nullptr) {
        HitcheckTrigSlot* allocated = malloc(512 * sizeof(*allocated));
        if (allocated != nullptr) {
            for (unsigned i=0; i<512; ++i) allocated[i].key=1;
            runner->hitcheckCache=allocated;
        } else runner->hitcheckCache=HITCHECK_CACHE_OFF;
    }
    HitcheckTrigSlot* trig=runner->hitcheckCache;
    uint32_t key;
    memcpy(&key, &radians, sizeof(key));
    float cs, sn;
    if ((void*)trig == HITCHECK_CACHE_OFF) {
        cs=(float)cos((double)radians);
        sn=(float)sin((double)radians);
    } else {
        // Fold exponent/high mantissa bits into the low mantissa before mixing:
        // recurring angles otherwise cluster in the same direct-mapped slots.
        uint32_t mixed=key^(key>>16);
        HitcheckTrigSlot* slot=&trig[(mixed * UINT32_C(0x9e3779b9)) >> 23];
        if (slot->key != key) {
            slot->cs = (float)cos((double)radians);
            slot->sn = (float)sin((double)radians);
            slot->key = key;
        }
        cs=slot->cs; sn=slot->sn;
    }
    // GMS 1.4 stores each local extent and trig product as float.
    // A product of two finite floats is exact in double before rounding to float.
    // Keep these inputs as float to avoid promote/multiply/narrow sequences;
    // ordered products and the position-first sums below remain double.
    float lx0=(float)(inst->imageXscale*(float)(ml-spr->originX));
    float lx1=(float)(inst->imageXscale*(float)(mr-spr->originX));
    float ly0=(float)(inst->imageYscale*(float)(mt-spr->originY));
    float ly1=(float)(inst->imageYscale*(float)(mb-spr->originY));
    GMLReal cx0=(float)(cs*lx0), cx1=(float)(cs*lx1);
    GMLReal sy0=(float)(sn*ly0), sy1=(float)(sn*ly1);
    GMLReal cy0=(float)(cs*ly0), cy1=(float)(cs*ly1);
    GMLReal sx0=(float)(sn*lx0), sx1=(float)(sn*lx1);
    Hitcheck_orderProducts(&cx0,&cx1);
    Hitcheck_orderProducts(&sy0,&sy1);
    Hitcheck_orderProducts(&cy0,&cy1);
    Hitcheck_orderProducts(&sx0,&sx1);
    GMLReal left=inst->x+cx0, right=inst->x+cx1;
    GMLReal top=inst->y+cy0, bottom=inst->y+cy1;
    // Preserve the PC's position-first double sums under -ffast-math.
#if defined(__arm__)
    __asm__ volatile("" : "+w"(left), "+w"(right), "+w"(top), "+w"(bottom));
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("" : "+x"(left), "+x"(right), "+x"(top), "+x"(bottom));
#else
    __asm__ volatile("" : "+m"(left), "+m"(right), "+m"(top), "+m"(bottom));
#endif
    return (InstanceBBox){ .left=Collision_bboxRound(left+sy0),
        .right=Collision_bboxRound(right+sy1),
        .top=Collision_bboxRound(top-sx1),
        .bottom=Collision_bboxRound(bottom-sx0), .valid=true };
}

HITCHECK_LATE InstanceBBox Hitcheck_collisionBBox(Runner* runner, Instance* inst) {
    if (!runner->collisionCompatibilityMode || runner->dataWin->gen8.wadVersion > 16)
        return Colexact_sharedBBox(runner,inst);
    if ((float)inst->imageAngle == 0.0f) {
        Sprite* spr=Collision_getSprite(runner->dataWin,inst);
        return spr == nullptr ? (InstanceBBox){0} : Colexact_inlineBBox(spr,inst);
    }
    if (GMLReal_fabs(inst->imageAngle) <= 0.0001)
        return Colexact_sharedBBox(runner,inst);
    return Hitcheck_rotatedBBox(runner,inst);
}

// Runner_free's final branch is redirected here after all its other cleanup.
HITCHECK_LATE void Hitcheck_freeRunner(Runner* runner) {
    if (runner->hitcheckCache != HITCHECK_CACHE_OFF) free(runner->hitcheckCache);
    free(runner);
}

// Explicit scalar ABI matches the two release GCC IPA-SRA pair clones:
// r0=dataWin, r1=compatMode, r2=a, r3=b, d0..d7=the eight bbox edges.
HITCHECK_LATE bool Hitcheck_pair(DataWin* dataWin, bool compatMode, Instance* a, Instance* b,
        GMLReal al, GMLReal ar, GMLReal at, GMLReal ab,
        GMLReal bl, GMLReal br, GMLReal bt, GMLReal bb) {
    InstanceBBox bboxA={al,ar,at,ab,true}, bboxB={bl,br,bt,bb,true};

    // Compute world-space intersection of the two AABBs
    GMLReal iLeft   = GMLReal_fmax(bboxA.left, bboxB.left);
    GMLReal iRight  = GMLReal_fmin(bboxA.right, bboxB.right);
    GMLReal iTop    = GMLReal_fmax(bboxA.top, bboxB.top);
    GMLReal iBottom = GMLReal_fmin(bboxA.bottom, bboxB.bottom);

    // AABB overlap test. Native uses identical semantics in both modern and compat for axis-aligned integer-coordinate cases (compat shifts bbox.right/bottom by -1 *and* the test by +1, which cancel).
    if (compatMode && dataWin->gen8.wadVersion <= 16) {
        // PC compares the four separating edges independently. A zero-area
        // non-precise mask can still lie strictly inside the other rectangle.
        if (bboxA.left >= bboxB.right || bboxB.left >= bboxA.right ||
            bboxA.top >= bboxB.bottom || bboxB.top >= bboxA.bottom) return false;
    } else if (iLeft >= iRight || iTop >= iBottom) return false;

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


HITCHECK_LATE bool Hitcheck_instancesOverlapPrecise(Runner* runner, Instance* a, Instance* b,
        InstanceBBox aa, InstanceBBox bb) {
    return Hitcheck_pair(runner->dataWin, runner->collisionCompatibilityMode, a, b,
        aa.left, aa.right, aa.top, aa.bottom, bb.left, bb.right, bb.top, bb.bottom);
}
#endif
