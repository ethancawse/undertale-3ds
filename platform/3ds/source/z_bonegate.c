// No listed instance at Draw Pre: bypass bonepulse until one is made or changed into a listed object.
#include "ctr_bonegate.h"
#include "stb_ds.h"
#include <string.h>
#if defined(ENABLE_WAD16) && !defined(CTRPORT_HALF_CENSUS) && !defined(CTRPORT_ASG_PROBE)
#define BONEGATE_TEXT __attribute__((noinline, noipa, optimize("Os"), section(".text_bonegate")))
// The covered fields share the ARM function-pointer representation; may_alias permits the table-driven view.
typedef BoneGateDraw BoneGateSlot __attribute__((may_alias));
static struct {
    Renderer* renderer;
    RendererVtable* outer;
} boneGate __attribute__((section(".bss.bonegate_late"))) = { nullptr, nullptr }; _Static_assert(sizeof(boneGate) == 8 || sizeof(void*) != 4, "z_sdmigrate.c carves these 8 B from smSpare (SM_CARVE_BONEGATE)");

// One compiled swap loop for all six entries and both skip states. Save the table changed at phase entry: a
// temporary layer (battlesplit) can be in front when creation restores us. Its six draw wrappers delegate through
// the saved table live. Only deeper copies keep bonepulse; matching entries in the phase's outer table change.
static void BONEGATE_TEXT boneGateSet(Renderer* plain) {
    Renderer* renderer = plain != nullptr ? plain : boneGate.renderer;
    if (renderer == nullptr) return;
    if (plain != nullptr) boneGate.outer = renderer->vtable;
    boneGate.renderer = plain;
    bool skipped = *(bool*) ((uint8_t*) renderer + Bonegate_bindings.skipOffset);
    for (uint32_t i = 0; sizeof(Bonegate_bindings.draws) / sizeof(Bonegate_bindings.draws[0]) > i; i++) {
        uint32_t offset = Bonegate_bindings.draws[i].offset;
        BoneGateSlot* own = (BoneGateSlot*) ((uint8_t*) Bonegate_bindings.vtable + offset);
        BoneGateSlot* outer = (BoneGateSlot*) ((uint8_t*) boneGate.outer + offset);
        BoneGateDraw want = Bonegate_bindings.draws[i].draw[plain != nullptr][skipped];
        // Defensive: CtrRenderer_beginFrame's setSkipWrappers runs before Draw Pre, but a skip-state change could
        // replace the plain entries installed in outer. Either plain variant is ours, whatever the skip state.
        if (*outer == *own || *outer == Bonegate_bindings.draws[i].draw[1][0]
                           || *outer == Bonegate_bindings.draws[i].draw[1][1]) *outer = want;
        *own = want;
    }
}

static void BONEGATE_TEXT boneGateBegin(Runner* runner) {
    Renderer* renderer = runner->renderer;
    if (renderer == nullptr || boneGate.renderer != nullptr || runner->instancesByExactObject == nullptr) return;
    if (Bonegate_bindings.objects[0] == -2) {
        // Fill bonepulse's existing cache. Its original lazy resolver remains byte-for-byte in its retained slot;
        // after this first phase the per-draw resolver has nothing left to do (one data.win per UNDERTALE run).
        const DataWin* dw = renderer->dataWin;
        for (int32_t i = 0; BONEGATE_OBJECTS > i; i++) {
            Bonegate_bindings.objects[i] = -1;
            for (uint32_t o = 0; dw != nullptr && dw->objt.count > o && 0x7fff >= o; o++) {
                const char* name = dw->objt.objects[o].name;
                if (name != nullptr && strcmp(name, Bonegate_bindings.names[i]) == 0) {
                    Bonegate_bindings.objects[i] = (int16_t) o;
                    break;
                }
            }
        }
    }
    for (int32_t i = 0; BONEGATE_OBJECTS > i; i++) {
        int32_t o = Bonegate_bindings.objects[i];
        if (o >= 0 && arrlen(runner->instancesByExactObject[o]) > 0) return;
    }
    boneGateSet(renderer);
}

static void BONEGATE_TEXT boneGateMade(int32_t objectIndex) {
    if (boneGate.renderer == nullptr || objectIndex < 0) return;
    for (int32_t i = 0; BONEGATE_OBJECTS > i; i++) {
        if (objectIndex == Bonegate_bindings.objects[i]) {
            boneGateSet(nullptr);
            return;
        }
    }
}

// utorder's existing wrappers keep their names and slots; only their continuation calls point here. Their keep
// still runs before Draw Pre / GUI, and their creation key is assigned after our allocator returns as before.
void __real_Runner_drawPre(Runner* runner, int32_t windowW, int32_t windowH);
void __real_Runner_drawGUI(Runner* runner, int32_t windowW, int32_t windowH, int32_t targetW, int32_t targetH);
Instance* __real_Instance_create(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y);
void __real_Runner_addInstanceToObjectLists(Runner* runner, Instance* inst);
void BONEGATE_TEXT Bonegate_drawPre(Runner* runner, int32_t windowW, int32_t windowH) {
    boneGateBegin(runner);
    __real_Runner_drawPre(runner, windowW, windowH);
}
void BONEGATE_TEXT Bonegate_drawGUI(Runner* runner, int32_t windowW, int32_t windowH, int32_t targetW, int32_t targetH) {
    __real_Runner_drawGUI(runner, windowW, windowH, targetW, targetH);
    boneGateSet(nullptr);
}
Instance* BONEGATE_TEXT Bonegate_instanceCreate(uint32_t instanceId, int32_t objectIndex, GMLReal x, GMLReal y) {
    Instance* inst = __real_Instance_create(instanceId, objectIndex, x, y);
    boneGateMade(objectIndex);
    return inst;
}
void BONEGATE_TEXT __wrap_Runner_addInstanceToObjectLists(Runner* runner, Instance* inst) {
    // instance_change has already assigned the index; restore before any Create event or further draw can run.
    boneGateMade(inst->objectIndex);
    __real_Runner_addInstanceToObjectLists(runner, inst);
}
#endif
