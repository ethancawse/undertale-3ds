/* Included at the end of vm_aot_bridge.inc, after its original definitions.
 * No player bytecode or fixed resource IDs: gmlaot selects an audited builtin
 * call in one effect and its normal entry digest binds the player's bytecode.
 * Reading arguments in their existing reverse order saves eight RValue moves,
 * the indirect builtin call and ownership cleanup. The renderer stays live. */
#if defined(__3DS__) && defined(ENABLE_WAD16) && defined(ENABLE_UTOLD_LAVA_DISPATCH) && !defined(UTOLD_LAVA_REFERENCE)
#include "renderer.h"
#include "vm_aot_lava.h"
extern BuiltinFunc const Utold_lavaPartBuiltin;

__attribute__((noinline, section(".text.late.utlava")))
void Utold_lavaPartCall(VMContext* ctx, uint32_t instr,
                      const uint8_t* extraData, uint32_t funcIndex) {
#if !defined(ENABLE_VM_TRACING) && !defined(ENABLE_VM_GML_PROFILER)
    if ((instr & 0xFFFF) == 8 && funcIndex < ctx->funcCallCacheCount &&
            ctx->stack.top >= 8 &&
            ctx->funcCallCache[funcIndex].builtin == (void*) Utold_lavaPartBuiltin) {
        RValue* args = &ctx->stack.slots[ctx->stack.top - 8];
        /* Guard the whole operation before touching the stack or drawing.
         * Other types, references, an unresolved/replaced builtin, tracing
         * and profiling retain the original dispatch and cleanup path. */
        for (int i = 0; i < 8; ++i) {
            if (args[i].ownsReference ||
                    (args[i].type != RVALUE_REAL && args[i].type != RVALUE_INT32))
                goto fallback;
        }
        aotBuiltinCalls++;
        if (ctx->runner->renderer != nullptr) {
            int32_t spriteIndex = RValue_toInt32(args[7]);
            int32_t subimg = RValue_toInt32(args[6]);
            float left = (float) RValue_toReal(args[5]);
            float top = (float) RValue_toReal(args[4]);
            float width = (float) RValue_toReal(args[3]);
            float height = (float) RValue_toReal(args[2]);
            float x = (float) RValue_toReal(args[1]);
            float y = (float) RValue_toReal(args[0]);
            if (subimg < 0 && ctx->currentInstance != nullptr)
                subimg = (int32_t) ctx->currentInstance->imageIndex;
            Renderer_drawSpritePart(ctx->runner->renderer, spriteIndex, subimg,
                                    left, top, width, height, x, y);
        }
        ctx->stack.top -= 8;
        stackPushTyped(ctx, RValue_makeUndefined(), GML_TYPE_VARIABLE);
        return;
    }
fallback:
#endif
    aotCallBuiltin(ctx, instr, extraData, funcIndex);
}
#endif
