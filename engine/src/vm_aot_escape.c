// Escape elision's run-time half (aot/vm_aot_escape.h, tools/aot/escape.py): the materializer a generated function
// calls when an elided struct has to become a real one, and point_direction's two results. Both are cold. All late
// code and statics (platform/3ds/late_reserve.ld), and nothing at all in a build without WAD 17 (UNDERTALE has no
// constructors, so no generated code calls these).
#include "wad_versions.h"

#if IS_WAD17_OR_HIGHER_ENABLED

#include <string.h>

#include "aot/vm_aot_escape.h"
#include "instance.h"
#include "rvalue.h"
#include "runner.h"
#include "vm.h"
#include "stb_ds.h"

#if defined(__arm__) && defined(__3DS__)
#define AOTE_COLD __attribute__((noinline, cold, section(".text.late.drold")))
#define AOTE_BSS __attribute__((section(".bss.late.drold")))
#else
#define AOTE_COLD __attribute__((noinline, cold))
#define AOTE_BSS
#endif

AoteDirCache aoteDir AOTE_BSS;

// point_direction(0, 0, 1, 0) and point_direction(0, 0, -1, 0), asked of the builtin bound to funcIndex.
AOTE_COLD void aoteDirInit(VMContext* ctx, uint32_t funcIndex) {
    if (funcIndex >= ctx->funcCallCacheCount) return;
    BuiltinFunc builtin = (BuiltinFunc) ctx->funcCallCache[funcIndex].builtin;
    if (builtin == nullptr) return;
    GMLReal r[2];
    for (int32_t k = 0; 2 > k; k++) {
        RValue args[4] = { RValue_makeInt32(0), RValue_makeInt32(0), RValue_makeReal(k == 0 ? 1.0 : -1.0), RValue_makeInt32(0) };
        RValue v = builtin(ctx, args, 4);
        if (v.type != RVALUE_REAL) {
            RValue_free(&v);
            return;
        }
        r[k] = v.real;
    }
    aoteDir.pos = r[0];
    aoteDir.neg = r[1];
    aoteDir.ready = 1;
}

// The key a struct keeps a member under: the varID, or for a builtin's name (x, y, direction...) the id varNameMap gives
// the name (structSetDataNamed and VM_structSet).
AOTE_COLD static int32_t memberKey(VMContext* ctx, const Variable* var) {
    if (var->varID >= 0) return var->varID;
    ptrdiff_t i = shgeti(ctx->varNameMap, (char*) var->name);
    return i >= 0 ? ctx->varNameMap[i].value : VM_getOrAllocateVarID(ctx, var->name);
}

AOTE_COLD bool aoteNamesCold(VMContext* ctx, const AoteCtor* ctor) {
    const Variable* vars = ctx->dataWin->vari.variables;
    for (int32_t j = 0; ctor->memberCount > j && AOTE_MAX_MEMBERS > j; j++) {
        const Variable* var = &vars[ctor->memberVar[j]];
        if (0 > var->varID && 0 > shgeti(ctx->varNameMap, (char*) var->name)) return false;
    }
    return true;
}

AOTE_COLD void aoteMaterialize(VMContext* ctx, const AoteRes* r, const AoteCtor* ctor, const RValue* m, const RValue* a,
                               int32_t argc, RValue* dst, int32_t minTop) {
    Runner* runner = (Runner*) ctx->runner;
#ifdef BS_REACH_GATE
    Instance* s = Runner_materializeStruct(runner, r, r->ctorCodeIndex);
#else
    // Runner_createStruct with the ID it reserved, where the generic code would have put it in the registry: after
    // every struct made before it, ahead of every struct made after it (they move up). No sweep runs between the
    // reservation and here, so the entries from the reserved index on are the ones registered since, those made
    // later with higher IDs and other reserved structs of lower ones materialized first.
    Instance* s = Instance_create((uint32_t) r->id, STRUCT_OBJECT_INDEX, 0, 0);
    hmput(runner->instancesById, s->instanceId, s);
    int32_t n = (int32_t) arrlen(runner->structInstances);
    int32_t low = r->registryIndex;
    if (0 > low || low > n) low = n;
    int32_t at = n;
    while (at > low && runner->structInstances[at - 1]->instanceId > (uint32_t) r->id) at--;
    arrins(runner->structInstances, at, s);
    for (int32_t j = at; n >= j; j++) runner->structInstances[j]->structRegistryIndex = j;
    s->refCount = 1;
    s->constructorCodeIndex = r->ctorCodeIndex;
#endif
    // @@NewGMLObject@@'s constructor call. Whatever made the caller leave (an exception it has to deliver) waits until
    // the constructor has run, as it ran long before.
    Instance* savedSelf = (Instance*) ctx->currentInstance;
    VMException* savedException = ctx->exception;
    ctx->exception = nullptr;
    int32_t savedTop = ctx->stack.top;
    if (minTop > ctx->stack.top) ctx->stack.top = minTop;
    ctx->currentInstance = s;
    RValue args[AOTE_MAX_ARGS];
    for (int32_t i = 0; argc > i && AOTE_MAX_ARGS > i; i++) args[i] = a[i];
    RValue result = VM_callCodeIndex(ctx, r->ctorCodeIndex, argc > 0 ? args : nullptr, argc);
    RValue_free(&result);
    ctx->currentInstance = savedSelf;
    ctx->stack.top = savedTop;
    if (ctx->exception == nullptr) ctx->exception = savedException;
    // The data members as they are in C, in the slots the constructor made (so its insertion order stays).
    const Variable* vars = ctx->dataWin->vari.variables;
    for (int32_t j = 0; ctor->memberCount > j && AOTE_MAX_MEMBERS > j; j++) {
        int32_t key = memberKey(ctx, &vars[ctor->memberVar[j]]);
        RValue* slot = IntRValueHashMap_findSlot(&s->selfVars, key);
        if (slot != nullptr) {
            RValue_free(slot);
            *slot = m[j];
        } else {
            Instance_setSelfVar(s, key, m[j]);
        }
    }
    RValue_free(dst);
    *dst = RValue_makeStructAndIncRef(s);
}

#endif
