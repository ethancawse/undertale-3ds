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
#include <string.h>

#if defined(__arm__) && defined(__3DS__)
#define UTF_LATE __attribute__((noinline, section(".text.late.utflames")))
#else
#define UTF_LATE __attribute__((noinline))
#endif
// Code added since round 17 (and its constants and statics), in sections only explicit late_reserve.ld lines at the end
// take, so nothing linked before it moves; no jump tables or literals in this file's plain .rodata.
#if defined(__arm__) && defined(__3DS__)
#define UTF_LAST __attribute__((noinline, section(".text_utfnative_late")))
#define UTF_LAST_BSS __attribute__((section(".bss.utfnative_late")))
#else
#define UTF_LAST __attribute__((noinline))
#define UTF_LAST_BSS
#endif
// The sine shape's step since round 17 (the helix): its own sections, after the ones above (late_reserve.ld).
#if defined(__arm__) && defined(__3DS__)
#define UTF_HELIX __attribute__((noinline, section(".text_utfhelix_late")))
#define UTF_HELIX_BSS __attribute__((section(".bss.utfhelix_late")))
#else
#define UTF_HELIX __attribute__((noinline))
#define UTF_HELIX_BSS
#endif

RValue UtFlames_sineStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_helixStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_HELIX;
RValue UtFlames_genericStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_circleStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_sidedStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
RValue UtFlames_emberStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE;
void UtKill_registerAll(VMContext* ctx);                   // z_utkill_waver.c
void UtFlames_registerCircle(VMContext* ctx) UTF_LAST;  // VM_create, through COLFLIP_REGISTER_ALL (vm_builtins.h)

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the generated code itself: every body is just it.
RValue UtFlames_sineStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_helixStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_genericStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_circleStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_sidedStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
RValue UtFlames_emberStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
void UtFlames_registerCircle(VMContext* ctx) { UtKill_registerAll(ctx); }
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
#if defined(ENABLE_WAD17) // UNDERTALE never runs this (argcheck, UT late-text reclaim)
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
#endif
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

// ===[ Native answers in their own late section (.text_utfnative_late, last) ]===

static inline uint32_t utfBits32(float f) {
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static inline uint64_t utfBitsReal(GMLReal r) {
    uint64_t b = 0;
    memcpy(&b, &r, sizeof r);
    return b;
}

// Collision_computeBBox, with what it calls inlined here, out of line in this section (utfCircle's and the helix's).
// utfRect stays its only other caller in this file, so GCC keeps inlining it there as before: another inlined caller made GCC emit an out-of-line
// copy in this file's plain .text, which the early utflames line takes, moving what follows (docs/3ds-hardware.md).
static InstanceBBox utfBox(Runner* runner, Instance* inst) UTF_LAST __attribute__((flatten));
static InstanceBBox utfBox(Runner* runner, Instance* inst) {
    return Collision_computeBBox(runner, inst);
}

// ===[ collision_circle's no-hit answer (the snow floor, room_tundra_snowpuzz) ]===
//
// Each snow floor tile's Draw asks collision_circle(cx, cy, 2, obj, 0, 1) for each of its 25 cells: 400 calls a frame
// in the snowball puzzle room, which almost never touch anything. UNDERTALE's builtin (colline_gm14.c) reads each
// candidate's box as four bbox_* variable reads, each a whole Collision_computeBBox. utfCircle answers the call first
// when nothing can be hit: an object-index operand, every argument a number, compatibility mode, prec 0. Its
// candidates are the builtin's (Runner_pushInstancesForTarget's list for an object: instancesByObject), active, self
// left out when notme; one without a collision sprite is never hit (Colline14_box). Each other one's box is the one
// the bbox_* reads give (the int32 of Collision_computeBBox's sides, right and bottom less one in compatibility mode),
// kept in utfCirc with every input Collision_computeBBox reads. Colline14_ellipseHits starts with the
// test of that box against the circle's float32 box; when it fails for every candidate the builtin returns noone, and
// so does utfCircle. Any candidate passing it, or any other case, calls the builtin, which answers.
static struct {
    const Sprite* spr;
    uint32_t w, h, mode, x, y, xs, ys;
    int32_t ml, mr, mt, mb, ox, oy;
    uint64_t ang;
    bool compat;
    int32_t l, t, r, b;
} utfCirc UTF_LAST_BSS;
static BuiltinFunc utfCirclePrevious UTF_LAST_BSS;
static const char utfCircleName[] __attribute__((section(".rodata_utfnative_late"))) = "collision_circle";

// A number argument as RValue_toReal gives it (no copy of RValue_toRealOther here); false for any other type.
static inline bool utfArgReal(const RValue* v, GMLReal* out) {
    switch (v->type) {
        case RVALUE_REAL: *out = v->real; return true;
        case RVALUE_INT32: case RVALUE_BOOL: *out = (GMLReal) v->int32; return true;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: *out = (GMLReal) v->int64; return true;
#endif
        default: return false;
    }
}

static RValue utfCircle(VMContext* ctx, RValue* args, int32_t argCount) UTF_LAST __attribute__((optimize("no-jump-tables")));
static RValue utfCircle(VMContext* ctx, RValue* args, int32_t argCount) {
    Runner* runner = ctx->runner;
    GMLReal v[6];
    if (!runner->collisionCompatibilityMode || 6 > argCount) return utfCirclePrevious(ctx, args, argCount);
    for (int32_t i = 0; 6 > i; i++)
        if (!utfArgReal(&args[i], &v[i])) return utfCirclePrevious(ctx, args, argCount);
    int32_t target = (int32_t) v[3];
    if (v[4] > 0.5 || 0 > target || (uint32_t) target >= ctx->dataWin->objt.count) return utfCirclePrevious(ctx, args, argCount);
    // builtin_collision_circle_gm14's box and Colline14_ellipseHits's bounds of it.
    float cx = (float) v[0], cy = (float) v[1], r = (float) v[2];
    float x1 = (float) ((double) cx - (double) r), y1 = (float) ((double) cy - (double) r);
    float x2 = (float) ((double) cx + (double) r), y2 = (float) ((double) cy + (double) r);
    float minX = x2 > x1 ? x1 : x2, maxX = x1 > x2 ? x1 : x2, minY = y2 > y1 ? y1 : y2, maxY = y1 > y2 ? y1 : y2;
    bool notme = v[5] > 0.5;
    Instance* self = ctx->currentInstance;
    Instance** list = runner->instancesByObject[target];
    for (int32_t i = 0, n = (int32_t) arrlen(list); n > i; i++) {
        Instance* other = list[i];
        if (!other->active || (notme && other == self)) continue;
        const Sprite* spr = Collision_getSprite(ctx->dataWin, other);
        if (spr == nullptr) continue;
        uint32_t ox = utfBits32(other->x), oy = utfBits32(other->y);
        uint32_t oxs = utfBits32(other->imageXscale), oys = utfBits32(other->imageYscale);
        uint64_t oang = utfBitsReal(other->imageAngle);
        bool compat = runner->collisionCompatibilityMode;
        if (utfCirc.spr != spr || utfCirc.x != ox || utfCirc.y != oy || utfCirc.xs != oxs || utfCirc.ys != oys
            || utfCirc.ang != oang || utfCirc.compat != compat || utfCirc.w != spr->width || utfCirc.h != spr->height
            || utfCirc.mode != spr->bboxMode || utfCirc.ml != spr->marginLeft || utfCirc.mr != spr->marginRight
            || utfCirc.mt != spr->marginTop || utfCirc.mb != spr->marginBottom || utfCirc.ox != spr->originX
            || utfCirc.oy != spr->originY) {
            InstanceBBox bbox = utfBox(runner, other);
            // VMBuiltins_getVariable's bbox_left, bbox_top, bbox_right and bbox_bottom, as Colline14_boxSide reads them.
            utfCirc.l = (int32_t) RValue_makeReal(bbox.left).real;
            utfCirc.t = (int32_t) RValue_makeReal(bbox.top).real;
            utfCirc.r = (int32_t) RValue_makeReal(runner->collisionCompatibilityMode ? bbox.right - 1 : bbox.right).real;
            utfCirc.b = (int32_t) RValue_makeReal(runner->collisionCompatibilityMode ? bbox.bottom - 1 : bbox.bottom).real;
            utfCirc.spr = spr;
            utfCirc.x = ox;
            utfCirc.y = oy;
            utfCirc.xs = oxs;
            utfCirc.ys = oys;
            utfCirc.ang = oang;
            utfCirc.compat = compat;
            utfCirc.w = spr->width;
            utfCirc.h = spr->height;
            utfCirc.mode = spr->bboxMode;
            utfCirc.ml = spr->marginLeft;
            utfCirc.mr = spr->marginRight;
            utfCirc.mt = spr->marginTop;
            utfCirc.mb = spr->marginBottom;
            utfCirc.ox = spr->originX;
            utfCirc.oy = spr->originY;
        }
        if (minX >= (float) (utfCirc.r + 1) || (float) utfCirc.l > maxX) continue;
        if (minY >= (float) (utfCirc.b + 1) || (float) utfCirc.t > maxY) continue;
        return utfCirclePrevious(ctx, args, argCount);
    }
    return RValue_makeReal((GMLReal) INSTANCE_NOONE);
}

// UtKill_registerAll (z_utkill_waver.c, which stays as it was), then utfCircle over UNDERTALE's collision_circle.
void UtFlames_registerCircle(VMContext* ctx) {
    UtKill_registerAll(ctx);
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) utfCircleName);
    if (0 > idx || ctx->builtinMap[idx].value == nullptr) return;
    utfCirclePrevious = ctx->builtinMap[idx].value;
    ctx->builtinMap[idx].value = utfCircle;
}

// utfRectFast's soul box and its inputs (below).
static struct {
    const Sprite* spr;
    uint32_t w, h, mode, x, y, xs, ys;
    int32_t ml, mr, mt, mb, ox, oy;
    uint64_t ang;
    bool compat;
    InstanceBBox box;
} utfSoul UTF_HELIX_BSS;
// utfRect (above, the other shapes' tail), for the sine shape, with the same arguments and result.
//
// When the operand is an object index, collision_rectangle's answer is first worked out here as the builtin does: the
// four values (rounded in compatibility mode, then ordered), the object's instances (Runner_pushInstancesForTarget's
// list), each active one but self put to Collision_rectOverlapsInstance's first test, its box against the rectangle
// (prec is 0). No box touching means the builtin returns noone, which the Conv.v.b takes as false: the call is skipped,
// as nothing else it does outlasts it (its instance snapshot is popped). A box touching calls it as before, and it
// answers. The fire bullets are some 250 a frame and almost none touch the soul. The soul's box is kept (utfSoul) from
// the last call with everything Collision_computeBBox reads: its sprite (which and that sprite's size, margins, mode
// and origin, as scripts can change them), its x, y, scales and angle (by their bits) and the compatibility mode.
static bool utfRectFast(VMContext* ctx, int32_t B, Instance* inst, BuiltinFunc rect, int32_t operand, uint32_t ipConv) UTF_HELIX;
static bool utfRectFast(VMContext* ctx, int32_t B, Instance* inst, BuiltinFunc rect, int32_t operand, uint32_t ipConv) {
    Runner* runner = ctx->runner;
    RValue* S = &ctx->stack.slots[B];
    InstanceBBox box = utfBox(runner, inst);
    GMLReal v[4];
    if (box.valid) {
        v[0] = box.left;
        v[1] = box.top;
        v[2] = runner->collisionCompatibilityMode ? box.right - 1 : box.right;
        v[3] = runner->collisionCompatibilityMode ? box.bottom - 1 : box.bottom;
    } else {
        v[0] = v[2] = inst->x;
        v[1] = v[3] = inst->y;
    }
    if (0 <= operand && (uint32_t) operand < ctx->dataWin->objt.count) {
        GMLReal x1 = v[0], y1 = v[1], x2 = v[2], y2 = v[3];
        if (runner->collisionCompatibilityMode) {
            x1 = GMLReal_bankersRound(x1);
            y1 = GMLReal_bankersRound(y1);
            x2 = GMLReal_bankersRound(x2);
            y2 = GMLReal_bankersRound(y2);
        }
        if (x1 > x2) { GMLReal t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { GMLReal t = y1; y1 = y2; y2 = t; }
        Instance** list = runner->instancesByObject[operand];
        bool overlap = false;
        for (int32_t i = 0, n = (int32_t) arrlen(list); n > i && !overlap; i++) {
            Instance* other = list[i];
            if (!other->active || other == inst) continue;
            const Sprite* spr = Collision_getSprite(ctx->dataWin, other);
            if (spr == nullptr) continue;
            uint32_t ox = utfBits32(other->x), oy = utfBits32(other->y);
            uint32_t oxs = utfBits32(other->imageXscale), oys = utfBits32(other->imageYscale);
            uint64_t oang = utfBitsReal(other->imageAngle);
            bool compat = runner->collisionCompatibilityMode;
            if (utfSoul.spr != spr || utfSoul.x != ox || utfSoul.y != oy || utfSoul.xs != oxs || utfSoul.ys != oys
                || utfSoul.ang != oang || utfSoul.compat != compat || utfSoul.w != spr->width || utfSoul.h != spr->height
                || utfSoul.mode != spr->bboxMode || utfSoul.ml != spr->marginLeft || utfSoul.mr != spr->marginRight
                || utfSoul.mt != spr->marginTop || utfSoul.mb != spr->marginBottom || utfSoul.ox != spr->originX
                || utfSoul.oy != spr->originY) {
                utfSoul.box = utfBox(runner, other);
                utfSoul.spr = spr;
                utfSoul.x = ox;
                utfSoul.y = oy;
                utfSoul.xs = oxs;
                utfSoul.ys = oys;
                utfSoul.ang = oang;
                utfSoul.compat = compat;
                utfSoul.w = spr->width;
                utfSoul.h = spr->height;
                utfSoul.mode = spr->bboxMode;
                utfSoul.ml = spr->marginLeft;
                utfSoul.mr = spr->marginRight;
                utfSoul.mt = spr->marginTop;
                utfSoul.mb = spr->marginBottom;
                utfSoul.ox = spr->originX;
                utfSoul.oy = spr->originY;
            }
            const InstanceBBox* b = &utfSoul.box;
            overlap = b->valid && !(x1 >= b->right || b->left > x2 || y1 >= b->bottom || b->top > y2);
        }
        if (!overlap) return false;
    }
    AOT_SET_REAL(&S[0], v[0], GML_TYPE_VARIABLE);
    AOT_SET_REAL(&S[1], v[1], GML_TYPE_VARIABLE);
    AOT_SET_REAL(&S[2], v[2], GML_TYPE_VARIABLE);
    AOT_SET_REAL(&S[3], v[3], GML_TYPE_VARIABLE);
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

// aotMathSin's results by the argument's bits, so equal arguments (the helix's bullets made in one frame share v0 and
// v1) pay for one sine. An empty slot holds +0's bits and sin(+0) = +0, so it needs no flag.
static struct { uint64_t bits; GMLReal r; } utfSinMemo[64] UTF_HELIX_BSS;
static inline GMLReal utfSin(GMLReal x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    __typeof__(utfSinMemo[0])* slot = &utfSinMemo[(bits * 0x9E3779B97F4A7C15ull) >> 58];
    if (slot->bits != bits) {
        slot->r = aotMathSin(x);
        slot->bits = bits;
    }
    return slot->r;
}

// Step of the fire bullet on a sine wave (UTF_NATIVE's first shape):
//     v0 += 1; x += sin(v0 / v1) * v2;
//     if (y > <the room's height> + 100) instance_destroy();
//     <the rectangle test>
// a: v0, v1 and v2's IDs, collision_rectangle's function index, the object operand. Some 250 run every frame at
// Asgore's peak.
RValue UtFlames_helixStep(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
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
        GMLReal r = utfSin(AOT_NUM(&S[1]));
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
    if (utfRectFast(ctx, B, inst, rect, a[4], 0xC0)) return aotDeopt(ctx, k, 0xC0);
    return aotx_scriptFallthroughReturnValue(ctx);
}

#endif

// ===[ utgapfix: string_char_at's walk to the position, 16 plain ASCII bytes at a time ]===
//
// builtin_string_char_at (vm_builtins.c) walks from the string's start on every call. Code that reads a long string
// one character at a time pays for every character before the one it reads: the vaporize object's Alarm 0 reads its
// data string (6,255 characters for Undyne's melt) 212 times a frame at a rising position, up to 30 ms of the frame on an
// Old 3DS (kb_undyne, 7 frames held 3 vblanks). This walk gives the same answer for plain ASCII text, about four times
// faster; anything else (a byte past ASCII before or at the position, another argument type) goes to the builtin.
// Its sections are taken by explicit late_reserve.ld lines at the end, so nothing linked before it moves. Compiled for
// size: the 16-byte loop comes out the same as at -O2.
#if defined(__arm__) && defined(__3DS__)
#define UTG_LATE __attribute__((noinline, optimize("Os"), section(".text_utgapfix_late")))
#define UTG_RODATA __attribute__((section(".rodata_utgapfix_in_text")))
#define UTG_BSS __attribute__((section(".bss.utgapfix_late")))
#else
#define UTG_LATE __attribute__((noinline))
#define UTG_RODATA
#define UTG_BSS
#endif

typedef uint32_t __attribute__((may_alias)) UtgWord;
static BuiltinFunc utgCharAtPrevious UTG_BSS;
static const char utgCharAtName[] UTG_RODATA = "string_char_at";

// string_char_at(str, pos) for a string and a real. builtin_string_char_at takes pos - 1 (clamped at 0) and steps one
// character at a time; over plain ASCII each character is one byte, so it lands on the byte pos - 1 bytes in, or on the
// terminator first (""), which is where this walk lands too. It stops early at a byte past ASCII, and hands that case
// (and a character past ASCII at the position) to the builtin.
static UTG_LATE RValue utgCharAt(VMContext* ctx, RValue* args, int32_t argCount) {
    const uint8_t* p;
    if (2 <= argCount && args[0].type == RVALUE_STRING && (p = (const uint8_t*) args[0].string) != nullptr &&
        args[1].type == RVALUE_REAL) {
        int32_t pos = (int32_t) args[1].real - 1; // RValue_toInt32's conversion of a real
        uint32_t k = 0x01010101u;
        __asm__("" : "+r"(k)); // kept in a register (the compiler would rebuild it from immediates for every word)
        uint32_t ch;
        for (;;) {
            // On a 16-byte boundary, 16 bytes at a time while none is the terminator or past ASCII (a zero byte turns
            // into 0xFF when subtracting). An aligned 16-byte block never crosses a page, and p[0] (not yet known to be
            // past the terminator) is in it. (Not under AddressSanitizer, which would flag the bytes past the end.)
#if !defined(__SANITIZE_ADDRESS__)
            if (((uintptr_t) p & 15) == 0) {
                while (pos >= 16) {
                    const UtgWord* w = (const UtgWord*) p;
                    uint32_t a = w[0], b = w[1], c = w[2], d = w[3];
                    if ((((a | (a - k)) | (b | (b - k))) | ((c | (c - k)) | (d | (d - k)))) & (k << 7)) break;
                    p += 16;
                    pos -= 16;
                }
            }
#endif
            ch = *p;
            if (0 >= pos || ch - 1u >= 0x7Fu) break; // at the position, or at the terminator or a byte past ASCII
            p++;
            pos--;
        }
        if (0x80 > ch) return RValue_makeString(RValue_asciiStrings[ch]); // the terminator gives ""
    }
    return utgCharAtPrevious(ctx, args, argCount);
}

// What VM_create calls through COLFLIP_REGISTER_ALL (vm_builtins.h): UtFlames_registerCircle, then the walk over
// string_char_at's builtin.
void Utgap_registerAll(VMContext* ctx) UTG_LATE;
void Utgap_registerAll(VMContext* ctx) {
    UtFlames_registerCircle(ctx);
    ptrdiff_t idx = shgeti(ctx->builtinMap, (char*) utgCharAtName);
    if (0 > idx || ctx->builtinMap[idx].value == nullptr) return;
    utgCharAtPrevious = ctx->builtinMap[idx].value;
    ctx->builtinMap[idx].value = utgCharAt;
}

#endif // ENABLE_WAD16
