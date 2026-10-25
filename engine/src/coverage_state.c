#include "vm.h"
#include "data_win.h"
#include "utils.h"
#include "stdio_compat.h"

// Diagnostic lookup with the original call signature so the dumping caller
// needs no extra local storage or inline recovery path.
static char coverageUnknownName[32] __attribute__((section(".bss.late.coverage")));
static const char coverageUnknownFormat[] __attribute__((section(".rodata.late.coverage"))) = "<var-id:%d>";
__attribute__((noinline, noclone, section(".text.late.coverage")))
char* coverageGlobalName(VMContext* ctx, int32_t id) {
    char* name = VM_getVariableNameByVarId(ctx, id);
    if (name != nullptr) return name;
    // BC16 can assign different IDs to the same name in different scopes.
    repeat(ctx->dataWin->vari.variableCount, i) {
        Variable* var = &ctx->dataWin->vari.variables[i];
        if (var->instanceType == INSTANCE_GLOBAL && var->varID == id && var->name != nullptr) return (char*) var->name;
    }
    snprintf(coverageUnknownName, sizeof(coverageUnknownName), coverageUnknownFormat, (int) id);
    return coverageUnknownName;
}
