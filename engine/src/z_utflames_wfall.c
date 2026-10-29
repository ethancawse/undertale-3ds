// UNDERTALE's falling-water foreground (Waterfall): a native body for its Draw event. Per-effect
// code like this is used only where pixels and game state stay identical to the PC game.
//
// tools/aot/gmlaot.py binds the event here only when its bytecode is exactly UTF_NATIVE's shape for it (through the
// wrapper aotu_<k>, in .text.late.utflames.wfall like this file), as z_utflames.c's bodies are bound. A file of its
// own, apart from z_utflames.c: any addition there changes where GCC puts that file's functions. All of it is late
// (late_reserve.ld: after z_utflames_waver.c's code), so nothing that existed moves; its constants are unmerged
// (-fno-merge-constants, CMakeLists.txt). A build without WAD 16 (DELTARUNE's) has none of it.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "instance.h"
#include "matrix_math.h"
#include "renderer.h"
#include "rvalue.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdbool.h>
#include <stdint.h>

#if defined(__arm__) && defined(__3DS__)
#define UTF_LATE_F __attribute__((noinline, section(".text.late.utflames.wfall")))
#else
#define UTF_LATE_F __attribute__((noinline))
#endif

// 1: rows wholly outside the view are not handed to the renderer (which would cull them itself): the same pixels, a
// shorter draw list (another draw hash). 0: every row is drawn, as the game does. On (10-24): state and every
// capture are byte-identical with it off.
#ifndef UTF_WFALL_CULL
#define UTF_WFALL_CULL 1
#endif

RValue UtFlames_wfallDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTF_LATE_F;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the generated code itself.
RValue UtFlames_wfallDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
#else
static RValue* utfFindF(Instance* inst, int32_t varID) UTF_LATE_F;
static RValue* utfFindF(Instance* inst, int32_t varID) {
    return IntRValueHashMap_findSlot(&inst->selfVars, varID);
}

// The variable a Push.v or Pop.v.v operand names (its index in the low 27 bits, as vm.c reads it).
// gmlaot only emits operands whose index is below the VARI count.
static inline Variable* utfVarF(VMContext* ctx, int32_t ref) {
    return &ctx->dataWin->vari.variables[(uint32_t) ref & 0x07FFFFFFu];
}

// A builtin variable's value as the interpreter reads it on self (resolveVariableRead: VMBuiltins_getVariable), with
// the array index (0) or -1.
static void utfGetBuiltin(VMContext* ctx, Instance* inst, int32_t ref, int32_t index, RValue* out) UTF_LATE_F;
static void utfGetBuiltin(VMContext* ctx, Instance* inst, int32_t ref, int32_t index, RValue* out) {
    Variable* v = utfVarF(ctx, ref);
    RValue r = VMBuiltins_getVariable(ctx, inst, v->builtinVarId, v->name, index);
    *out = r;
    out->gmlStackType = GML_TYPE_VARIABLE;
}

// vmArithNumbers and vmCmpNumbers once each (out of line: the call is a barrier, as AOT_OPAQUE is after each step in
// the generated code).
static void utfArith(RValue* x, const RValue* y, int op) UTF_LATE_F;
static void utfArith(RValue* x, const RValue* y, int op) {
    vmArithNumbers(x, y, op, GML_TYPE_VARIABLE);
}
static int utfCmp(const RValue* x, const RValue* y, uint8_t kind) UTF_LATE_F;
static int utfCmp(const RValue* x, const RValue* y, uint8_t kind) {
    return vmCmpNumbers(x, y, kind);
}

#if UTF_WFALL_CULL
// The view in world space, from the game's projection, when it is a plain 2D one: [x0, x1) x [y0, y1), widened by a
// tenth each side (the renderer's own test widens it for depth mode's right eye and rounding by less).
typedef struct { float x0, y0, x1, y1; } UtfView;
static bool utfViewRect(Renderer* renderer, UtfView* out) UTF_LATE_F;
static bool utfViewRect(Renderer* renderer, UtfView* out) {
    const Matrix4f* m = &renderer->gmlMatrices[MATRIX_WORLD_VIEW_PROJECTION];
    if (!Matrix4f_isAffine2D(m)) return false;
    float a = m->m[0], b = m->m[4], c = m->m[1], d = m->m[5];
    float det = a * d - b * c;
    if (!(det > 1e-12f || det < -1e-12f)) return false;
    float vx0 = 1e30f, vy0 = 1e30f, vx1 = -1e30f, vy1 = -1e30f;
    for (int32_t i = 0; 4 > i; i++) {
        float cx = (i & 1) ? 1.0f : -1.0f, cy = (i & 2) ? 1.0f : -1.0f;
        float u = cx - m->m[12], v = cy - m->m[13];
        float wx = (d * u - b * v) / det, wy = (a * v - c * u) / det;
        if (vx0 > wx) vx0 = wx;
        if (vy0 > wy) vy0 = wy;
        if (wx > vx1) vx1 = wx;
        if (wy > vy1) vy1 = wy;
    }
    float sx = (vx1 - vx0) * 0.1f, sy = (vy1 - vy0) * 0.1f;
    *out = (UtfView) { vx0 - sx, vy0 - sy, vx1 + sx, vy1 + sy };
    return true;
}
#endif

// Draw of a falling-water foreground strip (UTF_NATIVE's waterfall shape):
//     v0 += 3;
//     for (v1 = 0; v1 < 20; v1 += 1) draw_sprite_ext(sprite_index, 0, x, -210 + y + v1 * 30 + v0, 2, 2, 0, c_white, 0.2);
//     if (v0 > 180) v0 -= 180;
//     v2 = 0;
//     if (view_xview[0] > 0 && view_xview[0] < <the room's width> - view_wview[0]) v2 = view_xview[0];
//     if (view_xview[0] >= <the room's width> - view_wview[0]) v2 = <the room's width> - view_wview[0];
//     x = xstart - v2 * 0.5;
// a: v0, v1 and v2's IDs, draw_sprite_ext's function index, the operands of view_xview (0x11C), the room's width (0x140),
// view_wview (0x150), xstart (0x1DC) and x (0x200). Eleven run a frame in the falling-water rooms, 220 rows. Each value
// is worked out as the generated code works it (the same helpers, a barrier after each step); the rows go to the
// renderer with the arguments draw_sprite_ext converts them to.
RValue UtFlames_wfallDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    Instance* inst = (Instance*) ctx->currentInstance;
    if (inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX) return generated(ctx);
    Runner* runner = ctx->runner;
    Renderer* renderer = runner->renderer;
    RValue* p0 = utfFindF(inst, a[0]);
    RValue* p1 = utfFindF(inst, a[1]);
    RValue* p2 = utfFindF(inst, a[2]);
    if (renderer == nullptr || p0 == nullptr || p1 == nullptr || p2 == nullptr || p0->ownsReference || p1->ownsReference ||
        p2->ownsReference || !vmIsPlainNumber(p0->type) || (uint32_t) a[3] >= ctx->funcCallCacheCount ||
        ctx->funcCallCache[a[3]].builtin == nullptr)
        return generated(ctx);
    // draw_sprite_ext(sprite_index, 0, ...) draws the sprite's first frame (Renderer_drawSpriteExt: none for a sprite
    // that isn't there; a nine-slice sprite, scaled, takes its other path, left to the generated code).
    DataWin* dw = renderer->dataWin;
    int32_t spriteIndex = inst->spriteIndex;
    int32_t tpagIndex = Renderer_resolveTPAGIndex(dw, spriteIndex, 0);
    if (tpagIndex >= 0 && dw->sprt.sprites[spriteIndex].nineSliceEnabled) return generated(ctx);
    const int32_t B = ctx->stack.top;
    RValue* S = &ctx->stack.slots[B];
    __asm__("" : "+r"(S));
    RValue kv;

    // 0000-0010: v0 += 3.
    vmWeakCopyInto(&S[0], p0);
    AOT_SET_INT(&kv, 3, GML_TYPE_INT16);
    utfArith(&S[0], &kv, VM_ARITH_ADD);
    *p0 = S[0];

    // 0018-001C: v1 = 0 (Pop.v.i: stored as a real).
    AOT_SET_INT(&S[0], 0, GML_TYPE_INT16);
    coerceIntStoreToRealInPlace(&S[0], GML_TYPE_INT32);
    *p1 = S[0];

    // The arguments that stay the same each row.
    float x = inst->x;
    float scale = 2.0f, rot = 0.0f;
    __asm__("" : "+m"(scale), "+m"(rot));
    float alpha = (float) aotBitsDouble(0x3FC999999999999AULL);
    __asm__("" : "+m"(alpha));
    float originX = 0.0f, originY = 0.0f;
    if (tpagIndex >= 0) {
        originX = (float) dw->sprt.sprites[spriteIndex].originX;
        originY = (float) dw->sprt.sprites[spriteIndex].originY;
    }
#if UTF_WFALL_CULL
    // A row's quad spans [y + 2 * (targetY - originY), + 2 * targetHeight) down, x likewise across.
    UtfView view;
    bool cull = utfViewRect(renderer, &view);
    float ry0 = 0.0f, ry1 = 0.0f;
    if (cull && tpagIndex >= 0) {
        TexturePageItem* tpag = &dw->tpag.items[tpagIndex];
        float rx0 = x + 2.0f * ((float) tpag->targetX - originX);
        float rx1 = rx0 + 2.0f * (float) tpag->targetWidth;
        ry0 = 2.0f * ((float) tpag->targetY - originY);
        ry1 = ry0 + 2.0f * (float) tpag->targetHeight;
        if (view.x0 > rx1 || rx0 > view.x1) ry1 = ry0 - 1e30f;   // the whole strip is beside the view
    }
#endif

    // 0024-00D8: the rows.
    for (;;) {
        vmWeakCopyInto(&S[0], p1);
        AOT_SET_INT(&kv, 20, GML_TYPE_INT16);
        int r = utfCmp(&S[0], &kv, CMP_LT);
        if (r < 0) {
            ctx->stack.top = B;
            return aotDeopt(ctx, k, 0x24);
        }
        if (r == 0) break;
        // -210 + y + v1 * 30 + v0
        AOT_SET_INT(&S[0], -210, GML_TYPE_INT16);
        AOT_SET_REAL(&S[1], (GMLReal) inst->y, GML_TYPE_VARIABLE);
        AOT_OPAQUE(1);
        utfArith(&S[0], &S[1], VM_ARITH_ADD);
        vmWeakCopyInto(&S[1], p1);
        AOT_SET_INT(&kv, 30, GML_TYPE_INT16);
        utfArith(&S[1], &kv, VM_ARITH_MUL);
        utfArith(&S[0], &S[1], VM_ARITH_ADD);
        vmWeakCopyInto(&S[1], p0);
        utfArith(&S[0], &S[1], VM_ARITH_ADD);
        float y = (float) AOT_NUM(&S[0]);
#if UTF_WFALL_CULL
        bool draw = !cull || !(view.y0 > y + ry1 || y + ry0 > view.y1);
        if (draw && tpagIndex >= 0)
#else
        if (tpagIndex >= 0)
#endif
            renderer->vtable->drawSprite(renderer, tpagIndex, x, y, originX, originY, scale, scale, rot, 0xFFFFFFu, alpha);
        // v1 += 1
        vmWeakCopyInto(&S[0], p1);
        AOT_SET_INT(&kv, 1, GML_TYPE_INT16);
        utfArith(&S[0], &kv, VM_ARITH_ADD);
        *p1 = S[0];
    }

    // 00DC-0100: if (v0 > 180) v0 -= 180.
    vmWeakCopyInto(&S[0], p0);
    AOT_SET_INT(&kv, 180, GML_TYPE_INT16);
    int r = utfCmp(&S[0], &kv, CMP_GT);
    if (r < 0) {
        ctx->stack.top = B;
        return aotDeopt(ctx, k, 0xDC);
    }
    if (r != 0) {
        utfArith(&S[0], &kv, VM_ARITH_SUB);
        *p0 = S[0];
    }

    // 0108-010C: v2 = 0 (Pop.v.i: stored as a real).
    AOT_SET_INT(&S[0], 0, GML_TYPE_INT16);
    coerceIntStoreToRealInPlace(&S[0], GML_TYPE_INT32);
    *p2 = S[0];

    // 0114-017C: if (view_xview[0] > 0 && view_xview[0] < <the room's width> - view_wview[0]) v2 = view_xview[0];
    // 0184-01D4: if (view_xview[0] >= <the room's width> - view_wview[0]) v2 = <the room's width> - view_wview[0];
    // Every operand is a real (VMBuiltins_getVariable's), so each test is vmCmpNumbers's.
    utfGetBuiltin(ctx, inst, a[4], 0, &S[0]);
    AOT_SET_INT(&kv, 0, GML_TYPE_INT16);
    bool inside = utfCmp(&S[0], &kv, CMP_GT) > 0;
    if (inside) {
        utfGetBuiltin(ctx, inst, a[4], 0, &S[0]);
        utfGetBuiltin(ctx, inst, a[5], -1, &S[1]);
        utfGetBuiltin(ctx, inst, a[6], 0, &S[2]);
        utfArith(&S[1], &S[2], VM_ARITH_SUB);
        inside = utfCmp(&S[0], &S[1], CMP_LT) > 0;
    }
    if (inside) {
        utfGetBuiltin(ctx, inst, a[4], 0, &S[0]);
        *p2 = S[0];
    }
    utfGetBuiltin(ctx, inst, a[4], 0, &S[0]);
    utfGetBuiltin(ctx, inst, a[5], -1, &S[1]);
    utfGetBuiltin(ctx, inst, a[6], 0, &S[2]);
    utfArith(&S[1], &S[2], VM_ARITH_SUB);
    if (utfCmp(&S[0], &S[1], CMP_GTE) > 0) {
        utfGetBuiltin(ctx, inst, a[5], -1, &S[0]);
        utfGetBuiltin(ctx, inst, a[6], 0, &S[1]);
        utfArith(&S[0], &S[1], VM_ARITH_SUB);
        *p2 = S[0];
    }

    // 01DC-0200: x = xstart - v2 * 0.5 (Pop.v.v of a builtin on self: VMBuiltins_setVariable).
    utfGetBuiltin(ctx, inst, a[7], -1, &S[0]);
    vmWeakCopyInto(&S[1], p2);
    AOT_SET_REAL(&kv, (GMLReal) aotBitsDouble(0x3FE0000000000000ULL), GML_TYPE_DOUBLE);
    utfArith(&S[1], &kv, VM_ARITH_MUL);
    utfArith(&S[0], &S[1], VM_ARITH_SUB);
    Variable* xv = utfVarF(ctx, a[8]);
    VMBuiltins_setVariable(ctx, inst, xv->builtinVarId, xv->name, S[0], -1);
    ctx->stack.top = B;
    return aotx_scriptFallthroughReturnValue(ctx);
}
#endif

#endif // ENABLE_WAD16
