// ===[ Ahead-of-time compiled GML: the runtime half ]===
//
// Included at the end of vm.c when CTRPORT_AOT is defined. tools/aot/gmlaot.py reads the
// player's own data.win at build time and translates the hottest code entries into C functions, written to
// aot_gen_<n>.c and aot_table.c in the build directory (never to the repository: they are derived from the game).
// Those files are compiled on their own and see only aot/vm_aot.h. This file gives them what they need from the
// interpreter: its handlers (the aotx_ functions below, each a plain call of the static function in vm.c), the
// out-of-line halves of the fast paths, and the fallback to executeLoop. The generated code stays exactly as dynamic
// as the interpreter: every value is still an RValue on the VM stack, every variable access still goes through the
// instance's hash map, every call through handleCall.
//
// What the generated code removes is the interpreter's per-instruction work: fetching and decoding the instruction,
// the dispatch switch, reading the operands back out of the bytecode, keeping ctx->stack.top up to date (the stack
// depth at each instruction is known when the code is generated, so slots are addressed as S[depth]), looking up a
// local's slot in the per-code hash map on every access, and the branch through a bool on the stack after a
// comparison.
//
// Binding: the first time a code entry runs, aotDispatch looks for a generated function with its name and checks that
// the code is byte-for-byte the code the function was generated from (a digest of its bytecode after
// patchReferenceOperands has resolved the variable and function operands to indices, and the name, varID and scope of
// every variable and the name of every function it uses). Anything else (another version of the game, another
// chapter) runs in the interpreter as before.
//
// Falling back: whenever the generated code meets something it does not expect (a handler leaving the stack at a
// depth other than the one it was generated for, a pending exception), it stores the stack depth and the address of
// the next instruction and finishes the call in executeLoop, which carries on from exactly that point. The generated
// code keeps the interpreter's stack layout at every instruction boundary, so this is always possible.

#include "aot/vm_aot.h"

// ---[ The interpreter's functions, for the generated code ]---

RValue aotx_executeLoop(VMContext* ctx) { return executeLoop(ctx); }
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx) { return scriptFallthroughReturnValue(ctx); }
void aotx_stackPushTyped(VMContext* ctx, RValue val, uint8_t gmlStackType) { stackPushTyped(ctx, val, gmlStackType); }
AOT_TYPED_TEXT void aotx_handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint8_t type1) { handlePush(ctx, instr, extraData, type1); }
AOT_TYPED_TEXT void aotx_handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData) { handlePushBltn(ctx, instr, extraData); }
#if IS_WAD17_OR_HIGHER_ENABLED   // WAD16: these are WAD 17 only; generated WAD 16 code never calls them
void aotx_pushTopLevelArrayRef(VMContext* ctx, RValue* slot, bool forWrite) { pushTopLevelArrayRef(ctx, slot, forWrite); }
AOT_TYPED_TEXT bool aotx_tryPushStructMember(VMContext* ctx, Variable* varDef, int32_t operandCount) { return tryPushStructMember(ctx, varDef, operandCount); }
uint32_t aotx_tryFusedAccessorCall(VMContext* ctx, uint32_t instr, const uint8_t* bc, uint32_t available, uint32_t nextIp) { return tryFusedAccessorCall(ctx, instr, bc, available, nextIp); }
uint32_t aotx_fusedAccessorCall(VMContext* ctx, int32_t argCount, int32_t methodVarID, uint32_t nextIp) { return fusedAccessorCall(ctx, argCount, methodVarID, nextIp); }
void aotx_handleCallV(VMContext* ctx, uint32_t instr) { handleCallV(ctx, instr); }
void aotx_handleBreak(VMContext* ctx, uint32_t instr, uint32_t instrAddr, const uint8_t* extraData) { handleBreak(ctx, instr, instrAddr, extraData); }
#endif
bool aotx_pushSelfBuiltinVar(VMContext* ctx, Variable* varDef) { return pushSelfBuiltinVar(ctx, varDef); }
bool aotx_tryPushArrayElement(VMContext* ctx, Variable* varDef) { return tryPushArrayElement(ctx, varDef); }
bool aotx_tryPopArrayElement(VMContext* ctx, Variable* varDef, uint8_t type2, bool compound) { return tryPopArrayElement(ctx, varDef, type2, compound); }
// tryPopArrayElement with the scope known and the variable's slot found (AOT_CAWRITE).
bool aotx_popArrayElementIntoSlot(VMContext* ctx, RValue* slot, const RValue* indexSlot, RValue* src, uint8_t type2, bool compound) {
    if (indexSlot->type != RVALUE_INT32 || src->type == RVALUE_ARRAY || 0 > indexSlot->int32) return false;
    return popArrayElementIntoSlot(ctx, slot, indexSlot->int32, src, type2, compound);
}
void aotx_handlePop(VMContext* ctx, uint8_t type1, uint8_t type2, uint32_t varRef, uint8_t varType, int32_t instanceType) { handlePop(ctx, type1, type2, varRef, varType, instanceType); }
void aotx_handleAddSlow(VMContext* ctx, uint8_t resultType) { handleAddSlow(ctx, resultType); }
void aotx_handleSubSlow(VMContext* ctx, uint8_t resultType) { handleSubSlow(ctx, resultType); }
void aotx_handleMulSlow(VMContext* ctx, uint8_t resultType) { handleMulSlow(ctx, resultType); }
void aotx_handleDiv(VMContext* ctx, uint32_t instr) { handleDiv(ctx, instr); }
void aotx_handleRem(VMContext* ctx, uint32_t instr) { handleRem(ctx, instr); }
void aotx_handleMod(VMContext* ctx, uint32_t instr) { handleMod(ctx, instr); }
void aotx_handleAnd(VMContext* ctx, uint32_t instr) { handleAnd(ctx, instr); }
void aotx_handleOr(VMContext* ctx, uint32_t instr) { handleOr(ctx, instr); }
void aotx_handleXor(VMContext* ctx, uint32_t instr) { handleXor(ctx, instr); }
void aotx_handleShl(VMContext* ctx, uint32_t instr) { handleShl(ctx, instr); }
void aotx_handleShr(VMContext* ctx, uint32_t instr) { handleShr(ctx, instr); }
void aotx_handleNeg(VMContext* ctx, uint32_t instr) { handleNeg(ctx, instr); }
void aotx_handleNot(VMContext* ctx, uint32_t instr) { handleNot(ctx, instr); }
void aotx_handleConv(VMContext* ctx, uint8_t srcType, uint8_t dstType, uint8_t convKey) { handleConv(ctx, srcType, dstType, convKey); }
void aotx_handleCmp(VMContext* ctx, uint32_t instr) { handleCmp(ctx, instr); }
void aotx_handleDup(VMContext* ctx, uint32_t instr) { handleDup(ctx, instr); }
void aotx_handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData) { handleCall(ctx, instr, extraData); }
void aotx_handlePushEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr) { handlePushEnv(ctx, instr, instrAddr); }
void aotx_handlePopEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr) { handlePopEnv(ctx, instr, instrAddr); }

// ---[ Fallback ]---

// Deopts per generated function, and (with BS_AOT_STATS) per site, the first AOT_DEOPT_SITES of them.
static uint32_t* aotDeoptCounts;
static bool aotStats;
#define AOT_DEOPT_SITES 64
static struct { uint32_t functionIndex, ip, count; } aotDeoptSites[AOT_DEOPT_SITES];
static uint32_t aotDeoptSiteCount;

// Finishes the call in the interpreter from ip; ctx->stack.top must already hold the stack depth there.
NOINLINE RValue aotDeopt(VMContext* ctx, uint32_t functionIndex, uint32_t ip) {
    if (aotDeoptCounts != nullptr) aotDeoptCounts[functionIndex]++;
    if (aotStats) {
        uint32_t i = 0;
        while (aotDeoptSiteCount > i && (aotDeoptSites[i].functionIndex != functionIndex || aotDeoptSites[i].ip != ip)) i++;
        if (i == aotDeoptSiteCount && AOT_DEOPT_SITES > i) {
            aotDeoptSites[i].functionIndex = functionIndex;
            aotDeoptSites[i].ip = ip;
            aotDeoptSites[i].count = 0;
            aotDeoptSiteCount++;
        }
        if (aotDeoptSiteCount > i) aotDeoptSites[i].count++;
    }
    ctx->ip = ip;
    return executeLoop(ctx);
}

#ifdef CTRPORT_AOT_STRESS
uint32_t aotStressCounter, aotStressPeriod = 7;
#endif

// ---[ Math out of line (vm_aot.h: aotFloorOut) ]---

NOINLINE GMLReal aotFloorOut(GMLReal x) { return GMLReal_floor(x); }
NOINLINE GMLReal aotCeilOut(GMLReal x) { return GMLReal_ceil(x); }
NOINLINE GMLReal aotFmodOut(GMLReal x, GMLReal y) { return GMLReal_fmod(x, y); }

// ---[ Variable reads ]---

// PushGlb.v global.<var> when it isn't there: what OP_PUSHGLB falls back to.
NOINLINE void aotPushGlobalSlow(VMContext* ctx, uint32_t varRef) {
    RValue val = resolveVariableRead(ctx, INSTANCE_GLOBAL, varRef);
    stackPushTyped(ctx, val, GML_TYPE_VARIABLE);
}

// Push.v arg.argumentN (BC17 named script arguments), as the OP_PUSH fast path reads them; returns false for anything
// else, which then goes to handlePush.
bool aotPushNamedArg(VMContext* ctx, Variable* varDef) {
    int32_t argIndex = varDef->builtinVarId - BUILTIN_VAR_ARGUMENT0;
    if (argIndex >= 0 && BUILTIN_VAR_ARGUMENT15 - BUILTIN_VAR_ARGUMENT0 >= argIndex) {
        if (ctx->scriptArgs != nullptr && ctx->scriptArgCount > argIndex) {
            stackPushWeakCopy(ctx, &ctx->scriptArgs[argIndex]);
        } else {
            stackPushTyped(ctx, RValue_makeUndefined(), GML_TYPE_VARIABLE);
        }
        return true;
    }
    return false;
}

// ---[ Script calls ]---

// Call.i of a function that may be a script (AOT_CALL_SCRIPT). For a script that is neither a builtin nor forwarded
// (findScriptForward), this is handleCall's script path with popScriptArgs, runScriptFrame and aotDispatch in one
// call and no argument block: the arguments stay in the stack slots the caller pushed them to, are put in argument
// order (the first argument was pushed last) and made independent there, as popScriptArgs makes them on the way into
// its block, and become the callee's ctx->scriptArgs. The callee's frame starts above them, and the unwind at the end
// of the call frees them with anything else the callee left on the stack. The frame is the one runScriptFrame sets
// up, so the callee can hand over to the interpreter at any point as before.
//
// script_execute(<script index>, ...) before GameMaker 2.3 (including UNDERTALE script calls) is called
// the same way when the index is a number naming a script without a native version: what builtin_script_execute and
// VM_callCodeIndex do, with the arguments made independent in place instead of copied into a block.
//
// Everything else goes to handleCall: other builtins, forwards and native versions, the first call of a function
// (which looks for its forward), and calls with the stack more than half full, since arguments left on the stack take
// room the interpreter would have freed.
static uint32_t aotDirectCalls, aotHandedCalls, aotForwardCalls, aotBuiltinCalls;   // for BS_AOT_STATS
static BuiltinFunc aotScriptExecute;               // script_execute when it takes a SCPT index, else nullptr (per context)

// The call itself, once the callee is known: the argCount values on top of the stack are its arguments.
static inline void aotCallOnStack(VMContext* ctx, int32_t codeIndex, int32_t argCount) {
    int32_t base = ctx->stack.top - argCount;
    RValue* args = &ctx->stack.slots[base];
    for (int32_t i = 0, j = argCount - 1; j > i; i++, j--) {
        RValue t = args[i];
        args[i] = args[j];
        args[j] = t;
    }
    repeat(argCount, i) {
        RValue* arg = &args[i];
        // A static string needs no copy (popScriptArgs).
        if (arg->ownsReference || (arg->type == RVALUE_STRING && RValue_isStaticString(arg->string))) continue;
        *arg = RValue_makeIndependent(*arg);
    }
    aotDirectCalls++;
    RValue result = runScriptFrameWith(ctx, codeIndex, argCount > 0 ? args : nullptr, argCount, true);
    stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
}

NOINLINE void aotCallScript(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint32_t funcIndex) {
#ifndef ENABLE_VM_TRACING
    FuncCallCache* cache = &ctx->funcCallCache[funcIndex];
    int32_t argCount = (int32_t) (instr & 0xFFFF);
    if (__builtin_expect(cache->builtin == nullptr && cache->scriptCodeIndex >= 0 && cache->forwardChecked
                         && cache->forward == nullptr && VM_STACK_SIZE / 2 > ctx->stack.top, 1)) {
        aotCallOnStack(ctx, cache->scriptCodeIndex, argCount);
        return;
    }
    if (cache->builtin != nullptr && cache->builtin == (void*) aotScriptExecute && argCount >= 1 && VM_STACK_SIZE / 2 > ctx->stack.top) {
        const RValue* index = &ctx->stack.slots[ctx->stack.top - 1];   // the first argument, pushed last
        if (index->type == RVALUE_REAL || index->type == RVALUE_INT32) {
            int32_t rawArg = RValue_toInt32(*index);
            if (rawArg >= 0 && ctx->dataWin->scpt.count > (uint32_t) rawArg) {
                int32_t codeId = ctx->dataWin->scpt.scripts[rawArg].codeId;
                if (codeId >= 0 && ctx->dataWin->code.count > (uint32_t) codeId) {
                    ctx->stack.top--;   // the index: a plain number, nothing to free
                    struct Instance* savedInstance = ctx->currentInstance;
                    aotCallOnStack(ctx, codeId, argCount - 1);
                    ctx->currentInstance = savedInstance;
                    return;
                }
            }
        }
    }
    if (cache->forward != nullptr) aotForwardCalls++;
#endif
    aotHandedCalls++;
    handleCall(ctx, instr, extraData);
}

// Call.i of a builtin the generator found can't run code (AOT_CALL_BUILTIN): what handleCall does for a cached
// builtin, with the arguments left in the stack slots the caller pushed them to, put in argument order there (the
// first argument was pushed last) instead of being moved into an argument block. The builtin gets them as its args,
// as it would the block, and they are freed the same way after it returns. A builtin that can't run code leaves the
// operand stack alone, so the result goes where the arguments were. Anything else (a function that isn't a builtin
// in this game's VM) goes to handleCall.
NOINLINE void aotCallBuiltin(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint32_t funcIndex) {
#ifndef ENABLE_VM_TRACING
    FuncCallCache* cache = &ctx->funcCallCache[funcIndex];
    if (__builtin_expect(cache->builtin != nullptr, 1)) {
        BuiltinFunc builtin = (BuiltinFunc) cache->builtin;
        int32_t argCount = (int32_t) (instr & 0xFFFF);
        int32_t base = ctx->stack.top - argCount;
        RValue* args = &ctx->stack.slots[base];
        for (int32_t i = 0, j = argCount - 1; j > i; i++, j--) {
            RValue t = args[i];
            args[i] = args[j];
            args[j] = t;
        }
        aotBuiltinCalls++;
#ifdef ENABLE_VM_GML_PROFILER
        Profiler_enterKind(ctx->profiler, ctx->dataWin->func.functions[funcIndex].name, PROFILER_KIND_BUILTIN);
        RValue result = builtin(ctx, argCount > 0 ? args : nullptr, argCount);
        Profiler_exit(ctx->profiler);
#else
        RValue result = builtin(ctx, argCount > 0 ? args : nullptr, argCount);
#endif
        repeat(argCount, i) {
            if (args[i].ownsReference) RValue_free(&args[i]);
        }
        ctx->stack.top = base;
        stackPushTyped(ctx, result, GML_TYPE_VARIABLE);
        return;
    }
#endif
    handleCall(ctx, instr, extraData);
}

// ---[ Variable writes ]---

// Pop.v.v <scope>.<var> when tryFastVarWriteSlot has no slot for it: what OP_POP does then.
NOINLINE void aotPopVarSlow(VMContext* ctx, int32_t instanceType, uint32_t varRef, uint8_t type2) {
    RValue val = stackPop(ctx);
    val = coerceIntStoreToReal(val, type2);
#if IS_WAD17_OR_HIGHER_ENABLED && !defined(ENABLE_VM_TRACING)
    // A builtin-named member of the struct whose constructor or method this is (`x = argument0` in DELTARUNE's
    // Vector2): where resolveVariableWrite's builtin branch ends up for it, without the scope dispatch on the way.
    Instance* self = ctx->currentInstance;
    if (instanceType == INSTANCE_SELF && self != nullptr && self->objectIndex == STRUCT_OBJECT_INDEX && ((varRef >> 24) & 0xF8) == VARTYPE_NORMAL) {
        Variable* varDef = resolveVarDef(ctx, varRef);
        if (varDef->varID == VARIABLE_BUILTIN && VMBuiltins_isInstanceScopedBuiltinVar(varDef->builtinVarId)) {
            structSetDataNamed(ctx, self, varDef->name, val, -1);
            RValue_free(&val);
            return;
        }
    }
#endif
#ifndef ENABLE_VM_TRACING
    // A builtin on an instance's self (`image_index = e`), likewise: resolveVariableWrite takes a plain variable's
    // write with no array index (popArrayAccess pops nothing and gives -1) to VMBuiltins_setVariable on
    // currentInstance, and frees the value, which the setter only reads. (A tracing build logs the write there.)
    if (instanceType == INSTANCE_SELF && ((varRef >> 24) & 0xF8) == VARTYPE_NORMAL) {
        Instance* inst = ctx->currentInstance;
        Variable* varDef = resolveVarDef(ctx, varRef);
        if (varDef->varID == VARIABLE_BUILTIN && (inst == nullptr || inst->objectIndex != STRUCT_OBJECT_INDEX)) {
            VMBuiltins_setVariable(ctx, inst, varDef->builtinVarId, varDef->name, val, -1);
            RValue_free(&val);
            return;
        }
    }
#endif
    resolveVariableWrite(ctx, instanceType, varRef, val);
}

// ---[ Slow paths (AOT_SITE in vm_aot.h) ]---
//
// A site word holds the instruction's address in the code's bytecode (bits 0-23, in words), its size less one word
// (bits 24-25, in words) and the change in stack depth across it (bits 28-31, signed); the stack top before it comes
// separately (top). Each function reads the instruction and its operand where the interpreter reads them and makes the
// call executeLoop makes for it, which is what the generated code spelled out at the site before. The ones that are
// passed the variable (AOT_SITE_V) or the operand (AOT_SITE_POPVAR) read the bytecode only when their fast path can't
// take the instruction.

#define AOT_SITE_ADDR(site) (((site) & 0xFFFFFFu) << 2)
#define AOT_SITE_INSTR(ctx, site) ((ctx)->bytecodeBase + AOT_SITE_ADDR(site))

// AOT_CHECK: 0, or the address of the next instruction when the handler left the stack at another depth than the
// site's top plus its change, or an exception is pending.
static inline uint32_t aotSiteDone(VMContext* ctx, int32_t top, uint32_t site) {
#ifdef ENABLE_WAD17
    bool pending = ctx->exception != nullptr;
#else
    bool pending = false;
#endif
    if (__builtin_expect(ctx->stack.top != top + ((int32_t) site >> 28) || pending, 0)) return AOT_SITE_ADDR(site) + ((((site >> 24) & 3u) + 1) << 2);
    return 0;
}

static inline Variable* aotSiteVar(VMContext* ctx, const uint8_t* bc) {
    return &ctx->dataWin->vari.variables[resolveVarOperand(bc + 4) & 0x07FFFFFF];
}

// Push's handlePush call, for a fast path that couldn't take the instruction.
static NOINLINE void aotSitePushHandler(VMContext* ctx, uint32_t site) {
    const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
    handlePush(ctx, BinaryUtils_readUint32Aligned(bc), bc + 4, GML_TYPE_VARIABLE);
}

// Pop's handlePop call, as executeLoop decodes it.
static inline void aotSiteHandlePop(VMContext* ctx, const uint8_t* bc) {
    uint32_t instr = BinaryUtils_readUint32Aligned(bc);
    uint32_t varRef = resolveVarOperand(bc + 4);
    uint8_t varType = (uint8_t) ((varRef >> 24) & 0xF8);
    int32_t instanceType = instrInstanceType(instr);
    if (varType == VARTYPE_INSTANCE) instanceType += INSTANCE_ID_BASE;
    handlePop(ctx, instrType1(instr), (uint8_t) instrType2(instr), varRef, varType, instanceType);
}

NOINLINE uint32_t aotSitePush(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
    handlePush(ctx, BinaryUtils_readUint32Aligned(bc), bc + 4, GML_TYPE_VARIABLE);
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSitePushGlb(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    aotPushGlobalSlow(ctx, resolveVarOperand(AOT_SITE_INSTR(ctx, site) + 4));
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSitePushBltn(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
    handlePushBltn(ctx, BinaryUtils_readUint32Aligned(bc), bc + 4);
    return aotSiteDone(ctx, top, site);
}

// A builtin variable on self, read by PushBltn or Push.
NOINLINE uint32_t aotSiteSelfBltn(VMContext* ctx, int32_t top, uint32_t site, Variable* var) {
    ctx->stack.top = top;
    if (!pushSelfBuiltinVar(ctx, var)) {
        const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
        uint32_t instr = BinaryUtils_readUint32Aligned(bc);
        if ((instr >> 24) == OP_PUSHBLTN) handlePushBltn(ctx, instr, bc + 4);
        else handlePush(ctx, instr, bc + 4, GML_TYPE_VARIABLE);
    }
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSiteNamedArg(VMContext* ctx, int32_t top, uint32_t site, Variable* var) {
    ctx->stack.top = top;
    if (!aotPushNamedArg(ctx, var)) aotSitePushHandler(ctx, site);
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSitePushArray(VMContext* ctx, int32_t top, uint32_t site, Variable* var) {
    ctx->stack.top = top;
    if (!tryPushArrayElement(ctx, var)) aotSitePushHandler(ctx, site);
    return aotSiteDone(ctx, top, site);
}

// Pop.v.v: the word's bits 24-25 hold the instance type (0 self, 1 other, 2 global, 3 the instruction's) and bits
// 28-31 type2; the instruction is 8 bytes and leaves the stack one shorter.
NOINLINE uint32_t aotSitePopVar(VMContext* ctx, int32_t top, uint32_t site, uint32_t varRef) {
    ctx->stack.top = top;
    uint32_t code = (site >> 24) & 3u;
    int32_t instanceType = code == 0 ? INSTANCE_SELF : code == 1 ? INSTANCE_OTHER : code == 2 ? INSTANCE_GLOBAL
                                                                                                : instrInstanceType(BinaryUtils_readUint32Aligned(AOT_SITE_INSTR(ctx, site)));
    aotPopVarSlow(ctx, instanceType, varRef, (uint8_t) (site >> 28));
#ifdef ENABLE_WAD17
    bool pending = ctx->exception != nullptr;
#else
    bool pending = false;
#endif
    if (__builtin_expect(ctx->stack.top != top - 1 || pending, 0)) return AOT_SITE_ADDR(site) + 8;
    return 0;
}

NOINLINE uint32_t aotSitePop(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
#if IS_WAD17_OR_HIGHER_ENABLED && !defined(ENABLE_VM_TRACING)
    // A struct member write (`pos.x = e`), as executeLoop's OP_POP takes it.
    uint32_t instr = BinaryUtils_readUint32Aligned(bc);
    if (((resolveVarOperand(bc + 4) >> 24) & 0xF8) == VARTYPE_STACKTOP && IS_WAD17_OR_HIGHER(ctx) &&
        tryPopStructMember(ctx, aotSiteVar(ctx, bc), instrType1(instr), (uint8_t) instrType2(instr), instrInstanceType(instr)))
        return aotSiteDone(ctx, top, site);
#endif
    aotSiteHandlePop(ctx, bc);
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSitePopArray(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
    uint32_t instr = BinaryUtils_readUint32Aligned(bc);
    if (!tryPopArrayElement(ctx, aotSiteVar(ctx, bc), instrType2(instr), instrType1(instr) != GML_TYPE_VARIABLE)) aotSiteHandlePop(ctx, bc);
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSiteDup(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    handleDup(ctx, BinaryUtils_readUint32Aligned(AOT_SITE_INSTR(ctx, site)));
    return aotSiteDone(ctx, top, site);
}

#if IS_WAD17_OR_HIGHER_ENABLED
// Push of a struct member: Push.v with a stacktop instance (one operand) or Push.v [stacktop] (two).
NOINLINE uint32_t aotSiteMember1(VMContext* ctx, int32_t top, uint32_t site, Variable* var) {
    ctx->stack.top = top;
    if (!tryPushStructMember(ctx, var, 1)) aotSitePushHandler(ctx, site);
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSiteMember2(VMContext* ctx, int32_t top, uint32_t site, Variable* var) {
    ctx->stack.top = top;
    if (!tryPushStructMember(ctx, var, 2)) aotSitePushHandler(ctx, site);
    return aotSiteDone(ctx, top, site);
}

NOINLINE uint32_t aotSiteBreak(VMContext* ctx, int32_t top, uint32_t site) {
    ctx->stack.top = top;
    const uint8_t* bc = AOT_SITE_INSTR(ctx, site);
    handleBreak(ctx, BinaryUtils_readUint32Aligned(bc), (uint32_t) (bc - ctx->bytecodeBase), bc + 4);
    return aotSiteDone(ctx, top, site);
}

// The two steps of a[i][j] = v, Push.v [arraypopaf] and then Break popaf, take their common case here and hand the
// rest to aotSitePush and aotSiteBreak. Generated code calls them as it calls those, with the same arguments, so they
// add nothing to it. Inline, with the slot caches they need there, one write's two steps took 450-530 bytes of ARM
// code (95-180 with the fast paths out of line), and on the Old 3DS the size of the hot code costs more than calls do
// (docs/aot.md, "Two-step array writes through their own sites").

// Whether cowForkSlotForModificationIfNeeded leaves the array in slot as it is (or takes it over without a copy): it
// only stamps the owner then.
static inline bool aotWritableInPlace(const GMLArray* arr, const RValue* slot, void* owner) {
    return arr->owner == owner || (arr->refCount == 1 && slot->ownsReference);
}

// Push.v [arraypopaf] of a plain variable in a self, other or global scope, with the scope and i on the stack, when the
// variable holds an array that holds an array at i already and neither needs a copy: what handlePush does then, a weak
// ref to the sub-array over the scope, cowForkSlotForModificationIfNeeded only stamping the owner on both and
// GMLArray_growTo changing nothing.
NOINLINE uint32_t aotSitePopAF(VMContext* ctx, int32_t top, uint32_t site) {
    RValue* s = &ctx->stack.slots[top - 2];
    const Variable* var = aotSiteVar(ctx, AOT_SITE_INSTR(ctx, site));
    if (s[0].type == RVALUE_INT32 && s[1].type == RVALUE_INT32 && var->builtinVarId < 0 && var->varID >= 0) {
        RValue* slot = vmScopeVarSlot(ctx, s[0].int32, var->varID);
        if (slot != nullptr && slot->type == RVALUE_ARRAY) {
            GMLArray* arr = slot->array;
            RValue* sub = (RValue*) aotModernCell(arr, s[1].int32);
            void* owner = ctx->currentArrayOwner;
            if (sub != nullptr && sub->type == RVALUE_ARRAY && sub->array != nullptr && aotWritableInPlace(arr, slot, owner)
                && aotWritableInPlace(sub->array, sub, owner)) {
                arr->owner = owner;
                sub->array->owner = owner;
                s[0] = (RValue) { .array = sub->array, .type = RVALUE_ARRAY, .ownsReference = false, .gmlStackType = GML_TYPE_VARIABLE };
                ctx->stack.top = top - 1;
                return 0;
            }
        }
    }
    return aotSitePush(ctx, top, site);
}

// Break popaf, with the value, the weak array ref and the index on the stack, for an int32 index in range of an array
// that may be written in place: what handleBreakPopAF does then, GMLArray_set (whose growTo does nothing) and the free
// of the value; a plain value over a cell that owns nothing is a copy. Where the interpreter would abort, so does
// aotSiteBreak.
NOINLINE uint32_t aotSiteBreakPopAF(VMContext* ctx, int32_t top, uint32_t site) {
    RValue* s = &ctx->stack.slots[top - 3];
    if (s[1].type == RVALUE_ARRAY && !s[1].ownsReference && s[2].type == RVALUE_INT32) {
        GMLArray* arr = s[1].array;
        RValue* cell = (RValue*) aotModernCell(arr, s[2].int32);
        if (cell != nullptr && (arr->refCount == 1 || arr->owner == ctx->currentArrayOwner)) {
            if (cell->ownsReference || ((AOT_REF_TYPES >> (s[0].type & 31u)) & 1u)) {
                RValue_copyIntoSlot(cell, s[0]);
                RValue_free(&s[0]);
            } else {
                *cell = s[0];
            }
            ctx->stack.top = top - 3;
            return 0;
        }
    }
    return aotSiteBreak(ctx, top, site);
}
#endif

// ---[ Locals ]---

uint32_t aotEpoch = 1;

NOINLINE bool aotResolveLocals(VMContext* ctx, const uint32_t* varIndices, uint32_t* slots, AotLocalCache* cache, uint32_t count) {
    Variable* vars = ctx->dataWin->vari.variables;
    uint32_t maxSlot = 0;
    for (uint32_t i = 0; count > i; i++) {
        slots[i] = resolveLocalSlot(ctx, vars[varIndices[i]].varID);
        if (slots[i] > maxSlot) maxSlot = slots[i];
    }
    cache->epoch = aotEpoch;
    cache->map = ctx->currentCodeLocalsSlotMap;
    cache->maxSlot = maxSlot;
    // WAD16: a local's slot is its varID, and the frame doesn't grow to cover it (the interpreter stops with an error
    // when such a local is used). Generated code would read past the frame, so the whole call runs in the interpreter.
    return ctx->localVarCount > maxSlot;
}

// ---[ Binding ]---

// The bytes of the code entry's blob after patching: what aotBind compares against.
static uint64_t aotDigest(const uint8_t* bytes, uint32_t size) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; size > i; i++) {
        h ^= bytes[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// The generated code's helpers for its rare paths (declared in vm_aot.h): one copy for all of it.

bool aotGrew;

NOINLINE RValue* aotSlotWrite(VMContext* ctx, int32_t instanceType, int32_t varID) {
    Instance* inst = vmScopeInstance(ctx, instanceType);
    if (inst == nullptr) return nullptr;
    IntRValueEntry* entries = inst->selfVars.entries;
    RValue* slot = IntRValueHashMap_getOrInsertUndefinedFast(&inst->selfVars, varID);
    aotGrew = inst->selfVars.entries != entries;
    return slot;
}

NOINLINE void aotFreeCopy(const RValue* v) { AOTC(121);
    RValue c = *v;
    RValue_free(&c);
}

NOINLINE int aotCmpStrings(RValue* a, RValue* b, uint8_t kind) {
    return vmCmpStrings(a, b, kind);
}

#define AOT_UNBOUND ((AotFn) (uintptr_t) 1)

// The generated functions in use: the build's own (aot_table.c), or with CTRPORT_AOT_MODULES the table of the module
// the platform loaded for this data.win (VM_aotUseTable), else none.
static const AotTable aotNoTable = { nullptr, 0, nullptr, 0, nullptr, "aot" };
#ifdef CTRPORT_AOT_MODULES
static const AotTable* aotT = &aotNoTable;
#else
static const AotTable* aotT = &aotTable;
#endif

static VMContext* aotBoundCtx;
static AotFn* aotBound;      // per code index: AOT_UNBOUND, nullptr (interpret) or the generated function
static uint32_t aotBoundCount;
static int8_t aotEnabled = -1;
static uint32_t aotBindCount;

// Whether entry was generated from exactly this code (several entries may share a name: one per chapter). With
// loadBytes false (the start-up report) the bytecode digest is only checked when the bytecode is in memory, so the
// check reads nothing back from the bytecode cache; the variable and function references are always checked.
static bool aotMatches(VMContext* ctx, CodeEntry* code, const AotEntry* entry, bool loadBytes) {
    if (entry->offset != code->offset || entry->blobLength != code->length || entry->end > code->length) return false;
    const uint8_t* bytes = loadBytes ? DataWin_codeBytes(ctx->dataWin, code) : code->bytecode;
    if (bytes == nullptr ? loadBytes : aotDigest(bytes + entry->offset, entry->end - entry->offset) != entry->digest) return false;
    Vari* vari = &ctx->dataWin->vari;
    for (uint32_t i = 0; entry->varCount > i; i++) {
        const AotVarRef* ref = &entry->vars[i];
        if (ref->index >= vari->variableCount) return false;
        Variable* v = &vari->variables[ref->index];
        if (v->varID != ref->varID || v->instanceType != ref->instanceType || v->name == nullptr || strcmp(v->name, ref->name) != 0) return false;
    }
    Func* func = &ctx->dataWin->func;
    for (uint32_t i = 0; entry->funcCount > i; i++) {
        const AotFuncRef* ref = &entry->funcs[i];
        if (ref->index >= func->functionCount || func->functions[ref->index].name == nullptr || strcmp(func->functions[ref->index].name, ref->name) != 0) return false;
    }
    return true;
}

// lbind (aotprof): whether entry cannot be code's, tested by its offset and blob length (aotMatches's own first test)
// before its name, so aotBind's scan makes a strcmp only for the entries that could match (a first run of a code entry
// with no generated function made one for every entry: 376 on UNDERTALE Old). Late code.
// On the 3DS it is platform/3ds/source/z_lbind.c's, a unit of its own, so this file's inlining stays as it was.
#if defined(__3DS__)
bool aotNameDiffers(const AotEntry* entry, const CodeEntry* code);
#else
static bool aotNameDiffers(const AotEntry* entry, const CodeEntry* code) {
    return entry->offset != code->offset || entry->blobLength != code->length || strcmp(entry->codeName, code->name) != 0;
}
#endif
// With aotBind's strcmp a call to aotNameDiffers, aotDispatch's clone (where aotBind is inlined) is 4 B shorter
// (0x6c0 -> 0x6bc on all four 3DS targets, GCC 16.1.0). Zero bytes after its code, in a later subsection of its own
// section and outside its symbol, keep its slot at main's extent; nothing branches there
// (tools/analysis/check_lbind_slot.py). The pad is made inside aotDispatch's once-per-run set-up, not by a top-level
// asm: those are output before every function, which would make the clone's section vm.o's first; set-up is the one
// place in the clone where the statement leaves its code at 0x6bc (in aotBind it made it 0x6c0 or more).
#define LBIND_PAD "4"
#line 588 // lbind: main's numbering (the __LINE__ trap)
static AotFn aotBind(VMContext* ctx, int32_t codeIndex) {
    CodeEntry* code = &ctx->dataWin->code.entries[codeIndex];
    if (!IS_WAD16_OR_HIGHER(ctx) || code->name == nullptr) return nullptr;   // WAD16: WAD 16 and 17 bind
    for (uint32_t e = 0; aotT->entryCount > e; e++) {
        const AotEntry* entry = &aotT->entries[e];
        // A null function was left out of its 3DS module to fit the region (AOT_FN_DECL).
        if (entry->fn == nullptr || aotNameDiffers(entry, code) || !aotMatches(ctx, code, entry, true)) continue;
        aotBindCount++;
        return entry->fn;
    }
    return nullptr;
}

static void aotForget(VMContext* ctx) {
    if (aotBoundCtx != ctx) return;
    if (aotStats && aotDeoptCounts != nullptr) {
        fprintf(stderr, "AOT: %u of %u generated functions bound\n", aotBindCount, (unsigned) aotT->entryCount);
        fprintf(stderr, "AOT: %u builtin calls direct\n", aotBuiltinCalls);
        fprintf(stderr, "AOT: %u script calls direct, %u through handleCall (%u to forwards)\n", aotDirectCalls, aotHandedCalls,
                aotForwardCalls);
        for (uint32_t e = 0; aotT->entryCount > e; e++) {
            if (aotDeoptCounts[e] != 0) fprintf(stderr, "AOT: %u deopts in %s\n", aotDeoptCounts[e], aotT->entries[e].codeName);
        }
        for (uint32_t i = 0; aotDeoptSiteCount > i; i++) {
            fprintf(stderr, "AOT: %u deopts at %s+0x%X\n", aotDeoptSites[i].count,
                    aotT->entries[aotDeoptSites[i].functionIndex].codeName, (unsigned) aotDeoptSites[i].ip);
        }
    }
    aotDeoptSiteCount = 0;
    free(aotBound);
    free(aotDeoptCounts);
    aotBound = nullptr;
    aotDeoptCounts = nullptr;
    aotBoundCount = 0;
    aotBoundCtx = nullptr;
    aotBindCount = 0;
    aotDirectCalls = 0;
    aotBuiltinCalls = 0;
    aotHandedCalls = 0;
    aotForwardCalls = 0;
    aotEpoch++;   // the cached local slots belonged to this context's slot maps
}

// Another table from the next context on (nullptr: none). Whatever is bound to the current one is dropped first, as
// it points into the old table's code.
void VM_aotUseTable(const AotTable* table) { VM_TYPED_INVALIDATE();
    if (aotBoundCtx != nullptr) aotForget(aotBoundCtx);
    aotT = table != nullptr ? table : &aotNoTable;
}

// Logs, when a data.win starts, how much of the generated code it can use: which game the build generated functions
// for matches this data.win (by its counts of code entries, variables and functions), and how many of those
// functions match its code. Binding itself stays lazy (aotBind, the first time each entry runs); this only checks.
static void aotReport(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    const char* tag = aotT->buildTag;
    if (!aotEnabled) {
        logInfo("AOT: %s, off (BS_AOT=0): all %u generated functions unused\n", tag, (unsigned) aotT->entryCount);
        return;
    }
    if (aotT->partCount == 0) {
        logInfo("AOT: no generated functions loaded for this data.win, so all of it runs in the interpreter\n");
        return;
    }
    int32_t part = -1;
    for (uint32_t p = 0; aotT->partCount > p; p++) {
        const AotPart* ap = &aotT->parts[p];
        if (ap->codeCount == dw->code.count && ap->variableCount == dw->vari.variableCount && ap->functionCount == dw->func.functionCount) {
            part = (int32_t) p;
            break;
        }
    }
    if (0 > part) {
        logWarn("AOT: %s: none of the %u generated functions is for this data.win (%u code entries, %u variables, %u "
                "FUNC entries: another version of the game?), so all of it runs in the interpreter\n", tag,
                (unsigned) aotT->entryCount, dw->code.count, dw->vari.variableCount, dw->func.functionCount);
        return;
    }
    uint32_t total = 0, active = 0, differs = 0, missing = 0, leftOut = 0;
    for (uint32_t e = 0; aotT->entryCount > e; e++) {
        if (aotT->entryPart[e] != (uint32_t) part) continue;
        const AotEntry* entry = &aotT->entries[e];
        if (entry->fn == nullptr) {
            leftOut++;
            continue;
        }
        total++;
        ptrdiff_t at = shgeti(ctx->codeIndexByName, (char*) entry->codeName);
        int32_t codeIndex = at >= 0 ? ctx->codeIndexByName[at].value : -1;
        if (0 > codeIndex || (uint32_t) codeIndex >= dw->code.count || dw->code.entries[codeIndex].name == nullptr
            || strcmp(dw->code.entries[codeIndex].name, entry->codeName) != 0) {
            missing++;
        } else if (aotMatches(ctx, &dw->code.entries[codeIndex], entry, false)) {
            active++;
        } else {
            differs++;
        }
    }
    if (leftOut != 0) {
        logInfo("AOT: %s: %u functions for %s were left out of the build to fit the space for generated code\n", tag,
                leftOut, aotT->parts[part].name);
    }
    if (total == 0) {
        logInfo("AOT: %s: no generated functions for %s\n", tag, aotT->parts[part].name);
    } else if (active == total) {
        logInfo("AOT: %s: %u of %u functions for %s active\n", tag, active, total, aotT->parts[part].name);
    } else {
        logWarn("AOT: %s: %u of %u functions for %s active (%u rejected: code differs, %u missing)\n", tag, active,
                total, aotT->parts[part].name, differs, missing);
    }
}

// Runs the code entry set up in ctx (ip at its start) to completion: generated code when there is a matching
// function, else executeLoop.
static RValue aotDispatch(VMContext* ctx) {
    int32_t codeIndex = ctx->currentCodeIndex;
    if (__builtin_expect(aotBoundCtx != ctx, 0)) {
        if (aotEnabled < 0) {
            // BS_AOT=0 runs everything in the interpreter (the forced-fallback test mode).
            const char* env = getenv("BS_AOT");
            aotEnabled = (env != nullptr && env[0] == '0') ? 0 : 1;
            const char* stats = getenv("BS_AOT_STATS");
            aotStats = stats != nullptr && stats[0] != '\0' && stats[0] != '0';
#ifdef CTRPORT_AOT_STRESS
            const char* period = getenv("BS_AOT_STRESS_PERIOD");
            if (period != nullptr) aotStressPeriod = (uint32_t) atoi(period);
#endif
        }
        if (aotBoundCtx != nullptr) aotForget(aotBoundCtx);
        aotBoundCount = ctx->dataWin->code.count;
        aotBound = (AotFn*) safeMalloc(sizeof(AotFn) * (aotBoundCount > 0 ? aotBoundCount : 1));
        for (uint32_t i = 0; aotBoundCount > i; i++) aotBound[i] = aotEnabled ? AOT_UNBOUND : nullptr;
        aotDeoptCounts = (uint32_t*) safeCalloc(aotT->entryCount > 0 ? aotT->entryCount : 1, sizeof(uint32_t));
        aotScriptExecute = DataWin_isVersionAtLeast(ctx->dataWin, 2, 3, 0, 0) ? nullptr : VM_findBuiltin(ctx, "script_execute");
        aotBoundCtx = ctx;
        aotReport(ctx);
#if defined(__3DS__) && defined(NDEBUG)
        __asm__(".pushsection .text.aotDispatch.constprop.0, 1\n.space " LBIND_PAD "\n.popsection"); // lbind's retained tail
#endif
#line 724 // lbind: main's numbering (the __LINE__ trap)
    }
    if (0 > codeIndex || (uint32_t) codeIndex >= aotBoundCount) return executeLoop(ctx);
    AotFn fn = aotBound[codeIndex];
    if (__builtin_expect(fn == AOT_UNBOUND, 0)) {
        fn = aotBind(ctx, codeIndex);
        aotBound[codeIndex] = fn;
    }
    if (fn == nullptr || ctx->ip != ctx->dataWin->code.entries[codeIndex].offset) return executeLoop(ctx);
    return fn(ctx);
}

#if defined(__3DS__) && defined(ENABLE_WAD16)
#include "vm_aot_lava.inc"
#endif
