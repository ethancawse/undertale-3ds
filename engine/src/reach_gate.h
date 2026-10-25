#ifndef _BS_REACH_GATE_H_
#define _BS_REACH_GATE_H_

// The reach-mode identity gate for struct escape analysis. runner.h includes this file at
// its end, so every file that looks instances up by ID sees the accessor below.
//
// Release builds (BS_REACH_GATE undefined) get exactly the tokens the call sites had before:
//   RUNNER_INSTANCE_BY_ID(runner, id)        -> hmget((runner)->instancesById, (id))
//   REACH_CALLV_FIND(ctx, target, receiver)  -> VM_findInstanceByTarget((ctx), (target))
//   REACH_GATE_HOOK(call)                    -> ((void) 0)
// so the preprocessed code, and with it every byte of .text, is unchanged. The hooks sit at the end of existing source
// lines, so no line number moves either (safeMalloc and require() embed __LINE__).
//
// Gate builds (the desktop CLI runner, and platform/3ds/build.sh ... reachgate) define BS_REACH_GATE and compile
// engine/src/cli/reach_gate.c. Nothing changes until the gate is configured: statehash.txt (3DS) or $BS_REACH
// (desktop) holding the word "reach" and its options (reach_gate.c, "Configuration").

#ifdef BS_REACH_GATE

#include <stdint.h>
#include <stdbool.h>
#include "stb_ds.h"

struct Runner;
struct Instance;
struct VMContext;
struct RValue;

// Non-zero while any ID is trapped (reserved by a scalar-replaced allocation and not yet past the sweep that would have
// freed it; or, in a base run checking a typed run's list, listed and still registered). The accessor's fast path.
extern int32_t ReachGate_trapCount;

void ReachGate_trapHit(struct Runner* runner, int32_t id, const char* file, int line);
bool ReachGate_isTrapped(int32_t id);

// Every instancesById lookup in the engine goes through this (A1: the trap sits at the map, not at a list of builtins).
#define RUNNER_INSTANCE_BY_ID(runner, id) ReachGate_instanceById((runner), (id), __FILE__, __LINE__)
#define REACH_CALLV_FIND(ctx, target, receiver) ReachGate_findCallV((ctx), (target), &(receiver))
#define REACH_GATE_HOOK(call) call

// Hooks (each called from one place, named in reach_gate.c).
void ReachGate_onCreateStruct(struct Runner* runner, struct Instance* s);
void ReachGate_onSweep(struct Runner* runner);
void ReachGate_onSweepFree(struct Runner* runner, struct Instance* s);
void ReachGate_onReset(struct Runner* runner);
void ReachGate_onVmCreate(struct VMContext* ctx);
struct Instance* ReachGate_findCallV(struct VMContext* ctx, int32_t target, const struct RValue* receiver);

// ===[ The interface for a scalar-replaced allocation (typed3's generated code) ]===
//
// A replaced `new C(...)` takes its ID exactly where the generic code would (before the constructor body runs), and
// remembers where the struct would have gone in the registry so a deopt can put it there:
//
//   ReachReservation r = Runner_reserveStruct(runner, ctorCodeIndex);   // only once ctx->staticInitialized[ctor]
//   ... fields in C locals, methods called directly ...
//   if (guard fails || REACH_SHOULD_MATERIALIZE(runner)) {
//       Instance* s = Runner_materializeStruct(runner, &r, ctorCodeIndex);
//       // members in the constructor's insertion order, with the tags and gmlStackTypes it stores,
//       // fresh bound methods (GMLMethod_create(code, r.id)), then the generic code continues with s
//   }
//
// In a Release build Runner_reserveStruct is the two loads and the increment below. In a gate build it also checks
// the static guard (A4), traps the ID until the next sweep (A1) and lists it in the replaced-ID sidecar (A3).
typedef struct ReachReservation {
    int32_t id;            // the instance ID the struct would have had
    int32_t registryIndex; // arrlen(runner->structInstances) at that moment: where Runner_createStruct would put it
    int32_t ctorCodeIndex;
} ReachReservation;

ReachReservation Runner_reserveStruct(struct Runner* runner, int32_t ctorCodeIndex);
// Registers a struct under the reserved ID at the reserved registry index (shifting later entries up, so the
// registry is the one the generic code would have built) and ends the ID's trap. refCount 1 (the registry's), not
// pinned, constructorCodeIndex set; the caller adds the members.
struct Instance* Runner_materializeStruct(struct Runner* runner, const ReachReservation* r, int32_t ctorCodeIndex);
// true when a replaced region must materialize at its exit: always under "materialize", every Nth exit under
// "stress=N" (7, 31, 101, 997 as the AOT stress builds), never otherwise.
bool ReachGate_shouldMaterialize(struct Runner* runner);
#define REACH_SHOULD_MATERIALIZE(runner) ReachGate_shouldMaterialize(runner)

static inline struct Instance* ReachGate_instanceById(struct Runner* runner, int32_t id, const char* file, int line);

#else

#define RUNNER_INSTANCE_BY_ID(runner, id) hmget((runner)->instancesById, (id))
#define REACH_CALLV_FIND(ctx, target, receiver) VM_findInstanceByTarget((ctx), (target))
#define REACH_GATE_HOOK(call) ((void) 0)
// No replaced region exists in a Release build yet. When typed3 emits one, Release gets Runner_reserveStruct as
// { runner->nextInstanceId++, arrlen(runner->structInstances), ctor } inline in the generated module, and
// Runner_materializeStruct as a cold function in .text.late.<name>.
#define REACH_SHOULD_MATERIALIZE(runner) 0

#endif

#endif /* _BS_REACH_GATE_H_ */

// The accessor's body needs Runner, so it comes after runner.h's definitions (runner.h includes this file last).
#if defined(BS_REACH_GATE) && defined(_BS_RUNNER_H_) && !defined(_BS_REACH_GATE_ACCESSOR_)
#define _BS_REACH_GATE_ACCESSOR_
static inline struct Instance* ReachGate_instanceById(struct Runner* runner, int32_t id, const char* file, int line) {
    if (__builtin_expect(ReachGate_trapCount != 0, 0) && ReachGate_isTrapped(id)) ReachGate_trapHit(runner, id, file, line);
    return hmget(runner->instancesById, id);
}
#endif
