// UNDERTALE's heat shimmer (Pyrope, Hotland): a native body for its script (utflames:
// per-effect code is allowed when pixels and game state stay identical).
//
// A file of its own, apart from z_utflames.c: any addition there changes where GCC puts that file's functions. All of
// it is late (late_reserve.ld: .text.late.utflames.waver and this file's constants after z_utflames.c's code, its
// statics in .bss.late.utflames), so nothing that existed moves; its constants are unmerged (-fno-merge-constants,
// CMakeLists.txt), as a mergeable string pool would move other late code. A build without WAD 16 (DELTARUNE's) has
// none of it.
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
#define UTF_LATE_W __attribute__((noinline, section(".text.late.utflames.waver")))
#define UTF_BSS_W __attribute__((section(".bss.late.utflames")))
#else
#define UTF_LATE_W __attribute__((noinline))
#define UTF_BSS_W
#endif

void UtFlames_registerAll(VMContext* ctx) UTF_LATE_W;   // VM_create, through COLFLIP_REGISTER_ALL (vm_builtins.h)

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
// Tracing and stress builds test the interpreter and the generated code: the script stays the game's.
void UtFlames_registerAll(VMContext* ctx) { Btnprompt_registerAll(ctx); }
#else
// self when it is an instance (no struct).
static inline Instance* utfSelf(VMContext* ctx) {
    Instance* inst = (Instance*) ctx->currentInstance;
    return inst != nullptr && inst->objectIndex != STRUCT_OBJECT_INDEX ? inst : nullptr;
}

// ===[ The heat shimmer's rows (Pyrope, encounter 52) ]===
//
// The shimmer script is not in the AOT selection, so the interpreter ran its loop: some 5,500 instructions a frame for
// the ~136 one-pixel rows of draw_sprite_part_ext it draws. utfWaver is a builtin of the script's name, registered
// before VM_create builds the function cache, so every direct call of the script (interpreted or generated) comes here
// (script_execute by index still runs the script itself, with the same result):
//     b = argument0; c = argument1; a += 1;
//     for (i = 0; i < sprite_height; i++) { a += 1; draw_sprite_part_ext(sprite_index, image_index, 0, i,
//         sprite_width, 1, x + sin(a / b) * c, y + i, 1, 1, c_white, image_alpha); }
// The script is found by its bytecode, the one this was written from (UTF_WAVER_LENGTH bytes whose digest over the
// loaded and patched bytes is UTF_WAVER_DIGEST), so no game name is written here; a, b, c and i come from its
// operands. Data whose bytes differ registers nothing, and the script runs as it is. Each value is worked out as the
// interpreter works it (the same helpers, one operation at a time), and the draw gets the arguments
// draw_sprite_part_ext converts them to. Any other case (no instance, other arguments, a not a real) runs the script
// itself. utfWaverInfo is one per process: a 3DS build runs one UNDERTALE VM.
typedef struct {
    int32_t codeIndex;
    int32_t varA, varB, varC, varI;
} UtfWaver;
static UtfWaver utfWaverInfo UTF_BSS_W;

// 32 B, a whole number of words, so the late text after it (or its end) stays word-aligned.
static const char utfWaverLogOn[32] = "utflames: heat shimmer native\n";

#define UTF_WAVER_LENGTH 312u
#define UTF_WAVER_DIGEST 0x4F15EF07u
#define utfDigest UtFlames_waverDigest // also z_utkill_waver.c's (freebytes)
__attribute__((noipa)) uint32_t utfDigest(const uint8_t* bytes, uint32_t length) UTF_LATE_W;
uint32_t utfDigest(const uint8_t* bytes, uint32_t length) {
    uint32_t h = 0x811C9DC5u;
    for (uint32_t i = 0; length > i; i++) h = (h ^ bytes[i]) * 0x01000193u;
    return h;
}

#define utfOperandVar UtFlames_waverOperandVar // the varID the Push.v/Pop.v operand at off names; z_utkill_waver.c's too
__attribute__((noipa)) int32_t utfOperandVar(VMContext* ctx, const uint8_t* bytes, uint32_t off) UTF_LATE_W;
int32_t utfOperandVar(VMContext* ctx, const uint8_t* bytes, uint32_t off) {
    uint32_t w = (uint32_t) bytes[off] | (uint32_t) bytes[off + 1] << 8 | (uint32_t) bytes[off + 2] << 16 |
                 (uint32_t) bytes[off + 3] << 24;
    uint32_t index = w & 0x07FFFFFFu;
    if (index >= ctx->dataWin->vari.variableCount) return -1;
    return ctx->dataWin->vari.variables[index].varID;
}

#define utfFind UtFlames_slot // z_utflames.c's IntRValueHashMap_findSlot copy: one for every UNDERTALE late file
RValue* utfFind(Instance* inst, int32_t varID); // (freebytes: each file had its own byte-identical 108 B copy)
#line 95

static bool utfOwns(Instance* inst, int32_t varID) UTF_LATE_W;
static bool utfOwns(Instance* inst, int32_t varID) {
    RValue* p = utfFind(inst, varID);
    return p != nullptr && p->ownsReference;
}

static RValue* utfInsert(Instance* inst, int32_t varID) UTF_LATE_W;
static RValue* utfInsert(Instance* inst, int32_t varID) {
    return IntRValueHashMap_getOrInsertUndefinedFast(&inst->selfVars, varID);
}

// sprite_width (axis 0) or sprite_height (axis 1), as VMBuiltins_getVariable reads it.
static GMLReal utfSpriteSize(Runner* runner, Instance* inst, int axis) UTF_LATE_W;
static GMLReal utfSpriteSize(Runner* runner, Instance* inst, int axis) {
    if (inst->spriteIndex >= 0 && runner->dataWin->sprt.count > (uint32_t) inst->spriteIndex) {
        Sprite* s = &runner->dataWin->sprt.sprites[inst->spriteIndex];
        return axis == 0 ? (GMLReal) s->width * inst->imageXscale : (GMLReal) s->height * inst->imageYscale;
    }
    return 0.0;
}

static RValue utfWaver(VMContext* ctx, RValue* args, int32_t argCount) UTF_LATE_W;
static RValue utfWaver(VMContext* ctx, RValue* args, int32_t argCount) {
    const UtfWaver* w = &utfWaverInfo;
    Instance* inst = utfSelf(ctx);
    RValue* pa = inst != nullptr ? utfFind(inst, w->varA) : nullptr;
    if (pa == nullptr || pa->type != RVALUE_REAL || argCount != 2 || !vmIsPlainNumber(args[0].type) ||
        !vmIsPlainNumber(args[1].type) || utfOwns(inst, w->varB) || utfOwns(inst, w->varC) || utfOwns(inst, w->varI))
        return VM_callCodeIndex(ctx, w->codeIndex, args, argCount);
    Runner* runner = ctx->runner;
    RValue t, kv;
    AOT_SET_INT(&kv, 1, GML_TYPE_INT16);

    // 0000-0018: b = argument0; c = argument1 (Push.v of the argument, Pop.v.v), in that order (an insert of each).
    vmWeakCopyInto(&t, &args[0]);
    *utfInsert(inst, w->varB) = t;
    vmWeakCopyInto(&t, &args[1]);
    *utfInsert(inst, w->varC) = t;

    // 0020-0030: a += 1. From here a holds a's value as its slot would after each store (a real stays a real).
    RValue a;
    vmWeakCopyInto(&a, utfFind(inst, w->varA));
    vmArithNumbers(&a, &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);
    *utfFind(inst, w->varA) = a;

    // 0038-003C: i = 0 (PushI.e, Pop.v.i: an int store, made a real). Then the loop's operands: no step of it can
    // change b, c, the instance or its sprite (draw_sprite_part_ext only draws).
    RValue i;
    AOT_SET_INT(&i, 0, GML_TYPE_INT16);
    coerceIntStoreToRealInPlace(&i, GML_TYPE_INT32);
    *utfInsert(inst, w->varI) = i;
    RValue b = *utfFind(inst, w->varB);
    RValue c = *utfFind(inst, w->varC);
    Renderer* renderer = runner->renderer;
    RValue h;
    AOT_SET_REAL(&h, utfSpriteSize(runner, inst, 1), GML_TYPE_VARIABLE);
    // draw_sprite_part_ext's conversions of what the code pushes: RValue_toInt32 of a real truncates, and the colour
    // is the int 16777215 (RValue_toColour of an int).
    int32_t spriteIndex = (int32_t) (GMLReal) inst->spriteIndex;
    int32_t subimg = (int32_t) (GMLReal) inst->imageIndex;
    if (0 > subimg) subimg = (int32_t) inst->imageIndex;
    float width = (float) utfSpriteSize(runner, inst, 0);
    const uint32_t colour = 16777215u;
    float alpha = (float) (GMLReal) inst->imageAlpha;
    const GMLReal x = (GMLReal) inst->x;
    const GMLReal y = (GMLReal) inst->y;
    // left is 0, but not a constant the compiler can see: under -ffast-math the clip's `left -= tpag->targetX` with a
    // known 0 becomes -targetX, which is -0.0 where the builtin (left read at run time) passes +0.0. Same pixels, but
    // a different argument (the state hash's draw column saw it).
    float left = 0.0f;
    __asm__("" : "+m"(left));

    // 0044-0134: the rows, while i < sprite_height (the interpreter's compare, with its epsilon).
    while (vmCmpNumbers(&i, &h, CMP_LT) != 0) {
        vmArithNumbers(&a, &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        __asm__("" : "+m"(a));
        RValue yi;   // y + i
        AOT_SET_REAL(&yi, y, GML_TYPE_VARIABLE);
        vmArithNumbers(&yi, &i, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        __asm__("" : "+m"(yi));
        RValue q = a;   // x + sin(a / b) * c
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
        if (renderer != nullptr) {
            Renderer_drawSpritePartExt(renderer, spriteIndex, subimg, left, (float) i.real, width, 1.0f,
                                       (float) xs.real, (float) yi.real, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                                       colour, alpha);
        }
        vmArithNumbers(&i, &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE);   // i += 1 (a weak copy of i, so typed a variable)
        __asm__("" : "+m"(i));
    }
    *utfFind(inst, w->varA) = a;
    *utfFind(inst, w->varI) = i;
    return aotx_scriptFallthroughReturnValue(ctx);
}

// Registers utfWaver under the name of the script whose bytecode is the expected one, if there is one.
static void utfWaverLoad(VMContext* ctx) UTF_LATE_W;
static void utfWaverLoad(VMContext* ctx) {
    DataWin* dw = ctx->dataWin;
    if (dw == nullptr || dw->gen8.wadVersion != 16) return;
    for (uint32_t n = 0; dw->scpt.count > n; n++) {
        Script* s = &dw->scpt.scripts[n];
        if (s->name == nullptr || s->codeId < 0 || (uint32_t) s->codeId >= dw->code.count) continue;
        CodeEntry* code = &dw->code.entries[s->codeId];
        if (!code->present || code->offset != 0 || code->length != UTF_WAVER_LENGTH) continue;
        const uint8_t* bytes = DataWin_codeBytes(dw, code);
        if (bytes == nullptr || utfDigest(bytes, code->length) != UTF_WAVER_DIGEST) continue;
        UtfWaver* w = &utfWaverInfo;
        w->codeIndex = s->codeId;
        w->varB = utfOperandVar(ctx, bytes, 0x0C);
        w->varC = utfOperandVar(ctx, bytes, 0x1C);
        w->varA = utfOperandVar(ctx, bytes, 0x24);
        w->varI = utfOperandVar(ctx, bytes, 0x40);
        if (0 > w->varA || 0 > w->varB || 0 > w->varC || 0 > w->varI || VM_findBuiltin(ctx, s->name) != nullptr)
            return;
        VM_registerBuiltin(ctx, s->name, utfWaver);
        logInfo(utfWaverLogOn);
        return;
    }
}

void UtFlames_registerAll(VMContext* ctx) {
    Btnprompt_registerAll(ctx);
    utfWaverLoad(ctx);
}
#endif

#endif // ENABLE_WAD16
