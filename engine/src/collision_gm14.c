// UNDERTALE's collision rules (colflip; collision_gm14.h): GameMaker: Studio 1.4's point, rectangle and instance
// tests, and late builtins for collision_point, instance_position, position_meeting, collision_rectangle and
// collision_rectangle_list that use them. Colflip14_registerAll registers those over collision_compat.c's for
// bytecode 16 or older; for DELTARUNE it changes nothing after Colflip_registerAll.
//
// Everything here is late code (.text.late.colflip_gm14, .rodata.late.colflip_gm14, .bss.late.colflip_gm14; listed
// last in platform/3ds/late_reserve.ld), so collision_compat.c and every late function before it keep their bytes.

// rvalue.h's out-of-line RValue_toRealOther and RValue_toInt32Other get a copy in every file that calls them; this
// file's copies take other names, as collision_compat.c's do.
#define RValue_toRealOther Colflip14_toRealOther
#define RValue_toInt32Other Colflip14_toInt32Other
#define COLLISION_GM14_DEFINE
#include "collision_gm14.h"

#include "rvalue.h"
#include "runner.h"
#include "spatial_grid.h"
#include "stb_ds.h"
#include "vm.h"
#include "vm_builtins.h"

// The builtins registered before these (collision_compat.c's), for modern mode and for calls with too few arguments.
typedef struct {
    BuiltinFunc collisionPoint, instancePosition, positionMeeting, collisionRectangle, collisionRectangleList;
} Colflip14Previous;
static __attribute__((section(".bss.late.colflip_gm14"))) Colflip14Previous colflip14Previous;

// Names in late constants: a new string literal would land in .rodata.
static const char colflip14NameCollisionPoint[] __attribute__((section(".rodata.late.colflip_gm14"))) = "collision_point";
static const char colflip14NameInstancePosition[] __attribute__((section(".rodata.late.colflip_gm14"))) = "instance_position";
static const char colflip14NamePositionMeeting[] __attribute__((section(".rodata.late.colflip_gm14"))) = "position_meeting";
static const char colflip14NameCollisionRectangle[] __attribute__((section(".rodata.late.colflip_gm14"))) = "collision_rectangle";
static const char colflip14NameCollisionRectangleList[] __attribute__((section(".rodata.late.colflip_gm14"))) = "collision_rectangle_list";

static inline __attribute__((always_inline)) DsList* colflip14DsList(Runner* runner, int32_t id) {
    if (0 > id || id >= (int32_t) arrlen(runner->dsListPool)) return nullptr;
    if (runner->dsListPool[id].freed) return nullptr;
    return &runner->dsListPool[id];
}

// The builtins below are collision_compat.c's with GMS 1.4's tests.
static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_collision_point_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 5 > argCount) return colflip14Previous.collisionPoint(ctx, args, argCount);
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
        if (!Colflip14_pointHits(runner, inst, px, py, prec != 0)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_instance_position_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 3 > argCount) return colflip14Previous.instancePosition(ctx, args, argCount);
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
        if (!Colflip14_pointHits(runner, inst, px, py, true)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_position_meeting_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 3 > argCount) return colflip14Previous.positionMeeting(ctx, args, argCount);
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
                if (!Colflip14_pointHits(runner, other, px, py, true)) continue;
                found = true;
                break;
            }
        }
    }
    return RValue_makeBool(found);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_collision_rectangle_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 7 > argCount) return colflip14Previous.collisionRectangle(ctx, args, argCount);
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
        if (!Colflip14_rectHitsInstance(runner, inst, x1, y1, x2, y2, prec != 0)) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

static COLFLIP14_LATE COLFLIP14_FLAT RValue builtin_collision_rectangle_list_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 8 > argCount) return colflip14Previous.collisionRectangleList(ctx, args, argCount);
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
    DsList* list = colflip14DsList(runner, listId);
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
                if (!Colflip14_rectHitsInstance(runner, inst, x1, y1, x2, y2, prec != 0)) continue;
                arrput(list->items, RValue_makeReal((GMLReal) inst->instanceId));
                count++;
            }
        }
    }
    return RValue_makeReal((GMLReal) count);
}

// Puts late over the builtin registered as name and returns the one it replaced (nullptr, and nothing changed, if
// name is not registered).
static inline __attribute__((always_inline)) BuiltinFunc colflip14Replace(VMContext* ctx, const char* name, BuiltinFunc late) {
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    BuiltinFunc previous = ctx->builtinMap[idx].value;
    if (previous != nullptr) ctx->builtinMap[idx].value = late;
    return previous;
}

// Called in place of Colflip_registerAll (VM_create): Colflip_registerAll, then, for GMS 1.4's data, these builtins
// over collision_compat.c's.
COLFLIP14_LATE void Colflip14_registerAll(VMContext* ctx) {
    Colflip_registerAll(ctx);
    // GameMaker: Studio 1.4's runner (UNDERTALE): bytecode 16 or older.
    Colflip_gm14 = ctx->dataWin != nullptr && 16 >= ctx->dataWin->gen8.wadVersion;
    if (!Colflip_gm14) return;
    colflip14Previous.collisionPoint = colflip14Replace(ctx, colflip14NameCollisionPoint, builtin_collision_point_gm14);
    colflip14Previous.instancePosition = colflip14Replace(ctx, colflip14NameInstancePosition, builtin_instance_position_gm14);
    colflip14Previous.positionMeeting = colflip14Replace(ctx, colflip14NamePositionMeeting, builtin_position_meeting_gm14);
    colflip14Previous.collisionRectangle = colflip14Replace(ctx, colflip14NameCollisionRectangle, builtin_collision_rectangle_gm14);
    colflip14Previous.collisionRectangleList = colflip14Replace(ctx, colflip14NameCollisionRectangleList, builtin_collision_rectangle_list_gm14);
}
