// UNDERTALE's monster dust: native bodies for the dust maker's Draw and the dust pixel's Create, so
// that the effect itself costs less on the Old 3DS.
//
// When a monster dies, the dust maker draws the part of the monster's sprite below the rows it has read so far. Each
// frame it reads four more rows of a run-length string (one character a run: a gap, a run of dust pixels, or the
// row's end), and each dust pixel is a new instance with a Create that sets its gravity and its sideways speed. The
// generated code calls string_char_at for every character, which walks the string from its start (up to 2121
// characters), and ord three or four times. It also stores every intermediate value back into the instance's map,
// and calls instance_create once for every dust pixel.
//
// tools/aot/gmlaot.py binds the two code entries here only when their bytecode is exactly the shape in UTF_NATIVE
// (by the FNV-1a digest of its full tokens, which pins every instruction, every constant whole and every string by its
// text; the pixel's object index may be any, and the body gets it with the other operands). It binds them
// through a small wrapper (aotu_<k>) that passes every operand in one array. A body runs the generated function
// instead whenever a precondition fails, before it has changed anything. Otherwise it does what the generated
// function does and leaves the same state behind: the same values in the same variables (type and stack type too),
// the same calls of the same builtins with the same arguments in the same order (so the same instances, ids and
// random numbers), and the same draw.
// Intermediate values nothing else can read are not stored, and the maker's variable slots and its data string are
// held across its instance_create calls: no other code runs while a body runs except these builtins and the dust
// pixels' Create, which the Draw's body checks (once a data.win) is exactly the pixel's Create below, with no parent
// and no PreCreate (Utkill_codeIs: so it reads and writes none of the maker's variables).
//
// All code is late (.text.late.utkill.vapor, platform/3ds/late_reserve.ld), so nothing that existed moves. Only builds
// with generated code for WAD 16 (UNDERTALE's, GCC or Clang) have any of it: the bodies are only bound from there.
#include "wad_versions.h"

#if defined(ENABLE_WAD16) && defined(CTRPORT_AOT) && defined(__GNUC__)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "instance.h"
#include "log.h"
#include "rvalue.h"
#include "runner.h"
#include "vm.h"
#include "vm_builtins.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__arm__) && defined(__3DS__)
#define UKV_LATE __attribute__((noinline, section(".text.late.utkill.vapor")))
#define UKV_BSS __attribute__((section(".bss.late.utkill.vapor")))
#else
#define UKV_LATE __attribute__((noinline))
#define UKV_BSS
#endif

RValue Utkill_vaporDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* ops) UKV_LATE;
RValue Utkill_pixelCreate(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* ops) UKV_LATE;

// A code entry's bytecode (as the VM runs it, operands patched) against a template of words: every word as given,
// except the words a call names (at calls[i].word), which must be a function of that name. Any data.win where the
// functions sit at other indices still matches. Also used by the dust swarm (z_utkill_swarm.c).
typedef struct {
    uint32_t word;
    const char* name;
} UtkillCall;
bool Utkill_codeIs(DataWin* dw, int32_t codeId, const uint32_t* words, uint32_t wordCount, const UtkillCall* calls,
                   uint32_t callCount) UKV_LATE;
bool Utkill_codeIs(DataWin* dw, int32_t codeId, const uint32_t* words, uint32_t wordCount, const UtkillCall* calls,
                   uint32_t callCount) {
    if (0 > codeId || (uint32_t) codeId >= dw->code.count) return false;
    CodeEntry* code = &dw->code.entries[codeId];
    if (code->offset != 0 || code->length != wordCount * 4) return false;
    const uint8_t* bytes = DataWin_codeBytes(dw, code);
    if (bytes == nullptr) return false;
    uint32_t c = 0;
    for (uint32_t i = 0; wordCount > i; i++) {
        uint32_t w;
        memcpy(&w, bytes + 4 * i, sizeof w);
        if (c < callCount && calls[c].word == i) {
            if (w >= dw->func.functionCount || dw->func.functions[w].name == nullptr ||
                strcmp(dw->func.functions[w].name, calls[c].name) != 0)
                return false;
            c++;
        } else if (w != words[i]) {
            return false;
        }
    }
    return c == callCount;
}

// The dust pixel's Create, as Utkill_pixelCreate's shape: a random fall strength (gravity pointing down) and a
// random sideways speed.
extern const uint32_t Utkill_pixelCreateWords[24];
const uint32_t Utkill_pixelCreateWords[24] = {
    0xC0000000, 0x00000000, 0x3FE00000, 0x07500000, 0xD9020001, 0, 0xC0000000, 0x9999999A, 0x3FC99999, 0x0C500000,
    0x840F005A, 0x07520000, 0xD9020002, 0, 0x9E050000, 0x840F0004, 0x07520000, 0xD9020001, 0, 0x840F0002, 0x0D520000,
    0xD9020001, 0, 0x9E050000,
};
extern const UtkillCall Utkill_pixelCreateCalls[4];
const UtkillCall Utkill_pixelCreateCalls[4] = {
    { 5, "random" }, { 13, "action_set_gravity" }, { 18, "random" }, { 22, "action_set_hspeed" },
};

// Whether making an instance of the object runs only that Create: no parent (so nothing inherited), no PreCreate, and
// one Create event of one code action that is exactly it.
static bool ukvPixelPure(DataWin* dw, int32_t objectIndex) UKV_LATE;
static bool ukvPixelPure(DataWin* dw, int32_t objectIndex) {
    if (0 > objectIndex || (uint32_t) objectIndex >= dw->objt.count) return false;
    GameObject* o = &dw->objt.objects[objectIndex];
    if (o->parentId >= 0 || o->eventLists[EVENT_PRECREATE].eventCount != 0) return false;
    const ObjectEventList* create = &o->eventLists[EVENT_CREATE];
    if (create->eventCount != 1 || create->events[0].eventSubtype != 0 || create->events[0].actionCount != 1)
        return false;
    const EventAction* a = &create->events[0].actions[0];
    if (a->kind != 7 || a->exeType != 2 || a->who != -1 || a->relative || a->isNot) return false;
    return Utkill_codeIs(dw, a->codeId, Utkill_pixelCreateWords, 24, Utkill_pixelCreateCalls, 4);
}

// A dust pixel's instance_create: through the swarm where there is one (z_utkill_swarm.c), which makes the pixel
// itself or calls the builtin as ukvCallDrop does.
#if defined(UTSW_ON)
void Utsw_createDrop(VMContext* ctx, int32_t funcIndex, RValue* args, int32_t argCount);
#define UKV_CREATE_DROP Utsw_createDrop
#else
#define UKV_CREATE_DROP ukvCallDrop
#endif

#if !defined(ENABLE_VM_TRACING) && !defined(CTRPORT_AOT_STRESS)
// Keeps GCC (-ffast-math) from merging or reordering arithmetic across it, as AOT_OPAQUE does between instructions.
#define UKV_OPAQUE(v) __asm__("" : "+m"(v))

static inline __attribute__((always_inline)) RValue ukvReal(GMLReal v, uint8_t gmlType) {
    RValue r;
    memset(&r, 0, sizeof r);
    r.real = v;
    r.type = RVALUE_REAL;
    r.gmlStackType = gmlType;
    return r;
}

static inline __attribute__((always_inline)) RValue ukvInt(int32_t v, uint8_t gmlType) {
    RValue r;
    memset(&r, 0, sizeof r);
    r.int32 = v;
    r.type = RVALUE_INT32;
    r.gmlStackType = gmlType;
    return r;
}

#define ukvSlot UtFlames_slot // z_utflames.c's IntRValueHashMap_findSlot copy: one for every UNDERTALE late file
RValue* ukvSlot(Instance* inst, int32_t varID); // (freebytes: each file had its own byte-identical 108 B copy)
#line 149

// A store of a popped value (AOT_STORE after its coercion) into a slot that owns nothing (a precondition), of a number
// or a static string: storeStackValueIntoSlot's plain copy.
static inline void ukvStore(RValue* dst, RValue v) {
    *dst = v;
}

// A call of a builtin as aotCallBuiltin makes it (args first argument first), its result dropped (Popz). No argument
// here owns anything, so there is none to free.
static void ukvCallDrop(VMContext* ctx, int32_t funcIndex, RValue* args, int32_t argCount) UKV_LATE;
static void ukvCallDrop(VMContext* ctx, int32_t funcIndex, RValue* args, int32_t argCount) {
    BuiltinFunc builtin = (BuiltinFunc) ctx->funcCallCache[funcIndex].builtin;
    RValue result = builtin(ctx, argCount > 0 ? args : nullptr, argCount);
    RValue_free(&result);
}

static bool ukvIsString(VMContext* ctx, int32_t index, const char* want) {
    return index >= 0 && ctx->dataWin->strg.count > (uint32_t) index && strcmp(ctx->dataWin->strg.strings[index], want) == 0;
}

// The data string's length, or -1 when it has a character past ASCII (string_char_at then counts characters, not
// bytes). A string from the data.win's string table never changes, so the last one's answer is kept (for the
// data.win it was worked out for: ukvForDataWin).
static const char* ukvLastString UKV_BSS;
static int32_t ukvLastLength UKV_BSS;
static int8_t ukvStringsOk UKV_BSS; // 0: not checked, 1: the shape's strings are as expected, -1: not
static int8_t ukvPixelOk UKV_BSS;   // 0: not checked, 1: the pixel's Create is the pure one (ukvPixelPure), -1: not
static int32_t ukvPixelChecked UKV_BSS; // the object ukvPixelOk is for
static DataWin* ukvForDataWin UKV_BSS;
static const char* ukvForStrings UKV_BSS;
static int32_t ukvAsciiLength(const char* s) UKV_LATE;
static int32_t ukvAsciiLength(const char* s) {
    if (s == ukvLastString && (uintptr_t) s - (uintptr_t) RValue_gameStringsStart < RValue_gameStringsSize)
        return ukvLastLength;
    int32_t n = 0;
    while (s[n] != '\0') {
        if ((uint8_t) s[n] >= 0x80) { n = -1; break; }
        n++;
    }
    if ((uintptr_t) s - (uintptr_t) RValue_gameStringsStart < RValue_gameStringsSize) {
        ukvLastString = s;
        ukvLastLength = n;
    }
    return n;
}
#endif

// The dust maker's Draw (ops: the 15 variables' IDs in first-use order, the 7 functions, the dust pixel's object, the
// strings "0", "}", "~", "~"):
//     Each call first draws the part of the sprite below the rows already turned to dust, in white at full alpha.
//     While the dust is not finished it then reads up to four rows of the monster's dust data, one character at
//     a time until a row's end mark: a character in one range skips that many pixel steps across, a character in
//     another range makes that many dust pixels side by side on the row's line (a wide sprite makes one wide
//     pixel object instead), and the draw colour is set to black and back to white around every character.
//     After each row the next row begins further down the sprite; at the data's final mark the dust is finished
//     and the maker destroys itself, otherwise its alarm is set again from its delay.
// The wide case (a sprite over 120 pixels wide) is left to the generated function.
RValue Utkill_vaporDraw(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* ops) {
    (void) k;
#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
    (void) ops;
    return generated(ctx);
#else
    enum { V_DN, V_LINE, V_HTA, V_HTB, V_HTC, V_WD, V_FIN, V_WW, V_MYCHAR, V_MYREAD, V_MYDATA, V_I, V_SPEC, V_BLK,
           V_MYVAPOR, F_DRAWPART, F_CHARAT, F_SETCOLOR, F_ORD, F_CREATE, F_EVUSER, F_DESTROY, O_PIXEL, S_ZERO, S_ROWEND,
           S_END, S_END2 };
    if (ukvForDataWin != ctx->dataWin || ukvForStrings != RValue_gameStringsStart) {
        // Another data.win (a VM made again): nothing worked out for the last one holds.
        ukvForDataWin = ctx->dataWin;
        ukvForStrings = RValue_gameStringsStart;
        ukvStringsOk = 0;
        ukvPixelOk = 0;
        ukvLastString = nullptr;
    }
    if (ukvStringsOk == 0) {
        ukvStringsOk = ukvIsString(ctx, ops[S_ZERO], "0") && ukvIsString(ctx, ops[S_ROWEND], "}") &&
            ukvIsString(ctx, ops[S_END], "~") && ukvIsString(ctx, ops[S_END2], "~") ? 1 : -1;
    }
    if (ukvPixelOk == 0 || ukvPixelChecked != ops[O_PIXEL]) {
        ukvPixelOk = ukvPixelPure(ctx->dataWin, ops[O_PIXEL]) ? 1 : -1;
        ukvPixelChecked = ops[O_PIXEL];
        logInfo("utkill: dust pixel (object %d) %s\n", (int) ops[O_PIXEL],
                ukvPixelOk > 0 ? "pure: native Draw" : "not the pure Create: generated Draw");
    }
    Instance* inst = (Instance*) ctx->currentInstance;
    if (ukvStringsOk < 0 || ukvPixelOk < 0 || inst == nullptr || inst->objectIndex == STRUCT_OBJECT_INDEX)
        return generated(ctx);
    FuncCallCache* fc = ctx->funcCallCache;
    if (fc[ops[F_DRAWPART]].builtin == nullptr || fc[ops[F_CHARAT]].builtin == nullptr ||
        fc[ops[F_SETCOLOR]].builtin == nullptr || fc[ops[F_ORD]].builtin == nullptr ||
        fc[ops[F_CREATE]].builtin == nullptr || fc[ops[F_DESTROY]].builtin == nullptr) {
        return generated(ctx);
    }

    // Preconditions, before anything changes. Every variable the body reads or writes is there (so no store inserts
    // one and no slot moves), every one it writes owns nothing (so a store frees nothing), and the numbers it reads
    // are plain numbers.
    RValue* pDn = ukvSlot(inst, ops[V_DN]);
    RValue* pLine = ukvSlot(inst, ops[V_LINE]);
    RValue* pHtA = ukvSlot(inst, ops[V_HTA]);
    RValue* pHtB = ukvSlot(inst, ops[V_HTB]);
    RValue* pHtC = ukvSlot(inst, ops[V_HTC]);
    RValue* pWd = ukvSlot(inst, ops[V_WD]);
    RValue* pFin = ukvSlot(inst, ops[V_FIN]);
    if (pDn == nullptr || pLine == nullptr || pHtA == nullptr || pHtB == nullptr || pHtC == nullptr || pWd == nullptr ||
        pFin == nullptr || !vmIsPlainNumber(pLine->type) || !vmIsPlainNumber(pFin->type) || pDn->ownsReference ||
        pHtA->ownsReference || pHtB->ownsReference || pHtC->ownsReference || pLine->ownsReference ||
        pFin->ownsReference) {
        return generated(ctx);
    }
    const RValue kZero = ukvInt(0, GML_TYPE_INT16);
    const RValue kOne = ukvInt(1, GML_TYPE_INT16);
    const RValue kTwo = ukvInt(2, GML_TYPE_INT16);
    bool reading = vmCmpNumbers(pFin, &kZero, CMP_EQ) == 1;

    RValue* pWw = nullptr;
    RValue* pMychar = nullptr;
    RValue* pMyread = nullptr;
    RValue* pI = nullptr;
    RValue* pMyvapor = nullptr;
    const char* data = nullptr;
    int32_t start = 0, length = 0, rows = 0, end = 0;
    bool finish = false, pixels = false;
    if (reading) {
        pWw = ukvSlot(inst, ops[V_WW]);
        pMychar = ukvSlot(inst, ops[V_MYCHAR]);
        pMyread = ukvSlot(inst, ops[V_MYREAD]);
        RValue* pMydata = ukvSlot(inst, ops[V_MYDATA]);
        pI = ukvSlot(inst, ops[V_I]);
        pMyvapor = ukvSlot(inst, ops[V_MYVAPOR]);
        if (pWw == nullptr || pMychar == nullptr || pMyread == nullptr || pMydata == nullptr || pI == nullptr ||
            pMyvapor == nullptr || pMydata->type != RVALUE_STRING || pMydata->string == nullptr || pWw->ownsReference ||
            pMychar->ownsReference || pMyread->ownsReference || pI->ownsReference) {
            return generated(ctx);
        }
        // myread a whole number, so the n-th character read is at myread + n (string_char_at's position, 0-based).
        if (pMyread->type == RVALUE_INT32) {
            start = pMyread->int32;
        } else if (pMyread->type == RVALUE_REAL && pMyread->real >= 0 && pMyread->real < (GMLReal) (1 << 24) &&
                   (GMLReal) (int32_t) pMyread->real == pMyread->real) {
            start = (int32_t) pMyread->real;
        } else {
            return generated(ctx);
        }
        data = pMydata->string;
        length = ukvAsciiLength(data);
        if (start < 0 || length < 0) return generated(ctx);
        // The rows this call reads, to the end mark or four row ends. A row that runs off the string's end would never
        // end (the generated code loops for ever there): leave that to it.
        int32_t at = start;
        while (rows < 4) {
            for (;;) {
                if (at >= length) return generated(ctx);
                uint8_t c = (uint8_t) data[at++];
                if (c >= 39 && c <= 82) pixels = true;
                if (c == '}') break;
                if (c == '~') { finish = true; break; }
            }
            rows++;
            if (finish) break;
        }
        end = at;
        if (pixels) {
            // The wide case is the generated function's. wd undefined (a monster whose Create never sets it:
            // the defeat script copies its wd over) is never > 120 (handleCmp: undefined compares false but for !=).
            if (pWd->type != RVALUE_UNDEFINED && !vmIsPlainNumber(pWd->type)) return generated(ctx);
            const RValue k120 = ukvInt(120, GML_TYPE_INT16);
            if (pWd->type != RVALUE_UNDEFINED && vmCmpNumbers(pWd, &k120, CMP_GT) == 1) {
                RValue* pSpec = ukvSlot(inst, ops[V_SPEC]);
                if (pSpec == nullptr || !vmIsPlainNumber(pSpec->type)) return generated(ctx);
                if (vmCmpNumbers(pSpec, &kZero, CMP_EQ) == 1) return generated(ctx);
            }
        }
        if (!finish || rows > 1) {
            if (!vmIsPlainNumber(pMyvapor->type)) return generated(ctx);
        }
    }

    // The drawn part's flag, height, top on the screen and top in the sprite, from the row counter.
    ukvStore(pDn, ukvReal(1.0, GML_TYPE_DOUBLE));
    {
        RValue sh = VMBuiltins_getVariable(ctx, inst, BUILTIN_VAR_SPRITE_HEIGHT, "sprite_height", -1);
        sh.gmlStackType = GML_TYPE_VARIABLE;
        RValue t;
        vmWeakCopyInto(&t, pLine);
        vmArithNumbers(&t, &kTwo, VM_ARITH_MUL, GML_TYPE_VARIABLE);
        UKV_OPAQUE(t);
        if (!vmArithNumbers(&sh, &t, VM_ARITH_SUB, GML_TYPE_VARIABLE)) {
            // sprite_height is always a real: never here.
            RValue_free(&sh);
            sh = ukvReal(0.0, GML_TYPE_VARIABLE);
        }
        UKV_OPAQUE(sh);
        ukvStore(pHtA, sh);
        RValue b = ukvReal((GMLReal) inst->y, GML_TYPE_VARIABLE);
        vmWeakCopyInto(&t, pLine);
        vmArithNumbers(&t, &kTwo, VM_ARITH_MUL, GML_TYPE_VARIABLE);
        UKV_OPAQUE(t);
        vmArithNumbers(&b, &t, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        UKV_OPAQUE(b);
        ukvStore(pHtB, b);
        vmWeakCopyInto(&t, pLine);
        vmArithNumbers(&t, &kTwo, VM_ARITH_MUL, GML_TYPE_VARIABLE);
        UKV_OPAQUE(t);
        ukvStore(pHtC, t);
    }
    // The draw of the sprite's undissolved part (draw_sprite_part_ext, its 12 arguments in order).
    {
        RValue a[12];
        a[0] = ukvReal((GMLReal) inst->spriteIndex, GML_TYPE_VARIABLE);
        a[1] = ukvReal((GMLReal) inst->imageIndex, GML_TYPE_VARIABLE);
        a[2] = ukvInt(0, GML_TYPE_VARIABLE);
        vmWeakCopyInto(&a[3], pHtC);
        vmWeakCopyInto(&a[4], pWd);
        vmWeakCopyInto(&a[5], pHtA);
        a[6] = ukvReal((GMLReal) inst->x, GML_TYPE_VARIABLE);
        vmWeakCopyInto(&a[7], pHtB);
        a[8] = ukvReal((GMLReal) inst->imageXscale, GML_TYPE_VARIABLE);
        a[9] = ukvReal((GMLReal) inst->imageYscale, GML_TYPE_VARIABLE);
        a[10] = ukvInt(0xFFFFFF, GML_TYPE_VARIABLE);
        a[11] = ukvInt(1, GML_TYPE_VARIABLE);
        ukvCallDrop(ctx, ops[F_DRAWPART], a, 12);
    }
    if (!reading) return aotx_scriptFallthroughReturnValue(ctx);

    // The rows. line, myread and the last for loop's i as the generated code leaves them; ww only within a row.
    RValue line;
    vmWeakCopyInto(&line, pLine);
    line.gmlStackType = pLine->gmlStackType;
    int32_t at = start;
    int32_t lastCount = -1; // iterations of the last for loop started, -1: none
    uint8_t lastChar = 0;
    for (int32_t row = 0; row < rows; row++) {
        GMLReal ww = 0.0;
        for (;;) {
            uint8_t c = (uint8_t) data[at++];
            lastChar = c;
            if (c >= 84 && c <= 121) {
                int32_t n = c - 85;
                lastCount = n > 0 ? n : 0;
                for (int32_t j = 0; j < n; j++) { ww += 2.0; UKV_OPAQUE(ww); }
            }
            if (c >= 39 && c <= 82) {
                int32_t n = c - 40;
                lastCount = n > 0 ? n : 0;
                for (int32_t j = 0; j < n; j++) {
                    // One dust pixel at this step of the row (instance_create).
                    RValue a[3];
                    a[0] = ukvReal((GMLReal) inst->x, GML_TYPE_VARIABLE);
                    RValue w = ukvReal(ww, GML_TYPE_VARIABLE);
                    vmArithNumbers(&a[0], &w, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    UKV_OPAQUE(a[0]);
                    a[1] = ukvReal((GMLReal) inst->y, GML_TYPE_VARIABLE);
                    RValue t = line;
                    t.gmlStackType = GML_TYPE_VARIABLE;
                    vmArithNumbers(&t, &kTwo, VM_ARITH_MUL, GML_TYPE_VARIABLE);
                    UKV_OPAQUE(t);
                    vmArithNumbers(&a[1], &t, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    UKV_OPAQUE(a[1]);
                    vmArithNumbers(&a[1], &kTwo, VM_ARITH_ADD, GML_TYPE_VARIABLE);
                    UKV_OPAQUE(a[1]);
                    a[2] = ukvInt(ops[O_PIXEL], GML_TYPE_VARIABLE);
                    UKV_CREATE_DROP(ctx, ops[F_CREATE], a, 3);
                    ww += 2.0;
                    UKV_OPAQUE(ww);
                }
            }
            if (c == '}' || c == '~') break;
        }
        vmArithNumbers(&line, &kOne, VM_ARITH_ADD, GML_TYPE_VARIABLE);
        UKV_OPAQUE(line);
    }

    // What the generated code leaves: draw_set_color's last colour, then the variables.
    {
        RValue colour = ukvInt(0xFFFFFF, GML_TYPE_VARIABLE);
        ukvCallDrop(ctx, ops[F_SETCOLOR], &colour, 1);
    }
    if (lastCount >= 0) {
        // The last loop's counter: its first store is a real (an int store), each later one a real typed Variable.
        ukvStore(pI, lastCount == 0 ? ukvReal(0.0, GML_TYPE_DOUBLE) : ukvReal((GMLReal) lastCount, GML_TYPE_VARIABLE));
    }
    ukvStore(pWw, ukvReal(0.0, GML_TYPE_DOUBLE));
    {
        RValue ch = RValue_makeString(RValue_asciiStrings[lastChar]);
        ch.gmlStackType = GML_TYPE_VARIABLE;
        ukvStore(pMychar, ch);
    }
    {
        // myread + 1 once a character.
        RValue r;
        vmWeakCopyInto(&r, pMyread);
        int32_t n = end - start;
        if (r.type == RVALUE_INT32) r.int32 = (int32_t) ((uint32_t) r.int32 + (uint32_t) n);
        else r.real = (GMLReal) (start + n);
        if (n == 0) r.gmlStackType = pMyread->gmlStackType;
        ukvStore(pMyread, r);
    }
    line.ownsReference = false;
    ukvStore(pLine, line);
    if (!finish || rows > 1) {
        // The alarm set again from the delay (the same value after every row that isn't the last).
        RValue v = ukvInt(1, GML_TYPE_INT16);
        RValue mv;
        vmWeakCopyInto(&mv, pMyvapor);
        vmArithNumbers(&v, &mv, VM_ARITH_ADD, GML_TYPE_INT32);
        VMBuiltins_setVariable(ctx, inst, BUILTIN_VAR_ALARM, "alarm", v, 0);
    }
    if (finish) {
        ukvStore(pFin, ukvReal(1.0, GML_TYPE_DOUBLE));
        ukvCallDrop(ctx, ops[F_DESTROY], nullptr, 0);
    }
    return aotx_scriptFallthroughReturnValue(ctx);
#endif
}

// The dust pixel's Create (ops: random, action_set_gravity, action_set_hspeed, then the constants 90, 4 and 2):
//     a random fall strength (gravity pointing down) and a random sideways speed.
// A dust pixel's whole Create event: its arguments built and the builtins called as the generated code does it,
// without the stack. The doubles 0.5 and 0.2 are written here: the shape's digest pins both whole (all 64 bits), so a
// data.win with any other value there doesn't bind.
RValue Utkill_pixelCreate(VMContext* ctx, uint32_t k, AotFn generated, const int32_t* ops) {
    (void) k;
#if defined(ENABLE_VM_TRACING) || defined(CTRPORT_AOT_STRESS)
    (void) ops;
    return generated(ctx);
#else
    FuncCallCache* fc = ctx->funcCallCache;
    BuiltinFunc fRandom = (BuiltinFunc) fc[ops[0]].builtin;
    BuiltinFunc fGravity = (BuiltinFunc) fc[ops[1]].builtin;
    BuiltinFunc fHspeed = (BuiltinFunc) fc[ops[2]].builtin;
    if (fRandom == nullptr || fGravity == nullptr || fHspeed == nullptr) return generated(ctx);
    // random(0.5) + 0.2: Push.d 0.5 typed Variable (Conv), the result typed Variable, Add of Push.d 0.2.
    RValue a[2];
    a[0] = ukvReal((GMLReal) 0.5, GML_TYPE_VARIABLE);
    RValue r = fRandom(ctx, a, 1);
    r.gmlStackType = GML_TYPE_VARIABLE;
    RValue kv;
    {
        uint64_t bits = 0x3FC999999999999AULL; // 0.2
        double d;
        memcpy(&d, &bits, sizeof d);
        kv = ukvReal((GMLReal) d, GML_TYPE_DOUBLE);
    }
    if (!vmArithNumbers(&r, &kv, VM_ARITH_ADD, GML_TYPE_VARIABLE)) {
        // random returns a real: never here.
        RValue_free(&r);
        return generated(ctx);
    }
    UKV_OPAQUE(r);
    a[0] = ukvInt(ops[3], GML_TYPE_VARIABLE);
    a[1] = r;
    RValue res = fGravity(ctx, a, 2);
    RValue_free(&res);
    // random(4) - 2
    a[0] = ukvInt(ops[4], GML_TYPE_VARIABLE);
    r = fRandom(ctx, a, 1);
    r.gmlStackType = GML_TYPE_VARIABLE;
    const RValue k2 = ukvInt(ops[5], GML_TYPE_INT16);
    vmArithNumbers(&r, &k2, VM_ARITH_SUB, GML_TYPE_VARIABLE);
    UKV_OPAQUE(r);
    res = fHspeed(ctx, &r, 1);
    RValue_free(&res);
    return aotx_scriptFallthroughReturnValue(ctx);
#endif
}

#endif
