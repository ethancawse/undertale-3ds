// UNDERTALE's draw_self_border_true (the fire Asgore's sided fire draws, clipped to the battle box): a native body for
// the script. Per-effect code like this is used only where pixels and game state stay identical to the PC game.
//
// tools/aot/gmlaot.py binds the script here only when its bytecode is exactly UTF_NATIVE's "fnv:" digest for it (through
// the wrapper aotu_<k>, in .text.asgborder_late like this body). A file of its own: an addition to z_utflames.c would
// change where GCC puts that file's functions. All of it is late (late_reserve.ld: last before __late_text_end), so
// nothing that existed moves; its constants are unmerged (-fno-merge-constants, CMakeLists.txt) and named into the late
// text. A build without WAD 16 (DELTARUNE's) has none of it.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "instance.h"
#include "rvalue.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__arm__) && defined(__3DS__)
#define ASB_LATE __attribute__((noinline, section(".text.asgborder_late")))
#else
#define ASB_LATE __attribute__((noinline))
#endif

RValue UtFlames_borderDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) ASB_LATE;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the generated code itself.
RValue UtFlames_borderDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
#else
#define asbFind UtFlames_slot // z_utflames.c's IntRValueHashMap_findSlot copy: one for every UNDERTALE late file
RValue* asbFind(Instance* inst, int32_t varID);

#ifdef CTRPORT_ASG_PROBE
// Test builds only (asgprobe): calls run here [0], or handed to the generated code before the slots [1], for a
// missing slot [2], or for the border cells or objects [3] (ctr_asgprobe.c logs them at the end).
uint32_t asbProbe[4];
#define ASB_COUNT(i) (asbProbe[i]++)
#else
#define ASB_COUNT(i) ((void) 0)
#endif

// The variable a Push.v operand names (its index in the low 27 bits, as vm.c reads it).
static inline Variable* asbVar(VMContext* ctx, int32_t ref) {
    return &ctx->dataWin->vari.variables[(uint32_t) ref & 0x07FFFFFFu];
}

// A builtin variable's value on inst as the interpreter pushes it (VMBuiltins_getVariable, typed as a variable).
static void asbBuiltin(VMContext* ctx, Instance* inst, int32_t ref, RValue* out) ASB_LATE;
static void asbBuiltin(VMContext* ctx, Instance* inst, int32_t ref, RValue* out) {
    Variable* v = asbVar(ctx, ref);
    *out = VMBuiltins_getVariable(ctx, inst, v->builtinVarId, v->name, -1);
    out->gmlStackType = GML_TYPE_VARIABLE;
}

// out = a <op> b on weak copies of a and b (vmArithNumbers, as AOT_ADD / AOT_SUB / AOT_ADD_K / AOT_SUB_K do it; out of
// line: the call is a barrier, as AOT_OPAQUE is after each step in the generated code). out may be a. Both are plain
// numbers wherever it is called. A weak copy of an intermediate value (owning nothing, typed as a variable) or of the
// right operand changes nothing vmArithNumbers reads or writes.
static void asbBin(RValue* out, const RValue* a, const RValue* b, int op) ASB_LATE;
static void asbBin(RValue* out, const RValue* a, const RValue* b, int op) {
    RValue x, y;
    vmWeakCopyInto(&x, a);
    vmWeakCopyInto(&y, b);
    vmArithNumbers(&x, &y, op, GML_TYPE_VARIABLE);
    *out = x;
}

// A store with Pop.v.v's type2 5 (no conversion): the OP_POP fast path's store.
static void asbStore(RValue* slot, RValue* v) ASB_LATE;
static void asbStore(RValue* slot, RValue* v) {
    aotStoreInto(slot, v);
}

// The value > 0 (AOT_CMP_K_BRANCH and AOT_CMP_K's aotCmpFastK); a < b (AOT_CMP_BRANCH and AOT_CMP's vmCmpNumbers).
// Plain numbers wherever they are called.
static bool asbPositive(const RValue* p) ASB_LATE;
static bool asbPositive(const RValue* p) {
    RValue v;
    vmWeakCopyInto(&v, p);
    return aotCmpFastK(&v, (GMLReal) 0, CMP_GT) != 0;
}
static bool asbLess(const RValue* p, const RValue* q) ASB_LATE;
static bool asbLess(const RValue* p, const RValue* q) {
    RValue v, u;
    vmWeakCopyInto(&v, p);
    vmWeakCopyInto(&u, q);
    return vmCmpNumbers(&v, &u, CMP_LT) != 0;
}

// p = round(p) (01E8-0240): the bound builtin on the slot's value, its result typed as aotCallBuiltin pushes it.
static void asbRound(VMContext* ctx, BuiltinFunc round, RValue* p, uint32_t ip) ASB_LATE;
static void asbRound(VMContext* ctx, BuiltinFunc round, RValue* p, uint32_t ip) {
    RValue arg;
    vmWeakCopyInto(&arg, p);
    ctx->ip = ip;
    RValue r = round(ctx, &arg, 1);
    r.gmlStackType = GML_TYPE_VARIABLE;
    asbStore(p, &r);
}

// draw_self_border_true(offx, offy) (UTF_NATIVE's "fnv:DFDD618D3681C423") draws the calling fire's sprite clipped
// to the battle box. It starts from the whole sprite frame, measures how far the frame (moved by the two arguments)
// reaches past the left border object, the box's top edge, the right border object and the box's bottom edge,
// and trims each overhang that is positive. It then rounds the four edges and, when what is left of the frame is
// not empty, draws that part with draw_sprite_part at the matching place on the screen.
// The body below takes the same steps in the original order, with the same helpers, so every value it stores on
// the instance matches the generated code's.
//
// Up to some 200 sided fires call it a frame in Asgore's fight.
// a: the script's eleven variable IDs in first-use order (the eighth is the global holding the box edges), round's
// and draw_sprite_part's function indices, the two border objects (0x58, 0xD4), the operands of their x (0x58, 0xD4),
// and the operands of the sprite width and height builtins (0x18, 0x28). Everything a step
// could find other than a plain number (the arguments, the border cells), a variable that isn't there yet (the first
// call on an instance), a border object without an instance or a builtin not bound yet goes to the generated code
// before anything is written. Each value is then worked out as the generated code works it (the same helpers, a
// barrier after each step) and stored where it stores it; the builtins get the arguments it passes them.
RValue UtFlames_borderDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    (void) k;
    Instance* inst = (Instance*) ctx->currentInstance;
    Instance* global = (Instance*) ctx->globalScopeInstance;
    if (inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX || global == nullptr ||
        (uint32_t) a[11] >= ctx->funcCallCacheCount || (uint32_t) a[12] >= ctx->funcCallCacheCount)
        return ASB_COUNT(1), generated(ctx);
    BuiltinFunc round = (BuiltinFunc) ctx->funcCallCache[a[11]].builtin;
    BuiltinFunc part = (BuiltinFunc) ctx->funcCallCache[a[12]].builtin;
    const RValue* args = ctx->scriptArgs;
    if (round == nullptr || part == nullptr || args == nullptr || 2 > ctx->scriptArgCount ||
        !vmIsPlainNumber(args[0].type) || !vmIsPlainNumber(args[1].type))
        return ASB_COUNT(1), generated(ctx);
    // The eleven variables in first-use order; the eighth (the box edges) is a global.
    RValue* p[11];
    for (int32_t i = 0; 11 > i; i++) {
        p[i] = asbFind(i == 7 ? global : inst, a[i]);
        if (p[i] == nullptr) return ASB_COUNT(2), generated(ctx);
    }
    RValue v, u, x, y;
    AOT_SET_INT(&u, 2, GML_TYPE_INT16);
    const RValue* b2 = aotArrayFastCell(ctx, p[7], &u);
    AOT_SET_INT(&u, 3, GML_TYPE_INT16);
    const RValue* b3 = aotArrayFastCell(ctx, p[7], &u);
    if (b2 == nullptr || b3 == nullptr || !vmIsPlainNumber(b2->type) || !vmIsPlainNumber(b3->type)) return ASB_COUNT(3), generated(ctx);
    Instance* lb = VM_findInstanceByTarget(ctx, a[13]);
    Instance* rb = VM_findInstanceByTarget(ctx, a[14]);
    if (lb == nullptr || rb == nullptr || lb->objectIndex == STRUCT_OBJECT_INDEX || rb->objectIndex == STRUCT_OBJECT_INDEX)
        return ASB_COUNT(3), generated(ctx);
    ASB_COUNT(0);
    RValue *pl = p[0], *pt = p[1], *pw = p[2], *ph = p[3], *pox = p[4], *poy = p[5];
    RValue one;
    AOT_SET_INT(&one, 1, GML_TYPE_INT16);
    AOT_SET_REAL(&x, (GMLReal) inst->x, GML_TYPE_VARIABLE);   // AOT_SELF_FIELD's push of x
    AOT_SET_REAL(&y, (GMLReal) inst->y, GML_TYPE_VARIABLE);

    // 0000-0010: the left and top trims start at 0 (Pop.v.i: stored as reals).
    for (int32_t i = 0; 2 > i; i++) {
        AOT_SET_INT(&v, 0, GML_TYPE_INT16);
        coerceIntStoreToRealInPlace(&v, GML_TYPE_INT32);
        asbStore(p[i], &v);
    }
    // 0018-0050: the width and height start as the sprite's; the two offsets are the arguments.
    asbBuiltin(ctx, inst, a[17], &v);
    asbStore(pw, &v);
    asbBuiltin(ctx, inst, a[18], &v);
    asbStore(ph, &v);
    vmWeakCopyInto(&v, &args[0]);
    asbStore(pox, &v);
    vmWeakCopyInto(&v, &args[1]);
    asbStore(poy, &v);
    // 0058-0080: the left overhang, from the left border object's x.
    asbBuiltin(ctx, lb, a[15], &v);
    asbBin(&v, &v, &x, VM_ARITH_SUB);
    asbBin(&v, &v, &one, VM_ARITH_ADD);
    asbBin(&v, &v, pox, VM_ARITH_SUB);
    asbStore(p[6], &v);
    // 0088-00B8: the top overhang, from the box's top edge.
    asbBin(&v, b2, &y, VM_ARITH_SUB);
    asbBin(&v, &v, &one, VM_ARITH_ADD);
    asbBin(&v, &v, poy, VM_ARITH_SUB);
    asbStore(p[8], &v);
    // 00C0-00E8: the right overhang, from the right border object's x.
    asbBin(&v, &x, pw, VM_ARITH_ADD);
    asbBuiltin(ctx, rb, a[16], &u);
    asbBin(&v, &v, &u, VM_ARITH_SUB);
    asbBin(&v, &v, &one, VM_ARITH_SUB);
    asbStore(p[9], &v);
    // 00F0-0120: the bottom overhang, from the box's bottom edge.
    asbBin(&v, &y, ph, VM_ARITH_ADD);
    asbBin(&v, &v, b3, VM_ARITH_SUB);
    asbBin(&v, &v, &one, VM_ARITH_SUB);
    asbStore(p[10], &v);
    // 0128-01E0: the trims (each overhang that is positive moves its edge in).
    for (int32_t i = 0; 4 > i; i++) {
        RValue* q = p[i == 0 ? 6 : i + 7];
        if (!asbPositive(q)) continue;
        asbBin(&v, p[i], q, 2 > i ? VM_ARITH_ADD : VM_ARITH_SUB);
        asbStore(p[i], &v);
    }
    // 01E8-0240: the roundings.
    asbRound(ctx, round, pw, 0x1F8);
    asbRound(ctx, round, ph, 0x210);
    asbRound(ctx, round, pl, 0x228);
    asbRound(ctx, round, pt, 0x240);
    // 0248-02C4: the tests.
    asbBin(&v, pw, pox, VM_ARITH_ADD);
    if (!asbPositive(&v)) return aotx_scriptFallthroughReturnValue(ctx);
    asbBin(&v, ph, poy, VM_ARITH_ADD);
    if (!asbPositive(&v) || !asbLess(pl, pw) || !asbLess(pt, ph)) return aotx_scriptFallthroughReturnValue(ctx);
    // 02C8-0370: draw_sprite_part's arguments, first argument lowest (as aotCallBuiltin hands them over).
    RValue c[8];
    asbBin(&c[7], &y, pt, VM_ARITH_ADD);    // destination y
    asbBin(&c[7], &c[7], poy, VM_ARITH_SUB);
    asbBin(&c[6], &x, pl, VM_ARITH_ADD);    // destination x
    asbBin(&c[6], &c[6], pox, VM_ARITH_SUB);
    asbBin(&c[5], ph, pt, VM_ARITH_SUB);    // part height
    asbBin(&c[5], &c[5], poy, VM_ARITH_ADD);
    asbBin(&c[4], pw, pl, VM_ARITH_SUB);    // part width
    asbBin(&c[4], &c[4], pox, VM_ARITH_ADD);
    vmWeakCopyInto(&c[3], pt);
    vmWeakCopyInto(&c[2], pl);
    AOT_SET_REAL(&c[1], (GMLReal) inst->imageIndex, GML_TYPE_VARIABLE);
    AOT_SET_REAL(&c[0], (GMLReal) inst->spriteIndex, GML_TYPE_VARIABLE);
    ctx->ip = 0x370;
    RValue r = part(ctx, c, 8);
    if (r.ownsReference) RValue_free(&r);
    return aotx_scriptFallthroughReturnValue(ctx);
}
#endif

#endif
