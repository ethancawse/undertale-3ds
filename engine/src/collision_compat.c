// Compatibility-mode collisions (colflip): GameMaker's compatibility-mode rule for precise collisions, measured
// against the official runner.
//
// Everything here is late code (.text.late.colflip, .rodata.late.colflip, .bss.late.colflip; platform/3ds/
// late_reserve.ld), so the warm text keeps its addresses. The original builtins and collision functions are unchanged
// and still answer modern mode. This file defines collision.h's compatibility-mode paths, and late builtins for
// collision_point, instance_position, position_meeting, collision_rectangle and collision_rectangle_list, which
// Colflip_registerAll registers over the originals.

// rvalue.h's out-of-line RValue_toRealOther and RValue_toInt32Other get a copy in every file that calls them. This
// file's copies (late code, through late_reserve.ld's file pattern) take other names, so that each existing name keeps
// exactly the definitions it has in the base build.
#define RValue_toRealOther Colflip_toRealOther
#define RValue_toInt32Other Colflip_toInt32Other
#define COLLISION_COMPAT_DEFINE
#include "collision.h"

#include "rvalue.h"
#include "runner.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "vm.h"
#include "vm_builtins.h"

// The original builtins, for modern mode and for calls with too few arguments (which they warn about).
typedef struct {
    BuiltinFunc collisionPoint, instancePosition, positionMeeting, collisionRectangle, collisionRectangleList;
} ColflipOriginals;
static __attribute__((section(".bss.late.colflip"))) ColflipOriginals colflipOriginals;

// Names in late constants: a new string literal would land in .rodata.
static const char colflipNameCollisionPoint[] __attribute__((section(".rodata.late.colflip"))) = "collision_point";
static const char colflipNameInstancePosition[] __attribute__((section(".rodata.late.colflip"))) = "instance_position";
static const char colflipNamePositionMeeting[] __attribute__((section(".rodata.late.colflip"))) = "position_meeting";
static const char colflipNameCollisionRectangle[] __attribute__((section(".rodata.late.colflip"))) = "collision_rectangle";
static const char colflipNameCollisionRectangleList[] __attribute__((section(".rodata.late.colflip"))) = "collision_rectangle_list";

static inline __attribute__((always_inline)) DsList* colflipDsList(Runner* runner, int32_t id) {
    if (0 > id || id >= (int32_t) arrlen(runner->dsListPool)) return nullptr;
    if (runner->dsListPool[id].freed) return nullptr;
    return &runner->dsListPool[id];
}

static COLFLIP_LATE COLFLIP_FLAT RValue builtin_collision_point_compat(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 5 > argCount) return colflipOriginals.collisionPoint(ctx, args, argCount);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[2]));
    int32_t prec = RValue_toInt32(args[3]);
    int32_t notme = RValue_toInt32(args[4]);
    Instance* self = ctx->currentInstance;

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t snapIdx = snapBase; snapEnd > snapIdx; snapIdx++) {
        Instance* inst = runner->instanceSnapshots[snapIdx];
        if (!inst->active) continue;
        if (notme && inst == self) continue;
        if (!Collision_compatPointHits(runner, inst, px, py, prec != 0)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP_LATE COLFLIP_FLAT RValue builtin_instance_position_compat(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 3 > argCount) return colflipOriginals.instancePosition(ctx, args, argCount);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[2]));

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t i = snapBase; snapEnd > i; i++) {
        Instance* inst = runner->instanceSnapshots[i];
        if (!inst->active) continue;
        // GameMaker always tests the mask here.
        if (!Collision_compatPointHits(runner, inst, px, py, true)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP_LATE COLFLIP_FLAT RValue builtin_position_meeting_compat(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 3 > argCount) return colflipOriginals.positionMeeting(ctx, args, argCount);
    GMLReal px = RValue_toReal(args[0]);
    GMLReal py = RValue_toReal(args[1]);
    int32_t target = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[2]));
    if (target == INSTANCE_NOONE) return RValue_makeBool(false);

    SpatialGrid_syncGrid(runner, runner->spatialGrid);
    SpatialGridQuery query = SpatialGrid_prepareQuery(runner, px, py, px, py, target);
    bool found = false;
    for (int32_t gx = query.range.minGridX; query.range.maxGridX >= gx && !found; gx++) {
        for (int32_t gy = query.range.minGridY; query.range.maxGridY >= gy && !found; gy++) {
            Instance** cell = runner->spatialGrid->grid[SpatialGrid_cellIndex(runner->spatialGrid, gx, gy)];
            int32_t cellLen = (int32_t) arrlen(cell);
            repeat(cellLen, ci) {
                Instance* other = cell[ci];
                // Self is not skipped.
                if (!other->active) continue;
                if (other->lastCollisionQueryId == query.queryId) continue;
                other->lastCollisionQueryId = query.queryId;
                if (!query.matchAll && query.filterByObject && !VM_isObjectOrDescendant(runner->dataWin, other->objectIndex, target)) continue;
                if (!query.matchAll && query.filterByInstanceId && other->instanceId != (uint32_t) target) continue;
                // GameMaker always tests the mask here.
                if (!Collision_compatPointHits(runner, other, px, py, true)) continue;
                found = true;
                break;
            }
        }
    }
    return RValue_makeBool(found);
}

static COLFLIP_LATE COLFLIP_FLAT RValue builtin_collision_rectangle_compat(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 7 > argCount) return colflipOriginals.collisionRectangle(ctx, args, argCount);
    GMLReal x1 = RValue_toReal(args[0]);
    GMLReal y1 = RValue_toReal(args[1]);
    GMLReal x2 = RValue_toReal(args[2]);
    GMLReal y2 = RValue_toReal(args[3]);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[4]));
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);
    if (x1 > x2) { GMLReal tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { GMLReal tmp = y1; y1 = y2; y2 = tmp; }
    Instance* self = ctx->currentInstance;

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t snapIdx = snapBase; snapEnd > snapIdx; snapIdx++) {
        Instance* inst = runner->instanceSnapshots[snapIdx];
        if (!inst->active) continue;
        if (notme && inst == self) continue;
        if (!Collision_compatRectHitsInstance(runner, inst, x1, y1, x2, y2, prec != 0)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP_LATE COLFLIP_FLAT RValue builtin_collision_rectangle_list_compat(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 8 > argCount) return colflipOriginals.collisionRectangleList(ctx, args, argCount);
    GMLReal x1 = RValue_toReal(args[0]);
    GMLReal y1 = RValue_toReal(args[1]);
    GMLReal x2 = RValue_toReal(args[2]);
    GMLReal y2 = RValue_toReal(args[3]);
    int32_t target = VM_resolveInstanceTarget(ctx, RValue_toInt32(args[4]));
    int32_t prec = RValue_toInt32(args[5]);
    int32_t notme = RValue_toInt32(args[6]);
    int32_t listId = RValue_toInt32(args[7]);
    // arg 8 (ordered) is ignored, as in the original: instances are appended in iteration order.
    if (target == INSTANCE_NOONE) return RValue_makeReal(0.0);
    DsList* list = colflipDsList(runner, listId);
    if (list == nullptr) return RValue_makeReal(0.0);
    if (x1 > x2) { GMLReal tmp = x1; x1 = x2; x2 = tmp; }
    if (y1 > y2) { GMLReal tmp = y1; y1 = y2; y2 = tmp; }
    Instance* self = ctx->currentInstance;
    int32_t count = 0;

    SpatialGrid_syncGrid(runner, runner->spatialGrid);
    SpatialGridQuery query = SpatialGrid_prepareQuery(runner, x1, y1, x2, y2, target);
    for (int32_t gx = query.range.minGridX; query.range.maxGridX >= gx; gx++) {
        for (int32_t gy = query.range.minGridY; query.range.maxGridY >= gy; gy++) {
            Instance** cell = runner->spatialGrid->grid[SpatialGrid_cellIndex(runner->spatialGrid, gx, gy)];
            int32_t cellLen = (int32_t) arrlen(cell);
            repeat(cellLen, ci) {
                Instance* inst = cell[ci];
                if (!inst->active) continue;
                if (notme && inst == self) continue;
                if (inst->lastCollisionQueryId == query.queryId) continue;
                inst->lastCollisionQueryId = query.queryId;
                if (!query.matchAll && query.filterByObject && !VM_isObjectOrDescendant(ctx->dataWin, inst->objectIndex, target)) continue;
                if (!query.matchAll && query.filterByInstanceId && inst->instanceId != (uint32_t) target) continue;
                if (!Collision_compatRectHitsInstance(runner, inst, x1, y1, x2, y2, prec != 0)) continue;
                arrput(list->items, RValue_makeReal((GMLReal) inst->instanceId));
                count++;
            }
        }
    }
    return RValue_makeReal((GMLReal) count);
}

// Puts late over the builtin registered as name and returns the original (nullptr, and nothing changed, if name is
// not registered).
static inline __attribute__((always_inline)) BuiltinFunc colflipReplace(VMContext* ctx, const char* name, BuiltinFunc late) {
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    BuiltinFunc original = ctx->builtinMap[idx].value;
    if (original != nullptr) ctx->builtinMap[idx].value = late;
    return original;
}

// Called in place of VMBuiltins_registerAll (VM_create), before anything looks a builtin up.
COLFLIP_LATE void Colflip_registerAll(VMContext* ctx) {
    VMBuiltins_registerAll(ctx);
    colflipOriginals.collisionPoint = colflipReplace(ctx, colflipNameCollisionPoint, builtin_collision_point_compat);
    colflipOriginals.instancePosition = colflipReplace(ctx, colflipNameInstancePosition, builtin_instance_position_compat);
    colflipOriginals.positionMeeting = colflipReplace(ctx, colflipNamePositionMeeting, builtin_position_meeting_compat);
    colflipOriginals.collisionRectangle = colflipReplace(ctx, colflipNameCollisionRectangle, builtin_collision_rectangle_compat);
    colflipOriginals.collisionRectangleList = colflipReplace(ctx, colflipNameCollisionRectangleList, builtin_collision_rectangle_list_compat);
}
