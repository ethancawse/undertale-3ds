// trailfront (asrfight, UNDERTALE Old 3DS only): the rainbow trail object's Draw 0 (the SAVE
// Asriel barrage's ~129 trails a frame) run as hand code, with the interpreter as its fallback. Late code and constants
// only (platform/3ds/late_reserve.ld): no table row, nothing that existed moves.
//
// Why: in the barrage the consoles are CPU bound (o3DS XL rows asrfight-save-*: 28-32 of the attack's 162 frames
// held for 3 vblanks, Step max ~13 ms + Draw max ~17 ms, GPU ~5 ms). The generated Draw is the biggest GML cost there
// (Azahar profile, 1.80 ms self of 9.29 ms GML + builtins): per trail 24 array element reads and 24 writes through
// aotArrayRead / popArrayElementIntoSlot and 8 more reads for the lines, each a call with its checks.
//
// What: the bytecode shifts the trail's two 13-cell position histories (x and y) along by one and puts the
// current position first, sets the draw colour to the instance's blend, and draws four lines of growing width
// joining points further and further back in the history to the current position (draw_line_width).
// and the front does the same for two GameMaker 1 arrays (each its own, row 0 at least 13 long, writable in place)
// whose 13 cells are plain numbers: each element read is a weak copy of the cell (vmWeakCopyInto) and each write
// stores it whole, as popArrayElementIntoSlot does for a plain number with type2 Variable (an array with no owner
// takes the variable's slot as its owner, as there); x, y and image_blend are reals typed Variable as AOT_SELF_FIELD
// reads them; the builtins come from the same call cache, with the same arguments (a weak copy of each cell; the
// width an int32 typed Variable) in the same order, and their results are freed as AOT_POPZ frees them. The last
// line's x and y are xprev[0] and yprev[0], which hold exactly those values (nothing writes x, y or the arrays
// between). The event's local i stays unused. Anything else (no instance, a struct, a variable missing, not an array,
// a short or shared array, one array in both variables, a cell of another type) runs the whole event in the
// interpreter from its start (aotx_executeLoop), which the front has changed nothing for.
//
// Binding: the Draw is pinned by its bytecode's digest (FNV-1a 64, z_dodgefront.c's and aot_table.c's): the code
// entry named as aotTable's entry with that digest, its bytes hashing to it. The arrays by name in VARI (self, one
// varID each), the builtins by FUNC name (each once). Checked once per data.win. Bound through the prefetchRoom wrap
// chain (ctr_wrap_chain.h, between writerfront and dodgefront) at each room change, over that aotTable entry's
// generated function or the entry not looked up yet (VM_aotSwapFront, vm.c's late end).
//
// strfront (asrfight, same file and link): string() of a whole number without newlib's snprintf. Each DREAM press
// in the Asriel fight builds 128 item-name keys ("item_name_" + string(id)), and on the Old 3DS each string() call
// costs ~30 us (o3DS GML profile, rows asrfight-dreamprof-coverage-*: 128-135 calls, ~4 ms of the press's ~19 ms
// Step). builtin_string formats a real that is a whole number with snprintf("%lld") and an int32 with "%d"
// (rvalue.h, RValue_toString); for a real whose value is an int32, and for an int32, the front writes the same
// decimal digits itself (a '-' then the magnitude, no leading zeros; -0 is "0", as (int64_t) -0.0 is 0), and returns
// the same owned heap string (strdup, as safeStrdup; RValue_makeOwnedString). Any other argument (NaN, inf, a
// fraction, out of int32 range, another type, no argument), or a failed strdup, goes to the builtin it replaced (which
// then fails as before). Bound over the registered builtin (VM_findBuiltin) in every call cache entry that holds it,
// the cache both the interpreter and aotCallBuiltin call through; the registry itself is left as it is.
//
// This build is the only user: CMakeLists.txt compiles this file (with CTRPORT_DODGEFRONT, CTRPORT_WRITERFRONT and
// CTRPORT_TRAILFRONT) for UNDERTALE o3DS only, in writerfront builds.

#if defined(CTRPORT_TRAILFRONT) && defined(CTRPORT_WRITERFRONT) && defined(ENABLE_WAD16) && defined(__3DS__)

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "instance.h"
#include "int_rvalue_hashmap.h"
#include "log.h"
#include "renderer.h"
#include "runner.h"
#include "vm.h"
#include "vm_fastpath.h"
#include "aot/vm_aot.h"

#define TRF_CODE __attribute__((noinline, section(".text.trailfront_late")))
#define TRF_RO __attribute__((aligned(4), section(".rodata.trailfront_in_text")))
#define TRF_BSS __attribute__((section(".bss.trailfront_late")))

typedef RValue (*TrfFn)(VMContext* ctx);
bool VM_aotSwapFront(VMContext* ctx, int32_t codeIndex, TrfFn fn, TrfFn generated);
RValue aotx_executeLoop(VMContext* ctx);
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx);

// The two arrays, then the two builtins, then string() (strfront, at offset 43), each name ending in NUL.
static const char trfNames[] TRF_RO = "xprev\0yprev\0draw_set_color\0draw_line_width\0string\0";
// The Draw's digest, low word first (UNDERTALE 1.08).
static const uint32_t trfDigest[2] TRF_RO = {0x208DE70Du, 0xF8A82D18u};
// The lines in the bytecode's order: the cell of the first end, of the second, and the width.
static const uint8_t trfLines[4][3] TRF_RO = {{10, 12, 2}, {8, 10, 4}, {4, 8, 6}, {0, 4, 8}};
// The log line, once per data.win: the code index bound, or minus the lookup step that failed.
static const char trfLog[] TRF_RO = "trailfront: %ld\n";

static DataWin* trfChecked TRF_BSS;  // the data.win the lookups below are for
static int32_t trfCode TRF_BSS;      // the Draw's code index, or -1: not this game's
static int32_t trfId[4] TRF_BSS;     // xprev's and yprev's varIDs, then the builtins' FUNC indices
static TrfFn trfGen TRF_BSS;         // the generated function the front may replace
static BuiltinFunc trfFn[2] TRF_BSS; // draw_set_color and draw_line_width, from the call cache when bound
static bool trfLogged TRF_BSS;
static BuiltinFunc strOrig TRF_BSS;  // the registered string(), for every other argument

// A builtin call, then what AOT_POPZ frees of its result.
static TRF_CODE void trfCall(VMContext* ctx, int k, RValue* args, int32_t argc) {
    RValue r = trfFn[k](ctx, args, argc);
    if (r.ownsReference) aotFreeCopy(&r);
}

static TRF_CODE RValue trfDraw(VMContext* ctx) {
    Instance* self = (Instance*) ctx->currentInstance;
    RValue* slot[2];
    GMLArray* arr[2];
    RValue* c[2];
    bool ok = self != nullptr && self->objectIndex != STRUCT_OBJECT_INDEX;
    for (int k = 0; ok && 2 > k; k++) {
        slot[k] = IntRValueHashMap_findSlot(&self->selfVars, trfId[k]);
        arr[k] = slot[k] != nullptr && slot[k]->type == RVALUE_ARRAY ? slot[k]->array : nullptr;
        ok = arr[k] != nullptr && arr[k]->type == GML_LEGACY_ARRAY && arr[k]->legacy.rowCount != 0
             && arr[k]->legacy.rows[0].length >= 13 && !(arr[k]->refCount > 1 && arr[k]->owner != (void*) slot[k]);
        c[k] = ok ? arr[k]->legacy.rows[0].data : nullptr;
        for (int i = 0; ok && 13 > i; i++) ok = vmIsPlainNumber(c[k][i].type) && !c[k][i].ownsReference;
    }
    if (!ok || arr[0] == arr[1]) return aotx_executeLoop(ctx);
    for (int k = 0; 2 > k; k++) {
        if (arr[k]->owner == nullptr) arr[k]->owner = (void*) slot[k];
        for (int i = 12; i > 0; i--) vmWeakCopyInto(&c[k][i], &c[k][i - 1]);
    }
    c[0][0] = (RValue) {.real = (GMLReal) self->x, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_VARIABLE};
    c[1][0] = (RValue) {.real = (GMLReal) self->y, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_VARIABLE};
    RValue a[5];
    a[0] = (RValue) {.real = (GMLReal) self->imageBlend, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_VARIABLE};
    trfCall(ctx, 0, a, 1);
    for (int n = 0; 4 > n; n++) {
        const uint8_t* l = trfLines[n];
        // x of the first end, its y, x of the second, its y
        for (int j = 0; 4 > j; j++) vmWeakCopyInto(&a[j], &c[j & 1][l[j >> 1]]);
        a[4] = (RValue) {.int32 = l[2], .type = RVALUE_INT32, .gmlStackType = GML_TYPE_VARIABLE};
        trfCall(ctx, 1, a, 5);
    }
    return aotx_scriptFallthroughReturnValue(ctx);
}

// string(): a whole number in int32 range as its decimal digits; anything else (or no memory) the builtin.
static TRF_CODE RValue strFront(VMContext* ctx, RValue* args, int32_t argc) {
    if (argc >= 1 && (args[0].type == RVALUE_REAL || args[0].type == RVALUE_INT32)) {
        int32_t v = args[0].int32;
        bool whole = args[0].type == RVALUE_INT32;
        if (!whole) {
            GMLReal r = args[0].real;
            if (r >= (GMLReal) -2147483648.0 && r <= (GMLReal) 2147483647.0) {
                v = (int32_t) r;
                whole = (GMLReal) v == r;
            }
        }
        if (whole) {
            char buf[12];
            char* p = buf + sizeof buf - 1;
            *p = '\0';
            uint32_t u = 0 > v ? 0u - (uint32_t) v : (uint32_t) v;
            do {  // u - q * 10 rather than u % 10: one multiply-high, no division call
                uint32_t q = u / 10u;
                *--p = (char) ('0' + (u - q * 10u));
                u = q;
            } while (u != 0);
            if (0 > v) *--p = '-';
            char* text = strdup(p);
            if (text != nullptr) return RValue_makeOwnedString(text);
        }
    }
    return strOrig(ctx, args, argc);
}

// Binds strFront over the registered string() in the call cache (each room change: a refilled cache gets it again).
// builtin_string is registered under that one name, so a cache entry holding it is a call of string().
static TRF_CODE void strBind(VMContext* ctx) {
    if (strOrig == nullptr) strOrig = VM_findBuiltin(ctx, trfNames + 43);
    if (strOrig == nullptr || strOrig == strFront) return;
    for (uint32_t i = 0; ctx->funcCallCacheCount > i; i++)
        if (ctx->funcCallCache[i].builtin == (void*) strOrig) ctx->funcCallCache[i].builtin = (void*) strFront;
}

// The lookups, once per data.win: 0 when all match, else the step that didn't (for the log).
static TRF_CODE int32_t trfCheck(DataWin* dw) {
    uint64_t want = ((uint64_t) trfDigest[1] << 32) | trfDigest[0];
    const AotEntry* en = nullptr;
    for (uint32_t i = 0; aotTable.entryCount > i && en == nullptr; i++)
        if (aotTable.entries[i].digest == want) en = &aotTable.entries[i];
    if (en == nullptr || en->fn == nullptr) return 1;
    int32_t code = -1;
    for (uint32_t i = 0; dw->code.count > i; i++) {
        const char* n = dw->code.entries[i].name;
        if (n == nullptr || strcmp(n, en->codeName) != 0) continue;
        if (code >= 0) return 2;
        code = (int32_t) i;
    }
    if (0 > code) return 2;
    CodeEntry* e = &dw->code.entries[code];
    const uint8_t* bytes = DataWin_codeBytes(dw, e);
    if (bytes == nullptr || e->offset > e->length) return 3;
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t j = e->offset; e->length > j; j++) h = (h ^ bytes[j]) * 0x100000001B3ull;
    if (h != want) return 3;
    const char* name = trfNames;
    __asm__("" : "+r"(name));
    for (int k = 0; 4 > k; k++, name += strlen(name) + 1) {
        int32_t id = -1;
        uint32_t count = k >= 2 ? dw->func.functionCount : dw->vari.variableCount;
        for (uint32_t i = 0; count > i; i++) {
            const char* n;
            int32_t v = (int32_t) i;
            if (k >= 2) {
                n = dw->func.functions[i].name;
            } else {
                Variable* var = &dw->vari.variables[i];
                if (var->instanceType != INSTANCE_SELF) continue;
                n = var->name;
                v = var->varID;
            }
            if (n == nullptr || strcmp(n, name) != 0) continue;
            if (0 > v || (id >= 0 && (k >= 2 || id != v))) return 4;
            id = v;
        }
        if (0 > id) return 4;
        trfId[k] = id;
    }
    trfGen = en->fn;
    trfCode = code;
    return 0;
}

// The prefetchRoom chain's link after writerfront's: binds the front after each room change, then the next link.
TRF_CODE void CTR_PREFETCHROOM_TRAILFRONT(Renderer* renderer) {
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    VMContext* ctx = runner != nullptr ? runner->vmContext : nullptr;
    DataWin* dw = ctx != nullptr ? ctx->dataWin : nullptr;
    if (dw != nullptr) {
        int32_t step = 0;
        if (trfChecked != dw) {
            trfChecked = dw;
            trfCode = -1;
            trfLogged = false;
            step = trfCheck(dw);
        }
        strBind(ctx);
        bool bound = false;
        if (trfCode >= 0 && ctx->funcCallCacheCount == dw->func.functionCount) {
            BuiltinFunc f0 = (BuiltinFunc) ctx->funcCallCache[trfId[2]].builtin;
            BuiltinFunc f1 = (BuiltinFunc) ctx->funcCallCache[trfId[3]].builtin;
            // Set only when both are there: a front bound before keeps working builtins whatever this finds.
            if (f0 != nullptr && f1 != nullptr) {
                trfFn[0] = f0;
                trfFn[1] = f1;
                bound = VM_aotSwapFront(ctx, trfCode, trfDraw, trfGen);
            }
        }
        if ((bound && !trfLogged) || step != 0) {
            trfLogged = true;
            logInfo(trfLog, (long) (bound ? trfCode : -step));
        }
    }
    CTR_PREFETCHROOM_AFTER_TRAILFRONT(renderer);
}

#endif
