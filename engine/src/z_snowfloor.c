// UNDERTALE's snow floor (its Draw, in the snow puzzle room, 16 instances of 25 balls each), Old 3DS: a native
// body for the common case, a tile whose balls are all at rest.
//
// The event draws each ball with draw_circle and asks collision_circle whether the player touches it, then pushes it
// when it was touched or is still moving. Most tiles have nothing to push on most frames, and then the event reads four
// array cells a ball through the generated function's general array path, 400 balls a frame, on or off screen: some 7 ms
// on an Old 3DS, a quarter of the frame, in a room where the circles take another 10 ms (poffroom, 11-06).
//
// This body takes only that case and writes nothing but what the event writes in it. Before anything happens it reads
// every ball and goes to the generated function when any of these fails, so nothing has been drawn or written then:
// every variable there, the cells where they should be, numbers; dodraw exactly 1; moveme exactly 1 (not > 1, so no
// push and no random(), and the RNG stream is the generated function's whenever the stream moves at all); one active
// player object with a sprite, and the circle's box not reaching its box. That last test is utfCircle's (z_utflames.c)
// on the bbox_* values, so a ball it passes is one collision_circle answers noone for, with no side effect. A ball
// that reaches the box, whether or not the circle would really hit it, sends the tile to the generated function.
//
// Then the same builtin calls in the same order with the same values: draw_set_color once, draw_circle for each ball by
// row. xx and yy end as the loop leaves them. The bytecode is checked at build time (UTF_NATIVE's "fnv:" digest in
// tools/aot/gmlaot.py, the same on the Steam and GOG data); any other bytecode keeps the generated function.
// Built for UNDERTALE Old only (CMakeLists.txt). Late code only and last (late_reserve.ld); no statics.
#include "wad_versions.h"

#if defined(ENABLE_WAD16) && defined(CTRPORT_AOT)

#include "aot/vm_aot.h"
#include "collision.h"
#include "data_win.h"
#include "gml_array.h"
#include "instance.h"
#include "rvalue.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdbool.h>
#include <stdint.h>

#define SF_LATE __attribute__((noinline, section(".text_snowfloor")))

RValue SnowFloor_draw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) SF_LATE;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the generated code itself.
RValue SnowFloor_draw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
#else
RValue* UtFlames_slot(Instance* inst, int32_t varID);

// The five rows of the 5 x 5 GameMaker 1 array in a variable (a[xx, yy] is rows[xx].data[yy]), false for anything else.
static bool sfRows(Instance* inst, int32_t varID, const RValue** rows) SF_LATE;
static bool sfRows(Instance* inst, int32_t varID, const RValue** rows) {
    const RValue* slot = UtFlames_slot(inst, varID);
    if (slot == nullptr || slot->type != RVALUE_ARRAY) return false;
    const GMLArray* arr = slot->array;
    if (arr == nullptr || arr->type != GML_LEGACY_ARRAY || 5 > arr->legacy.rowCount) return false;
    for (int32_t i = 0; 5 > i; i++) {
        if (5 > arr->legacy.rows[i].length) return false;
        rows[i] = arr->legacy.rows[i].data;
    }
    return true;
}

// A bbox_* variable on inst as a side's int32 (what collision_circle's box reads), the operand as Push.v's.
static int32_t sfSide(VMContext* ctx, Instance* inst, int32_t ref) SF_LATE;
static int32_t sfSide(VMContext* ctx, Instance* inst, int32_t ref) {
    const Variable* v = &ctx->dataWin->vari.variables[(uint32_t) ref & 0x07FFFFFFu];
    RValue r = VMBuiltins_getVariable(ctx, inst, v->builtinVarId, v->name, -1);
    return (int32_t) r.real;
}

// Whether a cell holds exactly 1: then dodraw == 1 holds and moveme > 1 doesn't, with any epsilon.
#define SF_IS_ONE(c) (((c)->type == RVALUE_REAL && (c)->real == (GMLReal) 1) || ((c)->type == RVALUE_INT32 && (c)->int32 == 1))

// a: the variable IDs of yy, xx, dodraw, snowy, snowx and moveme; draw_set_color's and draw_circle's function indices;
// the player object (0x138); the operands of bbox_left, bbox_right, bbox_top and bbox_bottom (0x2D8, 0x3AC, 0x480, 0x554).
RValue SnowFloor_draw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    (void) k;
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = ctx->runner;
    if (inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX || runner == nullptr || !runner->collisionCompatibilityMode ||
        (uint32_t) a[6] >= ctx->funcCallCacheCount || (uint32_t) a[7] >= ctx->funcCallCacheCount ||
        0 > a[8] || (uint32_t) a[8] >= ctx->dataWin->objt.count)
        return generated(ctx);
    BuiltinFunc setColor = (BuiltinFunc) ctx->funcCallCache[a[6]].builtin;
    BuiltinFunc circle = (BuiltinFunc) ctx->funcCallCache[a[7]].builtin;
    RValue* yy = UtFlames_slot(inst, a[0]);
    RValue* xx = UtFlames_slot(inst, a[1]);
    if (setColor == nullptr || circle == nullptr || yy == nullptr || xx == nullptr ||
        (yy->type != RVALUE_REAL && yy->type != RVALUE_INT32) || (xx->type != RVALUE_REAL && xx->type != RVALUE_INT32) ||
        yy->ownsReference || xx->ownsReference)
        return generated(ctx);
    const RValue* dodraw[5];
    const RValue* snowy[5];
    const RValue* snowx[5];
    const RValue* moveme[5];
    if (!sfRows(inst, a[2], dodraw) || !sfRows(inst, a[3], snowy) || !sfRows(inst, a[4], snowx) || !sfRows(inst, a[5], moveme))
        return generated(ctx);
    Instance** list = runner->instancesByObject[a[8]];
    if (arrlen(list) != 1) return generated(ctx);
    Instance* other = list[0];
    if (!other->active || other == inst || Collision_getSprite(ctx->dataWin, other) == nullptr) return generated(ctx);
    int32_t boxL = sfSide(ctx, other, a[9]), boxR = sfSide(ctx, other, a[10]);
    int32_t boxT = sfSide(ctx, other, a[11]), boxB = sfSide(ctx, other, a[12]);
    float edgeL = (float) boxL, edgeR = (float) (boxR + 1), edgeT = (float) boxT, edgeB = (float) (boxB + 1);
    for (int32_t col = 0; 5 > col; col++) {
        for (int32_t row = 0; 5 > row; row++) {
            const RValue* px = &snowx[col][row];
            const RValue* py = &snowy[col][row];
            if (!SF_IS_ONE(&dodraw[col][row]) || !SF_IS_ONE(&moveme[col][row]) ||
                (px->type != RVALUE_REAL && px->type != RVALUE_INT32) || (py->type != RVALUE_REAL && py->type != RVALUE_INT32))
                return generated(ctx);
            // utfCircle's bounds of the circle (radius 2) and its test of them against the box.
            float cx = (float) (px->type == RVALUE_REAL ? px->real : (GMLReal) px->int32);
            float cy = (float) (py->type == RVALUE_REAL ? py->real : (GMLReal) py->int32);
            float x1 = (float) ((double) cx - (double) 2.0f), y1 = (float) ((double) cy - (double) 2.0f);
            float x2 = (float) ((double) cx + (double) 2.0f), y2 = (float) ((double) cy + (double) 2.0f);
            float minX = x2 > x1 ? x1 : x2, maxX = x1 > x2 ? x1 : x2, minY = y2 > y1 ? y1 : y2, maxY = y1 > y2 ? y1 : y2;
            if (!(minX >= edgeR || edgeL > maxX || minY >= edgeB || edgeT > maxY)) return generated(ctx);
        }
    }
    // 0000-0014: draw_set_color(16777215).
    RValue c[4];
    AOT_SET_INT(&c[0], 16777215, GML_TYPE_VARIABLE);
    ctx->ip = 0x14;
    setColor(ctx, c, 1);
    // 008C-0124 for each ball, by row: draw_circle(snowx, snowy, 2.8, 0), first argument lowest (as aotCallBuiltin hands them over).
    AOT_SET_REAL(&c[2], (GMLReal) aotBitsDouble(0x4006666666666666ULL), GML_TYPE_VARIABLE);
    AOT_SET_INT(&c[3], 0, GML_TYPE_VARIABLE);
    for (int32_t row = 0; 5 > row; row++) {
        for (int32_t col = 0; 5 > col; col++) {
            vmWeakCopyInto(&c[0], &snowx[col][row]);
            vmWeakCopyInto(&c[1], &snowy[col][row]);
            ctx->ip = 0x124;
            circle(ctx, c, 4);
        }
    }
    // 0880-0890 of the last ball: the loop leaves xx at 0 and yy at 5, reals typed as a variable (Pop.v.i made them reals).
    // Over plain numbers that own nothing, aotStoreInto's store is this one.
    AOT_SET_REAL(yy, (GMLReal) 5, GML_TYPE_VARIABLE);
    AOT_SET_REAL(xx, (GMLReal) 0, GML_TYPE_VARIABLE);
    ctx->ip = 0x1C8;
    return aotx_scriptFallthroughReturnValue(ctx);
}
#endif

#endif
