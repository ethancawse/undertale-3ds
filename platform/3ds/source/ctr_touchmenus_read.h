// Read-only popup context. The caller owns heap state and input scheduling.
#ifndef CTR_TOUCHMENUS_READ_H
#define CTR_TOUCHMENUS_READ_H
#ifdef ENABLE_WAD16
#include <limits.h>
#include <string.h>
#include "ctr_touchmenus_core.h"
#include "ctr_touch_ext.h"
#include "ctr_bottom_view.h"
#include "ctr_dialogue.h"
#include "ctr_renderer.h"
#include "instance.h"
#include "runner.h"
#include "stb_ds.h"

#ifndef TM_READ_FN
#define TM_READ_FN static __attribute__((noinline, flatten, section(".touchmenus_text")))
#endif
#ifndef TM_READ_RO
#define TM_READ_RO __attribute__((section(".touchmenus_ro")))
#endif
enum { TM_BUFFER, TM_CON, TM_HX, TM_HY, TM_TRIGGER, TM_COLUMN, TM_C0Y, TM_C1Y, TM_NV };
enum { TM_STORAGE, TM_ELEVATOR, TM_FINAL, TM_TAPES, TM_FAMILIES, TM_WRITER = TM_FAMILIES, TM_BATTLE, TM_NO };
static const char tmNames[] TM_READ_RO =
    "buffer\0con\0heartx\0hearty\0trigger\0column\0c0y\0c1y\0"
    "obj_itemswapper\0obj_elevatorpanel\0obj_finalelevatorpanel\0obj_labtv_event\0"
    "OBJ_WRITER\0obj_battlecontroller\0";
typedef struct {
    const DataWin* data;
    int32_t variable[TM_NV], object[TM_NO];
    bool named;
} TmReader;

extern bool CtrScreens_onBottom;
extern const uint32_t Popbot_layout[7];

TM_READ_FN void Tm_resolve(TmReader* state, const DataWin* data) {
    const char* name = tmNames;
    state->named = true;
    for (int k = 0; k < TM_NV + TM_NO; ++k, name += strlen(name) + 1) {
        int32_t id = -1;
        if (k < TM_NV) {
            for (uint32_t i = 0; i < data->vari.variableCount; ++i) {
                const Variable* var = &data->vari.variables[i];
                if (var->instanceType != INSTANCE_SELF || var->name == NULL || strcmp(name, var->name)) continue;
                if (var->varID < 0 || (id >= 0 && id != var->varID)) { state->named = false; break; }
                id = var->varID;
            }
            state->variable[k] = id;
        } else {
            for (uint32_t i = 0; i < data->objt.count; ++i)
                if (data->objt.objects[i].name != NULL && !strcmp(name, data->objt.objects[i].name)) {
                    id = (int32_t) i; break;
                }
            state->object[k - TM_NV] = id;
        }
        if (id < 0) state->named = false;
    }
    state->data = data;
}

TM_READ_FN int32_t Tm_value(Instance* instance, int32_t key) {
    RValue* value = IntRValueHashMap_findSlot(&instance->selfVars, key);
    if (value == NULL) return INT32_MIN;
    if (value->type == RVALUE_INT32 || value->type == RVALUE_BOOL) return value->int32;
    if (value->type == RVALUE_REAL && value->real > INT32_MIN && value->real <= INT32_MAX) {
        int32_t result = (int32_t) value->real;
        if ((double) result == value->real) return result;
    }
    return INT32_MIN;
}

// A missing owner and competing owners both reject. Invisible owners cannot
// present a target; writers/blockers still count when invisible.
TM_READ_FN Instance* Tm_only(Runner* runner, int32_t object) {
    if (object < 0 || (uint32_t) object >= runner->dataWin->objt.count) return NULL;
    Instance** list = runner->instancesByObject[object];
    Instance* found = NULL;
    for (ptrdiff_t i = 0; i < arrlen(list); ++i) {
        if (list[i]->destroyed || !list[i]->active) continue;
        if (found != NULL) return NULL;
        found = list[i];
    }
    return found;
}

TM_READ_FN bool Tm_exists(Runner* runner, int32_t object) {
    if (object < 0 || (uint32_t) object >= runner->dataWin->objt.count) return false;
    Instance** list = runner->instancesByObject[object];
    for (ptrdiff_t i = 0; i < arrlen(list); ++i)
        if (!list[i]->destroyed && list[i]->active) return true;
    return false;
}

TM_READ_FN bool Tm_view(Runner* runner, int32_t object, GMLCamera* camera, CtrBottomView* view) {
    if (CtrScreens_onBottom && CtrTouchUi_textOther()) return false;
    if (CtrLayout_bottomView(runner, view))
        return view->roomPerPx > 0 && view->roomPerPx <= 640;
    if (CtrScreens_onBottom || runner->renderer == NULL || !CtrRenderer_dialogueShown(runner->renderer)
        || !CtrRenderer_getDisplay(runner->renderer).dialogueBottom) return false;
    const CtrDialoguePolicy* policy = (const CtrDialoguePolicy*)
        ((const uint8_t*) runner->renderer + Popbot_layout[0]);
    // Same reserved-slot boundary as the popup recorder, accounting for its
    // pointer-sized saved renderer entry. Do not call its mutating policy wrap.
    int limit = CTR_DIALOGUE_BLOCK_OBJECTS - 14 - (int) (sizeof(void*) / sizeof(int32_t));
    if (policy->blockCount < 0 || policy->blockCount > limit || policy->redirect == NULL
        || object < 0 || object >= policy->objectCount || !policy->redirect[object]) return false;
    for (int i = 0; i < policy->blockCount; ++i)
        if (Tm_exists(runner, policy->blockObjects[i])) return false;
    if (runner->views[0].portX || runner->views[0].portY
        || runner->views[0].portWidth != 640 || runner->views[0].portHeight != 480) return false;
    // Popup anchors preserve the whole native view: unlike dialogue there is
    // no side-dependent crop or vertical centering to reconstruct.
    *view = (CtrBottomView) {camera->viewX, camera->viewY, 1};
    return true;
}

// Prototype reader, currently unbound. It writes only caller-owned outputs and
// its lookup cache. No Draw, script, builtin, RNG, HID or game-variable writes.
TM_READ_FN bool Tm_read(Runner* runner, TmReader* state, TuMenu* menu, CtrBottomView* view) {
    if (runner == NULL || runner->dataWin == NULL || runner->instancesByObject == NULL) return false;
    if (state->data != runner->dataWin) Tm_resolve(state, runner->dataWin);
    if (!state->named || Tm_exists(runner, state->object[TM_BATTLE])
        || !runner->viewsEnabled || !runner->views[0].enabled) return false;
    GMLCamera* camera = Runner_getCameraForView(runner, 0);
    if (camera == NULL || camera->viewWidth != 320 || camera->viewHeight != 240
        || !(camera->viewX >= -32768 && camera->viewX <= 32458)
        || !(camera->viewY >= -32768 && camera->viewY <= 32571)) return false;
    Instance* owner = NULL;
    int family = -1;
    for (int k = 0; k < TM_FAMILIES; ++k) {
        Instance* instance = Tm_only(runner, state->object[k]);
        if (instance == NULL || !instance->visible) continue;
        int buffer = Tm_value(instance, state->variable[TM_BUFFER]);
        if (buffer == INT32_MIN) continue;
        if (k == TM_STORAGE ? buffer < 6 :
            (buffer > 0 || Tm_value(instance, state->variable[TM_CON]) != 2
             || Tm_exists(runner, state->object[TM_WRITER]))) continue;
        if (owner != NULL) return false;
        owner = instance; family = k;
    }
    if (owner == NULL || !Tm_view(runner, state->object[family], camera, view)) return false;
    int x = (int) camera->viewX, y = (int) camera->viewY;
    if (family == TM_STORAGE)
        return Tm_storage(menu, owner->instanceId, Tm_value(owner, state->variable[TM_COLUMN]),
            Tm_value(owner, state->variable[TM_C0Y]), Tm_value(owner, state->variable[TM_C1Y]), x, y);
    int trigger = family == TM_TAPES ? 2 : Tm_value(owner, state->variable[TM_TRIGGER]);
    if (trigger < 0 || trigger > 2) return false;
    return Tm_panel(menu, owner->instanceId, Tm_value(owner, state->variable[TM_HX]),
        Tm_value(owner, state->variable[TM_HY]), trigger == 0 ? 11u : trigger == 1 ? 31u : 63u, x, y);
}
#endif
#endif
