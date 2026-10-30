// itemfront (asrfight, UNDERTALE Old 3DS only): two of the game's item-name scripts, B and L (with the script N
// that L calls), run as hand code, with the interpreter as their fallback. Late code and constants only
// (platform/3ds/late_reserve.ld, after exitpanic): no table row, nothing that existed moves.
//
// Why: each DREAM press in the Asriel fight runs the game's item-get script 8 times, and each runs B and L: 16
// interpreted loops, 128 string() calls and 128 calls of the text lookup script G. On the Old 3DS these are ~12-14 ms
// of the press's Step (o3DS GML profile, rows asrfight-dreamprof-*: B and N ~3.8-5.0 ms, G ~2.8 ms), and the press
// frame is held (the play-test's "spike before Hopes and Dreams").
//
// What: B fills the eight cells of the global battle item-name array: for each of the eight inventory slots it looks
// up the text whose key is a prefix (one for serious battles, another otherwise) followed by the item's number, and
// stores it. L does the same for the global item-name array with the plain-name prefix, but keeps its counter and
// the current item in self variables and stores each name through the small script N.
// The fronts do the same when every step is G's own fast path (aotGettextFront in tools/aot/gmlaot.py:
// global.language a string other than "ja", ds_map_find_value(global.text_data_en, key) an owned string with no "\["
// escape, which it returns as is) and every key's number is a whole one in int32 range (the digits string() makes:
// strfront's, z_trailfront.c). All eight texts are looked up first; if any step is another case, the ones found are
// freed and the script runs in the interpreter from its start (aotx_executeLoop), nothing changed. Then each cell is
// freed and set as the interpreter leaves it: for B an owned string typed String (the pop of the local's weak
// copy copies it; the front hands over the looked-up text itself, the same characters), for L the call's owned
// result typed Variable, moved in. L also leaves self.i 8 (a real typed Variable, as int + Variable makes)
// and self.itemid a weak copy of item[7], inserted in that order when missing. Both arrays GameMaker 1, row 0 at least
// 8 long, two different arrays, the written one writable in place (one with no owner takes the variable's slot as its
// owner, as popArrayElementIntoSlot does); global.seriousbattle a plain number exactly 0 or 1; L's self an instance.
// Anything else runs the interpreter.
//
// Binding: the four scripts by bytecode length and code name hash (FNV-1a 32, as z_textfront.c's; each once), the
// globals and self variables by VARI name (one varID each), ds_map_find_value by FUNC name, once per data.win. The four bytecode digests (FNV-1a 64, as
// z_trailfront.c's) are read the first call all four entries' bytes are in memory (the interpreter loads them, so the
// front never reads the SD itself; until then, and after a mismatch, it falls back). Bound at each room change through
// the prefetchRoom wrap chain (ctr_wrap_chain.h, between trailfront and dodgefront) with VM_aotBindFront (vm.c's late
// end): of the four only G has a generated function.
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

#define IF_CODE __attribute__((noinline, section(".text.itemfront_late")))
#define IF_RO __attribute__((aligned(4), section(".rodata.itemfront_in_text")))
#define IF_BSS __attribute__((section(".bss.itemfront_late")))

typedef RValue (*IfFn)(VMContext* ctx);
bool VM_aotBindFront(VMContext* ctx, int32_t codeIndex, IfFn fn);
RValue aotx_executeLoop(VMContext* ctx);
RValue aotx_scriptFallthroughReturnValue(VMContext* ctx);

// The globals, the two self variables, then the builtin (enum order), each name ending in NUL.
static const char ifNames[] IF_RO =
    "item\0itemnameb\0itemname\0seriousbattle\0language\0text_data_en\0i\0itemid\0ds_map_find_value\0";
enum { IF_ITEM, IF_NAMEB, IF_NAME, IF_SERIOUS, IF_LANG, IF_MAP, IF_I, IF_ITEMID, IF_FIND, IF_IDS };
// The three key prefixes and "ja", at 0, 12, 24 and 35.
static const char ifText[] IF_RO = "item_names_\0item_nameb_\0item_name_\0ja\0";
// The four scripts (B, L, N and G, see the top): bytecode length, the code name's FNV-1a (32-bit) hash, then the
// bytecode digest's low and high words (FNV-1a 64 of the bytes from the entry's offset, operands patched, as
// aot_table.c's; UNDERTALE 1.08).
static const uint32_t ifScript[4][4] IF_RO = {{236u, 0xE8B63435u, 0x3AFD95ABu, 0xA5B36C0Eu},
                                              {104u, 0x157BB075u, 0xE833ED9Bu, 0x3EF18CFDu},
                                              {60u, 0x650BC571u, 0x85F3461Eu, 0x16098489u},
                                              {1520u, 0x186FC860u, 0x54143E26u, 0xFF01FC2Cu}};
// The log line, once per data.win: the two code indices bound, or minus the lookup step that failed and 0.
static const char ifLog[] IF_RO = "itemfront: %ld %ld\n";

static DataWin* ifChecked IF_BSS;  // the data.win the lookups below are for
static int32_t ifCode[4] IF_BSS;   // the four scripts' code indices (ifScript's order)
static int32_t ifId[IF_IDS] IF_BSS; // the variables' varIDs, then ds_map_find_value's FUNC index
static int8_t ifState IF_BSS;      // 0: digests not read yet, 1: all four match, -1: one doesn't, -2: lookups failed
static bool ifLogged IF_BSS;

static IF_CODE void ifFree(RValue* v) {
    RValue_free(v);
}

// The digits string() makes of a whole number in int32 range (a '-' then the magnitude, no leading zeros; -0 is "0",
// as (int64_t) -0.0 is 0), written back to front ending at end (which gets the NUL); nullptr for any other value.
// strfront's (z_trailfront.c), which stays as it is.
static IF_CODE const char* ifDigits(const RValue* a, char* end) {
    int32_t v = a->int32;
    bool whole = a->type == RVALUE_INT32;
    if (a->type == RVALUE_REAL) {
        GMLReal r = a->real;
        if (r >= (GMLReal) -2147483648.0 && r <= (GMLReal) 2147483647.0) {
            v = (int32_t) r;
            whole = (GMLReal) v == r;
        }
    }
    if (!whole) return nullptr;
    char* p = end;
    *p = '\0';
    uint32_t u = 0 > v ? 0u - (uint32_t) v : (uint32_t) v;
    do { // u - q * 10 rather than u % 10: one multiply-high, no division call
        uint32_t q = u / 10u;
        *--p = (char) ('0' + (u - q * 10u));
        u = q;
    } while (u != 0);
    if (0 > v) *--p = '-';
    return p;
}

// Whether the four scripts are the ones above: read once, the first time all four entries' bytes are in memory.
static IF_CODE bool ifReady(DataWin* dw) {
    if (ifState != 0) return ifState > 0;
    for (int k = 0; 4 > k; k++)
        if (dw->code.entries[ifCode[k]].bytecode == nullptr) return false;
    ifState = 1;
    for (int k = 0; 4 > k; k++) {
        CodeEntry* e = &dw->code.entries[ifCode[k]];
        uint64_t h = 0xCBF29CE484222325ull;
        for (uint32_t j = e->offset; e->length > j; j++) h = (h ^ e->bytecode[j]) * 0x100000001B3ull;
        if (h != (((uint64_t) ifScript[k][3] << 32) | ifScript[k][2])) ifState = -1;
    }
    return ifState > 0;
}

// The GameMaker 1 array slot holds, with row 0 at least 8 long (and, to write, writable in place through slot), else
// nullptr.
static IF_CODE GMLArray* ifArray(RValue* slot, bool write) {
    GMLArray* a = slot != nullptr && slot->type == RVALUE_ARRAY ? slot->array : nullptr;
    if (a == nullptr || a->type != GML_LEGACY_ARRAY || a->legacy.rowCount == 0 || 8 > a->legacy.rows[0].length
        || (write && a->refCount > 1 && a->owner != (void*) slot))
        return nullptr;
    return a;
}

// The eight texts (prefix plus each slot's item number, through G) into t, each the string G's fast path returns (see the top). False, with nothing kept and nothing changed, when any step is another case.
static IF_CODE bool ifTexts(VMContext* ctx, const char* prefix, const RValue* item, RValue* t) {
    uint32_t f = (uint32_t) ifId[IF_FIND];
    BuiltinFunc find = ctx->funcCallCacheCount > f ? (BuiltinFunc) ctx->funcCallCache[f].builtin : nullptr;
    RValue* map = vmScopeVarSlot(ctx, INSTANCE_GLOBAL, ifId[IF_MAP]);
    RValue* lang = vmScopeVarSlot(ctx, INSTANCE_GLOBAL, ifId[IF_LANG]);
    if (find == nullptr || map == nullptr || lang == nullptr || lang->type != RVALUE_STRING || lang->string == nullptr
        || strcmp(lang->string, ifText + 35) == 0)
        return false;
    char key[32]; // the longest prefix (11) and an int32's digits (11), then the NUL
    size_t n = strlen(prefix);
    memcpy(key, prefix, n);
    for (int i = 0; 8 > i; i++) {
        char d[12];
        const char* p = ifDigits(&item[i], d + sizeof d - 1);
        bool ok = p != nullptr;
        if (ok) {
            strcpy(key + n, p);
            // as aotGettextFront calls it: a weak copy of the map, then the key (G's argument, weak)
            RValue a[2];
            vmWeakCopyInto(&a[0], map);
            a[1] = (RValue) {.string = key, .type = RVALUE_STRING, .gmlStackType = GML_TYPE_VARIABLE};
            t[i] = find(ctx, a, 2);
            const char* s = t[i].string;
            ok = t[i].type == RVALUE_STRING && s != nullptr && t[i].ownsReference;
            for (; ok && *s != '\0'; s++) ok = !(s[0] == '\\' && s[1] == '[');
            if (!ok) ifFree(&t[i]);
        }
        if (!ok) {
            while (i-- > 0) ifFree(&t[i]);
            return false;
        }
    }
    return true;
}

// B (list false), or L with N (list true): see the top.
static IF_CODE RValue ifRun(VMContext* ctx, bool list) {
    Instance* self = (Instance*) ctx->currentInstance;
    bool ok = ifReady(ctx->dataWin) && (!list || (self != nullptr && self->objectIndex != STRUCT_OBJECT_INDEX));
    RValue* ds = ok ? vmScopeVarSlot(ctx, INSTANCE_GLOBAL, ifId[list ? IF_NAME : IF_NAMEB]) : nullptr;
    GMLArray* da = ifArray(ds, true);
    GMLArray* ia = da != nullptr ? ifArray(vmScopeVarSlot(ctx, INSTANCE_GLOBAL, ifId[IF_ITEM]), false) : nullptr;
    const char* prefix = ifText + 24; // "item_name_"
    ok = ia != nullptr && ia != da;
    if (ok && !list) {
        // global.seriousbattle == 1 for a plain number that is exactly 0 or 1 (no epsilon question either way)
        RValue* sb = vmScopeVarSlot(ctx, INSTANCE_GLOBAL, ifId[IF_SERIOUS]);
        int32_t b = -1;
        if (sb != nullptr && sb->type == RVALUE_INT32) b = sb->int32;
        if (sb != nullptr && sb->type == RVALUE_REAL) b = sb->real == (GMLReal) 1 ? 1 : sb->real == (GMLReal) 0 ? 0 : -1;
        ok = b == 0 || b == 1;
        prefix = ifText + (b == 1 ? 0 : 12);
    }
    RValue t[8];
    if (!ok || !ifTexts(ctx, prefix, ia->legacy.rows[0].data, t)) return aotx_executeLoop(ctx);
    if (da->owner == nullptr) da->owner = (void*) ds;
    RValue* cell = da->legacy.rows[0].data;
    for (int i = 0; 8 > i; i++) {
        ifFree(&cell[i]);
        if (list) {
            cell[i] = t[i]; // the call's result as the stack held it, moved in
            cell[i].gmlStackType = GML_TYPE_VARIABLE;
        } else {
            cell[i] = RValue_makeOwnedString((char*) t[i].string); // the local's independent copy
        }
    }
    if (list) {
        // the self counter as the loop leaves it (int + Variable is a real typed Variable), then the self item as the last slot's (a plain
        // number: ifDigits took it); each stored as storeStackValueIntoSlot stores a value with no reference (the
        // old value freed), each slot found just before its store (inserting one may move the other)
        RValue v = {.real = (GMLReal) 8, .type = RVALUE_REAL, .gmlStackType = GML_TYPE_VARIABLE};
        RValue* si = vmScopeVarWriteSlot(ctx, INSTANCE_SELF, ifId[IF_I]);
        if (si != nullptr) {
            ifFree(si);
            *si = v;
        }
        vmWeakCopyInto(&v, &ia->legacy.rows[0].data[7]);
        RValue* sx = vmScopeVarWriteSlot(ctx, INSTANCE_SELF, ifId[IF_ITEMID]);
        if (sx != nullptr) {
            ifFree(sx);
            *sx = v;
        }
    }
    return aotx_scriptFallthroughReturnValue(ctx);
}

static IF_CODE RValue ifNameb(VMContext* ctx) {
    return ifRun(ctx, false);
}

static IF_CODE RValue ifName(VMContext* ctx) {
    return ifRun(ctx, true);
}

// The lookups, once per data.win: 0 when all match, else the step that didn't (for the log).
static IF_CODE int32_t ifCheck(DataWin* dw) {
    for (int k = 0; 4 > k; k++) {
        int32_t found = -1;
        for (uint32_t i = 0; dw->code.count > i; i++) {
            CodeEntry* e = &dw->code.entries[i];
            if (e->length != ifScript[k][0] || e->offset > e->length || e->name == nullptr) continue;
            uint32_t nh = 0x811C9DC5u;
            for (const char* c = e->name; *c != '\0'; c++) nh = (nh ^ (uint8_t) *c) * 0x01000193u;
            if (nh != ifScript[k][1]) continue;
            if (found >= 0) return 1 + k;
            found = (int32_t) i;
        }
        if (0 > found) return 1 + k;
        ifCode[k] = found;
    }
    const char* name = ifNames;
    __asm__("" : "+r"(name));
    for (int k = 0; IF_IDS > k; k++, name += strlen(name) + 1) {
        int32_t id = -1;
        bool fn = k == IF_FIND;
        int32_t scope = k >= IF_I ? INSTANCE_SELF : INSTANCE_GLOBAL;
        uint32_t count = fn ? dw->func.functionCount : dw->vari.variableCount;
        for (uint32_t i = 0; count > i; i++) {
            const char* n;
            int32_t v = (int32_t) i;
            if (fn) {
                n = dw->func.functions[i].name;
            } else {
                Variable* var = &dw->vari.variables[i];
                if (var->instanceType != scope) continue;
                n = var->name;
                v = var->varID;
            }
            if (n == nullptr || strcmp(n, name) != 0) continue;
            if (0 > v || (id >= 0 && (fn || id != v))) return 5 + k;
            id = v;
        }
        if (0 > id) return 5 + k;
        ifId[k] = id;
    }
    return 0;
}

// The prefetchRoom chain's link after trailfront's: binds both fronts after each room change, then the next link.
IF_CODE void CTR_PREFETCHROOM_ITEMFRONT(Renderer* renderer) {
    Runner* runner = renderer != nullptr ? renderer->runner : nullptr;
    VMContext* ctx = runner != nullptr ? runner->vmContext : nullptr;
    DataWin* dw = ctx != nullptr ? ctx->dataWin : nullptr;
    if (dw != nullptr) {
        if (ifChecked != dw) {
            ifChecked = dw;
            ifLogged = false;
            int32_t step = ifCheck(dw);
            ifState = step != 0 ? -2 : 0;
            if (step != 0) logInfo(ifLog, (long) -step, (long) 0);
        }
        if (ifState >= 0) {
            bool b = VM_aotBindFront(ctx, ifCode[0], ifNameb);
            bool n = VM_aotBindFront(ctx, ifCode[1], ifName);
            if (b && n && !ifLogged) {
                ifLogged = true;
                logInfo(ifLog, (long) ifCode[0], (long) ifCode[1]);
            }
        }
    }
    CTR_PREFETCHROOM_AFTER_ITEMFRONT(renderer);
}

#endif
