// textfront (UNDERTALE Old 3DS only): the game's two centred-text scripts run as hand code, with lines out of view left
// out. Late code and constants only (platform/3ds/late_reserve.ld, after every late input already linked): no
// generated function, no table row, and nothing that existed moves. It is bound ahead of z_dodgefront.c's front for
// the centred script (CtrRenderer_prefetchRoom's wrap chain, ctr_wrap_chain.h), which then stays unbound and is the
// fallback when these lookups fail.
//
// Why: the True Pacifist credits draw nearly all their text through the centred script (xx, yy, text), which calls
// the scaled one (xx, yy, text, 1, 1), or through the scaled one itself (the cast roll: names at scale 3, lines at 2,
// "#" between lines). Neither is in the Old 3DS AOT budget. The dodge's list of touched names (some 140 calls a frame,
// most names below the screen) still measured, with the centred script's single-line front, every off-screen name's
// string_width and draw builtin (string copies, the Crisp path's glyph walk); the cast roll spent about half its Draw
// time interpreting the scaled script and the substr calls it makes, and drew every line, on screen or not.
//
// What: the scaled script is
//   display_scale = surface_get_width(application_surface) / view_wview[view_current];
//   lineheight = round(string_height(" ") * yscale);
//   eol = string_pos("#", text);
//   yy = round(yy * display_scale) / display_scale;
//   while (eol != 0) {
//       line = substr(text, 1, eol); text = substr(text, eol + 1);   // the "#" stays on the end of line
//       width = string_width(line) * xscale;
//       line_x = round((xx - width / 2) * display_scale) / display_scale;
//       draw_text_transformed(line_x, yy, line, xscale, yscale, 0);
//       yy += lineheight; eol = string_pos("#", text);
//   }
//   width = string_width(text) * xscale;
//   draw_text_transformed(round(xx - width / 2), yy, text, xscale, yscale, 0);
// and the centred one calls it with both scales int32 1. substr(s, p, n) is string_copy(s, p, n) and substr(s, p) the
// rest from p; string_pos, string_copy and string_length all count UTF-8 code points, so on ASCII text (the only text
// split here) a line is the bytes up to and including a "#" and the rest the bytes after it. The front does exactly
// that, without the substr, string_pos and string_copy calls, which only read; string_height(" ") is called only when
// there is a "#" (it only reads too). Every other step is what the interpreter does: the builtins from the same call
// cache with the same arguments in the same order (surface_get_width, round, string_width and string_height always
// return a real, draw_text_transformed undefined: none owns anything), view_wview as the builtin variable reads it
// (Runner_getCameraForView), and each Div, Mul, Add and Sub as vmDivNumbers / vmArithNumbers does it: an int32 made a
// real, then one operation on two reals, each behind an empty asm statement (the engine is built with -ffast-math).
//
// A line wholly above or below the view is left out (its string_width and draw) when its draw would provably draw
// nothing and change nothing: the renderer's own rectOutsideView (draw_circle's test) says a box far taller than any
// glyph the line can draw, at any x, is out of view; the projection's y axis is flipped (the Crisp text path culls
// such a line too, before its glyph state); the font is a text font (the renderer culls such a line before it touches
// anything) with left and top alignment; and the line's x and y are whole numbers (an x from round with a display
// scale of 1, a y checked), so no Crisp glyph would have taken a shake slot.
//
// Anything else (other argument types, non-ASCII text with a "#", a piece of 256 bytes or more, no camera, a zero view
// width or surface width, a builtin not yet in the call cache) runs the whole script in the interpreter from its start
// (aotx_executeLoop), which the front has changed nothing for: every check comes before the first draw, and the
// builtins it may have called by then only read.
//
// Binding: each script is found by its bytecode's length, its name's hash and its bytecode's digest (FNV-1a,
// z_dodgefront.c's), exactly one entry each; the five builtins by their FUNC names (each exactly once). Checked once
// per data.win, bound at each room change through the wrap chain (VM_aotBindFront, vm.c's late end), so they are back
// after any VM reset.
//
// This build is the only user: CMakeLists.txt compiles this file (with CTRPORT_DODGEFRONT) for UNDERTALE o3DS only.

#if defined(CTRPORT_DODGEFRONT) && defined(ENABLE_WAD16)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <citro3d.h>

#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "vm.h"
#include "vm_fastpath.h"

#define TF_CODE __attribute__((noinline, section(".text.textfront_late")))
#define TF_RO __attribute__((aligned(4), section(".rodata.textfront_in_text")))
#define TF_BSS __attribute__((section(".bss.textfront_late")))
#define TF_OPAQUE(v) __asm__("" : "+m"(v))
#define TF_LINE 256 // a "#" piece's buffer

typedef RValue (*TfFn)(VMContext* ctx);
bool VM_aotBindFront(VMContext* ctx, int32_t codeIndex, TfFn fn);
RValue aotx_executeLoop(VMContext* ctx);
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx);
extern const uint32_t Smalltext_layout[3]; // ctr_renderer.c: the renderer's projection (C3D_Mtx) first

// The builtins in tfFn's order, each ending in NUL.
static const char tfNames[] TF_RO = "surface_get_width\0round\0string_width\0draw_text_transformed\0string_height\0";
enum { TF_SURFACE_W, TF_ROUND, TF_WIDTH, TF_DRAW, TF_HEIGHT, TF_FNS };
// The centred script's and the scaled script's code entry: bytecode length, the name's FNV-1a (32-bit) hash, then the
// bytecode digest's low and high words. Only an entry whose length and name hash both match has its bytecode read
// for the digest (DataWin_codeBytes reads and keeps the entry's whole blob group): the same two entries
// z_dodgefront.c reads, so no other group is loaded.
static const uint32_t tfScript[2][4] TF_RO = {{100u, 0xCF6B4BB1u, 0x99853B3Eu, 0x6FFB95C7u},
                                              {728u, 0xC08485CCu, 0x9346B5EFu, 0x90A6810Eu}};
static const char tfSpace[] TF_RO = " ";
// The log line, once per data.win: the two code indices bound, or minus the lookup step that failed.
static const char tfLog[] TF_RO = "textfront: %ld %ld\n";

static DataWin* tfChecked TF_BSS; // the data.win the lookups below are for
static int32_t tfCode[2] TF_BSS;  // the scripts' code indices; tfCode[0] -1: not this game's
static int32_t tfFn[TF_FNS] TF_BSS; // the builtins' FUNC indices
static bool tfLogged TF_BSS;      // "bound" logged for this data.win

static inline __attribute__((always_inline)) RValue tfValue(uint8_t type, uint8_t stackType) {
    RValue rv = {0};
    rv.type = type;
    rv.gmlStackType = stackType;
    return rv;
}

static TF_CODE RValue tfReal(GMLReal v) {
    RValue rv = tfValue(RVALUE_REAL, GML_TYPE_DOUBLE);
    rv.real = v;
    return rv;
}

static inline __attribute__((always_inline)) RValue tfString(const char* s) {
    RValue rv = tfValue(RVALUE_STRING, GML_TYPE_STRING);
    rv.string = s;
    return rv;
}

// The lookups, once per data.win: 0 when all match, else the step that didn't (for the log).
static TF_CODE int32_t tfCheck(DataWin* dw) {
    for (int k = 0; 2 > k; k++) {
        uint64_t want = ((uint64_t) tfScript[k][3] << 32) | tfScript[k][2];
        int32_t found = -1;
        for (uint32_t i = 0; dw->code.count > i; i++) {
            CodeEntry* code = &dw->code.entries[i];
            if (code->length != tfScript[k][0] || code->offset > code->length || code->name == NULL) continue;
            uint32_t nh = 0x811C9DC5u;
            for (const char* c = code->name; *c != '\0'; c++) nh = (nh ^ (uint8_t) *c) * 0x01000193u;
            if (nh != tfScript[k][1]) continue;
            const uint8_t* bytes = DataWin_codeBytes(dw, code);
            if (bytes == NULL) continue;
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t j = code->offset; code->length > j; j++) {
                h ^= bytes[j];
                h *= 0x100000001B3ull;
            }
            if (h != want) continue;
            if (found >= 0) return 3 + k;
            found = (int32_t) i;
        }
        if (0 > found) return 1 + k;
        tfCode[k] = found;
    }
    const char* name = tfNames;
    __asm__("" : "+r"(name));
    for (int k = 0; TF_FNS > k; k++, name += strlen(name) + 1) {
        tfFn[k] = -1;
        for (uint32_t i = 0; dw->func.functionCount > i; i++) {
            const char* n = dw->func.functions[i].name;
            if (n == NULL || strcmp(n, name) != 0) continue;
            if (tfFn[k] >= 0) return 5 + k;
            tfFn[k] = (int32_t) i;
        }
        if (0 > tfFn[k]) return 5 + k;
    }
    return 0;
}

static TF_CODE GMLReal tfNum(const RValue* v) {
    return v->type == RVALUE_INT32 ? (GMLReal) v->int32 : v->real;
}

// Builtin k of one argument; for these four the result is always a real (RValue_makeReal), which owns nothing.
static TF_CODE GMLReal tfCall(VMContext* ctx, int k, RValue arg) {
    RValue r = ((BuiltinFunc) ctx->funcCallCache[tfFn[k]].builtin)(ctx, &arg, 1);
    return r.real;
}

// Whether v is finite with |v| < 2^e (a bit test: -ffast-math assumes there is no NaN in comparisons).
static TF_CODE bool tfSmall(float v, uint32_t e) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return 127u + e > ((bits >> 23) & 0xFFu);
}

// Whether text s drawn at whole y (and a whole x) would draw nothing and change nothing (see the top).
static TF_CODE bool tfOut(Renderer* rd, const char* s, GMLReal y, const RValue* yscale) {
    if (rd == NULL || rd->vtable->rectOutsideView == NULL || rd->drawHalign != 0 || rd->drawValign != 0) return false;
    DataWin* dw = rd->dataWin;
    int32_t fi = rd->drawFont;
    if (0 > fi || dw == NULL || (uint32_t) fi >= dw->font.count) return false;
    const Font* font = &dw->font.fonts[fi];
    const C3D_Mtx* m = (const C3D_Mtx*) ((const uint8_t*) rd + Smalltext_layout[0]);
    float fy = (float) y, sy = (float) tfNum(yscale) * font->scaleY, a = -(float) font->ascenderOffset;
    if (font->isSpriteFont || font->maxGlyphHeight == 0 || font->maxGlyphHeight > 1024u || !(0.0f > m->r[1].y) || !tfSmall(fy, 20)
        || fy != (float) (int32_t) fy || !tfSmall(sy, 8) || !tfSmall(a, 12))
        return false;
    float lines = 3.0f; // the lines the draw can make ("#" and line breaks), and two more glyph heights
    for (; *s != '\0'; s++) lines += (*s == '#' || *s == '\n' || *s == '\r') ? 1.0f : 0.0f;
    float h = (float) font->maxGlyphHeight;
    float y0 = fy + sy * (a - 2.0f * h - 64.0f), y1 = fy + sy * (a + lines * h + 64.0f);
    float lo = (y1 > y0 ? y0 : y1) - 64.0f, hi = (y1 > y0 ? y1 : y0) + 64.0f;
    return rd->vtable->rectOutsideView(rd, -1.0e6f, lo, 1.0e6f, hi);
}

// The scaled script on args xx, yy and text and the two scales (see the top).
static TF_CODE RValue tfRun(VMContext* ctx, const RValue* args, const RValue* xscale, const RValue* yscale) {
    Runner* runner = ctx->runner;
    const RValue* text = &args[2];
    if (runner == NULL || !vmIsPlainNumber(args[0].type) || !vmIsPlainNumber(args[1].type) || !vmIsPlainNumber(xscale->type)
        || !vmIsPlainNumber(yscale->type) || text->type != RVALUE_STRING || text->string == NULL)
        return aotx_executeLoop(ctx);
    // A "#" splits only ASCII text, into pieces shorter than TF_LINE; a single line may be anything.
    const char* s = text->string;
    bool multi = false, bad = false;
    uint32_t run = 0;
    for (const char* p = s; *p != '\0'; p++, run++) {
        bad |= (uint8_t) *p >= 128u;
        if (*p != '#') continue;
        multi = true;
        bad |= run + 1u >= TF_LINE;
        run = (uint32_t) -1;
    }
    bad &= multi;
    uint32_t n = ctx->funcCallCacheCount;
    for (int k = 0; (multi ? TF_FNS : TF_HEIGHT) > k; k++) {
        if ((uint32_t) tfFn[k] >= n || ctx->funcCallCache[tfFn[k]].builtin == NULL) bad = true;
    }
    GMLCamera* camera = bad ? NULL : Runner_getCameraForView(runner, runner->viewCurrent);
    if (camera == NULL || camera->viewWidth == 0) return aotx_executeLoop(ctx);
    GMLReal sw = tfCall(ctx, TF_SURFACE_W, tfReal((GMLReal) runner->applicationSurfaceId));
    if (sw == (GMLReal) 0) return aotx_executeLoop(ctx);
    // display_scale = surface_get_width(application_surface) / view_wview[view_current]
    GMLReal scale = sw / (GMLReal) camera->viewWidth;
    TF_OPAQUE(scale);
    // lineheight = round(string_height(" ") * yscale)
    GMLReal lh = 0;
    if (multi) {
        GMLReal t = tfCall(ctx, TF_HEIGHT, tfString(tfSpace)) * tfNum(yscale);
        TF_OPAQUE(t);
        lh = tfCall(ctx, TF_ROUND, tfReal(t));
    }
    // yy = round(yy * display_scale) / display_scale
    GMLReal t = tfNum(&args[1]) * scale;
    TF_OPAQUE(t);
    GMLReal y = tfCall(ctx, TF_ROUND, tfReal(t)) / scale;
    TF_OPAQUE(y);
    GMLReal xx = tfNum(&args[0]);
    bool xWhole = tfSmall((float) xx, 20); // then round's x is a whole number
    BuiltinFunc draw = (BuiltinFunc) ctx->funcCallCache[tfFn[TF_DRAW]].builtin;
    char buf[TF_LINE];
    for (;;) {
        const char* e = strchr(s, '#');
        const char* line = s;
        if (e != NULL) {
            size_t len = (size_t) (e - s) + 1;
            memcpy(buf, s, len);
            buf[len] = '\0';
            line = buf;
        }
        // The last line's x is round's; a "#" line's is round's over display_scale, whole when that is 1.
        if (!(xWhole && (e == NULL || scale == (GMLReal) 1) && tfOut(runner->renderer, line, y, yscale))) {
            // width = string_width(line) * xscale; x = round((xx - width / 2) * display_scale) / display_scale, or
            // round(xx - width / 2) for the last line
            GMLReal v = tfCall(ctx, TF_WIDTH, tfString(line)) * tfNum(xscale);
            TF_OPAQUE(v);
            v = v / (GMLReal) 2;
            TF_OPAQUE(v);
            v = xx - v;
            TF_OPAQUE(v);
            if (e != NULL) {
                v = v * scale;
                TF_OPAQUE(v);
            }
            GMLReal x = tfCall(ctx, TF_ROUND, tfReal(v));
            if (e != NULL) {
                x = x / scale;
                TF_OPAQUE(x);
            }
            // draw_text_transformed(x, yy, line, xscale, yscale, 0)
            RValue a[6];
            a[0] = tfReal(x);
            a[1] = tfReal(y);
            a[2] = tfString(line);
            vmWeakCopyInto(&a[3], xscale);
            vmWeakCopyInto(&a[4], yscale);
            a[5] = tfValue(RVALUE_INT32, GML_TYPE_INT32); // int32 0
            (void) draw(ctx, a, 6);
        }
        if (e == NULL) break;
        // yy += lineheight
        y = y + lh;
        TF_OPAQUE(y);
        s = e + 1;
    }
    return aotx_scriptFallthroughReturnValue(ctx);
}

static TF_CODE RValue tfCentred(VMContext* ctx) {
    if (ctx->scriptArgs == NULL || 3 > ctx->scriptArgCount) return aotx_executeLoop(ctx);
    RValue one = tfValue(RVALUE_INT32, GML_TYPE_INT32);
    one.int32 = 1;
    return tfRun(ctx, ctx->scriptArgs, &one, &one);
}

static TF_CODE RValue tfScaled(VMContext* ctx) {
    if (ctx->scriptArgs == NULL || 5 > ctx->scriptArgCount) return aotx_executeLoop(ctx);
    return tfRun(ctx, ctx->scriptArgs, &ctx->scriptArgs[3], &ctx->scriptArgs[4]);
}

// Binds first, so that z_dodgefront.c's front for the centred script (the next link) finds it taken.
TF_CODE void CTR_PREFETCHROOM_TEXTFRONT(Renderer* renderer) {
    Runner* runner = renderer != NULL ? renderer->runner : NULL;
    VMContext* ctx = runner != NULL ? runner->vmContext : NULL;
    if (ctx != NULL && ctx->dataWin != NULL) {
        int32_t step = 0;
        if (tfChecked != ctx->dataWin) {
            tfChecked = ctx->dataWin;
            tfCode[0] = -1;
            tfLogged = false;
            step = tfCheck(ctx->dataWin);
            if (step != 0) tfCode[0] = -1;
        }
        bool bound = tfCode[0] >= 0 && VM_aotBindFront(ctx, tfCode[0], tfCentred);
        bound = tfCode[0] >= 0 && VM_aotBindFront(ctx, tfCode[1], tfScaled) && bound;
        // Once per data.win: the first binding, or the lookup step that failed.
        if ((bound && !tfLogged) || step != 0) {
            tfLogged = true;
            logInfo(tfLog, (long) (bound ? tfCode[0] : -step), (long) (bound ? tfCode[1] : 0));
        }
    }
    CTR_PREFETCHROOM_AFTER_TEXTFRONT(renderer);
}

#endif
