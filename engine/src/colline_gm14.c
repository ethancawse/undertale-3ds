// UNDERTALE's line, circle and distance rules and its make_color_rgb (colflip, the follow-up check; colline_gm14.h):
// late builtins for collision_line, collision_circle, distance_to_point, distance_to_object and make_color_rgb, which
// Colline14_registerAll puts over the ones registered before them for GMS 1.4's data. UT's code calls each of them
// (its FUNC chunk); it never calls make_colour_rgb, collision_ellipse or the _list forms, which keep their builtins.
//
// UNDERTALE builds only (ENABLE_WAD16): a DELTARUNE build compiles this file to nothing. Everything here is late code
// (.text.late.colline_gm14, .rodata.late.colline_gm14, .bss.late.colline_gm14; listed last in
// platform/3ds/late_reserve.ld), so every function linked before it keeps its bytes and address.
#if defined(ENABLE_WAD16)

// rvalue.h's out-of-line RValue_toRealOther and RValue_toInt32Other get a copy in every file that calls them; this
// file's copies take other names, as collision_gm14.c's do (only RValue_toRealOther is called).
#define RValue_toRealOther Colline14_toRealOther
#define RValue_toInt32Other Colline14_toInt32Other
#define COLLINE14_DEFINE
#include "colline_gm14.h"

#include "rvalue.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#include "vm_builtins.h"

// The builtins registered before these, for modern mode and for calls with too few arguments.
typedef struct {
    BuiltinFunc collisionLine, collisionCircle, distanceToPoint, distanceToObject, makeColorRgb;
} Colline14Previous;
static __attribute__((section(".bss.late.colline_gm14"))) Colline14Previous colline14Previous;

// Names in late constants: a new string literal would land in .rodata.
static const char colline14NameCollisionLine[] __attribute__((section(".rodata.late.colline_gm14"))) = "collision_line";
static const char colline14NameCollisionCircle[] __attribute__((section(".rodata.late.colline_gm14"))) = "collision_circle";
static const char colline14NameDistanceToPoint[] __attribute__((section(".rodata.late.colline_gm14"))) = "distance_to_point";
static const char colline14NameDistanceToObject[] __attribute__((section(".rodata.late.colline_gm14"))) = "distance_to_object";
static const char colline14NameMakeColorRgb[] __attribute__((section(".rodata.late.colline_gm14"))) = "make_color_rgb";

// An argument as a real (RValue_toReal), out of line: the builtins share it.
static COLLINE14_LATE GMLReal colline14Real(const RValue* v) {
    return RValue_toReal(*v);
}

// An argument as an int32: RValue_toInt32's truncation, which for any argument in int32 range is the real's
// (YYGetInt32 is cvttsd2si for a real).
static inline __attribute__((always_inline)) int32_t colline14Int(const RValue* v) {
    return (int32_t) colline14Real(v);
}

// YYGetBool (0x53b970): a real is true above 0.5.
static inline __attribute__((always_inline)) bool colline14Bool(RValue v) {
    return colline14Real(&v) > 0.5;
}

// YYGetFloat: the argument as float32, not rounded.
static COLLINE14_LATE float colline14Float(const RValue* v) {
    return (float) colline14Real(v);
}

// The first instance of objArg that the line (circle false) or the circle's box (circle true) hits, in the order the
// other collision builtins take (collision_gm14.c); prec and notme are the two arguments after objArg.
static COLLINE14_LATE RValue colline14FirstHit(VMContext* ctx, RValue* args, int32_t objArg, bool circle, float x1, float y1, float x2, float y2) {
    Runner* runner = ctx->runner;
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, colline14Int(&args[objArg]));
    bool prec = colline14Bool(args[objArg + 1]);
    bool notme = colline14Bool(args[objArg + 2]);
    Instance* self = ctx->currentInstance;

    int32_t resultId = INSTANCE_NOONE;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t snapIdx = snapBase; snapEnd > snapIdx; snapIdx++) {
        Instance* inst = runner->instanceSnapshots[snapIdx];
        if (!inst->active) continue;
        if (notme && inst == self) continue;
        if (!(circle ? Colline14_ellipseHits(runner, inst, x1, y1, x2, y2) : Colline14_lineHits(runner, inst, x1, y1, x2, y2, prec))) continue;
        resultId = inst->instanceId;
        break;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) resultId);
}

// collision_line(x1, y1, x2, y2, obj, prec, notme). YYGetFloat: the end points float32, not rounded.
static COLLINE14_LATE RValue builtin_collision_line_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    if (!ctx->runner->collisionCompatibilityMode || 7 > argCount) return colline14Previous.collisionLine(ctx, args, argCount);
    return colline14FirstHit(ctx, args, 4, false, colline14Float(&args[0]), colline14Float(&args[1]), colline14Float(&args[2]), colline14Float(&args[3]));
}

// collision_circle(x, y, r, obj, prec, notme): Instance::Collision_Ellipse of the float32 box (x - r, y - r, x + r,
// y + r), each bound the float32 sum or difference. Not precise only: UT's two calls pass prec 0, and a precise call
// goes to the builtin this one replaced.
static COLLINE14_LATE RValue builtin_collision_circle_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    if (!ctx->runner->collisionCompatibilityMode || 6 > argCount || colline14Bool(args[4])) return colline14Previous.collisionCircle(ctx, args, argCount);
    float cx = colline14Float(&args[0]), cy = colline14Float(&args[1]), r = colline14Float(&args[2]);
    return colline14FirstHit(ctx, args, 3, true, (float) ((double) cx - (double) r), (float) ((double) cy - (double) r), (float) ((double) cx + (double) r), (float) ((double) cy + (double) r));
}

static COLLINE14_LATE RValue builtin_distance_to_point_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 2 > argCount) return colline14Previous.distanceToPoint(ctx, args, argCount);
    return RValue_makeReal((GMLReal) Colline14_distanceToPoint(runner, ctx->currentInstance, colline14Real(&args[0]), colline14Real(&args[1])));
}

// distance_to_object(obj): the least float32 distance, from 1000000.
static COLLINE14_LATE RValue builtin_distance_to_object_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    if (!runner->collisionCompatibilityMode || 1 > argCount) return colline14Previous.distanceToObject(ctx, args, argCount);
    int32_t targetObjIndex = VM_resolveInstanceTarget(ctx, colline14Int(&args[0]));
    Instance* self = ctx->currentInstance;
    double best = 1000000.0;
    int32_t snapBase = Runner_pushInstancesForTarget(runner, targetObjIndex);
    int32_t snapEnd  = (int32_t) arrlen(runner->instanceSnapshots);
    for (int32_t i = snapBase; snapEnd > i; i++) {
        Instance* inst = runner->instanceSnapshots[i];
        if (!inst->active) continue;
        double d = (double) Colline14_distancePair(runner, inst, self);
        if (best >= d) best = d;
    }
    Runner_popInstanceSnapshot(runner, snapBase);
    return RValue_makeReal((GMLReal) best);
}

// make_color_rgb(r, g, b): (b << 16) + (g << 8) + r, the channels truncated and neither masked nor clamped (a channel
// out of 0..255 carries into the next, where DELTARUNE's rule ORs it in).
static COLLINE14_LATE RValue builtin_make_color_rgb_gm14(VMContext* ctx, RValue* args, int32_t argCount) {
    if (3 > argCount) return colline14Previous.makeColorRgb(ctx, args, argCount);
    return RValue_makeReal((GMLReal) Colline14_rgb(colline14Int(&args[0]), colline14Int(&args[1]), colline14Int(&args[2])));
}

// Puts late over the builtin registered as name and returns the one it replaced (nullptr, and nothing changed, if
// name is not registered).
static COLLINE14_LATE BuiltinFunc colline14Replace(VMContext* ctx, const char* name, BuiltinFunc late) {
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) name);
    if (0 > idx) return nullptr;
    BuiltinFunc previous = ctx->builtinMap[idx].value;
    if (previous != nullptr) ctx->builtinMap[idx].value = late;
    return previous;
}

// Called in place of Colflip14_registerAll (VM_create, through COLFLIP_REGISTER_ALL): that, then for GMS 1.4's data
// these builtins over the ones it left.
COLLINE14_LATE void Colline14_registerAll(VMContext* ctx) {
    Colflip14_registerAll(ctx);
    if (!Colflip_gm14) return;
    colline14Previous.collisionLine = colline14Replace(ctx, colline14NameCollisionLine, builtin_collision_line_gm14);
    colline14Previous.collisionCircle = colline14Replace(ctx, colline14NameCollisionCircle, builtin_collision_circle_gm14);
    colline14Previous.distanceToPoint = colline14Replace(ctx, colline14NameDistanceToPoint, builtin_distance_to_point_gm14);
    colline14Previous.distanceToObject = colline14Replace(ctx, colline14NameDistanceToObject, builtin_distance_to_object_gm14);
    colline14Previous.makeColorRgb = colline14Replace(ctx, colline14NameMakeColorRgb, builtin_make_color_rgb_gm14);
}

#endif /* ENABLE_WAD16 */
