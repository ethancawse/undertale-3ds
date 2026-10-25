// UNDERTALE's Photoshop Flowey: a native body for the three pipe objects' Draw (utflowey:
// per-effect code is allowed when pixels and game state stay identical).
//
// The fight's three pipe objects (pipes 0, 1 and 2 below) each draw two rows of 10-16 pipe segments every frame of
// the fight: per segment two to four sin/cos calls, three or four variable stores and a draw_sprite_ext with
// nine arguments. The generated code does each instruction separately through the operand stack, behind a barrier
// after every one, and their Draws were 19% of the main thread's samples on the Old 3DS sweep.
//
// tools/aot/gmlaot.py binds an entry here only when its bytecode's digest is one of the three (UTW_NATIVE there),
// through a late wrapper (aotu_<k>) that passes which object it is, the variables' IDs (by name), draw_sprite_ext's
// function index, the image_blend write's operand and the four draw calls' return addresses. The body does what the
// generated function does for the common case, flash neither 1 nor 2 and global.debug != 1, in bytecode order:
// every double operation rounds once, as the interpreter's handlers do (this file is compiled without fast math, as
// aot_typed.c is), every variable ends with the value and tags the generated code leaves, and each draw gets the
// arguments aotCallBuiltin would pass, in the same slots. Anything else (a variable missing, holding something other
// than a number, owning a reference; flash 1 or 2, or debug 1; no room on the stack) runs the generated function
// before anything has changed.
//
// sin and cos come from a small table keyed on the argument's exact bits, so a hit returns exactly what the call
// would. Most arguments repeat every frame ((i - 45) / 3 and the like) and the rest repeat across the rows and the
// three objects within a frame.
//
// All code and constants are late (.text.late.utflowey3, after aot_late.o in platform/3ds/late_reserve.ld) and the
// table is in .bss.late.utflowey3, so nothing that existed moves. A build without WAD 16 (DELTARUNE's) has none of it.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)

#include "aot/vm_aot.h"
#include "instance.h"
#include "real_type.h"
#include "rvalue.h"
#include "vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__arm__) && defined(__3DS__)
#if defined(__FAST_MATH__)
#error "z_utflowey.c must be compiled with -fno-fast-math -ffp-contract=off (platform/3ds/CMakeLists.txt)"
#endif
#define UTW_LATE __attribute__((noinline, section(".text.late.utflowey3")))
#define UTW_RODATA __attribute__((section(".rodata.utflowey3_in_text")))
#define UTW_BSS __attribute__((section(".bss.late.utflowey3")))
#else
#define UTW_LATE __attribute__((noinline))
#define UTW_RODATA
#define UTW_BSS
#endif

RValue UtFlowey_pipeDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) UTW_LATE;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS) || defined(ENABLE_VM_GML_PROFILER) || defined(__FAST_MATH__)
// Tracing, stress and GML profiler builds run the generated code itself (it logs, tests or times each builtin call),
// and so does any build with fast math (the non-3DS engine's Release flags): the body is exact only without it.
RValue UtFlowey_pipeDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) { (void) k; (void) a; return generated(ctx); }
#else

// The operands gmlaot.py passes (a[...]): which pipe, the variables' IDs (-1 for those the pipe doesn't have),
// draw_sprite_ext's function index, the Pop.v.v operand of image_blend's write at 0048 and the address after each
// draw_sprite_ext call (row 0's normal and grey draws, then row 1's).
enum {
    UTW_VARIANT, UTW_FROZEN, UTW_SINER, UTW_FLASH, UTW_BLEND2, UTW_I, UTW_OFFX, UTW_OFFY, UTW_OFFS, UTW_OFFX2, UTW_OFFY2,
    UTW_OFFS2, UTW_OP, UTW_SOUL, UTW_DEBUG, UTW_OFFV, UTW_OFFV2, UTW_MF1, UTW_MF2, UTW_MF3, UTW_DRAW, UTW_BLENDREF,
    UTW_CALL0, UTW_COUNT = UTW_CALL0 + 4
};

// The three pipes (UTW_VARIANT 0, 1 and 2, by digest in gmlaot.py). Per row r (0 the first loop, 1 the second):
//     offx = x +- sin((i - xa) / 3) * xm [pipe 0: -+ i * 2] [row 1: + op]
//     offy = y + cos((i - ya) / yd) * ym + cos((i + siner) / 6) * 8
//     offs = sin((siner + i * 4) / 6) * 2; offs = offs > 1 ? (offs - 1) / 3 : 0
//     [pipe 2: offv = the rotation, from i and (row 1) the magic factors]
//     if (global.soul_rescue < soul[r]) draw_sprite_ext(sprite_index, image_index, offx, offy, xs, ys, rot,
//         <image_blend or blend2>, 1)
//     else <grey: the same with 8421504, other scales, and for pipe 0 offy's wave * 4 and offs = 0>
typedef struct {
    uint8_t n, xa, xm, ya, ym;
    uint8_t soul[2];
    int16_t rotM[2], rotK[2];   // pipes 0 and 1: rot = i * rotM + rotK
    uint64_t ydBits;            // yd as a double's bits
} UtwPipe;

static const UtwPipe utwPipes[3] UTW_RODATA = {
    { 10, 45, 60, 45, 50, { 1, 2 }, { 20, -20 }, { 30, 150 }, 0x4008000000000000ULL },   // 3
    { 16, 34, 45, 34, 40, { 5, 6 }, { 20, -20 }, { 50, -50 }, 0x4008000000000000ULL },   // 3
    { 13, 49, 85, 3, 40, { 3, 4 }, { 0, 0 }, { 0, 0 }, 0x400199999999999AULL },          // 2.2
};

static inline GMLReal utwBits(uint64_t bits) {
    GMLReal v;
    memcpy(&v, &bits, sizeof v);
    return v;
}

// sin and cos through a direct-mapped table keyed on the argument's bits (tag = bits ^ UTW_TAG, so a zeroed entry
// stands for one NaN, and NaNs never look up). GCC may merge sin(x) and
// cos(x) of one argument in one function into sincos, which can round differently: the one lookup function calls
// whichever it is passed.
typedef struct {
    uint64_t tag;
    GMLReal v;
} UtwTrig;
#define UTW_TAG 0x7FF4A5A5A5A5A5A5ULL
#define UTW_TRIG_BITS 7
static UtwTrig utwSinTab[1u << UTW_TRIG_BITS] UTW_BSS;
static UtwTrig utwCosTab[1u << UTW_TRIG_BITS] UTW_BSS;

// The table's entry for bits, f(x) on a miss. f is passed in (sin or cos), so no function calls both.
static GMLReal utwTrig(UtwTrig* tab, GMLReal (*f)(GMLReal), GMLReal x) UTW_LATE;
static GMLReal utwTrig(UtwTrig* tab, GMLReal (*f)(GMLReal), GMLReal x) {
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    if (x != x) return f(x);
    uint32_t h = ((uint32_t) (bits >> 32) ^ (uint32_t) bits) * 0x9E3779B1u;
    UtwTrig* e = &tab[h >> (32 - UTW_TRIG_BITS)];
    if (e->tag == (bits ^ UTW_TAG)) return e->v;
    GMLReal v = f(x);
    e->tag = bits ^ UTW_TAG;
    e->v = v;
    return v;
}
#define utwSin(x) utwTrig(utwSinTab, GMLReal_sin, (x))
#define utwCos(x) utwTrig(utwCosTab, GMLReal_cos, (x))

static RValue* utwSlot(Instance* inst, int32_t varID) UTW_LATE;
static RValue* utwSlot(Instance* inst, int32_t varID) {
    return IntRValueHashMap_findSlot(&inst->selfVars, varID);
}

// A real or int32 with its tags as a push or handler leaves it (AOT_SET_REAL / AOT_SET_INT).
static inline void utwReal(RValue* s, GMLReal v, uint8_t g) {
    AOT_SET_REAL(s, v, g);
}
static inline void utwInt(RValue* s, int32_t v, uint8_t g) {
    AOT_SET_INT(s, v, g);
}

// A plain number's value, and whether a constant comparison (Cmp of the number against kv, aotCmpFastK) holds.
static inline GMLReal utwNum(const RValue* v) {
    return v->type == RVALUE_INT32 ? (GMLReal) v->int32 : v->real;
}
static inline bool utwCmp(GMLReal v, GMLReal kv, uint8_t kind) {
    return vmCmpHolds(vmCmpOrder(v - kv), kind) != 0;
}

// What a store of the stack top leaves in a variable: a real with gmlStackType g (5 after arithmetic, 0 for an int
// push coerced by Pop.v.i), nothing owned, no asset tag. The variables hold plain numbers (a precondition), so the
// store frees nothing.
static inline void utwStore(RValue* p, GMLReal v, uint8_t g) {
    utwReal(p, v, g);
}

// One draw_sprite_ext as AOT_CALL_BUILTIN makes it, the arguments in argument order in S[0..8] (where aotCallBuiltin
// leaves them), the stack top above them and ctx->ip after the call.
static void utwDraw(VMContext* ctx, int32_t B, BuiltinFunc draw, uint32_t ipAfter) UTW_LATE;
static void utwDraw(VMContext* ctx, int32_t B, BuiltinFunc draw, uint32_t ipAfter) {
    RValue* S = &ctx->stack.slots[B];
    ctx->stack.top = B + 9;
    ctx->ip = ipAfter;
    RValue r = draw(ctx, S, 9);
    if (r.ownsReference) RValue_free(&r);
    ctx->stack.top = B;
}

RValue UtFlowey_pipeDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* a) {
    // Preconditions, before anything changes.
    Instance* inst = (Instance*) ctx->currentInstance;
    const int32_t variant = a[UTW_VARIANT];
    if (inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX || (uint32_t) variant > 2) return generated(ctx);
    const int32_t B = ctx->stack.top;
    if (B < 0 || B + 9 > VM_STACK_SIZE) return generated(ctx);
    BuiltinFunc draw = (uint32_t) a[UTW_DRAW] < ctx->funcCallCacheCount ? (BuiltinFunc) ctx->funcCallCache[a[UTW_DRAW]].builtin : nullptr;
    if (draw == nullptr) return generated(ctx);
    // p[n]: the variable a[n] names (self, or global for soul_rescue and debug), nullptr for one the pipe hasn't.
    RValue* p[UTW_MF3 + 1];
    for (int n = UTW_FROZEN; n <= UTW_MF3; n++) {
        if (a[n] < 0) {
            p[n] = nullptr;
            continue;
        }
        p[n] = n == UTW_SOUL || n == UTW_DEBUG ? vmScopeVarSlot(ctx, INSTANCE_GLOBAL, a[n]) : utwSlot(inst, a[n]);
        // Each must be there and hold a plain number without an asset tag (what it reads as, and the tags the
        // arithmetic carries on), and own nothing (a store frees nothing).
        if (p[n] == nullptr || !vmIsPlainNumber(p[n]->type) || p[n]->assetRefType != 0 || p[n]->ownsReference)
            return generated(ctx);
    }
    if (p[UTW_FROZEN] == nullptr || p[UTW_SINER] == nullptr || p[UTW_FLASH] == nullptr || p[UTW_BLEND2] == nullptr ||
        p[UTW_I] == nullptr || p[UTW_OFFX] == nullptr || p[UTW_OFFY] == nullptr || p[UTW_OFFS] == nullptr ||
        p[UTW_OFFX2] == nullptr || p[UTW_OFFY2] == nullptr || p[UTW_OFFS2] == nullptr || p[UTW_OP] == nullptr ||
        p[UTW_SOUL] == nullptr || (variant != 0 && p[UTW_DEBUG] == nullptr) ||
        (variant == 2 && (p[UTW_OFFV] == nullptr || p[UTW_OFFV2] == nullptr || p[UTW_MF1] == nullptr ||
                          p[UTW_MF2] == nullptr || p[UTW_MF3] == nullptr)))
        return generated(ctx);
    // siner is a real: siner + 1 is then a real (an int32 would stay one).
    if (p[UTW_SINER]->type != RVALUE_REAL) return generated(ctx);
    // 002C-003C: flash == 0 writes image_blend and blend2 (below); flash 1 and 2 draw ellipses and are left to the
    // generated code; any other value skips all three blocks (the fight runs with none of 0, 1 and 2: Azahar counter
    // build, Flowey sweep). Nothing writes flash before those tests. Pipes 1 and 2 (02BC-02CC): global.debug != 1
    // (the debug keys, left likewise).
    const GMLReal flash = utwNum(p[UTW_FLASH]);
    const bool flash0 = utwCmp(flash, (GMLReal) 0, CMP_EQ);
    if (!flash0 && (utwCmp(flash, (GMLReal) 1, CMP_EQ) || utwCmp(flash, (GMLReal) 2, CMP_EQ))) return generated(ctx);
    // Row 1's colour is blend2 pushed as it is, which the body passes as a real (S[7] below): it must be one unless
    // flash == 0 writes it (as a real) first.
    if (!flash0 && p[UTW_BLEND2]->type != RVALUE_REAL) return generated(ctx);
    if (variant != 0 && utwCmp(utwNum(p[UTW_DEBUG]), (GMLReal) 1, CMP_EQ)) return generated(ctx);

    const UtwPipe* pipe = &utwPipes[variant];
    RValue* S = &ctx->stack.slots[B];

    // 0000-0024: if (frozen == 0) siner += 1.
    if (utwCmp(utwNum(p[UTW_FROZEN]), (GMLReal) 0, CMP_EQ)) utwStore(p[UTW_SINER], p[UTW_SINER]->real + (GMLReal) 1, GML_TYPE_VARIABLE);

    // 0040-0058, when flash == 0: image_blend = 16777215 (the generated code's site); blend2 = 16777215 (Pop.v.i: a
    // real, gml 0).
    if (flash0) {
        utwInt(&S[0], 16777215, GML_TYPE_INT32);
        uint32_t ip = aotSitePopVar(ctx, B + 1, (uint32_t) (0x48 >> 2) | ((uint32_t) GML_TYPE_INT32 << 28), (uint32_t) a[UTW_BLENDREF]);
        if (ip != 0) return aotDeopt(ctx, k, ip);
        ctx->stack.top = B;
        utwStore(p[UTW_BLEND2], (GMLReal) 16777215, GML_TYPE_DOUBLE);
    }

    const GMLReal siner = p[UTW_SINER]->real;
    const GMLReal soul = utwNum(p[UTW_SOUL]);
    const GMLReal op = utwNum(p[UTW_OP]);
    const GMLReal yd = utwBits(pipe->ydBits);
    for (int row = 0; row < 2; row++) {
        RValue* pOffx = p[row == 0 ? UTW_OFFX : UTW_OFFX2];
        RValue* pOffy = p[row == 0 ? UTW_OFFY : UTW_OFFY2];
        RValue* pOffs = p[row == 0 ? UTW_OFFS : UTW_OFFS2];
        RValue* pOffv = p[row == 0 ? UTW_OFFV : UTW_OFFV2];
        // i = 0 (Pop.v.i: a real, gml 0); while (i < n).
        GMLReal i = 0;
        utwStore(p[UTW_I], i, GML_TYPE_DOUBLE);
        while (utwCmp(i, (GMLReal) pipe->n, CMP_LT)) {
            const GMLReal x = (GMLReal) inst->x, y = (GMLReal) inst->y;
            // offx: row 0 x + sin((i - xa) / 3) * xm [pipe 0: - i * 2]; row 1 x - sin(...) * xm [pipe 0: + i * 2] + op.
            const GMLReal sx = utwSin((i - (GMLReal) pipe->xa) / (GMLReal) 3) * (GMLReal) pipe->xm;
            GMLReal offx = row == 0 ? x + sx : x - sx;
            if (variant == 0) offx = row == 0 ? offx - i * (GMLReal) 2 : offx + i * (GMLReal) 2;
            if (row == 1) offx = offx + op;
            utwStore(pOffx, offx, GML_TYPE_VARIABLE);
            // offy = y + cos((i - ya) / yd) * ym + cos((i + siner) / 6) * 8.
            const GMLReal cy = utwCos((i - (GMLReal) pipe->ya) / yd) * (GMLReal) pipe->ym;
            const GMLReal wave = utwCos((i + siner) / (GMLReal) 6);
            GMLReal offy = (y + cy) + wave * (GMLReal) 8;
            utwStore(pOffy, offy, GML_TYPE_VARIABLE);
            // offs = sin((siner + i * 4) / 6) * 2; if (offs > 1) offs = (offs - 1) / 3; else offs = 0.
            GMLReal offs = utwSin((siner + i * (GMLReal) 4) / (GMLReal) 6) * (GMLReal) 2;
            if (utwCmp(offs, (GMLReal) 1, CMP_GT)) {
                offs = (offs - (GMLReal) 1) / (GMLReal) 3;
                utwStore(pOffs, offs, GML_TYPE_VARIABLE);
            } else {
                offs = 0;
                utwStore(pOffs, offs, GML_TYPE_DOUBLE);
            }
            // The rotation: pipes 0 and 1 i * rotM + rotK; pipe 2 offv, row 0 i * -12 - 330, i * -20 - 310 past 5,
            // i * -22 - 310 past 7; row 1 i * 12 + magicfactor, i * 30 + magicfactor2, i * 20 + magicfactor3.
            GMLReal rot;
            if (variant != 2) {
                rot = i * (GMLReal) pipe->rotM[row] + (GMLReal) pipe->rotK[row];
            } else {
                rot = row == 0 ? i * (GMLReal) -12 - (GMLReal) 330 : i * (GMLReal) 12 + utwNum(p[UTW_MF1]);
                utwStore(pOffv, rot, GML_TYPE_VARIABLE);
                if (utwCmp(i, (GMLReal) 5, CMP_GT)) {
                    rot = row == 0 ? i * (GMLReal) -20 - (GMLReal) 310 : i * (GMLReal) 30 + utwNum(p[UTW_MF2]);
                    utwStore(pOffv, rot, GML_TYPE_VARIABLE);
                }
                if (utwCmp(i, (GMLReal) 7, CMP_GT)) {
                    rot = row == 0 ? i * (GMLReal) -22 - (GMLReal) 310 : i * (GMLReal) 20 + utwNum(p[UTW_MF3]);
                    utwStore(pOffv, rot, GML_TYPE_VARIABLE);
                }
            }
            // The arguments that don't depend on the branch: sprite_index, image_index, (offx, offy below), rot, alpha.
            utwReal(&S[0], (GMLReal) inst->spriteIndex, GML_TYPE_VARIABLE);
            utwReal(&S[1], (GMLReal) inst->imageIndex, GML_TYPE_VARIABLE);
            utwReal(&S[2], offx, GML_TYPE_VARIABLE);
            utwReal(&S[6], rot, GML_TYPE_VARIABLE);
            utwInt(&S[8], 1, GML_TYPE_VARIABLE);
            int call = row * 2;
            if (utwCmp(soul, (GMLReal) pipe->soul[row], CMP_LT)) {
                // xs, ys: 1 + offs (an int push plus a real: gml 2); pipe 2 ys (1 + offs) - sin(i / 5) * 0.3 and row
                // 1's xs -1 - offs. The colour: image_blend (row 0) or blend2 (row 1).
                GMLReal xs = (GMLReal) 1 + offs, ys = xs;
                uint8_t ysType = GML_TYPE_INT32;
                if (variant == 2) {
                    if (row == 1) xs = (GMLReal) -1 - offs;
                    ys = ys - utwSin(i / (GMLReal) 5) * utwBits(0x3FD3333333333333ULL);
                    ysType = GML_TYPE_VARIABLE;
                }
                utwReal(&S[3], offy, GML_TYPE_VARIABLE);
                utwReal(&S[4], xs, GML_TYPE_INT32);
                utwReal(&S[5], ys, ysType);
                utwReal(&S[7], row == 0 ? (GMLReal) inst->imageBlend : p[UTW_BLEND2]->real, GML_TYPE_VARIABLE);
            } else {
                call++;
                if (variant == 0) {
                    // offy = y + cos((i - 45) / 3) * 50 + cos((i + siner) / 6) * 4; offs = 0; xs = ys = 1 + offs.
                    offy = (y + cy) + wave * (GMLReal) 4;
                    utwStore(pOffy, offy, GML_TYPE_VARIABLE);
                    offs = 0;
                    utwStore(pOffs, offs, GML_TYPE_DOUBLE);
                    utwReal(&S[4], (GMLReal) 1 + offs, GML_TYPE_INT32);
                    utwReal(&S[5], (GMLReal) 1 + offs, GML_TYPE_INT32);
                } else {
                    // xs = 1 (pipe 2's row 1: -1), ys = 1: int pushes converted to Variable.
                    utwInt(&S[4], variant == 2 && row == 1 ? -1 : 1, GML_TYPE_VARIABLE);
                    utwInt(&S[5], 1, GML_TYPE_VARIABLE);
                }
                utwReal(&S[3], offy, GML_TYPE_VARIABLE);
                utwInt(&S[7], 8421504, GML_TYPE_VARIABLE);
            }
            utwDraw(ctx, B, draw, (uint32_t) a[UTW_CALL0 + call]);
            // i += 1.
            i = i + (GMLReal) 1;
            utwStore(p[UTW_I], i, GML_TYPE_VARIABLE);
        }
    }
    return aotx_scriptFallthroughReturnValue(ctx);
}
#endif

#endif // ENABLE_WAD16
