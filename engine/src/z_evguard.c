// evguard (asgore30): an event whose code starts with a test of a global that jumps to the code's end when false
// does nothing at all while the test is false, so the runner's event runs skip its events then.
//
// Many of UNDERTALE's bullets end every frame with an End Step that removes the bullet once a global turn counter
// has run out (Asgore's, Mettaton's and the generic monster bullets among 170 code entries that start with such a
// test). During an attack the test is false, and each bullet's End Step only reads one global and compares it: some
// 340 events a frame at Asgore's helix peaks, each with the event's own set-up and tear-down (VM_runEventBound) and
// its generated code.
//
// The code entry is recognised at the start of each event run (runner.c's VM_BEGIN_EVENT_RUN, vm.h) by its bytecode:
//     Push.v global.<var> (or PushGlb.v), a plain variable; PushI.e, Push.d or Push.i <constant>; Cmp.<kind>;
//     Bf <the code's end>
// Then, before each event of the run (VM_RUN_EVENT_IN_RUN), the global is read and compared as the generated code
// does (vmScopeVarSlot's slot, aotCmpFastK itself). When the test is false the event would read the global, compare and
// return: no write, no call, nothing to see, so it is skipped. Between the events of one run only the runner's own
// checks run (no code), so once false the test stays false for the rest of the run and is not read again; after an
// event that ran (a true test) it is read again. A variable that isn't there, or holds anything but a plain number,
// runs the event as before (its slow path, an error included). A run that begins inside an event (a nested dispatch)
// takes the test over, so the outer run's remaining events just run.
//
// All late (late_reserve.ld: .text.evguard_late, .bss.evguard_late, each after itemfront's, merge order), so nothing
// that existed moves; in runner.c only the two calls' targets change. UNDERTALE only (vm.h); tracing builds run every
// event.
#include "wad_versions.h"

#if defined(ENABLE_WAD16)

#include "aot/vm_aot.h"
#include "data_win.h"
#include "instance.h"
#include "vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(__arm__) && defined(__3DS__)
#define EVG_LATE __attribute__((noinline, section(".text.evguard_late")))
#define EVG_BSS __attribute__((section(".bss.evguard_late")))
#else
#define EVG_LATE __attribute__((noinline))
#define EVG_BSS
#endif

void VM_beginEventRunGuard(VMContext* ctx, VMEventRun* run, int32_t codeIndex, int32_t eventType, int32_t eventSubtype, int32_t ownerObjectIndex) EVG_LATE;
void VM_runEventGuarded(VMContext* ctx, VMEventRun* run, Instance* instance) EVG_LATE;

#if defined(ENABLE_VM_TRACING)
void VM_beginEventRunGuard(VMContext* ctx, VMEventRun* run, int32_t codeIndex, int32_t eventType, int32_t eventSubtype, int32_t ownerObjectIndex) {
    VM_beginEventRun(ctx, run, codeIndex, eventType, eventSubtype, ownerObjectIndex);
}
void VM_runEventGuarded(VMContext* ctx, VMEventRun* run, Instance* instance) {
    VM_runEventBound(ctx, run, instance);
}
#else
RValue* UtFlames_slot(Instance* inst, int32_t varID);  // z_utflames.c's IntRValueHashMap_findSlot copy

static struct {
    const VMEventRun* run;  // the run the test is for: the last one begun
    GMLReal k;              // the constant, as the generated code passes it
    int32_t varID;
    uint8_t kind;           // Cmp's kind (CMP_LT ... CMP_GT)
    uint8_t state;          // 0 no test, 1 read it before each event, 2 it was false and nothing has run since
} evg EVG_BSS;

static inline uint32_t evgWord(const uint8_t* p) {
    uint32_t w;
    memcpy(&w, p, sizeof w);
    return w;
}

void VM_beginEventRunGuard(VMContext* ctx, VMEventRun* run, int32_t codeIndex, int32_t eventType, int32_t eventSubtype, int32_t ownerObjectIndex) {
    VM_beginEventRun(ctx, run, codeIndex, eventType, eventSubtype, ownerObjectIndex);
    evg.run = run;
    evg.state = 0;
    const CodeEntry* code = run->code;
    const uint32_t len = code->length;
    const uint8_t* p = run->bytecode;
    if (code->offset != 0 || 20 > len || p == nullptr) return;
    // Push.v global.<var> (instance -5) or PushGlb.v, of a plain (not array) variable.
    uint32_t w = evgWord(p);
    if (!((w >> 24) == 0xC2 || ((w >> 24) == 0xC0 && (w & 0xFFFF) == 0xFFFB)) || ((w >> 16) & 0xF) != 5) return;
    const uint32_t ref = evgWord(p + 4);
    const uint32_t vi = ref & 0x07FFFFFF;
    if ((ref >> 24 & 0xF8) != 0xA0 || ctx->dataWin->vari.variableCount <= vi) return;
    const int32_t varID = ctx->dataWin->vari.variables[vi].varID;
    if (0 > varID) return;
    // The constant: PushI.e (int16 in the word), Push.d (a double after it) or Push.i (an int32 after it).
    uint32_t at = 8;
    GMLReal k;
    w = evgWord(p + at);
    if ((w >> 24) == 0x84 && ((w >> 16) & 0xF) == 0xF) {
        k = (GMLReal) (int16_t) w;
        at += 4;
    } else if ((w >> 24) == 0xC0 && ((w >> 16) & 0xF) == 0) {
        double d;
        memcpy(&d, p + at + 4, sizeof d);
        k = (GMLReal) d;
        at += 12;
    } else if ((w >> 24) == 0xC0 && ((w >> 16) & 0xF) == 2) {
        k = (GMLReal) (int32_t) evgWord(p + at + 4);
        at += 8;
    } else {
        return;
    }
    if (at + 8 > len) return;
    // Cmp.<kind>, then Bf to the end of the code (a 23-bit signed word offset from the Bf).
    w = evgWord(p + at);
    const uint32_t kind = (w >> 8) & 0xFF;
    if ((w >> 24) != 0x15 || kind - 1u > 5u) return;
    w = evgWord(p + at + 4);
    if ((w >> 24) != 0xB8 || at + 4 + (uint32_t) (((int32_t) (w << 9) >> 9) * 4) != len) return;
    evg.k = k;
    evg.varID = varID;
    evg.kind = (uint8_t) kind;
    evg.state = 1;
}

void VM_runEventGuarded(VMContext* ctx, VMEventRun* run, Instance* instance) {
    if (evg.run == run && evg.state != 0) {
        if (evg.state == 2) return;
        Instance* global = (Instance*) ctx->globalScopeInstance;
        RValue* v = global != nullptr ? UtFlames_slot(global, evg.varID) : nullptr;
        // The generated code's own test (aotCmpFastK: the interpreter's vmCmpOrder and vmCmpHolds); -1, not a plain
        // number, runs the event.
        if (v != nullptr && aotCmpFastK(v, evg.k, evg.kind) == 0) {
            evg.state = 2;
            return;
        }
    }
    VM_runEventBound(ctx, run, instance);
}
#endif

#endif // ENABLE_WAD16
