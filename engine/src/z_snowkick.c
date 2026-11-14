// UNDERTALE's snow floor (its Draw, in the snow puzzle room, 16 instances of 25 balls each), Old 3DS: a native body
// for a tile whose balls have been kicked (snowkick, 11-08).
//
// z_snowfloor.c's SnowFloor_draw takes a tile whose balls are all at rest and nowhere near the player. Any other tile
// it hands to its generated function, which gmlaot.py's UTF_NATIVE_NEXT makes this body (with the generated function
// behind it). Once the balls are kicked about, that is most tiles while the player walks among them: the generated
// Draw then reads four array cells a ball through the general array path and calls collision_circle for every ball,
// 400 a frame, and the room drops frames on an Old 3DS (maintainer, 11-08: "it goes down to 27 but there are held frames").
//
// Before anything happens this body checks, so that nothing has been drawn or written when the tile goes to the
// generated function instead: every variable there, the cells where they should be, numbers; the three arrays the
// event writes (snowx, snowy, moveme) ones popArrayElementIntoSlot writes in place, without a copy; one active player
// object with a sprite; its moving and box numbers; and the event's flag write out of reach: room isn't 57, or
// global.flag[64] is a number other than 0 (the first kick sets it; the tiles of that frame stay generated).
//
// Then the event's own steps, ball by ball in its order: the same builtins with the same arguments (draw_circle,
// collision_circle, random), the same comparisons (vmCmpOrder's epsilon), the same arithmetic (vmArithNumbers and
// vmDivNumbers, each result behind a barrier, as the generated code keeps it from -ffast-math), the same array writes
// (aotx_popArrayElementIntoSlot, the generated code's own), and ctx->ip where each call leaves it. The one thing left
// out is collision_circle for a ball whose circle box doesn't reach the player's box: utfCircle's test (z_utflames.c)
// on the bbox_* values, the same as SnowFloor_draw's, so such a ball is one collision_circle answers noone for, with
// no side effect. xx and yy end as the loop leaves them.
//
// The bytecode is checked at build time (UTF_NATIVE's "fnv:" digest of the event, the same on the Steam and GOG data);
// any other bytecode keeps the generated function. Built for UNDERTALE Old only (CMakeLists.txt). Late code only and
// last (late_reserve.ld): its code in .text_snowkick, gmlaot's wrapper and operands in .text_snowkick and
// .text_snowkick_ops. No statics, no constants outside the code.
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
#include <stdlib.h>

#define SK_LATE __attribute__((noinline, section(".text_snowkick")))
// Helpers always inlined: an out-of-line copy would land in .text and move everything after it.
#define SK_INLINE static inline __attribute__((always_inline))

RValue SnowFloor_moving(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) SK_LATE;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the generated code itself.
RValue SnowFloor_moving(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
#else
RValue* UtFlames_slot(Instance* inst, int32_t varID);

#define SK_IS_NUM(c) ((c)->type == RVALUE_REAL || (c)->type == RVALUE_INT32)
#define SK_NUM(c) ((c)->type == RVALUE_INT32 ? (GMLReal) (c)->int32 : (c)->real)

// The barrier the generated code puts on each instruction's result (AOT_OPAQUE): no arithmetic is folded across it.
#define SK_OPAQUE(v) __asm__("" : "+m"(v))

// vmWeakCopyInto (vm_fastpath.h), always inlined: Push.v of a cell or variable onto the stack.
SK_INLINE void skCopy(RValue* dst, const RValue* src) {
    *dst = *src;
    dst->ownsReference = false;
    dst->gmlStackType = GML_TYPE_VARIABLE;
}

// The array in a variable the event writes (a[xx, yy] op= ...), when it is a GameMaker 1 array of 5 rows of at least 5
// cells (a[xx, yy] is rows[xx].data[yy]) whose cells popArrayElementIntoSlot writes in place: this variable may write it
// without a copy. Else nullptr. dodraw, which the event only reads, needs only the shape (write false).
static GMLArray* skArray(Instance* inst, int32_t varID, bool write) SK_LATE;
static GMLArray* skArray(Instance* inst, int32_t varID, bool write) {
    RValue* slot = UtFlames_slot(inst, varID);
    if (slot == nullptr || slot->type != RVALUE_ARRAY || slot->array == nullptr) return nullptr;
    GMLArray* arr = slot->array;
    if (arr->type != GML_LEGACY_ARRAY || 5 > arr->legacy.rowCount) return nullptr;
    for (int32_t i = 0; 5 > i; i++) {
        if (5 > arr->legacy.rows[i].length) return nullptr;
    }
    if (write && arr->refCount > 1 && arr->owner != (void*) slot) return nullptr;
    return arr;
}

// A builtin variable (bbox_*, room) on inst as Push.v and PushBltn read it, the operand as theirs.
static RValue skBuiltin(VMContext* ctx, Instance* inst, int32_t ref) SK_LATE;
static RValue skBuiltin(VMContext* ctx, Instance* inst, int32_t ref) {
    const Variable* v = &ctx->dataWin->vari.variables[(uint32_t) ref & 0x07FFFFFFu];
    return VMBuiltins_getVariable(ctx, inst, v->builtinVarId, v->name, -1);
}

// Whether a ball's circle box reaches the player's box (z_snowfloor.c's test: utfCircle's bounds of a radius-2 circle
// against the box's edges as floats, right and bottom one past). A ball that doesn't is one collision_circle answers
// noone for.
SK_INLINE bool skReaches(const RValue* px, const RValue* py, float edgeL, float edgeR, float edgeT, float edgeB) {
    float cx = (float) SK_NUM(px), cy = (float) SK_NUM(py);
    float x1 = (float) ((double) cx - (double) 2.0f), y1 = (float) ((double) cy - (double) 2.0f);
    float x2 = (float) ((double) cx + (double) 2.0f), y2 = (float) ((double) cy + (double) 2.0f);
    float minX = x2 > x1 ? x1 : x2, maxX = x1 > x2 ? x1 : x2, minY = y2 > y1 ? y1 : y2, maxY = y1 > y2 ? y1 : y2;
    return !(minX >= edgeR || edgeL > maxX || minY >= edgeB || edgeT > maxY);
}

// What Cmp answers for two plain numbers (vmCmpNumbers), the difference behind a barrier as aotCmpFastK keeps it.
SK_INLINE bool skCmp(const RValue* a, const RValue* b, uint8_t kind) {
    GMLReal diff = SK_NUM(a) - SK_NUM(b);
    AOT_OPAQUE_REAL(diff);
    return vmCmpHolds(vmCmpOrder(diff), kind) != 0;
}

// An array write at cell, typed as the event's Pop.v (type2 Variable): the generated code's own (AOT_CAWRITE's fast
// path), which skArray has made sure takes it.
SK_INLINE void skWrite(VMContext* ctx, RValue* slot, int32_t cell, RValue* v, bool compound) {
    RValue ix;
    AOT_SET_INT(&ix, cell, GML_TYPE_INT32);
    if (!aotx_popArrayElementIntoSlot(ctx, slot, &ix, v, GML_TYPE_VARIABLE, compound)) abort();
}

// a: the variable IDs of yy, xx, dodraw, snowy, snowx and moveme; draw_set_color's and draw_circle's function indices;
// the player object (0x138); the operands of bbox_left, bbox_right, bbox_top and bbox_bottom (0x2D8, 0x3AC, 0x480,
// 0x554) (SnowFloor_draw's), then the variable IDs of moving and flag; collision_circle's and random's function indices;
// room's operand (0x28C), the room it is compared with (0x294) and the flag's index (0x2A4).
RValue SnowFloor_moving(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    (void) k;
    Instance* inst = (Instance*) ctx->currentInstance;
    Runner* runner = ctx->runner;
    if (inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX || runner == nullptr || !runner->collisionCompatibilityMode ||
        (uint32_t) a[6] >= ctx->funcCallCacheCount || (uint32_t) a[7] >= ctx->funcCallCacheCount ||
        (uint32_t) a[15] >= ctx->funcCallCacheCount || (uint32_t) a[16] >= ctx->funcCallCacheCount ||
        0 > a[8] || (uint32_t) a[8] >= ctx->dataWin->objt.count || 0 > a[19])
        return generated(ctx);
    BuiltinFunc setColor = (BuiltinFunc) ctx->funcCallCache[a[6]].builtin;
    BuiltinFunc circle = (BuiltinFunc) ctx->funcCallCache[a[7]].builtin;
    BuiltinFunc collide = (BuiltinFunc) ctx->funcCallCache[a[15]].builtin;
    BuiltinFunc rnd = (BuiltinFunc) ctx->funcCallCache[a[16]].builtin;
    RValue* yy = UtFlames_slot(inst, a[0]);
    RValue* xx = UtFlames_slot(inst, a[1]);
    if (setColor == nullptr || circle == nullptr || collide == nullptr || rnd == nullptr || yy == nullptr || xx == nullptr ||
        !SK_IS_NUM(yy) || !SK_IS_NUM(xx) || yy->ownsReference || xx->ownsReference)
        return generated(ctx);
    GMLArray* dodraw = skArray(inst, a[2], false);
    GMLArray* snowy = skArray(inst, a[3], true);
    GMLArray* snowx = skArray(inst, a[4], true);
    GMLArray* moveme = skArray(inst, a[5], true);
    if (dodraw == nullptr || snowy == nullptr || snowx == nullptr || moveme == nullptr) return generated(ctx);
    RValue* sy = UtFlames_slot(inst, a[3]);
    RValue* sx = UtFlames_slot(inst, a[4]);
    RValue* sm = UtFlames_slot(inst, a[5]);
    for (int32_t col = 0; 5 > col; col++) {
        for (int32_t row = 0; 5 > row; row++) {
            if (!SK_IS_NUM(&dodraw->legacy.rows[col].data[row]) || !SK_IS_NUM(&snowy->legacy.rows[col].data[row]) ||
                !SK_IS_NUM(&snowx->legacy.rows[col].data[row]) || !SK_IS_NUM(&moveme->legacy.rows[col].data[row]))
                return generated(ctx);
        }
    }
    // The player: what Push.v of the player's variables reads (VM_findInstanceByTarget's first active instance), the only one.
    Instance** list = runner->instancesByObject[a[8]];
    if (arrlen(list) != 1) return generated(ctx);
    Instance* other = list[0];
    if (!other->active || other == inst || Collision_getSprite(ctx->dataWin, other) == nullptr) return generated(ctx);
    const RValue* movingSlot = UtFlames_slot(other, a[13]);
    if (movingSlot == nullptr || !SK_IS_NUM(movingSlot)) return generated(ctx);
    RValue moving = *movingSlot;
    // Its box, read once: nothing in the event changes the player.
    RValue bl = skBuiltin(ctx, other, a[9]), br = skBuiltin(ctx, other, a[10]);
    RValue bt = skBuiltin(ctx, other, a[11]), bb = skBuiltin(ctx, other, a[12]);
    if (!SK_IS_NUM(&bl) || !SK_IS_NUM(&br) || !SK_IS_NUM(&bt) || !SK_IS_NUM(&bb)) return generated(ctx);
    float edgeL = (float) (int32_t) SK_NUM(&bl), edgeR = (float) ((int32_t) SK_NUM(&br) + 1);
    float edgeT = (float) (int32_t) SK_NUM(&bt), edgeB = (float) ((int32_t) SK_NUM(&bb) + 1);
    // 028C-02D0: room == 57 && global.flag[64] == 0 writes the flag. Here it may only be false.
    RValue room = skBuiltin(ctx, inst, a[17]);
    if (!SK_IS_NUM(&room)) return generated(ctx);
    if (aotCmpFastK(&room, (GMLReal) a[18], CMP_EQ) != 0) {
        Instance* global = (Instance*) ctx->globalScopeInstance;
        const RValue* fs = global != nullptr ? UtFlames_slot(global, a[14]) : nullptr;
        if (fs == nullptr || fs->type != RVALUE_ARRAY || fs->array == nullptr) return generated(ctx);
        const GMLArray* fa = fs->array;
        if (fa->type != GML_LEGACY_ARRAY || fa->legacy.rowCount == 0 || a[19] >= fa->legacy.rows[0].length) return generated(ctx);
        RValue f = fa->legacy.rows[0].data[a[19]];
        if (!SK_IS_NUM(&f) || aotCmpFastK(&f, (GMLReal) 0, CMP_EQ) != 0) return generated(ctx);
    }

    // 0000-0014: draw_set_color(16777215).
    RValue c[6];
    AOT_SET_INT(&c[0], 16777215, GML_TYPE_VARIABLE);
    ctx->ip = 0x14;
    setColor(ctx, c, 1);
    for (int32_t row = 0; 5 > row; row++) {
        for (int32_t col = 0; 5 > col; col++) {
            // This ball's cells. No write here moves a row (skArray: no copy, no growth).
            RValue* px = &snowx->legacy.rows[col].data[row];
            RValue* py = &snowy->legacy.rows[col].data[row];
            RValue* pm = &moveme->legacy.rows[col].data[row];
            int32_t cell = col * GML_LEGACY_ARRAY_STRIDE + row;
            // 0030-0124: dodraw == 1 draws the ball, draw_circle(snowx, snowy, 2.8, 0), first argument lowest (as
            // aotCallBuiltin hands them over).
            RValue d = dodraw->legacy.rows[col].data[row];
            if (aotCmpFastK(&d, (GMLReal) 1, CMP_EQ) != 0) {
                skCopy(&c[0], px);
                skCopy(&c[1], py);
                AOT_SET_REAL(&c[2], (GMLReal) aotBitsDouble(0x4006666666666666ULL), GML_TYPE_VARIABLE);
                AOT_SET_INT(&c[3], 0, GML_TYPE_VARIABLE);
                ctx->ip = 0x124;
                circle(ctx, c, 4);
            }
            // 0128-0228: collision_circle(snowx, snowy, 2, the player, 0, 1) != noone sets moveme to
            // floor(random(4)) + 2. Only a ball whose circle box reaches the player's box can be touched.
            ctx->ip = 0x1C8;
            if (skReaches(px, py, edgeL, edgeR, edgeT, edgeB)) {
                skCopy(&c[0], px);
                skCopy(&c[1], py);
                AOT_SET_INT(&c[2], 2, GML_TYPE_VARIABLE);
                AOT_SET_INT(&c[3], a[8], GML_TYPE_VARIABLE);
                AOT_SET_INT(&c[4], 0, GML_TYPE_VARIABLE);
                AOT_SET_INT(&c[5], 1, GML_TYPE_VARIABLE);
                RValue hit = collide(ctx, c, 6);
                // collision_circle answers a number (an instance's id, or noone): never what handleCmp would take.
                if (!SK_IS_NUM(&hit)) abort();
                if (aotCmpFastK(&hit, (GMLReal) -4, CMP_NEQ) != 0) {
                    AOT_SET_INT(&c[0], 4, GML_TYPE_VARIABLE);
                    ctx->ip = 0x1E4;
                    RValue m = rnd(ctx, c, 1);
                    if (!SK_IS_NUM(&m)) abort();
                    // 01E4: floor (AOT_MATH1's result, a real typed Variable), 01EC-01F0: + 2 (PushI.e 2, Add.i.v).
                    GMLReal fl = aotFloorOut(SK_NUM(&m));
                    AOT_SET_REAL(&m, fl, GML_TYPE_VARIABLE);
                    SK_OPAQUE(m);
                    RValue two;
                    AOT_SET_INT(&two, 2, GML_TYPE_INT16);
                    vmArithNumbers(&m, &two, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    SK_OPAQUE(m);
                    skWrite(ctx, sm, cell, &m, false);
                }
            }
            // 0230-0840: moveme > 1 pushes the ball while the player moves, and then moveme -= 1.
            if (aotCmpFastK(pm, (GMLReal) 1, CMP_GT) == 0) continue;
            if (aotCmpFastK(&moving, (GMLReal) 1, CMP_EQ) != 0) {
                RValue v, w;
                // 02D8-03A4: bbox_left > snowx: snowx -= moveme. 03AC-0478: bbox_right < snowx: snowx += moveme.
                if (skCmp(&bl, px, CMP_GT)) {
                    skCopy(&v, px);
                    skCopy(&w, pm);
                    vmArithNumbers(&v, &w, VM_ARITH_SUB, GML_TYPE_VARIABLE);
                    SK_OPAQUE(v);
                    skWrite(ctx, sx, cell, &v, true);
                }
                if (skCmp(&br, px, CMP_LT)) {
                    skCopy(&v, px);
                    skCopy(&w, pm);
                    vmArithNumbers(&v, &w, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    SK_OPAQUE(v);
                    skWrite(ctx, sx, cell, &v, true);
                }
                // 0480-054C: bbox_top > snowy: snowy -= moveme. 0554-0620: bbox_bottom < snowy: snowy += moveme.
                if (skCmp(&bt, py, CMP_GT)) {
                    skCopy(&v, py);
                    skCopy(&w, pm);
                    vmArithNumbers(&v, &w, VM_ARITH_SUB, GML_TYPE_VARIABLE);
                    SK_OPAQUE(v);
                    skWrite(ctx, sy, cell, &v, true);
                }
                if (skCmp(&bb, py, CMP_LT)) {
                    skCopy(&v, py);
                    skCopy(&w, pm);
                    vmArithNumbers(&v, &w, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    SK_OPAQUE(v);
                    skWrite(ctx, sy, cell, &v, true);
                }
                // 0628-0708, then 0710-07F0: snowx += (random(moveme) - moveme / 2) / 2, then the same for snowy.
                for (int32_t axis = 0; 2 > axis; axis++) {
                    RValue r, h, q;
                    skCopy(&v, axis == 0 ? px : py);
                    skCopy(&c[0], pm);
                    ctx->ip = axis == 0 ? 0x6AC : 0x794;
                    r = rnd(ctx, c, 1);
                    if (!SK_IS_NUM(&r)) abort();
                    r.gmlStackType = GML_TYPE_VARIABLE;
                    SK_OPAQUE(r);
                    skCopy(&h, pm);
                    AOT_SET_INT(&q, 2, GML_TYPE_INT16); // PushI.e 2, then Conv.i.d (vmConvFast's 0x02)
                    q.real = (GMLReal) q.int32;
                    q.type = RVALUE_REAL;
                    q.gmlStackType = GML_TYPE_DOUBLE;
                    SK_OPAQUE(q);
                    vmDivNumbers(&h, &q, false, GML_TYPE_VARIABLE, ctx->currentCodeName);
                    SK_OPAQUE(h);
                    vmArithNumbers(&r, &h, VM_ARITH_SUB, GML_TYPE_VARIABLE);
                    SK_OPAQUE(r);
                    SK_OPAQUE(q); // the second PushI.e 2, Conv.i.d
                    vmDivNumbers(&r, &q, false, GML_TYPE_VARIABLE, ctx->currentCodeName);
                    SK_OPAQUE(r);
                    vmArithNumbers(&v, &r, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    SK_OPAQUE(v);
                    skWrite(ctx, axis == 0 ? sx : sy, cell, &v, true);
                }
            }
            // 07F8-0840: moveme -= 1 (PushI.e 1, Sub.i.v).
            RValue m, one;
            skCopy(&m, pm);
            AOT_SET_INT(&one, 1, GML_TYPE_INT16);
            vmArithNumbers(&m, &one, VM_ARITH_SUB, GML_TYPE_VARIABLE);
            SK_OPAQUE(m);
            skWrite(ctx, sm, cell, &m, true);
        }
    }
    // 0880-0890 of the last ball: the loop leaves xx at 0 and yy at 5, reals typed as a variable (Pop.v.i made them reals).
    // Over plain numbers that own nothing, aotStoreInto's store is this one (SnowFloor_draw's).
    AOT_SET_REAL(yy, (GMLReal) 5, GML_TYPE_VARIABLE);
    AOT_SET_REAL(xx, (GMLReal) 0, GML_TYPE_VARIABLE);
    return aotx_scriptFallthroughReturnValue(ctx);
}
#endif

#endif
