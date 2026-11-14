// Native-M writer adapter, dev default on. The checked store is quizdraw A's
// public-engine wfFlag (862a69846), with aliases into the unchanged writer.
// The admission walk remains read-only; the run calls the original real and
// array-store helpers in order. No cross-call cache or allocation is added.
#if defined(CTRPORT_MEXDLG_NATIVE_M) && defined(ENABLE_WAD16) && defined(CTRPORT_TARGET_O3DS)
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "data_win.h"
#include "instance.h"
#include "vm.h"
#include "vm_fastpath.h"
#include "aot/vm_aot.h"

extern int32_t Mexdlg_writerCode;
void Mexdlg_writerCall(VMContext* ctx, RValue* d, int k, RValue* args, int32_t argc);
enum { F_REAL = 6 };

// This name bypasses the linker's older .text.late.* catch-all. Its explicit
// input is appended after .rodata_layoutd by the late-order registries.
__attribute__((noinline, used, section(".text_mexdlg")))
bool Mexdlg_flag(VMContext* ctx, char digit, bool run) {
    DataWin* dw = ctx->dataWin;
    if (IS_WAD17_OR_HIGHER(ctx) || 0 > Mexdlg_writerCode || (uint32_t) Mexdlg_writerCode >= dw->code.count) return false;
    CodeEntry* code = &dw->code.entries[Mexdlg_writerCode];
    if (code->bytecode == nullptr || code->offset != 0 || 0x03BC > code->length) return false;
    uint32_t ref;
    memcpy(&ref, code->bytecode + 0x03B8, sizeof(ref));
    if ((ref & 0x07FFFFFF) >= dw->vari.variableCount) return false;
    int32_t varID = dw->vari.variables[ref & 0x07FFFFFF].varID;
    RValue* flag = 0 > varID ? nullptr : vmScopeVarSlot(ctx, INSTANCE_GLOBAL, varID);
    if (flag == nullptr || flag->type != RVALUE_ARRAY || flag->array == nullptr) return false;
    GMLArray* arr = flag->array;
    if (GMLArray_slot(arr, 20) == nullptr || (arr->refCount > 1 && arr->owner != (const void*) flag)) return false;
    if (run) {
        // the character string_char_at gives (a shared one-letter string), real of it, then [value, -5, 20] popped
        RValue arg = {.string = RValue_asciiStrings[(uint8_t) digit], .type = RVALUE_STRING, .gmlStackType = GML_TYPE_VARIABLE};
        RValue v, ix = {.int32 = 20, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT16};
        Mexdlg_writerCall(ctx, &v, F_REAL, &arg, 1);
        (void) aotx_popArrayElementIntoSlot(ctx, flag, &ix, &v, GML_TYPE_VARIABLE, false);
    }
    return true;
}
#endif
