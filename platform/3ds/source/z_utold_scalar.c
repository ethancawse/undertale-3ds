#include "vm.h"
#include "runner.h"
#include "renderer.h"

#if defined(__3DS__) && defined(ENABLE_WAD16) && defined(CTRPORT_AOT)
_Static_assert(sizeof(GMLReal) == sizeof(double), "Private scalar probe requires double reals");
extern BuiltinFunc const Utold_scalarPartBuiltin;
__attribute__((noinline, section(".text.late.utoldscalar")))
bool Utold_scalarPartIsBuiltin(BuiltinFunc bound) {
    return bound == Utold_scalarPartBuiltin;
}

__attribute__((noinline, section(".text.late.utoldscalar")))
RValue Utold_scalarPart(VMContext* ctx, GMLReal sprite, GMLReal image,
        GMLReal left_, GMLReal top_, GMLReal width_, GMLReal height_,
        GMLReal x_, GMLReal y_) {
    Runner* runner = ctx->runner;
    if (runner->renderer == nullptr) return RValue_makeUndefined();
    int32_t spriteIndex = (int32_t) sprite;
    int32_t subimg = (int32_t) image;
    float left = (float) left_;
    float top = (float) top_;
    float width = (float) width_;
    float height = (float) height_;
    float x = (float) x_;
    float y = (float) y_;
    if (0 > subimg && ctx->currentInstance != nullptr) {
        subimg = (int32_t) ctx->currentInstance->imageIndex;
    }
    Renderer_drawSpritePart(runner->renderer, spriteIndex, subimg,
                           left, top, width, height, x, y);
    return RValue_makeUndefined();
}
#endif
