#ifndef _BS_VM_AOT_ESCAPE_H_
#define _BS_VM_AOT_ESCAPE_H_

// ===[ Escape elision: a `new C(...)` whose struct never leaves its function, kept in C ]===
//
// tools/aot/escape.py proves, at build time, that the struct a `new` makes in a generated function is only ever
// stored in one local, read and written member by member, and handed as argument0 to a script that does nothing but
// read and write that argument's members and return it (a vector-rotation script). Such a struct is never registered: its
// members live in locals of the generated function, the script runs as a C function generated from its own
// bytecode, and the ID the struct would have had is taken all the same (nextInstanceId moves exactly as it did).
//
// Fail closed, at every step:
// - The generic code stays in the function and runs whenever a guard fails: the constructor resolves to another code
//   entry, its static block hasn't run yet (A4 of docs/perf/reach-gate.md), an argument isn't a plain number, the
//   script resolves to something else.
// - Whenever the generated function leaves for the interpreter (a deopt, an exception, the stress build), every struct
//   still in C is materialized first (aoteMaterialize): registered under its reserved ID at its reserved registry
//   index, its constructor run on the saved arguments (so its members, methods and their order are the constructor's
//   own), then its data members set to the values they have in C. The interpreter carries on with a real struct.
// - A member write of anything but a plain number materializes the struct, then does the generic write.
//
// Release builds take the ID inline (AOTE_RESERVE); gate builds (BS_REACH_GATE) go through Runner_reserveStruct and
// Runner_materializeStruct, which trap the reserved ID and list it for the reach gate, and REACH_SHOULD_MATERIALIZE
// materializes at a region's exit under "materialize" and "stress=N". CTRPORT_AOT_ESCAPE=0 compiles every elided
// site to its generic code only (the identity check of the generated code itself).

#include <stdbool.h>
#include <stdint.h>

#include "vm.h"
#include "runner.h"
#include "rvalue.h"

#ifndef CTRPORT_AOT_ESCAPE
#define CTRPORT_AOT_ESCAPE 1
#endif
// A traced build hands every builtin call to handleCall, which reads the call's operand from the bytecode: the inlined
// script's calls have none to read in the caller's code.
#ifdef ENABLE_VM_TRACING
#undef CTRPORT_AOT_ESCAPE
#define CTRPORT_AOT_ESCAPE 0
#endif

#define AOTE_MAX_MEMBERS 4
#define AOTE_MAX_ARGS 4

#ifdef BS_REACH_GATE
typedef ReachReservation AoteRes;
#define AOTE_RESERVE(runner, ctor) Runner_reserveStruct((runner), (ctor))
#else
typedef struct AoteRes {
    int32_t id;
    int32_t registryIndex;
    int32_t ctorCodeIndex;
} AoteRes;
// Runner_createStruct's ID and registry slot, without the struct.
static inline __attribute__((always_inline)) AoteRes AOTE_RESERVE(struct Runner* runner, int32_t ctor) {
    AoteRes r;
    r.id = (int32_t) runner->nextInstanceId++;
    r.registryIndex = (int32_t) arrlen(runner->structInstances);
    r.ctorCodeIndex = ctor;
    return r;
}
#endif

// The constructor's data members, in its own order: the VARI index of the variable of the constructor's Pop of each (the
// generated code pins the constructor's bytecode and references, so its name and varID are the ones the generator saw).
typedef struct AoteCtor {
    int32_t memberCount;
    int32_t memberVar[AOTE_MAX_MEMBERS];
} AoteCtor;

// Registers the struct reserved in *r under its ID at its registry index, runs its constructor on the argc saved
// arguments a (plain numbers, as the `new` passed them) with self the struct, sets its data members to m (the values they
// have in C), and stores an owning reference to it in *dst (which holds nothing that owns). minTop: a stack index the
// constructor's frame must start at or above (the caller's frame may hold values above ctx->stack.top).
void aoteMaterialize(VMContext* ctx, const AoteRes* r, const AoteCtor* ctor, const RValue* m, const RValue* a, int32_t argc,
                     RValue* dst, int32_t minTop);

// Whether every data member named like a builtin (x, y: varID < 0) already has the ID varNameMap gives the name. The
// constructor allocates it on its first write to a struct (structSetDataNamed); a struct kept in C writes nothing, so
// one the generic code would have allocated an ID for first must not be kept (the next dynamic name would take it).
// aoteNames (below) remembers a yes for the context's epoch: names never leave varNameMap within a context.
bool aoteNamesCold(VMContext* ctx, const AoteCtor* ctor);

typedef struct AoteDirCache {
    int32_t ready;   // 0 not asked yet (or the builtin wasn't bound when asked), 1 ready
    GMLReal pos, neg;
} AoteDirCache;
extern AoteDirCache aoteDir;
void aoteDirInit(VMContext* ctx, uint32_t funcIndex);

// The generated code's helpers (it includes aot/vm_aot.h first).
#ifdef _BS_VM_AOT_H_

// The pinned code entries (the constructors and the inlined scripts) are this game's: checked once per context epoch.
AOT_INLINE bool aoteBound(VMContext* ctx, AotInlineCache* cache, const AotInlineBinding* bindings, uint32_t count) {
    if (__builtin_expect(cache->epoch != aotEpoch, 0)) aotResolveInlineBindings(ctx, cache, bindings, count);
    return cache->valid;
}

AOT_INLINE bool aoteNames(VMContext* ctx, const AoteCtor* ctor, uint32_t* known) {
    if (__builtin_expect(*known == aotEpoch, 1)) return true;
    if (!aoteNamesCold(ctx, ctor)) return false;
    *known = aotEpoch;
    return true;
}

// The constructor's statics exist, so its isstaticok is true and its static block doesn't run (reach-gate.md A4).
AOT_INLINE bool aoteStatics(VMContext* ctx, int32_t codeIndex) {
    return ctx->staticInitialized != nullptr && ctx->staticInitialized[codeIndex];
}

// Call.i of funcIndex would run code entry codeIndex as a plain script call (aotCallScript's direct path): no builtin
// of the name, no forward.
AOT_INLINE bool aoteScript(VMContext* ctx, uint32_t funcIndex, int32_t codeIndex) {
    const FuncCallCache* c = &ctx->funcCallCache[funcIndex];
    return c->builtin == nullptr && c->scriptCodeIndex == codeIndex && c->forwardChecked && c->forward == nullptr;
}

// A struct member write, `v.m = val` (simple: coerced by type2, as tryPopStructMember does) or `v.m += ...` (compound:
// as it is): false, changing nothing, unless val is a plain number.
AOT_INLINE bool aoteStoreMember(RValue* member, const RValue* val, bool simple, uint8_t type2) {
    if (__builtin_expect(!vmIsPlainNumber(val->type), 0)) return false;
    RValue v = *val;
    if (simple) coerceIntStoreToRealInPlace(&v, type2);
    *member = v;
    return true;
}

// point_direction(0, 0, x, y) and point_distance(0, 0, x, y) when y is +0 (a real with all bits clear, or int32 0)
// and x a finite real with 1e-150 < |x| < 1e150: the builtin's own result without atan2 or sqrt. The builtin computes
// atan2(+0, x), which C99 (Annex F) makes +0 for every x > 0 and pi for every x < 0, so its result takes one of two
// values: aoteDir holds the builtin's own result for x = 1 and x = -1, asked of the bound builtin once. sqrt(x * x) is
// |x| wherever x * x neither overflows nor loses bits to underflow (both operations correctly rounded). Anything else
// returns false and the builtin runs. The arguments are as the call leaves them on the stack: args[0] is S[d - 1].
AOT_INLINE bool aoteZeroY(const RValue* y) {
    if (y->type == RVALUE_INT32) return y->int32 == 0;
    return y->type == RVALUE_REAL && aotDoubleBits(y->real) == 0;
}
AOT_INLINE bool aoteRealX(const RValue* x, double* out) {
    if (x->type == RVALUE_REAL) *out = x->real;
    else if (x->type == RVALUE_INT32) *out = (double) x->int32;
    else return false;
    double ax = __builtin_fabs(*out);
    // NaN fails both compares (the engine is built with -ffast-math, so test the bits as well).
    return (aotDoubleBits(ax) & 0x7FF0000000000000ULL) != 0x7FF0000000000000ULL && ax > 1e-150 && 1e150 > ax;
}
// d is the stack depth at the call (four arguments on top).
AOT_INLINE bool aotePointDirection00(VMContext* ctx, RValue* S, int32_t d, uint32_t funcIndex) {
#ifdef USE_FLOAT_REALS
    (void) ctx; (void) S; (void) d; (void) funcIndex;
    return false;   // the bounds below are a double's
#else
    double x;
    if (!aoteZeroY(&S[d - 1]) || !aoteZeroY(&S[d - 2]) || !aoteZeroY(&S[d - 4]) || !aoteRealX(&S[d - 3], &x)) return false;
    if (__builtin_expect(aoteDir.ready != 1, 0)) {
        if (aoteDir.ready == 0) aoteDirInit(ctx, funcIndex);
        if (aoteDir.ready != 1) return false;
    }
    AOT_SET_REAL(&S[d - 4], x > 0 ? aoteDir.pos : aoteDir.neg, GML_TYPE_VARIABLE);
    AOT_OPAQUE(d - 4);
    return true;
#endif
}
AOT_INLINE bool aotePointDistance00(RValue* S, int32_t d) {
#ifdef USE_FLOAT_REALS
    (void) S; (void) d;
    return false;
#else
    double x;
    if (!aoteZeroY(&S[d - 1]) || !aoteZeroY(&S[d - 2]) || !aoteZeroY(&S[d - 4]) || !aoteRealX(&S[d - 3], &x)) return false;
    AOT_SET_REAL(&S[d - 4], (GMLReal) __builtin_fabs(x), GML_TYPE_VARIABLE);
    AOT_OPAQUE(d - 4);
    return true;
#endif
}

#endif /* _BS_VM_AOT_H_ */

#endif /* _BS_VM_AOT_ESCAPE_H_ */
