#pragma once

// Typed AOT regions (tools/aot/typed.py): what the generated aot_typed.c
// includes. A typed region is one loop of a generated function, with the variables it uses kept in C RValues and its
// stack temporaries in C scalars. The generated function calls it just before the loop and goes on after it from what
// it returns:
//   0                   a guard failed before anything happened: run the loop's generated code as always;
//   (target << 1) | 1   the loop left for target, with every variable written back: go there;
//   ip << 1             the region handed over at ip, with its variables written back and its operand stack in the
//                       VM's stack slots (ctx->stack.top set): finish the call in the interpreter (AOT_DEOPT_AT).
//
// Platform builds compile aot_typed.c without -ffast-math and FP contraction, so each double operation rounds once,
// as the interpreter's handlers do (they do one operation each on values in memory). Math builtins with more than
// one floating-point operation are called through copies compiled with the engine's own flags (aotTypedMath*), the
// way the interpreter computes them.

#include "aot/vm_aot.h"

#if VM_HAS_AOT

#if defined(__FAST_MATH__) && !defined(TYPED_ALLOW_FAST_MATH)
#error "aot_typed.c must be compiled with -fno-fast-math -ffp-contract=off (platform/3ds/CMakeLists.txt)"
#endif

// The values each kind of push or handler leaves in a stack slot: a real, an int32 (the 64-bit store AOT_SET_INT
// makes) and a bool, with gmlStackType g, no asset tag and nothing owned. Each sets the fields, as the push does, and
// leaves the padding: a compound literal would zero it, and an array of them became a memset per call in the loop.
static inline RValue typedKReal(GMLReal v, uint8_t g) {
    RValue r;
    AOT_SET_REAL(&r, v, g);
    return r;
}
#define TYPED_KR(v, g) typedKReal((v), (g))
static inline RValue typedKInt(int32_t v, uint8_t g, uint8_t type) {
    RValue r;
    AOT_SET_INT(&r, v, g);
    r.type = type;
    return r;
}
#define TYPED_KI(v, g) typedKInt((v), (g), RVALUE_INT32)
#define TYPED_KB(v, g) typedKInt((v), (g), RVALUE_BOOL)

// BT/BF on a real (stackPopInt32 != 0: aotTruth's conversion).
static inline bool typedTruthR(GMLReal x, uint8_t g) {
    RValue t = TYPED_KR(x, g);
    return aotTruth(&t);
}

// A builtin array variable an inlined script reads with a constant index (typed.py BUILTIN_ARRAY_READS, all of which
// return a real and none instance-scoped): what resolveVariableRead does for it, VMBuiltins_getVariable on the current
// instance with the variable's builtin id and name.
RValue VMBuiltins_getVariable(VMContext* ctx, Instance* inst, int16_t builtinVarId, const char* name, int32_t arrayIndex);
static inline GMLReal typedBuiltinArrayReal(VMContext* ctx, uint32_t varIndex, int32_t index) {
    const Variable* vd = &ctx->dataWin->vari.variables[varIndex];
    return VMBuiltins_getVariable(ctx, (Instance*) ctx->currentInstance, vd->builtinVarId, vd->name, index).real;
}

// A variable's value written back to its slot at a region exit: storeStackValueIntoSlot for the values a region
// stores (no string, array, struct or method: those hand over first).
#define TYPED_WB(p, v) do { if ((p)->ownsReference) RValue_free(p); *(p) = (v); } while (0)

// Every-entry check (CTRPORT_TYPED_STATS): per region, how often it was entered (0), failed a guard (1), handed over
// to the interpreter (2) and left normally (3); main.c logs the counts and last hand-over bytecode address with each
// stats window. Inlined scripts report their outer call address, where execution resumes.
#ifdef CTRPORT_TYPED_STATS
void aotTypedEvent(const char* tag, int what);
void aotTypedHandover(const char* tag, uint32_t ip);
#define TYPED_EVENT(tag, what) aotTypedEvent((tag), (what))
#define TYPED_HANDOVER(tag, ip) aotTypedHandover((tag), (ip))
#else
#define TYPED_EVENT(tag, what) ((void) 0)
#define TYPED_HANDOVER(tag, ip) ((void) 0)
#endif

// Guard-failure stress (CTRPORT_TYPED_STRESS=n): one region entry in n fails its guards (n = 1: all of them), and
// one block start in n inside a region hands over to the interpreter there.
#ifdef CTRPORT_TYPED_STRESS
extern uint32_t aotTypedStressEntries, aotTypedStressBlocks;
#define TYPED_STRESS_FAIL() (++aotTypedStressEntries % (CTRPORT_TYPED_STRESS) == 0)
#define TYPED_STRESS_HIT() (++aotTypedStressBlocks % (CTRPORT_TYPED_STRESS) == 0)
#else
#define TYPED_STRESS_FAIL() 0
#define TYPED_STRESS_HIT() 0
#endif

#define TYPED_FAIL() do { TYPED_EVENT(aottag_, 1); return 0; } while (0)

// Tracing and profiler builds run the generated code (they log or time what the regions would skip).
#if defined(ENABLE_VM_TRACING) || defined(ENABLE_VM_GML_PROFILER)
#define TYPED_PREAMBLE() do { (void) S; return 0; } while (0)
#else
#define TYPED_PREAMBLE() do { (void) S; if (TYPED_STRESS_FAIL()) TYPED_FAIL(); } while (0)
#endif

// The exception check AOT_CALL_BUILTIN makes after a builtin (AOT_CHECK; the stack depth can't change).
#define TYPED_EXCEPTION_PENDING() AOT_EXCEPTION_PENDING()

// Math builtins whose aotMath copy does more than one floating-point operation: compiled in vm_aot_bridge.inc with
// the engine's flags, as the interpreter's builtins are.
GMLReal aotTypedMathDsin(GMLReal x);
GMLReal aotTypedMathDcos(GMLReal x);
GMLReal aotTypedMathDegtorad(GMLReal x);
GMLReal aotTypedMathRadtodeg(GMLReal x);
GMLReal aotTypedMathFrac(GMLReal x);
GMLReal aotTypedMathSign(GMLReal x);
GMLReal aotTypedMathMax(GMLReal a, GMLReal b);
GMLReal aotTypedMathMin(GMLReal a, GMLReal b);
GMLReal aotTypedMathLengthdirX(GMLReal len, GMLReal dir);
GMLReal aotTypedMathLengthdirY(GMLReal len, GMLReal dir);
GMLReal aotTypedMathAngleDifference(GMLReal src, GMLReal dest);
GMLReal aotTypedMathClamp(GMLReal val, GMLReal lo, GMLReal hi);
GMLReal aotTypedMathLerp(GMLReal a, GMLReal b, GMLReal t);
GMLReal aotTypedMathPointDistance(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2);

#endif
