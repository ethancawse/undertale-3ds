// writerfront (freebies, UNDERTALE Old 3DS only): the dialogue writer's Draw (the base writer object's Draw 0,
// every text box's typed text) run as hand code for the strings it can run exactly, with the generated function as
// the fallback for everything else. Late code and constants only (platform/3ds/late_reserve.ld): no table row, nothing
// that existed moves.
//
// Why: the writer's Draw walks its string from the first character to stringpos every frame. Per character the
// generated function calls string_char_at (a new string each time), compares it with some forty one-letter strings
// (the escapes and the kerning tables) and looks up a dozen instance variables, about 2,900 ARM instructions a glyph
// over 36 KiB of code lines: on the Old 3DS's 16 KiB instruction cache, 72-86% of a writer's time is fetch stalls
// (freebies round 3: nx_lem's 12 writers 1.5-2.1 ms each, one Toriel box 6-8 ms).
//
// What: the bytecode's steps, in its order, for one character at a time. The writer starts at its first position and
// walks the string up to the typed position. A caret before anything but 0 is skipped. A backslash escape sets the
// colour, a face's emotion or choice (from the next character) or the half-size state, or skips one character;
// take the fallback. An ampersand runs the newline script (back to the left edge, one line down). A slash halts the
// writer (with the halt kind the next character gives) and ends the Draw; a percent sign takes the fallback.
// Any other character is a letter: a new line comes first if it would pass the right edge; then its place, half
// size and offset, the kerning before it (one typer's font), the font and colour, the shake (a random jitter, or for
// shake values 39-43 a motion of their own), the place snapped to the display scale's pixels, the draw of the letter
// with draw_text_transformed, the spacing and the kerning after it (two fonts), and the next letter's place
// (halfway for half size). The front covers that
// for vtext false and global.language "en" (otherwise the fallback). Every value is an RValue made as the interpreter makes
// it: each Add, Sub, Mul and Div through vmArithNumbers / vmDivNumbers with the instruction's result type and behind an
// empty asm statement (-ffast-math), each store through coerceIntStoreToRealInPlace with the instruction's type2 and
// storeStackValueIntoSlot, each comparison through vmCmpNumbers' epsilon, each read a weak copy (so an int32 myx stays
// an int32 and a pushed int stored by Pop.v.i becomes a real, as there); the builtins from the same call cache in the
// same order with the same arguments (random's RNG draws included, at shake 0 too); direction, hspeed and vspeed through
// VMBuiltins_getVariable / VMBuiltins_setVariable, where Push and Pop.v.v on self reach them. What it leaves out only reads:
// string_char_at and its strings (a character is a byte of the string, which is checked to be ASCII, past its end ""),
// the string comparisons, scr_setfont's call (with language "en" it is draw_set_font(myfont)) and script_execute's
// (the newline script's body runs inline). The event's locals stay unused.
//
// The fallback: before anything is written, a read-only pass over the characters up to stringpos decides; anything
// it does not take (an escape above, a "%" before a "/", non-ASCII text, vtext, another language, a
// variable missing (myx and myy before a writer's first Draw; a face global only for a \E or \F) or of another type,
// no camera) calls the generated function from the start, which the front has changed nothing for.
//
// Binding: the three code entries (the Draw, the newline script as 153, scr_setfont as the call's script) by their
// bytecode's length, name hash and digest (FNV-1a, z_dodgefront.c's), each exactly once; the variables by name and
// scope in VARI (one varID each); the builtins by FUNC name. Checked once per data.win. The fallback is the generated
// function aotBind would bind (wfTable: the first aotTable entry of the Draw's name, its code, variables and functions
// matching; no front if that entry doesn't). Bound through the wrap chain at each room change until it holds
// (VM_aotSwapFront, vm.c's late end: over that function or the entry not looked up yet), so from the first room
// change on, before the Draw's first run.
//
// Diagnostic words (build.sh): writerfront-fallback makes every Draw take the fallback after the checks (identity with
// stock, bit for bit); writerfront-count logs the counts at each room change and each distinct fallback string once.

#if defined(CTRPORT_WRITERFRONT) && defined(ENABLE_WAD16)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "data_win.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"
#include "vm_fastpath.h"
#include "aot/vm_aot.h"

#if defined(__3DS__)
#include <citro3d.h>

#include "ctr_wrap_chain.h"
#include "renderer.h"
#define WF_CODE __attribute__((noinline, section(".text.writerfront_late")))
#define WF_RO __attribute__((aligned(4), section(".rodata.writerfront_in_text")))
#define WF_BSS __attribute__((section(".bss.writerfront_late")))
#else
#define WF_CODE __attribute__((noinline))
#define WF_RO
#define WF_BSS
#endif
#define WF_OPQ(v) __asm__("" : "+m"(v))
#define WF_DIV 3

typedef RValue (*WfFn)(VMContext* ctx);
bool VM_aotSwapFront(VMContext* ctx, int32_t codeIndex, WfFn fn, WfFn generated);
bool WriterFront_bind(VMContext* ctx);
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx);

// Instance variables, globals and builtins in the orders below, each name ending in NUL.
static const char wfNames[] WF_RO = "vtext\0writingxend\0vspacing\0writingx\0writingy\0myx\0myy\0stringpos\0"
                                    "originalstring\0mycolor\0myfont\0shake\0spacing\0htextscale\0vtextscale\0halt\0"
                                    "language\0typer\0faceemotion\0facechoice\0facechange\0"
                                    "random\0round\0surface_get_width\0draw_set_color\0draw_set_font\0"
                                    "draw_text_transformed\0real\0scr_setfont\0";
enum { V_VTEXT, V_WXEND, V_VSPACING, V_WX, V_WY, V_MYX, V_MYY, V_SPOS, V_ORIG, V_COLOR, V_FONT, V_SHAKE, V_SPACING,
       V_HSCALE, V_VSCALE, V_HALT, V_N };
enum { G_LANGUAGE, G_TYPER, G_FACEEMOTION, G_FACECHOICE, G_FACECHANGE, G_N };
enum { F_RANDOM, F_ROUND, F_SURFW, F_COLOR, F_FONT, F_DRAW, F_REAL, F_SETFONT, F_N };
// The Draw, the newline script and scr_setfont: bytecode length, the name's FNV-1a (32-bit) hash, the digest's low
// and high words (UNDERTALE 1.08; the digest is aotMatches' and z_dodgefront.c's).
static const uint32_t wfPin[3][4] WF_RO = {{9404u, 0xC36C3191u, 0x267C3FF9u, 0xC5D6FCAAu},
                                           {108u, 0x3AEDA49Bu, 0x39895EB8u, 0x620EB407u},
                                           {156u, 0x28C9909Au, 0xB7DBE923u, 0x194DC81Au}};
// Constants as the bytecode pushes them and Pop.v.i leaves them: reals typed double; ints for Add / Cmp / Pop.v.i.
enum { K_0, K_1, K_HALF, K_033, K_2, K_I1, K_18, K_38, K_8, K_9, K_07 };
static const RValue wfK[] WF_RO = {
    {.real = 0.0, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE},
    {.real = 1.0, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE},
    {.real = 0.5, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE},
    {.real = 0.33, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE},
    {.real = 2.0, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE},
    {.int32 = 1, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32},
    {.int32 = 18, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32},
    {.int32 = 38, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32},
    {.int32 = 8, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32},
    {.int32 = 9, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32},
    {.real = 0.7, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_DOUBLE},
};
// The colours of \R \G \W \Y \X \B \O \L \P \p, in that order.
static const char wfColorKeys[] WF_RO = "RGWYXBOLPp";
static const int32_t wfColors[10] WF_RO = {255, 65280, 16777215, 65535, 0, 16711680, 4235519, 16629774, 16711935,
                                           13941759};
// Kerning: a letter, then its step + 8; papyrus (typer 18) before the draw, fnt 8 and fnt 9 after it.
static const char wfKernPap[] WF_RO = "l\x0Ai\x0AI\x0A!\x0A.\x0AS\x09?\x0A" "D\x09" "A\x09'\x09";
static const char wfKern8[] WF_RO = "w\x0Am\x0Ai\x06l\x06s\x07j\x07";
static const char wfKern9[] WF_RO = "D\x09Q\x0BM\x09L\x07K\x07" "C\x09.\x05!\x05O\x0AW\x0AI\x02T\x07P\x06R\x06"
                                    "A\x09H\x09" "B\x09G\x09" "F\x07?\x05'\x02J\x07";
static const char wfLog[] WF_RO = "writerfront: %ld\n";
#if defined(CTRPORT_WRITERFRONT_COUNT)
// The counts so far at each room change that follows a Draw (room = the runner's room then), and each distinct string
// the fallback took, once (its hash in wfSeen; 256 at most, then no more lines): the dialogue coverage.
static const char wfCountLog[] WF_RO = "writerfront: room %ld: %lu front, %lu fallback (setup %lu, text %lu)\n";
static const char wfSeenLog[] WF_RO = "writerfront: fallback (%s) \"%.80s\"\n";
static const char wfVarLog[] WF_RO = "writerfront: fallback (variable %d: %s, type %d, owns %d, object %ld)\n";
static const char wfWhyLog[] WF_RO = "writerfront: fallback (setup %d: vtext %d, shake %d, language %d, camera %d, ascii %d, object %ld)\n";
#endif

static DataWin* wfChecked WF_BSS; // the data.win the lookups below are for
static int32_t wfCode WF_BSS;     // the Draw's code index, or -1: not this game's
static int32_t wfSetfont WF_BSS;  // scr_setfont's code index
static int32_t wfId[V_N + G_N + F_N] WF_BSS; // varIDs (self, then global), then FUNC indices
static WfFn wfGen WF_BSS;         // the generated function: the fallback
static bool wfLogged WF_BSS;
#if defined(CTRPORT_WRITERFRONT_HARNESS)
int wfHarnessForceGen; // desktop harness: 1 runs the generated function after the checks
int wfHarnessTook;     // whether the last Draw's checks took the front
#endif
#if defined(CTRPORT_WRITERFRONT_COUNT)
static uint32_t wfCount[4] WF_BSS; // front, fallback, of which setup checks, of which the text pass
static uint32_t wfCountLogged WF_BSS; // front + fallback at the last count line
static uint32_t wfSeen[256] WF_BSS;   // the fallback strings logged (FNV-1a), 0 = free
static uint8_t wfVarSeen[V_N + G_N][16] WF_BSS; // variable checks logged: per variable, per type (15 = missing)
static uint8_t wfWhySeen[32] WF_BSS; // setup checks after the variables, logged once per set of failing checks
#endif

// ---[ values, as the interpreter makes them ]---

// d = a <op> b, typed as a variable: VM_ARITH_ADD / SUB / MUL (vmArithNumbers) or WF_DIV (vmDivNumbers). Every
// operand here is a plain number (checked on entry), so the slow paths never run.
static WF_CODE void wfOp(RValue* d, const RValue* a, const RValue* b, int op) {
    RValue t = *a;
    if (op == WF_DIV) (void) vmDivNumbers(&t, b, false, GML_TYPE_VARIABLE, nullptr);
    else (void) vmArithNumbers(&t, b, op, GML_TYPE_VARIABLE);
    WF_OPQ(t);
    *d = t;
}

// Cmp of two plain numbers (vmCmpNumbers' epsilon).
static WF_CODE bool wfCmp(const RValue* a, const RValue* b, uint8_t kind) { return vmCmpNumbers(a, b, kind) == 1; }

// Push of v, then Pop into a slot with the instruction's type2 (a pushed int stored by Pop.v.i becomes a real). v is
// always a plain number and the slot owns nothing (checked on entry), so this is storeStackValueIntoSlot's whole work.
static WF_CODE void wfSet(RValue* slot, const RValue* v, uint8_t type2) {
    RValue t;
    vmWeakCopyInto(&t, v);
    coerceIntStoreToRealInPlace(&t, type2);
    *slot = t;
}

// d = builtin k (from the call cache) of argc arguments; every builtin called here returns a value that owns nothing.
static WF_CODE void wfCall(VMContext* ctx, RValue* d, int k, RValue* args, int32_t argc) {
    *d = ((BuiltinFunc) ctx->funcCallCache[wfId[V_N + G_N + k]].builtin)(ctx, args, argc);
    d->gmlStackType = GML_TYPE_VARIABLE;
}

// d = builtin k of a weak copy of arg.
static WF_CODE void wfCall1(VMContext* ctx, RValue* d, int k, const RValue* arg) {
    RValue a;
    vmWeakCopyInto(&a, arg);
    wfCall(ctx, d, k, &a, 1);
}

// x += step (Add of a pushed int) or x -= -step (Sub) for letter c in kerning table t.
static WF_CODE void wfKern(RValue* x, const char* t, char c) {
    for (; *t != '\0'; t += 2) {
        if (*t != c) continue;
        int32_t step = (int32_t) (uint8_t) t[1] - 8;
        RValue k = {.int32 = step > 0 ? step : -step, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32};
        wfOp(x, x, &k, step > 0 ? VM_ARITH_ADD : VM_ARITH_SUB);
        return;
    }
}

// o = o + (random(shake) - shake / 2).
static WF_CODE void wfShake(VMContext* ctx, RValue* o, const RValue* shake) {
    RValue r, h;
    wfCall1(ctx, &r, F_RANDOM, shake);
    wfOp(&h, shake, &wfK[K_2], WF_DIV);
    wfOp(&r, &r, &h, VM_ARITH_SUB);
    wfOp(o, o, &r, VM_ARITH_ADD);
}

// A builtin on self (direction, hspeed, vspeed) as Push reads it.
static WF_CODE RValue wfBltn(VMContext* ctx, int16_t id) {
    RValue v = VMBuiltins_getVariable(ctx, (Instance*) ctx->currentInstance, id, nullptr, -1);
    v.gmlStackType = GML_TYPE_VARIABLE;
    return v;
}

// direction = direction <op> k (39: k pushed as is) or direction <op> k * n (Mul typed int32), through the setter.
static WF_CODE void wfTurn(VMContext* ctx, int32_t k, const RValue* nv, int op) {
    RValue d = wfBltn(ctx, BUILTIN_VAR_DIRECTION), m = {.int32 = k, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT16}, n;
    if (nv != nullptr) {
        vmWeakCopyInto(&n, nv);
        (void) vmArithNumbers(&m, &n, VM_ARITH_MUL, GML_TYPE_INT32);
        WF_OPQ(m);
    }
    wfOp(&d, &d, &m, op);
    VMBuiltins_setVariable(ctx, (Instance*) ctx->currentInstance, BUILTIN_VAR_DIRECTION, nullptr, d, -1);
}

// shake above 38 (the bytecode from 0x18B8; no RNG draw): see the top.
static WF_CODE void wfMotion(VMContext* ctx, const RValue* shake, const RValue* nv, RValue* ox, RValue* oy) {
    int32_t s = 39;
    for (; 43 >= s; s++) {
        RValue k = {.int32 = s, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT16};
        if (wfCmp(shake, &k, CMP_EQ)) break;
    }
    if (s > 43) return;
    int32_t k = s == 39 ? 10 : (s - 40) * 10;
    if (s != 40) wfTurn(ctx, k, s == 39 ? nullptr : nv, VM_ARITH_ADD);
    RValue h = wfBltn(ctx, BUILTIN_VAR_HSPEED);
    if (s == 43) {
        RValue ten = {.int32 = 10, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT16};
        wfOp(&h, &h, &wfK[K_07], VM_ARITH_MUL);
        wfOp(&h, &h, &ten, VM_ARITH_ADD);
    }
    wfOp(ox, ox, &h, VM_ARITH_ADD);
    h = wfBltn(ctx, BUILTIN_VAR_VSPEED);
    if (s == 43) wfOp(&h, &h, &wfK[K_07], VM_ARITH_MUL);
    wfOp(oy, oy, &h, VM_ARITH_ADD);
    if (s > 40) wfTurn(ctx, k, nv, VM_ARITH_SUB);
}

// The newline script's body for vtext 0: back to the left edge, one line down.
static WF_CODE void wfNewline(RValue** sv) {
    RValue t;
    wfSet(sv[V_MYX], sv[V_WX], GML_TYPE_VARIABLE);
    wfOp(&t, sv[V_MYY], sv[V_VSPACING], VM_ARITH_ADD);
    wfSet(sv[V_MYY], &t, GML_TYPE_VARIABLE);
}

// d = round(v * ds) / ds.
static WF_CODE void wfSnap(VMContext* ctx, RValue* d, const RValue* v, const RValue* ds) {
    RValue t;
    wfOp(&t, v, ds, VM_ARITH_MUL);
    wfCall1(ctx, &t, F_ROUND, &t);
    wfOp(d, &t, ds, WF_DIV);
}

// One glyph (the bytecode from 0x1274) for letter, which character n (nv) is (after a "^0", the "^").
static WF_CODE void wfGlyph(VMContext* ctx, RValue** sv, RValue** gv, char letter, bool halfsize, const RValue* vw,
                            const RValue* nv) {
    RValue lx, ox = wfK[K_0], oy = wfK[K_0], hs = wfK[halfsize ? K_HALF : K_1], t, ds, a[6];
    if (wfCmp(sv[V_MYX], sv[V_WXEND], CMP_GT)) wfNewline(sv);
    vmWeakCopyInto(&lx, sv[V_MYX]);
    if (halfsize) {
        wfOp(&t, sv[V_VSPACING], &wfK[K_033], VM_ARITH_MUL);
        wfOp(&oy, &oy, &t, VM_ARITH_ADD);
    }
    if (wfCmp(gv[G_TYPER], &wfK[K_18], CMP_EQ)) wfKern(&lx, wfKernPap, letter);
    // The font script with language "en" just sets the writer's font (draw_set_font).
    wfCall1(ctx, &t, F_FONT, sv[V_FONT]);
    wfCall1(ctx, &t, F_COLOR, sv[V_COLOR]);
    if (wfCmp(sv[V_SHAKE], &wfK[K_38], CMP_GT)) {
        wfMotion(ctx, sv[V_SHAKE], nv, &ox, &oy);
    } else {
        wfShake(ctx, &ox, sv[V_SHAKE]);
        wfShake(ctx, &oy, sv[V_SHAKE]);
    }
    // The display scale: the application surface's width over the view's width.
    RValue surf = {.real = (GMLReal) ctx->runner->applicationSurfaceId, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_VARIABLE};
    wfCall(ctx, &t, F_SURFW, &surf, 1);
    wfOp(&ds, &t, vw, WF_DIV);
    wfOp(&t, &lx, &ox, VM_ARITH_ADD);
    wfSnap(ctx, &a[0], &t, &ds);
    wfOp(&t, sv[V_MYY], &oy, VM_ARITH_ADD);
    wfSnap(ctx, &a[1], &t, &ds);
    a[2] = (RValue) {.string = RValue_asciiStrings[(uint8_t) letter], .type = RVALUE_STRING, .gmlStackType = GML_TYPE_VARIABLE};
    wfOp(&a[3], sv[V_HSCALE], &hs, VM_ARITH_MUL);
    wfOp(&a[4], sv[V_VSCALE], &hs, VM_ARITH_MUL);
    vmWeakCopyInto(&a[5], &wfK[K_0]);
    wfCall(ctx, &t, F_DRAW, a, 6);
    wfOp(&lx, &lx, sv[V_SPACING], VM_ARITH_ADD);
    if (wfCmp(sv[V_FONT], &wfK[K_8], CMP_EQ)) wfKern(&lx, wfKern8, letter);
    else if (wfCmp(sv[V_FONT], &wfK[K_9], CMP_EQ)) wfKern(&lx, wfKern9, letter);
    if (halfsize) {
        // Half size: the next letter goes halfway, rounded.
        wfOp(&t, &lx, sv[V_MYX], VM_ARITH_SUB);
        wfOp(&t, &t, &wfK[K_2], WF_DIV);
        wfOp(&t, sv[V_MYX], &t, VM_ARITH_ADD);
        wfCall1(ctx, &lx, F_ROUND, &t);
    }
    wfSet(sv[V_MYX], &lx, GML_TYPE_VARIABLE);
}

// Character n (1-based) of an ASCII string of length len, 0 past either end ("": unequal to every character here).
static WF_CODE char wfAt(const char* s, int32_t len, int32_t n) { return 0 >= n || n > len ? '\0' : s[n - 1]; }

// One walk over the characters up to stringpos (see the top). With run false it writes nothing and says whether the
// front takes them; with run true it is the Draw. n stays a whole number counted from 1, a real as Pop.v.i leaves it.
static WF_CODE bool wfWalk(VMContext* ctx, RValue** sv, RValue** gv, const char* s, int32_t len, bool run, const RValue* vw) {
    bool halfsize = false;
    RValue nv = wfK[K_1], t;
    if (run) {
        wfSet(sv[V_MYX], sv[V_WX], GML_TYPE_VARIABLE);
        wfSet(sv[V_MYY], sv[V_WY], GML_TYPE_VARIABLE);
    }
    for (;;) {
        if (!wfCmp(&nv, sv[V_SPOS], CMP_LTE)) return true;
        int32_t n = (int32_t) nv.real;
        if (n > (1 << 20)) return false;
        char ch = wfAt(s, len, n);
        if (ch == '\\') {
            wfOp(&nv, &nv, &wfK[K_I1], VM_ARITH_ADD);
            char c = wfAt(s, len, ++n);
            const char* key = c != '\0' ? strchr(wfColorKeys, c) : nullptr;
            if (key != nullptr) {
                t = (RValue) {.int32 = wfColors[key - wfColorKeys], .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32};
                if (run) wfSet(sv[V_COLOR], &t, GML_TYPE_INT32);
            } else if (c == 'E' || c == 'F' || c == 'S' || c == 'T') {
                wfOp(&nv, &nv, &wfK[K_I1], VM_ARITH_ADD);
                char d = wfAt(s, len, ++n);
                if (c == 'T') {
                    if (d != '-' && d != '+') return false;
                    halfsize = d == '-';
                } else if (c != 'S') {
                    if (d < '0' || d > '9' || gv[c == 'E' ? G_FACEEMOTION : G_FACECHOICE] == nullptr
                        || (c == 'F' && gv[G_FACECHANGE] == nullptr))
                        return false;
                    if (run) {
                        RValue arg = {.string = RValue_asciiStrings[(uint8_t) d], .type = RVALUE_STRING, .gmlStackType = GML_TYPE_VARIABLE};
                        wfCall(ctx, &t, F_REAL, &arg, 1);
                        wfSet(gv[c == 'E' ? G_FACEEMOTION : G_FACECHOICE], &t, GML_TYPE_VARIABLE);
                        if (c == 'F') wfSet(gv[G_FACECHANGE], &wfK[K_I1], GML_TYPE_INT32);
                    }
                }
            } else if (c == 'C' || c == 'M' || c == 'z' || c == '*' || c == '>') {
                return false;
            }
        } else if (ch == '&') {
            if (run) wfNewline(sv);
        } else if (ch == '/') {
            // halt = 1, then 2 ("/%"), 4 ("/^" not before "0") or 6 ("/*"); the Draw ends
            if (run) {
                char next = wfAt(s, len, n + 1);
                int32_t halt = next == '%' ? 2 : (next == '^' && wfAt(s, len, n + 2) != '0') ? 4 : next == '*' ? 6 : 1;
                wfSet(sv[V_HALT], &wfK[K_I1], GML_TYPE_INT32);
                t = (RValue) {.int32 = halt, .type = RVALUE_INT32, .gmlStackType = GML_TYPE_INT32};
                if (halt != 1) wfSet(sv[V_HALT], &t, GML_TYPE_INT32);
            }
            return true;
        } else if (ch == '%') {
            return false;
        } else if (ch == '^' && wfAt(s, len, n + 1) != '0') {
            wfOp(&nv, &nv, &wfK[K_I1], VM_ARITH_ADD);
        } else {
            if (ch == '^') wfOp(&nv, &nv, &wfK[K_I1], VM_ARITH_ADD);
            if (run) wfGlyph(ctx, sv, gv, ch, halfsize, vw, &nv);
        }
        wfOp(&nv, &nv, &wfK[K_I1], VM_ARITH_ADD);
    }
}

static WF_CODE RValue wfDraw(VMContext* ctx) {
    Instance* self = (Instance*) ctx->currentInstance;
    Instance* glob = (Instance*) ctx->globalScopeInstance;
    Runner* runner = ctx->runner;
    RValue* sv[V_N + G_N];
    RValue** gv = &sv[V_N];
    bool ok = self != nullptr && glob != nullptr && runner != nullptr && wfGen != nullptr;
    for (int k = 0; ok && V_N + G_N > k; k++) {
        sv[k] = IntRValueHashMap_findSlot(k >= V_N ? &glob->selfVars : &self->selfVars, wfId[k]);
        // every variable there but the face globals, which the game makes at their first \E or \F (none in battles
        // before one: wfWalk hands such a string to the fallback). Every instance variable but originalstring and halt
        // a plain number; of the globals, typer. halt and the face globals own nothing (wfSet overwrites them without
        // a free)
        if (sv[k] == nullptr) {
            ok = k >= V_N + G_FACEEMOTION;
            continue;
        }
        ok = (k == V_ORIG || k == V_HALT || (k >= V_N && k != V_N + G_TYPER) || vmIsPlainNumber(sv[k]->type))
             && (k == V_ORIG || k == V_N + G_LANGUAGE || !sv[k]->ownsReference);
    }
    const char* s = ok ? sv[V_ORIG]->string : nullptr;
    // vtext false
    ok = ok && sv[V_ORIG]->type == RVALUE_STRING && s != nullptr && !(sv[V_VTEXT]->type == RVALUE_INT32 ? sv[V_VTEXT]->int32 > 0 : sv[V_VTEXT]->real > (GMLReal) 0.5)
         && gv[G_LANGUAGE]->type == RVALUE_STRING
         && gv[G_LANGUAGE]->string != nullptr && strcmp(gv[G_LANGUAGE]->string, "en") == 0;
    GMLCamera* camera = ok ? Runner_getCameraForView(runner, runner->viewCurrent) : nullptr;
    int32_t len = 0;
    if (camera == nullptr) ok = false;
    for (; ok && s[len] != '\0'; len++) ok = (uint8_t) s[len] < 128u && len < (1 << 20);
    RValue vw = {.real = camera != nullptr ? (GMLReal) camera->viewWidth : 0.0, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_VARIABLE};
#if defined(CTRPORT_WRITERFRONT_COUNT)
    bool setupOk = ok;
#endif
    ok = ok && wfWalk(ctx, sv, gv, s, len, false, &vw);
#if defined(CTRPORT_WRITERFRONT_HARNESS)
    wfHarnessTook = ok;
    if (wfHarnessForceGen) ok = false;
#endif
#if defined(CTRPORT_WRITERFRONT_FALLBACK)
    ok = false;
#endif
#if defined(CTRPORT_WRITERFRONT_COUNT)
    wfCount[ok ? 0 : 1]++;
    if (!ok) wfCount[setupOk ? 3 : 2]++;
    if (!ok && s == nullptr && self != nullptr && glob != nullptr) {
        const char* name = wfNames;
        for (int k = 0; V_N + G_N > k; k++, name += strlen(name) + 1) {
            RValue* v = IntRValueHashMap_findSlot(k >= V_N ? &glob->selfVars : &self->selfVars, wfId[k]);
            if (v == nullptr && k >= V_N + G_FACEEMOTION) continue;
            bool good = v != nullptr && (k == V_ORIG || k == V_HALT || (k >= V_N && k != V_N + G_TYPER) || vmIsPlainNumber(v->type))
                        && (k == V_ORIG || k == V_N + G_LANGUAGE || !v->ownsReference);
            if (good) continue;
            int type = v == nullptr ? 15 : v->type & 15;
            if (!wfVarSeen[k][type]) {
                wfVarSeen[k][type] = 1;
                logInfo(wfVarLog, k, name, v == nullptr ? -1 : (int) v->type, v != nullptr && v->ownsReference,
                        (long) self->objectIndex);
            }
            break;
        }
    }
    if (!setupOk && s != nullptr && sv[V_ORIG]->type == RVALUE_STRING) {
        bool vt = sv[V_VTEXT]->type == RVALUE_INT32 ? sv[V_VTEXT]->int32 > 0 : sv[V_VTEXT]->real > (GMLReal) 0.5;
        bool sh = wfCmp(sv[V_SHAKE], &wfK[K_38], CMP_GT);
        bool la = gv[G_LANGUAGE]->type == RVALUE_STRING && gv[G_LANGUAGE]->string != nullptr
                  && strcmp(gv[G_LANGUAGE]->string, "en") == 0;
        bool ca = camera != nullptr;
        bool as = true;
        for (const char* c = s; *c != '\0'; c++) as = as && (uint8_t) *c < 128u;
        int why = (int) vt | (int) sh << 1 | (int) !la << 2 | (int) !ca << 3 | (int) !as << 4;
        if (!wfWhySeen[why]) {
            wfWhySeen[why] = 1;
            logInfo(wfWhyLog, why, (int) vt,
                    (int) (sv[V_SHAKE]->type == RVALUE_INT32 ? sv[V_SHAKE]->int32 : (int32_t) sv[V_SHAKE]->real), (int) la,
                    (int) ca, (int) as, self != nullptr ? (long) self->objectIndex : -1L);
        }
    }
    if (!ok && s != nullptr && sv[V_ORIG]->type == RVALUE_STRING) {
        uint32_t h = 2166136261u;
        for (const char* c = s; *c != '\0'; c++) h = (h ^ (uint8_t) *c) * 16777619u;
        h |= 1u;
        int k = 0;
        while (256 > k && wfSeen[k] != 0 && wfSeen[k] != h) k++;
        if (256 > k && wfSeen[k] == 0) {
            wfSeen[k] = h;
            logInfo(wfSeenLog, setupOk ? "text" : "setup", s);
        }
    }
#endif
    if (!ok) return wfGen(ctx);
    (void) wfWalk(ctx, sv, gv, s, len, true, &vw);
    return aotx_scriptFallthroughReturnValue(ctx);
}

// The lookups, once per data.win: 0 when all match, else the step that didn't (for the log).
static WF_CODE int32_t wfCheck(DataWin* dw) {
    int32_t found[3];
    for (int k = 0; 3 > k; k++) {
        found[k] = -1;
        for (uint32_t i = 0; dw->code.count > i; i++) {
            CodeEntry* code = &dw->code.entries[i];
            if (code->length != wfPin[k][0] || code->offset > code->length || code->name == nullptr) continue;
            uint32_t nh = 0x811C9DC5u;
            for (const char* c = code->name; *c != '\0'; c++) nh = (nh ^ (uint8_t) *c) * 0x01000193u;
            const uint8_t* bytes = nh == wfPin[k][1] ? DataWin_codeBytes(dw, code) : nullptr;
            if (bytes == nullptr) continue;
            uint64_t h = 0xCBF29CE484222325ull;
            for (uint32_t j = code->offset; code->length > j; j++) h = (h ^ bytes[j]) * 0x100000001B3ull;
            if (h != (((uint64_t) wfPin[k][3] << 32) | wfPin[k][2])) continue;
            if (found[k] >= 0) return 4;
            found[k] = (int32_t) i;
        }
        if (0 > found[k]) return 1 + k;
    }
    // script_execute(153) is the newline script
    if (153 >= dw->scpt.count || dw->scpt.scripts[153].codeId != found[1]) return 5;
    const char* name = wfNames;
    __asm__("" : "+r"(name));
    for (int k = 0; V_N + G_N + F_N > k; k++, name += strlen(name) + 1) {
        int32_t id = -1;
        if (k >= V_N + G_N) {
            for (uint32_t i = 0; dw->func.functionCount > i; i++) {
                const char* n = dw->func.functions[i].name;
                if (n == nullptr || strcmp(n, name) != 0) continue;
                if (id >= 0) return 6;
                id = (int32_t) i;
            }
        } else {
            int32_t want = k >= V_N ? INSTANCE_GLOBAL : INSTANCE_SELF;
            for (uint32_t i = 0; dw->vari.variableCount > i; i++) {
                Variable* v = &dw->vari.variables[i];
                if (v->instanceType != want || v->name == nullptr || strcmp(v->name, name) != 0) continue;
                if (0 > v->varID || (id >= 0 && id != v->varID)) return 7;
                id = v->varID;
            }
        }
        if (0 > id) return 8;
        wfId[k] = id;
    }
    // the generated function: aotBind's first entry of this name, which must match as aotMatches checks
    CodeEntry* code = &dw->code.entries[found[0]];
    for (uint32_t e = 0; aotTable.entryCount > e; e++) {
        const AotEntry* en = &aotTable.entries[e];
        if (en->fn == nullptr || strcmp(en->codeName, code->name) != 0) continue;
        if (en->offset != code->offset || en->blobLength != code->length || en->end != code->length
            || en->digest != (((uint64_t) wfPin[0][3] << 32) | wfPin[0][2]))
            return 9;
        for (uint32_t i = 0; en->varCount > i; i++) {
            const AotVarRef* r = &en->vars[i];
            Variable* v = r->index < dw->vari.variableCount ? &dw->vari.variables[r->index] : nullptr;
            if (v == nullptr || v->varID != r->varID || v->instanceType != r->instanceType || v->name == nullptr
                || strcmp(v->name, r->name) != 0)
                return 10;
        }
        for (uint32_t i = 0; en->funcCount > i; i++) {
            const AotFuncRef* r = &en->funcs[i];
            if (r->index >= dw->func.functionCount || dw->func.functions[r->index].name == nullptr
                || strcmp(dw->func.functions[r->index].name, r->name) != 0)
                return 11;
        }
        wfGen = en->fn;
        break;
    }
    if (wfGen == nullptr) return 12;
    wfSetfont = found[2];
    wfCode = found[0];
    return 0;
}

// Checks (once per data.win) and binds the front over the Draw's generated function (or its entry not looked up yet);
// whether the front is bound.
WF_CODE bool WriterFront_bind(VMContext* ctx) {
    if (ctx == nullptr || ctx->dataWin == nullptr) return false;
    DataWin* dw = ctx->dataWin;
    int32_t step = 0;
    if (wfChecked != dw) {
        wfChecked = dw;
        wfCode = -1;
        wfGen = nullptr;
        wfLogged = false;
        step = wfCheck(dw);
        if (step != 0) wfCode = -1;
    }
    bool bound = wfCode >= 0 && IS_WAD16_OR_HIGHER(ctx) && ctx->funcCallCacheCount == dw->func.functionCount;
    // the builtins cached; scr_setfont the code entry wfCheck pinned (its call is left out)
    for (int k = 0; bound && F_N > k; k++) {
        FuncCallCache* cache = &ctx->funcCallCache[wfId[V_N + G_N + k]];
        bound = k == F_SETFONT ? cache->builtin == nullptr && cache->scriptCodeIndex == wfSetfont : cache->builtin != nullptr;
    }
    bound = bound && VM_aotSwapFront(ctx, wfCode, wfDraw, wfGen);
    if ((bound && !wfLogged) || step != 0) {
        wfLogged = true;
        logInfo(wfLog, (long) (bound ? wfCode : -step));
    }
    return bound;
}

#if defined(__3DS__)
// The prefetchRoom chain's link after textfront's: binds the front after each room change, then the next link.
WF_CODE void CTR_PREFETCHROOM_WRITERFRONT(Renderer* renderer) {
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    if (runner != nullptr && runner->vmContext != nullptr) (void) WriterFront_bind(runner->vmContext);
#if defined(CTRPORT_WRITERFRONT_COUNT)
    if (runner != nullptr && wfCount[0] + wfCount[1] != wfCountLogged) {
        wfCountLogged = wfCount[0] + wfCount[1];
        logInfo(wfCountLog, (long) runner->currentRoomIndex, (unsigned long) wfCount[0], (unsigned long) wfCount[1],
                (unsigned long) wfCount[2], (unsigned long) wfCount[3]);
    }
#endif
    CTR_PREFETCHROOM_AFTER_WRITERFRONT(renderer);
}
#endif

#endif
