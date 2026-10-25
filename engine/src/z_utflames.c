// UNDERTALE's fire attacks: native bodies for the bullets' own code (utflames; per-effect code
// is allowed when pixels and game state stay identical).
//
// tools/aot/gmlaot.py binds a code entry here only when its bytecode is exactly a shape listed in UTF_NATIVE, through
// a small wrapper (aotu_<k>) that passes the operands the shape lists (variables' IDs, a function index, operand
// words) and the entry's generated function. A body does what the generated function does, step by step, with the
// same helpers (vm_fastpath.h, the AOT sites) and the same barriers between steps, so every value rounds as it would
// there. It runs the generated function instead when a precondition fails before it has changed anything, and hands
// the rest of the call to the interpreter (aotDeopt, at the instruction's address with the stack as the generated code
// would have it) for the rare branches: a destroy, a hit.
//
// The saving is the shared tail, collision_rectangle on the bullet's own box (utfRect): the box is worked out once
// instead of once per bbox read.
//
// All code is late (.text.late.utflames, platform/3ds/late_reserve.ld), so nothing that existed moves. A build without
// WAD 16 (DELTARUNE's) has none of it.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)

#include "aot/vm_aot.h"
#include "collision.h"
#include "instance.h"
#include "rvalue.h"
#include "runner.h"
#include "spatial_grid.h"
#include "vm.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__arm__) && defined(__3DS__)
#define UTF_LATE __attribute__((noinline, section(".text.late.utflames")))
#else
#define UTF_LATE __attribute__((noinline))
#endif

RValue UtFlames_sineStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_genericStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_circleStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_sidedStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_emberStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the generated code itself: every body is just it.
RValue UtFlames_sineStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_genericStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_circleStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_sidedStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_emberStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
#else
// The helpers' inline bodies once each, out of the bodies' way.
static RValue* utfSlot(Instance* inst, int32_t varID) UTF_LATE;
static RValue* utfSlot(Instance* inst, int32_t varID) {
    return IntRValueHashMap_findSlot(&inst->selfVars, varID);
}

// The variable a Push.v or Pop.v.v operand names (its index in the low 27 bits, as vm.c reads it), as the generated
// code's V[...].
// gmlaot only emits operands whose index is below the VARI count.
static inline Variable* utfVar(VMContext* ctx, int32_t ref) {
    return &ctx->dataWin->vari.variables[(uint32_t) ref & 0x07FFFFFFu];
}

// self when it is an instance (no struct), else nullptr; the builtin funcIndex is bound to, or nullptr.
static inline Instance* utfSelf(VMContext* ctx) {
    Instance* inst = (Instance*) ctx->currentInstance;
    return inst != nullptr && inst->objectIndex != STRUCT_OBJECT_INDEX ? inst : nullptr;
}
static inline BuiltinFunc utfBuiltin(VMContext* ctx, int32_t funcIndex) {
    return (uint32_t) funcIndex < ctx->funcCallCacheCount ? (BuiltinFunc) ctx->funcCallCache[funcIndex].builtin : nullptr;
}

// The fire bullets' shared tail, with the stack top at B:
//     if (collision_rectangle(bbox_left, bbox_top, bbox_right, bbox_bottom, operand, 0, 1)) event_user(0);
// The generated code reads each of the four bbox values through the builtin-variable switch, working the box out four
// times, then turns seven stack values back into numbers for the call. Here the box is worked out once and the
// arguments go where aotCallBuiltin leaves them (first argument lowest), each as pushed: the box as
// VMBuiltins_getVariable reads it. ipConv: the address of the Conv.v.b after the call. Returns false, the stack top
// back at B, when the result is false as after a Conv.v.b that vmConvFast takes; else true with the result at S[0]
// and the stack top above it, for the interpreter from ipConv.
static bool utfRect(VMContext* ctx, int32_t B, Instance* inst, BuiltinFunc rect, int32_t operand, uint32_t ipConv) UTF_LATE;
static bool utfRect(VMContext* ctx, int32_t B, Instance* inst, BuiltinFunc rect, int32_t operand, uint32_t ipConv) {
    Runner* runner = ctx->runner;
    RValue* S = &ctx->stack.slots[B];
    InstanceBBox box = Collision_computeBBox(runner, inst);
    if (box.valid) {
        AOT_SET_REAL(&S[0], box.left, GML_TYPE_VARIABLE);
        AOT_SET_REAL(&S[1], box.top, GML_TYPE_VARIABLE);
        AOT_SET_REAL(&S[2], runner->collisionCompatibilityMode ? box.right - 1 : box.right, GML_TYPE_VARIABLE);
        AOT_SET_REAL(&S[3], runner->collisionCompatibilityMode ? box.bottom - 1 : box.bottom, GML_TYPE_VARIABLE);
    } else {
        AOT_SET_REAL(&S[0], inst->x, GML_TYPE_VARIABLE);
        AOT_SET_REAL(&S[1], inst->y, GML_TYPE_VARIABLE);
        AOT_SET_REAL(&S[2], inst->x, GML_TYPE_VARIABLE);
        AOT_SET_REAL(&S[3], inst->y, GML_TYPE_VARIABLE);
    }
    AOT_SET_INT(&S[4], operand, GML_TYPE_VARIABLE);
    AOT_SET_INT(&S[5], 0, GML_TYPE_VARIABLE);
    AOT_SET_INT(&S[6], 1, GML_TYPE_VARIABLE);
    ctx->stack.top = B + 7;
    ctx->ip = ipConv;
    RValue hit = rect(ctx, S, 7);
    S[0] = hit;
    S[0].gmlStackType = GML_TYPE_VARIABLE;
    ctx->stack.top = B + 1;
    RValue t = S[0];
    if (!vmConvFast(&t, 0x45) || t.int32 != 0) return true;
    ctx->stack.top = B;
    return false;
}

// VMBuiltins_setVariable's x or y (field) from a real.
static void utfSetPos(Runner* runner, Instance* inst, float* field, GMLReal v) UTF_LATE;
static void utfSetPos(Runner* runner, Instance* inst, float* field, GMLReal v) {
    float value = (float) v;
    if (value != *field) {
        *field = (float) v;
        SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
    }
}

// Step of the fire bullet on a sine wave (UTF_NATIVE's first shape):
//     v0 += 1; x += sin(v0 / v1) * v2;
//     if (y > <the room's height> + 100) instance_destroy();
//     <the rectangle test>
// a: v0, v1 and v2's IDs, collision_rectangle's function index, the object operand. Some 250 run every frame at
// Asgore's peak.
RValue UtFlames_sineStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    // Preconditions, before anything changes: an instance (no struct), v0, v1 and v2 there holding plain numbers
    // (the generated fast paths' case, with nothing to free on v0's store) and the builtin bound.
    Instance* inst = utfSelf(ctx);
    if (inst == nullptr) return generated(ctx);
    RValue* p0 = utfSlot(inst, a[0]);
    RValue* p1 = utfSlot(inst, a[1]);
    RValue* p2 = utfSlot(inst, a[2]);
    BuiltinFunc rect = utfBuiltin(ctx, a[3]);
    if (p0 == nullptr || p1 == nullptr || p2 == nullptr || p0->ownsReference || !vmIsPlainNumber(p0->type) ||
        !vmIsPlainNumber(p1->type) || !vmIsPlainNumber(p2->type) || rect == nullptr)
        return generated(ctx);
    Runner* runner = ctx->runner;
    const int32_t B = ctx->stack.top;
    RValue* S = &ctx->stack.slots[B];
    __asm__("" : "+r"(S));
    RValue kv;

    // 0000-0010: v0 += 1 (Push.v, PushI.e 1, Add.i.v, Pop.v.v).
    vmWeakCopyInto(&S[0], p0);
    AOT_SET_INT(&kv, 1, GML_TYPE_INT16);
    vmArithNumbers(&S[0], &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);
    AOT_OPAQUE(0);
    coerceIntStoreToRealInPlace(&S[0], GML_TYPE_VARIABLE);
    *p0 = S[0];

    // 0018-004C: x = x + sin(v0 / v1) * v2.
    AOT_SET_REAL(&S[0], (GMLReal) inst->x, GML_TYPE_VARIABLE);
    AOT_OPAQUE(0);
    vmWeakCopyInto(&S[1], p0);
    vmWeakCopyInto(&S[2], p1);
    vmDivNumbers(&S[1], &S[2], false, GML_TYPE_VARIABLE, ctx->currentCodeName);
    AOT_OPAQUE(1);
    {
        GMLReal r = aotMathSin(AOT_NUM(&S[1]));
        AOT_SET_REAL(&S[1], r, GML_TYPE_VARIABLE);
        AOT_OPAQUE(1);
    }
    vmWeakCopyInto(&S[2], p2);
    vmArithNumbers(&S[1], &S[2], VM_ARITH_MUL, GML_TYPE_VARIABLE);
    AOT_OPAQUE(1);
    vmArithNumbers(&S[0], &S[1], VM_ARITH_ADD, GML_TYPE_VARIABLE);
    AOT_OPAQUE(0);
    utfSetPos(runner, inst, &inst->x, S[0].real);   // S[0] is a real (x was one): RValue_toReal gives S[0].real

    // 0054-0070: if (y > <the room's height> + 100); the destroy (0074) and what follows go to the interpreter.
    AOT_SET_REAL(&S[0], (GMLReal) inst->y, GML_TYPE_VARIABLE);
    AOT_OPAQUE(0);
    AOT_SET_REAL(&S[1], runner->currentRoom == nullptr ? (GMLReal) -1.0 : (GMLReal) runner->currentRoom->height, GML_TYPE_VARIABLE);
    AOT_OPAQUE(1);
    AOT_SET_INT(&kv, 100, GML_TYPE_INT16);
    vmArithNumbers(&S[1], &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);
    AOT_OPAQUE(1);
    if (vmCmpNumbers(&S[0], &S[1], CMP_GT) != 0) {
        ctx->stack.top = B;
        return aotDeopt(ctx, k, 0x74);
    }

    // 0080-00D8: the rectangle test; a hit goes to the interpreter from the Conv.v.b (00C0).
    if (utfRect(ctx, B, inst, rect, a[4], 0xC0)) return aotDeopt(ctx, k, 0xC0);
    return aotx_scriptFallthroughReturnValue(ctx);
}

// Step of the fire bullet that slows down (UTF_NATIVE's second shape):
//     <the rectangle test>
//     speed -= v0;
// a: collision_rectangle's function index, the object operand, the speed operand (Pop.v.v at 0070), v0's ID. The
// speed read and write go the generated code's ways (the sites).
RValue UtFlames_genericStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    Instance* inst = utfSelf(ctx);
    if (inst == nullptr) return generated(ctx);
    BuiltinFunc rect = utfBuiltin(ctx, a[0]);
    RValue* p0 = utfSlot(inst, a[3]);
    if (rect == nullptr || p0 == nullptr || !vmIsPlainNumber(p0->type)) return generated(ctx);
    const int32_t B = ctx->stack.top;
    RValue* S = &ctx->stack.slots[B];
    __asm__("" : "+r"(S));

    // 0000-0058: the rectangle test; a hit goes to the interpreter from the Conv.v.b (0040).
    if (utfRect(ctx, B, inst, rect, a[1], 0x40)) return aotDeopt(ctx, k, 0x40);

    // 005C-0070: speed = speed - v0.
    uint32_t ip = aotSiteSelfBltn(ctx, B, AOT_SITE_WORD(0x5C, 8, 0, 1), utfVar(ctx, a[2]));
    if (ip != 0) return aotDeopt(ctx, k, ip);
    vmWeakCopyInto(&S[1], p0);
    if (!vmArithNumbers(&S[0], &S[1], VM_ARITH_SUB, GML_TYPE_VARIABLE)) {
        ctx->stack.top = B + 2;
        return aotDeopt(ctx, k, 0x6C);
    }
    AOT_OPAQUE(0);
    ip = aotSitePopVar(ctx, B + 1, (uint32_t) (0x70 >> 2) | ((uint32_t) GML_TYPE_VARIABLE << 28), (uint32_t) a[2]);
    if (ip != 0) return aotDeopt(ctx, k, ip);
    ctx->stack.top = B;
    return aotx_scriptFallthroughReturnValue(ctx);
}

// Step of the fire bullet circling a point (UTF_NATIVE's third shape):
//     v0 -= v1; v2 += v3; if (v0 <= 0.5) instance_destroy();
//     x = v4 + lengthdir_x(v0, v2); y = v5 + lengthdir_y(v0, v2);
//     <the rectangle test>
// a: v0..v5's IDs, collision_rectangle's function index, the object operand.
RValue UtFlames_circleStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    Instance* inst = utfSelf(ctx);
    if (inst == nullptr) return generated(ctx);
    RValue* p[6];
    for (int i = 0; i < 6; i++) {
        p[i] = utfSlot(inst, a[i]);
        if (p[i] == nullptr || !vmIsPlainNumber(p[i]->type)) return generated(ctx);
    }
    BuiltinFunc rect = utfBuiltin(ctx, a[6]);
    if (rect == nullptr || p[0]->ownsReference || p[2]->ownsReference) return generated(ctx);
    Runner* runner = ctx->runner;
    const int32_t B = ctx->stack.top;
    RValue* S = &ctx->stack.slots[B];
    __asm__("" : "+r"(S));

    // 0000-0014: v0 = v0 - v1; 001C-0030: v2 = v2 + v3.
    for (int i = 0; i < 4; i += 2) {
        vmWeakCopyInto(&S[0], p[i]);
        vmWeakCopyInto(&S[1], p[i + 1]);
        vmArithNumbers(&S[0], &S[1], i == 0 ? VM_ARITH_SUB : VM_ARITH_ADD, GML_TYPE_VARIABLE);
        AOT_OPAQUE(0);
        coerceIntStoreToRealInPlace(&S[0], GML_TYPE_VARIABLE);
        *p[i] = S[0];
    }

    // 0038-0050: if (v0 <= 0.5); the destroy (0054) and what follows go to the interpreter.
    vmWeakCopyInto(&S[0], p[0]);
    if (aotCmpFastK(&S[0], (GMLReal) 0.5, CMP_LTE) != 0) {
        ctx->stack.top = B;
        return aotDeopt(ctx, k, 0x54);
    }

    // 0060-0084: x = v4 + lengthdir_x(v0, v2); 008C-00B0: y = v5 + lengthdir_y(v0, v2).
    for (int axis = 0; axis < 2; axis++) {
        vmWeakCopyInto(&S[0], p[4 + axis]);
        vmWeakCopyInto(&S[1], p[2]);
        vmWeakCopyInto(&S[2], p[0]);
        AOT_OPAQUE(2);
        AOT_OPAQUE(1);
        GMLReal r = axis == 0 ? aotMathLengthdirX(AOT_NUM(&S[2]), AOT_NUM(&S[1])) : aotMathLengthdirY(AOT_NUM(&S[2]), AOT_NUM(&S[1]));
        AOT_SET_REAL(&S[1], r, GML_TYPE_VARIABLE);
        AOT_OPAQUE(1);
        vmArithNumbers(&S[0], &S[1], VM_ARITH_ADD, GML_TYPE_VARIABLE);
        AOT_OPAQUE(0);
        // S[0] is a real (a real was added to it): RValue_toReal gives S[0].real.
        utfSetPos(runner, inst, axis == 0 ? &inst->x : &inst->y, S[0].real);
    }

    // 00B8-0110: the rectangle test; a hit goes to the interpreter from the Conv.v.b (00F8).
    if (utfRect(ctx, B, inst, rect, a[7], 0xF8)) return aotDeopt(ctx, k, 0xF8);
    return aotx_scriptFallthroughReturnValue(ctx);
}

// Step of the fire bullet crossing the box (UTF_NATIVE's fourth shape):
//     if (y > global.v0[3] && vspeed > 0) instance_destroy();
//     if (y < global.v0[2] - sprite_height && vspeed < 0) instance_destroy();
//     <the rectangle test>
// a: collision_rectangle's function index, the object operand, the operands of the array read (0010), vspeed (0020)
// and sprite_height (0060). Those reads go the generated code's slow ways (the sites); a subtraction or comparison
// of anything but numbers goes to the interpreter from its instruction.
RValue UtFlames_sidedStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    Instance* inst = utfSelf(ctx);
    if (inst == nullptr) return generated(ctx);
    BuiltinFunc rect = utfBuiltin(ctx, a[0]);
    if (rect == nullptr) return generated(ctx);
    const int32_t B = ctx->stack.top;
    RValue* S = &ctx->stack.slots[B];
    __asm__("" : "+r"(S));
    Variable* border = utfVar(ctx, a[2]);
    Variable* vspeed = utfVar(ctx, a[3]);

    // pass 0: 0000-0044 (index 3, Cmp GT, destroy at 003C); pass 1: 0048-0098 (index 2 less sprite_height at 0060,
    // Cmp LT, destroy at 0090).
    for (int pass = 0; pass < 2; pass++) {
        const uint32_t at = pass == 0 ? 0x00 : 0x48;
        const uint8_t kind = pass == 0 ? CMP_GT : CMP_LT;
        AOT_SET_REAL(&S[0], (GMLReal) inst->y, GML_TYPE_VARIABLE);
        AOT_OPAQUE(0);
        AOT_SET_INT(&S[1], -5, GML_TYPE_INT16);
        AOT_OPAQUE(1);
        AOT_SET_INT(&S[2], pass == 0 ? 3 : 2, GML_TYPE_INT16);
        AOT_OPAQUE(2);
        uint32_t ip = aotSitePushArray(ctx, B + 3, AOT_SITE_WORD(at + 0x10, 8, 3, 2), border);
        if (ip != 0) return aotDeopt(ctx, k, ip);
        if (pass == 1) {
            ip = aotSiteSelfBltn(ctx, B + 2, AOT_SITE_WORD(0x60, 8, 2, 3), utfVar(ctx, a[4]));
            if (ip != 0) return aotDeopt(ctx, k, ip);
            if (!vmArithNumbers(&S[1], &S[2], VM_ARITH_SUB, GML_TYPE_VARIABLE)) {
                ctx->stack.top = B + 3;
                return aotDeopt(ctx, k, 0x68);
            }
            AOT_OPAQUE(1);
        }
        int r = aotCmpFast(&S[0], &S[1], kind);
        if (r < 0) {
            ctx->stack.top = B + 2;
            return aotDeopt(ctx, k, pass == 0 ? 0x18 : 0x6C);
        }
        if (r == 0) continue;
        ip = aotSiteSelfBltn(ctx, B, AOT_SITE_WORD(pass == 0 ? 0x20 : 0x74, 8, 0, 1), vspeed);
        if (ip != 0) return aotDeopt(ctx, k, ip);
        r = aotCmpFastK(&S[0], (GMLReal) 0, kind);
        if (r < 0) {
            ctx->stack.top = B + 1;
            return aotDeopt(ctx, k, pass == 0 ? 0x28 : 0x7C);
        }
        if (r != 0) {
            ctx->stack.top = B;
            return aotDeopt(ctx, k, pass == 0 ? 0x3C : 0x90);
        }
    }

    // 009C-00F4: the rectangle test; a hit goes to the interpreter from the Conv.v.b (00DC).
    ctx->stack.top = B;
    if (utfRect(ctx, B, inst, rect, a[1], 0xDC)) return aotDeopt(ctx, k, 0xDC);
    return aotx_scriptFallthroughReturnValue(ctx);
}

// VMBuiltins_setVariable's image_xscale or image_yscale (field) from a number.
static void utfSetScale(Runner* runner, Instance* inst, float* field, GMLReal v) UTF_LATE;
static void utfSetScale(Runner* runner, Instance* inst, float* field, GMLReal v) {
    float value = (float) v;
    if (value != *field) {
        *field = value;
        SpatialGrid_markInstanceAsDirty(runner->spatialGrid, inst);
    }
}

// a + k (Add/Sub of a pushed real constant, AOT_ADD_R/AOT_SUB_R) in place, a a plain number.
static inline void utfArithK(RValue* a, uint64_t kbits, uint8_t op) {
    RValue kv;
    AOT_SET_REAL(&kv, (GMLReal) aotBitsDouble(kbits), GML_TYPE_DOUBLE);
    vmArithNumbers(a, &kv, op, GML_TYPE_VARIABLE);
}

// Step of the embers floating behind Asgore's fight (UTF_NATIVE's fifth shape), 140-190 of them every frame:
//     if (v0 > v1 + 0.1) v1 += 0.03; if (v0 < v1 - 0.1) v1 -= 0.03;
//     image_xscale = v1; image_yscale = v1; image_alpha -= 0.01; if (image_alpha < 0.01) instance_destroy();
//     v2 += 1; x += sin(v2 / v3) * v4; y += cos(v2 / v3) * v5;
// a: v0..v5's IDs. The builtin writes are VMBuiltins_setVariable's, without the generated code's way there (a site
// that reads the bytecode, then the variable's definition, then the setter's switch).
RValue UtFlames_emberStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    Instance* inst = utfSelf(ctx);
    if (inst == nullptr) return generated(ctx);
    RValue* p[6];
    for (int i = 0; i < 6; i++) {
        p[i] = utfSlot(inst, a[i]);
        if (p[i] == nullptr || !vmIsPlainNumber(p[i]->type)) return generated(ctx);
    }
    if (p[1]->ownsReference || p[2]->ownsReference) return generated(ctx);
    Runner* runner = ctx->runner;
    const int32_t B = ctx->stack.top;
    RValue* S = &ctx->stack.slots[B];
    __asm__("" : "+r"(S));

    // 0000-0040: if (v0 > v1 + 0.1) v1 = v1 + 0.03; 0048-0088: if (v0 < v1 - 0.1) v1 = v1 - 0.03.
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t op = pass == 0 ? VM_ARITH_ADD : VM_ARITH_SUB;
        vmWeakCopyInto(&S[0], p[0]);
        vmWeakCopyInto(&S[1], p[1]);
        utfArithK(&S[1], 0x3FB999999999999AULL, op);
        AOT_OPAQUE(1);
        int r = vmCmpNumbers(&S[0], &S[1], pass == 0 ? CMP_GT : CMP_LT);
        if (r == 0) continue;
        vmWeakCopyInto(&S[0], p[1]);
        utfArithK(&S[0], 0x3F9EB851EB851EB8ULL, op);
        AOT_OPAQUE(0);
        coerceIntStoreToRealInPlace(&S[0], GML_TYPE_VARIABLE);
        *p[1] = S[0];
    }

    // 0090-00A8: image_xscale = v1; image_yscale = v1.
    utfSetScale(runner, inst, &inst->imageXscale, AOT_NUM(p[1]));
    utfSetScale(runner, inst, &inst->imageYscale, AOT_NUM(p[1]));

    // 00B0-00C8: image_alpha = image_alpha - 0.01; 00D0-00E8: if (image_alpha < 0.01), the destroy (00EC) and what
    // follows in the interpreter.
    AOT_SET_REAL(&S[0], (GMLReal) inst->imageAlpha, GML_TYPE_VARIABLE);
    AOT_OPAQUE(0);
    utfArithK(&S[0], 0x3F847AE147AE147BULL, VM_ARITH_SUB);
    AOT_OPAQUE(0);
    inst->imageAlpha = (float) S[0].real;   // S[0] is a real (image_alpha was one)
    AOT_SET_REAL(&S[0], (GMLReal) inst->imageAlpha, GML_TYPE_VARIABLE);
    AOT_OPAQUE(0);
    if (aotCmpFastK(&S[0], (GMLReal) aotBitsDouble(0x3F847AE147AE147BULL), CMP_LT) != 0) {
        ctx->stack.top = B;
        return aotDeopt(ctx, k, 0xEC);
    }

    // 00F8-0108: v2 += 1.
    {
        RValue kv;
        vmWeakCopyInto(&S[0], p[2]);
        AOT_SET_INT(&kv, 1, GML_TYPE_INT16);
        vmArithNumbers(&S[0], &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        AOT_OPAQUE(0);
        coerceIntStoreToRealInPlace(&S[0], GML_TYPE_VARIABLE);
        *p[2] = S[0];
    }

    // 0110-0144: x = x + sin(v2 / v3) * v4; 014C-0180: y = y + cos(v2 / v3) * v5.
    for (int axis = 0; axis < 2; axis++) {
        float* field = axis == 0 ? &inst->x : &inst->y;
        AOT_SET_REAL(&S[0], (GMLReal) *field, GML_TYPE_VARIABLE);
        AOT_OPAQUE(0);
        vmWeakCopyInto(&S[1], p[2]);
        vmWeakCopyInto(&S[2], p[3]);
        vmDivNumbers(&S[1], &S[2], false, GML_TYPE_VARIABLE, ctx->currentCodeName);
        AOT_OPAQUE(1);
        GMLReal t = axis == 0 ? aotMathSin(AOT_NUM(&S[1])) : aotMathCos(AOT_NUM(&S[1]));
        AOT_SET_REAL(&S[1], t, GML_TYPE_VARIABLE);
        AOT_OPAQUE(1);
        vmWeakCopyInto(&S[2], p[4 + axis]);
        vmArithNumbers(&S[1], &S[2], VM_ARITH_MUL, GML_TYPE_VARIABLE);
        AOT_OPAQUE(1);
        vmArithNumbers(&S[0], &S[1], VM_ARITH_ADD, GML_TYPE_VARIABLE);
        AOT_OPAQUE(0);
        utfSetPos(runner, inst, field, S[0].real);   // S[0] is a real (x and y are)
    }
    return aotx_scriptFallthroughReturnValue(ctx);
}
#endif

#endif // ENABLE_WAD16
