// dodgefront (credfix, charter rev 1 item 2, option 3; UNDERTALE Old 3DS only): the script that draws a line of
// text centred, run as hand code for its common case. Late code and constants only (platform/3ds/late_reserve.ld): no
// generated function, no table row, and nothing outside the late blocks moves.
//
// Why: the True Pacifist credits dodge ends on the list of every credit the soul touched. The
// game draws each touched name every frame through gml_Script_scr_drawtext_centered(xx, yy, text), some 140 calls a
// frame with most names below the screen; the text itself is culled, and the cost is the two scripts around each draw
// (the centred one calls gml_Script_scr_drawtext_centered_scaled with both scales 1). On the Old 3DS neither is in the
// AOT budget, so both run in the interpreter: consoles cf1026-dodge-o3-b1 and -o2-b2 held 169 and 167 frames for 3
// vblanks there, CPU (Draw) bound, the GPU under 5 ms.
//
// What: for a line without "#" the scaled script's single-line path is
//   display_scale = surface_get_width(application_surface) / view_wview[view_current];
//   yy = round(yy * display_scale) / display_scale;
//   width = string_width(text) * xscale;
//   draw_text_transformed(round(xx - width / 2), yy, text, xscale, yscale, 0);
// Its string_height(" ") (the line height, used only between lines) and string_pos("#", text) (0 here) only read, so
// the front leaves them out. Every other step is what the interpreter does: the builtins from the same call cache with
// the same arguments in the same order, view_wview as the builtin variable reads it (Runner_getCameraForView), and
// each Div, Mul and Sub as vmDivNumbers / vmArithNumbers does it: an int32 made a real, then one operation on two reals,
// each behind an empty asm statement (the engine is built with -ffast-math). width * xscale, a real times int32 1, is
// width itself and is left out. Anything else (other argument types, a "#", no camera, a zero view width or surface
// width, a builtin not yet in the call cache or returning other than a real) runs the whole
// script in the interpreter from its start (aotx_executeLoop), which the front has changed nothing for: the builtins it
// may have called by then only read.
//
// Binding: both scripts are pinned by their bytecode's digests (the generator's, aot_table.c's on New), the four
// builtins by their FUNC names (each exactly once). The front goes into the VM's per-code-index binding
// (VM_aotBindFront, vm.c's late end) at each room change, through CtrRenderer_prefetchRoom's wrap chain
// (ctr_wrap_chain.h), so it is back after any VM reset. The digests are checked once per data.win.
//
// This build is the only user: CMakeLists.txt compiles this file and defines CTRPORT_DODGEFRONT (here, vm.c and
// z_roomplan.c) for UNDERTALE o3DS only.

#if defined(CTRPORT_DODGEFRONT) && defined(ENABLE_WAD16)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "vm.h"
#include "vm_fastpath.h"

#define DF_CODE __attribute__((noinline, section(".text.dodgefront_late")))
#define DF_RO __attribute__((aligned(4), section(".rodata.dodgefront_in_text")))
#define DF_BSS __attribute__((section(".bss.dodgefront_late")))
#define DF_OPAQUE(v) __asm__("" : "+m"(v))

typedef RValue (*DfFn)(VMContext* ctx);
bool VM_aotBindFront(VMContext* ctx, int32_t codeIndex, DfFn fn);
RValue aotx_executeLoop(VMContext* ctx);
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx);
void __real_CtrRenderer_prefetchRoom(Renderer* renderer);

// Names, one after another, each ending in NUL: the two scripts, then the four builtins in the front's order.
static const char dfNames[] DF_RO = "gml_Script_scr_drawtext_centered\0gml_Script_scr_drawtext_centered_scaled\0"
                                    "surface_get_width\0round\0string_width\0draw_text_transformed\0";
static const uint32_t dfDigest[2][2] DF_RO = {{0x99853B3Eu, 0x6FFB95C7u}, {0x9346B5EFu, 0x90A6810Eu}};
// The log line, once per data.win: the code index bound, or minus the lookup step that failed.
static const char dfLog[] DF_RO = "dodgefront: %ld\n";

static DataWin* dfChecked DF_BSS;   // the data.win the lookups below are for
static int32_t dfCode DF_BSS;       // the centred script's code index, or -1: not this game's script
static int32_t dfFn[4] DF_BSS;      // the builtins' FUNC indices
static bool dfLogged DF_BSS;        // "bound" logged for this data.win

// RValue_free (static inline) at one call site, so it is inlined here and no second out-of-line RValue_free is
// emitted (check_layout counts definitions per name).
static DF_CODE void dfFree(RValue* v) {
    RValue_free(v);
}

static DF_CODE uint64_t dfDigestOf(DataWin* dw, CodeEntry* code) {
    const uint8_t* bytes = DataWin_codeBytes(dw, code);
    if (bytes == NULL || code->offset > code->length) return 0;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = code->offset; code->length > i; i++) {
        h ^= bytes[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// The code index named, or -1.
static DF_CODE int32_t dfFindCode(DataWin* dw, const char* name) {
    for (uint32_t i = 0; dw->code.count > i; i++) {
        const char* n = dw->code.entries[i].name;
        if (n != NULL && strcmp(n, name) == 0) return (int32_t) i;
    }
    return -1;
}

// The lookups, once per data.win: 0 when all match, else the step that didn't (for the log).
static DF_CODE int32_t dfCheck(DataWin* dw) {
    const char* name = dfNames;
    __asm__("" : "+r"(name));
    int32_t codes[2];
    for (int k = 0; 2 > k; k++, name += strlen(name) + 1) {
        codes[k] = dfFindCode(dw, name);
        if (0 > codes[k]) return 1 + k;
        uint64_t want = ((uint64_t) dfDigest[k][1] << 32) | dfDigest[k][0];
        if (dfDigestOf(dw, &dw->code.entries[codes[k]]) != want) return 3 + k;
    }
    for (int k = 0; 4 > k; k++, name += strlen(name) + 1) {
        dfFn[k] = -1;
        for (uint32_t i = 0; dw->func.functionCount > i; i++) {
            const char* n = dw->func.functions[i].name;
            if (n == NULL || strcmp(n, name) != 0) continue;
            if (dfFn[k] >= 0) return 5 + k;
            dfFn[k] = (int32_t) i;
        }
        if (0 > dfFn[k]) return 5 + k;
    }
    dfCode = codes[0];
    return 0;
}

static DF_CODE RValue dfFront(VMContext* ctx) {
    Runner* runner = ctx->runner;
    uint32_t n = ctx->funcCallCacheCount;
    if (ctx->scriptArgs == NULL || 3 > ctx->scriptArgCount || runner == NULL
        || (uint32_t) dfFn[0] >= n || (uint32_t) dfFn[1] >= n || (uint32_t) dfFn[2] >= n || (uint32_t) dfFn[3] >= n)
        return aotx_executeLoop(ctx);
    BuiltinFunc surfaceWidth = (BuiltinFunc) ctx->funcCallCache[dfFn[0]].builtin;
    BuiltinFunc roundFn = (BuiltinFunc) ctx->funcCallCache[dfFn[1]].builtin;
    BuiltinFunc stringWidth = (BuiltinFunc) ctx->funcCallCache[dfFn[2]].builtin;
    BuiltinFunc draw = (BuiltinFunc) ctx->funcCallCache[dfFn[3]].builtin;
    const RValue* xx = &ctx->scriptArgs[0];
    const RValue* yy = &ctx->scriptArgs[1];
    const RValue* text = &ctx->scriptArgs[2];
    if (surfaceWidth == NULL || roundFn == NULL || stringWidth == NULL || draw == NULL || !vmIsPlainNumber(xx->type)
        || !vmIsPlainNumber(yy->type) || text->type != RVALUE_STRING || text->string == NULL
        || strchr(text->string, '#') != NULL)
        return aotx_executeLoop(ctx);
    GMLCamera* camera = Runner_getCameraForView(runner, runner->viewCurrent);
    if (camera == NULL || camera->viewWidth == 0) return aotx_executeLoop(ctx);
    RValue v[4];
    v[0] = RValue_makeReal((GMLReal) runner->applicationSurfaceId);
    v[1] = surfaceWidth(ctx, &v[0], 1);
    if (v[1].type != RVALUE_REAL || v[1].real == (GMLReal) 0) {
        dfFree(&v[1]);
        return aotx_executeLoop(ctx);
    }
    // display_scale = surface_get_width(application_surface) / view_wview[view_current]
    GMLReal scale = v[1].real / (GMLReal) camera->viewWidth;
    DF_OPAQUE(scale);
    // yy = round(yy * display_scale) / display_scale
    GMLReal t = (yy->type == RVALUE_INT32 ? (GMLReal) yy->int32 : yy->real) * scale;
    DF_OPAQUE(t);
    v[0] = RValue_makeReal(t);
    v[1] = roundFn(ctx, &v[0], 1);
    vmWeakCopyInto(&v[2], text);
    v[3] = stringWidth(ctx, &v[2], 1);
    if (v[1].type != RVALUE_REAL || v[3].type != RVALUE_REAL) {
        dfFree(&v[1]);
        dfFree(&v[3]);
        return aotx_executeLoop(ctx);
    }
    GMLReal y = v[1].real / scale;
    DF_OPAQUE(y);
    // draw_text_transformed(round(xx - width / 2), yy, text, xscale, yscale, 0), width = string_width(text) * xscale
    GMLReal half = v[3].real / (GMLReal) 2;
    DF_OPAQUE(half);
    t = (xx->type == RVALUE_INT32 ? (GMLReal) xx->int32 : xx->real) - half;
    DF_OPAQUE(t);
    v[0] = RValue_makeReal(t);
    RValue args[6];
    args[0] = roundFn(ctx, &v[0], 1);
    args[1] = RValue_makeReal(y);
    args[2] = v[2];
    args[3] = RValue_makeInt32(1);
    args[4] = args[3];
    args[5] = RValue_makeInt32(0);
    v[0] = draw(ctx, args, 6);
    dfFree(&v[0]);
    dfFree(&args[0]);
    return aotx_scriptFallthroughReturnValue(ctx);
}

DF_CODE void CTR_PREFETCHROOM_DODGEFRONT(Renderer* renderer) {
    __real_CtrRenderer_prefetchRoom(renderer);
    Runner* runner = renderer != NULL ? renderer->runner : NULL;
    VMContext* ctx = runner != NULL ? runner->vmContext : NULL;
    if (ctx == NULL || ctx->dataWin == NULL) return;
    int32_t step = 0;
    if (dfChecked != ctx->dataWin) {
        dfChecked = ctx->dataWin;
        dfCode = -1;
        dfLogged = false;
        step = dfCheck(ctx->dataWin);
    }
    bool bound = dfCode >= 0 && VM_aotBindFront(ctx, dfCode, dfFront);
    // Once per data.win: the first binding, or the lookup step that failed.
    if ((bound && !dfLogged) || step != 0) {
        dfLogged = true;
        logInfo(dfLog, (long) (bound ? dfCode : -step));
    }
}

#endif
