// UNDERTALE's waving sprite rows (Undyne's battle, through her two waving-sprite objects): a native body for
// the script they call each Draw (per-effect code like this is used only where pixels and game state
// stay identical). The same way as z_utflames_waver.c's heat shimmer, which this follows step for step.
//
// A file of its own: all of it is late (late_reserve.ld: .text.late.utkill.waver and this file's constants after the
// swarm's), so nothing that existed moves; its constants are unmerged (-fno-merge-constants, CMakeLists.txt). A build
// without WAD 16 (DELTARUNE's) has none of it.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "instance.h"
#include "log.h"
#include "renderer.h"
#include "rvalue.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__arm__) && defined(__3DS__)
#define UTK_LATE_W __attribute__((noinline, section(".text.late.utkill.waver")))
#define UTK_BSS_W __attribute__((section(".bss.late.utswarm")))
#else
#define UTK_LATE_W __attribute__((noinline))
#define UTK_BSS_W
#endif

void UtKill_registerAll(VMContext* ctx) UTK_LATE_W;   // VM_create, through COLFLIP_REGISTER_ALL (vm_builtins.h)

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the interpreter and the generated code: the script stays the game's.
void UtKill_registerAll(VMContext* ctx) { UtFlames_registerAll(ctx); }
#else

// ===[ The waving rows ]===
//
// Each Draw, either of those objects (both have the same code) copies a box of the screen into a sprite and
// draws it back one pixel row at a time, each row moved sideways by a sine: some 50 instructions of the interpreter
// and two builtins a row, ~190-320 rows a frame in Undyne's battle. The script (UT 1.08, 416 bytes):
//     b = argument0; c = argument1; a += 1; i = sprite_height;
//     while (i > 0) { a += 1; if (c > 0) c -= 0.1; if (c < 0) c -= 0.1;
//         draw_sprite_part_ext(sprite_index, image_index, 0, i, sprite_width, 1, x + sin(a / b) * c, y + i, 1, 1,
//                              16777215, image_alpha);
//         i -= 1; }
// (both tests subtract: as written.) utkWaver is a builtin of the script's name, registered before VM_create builds
// the function cache, so every call of the script (interpreted or generated, from either object) comes here. The
// script is found by its bytecode (UTK_WAVER_LENGTH bytes whose digest over the loaded and patched bytes is
// UTK_WAVER_DIGEST), so no game name is written here; a, b, c and i come from its operands. Each value is worked out
// as the interpreter works it (vm_fastpath.h's helpers, one instruction at a time, with the instruction's result type),
// and the draw gets the arguments draw_sprite_part_ext converts them to. Any other case (no instance, arguments that
// aren't plain numbers, a not a plain number, b, c or i holding a reference) runs the script itself.
typedef struct {
    int32_t codeIndex;
    int32_t varA, varB, varC, varI;
} UtkWaver;
static UtkWaver utkWaverInfo UTK_BSS_W;

static const char utkWaverLogOn[] = "utkill: waving rows native\n";

#if defined(__arm__) && defined(__3DS__) && !defined(UTKW_SHADOW)
// The rows go to the renderer in one call (ctr_utkill_waver.inc, at the end of ctr_renderer.c), which draws them as
// draw_sprite_part_ext would one by one, or answers false (the draw-hash wrapper is in, say) and they are drawn here
// one by one. They are all worked out first: drawing changes none of a, c, i or anything they are made from.
#define UTKW_BATCH 1
#define UTKW_MAX_ROWS 640
bool CtrUtk_waverRows(Renderer* renderer, int32_t spriteIndex, int32_t subimg, float left, float width, uint32_t colour,
                      float alpha, int32_t n, const float* tops, const float* xs, const float* ys);
static float utkRowTop[UTKW_MAX_ROWS] UTK_BSS_W, utkRowX[UTKW_MAX_ROWS] UTK_BSS_W, utkRowY[UTKW_MAX_ROWS] UTK_BSS_W;
#endif

#define UTK_WAVER_LENGTH 416u
#define UTK_WAVER_DIGEST 0x903B935Au

static inline Instance* utkSelf(VMContext* ctx) {
    Instance* inst = (Instance*) ctx->currentInstance;
    return inst != nullptr && inst->objectIndex != STRUCT_OBJECT_INDEX ? inst : nullptr;
}

static uint32_t utkDigest(const uint8_t* bytes, uint32_t length) UTK_LATE_W;
static uint32_t utkDigest(const uint8_t* bytes, uint32_t length) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; length > i; i++) h = (h ^ bytes[i]) * 0x01000193u;
    return h;
}

// The varID of the variable the Push.v or Pop.v operand at off names (patched to its index at load).
static int32_t utkOperandVar(VMContext* ctx, const uint8_t* bytes, uint32_t off) UTK_LATE_W;
static int32_t utkOperandVar(VMContext* ctx, const uint8_t* bytes, uint32_t off) {
    uint32_t w = (uint32_t) bytes[off] | (uint32_t) bytes[off + 1] << 8 | (uint32_t) bytes[off + 2] << 16 |
                 (uint32_t) bytes[off + 3] << 24;
    uint32_t index = w & 0x07FFFFFFu;
    if (index >= ctx->dataWin->vari.variableCount) return -1;
    return ctx->dataWin->vari.variables[index].varID;
}

static inline RValue* utkFind(Instance* inst, int32_t varID) {
    return IntRValueHashMap_findSlot(&inst->selfVars, varID);
}

static inline bool utkOwns(Instance* inst, int32_t varID) {
    RValue* p = utkFind(inst, varID);
    return p != nullptr && p->ownsReference;
}

static inline RValue* utkInsert(Instance* inst, int32_t varID) {
    return IntRValueHashMap_getOrInsertUndefinedFast(&inst->selfVars, varID);
}

// sprite_width (axis 0) or sprite_height (axis 1), as VMBuiltins_getVariable reads it.
static inline GMLReal utkSpriteSize(Runner* runner, Instance* inst, int axis) {
    if (inst->spriteIndex >= 0 && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
        Sprite* s = &runner->dataWin->sprt.sprites[inst->spriteIndex];
        return axis == 0 ? (GMLReal) s->width * inst->imageXscale : (GMLReal) s->height * inst->imageYscale;
    }
    return 0.0;
}

#if defined(UTKW_SHADOW)
// Test builds only (never committed on): each call runs the native body with its draws held back, puts a, b, c and i
// back, runs the script with draw_sprite_part_ext's call cache entry swapped for a recorder, then compares every
// draw's converted arguments and the four variables bit for bit. The script's draws are the ones made.
typedef struct {
    int32_t sprite, subimg;
    float left, top, width, height, x, y, xs, ys;
    uint32_t colour;
    float alpha;
} UtkDraw;
#define UTK_SHADOW_MAX 1024
static UtkDraw utkShadowNative[UTK_SHADOW_MAX], utkShadowScript[UTK_SHADOW_MAX];
static int32_t utkShadowNativeCount, utkShadowScriptCount;
static uint32_t utkShadowCalls, utkShadowBad;
static void utkShadowNote(UtkDraw* list, int32_t* count, int32_t sprite, int32_t subimg, float left, float top,
                          float width, float height, float x, float y, float xs, float ys, uint32_t colour, float alpha) {
    if (*count >= UTK_SHADOW_MAX) {
        (*count)++;
        return;
    }
    UtkDraw* d = &list[(*count)++];
    memset(d, 0, sizeof *d);
    d->sprite = sprite;
    d->subimg = subimg;
    d->left = left;
    d->top = top;
    d->width = width;
    d->height = height;
    d->x = x;
    d->y = y;
    d->xs = xs;
    d->ys = ys;
    d->colour = colour;
    d->alpha = alpha;
}
static RValue utkShadowRecorder(VMContext* ctx, RValue* args, int32_t argCount) {
    (void) argCount;
    int32_t subimg = RValue_toInt32(args[1]);
    if (0 > subimg && ctx->currentInstance != nullptr) subimg = (int32_t) ctx->currentInstance->imageIndex;
    utkShadowNote(utkShadowScript, &utkShadowScriptCount, RValue_toInt32(args[0]), subimg, (float) RValue_toReal(args[2]),
                  (float) RValue_toReal(args[3]), (float) RValue_toReal(args[4]), (float) RValue_toReal(args[5]),
                  (float) RValue_toReal(args[6]), (float) RValue_toReal(args[7]), (float) RValue_toReal(args[8]),
                  (float) RValue_toReal(args[9]), RValue_toColour(args[10]), (float) RValue_toReal(args[11]));
    Runner* runner = ctx->runner;
    if (runner->renderer != nullptr) {
        UtkDraw* d = &utkShadowScript[utkShadowScriptCount - 1 < UTK_SHADOW_MAX ? utkShadowScriptCount - 1 : 0];
        Renderer_drawSpritePartExt(runner->renderer, d->sprite, d->subimg, d->left, d->top, d->width, d->height, d->x,
                                   d->y, d->xs, d->ys, 0.0f, 0.0f, 0.0f, d->colour, d->alpha);
    }
    return RValue_makeUndefined();
}
#endif

#if defined(UTKW_PROF)
#include <3ds/svc.h>
static uint64_t utkProfEntry, utkProfHead, utkProfDry, utkProfReal;
static uint32_t utkProfCalls, utkProfRows;
#endif

static RValue utkWaver(VMContext* ctx, RValue* args, int32_t argCount) UTK_LATE_W;
static RValue utkWaver(VMContext* ctx, RValue* args, int32_t argCount) {
    const UtkWaver* w = &utkWaverInfo;
    Instance* inst = utkSelf(ctx);
    RValue* pa = inst != nullptr ? utkFind(inst, w->varA) : nullptr;
    if (pa == nullptr || !vmIsPlainNumber(pa->type) || argCount != 2 || !vmIsPlainNumber(args[0].type) ||
        !vmIsPlainNumber(args[1].type) || utkOwns(inst, w->varB) || utkOwns(inst, w->varC) || utkOwns(inst, w->varI))
        return VM_callCodeIndex(ctx, w->codeIndex, args, argCount);
    Runner* runner = ctx->runner;
#if defined(UTKW_PROF)
    utkProfEntry = svcGetSystemTick();
#endif
#if defined(UTKW_SHADOW)
    RValue savedA = *pa, savedB, savedC, savedI;
    RValue* sb = utkFind(inst, w->varB);
    RValue* sc = utkFind(inst, w->varC);
    RValue* si = utkFind(inst, w->varI);
    bool hadB = sb != nullptr, hadC = sc != nullptr, hadI = si != nullptr;
    if (hadB) savedB = *sb;
    if (hadC) savedC = *sc;
    if (hadI) savedI = *si;
    utkShadowNativeCount = 0;
#endif
    RValue t, one, zero, tenth;
    AOT_SET_INT(&one, 1, GML_TYPE_INT16);
    AOT_SET_INT(&zero, 0, GML_TYPE_INT16);
    AOT_SET_REAL(&tenth, 0.1, GML_TYPE_DOUBLE);

    // 0000-0018: b = argument0; c = argument1 (Push.v of the argument, Pop.v.v), in that order (an insert of each).
    vmWeakCopyInto(&t, &args[0]);
    *utkInsert(inst, w->varB) = t;
    vmWeakCopyInto(&t, &args[1]);
    *utkInsert(inst, w->varC) = t;

    // 0020-0030: a += 1 (Add.i.v). From here a holds a's value as its slot would after each store.
    RValue a;
    vmWeakCopyInto(&a, utkFind(inst, w->varA));
    vmArithNumbers(&a, &one, VM_ARITH_ADD, GML_TYPE_VARIABLE);
    *utkFind(inst, w->varA) = a;

    // 0038-0040: i = sprite_height (Push.v of the builtin, Pop.v.v: the real as it is). Then the loop's operands: no
    // step of it can change b, the instance or its sprite (draw_sprite_part_ext only draws).
    RValue i;
    AOT_SET_REAL(&i, utkSpriteSize(runner, inst, 1), GML_TYPE_VARIABLE);
    *utkInsert(inst, w->varI) = i;
    RValue b = *utkFind(inst, w->varB);
    RValue c = *utkFind(inst, w->varC);
    Renderer* renderer = runner->renderer;
    // draw_sprite_part_ext's conversions of what the code pushes: RValue_toInt32 of a real truncates, and the colour
    // is the int 16777215 (RValue_toColour of an int).
    int32_t spriteIndex = (int32_t) (GMLReal) inst->spriteIndex;
    int32_t subimg = (int32_t) (GMLReal) inst->imageIndex;
    if (0 > subimg) subimg = (int32_t) inst->imageIndex;
    float width = (float) utkSpriteSize(runner, inst, 0);
    const uint32_t colour = 16777215u;
    float alpha = (float) (GMLReal) inst->imageAlpha;
    const GMLReal x = (GMLReal) inst->x;
    const GMLReal y = (GMLReal) inst->y;
    // left is 0, but not a constant the compiler can see (z_utflames_waver.c: under -ffast-math a known 0 in the
    // clip's `left -= tpag->targetX` becomes -0.0 where the builtin passes +0.0).
    float left = 0.0f;
    __asm__("" : "+m"(left));
#if defined(UTKW_BATCH)
    // i starts at sprite_height and drops by 1 a row: fewer rows than the arrays hold when it is below their size.
    const bool batch = renderer != nullptr && (GMLReal) (UTKW_MAX_ROWS - 2) > i.real;
    int32_t rows = 0;
#endif

#if defined(UTKW_PROF)
    // Measurement builds only (never committed on): the same rows worked out once without drawing (the native's own
    // cost), then the real loop (the same plus the draws): the difference is what the renderer takes per call.
    uint64_t utkT0 = svcGetSystemTick();
    {
        RValue pa2 = a, pc = c, pi = i;
        float sink = 0.0f;
        int32_t rows = 0;
        while (vmCmpNumbers(&pi, &zero, CMP_GT) != 0) {
            vmArithNumbers(&pa2, &one, VM_ARITH_ADD, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(pa2));
            if (vmCmpNumbers(&pc, &zero, CMP_GT) != 0) {
                vmArithNumbers(&pc, &tenth, VM_ARITH_SUB, GML_TYPE_VARIABLE);
                __asm__("" : "+m"(pc));
            }
            if (vmCmpNumbers(&pc, &zero, CMP_LT) != 0) {
                vmArithNumbers(&pc, &tenth, VM_ARITH_SUB, GML_TYPE_VARIABLE);
                __asm__("" : "+m"(pc));
            }
            RValue yi;
            AOT_SET_REAL(&yi, y, GML_TYPE_VARIABLE);
            vmArithNumbers(&yi, &pi, VM_ARITH_ADD, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(yi));
            RValue q = pa2;
            vmDivNumbers(&q, &b, false, GML_TYPE_VARIABLE, ctx->currentCodeName);
            __asm__("" : "+m"(q));
            AOT_SET_REAL(&q, aotMathSin(q.real), GML_TYPE_VARIABLE);
            __asm__("" : "+m"(q));
            vmArithNumbers(&q, &pc, VM_ARITH_MUL, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(q));
            RValue xs;
            AOT_SET_REAL(&xs, x, GML_TYPE_VARIABLE);
            vmArithNumbers(&xs, &q, VM_ARITH_ADD, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(xs));
            sink += (float) xs.real + (float) yi.real;
            vmArithNumbers(&pi, &one, VM_ARITH_SUB, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(pi));
            rows++;
        }
        __asm__("" : "+m"(sink));
        utkProfRows += (uint32_t) rows;
    }
    uint64_t utkT1 = svcGetSystemTick();
#endif
    // 0048-019C: the rows, while i > 0 (Cmp.i.v Gt: the interpreter's compare, with its epsilon).
    while (vmCmpNumbers(&i, &zero, CMP_GT) != 0) {
        vmArithNumbers(&a, &one, VM_ARITH_ADD, GML_TYPE_VARIABLE);   // a += 1
        __asm__("" : "+m"(a));
        if (vmCmpNumbers(&c, &zero, CMP_GT) != 0) {                  // if (c > 0) c -= 0.1 (Sub.d.v)
            vmArithNumbers(&c, &tenth, VM_ARITH_SUB, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(c));
        }
        if (vmCmpNumbers(&c, &zero, CMP_LT) != 0) {                  // if (c < 0) c -= 0.1
            vmArithNumbers(&c, &tenth, VM_ARITH_SUB, GML_TYPE_VARIABLE);
            __asm__("" : "+m"(c));
        }
        RValue yi;   // y + i (Add.v.v)
        AOT_SET_REAL(&yi, y, GML_TYPE_VARIABLE);
        vmArithNumbers(&yi, &i, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        __asm__("" : "+m"(yi));
        RValue q = a;   // x + sin(a / b) * c (Div.v.v, sin, Mul.v.v, Add.v.v)
        vmDivNumbers(&q, &b, false, GML_TYPE_VARIABLE, ctx->currentCodeName);
        __asm__("" : "+m"(q));
        AOT_SET_REAL(&q, aotMathSin(q.real), GML_TYPE_VARIABLE);
        __asm__("" : "+m"(q));
        vmArithNumbers(&q, &c, VM_ARITH_MUL, GML_TYPE_VARIABLE);
        __asm__("" : "+m"(q));
        RValue xs;
        AOT_SET_REAL(&xs, x, GML_TYPE_VARIABLE);
        vmArithNumbers(&xs, &q, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        __asm__("" : "+m"(xs));
#if defined(UTKW_SHADOW)
        utkShadowNote(utkShadowNative, &utkShadowNativeCount, spriteIndex, subimg, left, (float) i.real, width, 1.0f,
                      (float) xs.real, (float) yi.real, 1.0f, 1.0f, colour, alpha);
#else
#if defined(UTKW_BATCH)
        if (batch) {
            utkRowTop[rows] = (float) i.real;
            utkRowX[rows] = (float) xs.real;
            utkRowY[rows] = (float) yi.real;
            rows++;
        } else
#endif
        if (renderer != nullptr) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, left, (float) i.real, width, 1.0f,
                                       (float) xs.real, (float) yi.real, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, colour, alpha);
        }
#endif
        vmArithNumbers(&i, &one, VM_ARITH_SUB, GML_TYPE_VARIABLE);   // i -= 1 (Sub.i.v)
        __asm__("" : "+m"(i));
    }
#if defined(UTKW_BATCH)
    if (batch && !CtrUtk_waverRows(renderer, spriteIndex, subimg, left, width, colour, alpha, rows, utkRowTop, utkRowX,
                                   utkRowY)) {
        for (int32_t k = 0; rows > k; k++)
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, left, utkRowTop[k], width, 1.0f, utkRowX[k],
                                       utkRowY[k], 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, colour, alpha);
    }
#endif
    *utkFind(inst, w->varA) = a;
    *utkFind(inst, w->varC) = c;
    *utkFind(inst, w->varI) = i;
#if defined(UTKW_PROF)
    uint64_t utkT2 = svcGetSystemTick();
    utkProfCalls++;
    utkProfHead += utkT0 - utkProfEntry;
    utkProfDry += utkT1 - utkT0;
    utkProfReal += utkT2 - utkT1;
    if (utkProfCalls % 300 == 0) {
        // Ticks at 268111856 Hz; microseconds per call and per row.
        double us = 1e6 / 268111856.0, n = (double) utkProfCalls, rows = (double) utkProfRows;
        logInfo("utkill waver prof: %u calls, %.1f rows/call | per call: head %.1f us, compute %.1f us, compute+draw "
                "%.1f us, draw %.1f us | per row: compute %.2f us, draw %.2f us\n", (unsigned) utkProfCalls, rows / n,
                (double) utkProfHead * us / n, (double) utkProfDry * us / n, (double) utkProfReal * us / n,
                (double) (utkProfReal - utkProfDry) * us / n, (double) utkProfDry * us / rows,
                (double) (utkProfReal - utkProfDry) * us / rows);
    }
#endif
#if defined(UTKW_SHADOW)
    (void) renderer;
    RValue na = a, nb = *utkFind(inst, w->varB), nc = c, ni = i;
    // Put the variables back as they were (b, c and i were inserted above if missing; the script inserts them again
    // in the same order, so their slots' contents are all that differ), then the script, recorded.
    *utkFind(inst, w->varA) = savedA;
    if (hadB) *utkFind(inst, w->varB) = savedB;
    if (hadC) *utkFind(inst, w->varC) = savedC;
    if (hadI) *utkFind(inst, w->varI) = savedI;
    int32_t drawIndex = -1;
    for (uint32_t k = 0; ctx->dataWin->func.functionCount > k; k++) {
        const char* name = ctx->dataWin->func.functions[k].name;
        if (name != nullptr && strcmp(name, "draw_sprite_part_ext") == 0 && ctx->funcCallCache[k].builtin != nullptr) {
            drawIndex = (int32_t) k;
            break;
        }
    }
    void* original = drawIndex >= 0 ? ctx->funcCallCache[drawIndex].builtin : nullptr;
    if (drawIndex >= 0) ctx->funcCallCache[drawIndex].builtin = (void*) utkShadowRecorder;
    utkShadowScriptCount = 0;
    RValue result = VM_callCodeIndex(ctx, w->codeIndex, args, argCount);
    if (drawIndex >= 0) ctx->funcCallCache[drawIndex].builtin = original;
    utkShadowCalls++;
    bool same = drawIndex >= 0 && utkShadowNativeCount == utkShadowScriptCount && UTK_SHADOW_MAX >= utkShadowNativeCount &&
                memcmp(utkShadowNative, utkShadowScript, sizeof(UtkDraw) * (size_t) utkShadowNativeCount) == 0;
    const RValue* pairs[4][2] = { { &na, utkFind(inst, w->varA) }, { &nb, utkFind(inst, w->varB) },
                                  { &nc, utkFind(inst, w->varC) }, { &ni, utkFind(inst, w->varI) } };
    for (int k = 0; 4 > k; k++) {
        const RValue* p = pairs[k][0];
        const RValue* q = pairs[k][1];
        if (q == nullptr || p->type != q->type || p->gmlStackType != q->gmlStackType || p->ownsReference != q->ownsReference ||
            memcmp(&p->real, &q->real, sizeof p->real) != 0)
            same = false;
    }
    if (!same) {
        utkShadowBad++;
        logInfo("utkill waver shadow: MISMATCH at frame %d (call %u): draws %d native, %d script\n", (int) runner->frameCount,
                (unsigned) utkShadowCalls, (int) utkShadowNativeCount, (int) utkShadowScriptCount);
    }
    if (utkShadowCalls % 300 == 0)
        logInfo("utkill waver shadow: %u calls, %u mismatches\n", (unsigned) utkShadowCalls, (unsigned) utkShadowBad);
    return result;
#else
    return aotx_scriptFallthroughReturnValue(ctx);
#endif
}

// Registers utkWaver under the name of the script whose bytecode is the expected one, if there is one.
static void utkWaverLoad(VMContext* ctx) UTK_LATE_W;
static void utkWaverLoad(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    if (dw == nullptr || dw->gen8.wadVersion != 16) return;
    for (uint32_t n = 0; dw->scpt.count > n; n++) {
        Script* s = &dw->scpt.scripts[n];
        if (s->name == nullptr || s->codeId < 0 || (uint32_t) s->codeId >= dw->code.count) continue;
        CodeEntry* code = &dw->code.entries[s->codeId];
        if (!code->present || code->offset != 0 || code->length != UTK_WAVER_LENGTH) continue;
        const uint8_t* bytes = DataWin_codeBytes(dw, code);
        if (bytes == nullptr || utkDigest(bytes, code->length) != UTK_WAVER_DIGEST) continue;
        UtkWaver* w = &utkWaverInfo;
        w->codeIndex = s->codeId;
        w->varB = utkOperandVar(ctx, bytes, 0x0C);
        w->varC = utkOperandVar(ctx, bytes, 0x1C);
        w->varA = utkOperandVar(ctx, bytes, 0x24);
        w->varI = utkOperandVar(ctx, bytes, 0x44);
        if (0 > w->varA || 0 > w->varB || 0 > w->varC || 0 > w->varI || VM_findBuiltin(ctx, s->name) != nullptr)
            return;
        VM_registerBuiltin(ctx, s->name, utkWaver);
        logInfo(utkWaverLogOn);
        return;
    }
}

void UtKill_registerAll(VMContext* ctx) {
    UtFlames_registerAll(ctx);
    utkWaverLoad(ctx);
}
#endif

#endif // ENABLE_WAD16
