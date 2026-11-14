// utrect (change uthunt): a native body for UNDERTALE's filled-rectangle script, the one every rectangle of the game goes
// through (Hotland's 18 rising-heat columns in one room alone call it 144 times a frame, each call a script frame with
// six locals and two os_type reads around one draw_rectangle). Per-effect code like this is used only where pixels and
// game state stay identical to the PC game.
//
// The script (UT 1.08, 356 bytes, no instance or global variable touched): its four arguments into locals; the corners
// put in order (x1 > x2 swaps them, then y1 > y2: the interpreter's compare, with its epsilon); on two consoles'
// os_type (12, 14) the far corner moves out by one (Add.i.v); then draw_rectangle(x1, y1, x2, y2, 0). This port's
// os_type is OS_WINDOWS, set once by main.c and never changed, so the corner move never runs here. The builtin here
// puts the corners in order the same way and draws as draw_rectangle's builtin does with outline 0 (vm_builtins.c,
// the only one of that name: each corner to a float, the renderer's drawRectangle with the draw colour and alpha), so
// every draw and the draw state are the script's. Anything unusual (not four arguments, an argument that isn't a real)
// runs the script itself.
//
// The script is found by its bytecode (UTR_LENGTH bytes whose FNV-1a over the loaded and patched bytes is
// UTR_DIGEST, as z_utkill_waver.c's; the digest covers the patched Call of draw_rectangle), so no game name is written
// here; no builtin has a script's name, so none is replaced. It is registered by Utgap_registerAll's wrap
// (CMakeLists.txt, UNDERTALE only): the last of VM_create's registrations, before the function cache is built, so
// every call of the script (interpreted or generated) comes here; in vm.c only that call's target changes. All of it
// is late (late_reserve.ld: last, after pacerflt), so nothing that existed moves. Tracing and stress builds keep the
// script.
#include "wad_versions.h"

#if defined(ENABLE_WAD16) && defined(__3DS__)   // 3DS only: the wrap is a 3DS link option (PC builds keep the script)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdint.h>

#if defined(__arm__) && defined(__3DS__)
#define UTR_LATE __attribute__((noinline, section(".text.utrect_late")))
#define UTR_BSS __attribute__((section(".bss.utrect_late")))
#else
#define UTR_LATE __attribute__((noinline))
#define UTR_BSS
#endif

void __real_Utgap_registerAll(VMContext* ctx);
void __wrap_Utgap_registerAll(VMContext* ctx) UTR_LATE;

#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
void __wrap_Utgap_registerAll(VMContext* ctx) { __real_Utgap_registerAll(ctx); }
#else

#define UTR_LENGTH 356u
#define UTR_DIGEST 0x24F09522u

uint32_t UtFlames_waverDigest(const uint8_t* bytes, uint32_t length);   // z_utflames_waver.c's FNV-1a

static int32_t utrCodeIndex UTR_BSS;

static RValue utrFill(VMContext* ctx, RValue* args, int32_t argCount) UTR_LATE;
static RValue utrFill(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount != 4) goto script;
    for (int k = 0; 4 > k; k++)
        if (args[k].type != RVALUE_REAL) goto script;
    GMLReal x1 = args[0].real, y1 = args[1].real, x2 = args[2].real, y2 = args[3].real, t;
    // x1 > x2, then y1 > y2, as vmCmpNumbers: Cmp GT holds unless the difference is within GML_MATH_EPSILON or below
    // it (a NaN holds: vmCmpOrder gives it 1).
    if (!(x1 - x2 <= GML_MATH_EPSILON)) { t = x1; x1 = x2; x2 = t; }
    if (!(y1 - y2 <= GML_MATH_EPSILON)) { t = y1; y1 = y2; y2 = t; }
    // builtin_draw_rectangle with outline 0 (its corner offset is for outlines only).
    Renderer* renderer = ctx->runner->renderer;
    if (renderer != nullptr)
        renderer->vtable->drawRectangle(renderer, (float) x1, (float) y1, (float) x2, (float) y2, renderer->drawColor,
                                        renderer->drawAlpha, false);
    return aotx_scriptFallthroughReturnValue(ctx);
script:
    return VM_callCodeIndex(ctx, utrCodeIndex, args, argCount);
}

static const char utrLogOn[] __attribute__((section(".rodata.utrect_in_text"))) = "utrect on\n";

void __wrap_Utgap_registerAll(VMContext* ctx) {
    __real_Utgap_registerAll(ctx);
    DataWin* dw = ctx->dataWin;
    Script* s = dw->scpt.scripts;
    for (uint32_t n = dw->scpt.count; n != 0; n--, s++) {
        // codeId as unsigned: a negative one (no code) is out of range too.
        if ((uint32_t) s->codeId >= dw->code.count) continue;
        CodeEntry* code = &dw->code.entries[s->codeId];
        // A missing entry's bytes are a 4-byte stand-in (DataWin_loadCodeBytes): never digested.
        if (!code->present || code->length != UTR_LENGTH) continue;
        if (UtFlames_waverDigest(DataWin_codeBytes(dw, code), UTR_LENGTH) != UTR_DIGEST) continue;
        utrCodeIndex = s->codeId;
        VM_registerBuiltin(ctx, s->name, utrFill);
        logInfo(utrLogOn);
        return;
    }
}
#endif

#endif // ENABLE_WAD16 && __3DS__
