// Swarms of short-lived instances (utdust): a monster killed in battle turns to dust, one small instance per
// pixel of its picture, about 1,100 to 1,600 for a common monster, each made from another object's Draw event, rising
// under gravity and gone after its sprite's 12 frames. Up to 192 are made and 192 destroyed in one frame, with 1,600
// alive. Two of the runner's per-instance costs grew with that:
//
// - Taking destroyed instances out of the per-object lists. Runner_removeInstanceFromObjectLists finds each one
//   and moves the rest of the list down a place, so k destroyed instances in a list of n cost k * n: 2.7 to 4.5 ms
//   a frame on the Old 3DS (Azahar) as the largest dust fades. Runner_cleanupDestroyedInstances now hands its
//   destroyed instances to Utdust_deferDestroyed and Utdust_flushDestroyed. When BATCH_MIN or more of one object
//   go at once, that object's lists are compacted in one pass each, the survivors keeping their order, which is
//   what removing them one by one leaves. Other batches take the old removal. Everything else (the spatial grid,
//   the layer element, the id map and the free) happens per instance in the same order as before.
// - Gravity. Every frame, each instance with gravity took the cosine and sine of its gravity_direction (in double
//   precision, with clampFloat's two int64 conversions each). Utdust_applyGravity keeps the last direction's pair:
//   the dust's 1,600 pixels all fall at 90 degrees. The pair is a pure function of the direction's bits, so the
//   speeds are the same to the bit. Then speed and direction come from speedFromComponents (utkill): the
//   build's Instance_computeSpeedFromComponents with its runtime calls (int64 conversions, round, fmod) done
//   cheaper, the same to the bit (tools/analysis/check_utdust_speed.py).
// - The Outside Room test (utkill). Every pixel, every frame, took Collision_computeBBox and four floor()
//   calls of collision-compatibility rounding. Utdust_insideRoom proves most of them inside with the unrounded box
//   first; the rest go the full way.
//
// Everything here is late code and data (platform/3ds/late_reserve.ld), and uses no .rodata: UNDERTALE Old's late
// constants reserve was 139 B. The callers in runner.c keep their slots (layout rule "body-size change with retained
// slot"; tools/analysis/check_utdust_slots.py).

#include "runner.h"
#include "data_win.h"
#include "instance.h"
#include "utils.h"
#include "spatial_grid.h"
#include "math_compat.h"
#include "stb_ds.h"

#include <stdint.h>
#include <math.h>
#include <string.h>

#if defined(__GNUC__)
#define UTDUST_LATE __attribute__((noinline, section(".text.late.utdust")))
#define UTDUST_BSS __attribute__((section(".bss.late.utdust")))
#else
#define UTDUST_LATE
#define UTDUST_BSS
#endif

void Utdust_deferDestroyed(Runner* runner, Instance* inst) UTDUST_LATE;
void Utdust_flushDestroyed(Runner* runner) UTDUST_LATE;
void Utdust_applyGravity(Instance* inst) UTDUST_LATE;
bool Utdust_insideRoom(Runner* runner, Instance* inst, int32_t roomWidth, int32_t roomHeight) UTDUST_LATE;

// Destroyed instances waiting for the flush, in Runner_cleanupDestroyedInstances' order. A full buffer is flushed
// at once, so any number of them goes through in batches of DEAD_CAP.
#define DEAD_CAP 256
// An object with at least this many instances in one batch has its lists compacted: its exact-object list and the
// list of each object in its parent chain (every one holds all of them). A compaction costs a probe of the set per
// entry; taking instances out one by one costs a compare and a word moved per entry for each.
#define BATCH_MIN 16
// The batch's destroyed instances, as a set: open addressing, at most half full.
#define SET_BITS 9
#define SET_SIZE (1u << SET_BITS)
// Two small maps of int keys, open addressing at most half full: per exact object, how many of the batch it holds;
// and the lists compacted, (object << 1) | 1 for an exact-object list. A batch of more distinct objects than
// KEY_CAP takes the old removal, and an object whose lists would take the compacted map past KEY_CAP keys is not
// compacted (its instances take the old removal from every list not already compacted), so both maps always keep
// an empty slot and every probe ends.
#define KEY_BITS 7
#define KEY_SIZE (1u << KEY_BITS)
#define KEY_CAP ((int32_t) (KEY_SIZE / 2))
// The object parent chain's depth limit, as Runner_addInstanceToObjectLists has it.
#define CHAIN_MAX 32

static Instance* dead[DEAD_CAP] UTDUST_BSS;
static int32_t deadCount UTDUST_BSS;
static Instance* deadSet[SET_SIZE] UTDUST_BSS;
// Keys are stored plus one, so zero is an empty slot.
static int32_t objectKey[KEY_SIZE] UTDUST_BSS;
static int32_t objectCount[KEY_SIZE] UTDUST_BSS;
static int32_t compactedKey[KEY_SIZE] UTDUST_BSS;

static uint32_t gravityBits UTDUST_BSS;
static bool gravityKnown UTDUST_BSS;
static GMLReal gravityCos UTDUST_BSS;
static GMLReal gravitySin UTDUST_BSS;

static inline uint32_t setSlot(const Instance* inst) {
    return ((uint32_t) (uintptr_t) inst * 2654435761u) >> (32 - SET_BITS);
}

static inline bool setHas(const Instance* inst) {
    for (uint32_t s = setSlot(inst);; s = (s + 1) & (SET_SIZE - 1)) {
        const Instance* held = deadSet[s];
        if (held == inst) return true;
        if (held == nullptr) return false;
    }
}

// The slot of key in keys: where it is, or the empty slot where it would go.
static inline uint32_t keySlot(const int32_t* keys, int32_t key) {
    for (uint32_t s = ((uint32_t) key * 2654435761u) >> (32 - KEY_BITS);; s = (s + 1) & (KEY_SIZE - 1)) {
        if (keys[s] == key + 1 || keys[s] == 0) return s;
    }
}

// Runner's removeInstanceFromList: the first entry for inst out, the rest moved down, the order kept.
static UTDUST_LATE void removeOne(Instance*** listPtr, Instance* inst) {
    Instance** list = *listPtr;
    int32_t n = (int32_t) arrlen(list);
    repeat(n, i) {
        if (list[i] == inst) {
            if (n - 1 > i) memmove(&list[i], &list[i + 1], (size_t) (n - 1 - i) * sizeof(Instance*));
            arrsetlen(*listPtr, n - 1);
            return;
        }
    }
}

// Every entry of the set out of the list in one pass, the rest in their order; once per list and batch. Returns the
// keys it added to the compacted map (0 or 1).
static UTDUST_LATE int32_t compactList(Instance*** listPtr, int32_t key) {
    uint32_t s = keySlot(compactedKey, key);
    if (compactedKey[s] != 0) return 0;
    compactedKey[s] = key + 1;
    Instance** list = *listPtr;
    int32_t n = (int32_t) arrlen(list);
    int32_t kept = 0;
    repeat(n, i) {
        Instance* inst = list[i];
        if (!setHas(inst)) list[kept++] = inst;
    }
    if (kept != n) arrsetlen(*listPtr, kept);
    return 1;
}

static inline bool compacted(int32_t key) {
    return compactedKey[keySlot(compactedKey, key)] != 0;
}

// Runner_removeInstanceFromObjectLists for the whole batch: each list loses the same entries and keeps the rest in
// the same order. A list compacted loses every entry of the batch's instances; one by one, an instance leaves each
// list once per visit of its parent chain, and it was put there once per visit (Runner_addInstanceToObjectLists;
// instance_change takes it out and puts it back).
static UTDUST_LATE void removeBatchFromObjectLists(Runner* runner, int32_t count) {
    if (BATCH_MIN > count) {
        repeat(count, i) Runner_removeInstanceFromObjectLists(runner, dead[i]);
        return;
    }
    DataWin* dataWin = runner->dataWin;
    uint32_t objectTotal = dataWin->objt.count;
    bool big = false;
    memset(objectKey, 0, sizeof(objectKey));
    int32_t distinct = 0;
    repeat(count, i) {
        int32_t obj = dead[i]->objectIndex;
        if (0 > obj || objectTotal <= (uint32_t) obj) continue;
        uint32_t s = keySlot(objectKey, obj);
        if (objectKey[s] == 0) {
            if (distinct == KEY_CAP) {
                big = false;
                break;
            }
            distinct++;
            objectKey[s] = obj + 1;
            objectCount[s] = 0;
        }
        if (++objectCount[s] == BATCH_MIN) big = true;
    }
    if (!big) {
        repeat(count, i) Runner_removeInstanceFromObjectLists(runner, dead[i]);
        return;
    }

    memset(deadSet, 0, sizeof(deadSet));
    repeat(count, i) {
        uint32_t s = setSlot(dead[i]);
        while (deadSet[s] != nullptr) s = (s + 1) & (SET_SIZE - 1);
        deadSet[s] = dead[i];
    }
    memset(compactedKey, 0, sizeof(compactedKey));
    int32_t compactedUsed = 0;
    repeat(KEY_SIZE, k) {
        if (objectKey[k] == 0 || BATCH_MIN > objectCount[k]) continue;
        int32_t obj = objectKey[k] - 1;
        // The keys this object can add: its exact list and each list of its chain (fewer if some are compacted).
        int32_t chain = 0;
        for (int32_t currentObj = obj; currentObj >= 0 && objectTotal > (uint32_t) currentObj && CHAIN_MAX > chain; chain++) {
            currentObj = dataWin->objt.objects[currentObj].parentId;
        }
        if (compactedUsed + 1 + chain > KEY_CAP) continue;
        compactedUsed += compactList(&runner->instancesByExactObject[obj], (obj << 1) | 1);
        int32_t currentObj = obj;
        int32_t depth = 0;
        while (currentObj >= 0 && objectTotal > (uint32_t) currentObj && CHAIN_MAX > depth) {
            compactedUsed += compactList(&runner->instancesByObject[currentObj], currentObj << 1);
            currentObj = dataWin->objt.objects[currentObj].parentId;
            depth++;
        }
    }
    // The rest: one by one from the lists not compacted, as before.
    repeat(count, i) {
        Instance* inst = dead[i];
        int32_t obj = inst->objectIndex;
        int32_t currentObj = obj;
        int32_t depth = 0;
        while (currentObj >= 0 && objectTotal > (uint32_t) currentObj && CHAIN_MAX > depth) {
            if (!compacted(currentObj << 1)) removeOne(&runner->instancesByObject[currentObj], inst);
            currentObj = dataWin->objt.objects[currentObj].parentId;
            depth++;
        }
        if (obj >= 0 && objectTotal > (uint32_t) obj && !compacted((obj << 1) | 1)) removeOne(&runner->instancesByExactObject[obj], inst);
    }
}

static UTDUST_LATE void flushDeadBatch(Runner* runner) {
    int32_t count = deadCount;
    if (count == 0) return;
    removeBatchFromObjectLists(runner, count);
    repeat(count, i) {
        Instance* inst = dead[i];
        SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
        Runner_removeInstanceLayerElement(runner, inst->instanceId);
        hmdel(runner->instancesById, inst->instanceId);
        Instance_free(inst);
        dead[i] = nullptr;
    }
    // Cached drawables hold raw Instance* that were just freed; force a rebuild before the next draw.
    runner->drawableListStructureDirty = true;
    deadCount = 0;
}

// The flush at the end of Runner_cleanupDestroyedInstances. UNDERTALE on the 3DS then takes the destroyed out of the
// draw list and merges in the instances made since it was last brought up to date (runner.c, utkill), when the
// list allows it; otherwise Runner_drawPre rebuilds it.
void Utdust_flushDestroyed(Runner* runner) {
    flushDeadBatch(runner);
#if defined(UTK_PLAIN_RUNS)
    extern bool Utk_keepDrawList(Runner* runner);
    if (runner->drawableListStructureDirty) Utk_keepDrawList(runner);
#endif
}

void Utdust_deferDestroyed(Runner* runner, Instance* inst) {
    if (deadCount == DEAD_CAP) flushDeadBatch(runner);
    dead[deadCount++] = inst;
}

// Instance_computeSpeedFromComponents exactly as this build compiles it (-ffast-math folds 180 / pi * 1e6 into one
// constant and the 1e-6 divide into a multiply; the hspeed == 0 branches skip the rounding), with its library calls
// done cheaper where the values allow it, the same to the bit:
// - clampFloat's int64 round trip (two runtime calls) as an int32 one: |180 / pi * 1e6 * atan2| <= 1.8e8 fits;
// - round(x) for 0 < x < 2e9 as (int32_t) (x + 0.5) (x is a float, so x + 0.5 is exact), and round(+-0) = +-0;
// - fmod(y, 360) for y in [-0, 360] (all a computed direction can be) as y, or +0 for 360.
// No -ffast-math here, so the compiler keeps each step as written: the constants are the build's own, in hex. An
// hspeed of 0, or a value outside these ranges (NaN, infinity), takes Instance_computeSpeedFromComponents itself.
static UTDUST_LATE __attribute__((optimize("no-fast-math"))) void speedFromComponents(Instance* inst) {
    float hspeed = inst->hspeed;
    float vspeed = inst->vspeed;
    if (hspeed == 0.0f) {
        Instance_computeSpeedFromComponents(inst);
        return;
    }
    double scaled = atan2((double) vspeed, (double) hspeed) * 0x1.b521d1c1acae8p+25;
    if (!(2.0e9 > fabs(scaled))) {
        Instance_computeSpeedFromComponents(inst);
        return;
    }
    double dd = (double) (int32_t) scaled * 0x1.0c6f7a0b5ed8dp-20;
    float direction = (dd > 0.0) ? (float) (360.0 - dd) : -(float) dd;
    double d = (double) direction;
    double rounded = (double) (int32_t) (d + 0.5);
    if (d == 0.0) rounded = d;
    double kept = (0x1.a36e2eb1c432dp-14 > fabs(d - rounded)) ? (double) (float) rounded : d;
    inst->direction = (float) ((kept >= 360.0) ? kept - 360.0 : kept);

    float speed = (float) sqrt((double) (hspeed * hspeed + vspeed * vspeed));
    double sd = (double) speed;
    double sr = (2.0e9 > sd) ? (double) (int32_t) (sd + 0.5) : round(sd);
    inst->speed = (0x1.a36e2eb1c432dp-14 > fabs(sd - sr)) ? (float) sr : speed;
}

// stepFrame's gravity: velocity added in gravity_direction (HTML5: AddTo_Speed), then speed and direction again.
void Utdust_applyGravity(Instance* inst) {
    uint32_t bits;
    memcpy(&bits, &inst->gravityDirection, sizeof(bits));
    if (!gravityKnown || bits != gravityBits) {
        GMLReal gravDirRad = inst->gravityDirection * (M_PI / 180.0);
        gravityCos = clampFloat(GMLReal_cos(gravDirRad));
        gravitySin = clampFloat(GMLReal_sin(gravDirRad));
        gravityBits = bits;
        gravityKnown = true;
    }
    inst->hspeed += (float) (inst->gravity * gravityCos);
    inst->vspeed -= (float) (inst->gravity * gravitySin);
    speedFromComponents(inst);
}

// Is the instance certainly inside the room for the Outside Room event? Collision_computeBBox's unrotated box, before
// any rounding, at least 1 pixel inside every edge: then the box the event tests, rounded or not, is inside, since
// rounding to an integer can't cross an integer edge (the margin also covers any difference in the last bit). When
// this says no, the caller works it out in full, as before. It saves the dust the four floor() calls of
// collision-compatibility rounding and the box's return.
bool Utdust_insideRoom(Runner* runner, Instance* inst, int32_t roomWidth, int32_t roomHeight) {
    DataWin* dataWin = runner->dataWin;
    int32_t sprIdx = (inst->maskIndex >= 0) ? inst->maskIndex : inst->spriteIndex;
    if (0 > sprIdx || (uint32_t) sprIdx >= dataWin->sprt.count) return false;
    if (!(0.0001 >= GMLReal_fabs(inst->imageAngle))) return false;
    Sprite* spr = &dataWin->sprt.sprites[sprIdx];
    GMLReal marginL = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginLeft;
    GMLReal marginR = (spr->bboxMode == 1) ? (GMLReal) spr->width : (GMLReal) (spr->marginRight + 1);
    GMLReal marginT = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginTop;
    GMLReal marginB = (spr->bboxMode == 1) ? (GMLReal) spr->height : (GMLReal) (spr->marginBottom + 1);
    GMLReal originX = (GMLReal) spr->originX;
    GMLReal originY = (GMLReal) spr->originY;
    GMLReal left   = inst->x + inst->imageXscale * (marginL - originX);
    GMLReal right  = inst->x + inst->imageXscale * (marginR - originX);
    GMLReal top    = inst->y + inst->imageYscale * (marginT - originY);
    GMLReal bottom = inst->y + inst->imageYscale * (marginB - originY);
    if (left > right) { GMLReal tmp = left; left = right; right = tmp; }
    if (top > bottom) { GMLReal tmp = top; top = bottom; bottom = tmp; }
    // Each test fails for a NaN, which then goes the full way too.
    return right >= 1.0 && (GMLReal) (roomWidth - 1) >= left && bottom >= 1.0 && (GMLReal) (roomHeight - 1) >= top;
}
