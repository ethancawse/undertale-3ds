// offkill (change offdraw; UNDERTALE only): early destroy of bullets that have left the view for good.
//
// Some of UNDERTALE's bullets fly on past the view and live on until a far line (the room's edge plus a margin, a
// timer, the end of the turn), running their Step and Draw events the whole time. The builder lists the ones whose
// PC code proves they can never come back or matter again (tools/aot/offkill.py; change offdraw) in
// romfs:/offkill.bin: per entry an object index, an axis (0 = x, 1 = y), a direction (+1 = right or down, -1 = left
// or up) and a reach in pixels, the farthest any of its pixels can lie from its position plus twice the scene's
// largest view shake. At the end of each frame (after Draw, from Utsw_handleRoomChange) an instance of a listed object
// that is moving away along the axis and is farther than the reach past that edge of view 0 is destroyed without its
// Destroy event (RULES.md, "Early destroy": only the early-destroyed instances differ). Without the file, nothing.

#include "runner.h"

#if defined(UTSW_ON)
#include <stdio.h>

#include "ctr_benchmark.h"
#include "stb_ds.h"

#define OK_TEXT __attribute__((noinline, optimize("Os"), section(".text.offkill_late")))
#define OK_MAX 8

// One entry of romfs:/offkill.bin (8 bytes, little-endian).
typedef struct {
    int16_t object;
    int8_t axis;
    int8_t sign;
    uint8_t reach;
    uint8_t pad[3];
} OffKillRule;

static OffKillRule okRules[OK_MAX] __attribute__((section(".bss.offkill_late")));
// 0: the file not read yet; -1: no file or no entries; otherwise the number of entries.
static int8_t okCount __attribute__((section(".bss.offkill_late")));
static const char okPath[] __attribute__((section(".rodata.offkill_in_text"))) = "romfs:/offkill.bin";
static const char okMode[] __attribute__((section(".rodata.offkill_in_text"))) = "rb";

#if defined(CTRPORT_OFFDRAW)
bool CtrOffdraw_offkillOff(void);
#endif

// Inactive for good (change omegaflowey, GO 11-02, "Omega Flowey after the
// 2nd SOUL"). Some bullets save their state and load it back later; one that went inactive and invisible in between
// stays an instance with its own "active" variable 0 until the end of the phase, and every frame it still runs the
// collision pass against the heart (its event does nothing while inactive). The builder lists the objects whose PC
// code proves that such an instance can never be active again (tools/aot/offkill.py) in romfs:/offdoom.bin: per entry
// the object index and the variable ids of its active flag, its saved flag and its saved copy of the active flag. At
// the end of each frame an active, invisible instance of a listed object whose active variable is 0 and whose saved
// flag or saved active flag is 0 (each a number, exactly 0) is destroyed without its Destroy event, as offkill's
// own entries (RULES.md, "Early destroy"). Without the file, nothing. Its own sections, last in late_reserve.ld, so
// no earlier late code moves; OffKill_roomChange reaches it through the call it made to CtrBench_handleRoomChange.
#define OD_TEXT __attribute__((noinline, noipa, optimize("Os"), section(".omegaflowey_ifg")))
#define OD_RO __attribute__((aligned(4), section(".omegaflowey_ifg_ro")))
#define OD_BSS __attribute__((section(".bss_omegaflowey_ifg,\"aw\",%nobits @")))
#define OD_MAX 4

// One entry of romfs:/offdoom.bin (8 bytes, little-endian).
typedef struct {
    int16_t object;
    int16_t active;
    int16_t saved;
    int16_t savedActive;
} OffDoomRule;

static OffDoomRule odRules[OD_MAX] OD_BSS;
// 0: the file not read yet; -1: no file or no entries; otherwise the number of entries.
static int32_t odCount OD_BSS;
// Instances destroyed (the proof count, logged at 1, 2, 4, 8, ...).
static uint32_t odKilled OD_BSS;
static const char odPath[] OD_RO = "romfs:/offdoom.bin";
static const char odLog[] OD_RO = "[offkill] %lu inactive destroyed\n";

// Whether the instance's variable holds a real, int32 or bool that is exactly 0 (missing or any other type: no, so
// the instance stays). Flattened: the header's lookup is inlined here, not copied out into z_offkill.o's .text.
static bool odZero(Instance* inst, int32_t var) OD_TEXT __attribute__((flatten));
static bool odZero(Instance* inst, int32_t var) {
    RValue* v = IntRValueHashMap_findSlot(&inst->selfVars, var);
    if (v == nullptr) return false;
    if (v->type == RVALUE_REAL) return v->real == 0.0;
    // RVALUE_INT32 (2) or RVALUE_BOOL (4), without a jump table in .rodata
    return (((uint32_t) v->type - 2u) & ~2u) == 0 && v->int32 == 0;
}

static size_t auditfixgR2LoadRules(const char*, void*, size_t, uint32_t);
void OffKill_doomed(Runner* runner) OD_TEXT;

// OffKill_roomChange calls this in place of CtrBench_handleRoomChange.
void OffKill_doomed(Runner* runner) {
    CtrBench_handleRoomChange(runner);
    if (odCount == 0) {
        odCount = -1;
        size_t n = auditfixgR2LoadRules(odPath, odRules, OD_MAX, runner->dataWin->objt.count);
        if (n != 0) odCount = (int32_t) n;
    }
    // A frame that changes room ends its instances as the game does.
    if (0 >= odCount || runner->pendingRoom != -1 || runner->currentRoom == nullptr) return;
    for (int32_t k = 0; odCount > k; k++) {
        const OffDoomRule* rule = &odRules[k];
        // rule->object indexes this data.win's objects, the variables its VARI ids: the builder wrote only what it
        // resolved there. A destroy changes no list (see OffKill_roomChange).
        Instance** list = runner->instancesByExactObject[rule->object];
        for (int32_t i = 0, n = (int32_t) arrlen(list); n > i; i++) {
            Instance* inst = list[i];
            if (inst->destroyed || !inst->active || inst->visible || !odZero(inst, rule->active)) continue;
            if (!odZero(inst, rule->saved) && !odZero(inst, rule->savedActive)) continue;
            Runner_destroyInstance(runner, inst, false);
            odKilled++;
            if ((odKilled & (odKilled - 1)) == 0) logInfo(odLog, (unsigned long) odKilled);
        }
    }
}

void OffKill_roomChange(Runner* runner) OK_TEXT;

// Utsw_handleRoomChange calls this in place of CtrBench_handleRoomChange.
void OffKill_roomChange(Runner* runner) {
    OffKill_doomed(runner);
    if (okCount == 0) {
        okCount = -1;
        size_t n = auditfixgR2LoadRules(okPath, okRules, OK_MAX, runner->dataWin->objt.count);
        if (n != 0) okCount = (int8_t) n;
    }
#if defined(CTRPORT_OFFDRAW)
    if (CtrOffdraw_offkillOff()) return;
#endif
    // A frame that changes room ends its instances as the game does.
    if (0 >= okCount || runner->pendingRoom != -1 || runner->currentRoom == nullptr) return;
    // View 0's edges: its camera, or the room without one (as the diagnostic that proved the entries,
    // platform/3ds/diag/ctr_offdraw.c).
    float lo[2] = { 0.0f, 0.0f };
    float hi[2] = { (float) runner->currentRoom->width, (float) runner->currentRoom->height };
    GMLCamera* cam = Runner_getCameraForView(runner, 0);
    if (cam != nullptr && cam->allocated) {
        lo[0] = cam->viewX;
        lo[1] = cam->viewY;
        hi[0] = lo[0] + (float) cam->viewWidth;
        hi[1] = lo[1] + (float) cam->viewHeight;
    }
    // Each rule walks only its object's instances (change pacer 10-31): every instance of runner->instances is in
    // instancesByExactObject under its current object until the cleanup after its destroy (runner.c, "Draw passes
    // with no instance"), and a destroy changes no list. Position before speed: an instance in view reads no more.
    // (the lists are read per rule: hoisted, the pointer is kept across Runner_destroyInstance on the stack)
    for (int32_t k = 0; okCount > k; k++) {
        const OffKillRule* rule = &okRules[k];
        // rule->object indexes this data.win's objects: tools/aot/offkill.py writes only those it resolved in its OBJT.
        Instance** list = runner->instancesByExactObject[rule->object];
        int a = rule->axis != 0;
        float s = (float) rule->sign;
        float edge = rule->sign > 0 ? hi[a] : lo[a];
        for (int32_t i = 0, n = (int32_t) arrlen(list); n > i; i++) {
            Instance* inst = list[i];
            if (inst->destroyed || !inst->active) continue;
            if (s * ((float) (a ? inst->y : inst->x) - edge) > (float) rule->reach &&
                s * (float) (a ? inst->vspeed : inst->hspeed) > 0.0f) Runner_destroyInstance(runner, inst, false);
        }
    }
    // No cleanup here: the next Step's own cleanup takes them out, as it does the game's own destroys.
}
#endif

#if defined(UTSW_ON)
// Both records start with a signed int16 OBJT index and occupy eight bytes.
// Refuse the entire file on any invalid record, before publishing its count;
// later frames see -1 and never retry the failed load or index its records.
_Static_assert(sizeof(OffKillRule) == 8 && sizeof(OffDoomRule) == 8, "offkill record stride");
__attribute__((noinline, noipa, optimize("Os"), section(".auditfixg_r2_offrules_text")))
static size_t auditfixgR2LoadRules(const char* path, void* data, size_t capacity, uint32_t count) {
    FILE* f = fopen(path, okMode);
    if (f == nullptr) return 0;
    size_t n = fread(data, sizeof(OffKillRule), capacity, f);
    fclose(f);
    unsigned char* bytes = data;
    for (size_t i = 0; i < n; ++i) {
        int16_t object;
        memcpy(&object, bytes + sizeof(OffKillRule)*i, sizeof(object));
        if ((uint32_t) object >= count) return 0;
    }
    return n;
}
#endif

#if defined(UTSW_ON) && defined(__3DS__)
// Outline just the cold file-read path. Fill the freed tails rather than
// shifting either existing input section or any function that follows it.

__asm__(".pushsection .text.offkill_late,\"ax\",%progbits\n.subsection 1\n.org 516, 0\n.popsection\n");

__asm__(".pushsection .omegaflowey_ifg,\"ax\",%progbits\n.subsection 1\n.org 540, 0\n.popsection\n");

#endif
