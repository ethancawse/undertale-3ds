// Constant bindings only: executable gate code lives in z_bonegate.c so GCC's temporary numbering and
// scheduling in this original renderer translation unit remain unchanged.
#if defined(ENABLE_WAD16) && !defined(CTRPORT_HALF_CENSUS) && !defined(CTRPORT_ASG_PROBE)
#include "ctr_bonegate.h"
// GCC must not fold these new references into the old unit's data/call graph: doing so grows bonePieceDx,
// reorders ctrVtable's BSS and changes warm renderer bodies. Assembler-name views name the existing storage and
// functions without adding compiler-visible uses of those private definitions (the renderer uses this convention
// for utLineOffset's callers too). No aliases add ELF symbols or storage.
extern RendererVtable boneGateVtable __asm__("ctrVtable");
extern int16_t boneGateObjects[BONEPULSE_OBJECTS] __asm__("bonePieceObj");
extern const char boneGateNames[BONEPULSE_OBJECTS][17] __asm__("bonePieceNames");
#define BG_ALIAS(fn) extern __typeof__(fn) bg_##fn __asm__(#fn);
#define BG_ALIASES(suffix) BG_ALIAS(bp##suffix) BG_ALIAS(bpSkip##suffix) BG_ALIAS(e5x##suffix) BG_ALIAS(skip##suffix)
BG_ALIASES(DrawSprite)
BG_ALIASES(DrawSpritePart)
BG_ALIASES(DrawSpritePartColor)
BG_ALIASES(DrawRectangle)
BG_ALIASES(DrawLine)
BG_ALIASES(DrawLineColor)
#undef BG_ALIASES
#undef BG_ALIAS
const BoneGateBindings Bonegate_bindings __attribute__((section(".rodata.bonegate_in_text"))) = {
    &boneGateVtable, boneGateObjects, boneGateNames, offsetof(CtrRenderer, skip.wrapped), {
#define BG_ROW(field, suffix)                                                                                   \
    { offsetof(RendererVtable, field),                                                                          \
      { { (BoneGateDraw) bg_bp##suffix, (BoneGateDraw) bg_bpSkip##suffix },                                       \
        { (BoneGateDraw) bg_e5x##suffix, (BoneGateDraw) ABQ(skip##suffix) } } }
    BG_ROW(drawSprite, DrawSprite),
    BG_ROW(drawSpritePart, DrawSpritePart),
    BG_ROW(drawSpritePartColor, DrawSpritePartColor),
    BG_ROW(drawRectangle, DrawRectangle),
    BG_ROW(drawLine, DrawLine),
    BG_ROW(drawLineColor, DrawLineColor),
#undef BG_ROW
    },
};
#endif
