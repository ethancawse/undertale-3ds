// Cold validation for typed inlining. Kept out of vm.c so adding a cold caller cannot change its unit-wide inline
// budget, and in the retained late reserve. No AotEntry or module-header ABI change.
#include "aot/vm_aot.h"
#include "vm_builtins.h"

#if defined(__arm__) && defined(__3DS__)
#define INLINE_BIND_COLD __attribute__((noinline, section(".text.late.typed3")))
#else
#define INLINE_BIND_COLD NOINLINE
#endif

#if VM_HAS_AOT
static INLINE_BIND_COLD bool inlineBindingMatches(VMContext* ctx, const AotInlineBinding* binding) {
    DataWin* dw = ctx->dataWin;
    const AotEntry* entry = &binding->entry;
    if (binding->wadVersion != dw->gen8.wadVersion || binding->codeIndex >= dw->code.count) return false;
    CodeEntry* code = &dw->code.entries[binding->codeIndex];
    if (code->name == nullptr || strcmp(code->name, entry->codeName) != 0 || code->offset != entry->offset
        || code->length != entry->blobLength || entry->offset > entry->end || entry->end > code->length) return false;
    const uint8_t* bytes = DataWin_codeBytes(dw, code);
    if (bytes == nullptr) return false;
    uint64_t digest = 0xCBF29CE484222325ull;
    for (uint32_t i = entry->offset; entry->end > i; i++) {
        digest ^= bytes[i];
        digest *= 0x100000001B3ull;
    }
    if (digest != entry->digest) return false;
    for (uint32_t i = 0; entry->varCount > i; i++) {
        const AotVarRef* ref = &entry->vars[i];
        if (ref->index >= dw->vari.variableCount) return false;
        const Variable* var = &dw->vari.variables[ref->index];
        if (var->varID != ref->varID || var->instanceType != ref->instanceType || var->name == nullptr
            || strcmp(var->name, ref->name) != 0) return false;
        // VM_create derives this dispatch ID from the definition. Pin it too: inline argument/builtin reads may
        // fold the name into an expression rather than performing the interpreter's dispatch on this ID.
        int16_t builtinId = var->varID == VARIABLE_BUILTIN || var->instanceType == -6
                            ? VMBuiltins_resolveBuiltinVarId(var->name) : BUILTIN_VAR_UNKNOWN;
        if (var->builtinVarId != builtinId) return false;
    }
    for (uint32_t i = 0; entry->funcCount > i; i++) {
        const AotFuncRef* ref = &entry->funcs[i];
        if (ref->index >= dw->func.functionCount || dw->func.functions[ref->index].name == nullptr
            || strcmp(dw->func.functions[ref->index].name, ref->name) != 0) return false;
    }
    return true;
}

INLINE_BIND_COLD void aotResolveInlineBindings(VMContext* ctx, AotInlineCache* cache,
                                             const AotInlineBinding* bindings, uint32_t count) {
    // The generated caller checks epoch before calling. Both success and failure are cached; a failed dependency
    // interprets the caller before any generated instruction (slot analysis can also depend on callee purity).
    // Bytecode and reference definitions are immutable
    // while a context runs. aotDispatch serializes contexts and aotForget invalidates before another context binds.
    cache->valid = false;
    for (uint32_t i = 0; count > i; i++) {
        if (!inlineBindingMatches(ctx, &bindings[i])) {
            cache->epoch = aotEpoch;
            return;
        }
    }
    cache->valid = true;
    cache->epoch = aotEpoch;
}
#endif

// On 3DS, cold wrappers add reset invalidation without changing VM_reset/VM_aotUseTable's input sections or the
// compiler's inline choices in vm.c. All external reset/table calls are linker-wrapped. VM_free's same-unit reset
// is already preceded by aotForget. Other platforms invalidate directly in the two original functions.
// Keep the bridge's allocation diagnostic source lines stable too: changing __LINE__ can add/remove an ARM
// literal-pool word even when new host-only code preprocesses away, shifting subsequent warm functions.
#if defined(__3DS__)
void __real_VM_reset(VMContext* ctx);

INLINE_BIND_COLD void __wrap_VM_reset(VMContext* ctx) {
#if VM_HAS_AOT
    aotEpoch++;
#endif
    __real_VM_reset(ctx);
}

#if defined(CTRPORT_AOT_MODULES)
// Static-table builds never rebind a chapter. Do not retain their otherwise unused VM_aotUseTable.
void __real_VM_aotUseTable(const AotTable* table);
INLINE_BIND_COLD void __wrap_VM_aotUseTable(const AotTable* table) {
#if VM_HAS_AOT
    aotEpoch++;
#endif
    __real_VM_aotUseTable(table);
}
#endif
#endif
