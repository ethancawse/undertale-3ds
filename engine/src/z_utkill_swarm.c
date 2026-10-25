// UNDERTALE's monster dust as a swarm, so that the dust costs less on the Old 3DS.
//
// A monster's dust is up to 1,600 instances of obj_whtpxlgrav at once: a 2x2 white pixel that rises under gravity and
// is destroyed at its sprite's end (12 frames) or when it leaves the room. Each one went through everything a runner
// instance costs: the allocation, its creator variable, the id map, the per-object lists, the spatial grid, a Create
// event dispatch, an Animation End event dispatch, the destroyed-instance cleanup, a draw list entry. The dust maker's
// Draw (z_utkill_vapor.c) now makes them here instead, as UtswPixel records (runner.h): what an instance of them holds
// that can differ between pixels or frames. Every other field is the same for all of them and never changes (nothing
// can see them to change it), and is what createAndInitInstance gives an instance of the object: swDraw holds those.
// The record has the same id, and its Create runs the same builtins with the same arguments on an instance with those
// fields (swMake), so the same random numbers and the same results. Its own passes here do what the runner's passes
// would do to the instance, in the same frame and with the same sums:
//
// - Utsw_animate (stepFrame, after the animate pass): the previous position saved, image_index advanced, and at the
//   sprite's end its Animation End (action_kill_object: destroyed).
// - Utsw_motion (stepFrame, before the motion pass): gravity (z_utdust.c's sums: the same cosine and sine), the move,
//   and the Outside Room test (Utdust_insideRoom's sums, else Collision_computeBBox) with its event (destroyed). No
//   friction, path or physics: the instance's are Instance_create's.
// - Draw: Utsw_merge (after utkDrawRoom's list rebuild) puts a span entry in the draw list for each run of pixels with
//   nothing else between them in the list's order (depth, then id), where their instances' entries would be, and
//   utkDrawRoom draws a span's pixels as it would draw their instances (runner.c's utswDrawSpan).
// - speed and direction are only worked out when something can see them (swBuild): from hspeed and vspeed, as the
//   runner's passes left them (Instance_computeSpeedFromComponents), with direction0 for the direction it keeps when
//   both are 0.
//
// Nothing in UNDERTALE 1.08 can tell (scratch/sw/scan.py, the results file lists every event and observer): the
// pixel's events are exactly Create, Outside Room and Animation End; no collision event names it; the only code that
// looks at its instances is instance_exists(obj_whtpxlgrav) (answered here), and the only with (all) is in
// obj_vsflowey_heartdefeated's Create. Anything that could see the instances turns the swarm into runner instances
// first (swMaterialize: built from the records with the same ids and fields, in every list where the runner would
// have them): a room change, a room restart or a game restart (main.c's call, Utsw_handleRoomChange),
// instance_(de)activate_*, the observer builtins given the pixel's object or all, and creating
// obj_vsflowey_heartdefeated. A request while the swarm and the runner's instances are between passes (Utsw_animate
// not yet run this frame, or the swarm moved before the runner's motion and Outside Room passes) is counted and logged
// ("mid-pass"): none can happen in UNDERTALE 1.08 (scan.py, section 6). The state hash (test builds) hashes the
// instances swBuild makes, where the runner would have them.
//
// The strips (swarm v3, utkill15). Undyne's neutral death and the Glad Dummy melt through obj_vaporized instead.
// Its Alarm 0 makes a row of obj_blkpxltall strips each frame, and each strip's Create makes one pixel and keeps its
// id: dt = instance_create(x, y, obj_whtpxlgrav). Each strip's Step is if (!instance_exists(dt)) instance_destroy().
// The strips go in the swarm too (swCreateStrip, from the instance_create wrapper), as records in swSt beside the
// pixels: the strip's id, then its pixel through swCreate with the strip as the caller (id + 1, the same Create),
// then the strip's creator. What a strip's instance would hold never changes: it doesn't move, image_index stays 0
// (one subimage) and it has no Outside Room event. Its Step is the one thing it does:
// - Its pixel is gone at the strips' turn in the Step pass if it went at this frame's Animation End or the frame
//   before's Outside Room, so Utsw_motion (just after the Step pass) drops those strips (swStripSteps).
// - Between the pixel's end and that point, a strip without its pixel is still there for anything that runs before
//   the strips' turn and gone for anything after it. The turn goes by object index (swStripPhase).
// - instance_exists and instance_number of obj_blkpxltall are answered that way.
// - A materialize builds the strips too, each with its dt and creator in the runner's order. One inside the Step
//   pass drops the strips without their pixel after their turn; before it, those strips would miss the pass's
//   snapshot.
// - Anything that can't be placed is counted and logged ("UNPROVEN"); none can happen in UNDERTALE 1.08.
// - The draw list gets span entries for runs of strips at their depth (Utsw_merge, Utsw_span), drawn as their
//   instances' draw_self would draw them.
// scratch/sw/scan3.py: the only code that looks at the strips' instances is instance_exists(obj_blkpxltall) in
// obj_vaporized's Step (object 158, before the strips' turn). Nothing reads a strip's id or user event 0. Only
// obj_vsflowey_heart's Step runs after the strips' turn, and making obj_vsflowey_heartdefeated materializes first.
//
// It binds at the first record (the first instance_create of obj_blkpxltall, through instance_create's wrapper, or the
// first dust pixel), as v2 did; until then every wrapper passes through, so it changes nothing that came before. At
// the first frame end only instance_create is wrapped (pointer checks) and the two objects are found by name: nothing
// reads bytecode there, which would load code groups early and leave the heap unlike the runner's without the swarm.
// - The pixel: by the object's name, its exact event list, and its three code entries by their bytecode
//   (Utkill_codeIs, functions by name).
// - The strips: by obj_blkpxltall's fields and exact event list, and its three entries and obj_vaporized's Alarm 0
//   by their digests (swCanon; scratch/sw/canon.py computes the same digests from data.win).
// - A strip mismatch leaves the strips runner instances. A pixel mismatch, the off-switch file
//   (sdmc:/3ds/undertale-3ds/utswarm-off) or anything unexpected leaves everything a runner instance.
// The file is looked for once, at the first frame end, and never in a dust maker's Draw, which often runs while a
// texture page is read whole in the background: an SD call waits behind that read (467 ms on the o2ds, utkill12).
//
// All code and data are late (.text.late.utswarm, .bss.late.utswarm; platform/3ds/late_reserve.ld).
// UNDERTALE 3DS builds only (runner.h, UTSW_ON).
#include "runner.h"

#if defined(UTSW_ON)

#include "collision.h"
#include "data_win.h"
#include "instance.h"
#include "log.h"
#include "math_compat.h"
#include "rvalue.h"
#include "stb_ds.h"
#include "utils.h"
#include "vm.h"
#include "vm_builtins.h"
#include "vm_fastpath.h"
#include "cli/state_hash.h"

#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SW_LATE __attribute__((noinline, section(".text.late.utswarm")))
#define SW_BSS __attribute__((section(".bss.late.utswarm")))
// Rare paths: compiled for size.
#define SW_COLD __attribute__((noinline, cold, section(".text.late.utswarm")))

void CtrBench_handleRoomChange(Runner* runner);

void Utsw_createDrop(VMContext* ctx, int32_t funcIndex, RValue* args, int32_t argCount) SW_LATE;
void Utsw_animate(Runner* runner) SW_LATE;
void Utsw_motion(Runner* runner) SW_LATE;
void Utsw_outsideDone(Runner* runner) SW_LATE;
void Utsw_merge(Runner* runner) SW_LATE;
const UtswPixel* Utsw_span(const Drawable* d, int32_t* count, Instance** drawInstance) SW_LATE;
void Utsw_handleRoomChange(Runner* runner) SW_COLD;
void Utsw_stateHashEndFrame(Runner* runner) SW_COLD;

enum { WHY_ROOM, WHY_ACTIVATE, WHY_OBSERVER, WHY_WITHALL, WHY_PENDING, WHY_TEST, WHY_COUNT };

// The swarm: swPx[swBase] to swPx[swEnd - 1], ascending id (= creation order); 32-byte aligned, two cache lines each.
static UtswPixel* swPx SW_BSS;
static int32_t swBase SW_BSS, swEnd SW_BSS, swCap SW_BSS;
// Utsw_merge's spans: each one's first pixel (an index into swPx) and count.
static int32_t* swSpanFirst SW_BSS; // stb_ds
static int32_t* swSpanCount SW_BSS; // stb_ds
static int32_t* swSpanAt SW_BSS;    // stb_ds: the list entry each span goes before (Utsw_merge's scratch)
static Instance* swDraw SW_BSS;     // an instance of the object as createAndInitInstance makes it: the span entries' and draws'
static Instance* swMake SW_BSS;     // the instance the Create runs on
static int32_t swObject SW_BSS;     // obj_whtpxlgrav once bound
static int32_t swHeart SW_BSS;      // obj_vsflowey_heartdefeated (its Create runs with (all))
static int32_t swRoomIndex SW_BSS;  // the room the swarm's pixels were made in (a room change ends the swarm)
static int32_t swCreatorVar SW_BSS; // ctx->creatorVarID when they were made
static int8_t swState SW_BSS;       // 0: not bound yet, 1: on, -1: off
static int32_t swArmPixel SW_BSS;   // obj_whtpxlgrav and obj_blkpxltall by name, for the bind's trigger
static int32_t swArmStrip SW_BSS;
static uint8_t swWindow SW_BSS;     // 1: before this frame's Utsw_animate, 2: between Utsw_motion and the Outside Room pass
static bool swMarked SW_BSS;        // the draw list may hold span entries
static bool swRoomSeen SW_BSS;
static bool swOffFile SW_BSS;       // the off-switch file was there at the first frame end
static uint32_t swMidPass SW_BSS;
static uint32_t swAdded SW_BSS;
static uint32_t swMaterialized SW_BSS;
static uint32_t swWhy[WHY_COUNT] SW_BSS;
static BuiltinFunc swRandom SW_BSS, swGravity SW_BSS, swHspeed SW_BSS;
static uint32_t swGravityBits SW_BSS;
static bool swGravityKnown SW_BSS;
static GMLReal swGravityCos SW_BSS, swGravitySin SW_BSS;
// The strips (obj_blkpxltall, each made with its pixel): swSt[swStBase] to swSt[swStEnd - 1], ascending id, as
// UtswPixel records (x, y, the starts and the previous position all where it was made; imageIndex its instance's,
// which stays 0; id and creator). A strip's pixel is the one made in its Create: id + 1.
static UtswPixel* swSt SW_BSS;
static int32_t swStBase SW_BSS, swStEnd SW_BSS, swStCap SW_BSS;
static int8_t* swSpanStrip SW_BSS;  // stb_ds: 1 for a span of strips (Utsw_merge's)
static Instance* swStripDraw SW_BSS; // an instance of obj_blkpxltall as createAndInitInstance makes it
static Instance* swStripMake SW_BSS; // the strip its pixel's creator reads
static int32_t swStripObject SW_BSS; // obj_blkpxltall once its strips are on (else -1)
static int32_t swVapObject SW_BSS;   // obj_vaporized (its Alarm 0 makes the strips)
static int32_t swDtVar SW_BSS;       // the strip's dt (its pixel's id)
#define SW_MAX_OBJECTS 4096        // swOwnerPhase's size; more objects leave the strips runner instances
static int8_t swOwnerPhase[SW_MAX_OBJECTS] SW_BSS; // per object: SWP_BEFORE, SWP_AFTER or SWP_UNKNOWN for a Step it owns
static uint32_t swOwnerCount SW_BSS;
static bool swAfterStep SW_BSS;     // from Utsw_motion (after the Step pass) to the next Utsw_animate
static bool swInAnimate SW_BSS;     // in Utsw_animate (before the Step pass)
static uint32_t swStripsAdded SW_BSS;
static uint32_t swUnproven SW_BSS;  // strip answers or materializations whose place in the Step pass isn't known

static inline int32_t swCount(void) {
    return swEnd - swBase;
}

static inline int32_t swStCount(void) {
    return swStEnd - swStBase;
}

// Pixels or strips.
static inline bool swAny(void) {
    return swEnd != swBase || swStEnd != swStBase;
}

enum { SWP_BEFORE, SWP_AFTER, SWP_UNKNOWN };

// ---[ Materialize ]---

// The instance the runner would hold for p: createAndInitInstance's (swCreate's sequence), its Create's results, and
// what the runner's passes left in it since. speed and direction: Instance_computeSpeedFromComponents from hspeed and
// vspeed, which is what the runner's motion pass leaves each frame (z_utdust.c's speedFromComponents is the same to
// the bit), and what the Create's action_set_hspeed left for a pixel not yet moved (the same call on the same values);
// direction0 is the direction that call keeps when both are 0.
static Instance* swBuild(Runner* runner, const UtswPixel* p) SW_COLD;
static Instance* swBuild(Runner* runner, const UtswPixel* p) {
    GameObject* o = &runner->dataWin->objt.objects[swObject];
    Instance* inst = Instance_create(p->id, swObject, (GMLReal) p->xstart, (GMLReal) p->ystart);
    inst->roomIndex = swRoomIndex;
    inst->spriteIndex = o->spriteId;
    inst->visible = o->visible;
    inst->solid = o->solid;
    inst->persistent = o->persistent;
    inst->depth = o->depth;
    inst->maskIndex = o->textureMaskId;
    inst->createEventFired = true;
    inst->gravityDirection = p->gravityDirection;
    inst->gravity = p->gravity;
    inst->hspeed = p->hspeed;
    inst->vspeed = p->vspeed;
    inst->direction = p->direction0;
    Instance_computeSpeedFromComponents(inst);
    inst->x = p->x;
    inst->y = p->y;
    inst->xprevious = p->xprevious;
    inst->yprevious = p->yprevious;
    inst->imageIndex = p->imageIndex;
    inst->outsideRoom = p->outsideRoom;
    if (swCreatorVar >= 0) {
        // Instance_setSelfVar with a real (RValue_makeIndependent returns a real as it is)
        RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, swCreatorVar);
        RValue_free(slot);
        *slot = RValue_makeReal((GMLReal) p->creator);
    }
    return inst;
}

// The draw list without its span entries.
static void swStrip(Runner* runner) SW_LATE;
static void swStrip(Runner* runner) {
    if (!swMarked) return;
    int32_t n = (int32_t) arrlen(runner->cachedDrawables);
    Drawable* l = runner->cachedDrawables;
    int32_t w = 0;
    for (int32_t i = 0; n > i; i++) {
        if (l[i].type == DRAWABLE_INSTANCE && UTSW_IS_SPAN(&l[i])) continue;
        if (w != i) l[w] = l[i];
        w++;
    }
    arrsetlen(runner->cachedDrawables, w);
    swMarked = false;
}

// An object list kept in id order (creation order, as appends with stable removal leave it) after the swarm's appends.
static void swSortList(Instance** list) SW_COLD;
static void swSortList(Instance** list) {
    int32_t n = (int32_t) arrlen(list);
    for (int32_t i = 1; n > i; i++) {
        Instance* x = list[i];
        int32_t j = i;
        while (j > 0 && list[j - 1]->instanceId > x->instanceId) { list[j] = list[j - 1]; j--; }
        list[j] = x;
    }
}

// Merges built (the swarm's instances, ascending id) into dst (a copy of runner->instances, or the array itself): each
// where the runner would have it, before the first instance with a larger id. Instances after the room's start are
// in ascending id order, and every one before has a smaller id than any pixel (ids only grow; the swarm never
// outlives a room).
static void swMergeInstances(Instance*** dst, Instance** built, int32_t k) SW_COLD;
static void swMergeInstances(Instance*** dst, Instance** built, int32_t k) {
    int32_t n = (int32_t) arrlen(*dst);
    arrsetlen(*dst, n + k);
    Instance** a = *dst;
    int32_t i = n - 1, j = k - 1, w = n + k - 1;
    while (j >= 0) {
        if (i >= 0 && a[i]->instanceId > built[j]->instanceId) a[w--] = a[i--];
        else a[w--] = built[j--];
    }
}

// Instance_setSelfVar with a real (RValue_makeIndependent returns a real as it is).
static void swSetReal(Instance* inst, int32_t varID, GMLReal v) SW_COLD;
static void swSetReal(Instance* inst, int32_t varID, GMLReal v) {
    RValue* slot = IntRValueHashMap_getOrInsertUndefined(&inst->selfVars, varID);
    RValue_free(slot);
    *slot = RValue_makeReal(v);
}

// The instance the runner would hold for strip s: createAndInitInstance's, then its Create's dt (its pixel's id), then
// builtin_instance_create's creator, in that order. It never moves (nothing can see it to move it), so x, xstart and
// xprevious are where it was made; its sprite has one subimage and image_speed is 1, so the animate pass leaves
// image_index at 0 every frame; it has no Outside Room event, so outsideRoom stays false.
static Instance* swBuildStrip(Runner* runner, const UtswPixel* s) SW_COLD;
static Instance* swBuildStrip(Runner* runner, const UtswPixel* s) {
    GameObject* o = &runner->dataWin->objt.objects[swStripObject];
    Instance* inst = Instance_create(s->id, swStripObject, (GMLReal) s->xstart, (GMLReal) s->ystart);
    inst->roomIndex = swRoomIndex;
    inst->spriteIndex = o->spriteId;
    inst->visible = o->visible;
    inst->solid = o->solid;
    inst->persistent = o->persistent;
    inst->depth = o->depth;
    inst->maskIndex = o->textureMaskId;
    inst->createEventFired = true;
    inst->x = s->x;
    inst->y = s->y;
    inst->xprevious = s->xprevious;
    inst->yprevious = s->yprevious;
    inst->imageIndex = s->imageIndex;
    swSetReal(inst, swDtVar, (GMLReal) (s->id + 1));
    if (swCreatorVar >= 0) swSetReal(inst, swCreatorVar, (GMLReal) s->creator);
    return inst;
}

// Whether strip s's pixel is gone (the pixel ids are ascending; *j is a cursor for strips taken in ascending order).
static inline bool swPixelGone(const UtswPixel* s, int32_t* j) {
    uint32_t pid = s->id + 1;
    while (swEnd > *j && pid > swPx[*j].id) (*j)++;
    return !(swEnd > *j && swPx[*j].id == pid);
}

// Strips whose pixel is gone (their Step destroys them at the strips' turn in the Step pass).
static int32_t swPendingStrips(void) SW_COLD;
static int32_t swPendingStrips(void) {
    int32_t n = 0, j = swBase;
    for (int32_t k = swStBase; swStEnd > k; k++) n += swPixelGone(&swSt[k], &j) ? 1 : 0;
    return n;
}

// Where now is against the strips' turn in the Step pass: SWP_BEFORE (a strip whose pixel is gone is still there;
// also anywhere outside the Step pass up to the next one), SWP_AFTER (it is gone), SWP_UNKNOWN (code the Step pass
// runs that can't be placed: counted and logged). The Step pass (event_table.c's bySlot, dispatched in order) runs
// object by object in ascending index, the strips' after every smaller object's and before every larger one's. In a
// Step, the event's owner (currentEventObjectIndex: the object whose code runs, which with () and scripts keep) places
// it when every object that can run a Step with that owner (it and its descendants) is on one side (swOwnerPhase).
static int swStripPhase(VMContext* ctx) SW_COLD;
static int swStripPhase(VMContext* ctx) {
    if (swAfterStep || swWindow == 1) return SWP_BEFORE;
    int32_t t = ctx->currentEventType, sub = ctx->currentEventSubtype;
    if (t == EVENT_STEP && sub == STEP_NORMAL) {
        int32_t owner = ctx->currentEventObjectIndex;
        if (owner >= 0 && swOwnerCount > (uint32_t) owner) return swOwnerPhase[owner];
        return SWP_UNKNOWN;
    }
    if (t == EVENT_ALARM || (t == EVENT_STEP && sub == STEP_BEGIN) || t == EVENT_KEYBOARD || t == EVENT_KEYPRESS ||
        t == EVENT_KEYRELEASE || t == EVENT_MOUSE)
        return SWP_BEFORE;
    return SWP_UNKNOWN;
}

static void swUnprovenAt(Runner* runner, const char* what, int32_t pending) SW_COLD;
static void swUnprovenAt(Runner* runner, const char* what, int32_t pending) {
    swUnproven++;
    logInfo("utswarm: UNPROVEN %s (%d strips without their pixel, frame %d) count %u\n", what, (int) pending,
            (int) runner->frameCount, (unsigned) swUnproven);
}

// Every pixel's and strip's instance (swBuild, swBuildStrip), ascending id; stb_ds. dropPending: no instance for a
// strip whose pixel is gone (its Step has run).
static Instance** swBuildAll(Runner* runner, bool dropPending) SW_COLD;
static Instance** swBuildAll(Runner* runner, bool dropPending) {
    Instance** built = nullptr;
    int32_t i = swBase, k = swStBase, j = swBase;
    while (swEnd > i || swStEnd > k) {
        if (swStEnd > k && (i >= swEnd || swPx[i].id > swSt[k].id)) {
            const UtswPixel* s = &swSt[k++];
            if (dropPending && swPixelGone(s, &j)) continue;
            arrput(built, swBuildStrip(runner, s));
        } else {
            arrput(built, swBuild(runner, &swPx[i++]));
        }
    }
    return built;
}

static void swMaterialize(Runner* runner, int why) SW_COLD;
static void swMaterialize(Runner* runner, int why) {
    if (!swAny()) return;
    int32_t pixels = swCount(), strips = swStCount();
    if (swWindow != 0 && pixels != 0) {
        swMidPass++;
        logInfo("utswarm: MID-PASS materialize (window %d, reason %d, %d pixels, frame %d) count %u\n", (int) swWindow,
                why, (int) pixels, (int) runner->frameCount, (unsigned) swMidPass);
    }
    // A strip whose pixel is gone, made an instance inside the Step pass, would miss the snapshot of that pass (and
    // its Step) before its turn, and must be gone after it.
    bool dropPending = false;
    if (strips != 0 && !swAfterStep && swWindow != 1 && !swInAnimate) {
        int32_t pending = swPendingStrips();
        if (pending != 0) {
            VMContext* ctx = runner->vmContext;
            int32_t t = ctx->currentEventType;
            int phase = swStripPhase(ctx);
            bool beforePass = phase == SWP_BEFORE && !(t == EVENT_STEP && ctx->currentEventSubtype == STEP_NORMAL);
            dropPending = phase == SWP_AFTER;
            if (!beforePass) swUnprovenAt(runner, "materialize", pending);
        }
    }
    Instance** built = swBuildAll(runner, dropPending);
    int32_t k = (int32_t) arrlen(built);
    swStrip(runner);
    swMergeInstances(&runner->instances, built, k);
    for (int32_t j = 0; k > j; j++) {
        Instance* inst = built[j];
        __typeof__(runner->instancesById->key) key = inst->instanceId;
        hmput(runner->instancesById, key, inst);
        Runner_addInstanceToObjectLists(runner, inst);
    }
    arrfree(built);
    swSortList(runner->instancesByExactObject[swObject]);
    swSortList(runner->instancesByObject[swObject]);
    if (strips != 0) {
        swSortList(runner->instancesByExactObject[swStripObject]);
        swSortList(runner->instancesByObject[swStripObject]);
    }
    runner->drawableListStructureDirty = true;
    swBase = swEnd = 0;
    swStBase = swStEnd = 0;
    swMaterialized += (uint32_t) k;
    swWhy[why]++;
    logInfo("utswarm: materialized %d pixels, %d strips (reason %d, frame %d)\n", (int) pixels, (int) strips, why,
            (int) runner->frameCount);
}

// ---[ Observers ]---

static BuiltinFunc swOrigExists SW_BSS, swOrigNumber SW_BSS, swOrigCreate SW_BSS;

// An object or instance argument as RValue_toInt32 reads a real or an int32; SW_UNKNOWN for any other type, which the
// callers treat as naming the swarm (it becomes instances, and the builtin then answers for itself).
#define SW_UNKNOWN INT32_MIN
static inline int32_t swTarget(const RValue* v) {
    if (v->type == RVALUE_REAL) return (int32_t) v->real;
    if (v->type == RVALUE_INT32) return v->int32;
    return SW_UNKNOWN;
}

static inline bool swNames(RValue* args, int32_t argCount, int32_t at) {
    if (argCount <= at) return false;
    int32_t target = swTarget(&args[at]);
    return target == swObject || target == INSTANCE_ALL || target == SW_UNKNOWN ||
        (target == swStripObject && swStripObject >= 0);
}

// How many strips there are now, as the runner would have them (swStripPhase).
static int32_t swStripsNow(VMContext* ctx, const char* what) SW_COLD;
static int32_t swStripsNow(VMContext* ctx, const char* what) {
    int32_t n = swStCount();
    if (n == 0) return 0;
    int phase = swStripPhase(ctx);
    if (phase == SWP_BEFORE) return n;
    int32_t pending = swPendingStrips();
    if (pending == 0) return n;
    if (phase == SWP_AFTER) return n - pending;
    swUnprovenAt(ctx->runner, what, pending);
    return n;
}

static RValue swInstanceExists(VMContext* ctx, RValue* args, int32_t argCount) SW_LATE;
static RValue swInstanceExists(VMContext* ctx, RValue* args, int32_t argCount) {
    if (swAny() && argCount >= 1) {
        int32_t target = swTarget(&args[0]);
        if (target == swObject && swCount() != 0) return RValue_makeBool(true);
        if (target == swStripObject && swStripObject >= 0 && swStripsNow(ctx, "instance_exists") != 0)
            return RValue_makeBool(true);
        if (target == INSTANCE_ALL || target == SW_UNKNOWN) swMaterialize(ctx->runner, WHY_OBSERVER);
    }
    return swOrigExists(ctx, args, argCount);
}

static RValue swInstanceNumber(VMContext* ctx, RValue* args, int32_t argCount) SW_LATE;
static RValue swInstanceNumber(VMContext* ctx, RValue* args, int32_t argCount) {
    int32_t extra = 0;
    if (swAny() && argCount >= 1) {
        int32_t target = swTarget(&args[0]);
        if (target == swObject) extra = swCount();
        else if (target == swStripObject && swStripObject >= 0) extra = swStripsNow(ctx, "instance_number");
        else if (target == INSTANCE_ALL || target == SW_UNKNOWN) swMaterialize(ctx->runner, WHY_OBSERVER);
    }
    RValue r = swOrigNumber(ctx, args, argCount);
    if (extra != 0 && r.type == RVALUE_REAL) r.real += (GMLReal) extra;
    return r;
}

static bool swCreateStrip(VMContext* ctx, RValue* args, RValue* out) SW_LATE;

static RValue swInstanceCreate(VMContext* ctx, RValue* args, int32_t argCount) SW_LATE;
static void swBindLate(Runner* runner) SW_COLD;

static RValue swInstanceCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    if (swState <= 0) {
        if (swState == 0 && argCount >= 3 && swTarget(&args[2]) == swArmStrip) swBindLate(ctx->runner);
        if (swState <= 0) return swOrigCreate(ctx, args, argCount);
    }
    if (argCount >= 3) {
        int32_t target = swTarget(&args[2]);
        if (target == swStripObject && swStripObject >= 0) {
            RValue out;
            if (swCreateStrip(ctx, args, &out)) return out;
        }
        if (swAny() && (target == swHeart || target == SW_UNKNOWN)) swMaterialize(ctx->runner, WHY_WITHALL);
    }
    return swOrigCreate(ctx, args, argCount);
}

// Builtins that look at the instances of the object (or all) in argument AT, or at every instance (AT < 0): the
// swarm becomes instances first.
static void swGuardCheck(VMContext* ctx, RValue* args, int32_t argCount, int32_t at) SW_COLD;
static void swGuardCheck(VMContext* ctx, RValue* args, int32_t argCount, int32_t at) {
    if (!swAny()) return;
    if (0 > at) swMaterialize(ctx->runner, WHY_ACTIVATE);
    else if (swNames(args, argCount, at)) swMaterialize(ctx->runner, WHY_OBSERVER);
}
#define SW_GUARD(slot, at)                                                                                           \
static BuiltinFunc swOrig##slot SW_BSS;                                                                              \
static RValue swGuard##slot(VMContext* ctx, RValue* args, int32_t argCount) SW_LATE;                                 \
static RValue swGuard##slot(VMContext* ctx, RValue* args, int32_t argCount) {                                        \
    if (swAny()) swGuardCheck(ctx, args, argCount, (at));                                                            \
    return swOrig##slot(ctx, args, argCount);                                                                        \
}
#define SW_ALL(slot) SW_GUARD(slot, -1)

SW_GUARD(Find, 0)
SW_GUARD(Nearest, 2)
SW_GUARD(Furthest, 2)
SW_GUARD(Place, 2)
SW_GUARD(Position, 2)
SW_GUARD(PlaceMeeting, 2)
SW_GUARD(PositionMeeting, 2)
SW_GUARD(CollPoint, 2)
SW_GUARD(CollRect, 4)
SW_GUARD(CollCircle, 3)
SW_GUARD(CollEllipse, 4)
SW_GUARD(CollLine, 4)
SW_GUARD(Distance, 0)
SW_GUARD(Destroy, 0)
SW_GUARD(DeactObject, 0)
SW_GUARD(ActObject, 0)
SW_ALL(DeactAll)
SW_ALL(ActAll)
SW_ALL(DeactRegion)
SW_ALL(ActRegion)
SW_ALL(PlaceEmpty)
SW_ALL(PositionEmpty)
SW_ALL(PositionDestroy)
SW_ALL(PositionChange)
SW_ALL(ContactAll)
SW_ALL(OutsideAll)
SW_ALL(BounceAll)

typedef struct {
    const char* name;
    BuiltinFunc* orig;
    BuiltinFunc wrapper;
} SwWrap;

// Replaces each named builtin in the call cache (every call of a builtin, interpreted or generated, goes through
// it), keeping the original. False when one isn't the builtin it should be.
static bool swWrapBuiltins(VMContext* ctx) SW_COLD;
static bool swWrapBuiltins(VMContext* ctx) {
    static const SwWrap wraps[] = {
        { "instance_exists", &swOrigExists, swInstanceExists },
        { "instance_number", &swOrigNumber, swInstanceNumber },
        { "instance_create", &swOrigCreate, swInstanceCreate },
        { "instance_find", &swOrigFind, swGuardFind },
        { "instance_nearest", &swOrigNearest, swGuardNearest },
        { "instance_furthest", &swOrigFurthest, swGuardFurthest },
        { "instance_place", &swOrigPlace, swGuardPlace },
        { "instance_position", &swOrigPosition, swGuardPosition },
        { "place_meeting", &swOrigPlaceMeeting, swGuardPlaceMeeting },
        { "position_meeting", &swOrigPositionMeeting, swGuardPositionMeeting },
        { "collision_point", &swOrigCollPoint, swGuardCollPoint },
        { "collision_rectangle", &swOrigCollRect, swGuardCollRect },
        { "collision_circle", &swOrigCollCircle, swGuardCollCircle },
        { "collision_ellipse", &swOrigCollEllipse, swGuardCollEllipse },
        { "collision_line", &swOrigCollLine, swGuardCollLine },
        { "distance_to_object", &swOrigDistance, swGuardDistance },
        { "instance_destroy", &swOrigDestroy, swGuardDestroy },
        { "instance_deactivate_object", &swOrigDeactObject, swGuardDeactObject },
        { "instance_activate_object", &swOrigActObject, swGuardActObject },
        { "instance_deactivate_all", &swOrigDeactAll, swGuardDeactAll },
        { "instance_activate_all", &swOrigActAll, swGuardActAll },
        { "instance_deactivate_region", &swOrigDeactRegion, swGuardDeactRegion },
        { "instance_activate_region", &swOrigActRegion, swGuardActRegion },
        { "place_empty", &swOrigPlaceEmpty, swGuardPlaceEmpty },
        { "position_empty", &swOrigPositionEmpty, swGuardPositionEmpty },
        { "position_destroy", &swOrigPositionDestroy, swGuardPositionDestroy },
        { "position_change", &swOrigPositionChange, swGuardPositionChange },
        { "move_contact_all", &swOrigContactAll, swGuardContactAll },
        { "move_outside_all", &swOrigOutsideAll, swGuardOutsideAll },
        { "move_bounce_all", &swOrigBounceAll, swGuardBounceAll },
    };
    uint32_t count = ctx->dataWin->func.functionCount;
    if (ctx->funcCallCacheCount != count) return false;
    for (uint32_t w = 0; sizeof(wraps) / sizeof(wraps[0]) > w; w++) *wraps[w].orig = VM_findBuiltin(ctx, wraps[w].name);
    for (uint32_t i = 0; count > i; i++) {
        const char* name = ctx->dataWin->func.functions[i].name;
        if (name == nullptr) continue;
        for (uint32_t w = 0; sizeof(wraps) / sizeof(wraps[0]) > w; w++) {
            if (strcmp(name, wraps[w].name) != 0) continue;
            FuncCallCache* c = &ctx->funcCallCache[i];
            if (c->builtin == nullptr) break; // a script of that name: not a builtin call
            if (*wraps[w].orig == nullptr || c->forward != nullptr) return false;
            if (c->builtin != (void*) *wraps[w].orig && c->builtin != (void*) wraps[w].wrapper) return false;
            break;
        }
    }
    for (uint32_t i = 0; count > i; i++) {
        const char* name = ctx->dataWin->func.functions[i].name;
        if (name == nullptr) continue;
        for (uint32_t w = 0; sizeof(wraps) / sizeof(wraps[0]) > w; w++) {
            if (strcmp(name, wraps[w].name) != 0) continue;
            FuncCallCache* c = &ctx->funcCallCache[i];
            if (c->builtin != nullptr) c->builtin = (void*) wraps[w].wrapper;
            break;
        }
    }
    return true;
}

// instance_create alone (the first frame end): its wrapper makes the strips and runs the bind at the first one.
static bool swWrapCreate(VMContext* ctx) SW_COLD;
static bool swWrapCreate(VMContext* ctx) {
    uint32_t count = ctx->dataWin->func.functionCount;
    if (ctx->funcCallCacheCount != count) return false;
    swOrigCreate = VM_findBuiltin(ctx, "instance_create");
    if (swOrigCreate == nullptr) return false;
    for (uint32_t i = 0; count > i; i++) {
        const char* name = ctx->dataWin->func.functions[i].name;
        if (name == nullptr || strcmp(name, "instance_create") != 0) continue;
        FuncCallCache* c = &ctx->funcCallCache[i];
        if (c->builtin == nullptr) continue;
        if (c->builtin != (void*) swOrigCreate || c->forward != nullptr) return false;
    }
    for (uint32_t i = 0; count > i; i++) {
        const char* name = ctx->dataWin->func.functions[i].name;
        if (name == nullptr || strcmp(name, "instance_create") != 0) continue;
        FuncCallCache* c = &ctx->funcCallCache[i];
        if (c->builtin != nullptr) c->builtin = (void*) swInstanceCreate;
    }
    return true;
}

// ---[ Bind ]---

// z_utkill_vapor.c's bytecode matcher (functions by name, so other function indices still match) and the pixel's
// pure Create as its template.
typedef struct {
    uint32_t word;
    const char* name;
} UtkillCall;
bool Utkill_codeIs(DataWin* dw, int32_t codeId, const uint32_t* words, uint32_t wordCount, const UtkillCall* calls,
                   uint32_t callCount);
extern const uint32_t Utkill_pixelCreateWords[24];
extern const UtkillCall Utkill_pixelCreateCalls[4];

// Outside Room and Animation End: action_kill_object().
static const uint32_t swKillWords[3] = { 0xD9020000, 0, 0x9E050000 };
static const UtkillCall swKillCalls[1] = { { 1, "action_kill_object" } };

static int32_t swFindObject(DataWin* dw, const char* name) SW_COLD;
static int32_t swFindObject(DataWin* dw, const char* name) {
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        if (dw->objt.objects[i].name != nullptr && strcmp(dw->objt.objects[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

// The event's single code action, if it is the named entry and its bytecode is the template: its code entry, else -1.
static int32_t swEventIs(DataWin* dw, const ObjectEvent* ev, uint32_t subtype, const char* codeName,
                         const uint32_t* words, uint32_t wordCount, const UtkillCall* calls, uint32_t callCount) SW_COLD;
static int32_t swEventIs(DataWin* dw, const ObjectEvent* ev, uint32_t subtype, const char* codeName,
                         const uint32_t* words, uint32_t wordCount, const UtkillCall* calls, uint32_t callCount) {
    if (ev->eventSubtype != subtype || ev->actionCount != 1) return -1;
    const EventAction* a = &ev->actions[0];
    if (a->kind != 7 || a->exeType != 2 || a->who != -1 || a->relative || a->isNot) return -1;
    if (0 > a->codeId || (uint32_t) a->codeId >= dw->code.count) return -1;
    CodeEntry* code = &dw->code.entries[a->codeId];
    if (code->name == nullptr || strcmp(code->name, codeName) != 0) return -1;
    if (!Utkill_codeIs(dw, a->codeId, words, wordCount, calls, callCount)) {
        logInfo("utswarm: %s is not the expected bytecode\n", codeName);
        return -1;
    }
    return a->codeId;
}

// The function a matched entry calls at that word.
static uint32_t swCallAt(DataWin* dw, int32_t codeId, uint32_t word) SW_COLD;
static uint32_t swCallAt(DataWin* dw, int32_t codeId, uint32_t word) {
    uint32_t w;
    memcpy(&w, DataWin_codeBytes(dw, &dw->code.entries[codeId]) + 4 * word, sizeof w);
    return w;
}

static bool swFuncIs(VMContext* ctx, uint32_t index, const char* name) SW_COLD;
static bool swFuncIs(VMContext* ctx, uint32_t index, const char* name) {
    return ctx->dataWin->func.functionCount > index && ctx->dataWin->func.functions[index].name != nullptr &&
        strcmp(ctx->dataWin->func.functions[index].name, name) == 0 && ctx->funcCallCache[index].builtin != nullptr &&
        ctx->funcCallCache[index].builtin == (void*) VM_findBuiltin(ctx, name);
}

// swCanon: FNV-1a 64 over a code entry's instructions (as the VM runs them, operands patched): each instruction word,
// then a call's function by its name, a variable operand by its type bits and its name, a Push.s by its text (each
// name or text with its NUL), any other operand bytes as they are. Sizes as gmlaot.Instr's. scratch/sw/canon.py
// gives the same digest from data.win. False when the entry isn't exactly wordCount words long or doesn't read
// straight through.
static uint64_t swFnv(uint64_t h, const void* p, size_t n) SW_COLD;
static uint64_t swFnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*) p;
    for (size_t i = 0; n > i; i++) {
        h ^= b[i];
        h *= 0x100000001B3ULL;
    }
    return h;
}

static bool swCanon(DataWin* dw, int32_t codeId, uint32_t wordCount, uint64_t want) SW_COLD;
static bool swCanon(DataWin* dw, int32_t codeId, uint32_t wordCount, uint64_t want) {
    if (0 > codeId || (uint32_t) codeId >= dw->code.count) return false;
    CodeEntry* code = &dw->code.entries[codeId];
    if (code->offset != 0 || code->length != wordCount * 4) return false;
    const uint8_t* b = DataWin_codeBytes(dw, code);
    if (b == nullptr) return false;
    uint64_t h = 0xCBF29CE484222325ULL;
    uint32_t len = code->length, addr = 0;
    while (len > addr) {
        uint32_t w;
        memcpy(&w, b + addr, sizeof w);
        uint32_t op = w >> 24, t1 = (w >> 16) & 0xF, size = 4;
        if (op == 0xC0 || op == 0xC1 || op == 0xC2 || op == 0xC3 || op == 0x45 || op == 0xD9 || op == 0xFF) {
            if (t1 == 0 || t1 == 3) size = 12;                                   // double, int64
            else if (t1 == 1 || t1 == 2 || t1 == 4 || t1 == 5 || t1 == 6) size = 8; // float, int32, bool, var, string
        }
        if (addr + size > len) return false;
        h = swFnv(h, &w, 4);
        if (size > 4) {
            uint32_t x;
            memcpy(&x, b + addr + 4, sizeof x);
            const char* name = nullptr;
            if (op == 0xD9) {
                if (x >= dw->func.functionCount || (name = dw->func.functions[x].name) == nullptr) return false;
            } else if (op == 0xC1 || op == 0xC2 || op == 0xC3 || op == 0x45 || (op == 0xC0 && t1 == 5)) {
                uint32_t vi = x & 0x07FFFFFF, bits = x & 0xF8000000;
                if (vi >= dw->vari.variableCount || (name = dw->vari.variables[vi].name) == nullptr) return false;
                h = swFnv(h, &bits, 4);
            } else if (op == 0xC0 && t1 == 6) {
                if (x >= dw->strg.count || (name = dw->strg.strings[x]) == nullptr) return false;
            }
            if (name != nullptr) h = swFnv(h, name, strlen(name) + 1);
            else h = swFnv(h, b + addr + 4, size - 4);
        }
        addr += size;
    }
    return h == want;
}

// The event's single code action, if it is the named entry with the digest: its code entry, else -1.
static int32_t swEventDigest(DataWin* dw, const ObjectEvent* ev, uint32_t subtype, const char* codeName,
                             uint32_t wordCount, uint64_t digest) SW_COLD;
static int32_t swEventDigest(DataWin* dw, const ObjectEvent* ev, uint32_t subtype, const char* codeName,
                             uint32_t wordCount, uint64_t digest) {
    if (ev->eventSubtype != subtype || ev->actionCount != 1) return -1;
    const EventAction* a = &ev->actions[0];
    if (a->kind != 7 || a->exeType != 2 || a->who != -1 || a->relative || a->isNot) return -1;
    if (0 > a->codeId || (uint32_t) a->codeId >= dw->code.count) return -1;
    CodeEntry* code = &dw->code.entries[a->codeId];
    if (code->name == nullptr || strcmp(code->name, codeName) != 0) return -1;
    if (!swCanon(dw, a->codeId, wordCount, digest)) {
        logInfo("utswarm: %s is not the expected bytecode\n", codeName);
        return -1;
    }
    return a->codeId;
}

// The strips (swarm v3): obj_blkpxltall exactly as UNDERTALE 1.08 has it (Steam and GOG), and obj_vaporized's Alarm 0
// that makes them:
//     Create:      dt = instance_create(x, y, obj_whtpxlgrav)
//     Step:        if (!instance_exists(dt)) instance_destroy()
//     User event 0: dt.image_xscale = image_xscale   (nothing calls it: scratch/sw/scan3.py)
// No parent or children, not solid or persistent, no physics; a sprite of one subimage (image_index stays 0) and no
// Draw event (drawn as draw_self). Null when they can go in the swarm (swStripObject, swVapObject and swDtVar set).
static const char* swStripBind(VMContext* ctx) SW_COLD;
static const char* swStripBind(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    int32_t obj = swFindObject(dw, "obj_blkpxltall"), vap = swFindObject(dw, "obj_vaporized");
    if (obj < 0 || vap < 0) return "objects";
    GameObject* o = &dw->objt.objects[obj];
    if (o->parentId >= 0 || o->persistent || o->solid || o->usesPhysics || o->spriteId < 0 ||
        (uint32_t) o->spriteId >= dw->sprt.count)
        return "object fields";
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        if (dw->objt.objects[i].parentId == obj) return "child object";
    }
    Sprite* spr = &dw->sprt.sprites[o->spriteId];
    if (spr->specialType || spr->messages != nullptr || spr->textureCount != 1) return "sprite";
    int32_t createCode = -1;
    for (int32_t t = 0; OBJT_EVENT_TYPE_COUNT > t; t++) {
        const ObjectEventList* list = &o->eventLists[t];
        if (t == EVENT_CREATE || t == EVENT_STEP || t == EVENT_OTHER) {
            if (list->eventCount != 1) return "events";
            const ObjectEvent* ev = &list->events[0];
            int32_t c;
            if (t == EVENT_CREATE)
                c = createCode = swEventDigest(dw, ev, 0, "gml_Object_obj_blkpxltall_Create_0", 10, 0xABF93611B7671536ULL);
            else if (t == EVENT_STEP)
                c = swEventDigest(dw, ev, STEP_NORMAL, "gml_Object_obj_blkpxltall_Step_0", 10, 0xC7B99824BFE67731ULL);
            else
                c = swEventDigest(dw, ev, 10, "gml_Object_obj_blkpxltall_Other_10", 7, 0x9B1A9F7F997F2FABULL);
            if (c < 0) return "strip code";
        } else if (list->eventCount != 0) {
            return "other event type";
        }
    }
    // obj_vaporized's Alarm 0 (the two instance_create calls of the strip)
    const ObjectEventList* alarms = &dw->objt.objects[vap].eventLists[EVENT_ALARM];
    int32_t alarmCode = -1;
    for (uint32_t e = 0; alarms->eventCount > e; e++) {
        if (alarms->events[e].eventSubtype != 0) continue;
        alarmCode = swEventDigest(dw, &alarms->events[e], 0, "gml_Object_obj_vaporized_Alarm_0", 175, 0x5CD076406286C499ULL);
    }
    if (alarmCode < 0) return "obj_vaporized Alarm 0";
    // dt: the variable the Create's pop (word 8, its operand word 9) stores
    uint32_t pop = swCallAt(dw, createCode, 9);
    uint32_t vi = pop & 0x07FFFFFF;
    if (vi >= dw->vari.variableCount || dw->vari.variables[vi].name == nullptr ||
        strcmp(dw->vari.variables[vi].name, "dt") != 0)
        return "dt";
    // swOwnerPhase: each object's descendants' (and its own) indices against the strips': BEFORE when all are smaller,
    // AFTER when all are larger, else UNKNOWN (each object is its own descendant, so every entry is set). No heap use:
    // the bind runs at the first frame end, and the heap's later layout stays as v2 left it.
    uint32_t count = dw->objt.count;
    if (count > SW_MAX_OBJECTS) return "objects";
    enum { SWP_UNSET = 3 };
    for (uint32_t i = 0; count > i; i++) swOwnerPhase[i] = SWP_UNSET;
    for (uint32_t c = 0; count > c; c++) {
        int8_t side = (int8_t) ((int32_t) c < obj ? SWP_BEFORE : (int32_t) c > obj ? SWP_AFTER : SWP_UNKNOWN);
        int32_t a = (int32_t) c;
        for (uint32_t depth = 0; a >= 0 && (uint32_t) a < count; depth++) {
            if (depth > count) {
                swOwnerCount = 0;
                return "parent loop";
            }
            if (swOwnerPhase[a] == SWP_UNSET) swOwnerPhase[a] = side;
            else if (swOwnerPhase[a] != side) swOwnerPhase[a] = SWP_UNKNOWN;
            a = dw->objt.objects[a].parentId;
        }
    }
    swOwnerCount = count;
    swDtVar = (int32_t) dw->vari.variables[vi].varID;
    swStripObject = obj;
    swVapObject = vap;
    return nullptr;
}

static const char* swBind(VMContext* ctx) SW_COLD;
static const char* swBind(VMContext* ctx) {
    swStripObject = -1;
    if (swOffFile) return "off-switch file";
    DataWin* dw = ctx->dataWin;
    if (DataWin_isVersionAtLeast(dw, 2, 0, 0, 0)) return "not GMS1";
    int32_t obj = swFindObject(dw, "obj_whtpxlgrav");
    if (obj < 0) return "object";
    swHeart = swFindObject(dw, "obj_vsflowey_heartdefeated");
    if (swHeart < 0) return "heartdefeated";
    GameObject* o = &dw->objt.objects[obj];
    if (o->parentId >= 0 || o->persistent || o->usesPhysics || o->spriteId < 0 || (uint32_t) o->spriteId >= dw->sprt.count)
        return "object fields";
    for (uint32_t i = 0; dw->objt.count > i; i++) {
        if (dw->objt.objects[i].parentId == obj) return "child object";
    }
    Sprite* spr = &dw->sprt.sprites[o->spriteId];
    if (spr->specialType || spr->messages != nullptr || spr->textureCount == 0) return "sprite";
    // Exactly Create, Outside Room and Animation End.
    int32_t createCode = -1, killCode = -1;
    for (int32_t t = 0; OBJT_EVENT_TYPE_COUNT > t; t++) {
        const ObjectEventList* list = &o->eventLists[t];
        if (t == EVENT_CREATE) {
            if (list->eventCount == 1)
                createCode = swEventIs(dw, &list->events[0], 0, "gml_Object_obj_whtpxlgrav_Create_0",
                                       Utkill_pixelCreateWords, 24, Utkill_pixelCreateCalls, 4);
            if (createCode < 0) return "Create";
        } else if (t == EVENT_OTHER) {
            if (list->eventCount != 2) return "Other events";
            const ObjectEvent* outside = list->events[0].eventSubtype == OTHER_OUTSIDE_ROOM ? &list->events[0] : &list->events[1];
            const ObjectEvent* animEnd = outside == &list->events[0] ? &list->events[1] : &list->events[0];
            killCode = swEventIs(dw, outside, OTHER_OUTSIDE_ROOM, "gml_Object_obj_whtpxlgrav_Other_0", swKillWords, 3,
                                 swKillCalls, 1);
            if (killCode < 0 || swEventIs(dw, animEnd, OTHER_ANIMATION_END, "gml_Object_obj_whtpxlgrav_Other_7",
                                          swKillWords, 3, swKillCalls, 1) < 0)
                return "Other";
        } else if (list->eventCount != 0) {
            return "other event type";
        }
    }
    // The functions those entries call (Steam and GOG 1.08: 3, 254, 255 and 222), each the engine's builtin.
    uint32_t fRandom = swCallAt(dw, createCode, 5), fGravity = swCallAt(dw, createCode, 13);
    uint32_t fHspeed = swCallAt(dw, createCode, 22), fKill = swCallAt(dw, killCode, 1);
    if (swCallAt(dw, createCode, 18) != fRandom || !swFuncIs(ctx, fRandom, "random") ||
        !swFuncIs(ctx, fGravity, "action_set_gravity") || !swFuncIs(ctx, fHspeed, "action_set_hspeed") ||
        !swFuncIs(ctx, fKill, "action_kill_object"))
        return "functions";
    swRandom = (BuiltinFunc) ctx->funcCallCache[fRandom].builtin;
    swGravity = (BuiltinFunc) ctx->funcCallCache[fGravity].builtin;
    swHspeed = (BuiltinFunc) ctx->funcCallCache[fHspeed].builtin;
    swObject = obj;
    const char* strips = swStripBind(ctx);
    if (strips != nullptr) {
        swStripObject = -1;
        logInfo("utswarm: strips off (%s)\n", strips);
    }
    if (!swWrapBuiltins(ctx)) {
        swStripObject = -1;
        return "builtins";
    }
    return nullptr;
}

// ---[ Create ]---

static inline RValue swReal(GMLReal v, uint8_t gmlType) {
    RValue r;
    memset(&r, 0, sizeof r);
    r.real = v;
    r.type = RVALUE_REAL;
    r.gmlStackType = gmlType;
    return r;
}

static inline RValue swInt(int32_t v, uint8_t gmlType) {
    RValue r;
    memset(&r, 0, sizeof r);
    r.int32 = v;
    r.type = RVALUE_INT32;
    r.gmlStackType = gmlType;
    return r;
}

#define SW_OPAQUE(v) __asm__("" : "+m"(v))

// The pixel's Create as z_utkill_vapor.c's Utkill_pixelCreate runs it (the code bound there):
//     action_set_gravity(90, random(0.5) + 0.2); action_set_hspeed(random(4) - 2);
static void swPixelCreate(VMContext* ctx) SW_LATE;
static void swPixelCreate(VMContext* ctx) {
    RValue a[2];
    a[0] = swReal((GMLReal) 0.5, GML_TYPE_VARIABLE);
    RValue r = swRandom(ctx, a, 1);
    r.gmlStackType = GML_TYPE_VARIABLE;
    RValue kv;
    {
        uint64_t bits = 0x3FC999999999999AULL; // 0.2
        double d;
        memcpy(&d, &bits, sizeof d);
        kv = swReal((GMLReal) d, GML_TYPE_DOUBLE);
    }
    vmArithNumbers(&r, &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);
    SW_OPAQUE(r);
    a[0] = swInt(90, GML_TYPE_VARIABLE);
    a[1] = r;
    RValue res = swGravity(ctx, a, 2);
    RValue_free(&res);
    a[0] = swInt(4, GML_TYPE_VARIABLE);
    r = swRandom(ctx, a, 1);
    r.gmlStackType = GML_TYPE_VARIABLE;
    const RValue k2 = swInt(2, GML_TYPE_INT16);
    vmArithNumbers(&r, &k2, VM_ARITH_SUB, GML_TYPE_VARIABLE);
    SW_OPAQUE(r);
    res = swHspeed(ctx, &r, 1);
    RValue_free(&res);
}

// An instance of the object as createAndInitInstance makes it, before its Create (swCreate's sequence), at (0, 0)
// with id 0: never in any runner list.
static Instance* swProto(Runner* runner, int32_t object) SW_COLD;
static Instance* swProto(Runner* runner, int32_t object) {
    GameObject* o = &runner->dataWin->objt.objects[object];
    Instance* inst = Instance_create(0, object, 0.0, 0.0);
    inst->roomIndex = runner->currentRoomIndex;
    inst->spriteIndex = o->spriteId;
    inst->visible = o->visible;
    inst->solid = o->solid;
    inst->persistent = o->persistent;
    inst->depth = o->depth;
    inst->maskIndex = o->textureMaskId;
    inst->createEventFired = true;
    return inst;
}

// Room for one more pixel at swEnd: the live ones moved to the front, or a larger buffer.
static bool swRoom(void) SW_COLD;
static bool swRoom(void) {
    int32_t n = swCount();
    if (swBase > 0 && swCap / 2 > n) {
        memmove(swPx, swPx + swBase, (size_t) n * sizeof(UtswPixel));
    } else {
        int32_t cap = swCap == 0 ? 512 : 2 * swCap;
        UtswPixel* px = (UtswPixel*) memalign(32, (size_t) cap * sizeof(UtswPixel));
        if (px == nullptr) return false;
        if (n != 0) memcpy(px, swPx + swBase, (size_t) n * sizeof(UtswPixel));
        free(swPx);
        swPx = px;
        swCap = cap;
    }
    swBase = 0;
    swEnd = n;
    return true;
}

// instance_create(x, y, obj_whtpxlgrav) from the dust maker's Draw, its result dropped: builtin_instance_create,
// Runner_createInstance and createAndInitInstance in their order, without the instance or the runner's lists: the
// id, the object's count, the draw list marked, the Create on swMake (the fields it reads and writes as a fresh
// instance's: swDraw's), then the record. False (nothing done) when the runner has to make it.
// The prototype instances, made at the first record (v2 made them at its bind, the first dust pixel; the bind now
// runs at the first frame end, and making them there would move every later heap block). Instance_create(0, ...)
// takes no instance id.
static void swProtos(Runner* runner) SW_COLD;
static void swProtos(Runner* runner) {
    if (swMake == nullptr) {
        swDraw = swProto(runner, swObject);
        swMake = swProto(runner, swObject);
    }
    if (swStripObject >= 0 && swStripMake == nullptr) {
        swStripDraw = swProto(runner, swStripObject);
        swStripMake = swProto(runner, swStripObject);
    }
}

// The runner's state a record can be made in (swCreate's and swCreateStrip's tests).
static inline bool swCanCreate(VMContext* ctx) {
    Runner* runner = ctx->runner;
    if (swState <= 0 || ctx->currentInstance == nullptr || runner->pendingRoom != -1 || runner->shouldExit ||
        runner->physics != nullptr || runner->disabledObjects != nullptr || arrlen(runner->runtimeLayers) != 0 ||
        swWindow != 0 || runner->currentRoom == nullptr)
        return false;
    return !swAny() || (swRoomIndex == runner->currentRoomIndex && swCreatorVar == ctx->creatorVarID);
}

static bool swCreate(VMContext* ctx, RValue* args) SW_LATE;
static bool swCreate(VMContext* ctx, RValue* args) {
    Runner* runner = ctx->runner;
    if (swState == 0 && args[2].type == RVALUE_INT32 && args[2].int32 == swArmPixel) swBindLate(runner);
    if (args[2].type != RVALUE_INT32 || args[2].int32 != swObject || args[0].type != RVALUE_REAL ||
        args[1].type != RVALUE_REAL || !swCanCreate(ctx))
        return false;
    Instance* caller = ctx->currentInstance;
    swProtos(runner);
    if (swEnd == swCap && !swRoom()) return false;

    uint32_t id = runner->nextInstanceId++;
    if (runner->createCounts != nullptr) runner->createCounts[swObject]++;
    runner->drawableListStructureDirty = true;

    // The Create event (Runner_executeResolvedEvent and executeCode around it)
    Instance* m = swMake;
    m->instanceId = id;
    m->x = m->xprevious = m->xstart = (float) args[0].real;
    m->y = m->yprevious = m->ystart = (float) args[1].real;
    m->gravity = swDraw->gravity;
    m->gravityDirection = swDraw->gravityDirection;
    m->hspeed = swDraw->hspeed;
    m->vspeed = swDraw->vspeed;
    m->speed = swDraw->speed;
    m->direction = swDraw->direction;
    int32_t savedEventType = ctx->currentEventType;
    int32_t savedEventSubtype = ctx->currentEventSubtype;
    int32_t savedEventObjectIndex = ctx->currentEventObjectIndex;
    bool savedActionRelativeFlag = ctx->actionRelativeFlag;
    ctx->actionRelativeFlag = false;
    ctx->currentEventType = EVENT_CREATE;
    ctx->currentEventSubtype = 0;
    ctx->currentEventObjectIndex = swObject;
    ctx->currentInstance = m;
    swPixelCreate(ctx);
    ctx->currentInstance = caller;
    ctx->currentEventType = savedEventType;
    ctx->currentEventSubtype = savedEventSubtype;
    ctx->currentEventObjectIndex = savedEventObjectIndex;
    ctx->actionRelativeFlag = savedActionRelativeFlag;

    if (!swAny()) {
        swRoomIndex = runner->currentRoomIndex;
        swCreatorVar = ctx->creatorVarID;
    }
    UtswPixel* p = &swPx[swEnd++];
    p->x = m->x;
    p->y = m->y;
    p->hspeed = m->hspeed;
    p->vspeed = m->vspeed;
    p->gravity = m->gravity;
    p->gravityDirection = m->gravityDirection;
    p->imageIndex = m->imageIndex; // as Instance_create left it: the Create and the draws don't touch swMake's
    p->outsideRoom = m->outsideRoom;
    p->xprevious = m->xprevious;
    p->yprevious = m->yprevious;
    p->xstart = m->xstart;
    p->ystart = m->ystart;
    p->direction0 = m->direction;
    p->id = id;
    p->creator = caller->instanceId;
    swAdded++;
    return true;
}

// Room for one more strip at swStEnd (swRoom's way).
static bool swStRoom(void) SW_COLD;
static bool swStRoom(void) {
    int32_t n = swStCount();
    if (swStBase > 0 && swStCap / 2 > n) {
        memmove(swSt, swSt + swStBase, (size_t) n * sizeof(UtswPixel));
    } else {
        int32_t cap = swStCap == 0 ? 256 : 2 * swStCap;
        UtswPixel* st = (UtswPixel*) memalign(32, (size_t) cap * sizeof(UtswPixel));
        if (st == nullptr) return false;
        if (n != 0) memcpy(st, swSt + swStBase, (size_t) n * sizeof(UtswPixel));
        free(swSt);
        swSt = st;
        swStCap = cap;
    }
    swStBase = 0;
    swStEnd = n;
    return true;
}

// instance_create(x, y, obj_blkpxltall) from obj_vaporized's Alarm 0, its result dropped: builtin_instance_create and
// Runner_createInstance in their order for the strip (its id, the object's count, the draw list marked), then its
// Create, dt = instance_create(x, y, obj_whtpxlgrav), on swStripMake (the strip's id and position: all its Create
// reads) through swCreate (the pixel, id + 1, its creator the strip), then the strip's record (its creator the
// alarm's instance; dt is its id + 1). False (nothing done) when the runner has to make it.
static bool swCreateStrip(VMContext* ctx, RValue* args, RValue* out) {
    Runner* runner = ctx->runner;
    Instance* caller = ctx->currentInstance;
    if (caller == nullptr || caller->objectIndex != swVapObject || ctx->currentEventType != EVENT_ALARM ||
        ctx->currentEventSubtype != 0 || ctx->currentEventObjectIndex != swVapObject ||
        args[0].type != RVALUE_REAL || args[1].type != RVALUE_REAL || !swCanCreate(ctx))
        return false;
    swProtos(runner);
    if (swEnd == swCap && !swRoom()) return false;
    if (swStEnd == swStCap && !swStRoom()) return false;

    uint32_t id = runner->nextInstanceId++;
    if (runner->createCounts != nullptr) runner->createCounts[swStripObject]++;
    runner->drawableListStructureDirty = true;

    // The Create (dispatchInstanceCreationEvents: no PreCreate), on the strip: its x and y as an instance holds them
    Instance* st = swStripMake;
    st->instanceId = id;
    st->x = st->xprevious = st->xstart = (float) args[0].real;
    st->y = st->yprevious = st->ystart = (float) args[1].real;
    RValue a[3];
    a[0] = RValue_makeReal((GMLReal) st->x);
    a[1] = RValue_makeReal((GMLReal) st->y);
    a[2] = RValue_makeInt32(swObject);
    int32_t savedEventType = ctx->currentEventType;
    int32_t savedEventSubtype = ctx->currentEventSubtype;
    int32_t savedEventObjectIndex = ctx->currentEventObjectIndex;
    ctx->currentEventType = EVENT_CREATE;
    ctx->currentEventSubtype = 0;
    ctx->currentEventObjectIndex = swStripObject;
    ctx->currentInstance = st;
    bool made = swCreate(ctx, a);
    ctx->currentInstance = caller;
    ctx->currentEventType = savedEventType;
    ctx->currentEventSubtype = savedEventSubtype;
    ctx->currentEventObjectIndex = savedEventObjectIndex;
    // swCanCreate held and there was room, so swCreate made it.
    require(made);

    UtswPixel* r = &swSt[swStEnd++];
    memset(r, 0, sizeof *r);
    r->x = r->xprevious = r->xstart = st->x;
    r->y = r->yprevious = r->ystart = st->y;
    r->imageIndex = swStripDraw->imageIndex;
    r->id = id;
    r->creator = caller->instanceId;
    swStripsAdded++;
    *out = RValue_makeReal((GMLReal) id);
    return true;
}

// z_utkill_vapor.c's call of instance_create for one dust pixel (ukvCallDrop's arguments).
void Utsw_createDrop(VMContext* ctx, int32_t funcIndex, RValue* args, int32_t argCount) {
    if (swCreate(ctx, args)) return;
    BuiltinFunc builtin = (BuiltinFunc) ctx->funcCallCache[funcIndex].builtin;
    RValue result = builtin(ctx, argCount > 0 ? args : nullptr, argCount);
    RValue_free(&result);
}

// ---[ Step ]---

// isEventBlockedByPendingRoom and the shouldExit test for an Other event of a non-persistent instance.
static inline bool swOtherBlocked(Runner* runner) {
    if (runner->shouldExit) return true;
    if (0 > runner->pendingRoom) return false;
    return !(runner->currentRoom != nullptr && runner->currentRoom->persistent);
}

// After stepFrame's animate pass: each pixel's previous position saved and its image advanced; at the sprite's end,
// Animation End destroys it (action_kill_object: Runner_destroyInstance with no Destroy or CleanUp event and nothing
// holding a reference), unless a pending room change blocks the event. The dead at the front just move swBase.
void Utsw_animate(Runner* runner) {
    swWindow = 0;
    swAfterStep = false;
    swStrip(runner);
    if (!swAny()) return;
    bool blocked = swOtherBlocked(runner);
    const Sprite* spr = &runner->dataWin->sprt.sprites[swDraw->spriteIndex];
    const float frameCount = (float) spr->textureCount;
    const float imageSpeed = swDraw->imageSpeed;
    int32_t first = swBase, w = -1;
    for (int32_t i = swBase; swEnd > i; i++) {
        UtswPixel* p = &swPx[i];
        p->xprevious = p->x;
        p->yprevious = p->y;
        float image = p->imageIndex + imageSpeed;
        bool wrapped = false;
        if (image >= frameCount) {
            image -= frameCount;
            wrapped = true;
        } else if (0.0f > image) {
            image += frameCount;
            wrapped = true;
        }
        p->imageIndex = image;
        if (wrapped && !blocked) {
            if (w < 0) {
                if (i == first) first++;
                else w = i;
            }
            continue;
        }
        if (w >= 0) swPx[w++] = *p;
    }
    if (w >= 0) swEnd = w;
    swBase = first;
    if (swBase == swEnd) swBase = swEnd = 0;
    if (runner->pendingRoom != -1 || runner->shouldExit) {
        swInAnimate = true;
        swMaterialize(runner, WHY_PENDING);
        swInAnimate = false;
    }
}

// The Step pass has run each strip's Step: if (!instance_exists(dt)) instance_destroy(). Its pixel was gone by then
// when it went at an Animation End this frame or at an Outside Room the frame before (nothing else ends one while it
// is in the swarm), so the strip went (Runner_destroyInstance: no Destroy or CleanUp event, nothing holds its id).
// A room change pending since Utsw_animate (which made the swarm instances then) blocks the Step events of the
// room's instances; whether it came before or after the strips' turn isn't known here, so strips whose pixel is gone
// then are counted and logged, and kept.
static void swStripSteps(Runner* runner) SW_LATE;
static void swStripSteps(Runner* runner) {
    if (swStEnd == swStBase) return;
    int32_t j = swBase, w = swStBase;
    bool blocked = runner->pendingRoom != -1 || runner->shouldExit;
    int32_t gone = 0;
    for (int32_t k = swStBase; swStEnd > k; k++) {
        if (swPixelGone(&swSt[k], &j)) {
            gone++;
            if (!blocked) continue;
        }
        if (w != k) swSt[w] = swSt[k];
        w++;
    }
    swStEnd = w;
    if (swStBase == swStEnd) swStBase = swStEnd = 0;
    if (blocked && gone != 0) swUnprovenAt(runner, "room change in the Step pass", gone);
}

// dispatchOutsideRoomEvents' full test (Collision_computeBBox), for a pixel the fast test can't place inside the room.
static bool swOutside(Runner* runner, const UtswPixel* p, int32_t roomWidth, int32_t roomHeight) SW_COLD;
static bool swOutside(Runner* runner, const UtswPixel* p, int32_t roomWidth, int32_t roomHeight) {
    Instance* inst = swDraw;
    inst->x = p->x;
    inst->y = p->y;
    InstanceBBox bbox = Collision_computeBBox(runner, inst);
    if (bbox.valid) return (0 > bbox.right || bbox.left > roomWidth || 0 > bbox.bottom || bbox.top > roomHeight);
    return (0 > inst->x || inst->x > roomWidth || 0 > inst->y || inst->y > roomHeight);
}

// Gravity stopped a pixel (hspeed and vspeed both 0): its instance's motion pass works out the direction from the
// speeds before (Instance_computeSpeedFromComponents, which keeps the old direction when they are both 0 too), then
// keeps it while both stay 0.
static void swStopped(UtswPixel* p, float h0, float v0) SW_COLD;
static void swStopped(UtswPixel* p, float h0, float v0) {
    Instance* m = swMake;
    m->hspeed = h0;
    m->vspeed = v0;
    m->direction = p->direction0;
    Instance_computeSpeedFromComponents(m);
    p->direction0 = m->direction;
}

// Before stepFrame's motion pass: gravity and the move as the motion pass does them for a pixel (no friction, path or
// physics), then the Outside Room test and event as dispatchOutsideRoomEvents does them. The test first goes
// Utdust_insideRoom's way with its sums (the instance's mask or sprite, angle and scales are swDraw's, the same for
// every pixel): inside then, or else the full test.
void Utsw_motion(Runner* runner) {
    swStripSteps(runner);
    swAfterStep = true;
    if (swCount() == 0) {
        if (swAny() && (runner->pendingRoom != -1 || runner->shouldExit)) swMaterialize(runner, WHY_PENDING);
        return;
    }
    if (runner->pendingRoom != -1 || runner->shouldExit) {
        swMaterialize(runner, WHY_PENDING);
        return;
    }
    int32_t roomWidth = (int32_t) runner->currentRoom->width;
    int32_t roomHeight = (int32_t) runner->currentRoom->height;
    // Utdust_insideRoom's per-instance terms, for swDraw's fields
    const Instance* d = swDraw;
    DataWin* dataWin = runner->dataWin;
    int32_t sprIdx = (d->maskIndex >= 0) ? d->maskIndex : d->spriteIndex;
    bool fast = !(0 > sprIdx || (uint32_t) sprIdx >= dataWin->sprt.count) && (0.0001 >= GMLReal_fabs(d->imageAngle));
    GMLReal kL = 0.0, kR = 0.0, kT = 0.0, kB = 0.0;
    if (fast) {
        Sprite* spr = &dataWin->sprt.sprites[sprIdx];
        GMLReal marginL = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginLeft;
        GMLReal marginR = (spr->bboxMode == 1) ? (GMLReal) spr->width : (GMLReal) (spr->marginRight + 1);
        GMLReal marginT = (spr->bboxMode == 1) ? 0.0 : (GMLReal) spr->marginTop;
        GMLReal marginB = (spr->bboxMode == 1) ? (GMLReal) spr->height : (GMLReal) (spr->marginBottom + 1);
        GMLReal originX = (GMLReal) spr->originX;
        GMLReal originY = (GMLReal) spr->originY;
        kL = d->imageXscale * (marginL - originX);
        kR = d->imageXscale * (marginR - originX);
        kT = d->imageYscale * (marginT - originY);
        kB = d->imageYscale * (marginB - originY);
    }
    const GMLReal lastX = (GMLReal) (roomWidth - 1), lastY = (GMLReal) (roomHeight - 1);
    int32_t first = swBase, w = -1;
    for (int32_t i = swBase; swEnd > i; i++) {
        UtswPixel* p = &swPx[i];
        if (p->gravity != 0.0f) {
            // Utdust_applyGravity's sums
            uint32_t bits;
            memcpy(&bits, &p->gravityDirection, sizeof(bits));
            if (!swGravityKnown || bits != swGravityBits) {
                GMLReal gravDirRad = p->gravityDirection * (M_PI / 180.0);
                swGravityCos = clampFloat(GMLReal_cos(gravDirRad));
                swGravitySin = clampFloat(GMLReal_sin(gravDirRad));
                swGravityBits = bits;
                swGravityKnown = true;
            }
            float h0 = p->hspeed, v0 = p->vspeed;
            p->hspeed += (float) (p->gravity * swGravityCos);
            p->vspeed -= (float) (p->gravity * swGravitySin);
            if (p->hspeed == 0.0f && p->vspeed == 0.0f) swStopped(p, h0, v0);
        }
        if (p->hspeed != 0.0f || p->vspeed != 0.0f) {
            p->x += p->hspeed;
            p->y += p->vspeed;
        }
        bool inside = false;
        if (fast) {
            GMLReal left = p->x + kL, right = p->x + kR, top = p->y + kT, bottom = p->y + kB;
            if (left > right) { GMLReal tmp = left; left = right; right = tmp; }
            if (top > bottom) { GMLReal tmp = top; top = bottom; bottom = tmp; }
            // Each test fails for a NaN, which then goes the full way too.
            inside = right >= 1.0 && lastX >= left && bottom >= 1.0 && lastY >= top;
        }
        bool outside = !inside && swOutside(runner, p, roomWidth, roomHeight);
        if (outside && !p->outsideRoom) {
            // Outside Room: action_kill_object
            if (w < 0) {
                if (i == first) first++;
                else w = i;
            }
            continue;
        }
        p->outsideRoom = outside;
        if (w >= 0) swPx[w++] = *p;
    }
    if (w >= 0) swEnd = w;
    swBase = first;
    if (swBase == swEnd) swBase = swEnd = 0;
    swWindow = 2;
}

// After the Outside Room pass: the swarm and the runner's instances are at the same point again.
void Utsw_outsideDone(Runner* runner) {
    (void) runner;
    swWindow = 0;
}

// ---[ Draw ]---

// compareDrawables (runner.c's utkCompareDrawables)
static inline int swCompareDrawables(const Drawable* a, const Drawable* b) {
    if (a->depth != b->depth)
        return a->depth > b->depth ? -1 : 1;
    if (a->layerOrder != 0 && b->layerOrder != 0) {
        if (a->layerOrder != b->layerOrder)
            return a->layerOrder > b->layerOrder ? -1 : 1;
        if (a->elementOrder != b->elementOrder)
            return a->elementOrder < b->elementOrder ? -1 : 1;
    }
    if (a->type != b->type)
        return (int32_t) a->type < (int32_t) b->type ? -1 : 1;
    if (a->type == DRAWABLE_TILE || a->type == DRAWABLE_INSTANCE)
        return (a->order > b->order) - (a->order < b->order);
    return (a->order < b->order) - (a->order > b->order);
}

// After the draw list's rebuild in utkDrawRoom: where the rebuild would put the pixels' instances' entries (all keys
// differ: an instance's ends with its id, and every pixel has the object's depth), a span entry for each run of
// pixels with no other entry between them. Its key is the run's last pixel's (so the loop's cursor search after a
// rebuild mid-draw, which compares keys, places it as it would that pixel's), its instance swDraw.
// The spans of a (swPx or swSt) from base to end, from list entry *i on (each span's records in a, its list entry and
// kind in swSpanFirst, swSpanCount, swSpanAt and swSpanStrip).
static void swSpans(const Drawable* l, int32_t n, int32_t* i, Drawable* d, const UtswPixel* a, int32_t base,
                    int32_t end, int8_t strip) SW_LATE;
static void swSpans(const Drawable* l, int32_t n, int32_t* i, Drawable* d, const UtswPixel* a, int32_t base,
                    int32_t end, int8_t strip) {
    for (int32_t j = base; end > j;) {
        d->order = (int32_t) a[j].id;
        while (n > *i && 0 > swCompareDrawables(&l[*i], d)) (*i)++;
        int32_t start = j;
        j++;
        if (n > *i) {
            while (end > j) {
                d->order = (int32_t) a[j].id;
                if (0 > swCompareDrawables(&l[*i], d)) break;
                j++;
            }
        } else {
            j = end;
        }
        arrput(swSpanFirst, start);
        arrput(swSpanCount, j - start);
        arrput(swSpanAt, *i);
        arrput(swSpanStrip, strip);
    }
}

void Utsw_merge(Runner* runner) {
    swStrip(runner);
    if (!swAny()) return;
    int32_t n = (int32_t) arrlen(runner->cachedDrawables);
    Drawable* l = runner->cachedDrawables;
    // The runs, and before which entry each goes: the strips' first (their depth is the larger: every strip entry
    // comes before every pixel entry), then the pixels'.
    arrsetlen(swSpanFirst, 0);
    arrsetlen(swSpanCount, 0);
    arrsetlen(swSpanAt, 0);
    arrsetlen(swSpanStrip, 0);
    Drawable ds, dp;
    memset(&ds, 0, sizeof ds);
    memset(&dp, 0, sizeof dp);
    int32_t i = 0;
    if (swStEnd != swStBase) {
        ds.type = DRAWABLE_INSTANCE;
        ds.depth = swStripDraw->depth;
        ds.objectIndex = swStripObject;
        ds.instance = swStripDraw;
        swSpans(l, n, &i, &ds, swSt, swStBase, swStEnd, 1);
    }
    dp.type = DRAWABLE_INSTANCE;
    dp.depth = swDraw->depth;
    dp.objectIndex = swObject;
    dp.instance = swDraw;
    swSpans(l, n, &i, &dp, swPx, swBase, swEnd, 0);
    int32_t s = (int32_t) arrlen(swSpanFirst);
    arrsetlen(runner->cachedDrawables, n + s);
    l = runner->cachedDrawables;
    int32_t src = n - 1, w = n + s - 1;
    for (int32_t g = s - 1; g >= 0; g--) {
        while (src >= swSpanAt[g]) l[w--] = l[src--];
        Drawable* d = swSpanStrip[g] ? &ds : &dp;
        const UtswPixel* a = swSpanStrip[g] ? swSt : swPx;
        d->order = (int32_t) a[swSpanFirst[g] + swSpanCount[g] - 1].id;
        d->elementOrder = UTSW_SPAN0 - g;
        l[w--] = *d;
    }
    swMarked = true;
}

// A span entry's pixels and the instance to draw them with (runner.c's utswDrawSpan).
const UtswPixel* Utsw_span(const Drawable* d, int32_t* count, Instance** drawInstance) {
    int32_t g = UTSW_SPAN0 - d->elementOrder;
    *drawInstance = swDraw;
    if (0 > g || (int32_t) arrlen(swSpanFirst) <= g) {
        *count = 0;
        return nullptr;
    }
    *count = swSpanCount[g];
    if (swSpanStrip[g]) {
        *drawInstance = swStripDraw;
        return &swSt[swSpanFirst[g]];
    }
    return &swPx[swSpanFirst[g]];
}

// ---[ Frame end (main.c's call targets) ]---

// The first frame end: the off-switch file, the two objects by name and instance_create's wrapper (swWrapCreate). The
// bind waits for the first record.
static void swArm(Runner* runner) SW_COLD;
static void swArm(Runner* runner) {
    const char* why = nullptr;
    if (swOffFile) why = "off-switch file";
    else if ((swArmPixel = swFindObject(runner->dataWin, "obj_whtpxlgrav")) < 0) why = "object";
    else if ((swArmStrip = swFindObject(runner->dataWin, "obj_blkpxltall")) < 0) swArmStrip = SW_UNKNOWN + 1;
    if (why == nullptr && !swWrapCreate(runner->vmContext)) why = "instance_create";
    if (why != nullptr) {
        swState = -1;
        logInfo("utswarm: off (%s)\n", why);
    }
}

// The bind (swBind), at the first record: the first instance_create of obj_blkpxltall (from obj_vaporized's Alarm 0)
// or the first dust pixel. Every wrapper passed through until then, and the first record comes after it.
static void swBindLate(Runner* runner) {
    const char* why = swBind(runner->vmContext);
    swState = why == nullptr ? 1 : -1;
    if (why == nullptr) logInfo("utswarm: bound at frame %d (object %d, strips %d)\n", (int) runner->frameCount,
                                (int) swObject, (int) swStripObject);
    else logInfo("utswarm: off (%s)\n", why);
}

// CtrBench_handleRoomChange: a room change (to another room, a room restart or a game restart) ends the room's non-persistent
// instances, so the swarm becomes instances first and the runner does the rest.
void Utsw_handleRoomChange(Runner* runner) {
    if (!swRoomSeen) {
        swRoomSeen = true;
        struct stat st;
        swOffFile = stat("sdmc:/3ds/undertale-3ds/utswarm-off", &st) == 0;
        logInfo("utswarm: room-change hook reached\n");
        swArm(runner);
    }
    if (runner->pendingRoom != -1 && swAny()) swMaterialize(runner, WHY_ROOM);
    if (runner->pendingRoom != -1 && swState > 0) {
        logInfo("utswarm: room change at frame %d: %u made, %u strips, %u materialized, %u mid-pass, %u unproven\n",
                (int) runner->frameCount, (unsigned) swAdded, (unsigned) swStripsAdded, (unsigned) swMaterialized,
                (unsigned) swMidPass, (unsigned) swUnproven);
    }
    CtrBench_handleRoomChange(runner);
#if defined(UTSW_TEST_EVERY)
    // Test builds only (never committed on): the swarm made instances every UTSW_TEST_EVERY frames.
    if (runner->frameCount % UTSW_TEST_EVERY == 0) swMaterialize(runner, WHY_TEST);
#endif
    swWindow = 1;
}

// StateHash_endFrame with the swarm's instances (swBuild) in runner->instances where the runner would have them
// (test builds).
void Utsw_stateHashEndFrame(Runner* runner) {
    if (!swAny()) {
        StateHash_endFrame(runner);
        return;
    }
    Instance** built = swBuildAll(runner, false);
    int32_t k = (int32_t) arrlen(built);
    Instance** saved = runner->instances;
    Instance** merged = nullptr;
    int32_t n = (int32_t) arrlen(saved);
    arrsetlen(merged, n);
    if (n != 0) memcpy(merged, saved, (size_t) n * sizeof(Instance*));
    swMergeInstances(&merged, built, k);
    runner->instances = merged;
    StateHash_endFrame(runner);
    runner->instances = saved;
    arrfree(merged);
    for (int32_t j = 0; k > j; j++) Instance_free(built[j]);
    arrfree(built);
}

#endif
