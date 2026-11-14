// Read-only overworld menu context. No VM evaluation or game writes.
#ifndef CTR_TOUCHOVERWORLD_READ_H
#define CTR_TOUCHOVERWORLD_READ_H
#ifdef ENABLE_WAD16
#include <limits.h>
#include <string.h>
#include "ctr_touchoverworld_core.h"
#include "ctr_bottom_view.h"
#include "instance.h"
#include "runner.h"
#include "gml_array.h"
#include "vm.h"

#ifndef TO_READ_FN
#define TO_READ_FN static __attribute__((noinline, flatten, section(".touchoverworld_text")))
#endif
#ifndef TO_READ_RO
#define TO_READ_RO __attribute__((section(".touchoverworld_ro")))
#endif
// Appended AAC branch thunks reach the unchanged local popup helpers.
#ifndef TO_ONLY
#define TO_ONLY To_only
extern Instance* TO_ONLY(Runner*, int32_t);
#endif
#ifndef TO_EXISTS
#define TO_EXISTS To_exists
extern bool TO_EXISTS(Runner*, int32_t);
#endif
#ifndef TO_VIEW
#define TO_VIEW To_view
extern bool TO_VIEW(Runner*, int32_t, GMLCamera*, CtrBottomView*);
#endif

enum { TO_INTERACT, TO_PAGE, TO_COORD, TO_CHOICES, TO_ITEMS, TO_CONTACTS, TO_FLAGS, TO_NV };
enum { TO_OWNER, TO_BATTLE, TO_WRITER, TO_NO };
static const char toNames[] TO_READ_RO =
    "interact\0menuno\0menucoord\0menuchoice\0item\0phone\0flag\0"
    "obj_overworldcontroller\0obj_battlecontroller\0OBJ_WRITER\0";
typedef struct {
    const DataWin* data;
    int32_t variable[TO_NV], object[TO_NO];
    int32_t page;
    bool named;
} ToReader;

TO_READ_FN void To_resolve(ToReader* cache, const DataWin* data) {
    const char* name = toNames;
    cache->named = true;
    for (int k = 0; k < TO_NV + TO_NO; ++k, name += strlen(name) + 1) {
        int32_t id = -1;
        if (k < TO_NV) {
            for (uint32_t i = 0; i < data->vari.variableCount; ++i) {
                const Variable* var = &data->vari.variables[i];
                if (var->instanceType != INSTANCE_GLOBAL || var->name == NULL || strcmp(name, var->name)) continue;
                if (var->varID < 0 || (id >= 0 && id != var->varID)) { cache->named = false; break; }
                id = var->varID;
            }
            cache->variable[k] = id;
        } else {
            for (uint32_t i = 0; i < data->objt.count; ++i)
                if (data->objt.objects[i].name != NULL && !strcmp(name, data->objt.objects[i].name)) {
                    id = (int32_t) i; break;
                }
            cache->object[k - TO_NV] = id;
        }
        if (id < 0) cache->named = false;
    }
    cache->data = data;
}

TO_READ_FN int To_cell(Instance* scope, int32_t key, int index) {
    RValue* value = IntRValueHashMap_findSlot(&scope->selfVars, key);
    if (index >= 0) {
        if (value == NULL || value->type != RVALUE_ARRAY || value->array == NULL) return INT32_MIN;
        value = GMLArray_slot(value->array, index);
    }
    if (value == NULL) return INT32_MIN;
    if (value->type == RVALUE_INT32 || value->type == RVALUE_BOOL) return value->int32;
    if (value->type == RVALUE_REAL && value->real > INT32_MIN && value->real <= INT32_MAX) {
        int32_t result = (int32_t) value->real;
        if ((double) result == value->real) return result;
    }
    return INT32_MIN;
}

TO_READ_FN bool To_read(Runner* runner, ToReader* cache, TuMenu* menu, CtrBottomView* view) {
    if (runner == NULL || runner->dataWin == NULL || runner->instancesByObject == NULL
        || runner->vmContext == NULL || runner->vmContext->globalScopeInstance == NULL) return false;
    if (cache->data != runner->dataWin) To_resolve(cache, runner->dataWin);
    if (!cache->named || TO_EXISTS(runner, cache->object[TO_BATTLE])
        || TO_EXISTS(runner, cache->object[TO_WRITER]) || !runner->viewsEnabled || !runner->views[0].enabled) return false;
    Instance* owner = TO_ONLY(runner, cache->object[TO_OWNER]);
    Instance* global = runner->vmContext->globalScopeInstance;
    if (owner == NULL || !owner->visible || To_cell(global, cache->variable[TO_INTERACT], -1) != 5) return false;
    GMLCamera* camera = Runner_getCameraForView(runner, 0);
    if (camera == NULL || camera->viewWidth != 320 || camera->viewHeight != 240
        || !(camera->viewX >= -32768 && camera->viewX <= 32501)
        || !(camera->viewY >= -32768 && camera->viewY <= 32571)
        || !TO_VIEW(runner, cache->object[TO_OWNER], camera, view)) return false;
    int page = To_cell(global, cache->variable[TO_PAGE], -1);
    if (page < 0 || page > 7 || page == 4) return false;
    int cursor = To_cell(global, cache->variable[TO_COORD], page);
    int count = page == TO_ROOT || page == TO_ACTION ? 3 : page == TO_STAT ? 1 : page == TO_TAKE ? 10 : 8;
    unsigned occupied = 0, drawn = 0;
    for (int i = 0; i < count; ++i) {
        int value = page == TO_STAT || page == TO_ACTION ? 1
            : To_cell(global, cache->variable[page == TO_ROOT ? TO_CHOICES : page == TO_PHONE ? TO_CONTACTS
                : page == TO_TAKE ? TO_FLAGS : TO_ITEMS], i + (page == TO_TAKE ? 300 : 0));
        if (value < 0 || (page == TO_ROOT && value > 1)) return false;
        if (value != 0) occupied |= 1u << i;
    }
    drawn = occupied;
    if (page == TO_PHONE) drawn &= 127;
    if (!To_graph(menu, owner->instanceId, page, cursor, occupied, drawn, (int) camera->viewX, (int) camera->viewY)) return false;
    cache->page = page; // caller must reset its gesture when this page changes
    return true;
}
#endif
#endif
