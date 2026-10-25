#ifndef _BS_VM_FASTPATH_H_
#define _BS_VM_FASTPATH_H_

// The interpreter's inline fast paths that the ahead-of-time compiled code (aot/vm_aot.h) also uses.
// executeLoop and the generated code both call these, so the two compute every result the same way by construction.
// They only touch values and slots handed to them, never the VM stack pointer.

#include "vm.h"
#include "rvalue.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "wad_versions.h"

#if defined(__GNUC__) || defined(__clang__)
#define VM_FAST_INLINE static inline __attribute__((always_inline))
#else
#define VM_FAST_INLINE static inline
#endif

// Returns the native byte size of a GML data type on the runner's stack.
// The Dup instruction (and several BC17+ BREAK sub-opcodes) encode byte counts, not slot counts.
static inline int gmlTypeNativeSize(uint8_t gmlType) {
    switch (gmlType) {
        case GML_TYPE_DOUBLE:   return 8;
        case GML_TYPE_FLOAT:    return 4;
        case GML_TYPE_INT32:    return 4;
        case GML_TYPE_INT64:    return 8;
        case GML_TYPE_BOOL:     return 4;
        case GML_TYPE_VARIABLE: return 16;
        case GML_TYPE_STRING:   return 4;
        case GML_TYPE_INT16:    return 4;
        default:                return 16;
    }
}

// ---[ Variables ]---

// The instance whose variables a plain (non-builtin) variable access in the self, other or global scope goes to;
// nullptr for any other scope, or when there is no such instance.
static inline Instance* vmScopeInstance(VMContext* ctx, int32_t instanceType) {
    switch (instanceType) {
        case INSTANCE_SELF:   return (Instance*) ctx->currentInstance;
        case INSTANCE_OTHER:  return (Instance*) ctx->otherInstance;
        case INSTANCE_GLOBAL: return (Instance*) ctx->globalScopeInstance;
        default:              return nullptr;
    }
}

// The slot of variable varID (>= 0) in the self/other/global scope, or nullptr when it isn't there (or the scope is
// another one): the read fast path. The caller pushes a weak copy of it (vmWeakCopyInto).
static inline RValue* vmScopeVarSlot(VMContext* ctx, int32_t instanceType, int32_t varID) {
    Instance* inst = vmScopeInstance(ctx, instanceType);
    if (inst == nullptr) return nullptr;
    return IntRValueHashMap_findSlot(&inst->selfVars, varID);
}

// The slot a write of variable varID (>= 0) in the self/other/global scope goes to, inserted if needed, or nullptr
// when the write has to go through resolveVariableWrite.
static inline RValue* vmScopeVarWriteSlot(VMContext* ctx, int32_t instanceType, int32_t varID) {
    Instance* inst = vmScopeInstance(ctx, instanceType);
    if (inst == nullptr) return nullptr;
    return IntRValueHashMap_getOrInsertUndefinedFast(&inst->selfVars, varID);
}

// What stackPushWeakCopy stores: the value without its reference, typed as a variable.
static inline void vmWeakCopyInto(RValue* dst, const RValue* src) {
    *dst = *src;
    dst->ownsReference = false;
    dst->gmlStackType = GML_TYPE_VARIABLE;
}

// In-place version of coerceIntStoreToReal for a value on the stack.
static inline void coerceIntStoreToRealInPlace(RValue* val, uint8_t type2) {
    if (type2 == GML_TYPE_INT32 || type2 == GML_TYPE_INT64 || type2 == GML_TYPE_INT16) {
        if (val->type == RVALUE_INT32) {
            *val = (RValue) { .real = (GMLReal) val->int32, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE };
        }
#ifndef NO_RVALUE_INT64
        else if (val->type == RVALUE_INT64) {
            *val = (RValue) { .real = (GMLReal) val->int64, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE };
        }
#endif
    }
}

// Moves a popped stack value into a variable slot: an owning value hands its reference over, anything else is copied
// (RValue_makeIndependent). This gives the same result as RValue_writeIntoSlotStealingOwnershipOrCopying, and as
// Instance_setSelfVar followed by freeing the popped value. The one exception is a static string
// (RValue_isStaticString), which never goes away, so the variable points at it instead of holding a copy.
static inline void storeStackValueIntoSlot(RValue* dst, RValue* src) {
    uint8_t type = src->type;
    bool refType = type == RVALUE_STRING || type == RVALUE_ARRAY || type == RVALUE_STRUCT;
#if IS_WAD17_OR_HIGHER_ENABLED
    refType = refType || type == RVALUE_METHOD;
#endif
    if (refType && !src->ownsReference && !(type == RVALUE_STRING && RValue_isStaticString(src->string))) {
        RValue_writeIntoSlotStealingOwnershipOrCopying(dst, *src);
        return;
    }
    if (dst->ownsReference) RValue_free(dst);
    *dst = *src;
}

// ---[ Truth ]---

// What stackPopInt32 reads from a value (branch conditions, indices): bools and int32s directly, else the conversion.
VM_FAST_INLINE int32_t vmToInt32Fast(const RValue* v) {
    if (v->type == RVALUE_BOOL || v->type == RVALUE_INT32) return v->int32;
    return RValue_toInt32(*v);
}

// ---[ Arithmetic ]---

enum { VM_ARITH_ADD, VM_ARITH_SUB, VM_ARITH_MUL };

static inline bool vmIsPlainNumber(uint8_t type) {
    return type == RVALUE_INT32 || type == RVALUE_REAL;
}

// Add, Sub or Mul (op, a constant at every call site) of two plain numbers, int32 or real, into a, typed resultType.
// Two int32s give an int32 (wrapping), anything else a real. Returns false, changing nothing, when either operand is
// anything else: strings, int64s and the rest take the slow path.
VM_FAST_INLINE bool vmArithNumbers(RValue* a, const RValue* b, int op, uint8_t resultType) {
    uint8_t aType = a->type;
    uint8_t bType = b->type;
    if (!vmIsPlainNumber(aType) || !vmIsPlainNumber(bType)) return false;
    if (aType == RVALUE_INT32 && bType == RVALUE_INT32) {
        uint32_t x = (uint32_t) a->int32, y = (uint32_t) b->int32;
        a->int32 = (int32_t) (op == VM_ARITH_ADD ? x + y : op == VM_ARITH_SUB ? x - y : x * y);
    } else {
        // Read both operands before writing back: a->real and a->int32 share storage.
        GMLReal aVal = (aType == RVALUE_INT32) ? (GMLReal) a->int32 : a->real;
        GMLReal bVal = (bType == RVALUE_INT32) ? (GMLReal) b->int32 : b->real;
        a->real = op == VM_ARITH_ADD ? aVal + bVal : op == VM_ARITH_SUB ? aVal - bVal : aVal * bVal;
        a->type = RVALUE_REAL;
    }
    a->gmlStackType = resultType;
    return true;
}

// Div of two plain numbers into a: always a real, typed resultType. intDivide (both of the instruction's types are
// integers) makes a division by zero an error, as handleDiv does. Returns false, changing nothing, for anything else.
VM_FAST_INLINE bool vmDivNumbers(RValue* a, const RValue* b, bool intDivide, uint8_t resultType, const char* codeName) {
    uint8_t aType = a->type;
    uint8_t bType = b->type;
    if (!vmIsPlainNumber(aType) || !vmIsPlainNumber(bType)) return false;
    GMLReal divisor = bType == RVALUE_INT32 ? (GMLReal) b->int32 : b->real;
    if (intDivide) {
        requireMessageFormatted(__FILE__, __LINE__, divisor != 0.0, "VM: [%s] DoDiv :: Divide by zero", codeName);
    }
    GMLReal dividend = aType == RVALUE_INT32 ? (GMLReal) a->int32 : a->real;
    a->real = dividend / divisor;
    a->type = RVALUE_REAL;
    a->gmlStackType = resultType;
    return true;
}

// ---[ Conversions ]---

// The four conversions done inline (about 93% of all Conv instructions in real workloads), on v in place; convKey is
// (dstType << 4) | srcType. Returns false, changing nothing, for anything else (handleConv's job). The caller then
// types the value as dstType.
VM_FAST_INLINE bool vmConvFast(RValue* v, uint8_t convKey) {
    switch (convKey) {
        case 0x52: // Int32 -> Variable (pure passthrough; just retag stack slot)
            return true;
        case 0x45: // Variable -> Bool
            switch (v->type) {
                case RVALUE_INT32: v->int32 = v->int32 > 0 ? 1 : 0; v->type = RVALUE_BOOL; return true;
                case RVALUE_BOOL: return true; // already 0/1
                case RVALUE_REAL: v->int32 = v->real > (GMLReal) 0.5 ? 1 : 0; v->type = RVALUE_BOOL; return true;
            }
            return false;
        case 0x25: // Variable -> Int32
            switch (v->type) {
                case RVALUE_INT32: return true;
                case RVALUE_BOOL: v->type = RVALUE_INT32; return true;
                case RVALUE_REAL: v->int32 = (int32_t) v->real; v->type = RVALUE_INT32; return true;
            }
            return false;
        case 0x02: // Int32 -> Double (Real)
            v->real = (GMLReal) v->int32;
            v->type = RVALUE_REAL;
            return true;
    }
    return false;
}

// ---[ Comparisons ]---

// The order of two numbers that differ by diff: equal within GML_MATH_EPSILON, else by sign.
VM_FAST_INLINE int vmCmpOrder(GMLReal diff) {
    return (GMLReal_fabs(diff) <= GML_MATH_EPSILON) ? 0 : (diff < 0) ? -1 : 1;
}

// Whether comparison kind (CMP_LT...) holds for an order (negative, zero or positive).
VM_FAST_INLINE int vmCmpHolds(int order, uint8_t kind) {
    switch (kind) {
        case CMP_LT:  return order < 0;
        case CMP_LTE: return order <= 0;
        case CMP_EQ:  return order == 0;
        case CMP_NEQ: return order != 0;
        case CMP_GTE: return order >= 0;
        case CMP_GT:  return order > 0;
        default:      return 0;
    }
}

// Cmp of two plain numbers: 1 or 0, or -1 when either isn't one.
VM_FAST_INLINE int vmCmpNumbers(const RValue* a, const RValue* b, uint8_t kind) {
    uint8_t aType = a->type;
    uint8_t bType = b->type;
    if (!vmIsPlainNumber(aType) || !vmIsPlainNumber(bType)) return -1;
    GMLReal av = aType == RVALUE_INT32 ? (GMLReal) a->int32 : a->real;
    GMLReal bv = bType == RVALUE_INT32 ? (GMLReal) b->int32 : b->real;
    return vmCmpHolds(vmCmpOrder(av - bv), kind);
}

// Cmp of two strings, the same result as handleCmp's string branch: 1 or 0, after freeing both, or -1 (nothing freed)
// when either isn't a string.
VM_FAST_INLINE int vmCmpStrings(RValue* a, RValue* b, uint8_t kind) {
    if (a->type != RVALUE_STRING || b->type != RVALUE_STRING) return -1;
    int order = strcmp(a->string != nullptr ? a->string : "", b->string != nullptr ? b->string : "");
    int result = vmCmpHolds(order, kind);
    RValue_free(a);
    RValue_free(b);
    return result;
}

// The bool a numeric Cmp leaves, written over its first operand in place (its other fields stay as they were).
VM_FAST_INLINE void vmSetCmpBoolInPlace(RValue* a, int result) {
    a->int32 = result;
    a->type = RVALUE_BOOL;
    a->gmlStackType = GML_TYPE_BOOL;
}

// The bool a string Cmp leaves (a fresh value: the strings are gone).
VM_FAST_INLINE RValue vmCmpBool(int result) {
    return (RValue) { .int32 = result, .type = RVALUE_BOOL, .gmlStackType = GML_TYPE_BOOL };
}

// ---[ Dup ]---

// Whether a Dup of the count values from first on can copy them as they are: each is one unit of the instruction's
// type (typeSize, gmlTypeNativeSize of its type1) and owns nothing. Otherwise handleDup has to do it.
VM_FAST_INLINE bool vmDupIsPlain(const RValue* first, int32_t count, int32_t typeSize) {
    for (int32_t i = 0; count > i; i++) {
        if (first[i].ownsReference || gmlTypeNativeSize(first[i].gmlStackType) != typeSize) return false;
    }
    return true;
}

#endif
