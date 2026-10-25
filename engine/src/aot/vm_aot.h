#ifndef _BS_VM_AOT_H_
#define _BS_VM_AOT_H_

// ===[ Ahead-of-time compiled GML: what the generated code sees ]===
//
// tools/aot/gmlaot.py translates code entries of the player's own data.win into C files (aot_gen_<n>.c and
// aot_table.c) in the build directory; never in the repository, as they are derived from the game. Each of those files
// includes this header and nothing else. It holds the macros the generated code is written in, the interpreter's
// inline fast paths they use (vm_fastpath.h, the same functions executeLoop calls), and the declarations of the
// runtime half in aot/vm_aot_bridge.inc, which is compiled into vm.c and gives the generated code the interpreter's
// handlers (the aotx_ functions) and the fallback to executeLoop.
//
// Splitting the generated code over several files keeps every translation unit small: compiled inside vm.c, a set of
// fifty large functions took most of ten minutes and 2.6 GB of compiler memory, and nothing could build in parallel.

#if !defined(__GNUC__)
#error "CTRPORT_AOT needs GCC or Clang (statement expressions and asm barriers)"
#endif

#include <stdint.h>
#include <string.h>

#include "vm.h"
#include "vm_aot_hooks.h"
#include "instance.h"
#include "binary_utils.h"
#include "real_type.h"
#include "utils.h"
#include "wad_versions.h"
#include "vm_fastpath.h"

#define AOT_INLINE static inline __attribute__((always_inline))

#if defined(__arm__) && defined(__3DS__)
#define AOT_TYPED_TEXT __attribute__((noinline, section(".text.late.typed3")))
#define AOT_TYPED_CONST __attribute__((section(".rodata.late.typed3")))
#define AOT_TYPED_DATA __attribute__((section(".data.late.typed3")))
#define AOT_TYPED_BSS __attribute__((section(".bss.late.typed3")))
#else
#define AOT_TYPED_TEXT NOINLINE
#define AOT_TYPED_CONST
#define AOT_TYPED_DATA
#define AOT_TYPED_BSS
#endif

// typedaot prototype only (CTRPORT_TYPED_CENSUS): event counters for the CPU census.
// The generated files of a census build are post-processed to call AOTC(n) before each macro statement.
#ifdef CTRPORT_TYPED_CENSUS
#define AOTC_COUNT 128
extern uint32_t aotCensus[AOTC_COUNT];
#define AOTC(n) (aotCensus[n]++)
#else
#define AOTC(n) ((void) 0)
#endif

// The out-of-line helpers of the fast paths (aotPushVarFast and the rest below). By default each compile unit has its
// own copy, so that GCC sees which registers they use and keeps the caller's values in the others across the call.
// With gmlaot.py --shared-helpers the generated files define AOT_SHARED_HELPERS and the helpers are weak: the link
// keeps the first copy (aot_table.c's, in a 3DS module) and --gc-sections drops the others. That saves 40-45 KB of
// helper copies a module, but every call takes the full AAPCS register clobber, since GCC can't see into a function
// the link may replace, and the callers grow by about as much (Function order in the modules).
#ifdef AOT_SHARED_HELPERS
#define AOT_HELPER __attribute__((weak)) NOINLINE
#else
#define AOT_HELPER static NOINLINE __attribute__((unused))
#endif

typedef RValue (*AotFn)(VMContext* ctx);

typedef struct {
    uint32_t index;          // index into dataWin->vari.variables, as patchReferenceOperands writes it into the bytecode
    int32_t varID;
    int32_t instanceType;
    const char* name;
} AotVarRef;

typedef struct {
    uint32_t index;          // index into dataWin->func.functions
    const char* name;
} AotFuncRef;

typedef struct {
    const char* codeName;
    uint32_t offset;         // CodeEntry.offset
    uint32_t blobLength;     // CodeEntry.length
    uint32_t end;            // the digest covers bytes [offset, end) of the code's blob
    uint64_t digest;         // FNV-1a 64 of those bytes, operands patched
    AotFn fn;
    const AotVarRef* vars;
    uint32_t varCount;
    const AotFuncRef* funcs;
    uint32_t funcCount;
} AotEntry;

// Typed-only dependencies. These are separate from AotEntry/AotTable: chapter modules keep their table ABI and
// generated code without typed regions keeps exactly the same initializers. Each caller lists the complete
// transitive set of scripts it inlines, with code and reference definitions pinned as for an ordinary binding.
typedef struct {
    uint32_t codeIndex;
    uint32_t wadVersion;
    AotEntry entry;
} AotInlineBinding;

typedef struct {
    uint32_t epoch;
    bool valid;
} AotInlineCache;

// Locals: see AOT_LOCALS.
typedef struct {
    uint32_t epoch;
    const IntIntHashMap* map;
    uint32_t maxSlot;
} AotLocalCache;

// ---[ aot_table.c (generated) ]---

// How aot_table.c declares the generated functions. In a 3DS AOT module (CTRPORT_AOT_MODULES) they are weak
// references: a module that would overflow its region is linked without its last (lowest-ranked) files, which leaves
// those functions null, and a null function is never bound, so the interpreter runs that code.
#ifdef CTRPORT_AOT_MODULES
#define AOT_FN_DECL __attribute__((weak))
#else
#define AOT_FN_DECL
#endif

// The games (data.win files) the functions were generated from, for the start-up report: a data.win is recognised by
// its counts of code entries, variables and functions.
typedef struct {
    const char* name;        // the data.win's folder (chapter2_windows, ...)
    uint32_t codeCount;
    uint32_t variableCount;
    uint32_t functionCount;
} AotPart;

typedef struct AotTable {
    const AotEntry* entries;
    uint32_t entryCount;
    const AotPart* parts;
    uint32_t partCount;
    const uint8_t* entryPart;   // per entry: its index in parts
    const char* buildTag;    // "aot4" for a 4 MB budget, ...: added to the build ID in the log
} AotTable;

// The table of the code compiled into the build. A build that loads its generated code as modules at run time (the
// 3DS layer's per-chapter modules, CTRPORT_AOT_MODULES) has none of its own: each module holds a table, which the
// platform hands to VM_aotUseTable.
extern const AotTable aotTable;

// ---[ The runtime, in aot/vm_aot_bridge.inc ]---

RValue aotDeopt(VMContext* ctx, uint32_t functionIndex, uint32_t ip);
void aotPushGlobalSlow(VMContext* ctx, uint32_t varRef);
bool aotPushNamedArg(VMContext* ctx, Variable* varDef);
void aotCallScript(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint32_t funcIndex);
void aotCallBuiltin(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint32_t funcIndex);
void aotPopVarSlow(VMContext* ctx, int32_t instanceType, uint32_t varRef, uint8_t type2);
bool aotResolveLocals(VMContext* ctx, const uint32_t* varIndices, uint32_t* slots, AotLocalCache* cache, uint32_t count);
extern uint32_t aotEpoch;
void aotResolveInlineBindings(VMContext* ctx, AotInlineCache* cache, const AotInlineBinding* bindings, uint32_t count);

// The slow paths (AOT_SITE): each sets the stack top to the site's (top), runs what the interpreter runs for the
// instruction at the site's address, and returns 0, or the address to carry on from when the stack isn't at the
// depth after it or an exception is pending. Those whose fast path needs only the variable get it (var) from the
// site, and read the bytecode only when that path can't take the instruction.
uint32_t aotSitePush(VMContext* ctx, int32_t top, uint32_t site);         // handlePush
uint32_t aotSitePushGlb(VMContext* ctx, int32_t top, uint32_t site);      // aotPushGlobalSlow
uint32_t aotSitePushBltn(VMContext* ctx, int32_t top, uint32_t site);     // handlePushBltn
uint32_t aotSiteSelfBltn(VMContext* ctx, int32_t top, uint32_t site, Variable* var);   // pushSelfBuiltinVar, else the push's handler
uint32_t aotSiteNamedArg(VMContext* ctx, int32_t top, uint32_t site, Variable* var);   // aotPushNamedArg, else handlePush
uint32_t aotSitePushArray(VMContext* ctx, int32_t top, uint32_t site, Variable* var);  // tryPushArrayElement, else handlePush
uint32_t aotSitePopVar(VMContext* ctx, int32_t top, uint32_t site, uint32_t varRef);   // aotPopVarSlow (AOT_SITE_POPVAR)
uint32_t aotSitePop(VMContext* ctx, int32_t top, uint32_t site);          // handlePop
uint32_t aotSitePopArray(VMContext* ctx, int32_t top, uint32_t site);     // tryPopArrayElement, else handlePop
uint32_t aotSiteDup(VMContext* ctx, int32_t top, uint32_t site);          // handleDup
#if IS_WAD17_OR_HIGHER_ENABLED   // WAD16: WAD 17 only
uint32_t aotSiteMember1(VMContext* ctx, int32_t top, uint32_t site, Variable* var);    // tryPushStructMember (1 operand), else handlePush
uint32_t aotSiteMember2(VMContext* ctx, int32_t top, uint32_t site, Variable* var);    // the same, 2 operands
uint32_t aotSiteBreak(VMContext* ctx, int32_t top, uint32_t site);        // handleBreak
uint32_t aotSitePopAF(VMContext* ctx, int32_t top, uint32_t site);        // a[i][j] = v's first step, else aotSitePush
uint32_t aotSiteBreakPopAF(VMContext* ctx, int32_t top, uint32_t site);   // its Break popaf, else aotSiteBreak
#endif
#ifdef CTRPORT_AOT_STRESS
extern uint32_t aotStressCounter, aotStressPeriod;
#endif

// The interpreter's own functions (static in vm.c), each passed straight through.
RValue aotx_executeLoop(VMContext* ctx);
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx);
void aotx_stackPushTyped(VMContext* ctx, RValue val, uint8_t gmlStackType);
void aotx_handlePush(VMContext* ctx, uint32_t instr, const uint8_t* extraData, uint8_t type1);
void aotx_handlePushBltn(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
#if IS_WAD17_OR_HIGHER_ENABLED   // WAD16: WAD 17 only, never called by generated WAD 16 code
void aotx_pushTopLevelArrayRef(VMContext* ctx, RValue* slot, bool forWrite);
bool aotx_tryPushStructMember(VMContext* ctx, Variable* varDef, int32_t operandCount);
uint32_t aotx_tryFusedAccessorCall(VMContext* ctx, uint32_t instr, const uint8_t* bc, uint32_t available, uint32_t nextIp);
uint32_t aotx_fusedAccessorCall(VMContext* ctx, int32_t argCount, int32_t methodVarID, uint32_t nextIp);
void aotx_handleCallV(VMContext* ctx, uint32_t instr);
void aotx_handleBreak(VMContext* ctx, uint32_t instr, uint32_t instrAddr, const uint8_t* extraData);
#endif
bool aotx_pushSelfBuiltinVar(VMContext* ctx, Variable* varDef);
bool aotx_tryPushArrayElement(VMContext* ctx, Variable* varDef);
bool aotx_tryPopArrayElement(VMContext* ctx, Variable* varDef, uint8_t type2, bool compound);
bool aotx_popArrayElementIntoSlot(VMContext* ctx, RValue* slot, const RValue* indexSlot, RValue* src, uint8_t type2, bool compound);
void aotx_handlePop(VMContext* ctx, uint8_t type1, uint8_t type2, uint32_t varRef, uint8_t varType, int32_t instanceType);
void aotx_handleAddSlow(VMContext* ctx, uint8_t resultType);
void aotx_handleSubSlow(VMContext* ctx, uint8_t resultType);
void aotx_handleMulSlow(VMContext* ctx, uint8_t resultType);
void aotx_handleDiv(VMContext* ctx, uint32_t instr);
void aotx_handleRem(VMContext* ctx, uint32_t instr);
void aotx_handleMod(VMContext* ctx, uint32_t instr);
void aotx_handleAnd(VMContext* ctx, uint32_t instr);
void aotx_handleOr(VMContext* ctx, uint32_t instr);
void aotx_handleXor(VMContext* ctx, uint32_t instr);
void aotx_handleShl(VMContext* ctx, uint32_t instr);
void aotx_handleShr(VMContext* ctx, uint32_t instr);
void aotx_handleNeg(VMContext* ctx, uint32_t instr);
void aotx_handleNot(VMContext* ctx, uint32_t instr);
void aotx_handleConv(VMContext* ctx, uint8_t srcType, uint8_t dstType, uint8_t convKey);
void aotx_handleCmp(VMContext* ctx, uint32_t instr);
void aotx_handleDup(VMContext* ctx, uint32_t instr);
void aotx_handleCall(VMContext* ctx, uint32_t instr, const uint8_t* extraData);
void aotx_handlePushEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr);
void aotx_handlePopEnv(VMContext* ctx, uint32_t instr, uint32_t instrAddr);

// ---[ Frame, deopt and checks ]---

// The generated code addresses the stack frame as S[depth] with S = &slots[B], B being ctx->stack.top on entry.
#define AOT_SYNC(d) (ctx->stack.top = B + (d))

#ifdef ENABLE_WAD17
#define AOT_EXCEPTION_PENDING() (ctx->exception != nullptr)
#else
#define AOT_EXCEPTION_PENDING() 0
#endif

// Every way out to the interpreter goes through one call at the end of the generated function (aot_deopt_), with the
// address to carry on from in aotDeoptIp_: one call site per function instead of one per instruction.
#define AOT_DEOPT_AT(ip) do { aotDeoptIp_ = (ip); goto aot_deopt_; } while (0)

// After a handler that may leave the stack at another depth than expected (or set an exception): carry on in the
// interpreter from the next instruction when it did.
#define AOT_CHECK(d, next) \
    do { if (__builtin_expect(ctx->stack.top != B + (d) || AOT_EXCEPTION_PENDING(), 0)) AOT_DEOPT_AT(next); } while (0)

#define AOT_DEOPT(d, addr) do { AOT_SYNC(d); AOT_DEOPT_AT(addr); } while (0)

// Slow paths. Many instructions hand their work to an interpreter handler at some point: a variable that isn't
// there, a builtin variable, an array element the fast path can't take, a Dup of values that own something. Each such
// call syncs the stack, calls the handler and checks the depth after it (AOT_SYNC; handler; AOT_CHECK). Spelled out at
// every site, with its operands built from constants, that was some 95 bytes of ARM code, about a twelfth of the
// generated code. Instead a site calls one of the aotSite functions in aot/vm_aot_bridge.inc with the stack top before
// the instruction (B + d) and a site word: the instruction's address and size and the change in depth across it. The
// function reads the instruction and its operand from the bytecode, as the interpreter does, does the same sync, call
// and check, and returns 0, or the address to carry on from in the interpreter. The generator spells a site out as
// before when the word can't hold it (an address past 64 MB, a depth change outside -8..7).
//
// The bytecode is otherwise left alone by generated code, and reading it at a hot site costs a data cache line (the
// o3DS has 16 KB of data cache and no L2). So a site whose fast path needs only the variable passes it (AOT_SITE_V),
// and a Pop.v.v to a variable without a cached slot (a builtin, say) passes its operand and holds its type2 and its
// instance type, for self, other and global, in the word's size and depth change, which are always 8 and -1
// (AOT_SITE_POPVAR; instance code 3 reads the instance type from the instruction).
#define AOT_SITE_WORD(addr, size, d, dA) \
    ((uint32_t) ((addr) >> 2) | ((uint32_t) ((size) / 4 - 1) << 24) | ((uint32_t) ((dA) - (d)) << 28))
#define AOT_SITE(fn, addr, size, d, dA) \
    do { uint32_t ip_ = fn(ctx, B + (d), AOT_SITE_WORD(addr, size, d, dA)); if (__builtin_expect(ip_ != 0, 0)) AOT_DEOPT_AT(ip_); } while (0)
#define AOT_SITE_V(fn, addr, size, d, dA, var) \
    do { uint32_t ip_ = fn(ctx, B + (d), AOT_SITE_WORD(addr, size, d, dA), (var)); if (__builtin_expect(ip_ != 0, 0)) AOT_DEOPT_AT(ip_); } while (0)
#define AOT_SITE_POPVAR(addr, d, ic, type2, varRef) \
    do { \
        uint32_t ip_ = aotSitePopVar(ctx, B + (d), (uint32_t) ((addr) >> 2) | ((uint32_t) (ic) << 24) | ((uint32_t) (type2) << 28), (varRef)); \
        if (__builtin_expect(ip_ != 0, 0)) AOT_DEOPT_AT(ip_); \
    } while (0)

// A real or int32 value with its tags, field by field, as the interpreter's pushes leave it but for the padding after
// the tags: GCC writes the tags as one word. Storing a whole RValue made from a compound literal also writes the
// padding, and when the value isn't a constant, zeroes the slot first and then writes each field over it.
#define AOT_SET_TAGS(s_, rvType, gmlType) \
    do { (s_)->type = (rvType); (s_)->ownsReference = false; (s_)->gmlStackType = (gmlType); (s_)->assetRefType = 0; } while (0)
#define AOT_SET_REAL(slot, v, gmlType) \
    do { RValue* s_ = (slot); s_->real = (v); AOT_SET_TAGS(s_, RVALUE_REAL, gmlType); } while (0)
#ifdef NO_RVALUE_INT64
#define AOT_SET_INT(slot, v, gmlType) \
    do { RValue* s_ = (slot); s_->int32 = (v); AOT_SET_TAGS(s_, RVALUE_INT32, gmlType); } while (0)
#else
// The int32 and the zeroed rest of the value, as one 64-bit store (little-endian).
#define AOT_SET_INT(slot, v, gmlType) \
    do { RValue* s_ = (slot); s_->int64 = (int64_t) (uint32_t) (v); AOT_SET_TAGS(s_, RVALUE_INT32, gmlType); } while (0)
#endif

// A read of one of the builtins pushSelfBuiltinVar reads straight from the instance (x, y, image_index and so on),
// on self: the same read here, and the site (slow) when self is a struct or there is no instance.
#define AOT_SELF_FIELD(d, field, slow) \
    do { \
        Instance* inst_ = (Instance*) ctx->currentInstance; \
        if (__builtin_expect(inst_ != nullptr && inst_->objectIndex != STRUCT_OBJECT_INDEX, 1)) { \
            AOT_SET_REAL(&S[d], (GMLReal) inst_->field, GML_TYPE_VARIABLE); \
            AOT_OPAQUE(d); \
        } else { \
            slow; \
        } \
    } while (0)

// The end of every generated function.
#define AOT_EPILOGUE() aot_deopt_: __attribute__((unused)); return aotDeopt(ctx, AOT_K, aotDeoptIp_)

// Each interpreter instruction reads its operands from the stack in memory and writes its result back there. The
// engine is built with -ffast-math, so if the compiler could see a value flow from one instruction into the next it
// would be free to reassociate a chain of additions or turn a division by a constant into a multiplication, and the
// generated code would round differently from the interpreter. Marking each written slot as modified by an empty asm
// statement keeps every instruction's arithmetic separate, exactly as the interpreter performs it.
#define AOT_OPAQUE(d) __asm__("" : "+m"(S[d]))

static inline uint64_t aotDoubleBits(double v) { uint64_t b; memcpy(&b, &v, sizeof b); return b; }
static inline double aotBitsDouble(uint64_t b) { double v; memcpy(&v, &b, sizeof v); return v; }
static inline float aotBitsFloat(uint32_t b) { float v; memcpy(&v, &b, sizeof v); return v; }

// ---[ Pushes of constants: what stackPushInt32Typed / stackPushRealTyped / stackPushConstString store ]---

#define AOT_PUSH_INT(d, v, gmlType) \
    do { AOT_SET_INT(&S[d], (v), (gmlType)); AOT_OPAQUE(d); } while (0)
#define AOT_PUSH_REAL(d, v, gmlType) \
    do { AOT_SET_REAL(&S[d], (v), (gmlType)); AOT_OPAQUE(d); } while (0)
#define AOT_PUSH_STRING(d, stringIndex) \
    do { \
        require((stringIndex) >= 0 && ctx->dataWin->strg.count > (uint32_t) (stringIndex)); \
        S[d] = (RValue) { .string = ctx->dataWin->strg.strings[stringIndex], .type = RVALUE_STRING, .gmlStackType = GML_TYPE_STRING }; \
    } while (0)

// stackPushWeakCopy into S[d].
#define AOT_WEAK_COPY(d, src) vmWeakCopyInto(&S[d], (src))

// ---[ Variable reads and writes ]---

// Push of argumentn (n <= 15), whatever self is: what pushSelfBuiltinVar (WAD 16) and aotPushNamedArg (WAD 17 named
// arguments) push for it.
#define AOT_PUSH_ARG(d, n) \
    do { \
        if (__builtin_expect(ctx->scriptArgs != nullptr && ctx->scriptArgCount > (n), 1)) vmWeakCopyInto(&S[d], &ctx->scriptArgs[n]); \
        else S[d] = RValue_makeUndefined(); \
    } while (0)

// The variable lookups are out of line: inlined, the hash probe made every variable access some 30 ARM instructions,
// and the generated code several times the size of the Old 3DS's 16 KB instruction cache. Each compile unit has its
// own copy (AOT_HELPER above). The helpers only the rare paths call (aotSlotWrite, aotFreeCopy, aotCmpStrings) are
// in vm_aot_bridge.inc instead, once for all of the generated code.

// Push.v of a self/other/global variable: the OP_PUSH fast path (tryFastVarRead and stackPushWeakCopy) into dst.
AOT_HELPER bool aotPushVarFast(VMContext* ctx, RValue* dst, int32_t instanceType, int32_t varID) {
    RValue* src = vmScopeVarSlot(ctx, instanceType, varID);
    if (src == nullptr) return false;
    vmWeakCopyInto(dst, src);
    return true;
}

// Pop.v.v of a self/other/global variable: the OP_POP fast path (tryFastVarWriteSlot, coerceIntStoreToRealInPlace and
// storeStackValueIntoSlot) from src.
AOT_HELPER bool aotPopVarFast(VMContext* ctx, RValue* src, int32_t instanceType, Variable* varDef, uint8_t type2) {
    if (0 > varDef->varID) return false;
    RValue* dst = vmScopeVarWriteSlot(ctx, instanceType, varDef->varID);
    if (dst == nullptr) return false;
    coerceIntStoreToRealInPlace(src, type2);
    storeStackValueIntoSlot(dst, src);
    return true;
}

// Push.v <scope>.<var> for self/other/global scopes and a non-builtin variable: aotPushVarFast, else slow (the
// interpreter's handlePush). PushGlb.v global.<var> is the same with aotPushGlobalSlow, what OP_PUSHGLB falls back to.
#define AOT_PUSH_VAR(d, scope, varIdx, slow) \
    do { if (__builtin_expect(!aotPushVarFast(ctx, &S[d], (scope), V[varIdx].varID), 0)) { slow; } } while (0)

// The value types storeStackValueIntoSlot may have to copy or reference count.
#if IS_WAD17_OR_HIGHER_ENABLED
#define AOT_REF_TYPES ((1u << RVALUE_STRING) | (1u << RVALUE_ARRAY) | (1u << RVALUE_STRUCT) | (1u << RVALUE_METHOD))
#else
#define AOT_REF_TYPES ((1u << RVALUE_STRING) | (1u << RVALUE_ARRAY) | (1u << RVALUE_STRUCT))
#endif

AOT_HELPER void aotStoreSlow(RValue* dst, RValue* src) {
    AOTC(120);
    storeStackValueIntoSlot(dst, src);
}

// storeStackValueIntoSlot, with only its common case inline: a value that is no string, array, struct or method
// going over one that owns nothing is a plain copy there too. Inlined whole, each store was some 60 ARM
// instructions of reference counting, most of a generated function's size. (A type of 32 or more, which no value
// has, may take the slow path for nothing; the slow path is the whole store, so that is never wrong.)
AOT_INLINE void aotStoreInto(RValue* dst, RValue* src) {
    uint32_t slow = (uint32_t) dst->ownsReference | ((AOT_REF_TYPES >> (src->type & 31u)) & 1u);
    if (__builtin_expect(slow != 0, 0)) aotStoreSlow(dst, src);
    else *dst = *src;
}

// coerceIntStoreToRealInPlace followed by storeStackValueIntoSlot: the OP_POP fast path's store.
#define AOT_STORE(dst, d, type2) \
    do { RValue* s_ = &S[(d) - 1]; coerceIntStoreToRealInPlace(s_, (type2)); aotStoreInto((dst), s_); } while (0)

// Pop.v.v <scope>.<var>: aotPopVarFast, else slow (aotPopVarSlow).
#define AOT_POP_VAR(d, scope, varIdx, type2, slow) \
    do { if (__builtin_expect(!aotPopVarFast(ctx, &S[(d) - 1], (scope), &V[varIdx], (type2)), 0)) { slow; } } while (0)

// ---[ Cached variable slots ]---
//
// A self, other or global variable's slot is looked up once and then used directly, for as long as nothing can have
// moved it. Instance variables live in each instance's open-addressing hash map (Instance.selfVars); nothing ever
// removes a key, so a slot only moves when the map grows, which happens only on an insert. The generator keeps a
// pointer per variable (aotc_<n>) and knows, at each instruction, which of them are still good: a pointer is set by a
// lookup and stays good until the generated code does something that could insert a variable into any map or change
// the instance a scope refers to: any call that may run code or reach such a builtin (builtin_audit.py works out
// which builtins can't), with, a builtin-variable write and the slow paths. Arithmetic, comparisons, conversions,
// Dup, locals, arguments, builtin-variable reads on self and stores into slots already found can't, so pointers
// survive them. A write whose variable isn't there yet inserts it. When that grows the map (which self, other and
// global may all share) every pointer still good is looked up again; when it doesn't, no slot moved (linear probing
// displaces nothing), and only the pointers of the same variable in the other scopes are, as the instance may be the
// same and one of them may have been nullptr. A good pointer is nullptr when the
// variable isn't there (or there is no such instance), which stays true until something inserts it: a read then
// goes the interpreter's way, and a write inserts it (or, with no instance, hands the rest of the call to the
// interpreter). In a stress build every use of a cached pointer is checked against a fresh lookup.

// The slot of variable varID in a self/other/global scope, or nullptr (vmScopeVarSlot).
AOT_HELPER RValue* aotSlotFind(VMContext* ctx, int32_t instanceType, int32_t varID) {
    return vmScopeVarSlot(ctx, instanceType, varID);
}

// Whether the last aotSlotWrite grew the map it inserted into, which moves every slot in it.
extern bool aotGrew;

// The slot a write goes to, inserted if needed, or nullptr when there is no instance (vmScopeVarWriteSlot): only
// when the pointer is nullptr. Sets aotGrew.
RValue* aotSlotWrite(VMContext* ctx, int32_t instanceType, int32_t varID);

// aotArrayRead's full read, for the elements its common case leaves out. It is cold, so the compiler keeps it out of
// the hot code.
static NOINLINE __attribute__((unused, cold)) void aotArrayReadFull(RValue* dst, RValue* slot, const RValue* index) {
    RValue v = GMLArray_getOnArrayRef(slot, vmToInt32Fast(index));
    *dst = v;
    dst->gmlStackType = GML_TYPE_VARIABLE;
}

// The cell at index in the array in slot, when that is aotArrayRead's common case: an int32 index within the length of
// an array of the kind the game makes (a GameMaker 2.3 array in a WAD 17 game, else a GameMaker 1 array's first row).
// Else nullptr.
AOT_INLINE const RValue* aotArrayFastCell(VMContext* ctx, const RValue* slot, const RValue* index) {
    (void) ctx; // IS_WAD17_OR_HIGHER(ctx) is a constant in a build with one WAD version
    if (slot->type != RVALUE_ARRAY || index->type != RVALUE_INT32) return nullptr;
    const GMLArray* arr = slot->array;
    if (arr == nullptr) return nullptr;
    uint32_t i = (uint32_t) index->int32; // a negative index is out of range either way
    if (IS_WAD17_OR_HIGHER(ctx)) {
        if (arr->type != GML_MODERN_ARRAY || i >= (uint32_t) arr->modern.length) return nullptr;
        return &arr->modern.data[i];
    }
    if (arr->type != GML_LEGACY_ARRAY || i >= GML_LEGACY_ARRAY_STRIDE || arr->legacy.rowCount == 0 || i >= (uint32_t) arr->legacy.rows[0].length) return nullptr;
    return &arr->legacy.rows[0].data[i];
}

// Push.v [array] with a self/other/global scope, once the variable's slot is known: what tryPushArrayElement pushes,
// written over the scope (S[d - 2]) with the index in S[d - 1]. The common case is a weak copy of the cell; the rest
// is aotArrayReadFull. Every read site calls it (AOT_CAREAD), so the common case is in each compile unit once, not at
// every site, and the cold full read keeps it short.
AOT_HELPER void aotArrayRead(VMContext* ctx, RValue* dst, RValue* slot, const RValue* index) {
    const RValue* cell = aotArrayFastCell(ctx, slot, index);
    if (__builtin_expect(cell != nullptr, 1)) vmWeakCopyInto(dst, cell); else aotArrayReadFull(dst, slot, index);
}

// Whether self is a struct, whose builtin-named members (x, image_index, ...) are ordinary variables: a write to
// one may insert it.
static inline __attribute__((unused)) bool aotSelfIsStruct(VMContext* ctx) {
    Instance* inst = (Instance*) ctx->currentInstance;
    return inst != nullptr && inst->objectIndex == STRUCT_OBJECT_INDEX;
}

#ifdef CTRPORT_AOT_STRESS
static NOINLINE __attribute__((unused)) void aotVerifySlot(VMContext* ctx, const RValue* p, int32_t instanceType, int32_t varID, int line) {
    requireMessageFormatted(__FILE__, __LINE__, vmScopeVarSlot(ctx, instanceType, varID) == p,
                            "AOT: [%s] cached slot of variable %d (scope %d) is stale (generated line %d)", ctx->currentCodeName, varID, instanceType, line);
}
#define AOT_CVERIFY(p, scope, varID) aotVerifySlot(ctx, (p), (scope), (varID), __LINE__)
#else
#define AOT_CVERIFY(p, scope, varID) ((void) 0)
#endif

// Looks up p (nullptr when the variable isn't there).
#define AOT_CFIND(p, scope, varID) do { (p) = aotSlotFind(ctx, (scope), (varID)); } while (0)

// Push.v of the variable at p: the OP_PUSH fast path's weak copy, else slow (the interpreter's read of a missing
// variable).
#define AOT_CREAD(d, p, scope, varID, slow) \
    do { AOT_CVERIFY(p, scope, varID); if (__builtin_expect((p) != nullptr, 1)) vmWeakCopyInto(&S[d], (p)); else { slow; } } while (0)

// Pop.v.v into the variable at p: the OP_POP fast path's store. When the variable isn't there it is inserted, or with
// no instance (other outside a with, say) the write goes the interpreter's way (slow). An insert moves no other slot
// unless it grew the map (linear probing: nothing is ever displaced), so refind (AOT_CFINDs of every pointer still
// good, or a jump to the function's block that looks up all of them) runs only then, or after slow, which may have
// grown any map. After an insert, alias looks up again the pointers of this variable in the other scopes, which may
// be the same instance: one of them may have been nullptr. The store is in the code once: an insert that found a slot
// goes on to the same store as a pointer that was good (GCC keeps the good pointer's path as short as it was, one
// test of p). refind runs after the store, which can't move a slot (a new slot holds nothing to free).
#define AOT_CSTORE(d, p, scope, varID, type2, slow, alias, refind) \
    do { \
        AOT_CVERIFY(p, scope, varID); \
        const bool ins_ = (p) == nullptr; \
        if (__builtin_expect(ins_, 0)) (p) = aotSlotWrite(ctx, (scope), (varID)); \
        if (__builtin_expect((p) != nullptr, 1)) { AOT_STORE((p), d, type2); } else { slow; } \
        if (__builtin_expect(ins_, 0)) { \
            if ((p) != nullptr) { alias; } \
            if ((p) == nullptr || aotGrew) { refind; } \
        } \
    } while (0)

// Push.v [array] of the variable at p, the scope and index on the stack, else slow. The element read and write
// (AOT_CAWRITE) stay calls: inlining their common cases at every site saved instructions but made array loops too big
// for the Old 3DS's 16 KB instruction cache, and slower.
#define AOT_CAREAD(d, p, scope, varID, slow) \
    do { \
        AOT_CVERIFY(p, scope, varID); \
        if (__builtin_expect((p) != nullptr, 1)) { \
            aotArrayRead(ctx, &S[(d) - 2], (p), &S[(d) - 1]); \
            AOT_OPAQUE((d) - 2); \
        } else { slow; } \
    } while (0)

#if IS_WAD17_OR_HIGHER_ENABLED
// The cell at index of a GameMaker 2.3 (modern) array, or nullptr when it's out of range or a legacy array:
// GMLArray_slot's modern half. WAD 17 games make modern arrays (GMLArray_create).
AOT_INLINE const RValue* aotModernCell(const GMLArray* arr, int32_t index) {
    if (arr == nullptr || arr->type != GML_MODERN_ARRAY || (uint32_t) index >= (uint32_t) arr->modern.length) return nullptr;
    return &arr->modern.data[index];
}

// Push.v [arraypushaf] of the variable at p, the first step of a[i][j], with the scope and i on the stack: when the
// variable holds an array and that holds an array at i already, what handlePush pushes, a weak ref to it, written
// over the scope (S[d - 2]). Its GMLArray_growTo changes nothing then (i is in range). Anything else (no such
// variable or instance, an array or sub-array still to make, an index that isn't an int32 or is out of range) is
// slow, handlePush, which may insert the variable; refind (AOT_CFINDs of every pointer still good, this one among
// them) runs after it. The generator emits it for plain variables only (no builtin, which handlePush reads another way).
AOT_INLINE bool aotPushAFFast(RValue* dst, const RValue* slot, const RValue* index) {
    if (slot == nullptr || slot->type != RVALUE_ARRAY || index->type != RVALUE_INT32) return false;
    const RValue* sub = aotModernCell(slot->array, index->int32);
    if (sub == nullptr || sub->type != RVALUE_ARRAY || sub->array == nullptr) return false;
    // RValue_makeArrayWeak's value (GCC calls that rather than inline it)
    *dst = (RValue) { .array = sub->array, .type = RVALUE_ARRAY, .ownsReference = false, .gmlStackType = GML_TYPE_VARIABLE };
    return true;
}

#define AOT_PUSHAF(d, p, scope, varID, slow, refind) \
    do { \
        AOT_CVERIFY(p, scope, varID); \
        if (__builtin_expect(!aotPushAFFast(&S[(d) - 2], (p), &S[(d) - 1]), 0)) { slow; refind; } \
    } while (0)

// Break pushaf, the last step of a[i][j]: what handleBreakPushAF pushes, a weak copy of the cell at the index (S[d - 1])
// of the weak array ref under it (S[d - 2]), written over the ref, when the index is an int32 in range. Anything else
// is slow (handleBreakPushAF, which also frees a ref that owns its array), then refind (AOT_CFINDs of every pointer
// still good).
AOT_INLINE bool aotBreakPushAFFast(RValue* ref, const RValue* index) {
    if (ref->type != RVALUE_ARRAY || ref->ownsReference || index->type != RVALUE_INT32) return false;
    const RValue* cell = aotModernCell(ref->array, index->int32);
    if (cell == nullptr) return false;
    vmWeakCopyInto(ref, cell);
    return true;
}

#define AOT_BREAK_PUSHAF(d, slow, refind) \
    do { if (__builtin_expect(!aotBreakPushAFFast(&S[(d) - 2], &S[(d) - 1]), 0)) { slow; refind; } } while (0)
#endif

// Pop [array] into the variable at p: popArrayElementIntoSlot, else slow (handlePop, with the stack synced and
// checked), which finds the variable where p points. When the variable isn't there, handlePop inserts it and refind
// (AOT_CFINDs of every pointer still good, this one among them) runs. The slow path is in the code once, for both.
#define AOT_CAWRITE(d, p, scope, varID, ix, sx, type2, compound, slow, refind) \
    do { \
        AOT_CVERIFY(p, scope, varID); \
        const bool miss_ = (p) == nullptr; \
        if (__builtin_expect(miss_ || !aotx_popArrayElementIntoSlot(ctx, (p), &S[ix], &S[sx], (type2), (compound)), 0)) { \
            slow; \
            if (miss_) { refind; } \
        } \
    } while (0)

// ---[ Popz ]---

void aotFreeCopy(const RValue* v);

// Popz: what the interpreter frees, a copy of S[x] (the slot itself is above the stack top from here on). RValue_free
// frees nothing unless the value owns a reference, so only then is it called.
#define AOT_POPZ(x) do { if (__builtin_expect(S[x].ownsReference, 0)) aotFreeCopy(&S[x]); } while (0)

// ---[ Arithmetic ]---

// Add/Sub/Mul of two plain numbers in place (vmArithNumbers, as executeLoop does); anything else goes to the slow half.
#define AOT_ARITH(d, op, resultType, slow) \
    do { \
        if (__builtin_expect(!vmArithNumbers(&S[(d) - 2], &S[(d) - 1], (op), (resultType)), 0)) { \
            AOT_SYNC(d); slow(ctx, (resultType)); \
        } \
        AOT_OPAQUE((d) - 2); \
    } while (0)

#define AOT_ADD(d, resultType) AOT_ARITH(d, VM_ARITH_ADD, resultType, aotx_handleAddSlow)
#define AOT_SUB(d, resultType) AOT_ARITH(d, VM_ARITH_SUB, resultType, aotx_handleSubSlow)
#define AOT_MUL(d, resultType) AOT_ARITH(d, VM_ARITH_MUL, resultType, aotx_handleMulSlow)

// The RValue a constant push stores: Push.i/Push.e (k, typed pushType) and Push.d (kv).
#define AOT_KR(kv) ((RValue) { .real = (kv), .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE })
#define AOT_KI(k, pushType) ((RValue) { .int32 = (k), .type = RVALUE_INT32, .gmlStackType = (pushType) })
// The same as an initializer, for a static constant.
#define AOT_KI_INIT(k, pushType) { .int32 = (k), .type = RVALUE_INT32, .gmlStackType = (pushType) }

// A constant push followed by Add/Sub/Mul (x + 1, x * 0.5): the same vmArithNumbers with the constant as the right
// operand, which only goes onto the stack when the other operand isn't a plain number and the slow half needs it
// there. kdecl declares the constant k_: AOT_KI_STATIC for an int, a static constant, so the fast path reads it as
// constants (a local one is written out to the C stack first); AOT_KR_LOCAL for a real, whose bits come from
// aotBitsDouble. d is the depth before the push.
#define AOT_KI_STATIC(k, pushType) static const RValue k_ = AOT_KI_INIT(k, pushType)
#define AOT_KR_LOCAL(kv) RValue k_ = AOT_KR(kv)
#define AOT_ARITH_C(d, op, kdecl, resultType, slow) \
    do { \
        kdecl; \
        if (__builtin_expect(!vmArithNumbers(&S[(d) - 1], &k_, (op), (resultType)), 0)) { \
            S[d] = k_; AOT_SYNC((d) + 1); slow(ctx, (resultType)); \
        } \
        AOT_OPAQUE((d) - 1); \
    } while (0)

#define AOT_ADD_K(d, k, pushType, resultType) AOT_ARITH_C(d, VM_ARITH_ADD, AOT_KI_STATIC(k, pushType), resultType, aotx_handleAddSlow)
#define AOT_SUB_K(d, k, pushType, resultType) AOT_ARITH_C(d, VM_ARITH_SUB, AOT_KI_STATIC(k, pushType), resultType, aotx_handleSubSlow)
#define AOT_MUL_K(d, k, pushType, resultType) AOT_ARITH_C(d, VM_ARITH_MUL, AOT_KI_STATIC(k, pushType), resultType, aotx_handleMulSlow)
#define AOT_ADD_R(d, kv, resultType) AOT_ARITH_C(d, VM_ARITH_ADD, AOT_KR_LOCAL(kv), resultType, aotx_handleAddSlow)
#define AOT_SUB_R(d, kv, resultType) AOT_ARITH_C(d, VM_ARITH_SUB, AOT_KR_LOCAL(kv), resultType, aotx_handleSubSlow)
#define AOT_MUL_R(d, kv, resultType) AOT_ARITH_C(d, VM_ARITH_MUL, AOT_KR_LOCAL(kv), resultType, aotx_handleMulSlow)

// Div of two plain numbers (vmDivNumbers, as executeLoop does it), else handleDiv. type2 is the instruction's type2.
#define AOT_DIV(d, instr, type2, intDivide) \
    do { \
        if (__builtin_expect(!vmDivNumbers(&S[(d) - 2], &S[(d) - 1], (intDivide), (type2), ctx->currentCodeName), 0)) { \
            AOT_SYNC(d); aotx_handleDiv(ctx, (instr)); \
        } \
        AOT_OPAQUE((d) - 2); \
    } while (0)

// Any other handler that pops and pushes a known number of values and cannot leave an exception behind.
#define AOT_HANDLER(d, call, dAfter) do { AOT_SYNC(d); call; AOT_OPAQUE((dAfter) - 1); } while (0)

// A plain number's value (int32 or real).
#define AOT_NUM(v) ((v)->type == RVALUE_INT32 ? (GMLReal) (v)->int32 : (v)->real)

// floor, ceil and fmod out of line (vm_aot_bridge.inc), for the inline Mod and math below. With -ffast-math GCC may
// expand floor and ceil in place without keeping the sign of a zero result (ceil(-0.5) gives +0), or call the C
// library (-0), depending on how hot it thinks the code is; a function of its own is compiled the way the builtins
// and handlers are, so the results match theirs.
GMLReal aotFloorOut(GMLReal x);
GMLReal aotCeilOut(GMLReal x);
GMLReal aotFmodOut(GMLReal x, GMLReal y);

// The operands of the inline Mod, Not, Neg and math below are read through AOT_OPAQUE, so the compiler sees values it
// knows nothing about, as the handler or builtin does. Without it, -ffast-math lets it fold a pushed constant into
// the arithmetic (dropping the sign of a zero, or folding sin of a constant at compile time with other rounding than
// the C library's) or merge two calls on the same value (sin and cos into sincos), and whether it does depends on
// the code around, so results could differ from the interpreter's in one build and not another.
#define AOT_OPAQUE_IN(d, n) \
    do { \
        if ((n) >= 1) AOT_OPAQUE((d) - 1); \
        if ((n) >= 2) AOT_OPAQUE((d) - 2); \
        if ((n) >= 3) AOT_OPAQUE((d) - 3); \
        if ((n) >= 4) AOT_OPAQUE((d) - 4); \
    } while (0)

// Mod of two plain numbers in place: what handleMod computes and pushes (fmod, a real typed type2). Anything else, or
// a zero divisor (handleMod's error), goes to handleMod.
#define AOT_MOD(d, instr, type2) \
    do { \
        AOT_OPAQUE_IN(d, 2); \
        RValue* a_ = &S[(d) - 2]; \
        const RValue* b_ = &S[(d) - 1]; \
        GMLReal div_ = vmIsPlainNumber(b_->type) ? AOT_NUM(b_) : (GMLReal) 0; \
        if (__builtin_expect(vmIsPlainNumber(a_->type) && div_ != (GMLReal) 0, 1)) { \
            GMLReal r_ = aotFmodOut(AOT_NUM(a_), div_); \
            AOT_SET_REAL(a_, r_, (type2)); \
        } else { \
            AOT_SYNC(d); aotx_handleMod(ctx, (instr)); \
        } \
        AOT_OPAQUE((d) - 2); \
    } while (0)

// Not of a bool or int32 in place: handleNot's logical not (resultType Bool) or bitwise not, pushed typed resultType
// (the instruction's type1). Anything else goes to handleNot.
#define AOT_NOT(d, instr, resultType) \
    do { \
        AOT_OPAQUE_IN(d, 1); \
        RValue* v_ = &S[(d) - 1]; \
        if (__builtin_expect(v_->type == RVALUE_BOOL || v_->type == RVALUE_INT32, 1)) { \
            int32_t x_ = v_->int32; \
            if ((resultType) == GML_TYPE_BOOL) { AOT_SET_INT(v_, x_ == 0 ? 1 : 0, (resultType)); v_->type = RVALUE_BOOL; } \
            else AOT_SET_INT(v_, ~x_, (resultType)); \
        } else { \
            AOT_SYNC(d); aotx_handleNot(ctx, (instr)); \
        } \
        AOT_OPAQUE((d) - 1); \
    } while (0)

// Neg of a plain number in place: handleNeg's real, typed resultType (the instruction's type1), else handleNeg.
#define AOT_NEG(d, instr, resultType) \
    do { \
        AOT_OPAQUE_IN(d, 1); \
        RValue* v_ = &S[(d) - 1]; \
        if (__builtin_expect(vmIsPlainNumber(v_->type), 1)) { GMLReal r_ = -AOT_NUM(v_); AOT_SET_REAL(v_, r_, (resultType)); } \
        else { AOT_SYNC(d); aotx_handleNeg(ctx, (instr)); } \
        AOT_OPAQUE((d) - 1); \
    } while (0)

// ---[ Conversions ]---

// The four conversions executeLoop handles inline (vmConvFast), else handleConv.
#define AOT_CONV(d, srcType, dstType) \
    do { \
        RValue* t_ = &S[(d) - 1]; \
        if (__builtin_expect(vmConvFast(t_, (uint8_t) (((dstType) << 4) | (srcType))), 1)) t_->gmlStackType = (dstType); \
        else { AOT_SYNC(d); aotx_handleConv(ctx, (srcType), (dstType), (uint8_t) (((dstType) << 4) | (srcType))); } \
        AOT_OPAQUE((d) - 1); \
    } while (0)

// ---[ Comparisons and branches ]---

// The result of Cmp for two plain numbers or two strings, as executeLoop computes it inline (vmCmpNumbers,
// vmCmpStrings); -1 for anything else, which goes to handleCmp. Strings are freed, as executeLoop frees them.
int aotCmpStrings(RValue* a, RValue* b, uint8_t kind);

AOT_INLINE int aotCmpFast(RValue* a, RValue* b, uint8_t kind) {
    int r = vmCmpNumbers(a, b, kind);
    if (r < 0) r = aotCmpStrings(a, b, kind);
    return r;
}

AOT_HELPER int32_t aotToInt32Slow(const RValue* v) {
    return RValue_toInt32(*v);
}

// stackPopInt32 != 0 on S[d - 1], without changing ctx->stack.top: vmToInt32Fast, its conversion out of line.
AOT_INLINE bool aotTruth(RValue* v) {
    if (__builtin_expect(v->type == RVALUE_BOOL || v->type == RVALUE_INT32, 1)) return v->int32 != 0;
    return aotToInt32Slow(v) != 0;
}

// Cmp leaving its bool on the stack, written the way executeLoop writes it. kind is the instruction's comparison kind.
#define AOT_CMP(d, instr, kind) \
    do { \
        int r_ = vmCmpNumbers(&S[(d) - 2], &S[(d) - 1], (kind)); \
        if (__builtin_expect(r_ >= 0, 1)) vmSetCmpBoolInPlace(&S[(d) - 2], r_); \
        else if ((r_ = aotCmpStrings(&S[(d) - 2], &S[(d) - 1], (kind))) >= 0) S[(d) - 2] = vmCmpBool(r_); \
        else { AOT_SYNC(d); aotx_handleCmp(ctx, (instr)); } \
    } while (0)

// Cmp straight into a branch: the bool Cmp would push and BT/BF pop is never stored.
#define AOT_CMP_BRANCH(d, instr, kind, branchIfTrue, label) \
    do { \
        int r_ = aotCmpFast(&S[(d) - 2], &S[(d) - 1], (kind)); \
        if (__builtin_expect(r_ < 0, 0)) { AOT_SYNC(d); aotx_handleCmp(ctx, (instr)); r_ = aotTruth(&S[(d) - 2]); } \
        if ((r_ != 0) == (branchIfTrue)) goto label; \
    } while (0)

// An optimisation barrier on a real: the compiler can no longer see where its value came from. In a floating-point
// register where the target has a constraint for one (a memory operand costs a store and a load).
#if defined(__arm__) && defined(__ARM_FP)
#define AOT_OPAQUE_REAL(v) __asm__("" : "+w"(v))
#elif defined(__x86_64__) || defined(__i386__)
#define AOT_OPAQUE_REAL(v) __asm__("" : "+x"(v))
#else
#define AOT_OPAQUE_REAL(v) __asm__("" : "+m"(v))
#endif

// A constant push followed by Cmp (x == 2): vmCmpNumbers with a known number as the right operand (kv, the value the
// push would store), -1 when the left operand isn't a plain number. d is the depth before the push.
AOT_INLINE int aotCmpFastK(RValue* a, GMLReal kv, uint8_t kind) {
    uint8_t at = a->type;
    if (!vmIsPlainNumber(at)) return -1;
    GMLReal av = at == RVALUE_INT32 ? (GMLReal) a->int32 : a->real;
    GMLReal diff = av - kv;
    // With a constant on one side, -ffast-math could fold the constant into the epsilon test (x - 2 <= eps into
    // x <= 2 + eps), which rounds differently; computing the difference behind a barrier keeps the interpreter's test.
    AOT_OPAQUE_REAL(diff);
    return vmCmpHolds(vmCmpOrder(diff), kind);
}

// kslot is the RValue the push would have stored (AOT_KI / AOT_KR): handleCmp needs it on the stack.
#define AOT_CMP_K_SLOW(d, instr, kslot) \
    do { S[d] = kslot; AOT_SYNC((d) + 1); aotx_handleCmp(ctx, (instr)); } while (0)

#define AOT_CMP_K(d, instr, kind, kv, kslot) \
    do { \
        int r_ = aotCmpFastK(&S[(d) - 1], (kv), (kind)); \
        if (__builtin_expect(r_ >= 0, 1)) vmSetCmpBoolInPlace(&S[(d) - 1], r_); \
        else AOT_CMP_K_SLOW(d, instr, kslot); \
    } while (0)

#define AOT_CMP_K_BRANCH(d, instr, kind, kv, kslot, branchIfTrue, label) \
    do { \
        int r_ = aotCmpFastK(&S[(d) - 1], (kv), (kind)); \
        if (__builtin_expect(r_ < 0, 0)) { AOT_CMP_K_SLOW(d, instr, kslot); r_ = aotTruth(&S[(d) - 1]); } \
        if ((r_ != 0) == (branchIfTrue)) goto label; \
    } while (0)

// ---[ Dup ]---

// Dup of up to four values that each are one unit of the instruction's type (typeSize: gmlTypeNativeSize of its type1)
// and own nothing (executeLoop's inline case, vmDupIsPlain) when the stack has room for the copies, else slow
// (handleDup, which may copy another number of slots).
#define AOT_DUP_PLAIN(d, typeSize, count, slow) \
    do { \
        RValue* first_ = &S[(d) - (count)]; \
        if (__builtin_expect(VM_STACK_SIZE >= B + (d) + (count) && vmDupIsPlain(first_, (count), (typeSize)), 1)) { \
            for (int32_t i_ = 0; (count) > i_; i_++) first_[(count) + i_] = first_[i_]; \
        } else { slow; } \
    } while (0)

// Dup.v swap(N, 1) at the start of a method call: the fused getter/setter call, else the rotation executeLoop does.
// The fused call may have called a method that is not a getter or setter, which can leave an exception pending: then
// carry on in the interpreter after the CallV (fusedLabel's address, addr + 20), as AOT_CALLV's check does.
#define AOT_DUP_SWAP_CALL(d, instr, moved, addr, fusedLabel) \
    do { \
        AOT_SYNC(d); \
        if (aotx_tryFusedAccessorCall(ctx, (instr), BC + (addr) + 4, ctx->codeEnd - ((addr) + 4), (addr) + 20) != 0) { \
            if (__builtin_expect(AOT_EXCEPTION_PENDING(), 0)) AOT_DEOPT_AT((addr) + 20); \
            goto fusedLabel; \
        } \
        RValue below_ = S[(d) - 1 - (moved)]; \
        for (int32_t i_ = 0; (moved) > i_; i_++) S[(d) - 1 - (moved) + i_] = S[(d) - (moved) + i_]; \
        S[(d) - 1] = below_; \
    } while (0)

// AOT_DUP_SWAP_CALL where the generator has matched the getter/setter call's four instructions itself (the digest pins
// them) and knows the method's varID (>= 0) and that there are at most one argument: no bytecode is read. next is the
// address after the CallV (fusedLabel's).
#define AOT_DUP_SWAP_CALL_K(d, moved, methodVarID, fusedLabel, next) \
    do { \
        AOT_SYNC(d); \
        if (aotx_fusedAccessorCall(ctx, (moved), (methodVarID), (next)) != 0) { \
            if (__builtin_expect(AOT_EXCEPTION_PENDING(), 0)) AOT_DEOPT_AT(next); \
            goto fusedLabel; \
        } \
        RValue below_ = S[(d) - 1 - (moved)]; \
        for (int32_t i_ = 0; (moved) > i_; i_++) S[(d) - 1 - (moved) + i_] = S[(d) - (moved) + i_]; \
        S[(d) - 1] = below_; \
    } while (0)

// ---[ Calls ]---

#define AOT_CALL(d, instr, addr, dAfter) \
    do { AOT_SYNC(d); ctx->ip = (addr) + 8; aotx_handleCall(ctx, (instr), BC + (addr) + 4); AOT_CHECK(dAfter, (addr) + 8); } while (0)

// Call.i of a function that may be a script (one with a code entry of its name): aotCallScript, which calls a plain
// script without handleCall or an argument block, and hands anything else to handleCall. funcIndex is the function
// operand, which the digest and aotMatches pin.
#define AOT_CALL_SCRIPT(d, instr, addr, dAfter, funcIndex) \
    do { AOT_SYNC(d); ctx->ip = (addr) + 8; aotCallScript(ctx, (instr), BC + (addr) + 4, (funcIndex)); AOT_CHECK(dAfter, (addr) + 8); } while (0)

// Call.i of a builtin that can't run code or touch the operand stack (builtin_audit.py): aotCallBuiltin, which calls
// it on the arguments where the caller pushed them, without handleCall's argument block.
#define AOT_CALL_BUILTIN(d, instr, addr, dAfter, funcIndex) \
    do { AOT_SYNC(d); ctx->ip = (addr) + 8; aotCallBuiltin(ctx, (instr), BC + (addr) + 4, (funcIndex)); AOT_CHECK(dAfter, (addr) + 8); } while (0)

// Call.i of one of a few math builtins, with arguments that are all plain numbers: the builtin's own arithmetic
// inline, its result where aotCallBuiltin leaves it (a real typed Variable over the first argument slot); any other
// argument takes the call. Each aotMath function is a copy of its builtin's body in vm_builtins.c, and gmlaot.py
// inlines a builtin only while that body is the one the copy was made from (INLINE_MATH, by a hash of it). The
// arguments are on the stack last first: args[0] is S[d - 1].
AOT_INLINE GMLReal aotMathFloor(GMLReal x) { return aotFloorOut(x); }
AOT_INLINE GMLReal aotMathCeil(GMLReal x) { return aotCeilOut(x); }
AOT_INLINE GMLReal aotMathAbs(GMLReal x) { return GMLReal_fabs(x); }
AOT_INLINE GMLReal aotMathSqrt(GMLReal x) { return GMLReal_sqrt(x); }
AOT_INLINE GMLReal aotMathSqr(GMLReal x) { return x * x; }
AOT_INLINE GMLReal aotMathSin(GMLReal x) { return GMLReal_sin(x); }
AOT_INLINE GMLReal aotMathCos(GMLReal x) { return GMLReal_cos(x); }
AOT_INLINE GMLReal aotMathDsin(GMLReal x) { return GMLReal_sin(x * (M_PI / 180.0)); }
AOT_INLINE GMLReal aotMathDcos(GMLReal x) { return GMLReal_cos(x * (M_PI / 180.0)); }
AOT_INLINE GMLReal aotMathDegtorad(GMLReal x) { return x * (M_PI / 180.0); }
AOT_INLINE GMLReal aotMathRadtodeg(GMLReal x) { return x * (180.0 / M_PI); }
AOT_INLINE GMLReal aotMathFrac(GMLReal val) {
    GMLReal truncated = (val >= 0.0) ? aotFloorOut(val) : aotCeilOut(val);
    return val - truncated;
}
AOT_INLINE GMLReal aotMathSign(GMLReal val) {
    GMLReal result = (val > 0.0) ? 1.0 : ((0.0 > val) ? -1.0 : 0.0);
    return result;
}
AOT_INLINE GMLReal aotMathMax(GMLReal a0, GMLReal a1) {
    GMLReal result = -INFINITY;
    if (a0 > result) result = a0;
    if (a1 > result) result = a1;
    return result;
}
AOT_INLINE GMLReal aotMathMin(GMLReal a0, GMLReal a1) {
    GMLReal result = INFINITY;
    if (result > a0) result = a0;
    if (result > a1) result = a1;
    return result;
}
AOT_INLINE GMLReal aotMathLengthdirX(GMLReal len, GMLReal dir0) {
    GMLReal dir = dir0 * (M_PI / 180.0);
    GMLReal result = len * GMLReal_cos(dir);
    if ((GMLReal) 1e-4 > GMLReal_fabs(result)) result = 0.0;
    return result;
}
AOT_INLINE GMLReal aotMathLengthdirY(GMLReal len, GMLReal dir0) {
    GMLReal dir = dir0 * (M_PI / 180.0);
    GMLReal result = -len * GMLReal_sin(dir);
    if ((GMLReal) 1e-4 > GMLReal_fabs(result)) result = 0.0;
    return result;
}
AOT_INLINE GMLReal aotMathAngleDifference(GMLReal src, GMLReal dest) {
    return aotFmodOut(aotFmodOut(src - dest, 360.0) + 540.0, 360.0) - 180.0;
}
AOT_INLINE GMLReal aotMathClamp(GMLReal val, GMLReal lo, GMLReal hi) {
    if (lo > val) val = lo;
    if (val > hi) val = hi;
    return val;
}
AOT_INLINE GMLReal aotMathLerp(GMLReal a, GMLReal b, GMLReal t) {
    GMLReal result = a + (b - a) * t;
#ifdef USE_FLOAT_REALS
    if (result == a && a != b) result = GMLReal_nextafter(a, b);
#endif
    return result;
}
AOT_INLINE GMLReal aotMathPointDistance(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) {
    GMLReal dx = x2 - x1;
    GMLReal dy = y2 - y1;
    return GMLReal_sqrt(dx * dx + dy * dy);
}

#define AOT_MATH_RESULT(d, argc, r) do { RValue* o_ = &S[(d) - (argc)]; AOT_SET_REAL(o_, (r), GML_TYPE_VARIABLE); AOT_OPAQUE((d) - (argc)); } while (0)
#define AOT_MATH1(d, instr, addr, dAfter, funcIndex, f) \
    do { \
        AOT_OPAQUE_IN(d, 1); \
        const RValue* a0_ = &S[(d) - 1]; \
        if (__builtin_expect(vmIsPlainNumber(a0_->type), 1)) { GMLReal r_ = f(AOT_NUM(a0_)); AOT_MATH_RESULT(d, 1, r_); } \
        else AOT_CALL_BUILTIN(d, instr, addr, dAfter, funcIndex); \
    } while (0)
#define AOT_MATH2(d, instr, addr, dAfter, funcIndex, f) \
    do { \
        AOT_OPAQUE_IN(d, 2); \
        const RValue *a0_ = &S[(d) - 1], *a1_ = &S[(d) - 2]; \
        if (__builtin_expect(vmIsPlainNumber(a0_->type) && vmIsPlainNumber(a1_->type), 1)) { \
            GMLReal r_ = f(AOT_NUM(a0_), AOT_NUM(a1_)); AOT_MATH_RESULT(d, 2, r_); \
        } else AOT_CALL_BUILTIN(d, instr, addr, dAfter, funcIndex); \
    } while (0)
#define AOT_MATH3(d, instr, addr, dAfter, funcIndex, f) \
    do { \
        AOT_OPAQUE_IN(d, 3); \
        const RValue *a0_ = &S[(d) - 1], *a1_ = &S[(d) - 2], *a2_ = &S[(d) - 3]; \
        if (__builtin_expect(vmIsPlainNumber(a0_->type) && vmIsPlainNumber(a1_->type) && vmIsPlainNumber(a2_->type), 1)) { \
            GMLReal r_ = f(AOT_NUM(a0_), AOT_NUM(a1_), AOT_NUM(a2_)); AOT_MATH_RESULT(d, 3, r_); \
        } else AOT_CALL_BUILTIN(d, instr, addr, dAfter, funcIndex); \
    } while (0)
#define AOT_MATH4(d, instr, addr, dAfter, funcIndex, f) \
    do { \
        AOT_OPAQUE_IN(d, 4); \
        const RValue *a0_ = &S[(d) - 1], *a1_ = &S[(d) - 2], *a2_ = &S[(d) - 3], *a3_ = &S[(d) - 4]; \
        if (__builtin_expect(vmIsPlainNumber(a0_->type) && vmIsPlainNumber(a1_->type) && vmIsPlainNumber(a2_->type) \
                             && vmIsPlainNumber(a3_->type), 1)) { \
            GMLReal r_ = f(AOT_NUM(a0_), AOT_NUM(a1_), AOT_NUM(a2_), AOT_NUM(a3_)); AOT_MATH_RESULT(d, 4, r_); \
        } else AOT_CALL_BUILTIN(d, instr, addr, dAfter, funcIndex); \
    } while (0)

#define AOT_CALLV(d, instr, addr, dAfter) \
    do { AOT_SYNC(d); ctx->ip = (addr) + 4; aotx_handleCallV(ctx, (instr)); AOT_CHECK(dAfter, (addr) + 4); } while (0)

// ---[ with ]---

#define AOT_PUSHENV(d, instr, addr, dAfter, skipLabel, skipAddr) \
    do { \
        AOT_SYNC(d); ctx->ip = (addr) + 4; aotx_handlePushEnv(ctx, (instr), (addr)); \
        if (__builtin_expect(ctx->stack.top != B + (dAfter), 0)) AOT_DEOPT_AT(ctx->ip); \
        if (ctx->ip == (skipAddr)) goto skipLabel; \
        if (__builtin_expect(ctx->ip != (addr) + 4, 0)) AOT_DEOPT_AT(ctx->ip); \
    } while (0)

#define AOT_POPENV(d, instr, addr, loopLabel, loopAddr) \
    do { \
        AOT_SYNC(d); ctx->ip = (addr) + 4; aotx_handlePopEnv(ctx, (instr), (addr)); \
        if (ctx->ip == (loopAddr)) goto loopLabel; \
        if (__builtin_expect(ctx->ip != (addr) + 4, 0)) AOT_DEOPT_AT(ctx->ip); \
    } while (0)

#define AOT_POPENV_EXIT(d, instr, addr) \
    do { \
        AOT_SYNC(d); ctx->ip = (addr) + 4; aotx_handlePopEnv(ctx, (instr), (addr)); \
        if (__builtin_expect(ctx->ip != (addr) + 4, 0)) AOT_DEOPT_AT(ctx->ip); \
    } while (0)

// ---[ Returns ]---

#define AOT_RET(d) do { RValue r_ = S[(d) - 1]; ctx->stack.top = B + (d) - 1; return r_; } while (0)
#define AOT_EXIT(d) do { AOT_SYNC(d); return aotx_scriptFallthroughReturnValue(ctx); } while (0)

// The start of every generated function. S is made opaque so that GCC keeps it in a register: otherwise it rebuilds
// each &S[d] from ctx and B (three instructions apiece), which made the code some 10% bigger.
#define AOT_PROLOGUE() \
    const int32_t B = ctx->stack.top; \
    RValue* S = &ctx->stack.slots[B]; \
    __asm__("" : "+r"(S)); \
    const uint8_t* const BC = ctx->bytecodeBase; \
    Variable* const V = ctx->dataWin->vari.variables; \
    uint32_t aotDeoptIp_ = 0; \
    (void) S; (void) BC; (void) V; (void) aotDeoptIp_

// Locals. Each generated function keeps the localVars slot of every local it uses in a static array (AOT_LS). The
// slots come from resolveLocalSlot, which keeps them in the code entry's slot map, so they are the slots the
// interpreter uses too, and a slot never changes once the map holds it. They are looked up on the first call and
// again only when the map is another one (another VMContext: aotEpoch changes whenever the bindings are dropped) or
// this frame's localVars window doesn't cover them yet. Slot numbers stay valid when ctx->localVars itself is
// reallocated, which is why locals are read as LV(n).
#define AOT_LOCALS(varIndices, slots, cache, count) \
    do { \
        if (__builtin_expect((cache)->epoch != aotEpoch || (cache)->map != ctx->currentCodeLocalsSlotMap \
                             || (cache)->maxSlot >= ctx->localVarCount, 0)) \
            if (!aotResolveLocals(ctx, (varIndices), (slots), (cache), (count))) return aotx_executeLoop(ctx); \
    } while (0)

#ifdef ENABLE_WAD17
#define AOT_ENTRY_CHECK() do { if (ctx->exception != nullptr) return aotx_executeLoop(ctx); } while (0)
#else
#define AOT_ENTRY_CHECK() do { } while (0)
#endif

#define LV(n) (ctx->localVars[AOT_LS[n]])

// Stress mode (CTRPORT_AOT_STRESS): every 7th block start (BS_AOT_STRESS_PERIOD=n: every nth, 0: none) hands over
// to the interpreter, which tests that a deopt at any point gives the same results.
#ifdef CTRPORT_AOT_STRESS
#define AOT_BLOCK(d, addr) \
    do { if (aotStressPeriod != 0 && ++aotStressCounter % aotStressPeriod == 0) AOT_DEOPT(d, addr); } while (0)
#else
#define AOT_BLOCK(d, addr) do { } while (0)
#endif

#endif
