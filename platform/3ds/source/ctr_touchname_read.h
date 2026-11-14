// Read-only native naming layout for the shared input continuation.
#ifndef CTR_TOUCHNAME_READ_H
#define CTR_TOUCHNAME_READ_H
#ifdef ENABLE_WAD16
#include <limits.h>
#include <string.h>
#include "ctr_touchname_core.h"
#include "ctr_bottom_view.h"
#include "runner.h"
#include "instance.h"
#include "vm.h"
#include "gml_array.h"

#ifndef NN_READ_FN
#define NN_READ_FN static __attribute__((unused, noinline, flatten, section(".touchname_text")))
#endif
#ifndef NN_RO
#define NN_RO __attribute__((section(".touchname_ro")))
#endif
#ifndef NN_ONLY
#define NN_ONLY To_only
extern Instance* NN_ONLY(Runner*, int32_t);
#endif
#ifndef NN_EXISTS
#define NN_EXISTS To_exists
extern bool NN_EXISTS(Runner*, int32_t);
#endif
#ifndef NN_VIEW
#define NN_VIEW To_view
extern bool NN_VIEW(Runner*, int32_t, GMLCamera*, CtrBottomView*);
#endif

enum { NN_PHASE, NN_ROW, NN_COL, NN_ROWS, NN_COLS, NN_X, NN_Y, NN_MAP,
       NN_MX0, NN_MX1, NN_MX2, NN_MY, NN_CX0, NN_CX1, NN_CX2, NN_CY,
       NN_CHARSET, NN_SELECTED, NN_ALLOW, NN_NV };
enum { NN_OWNER, NN_BATTLE, NN_NO };
static const char nnNames[] NN_RO =
    "naming\0selected_row\0selected_col\0rows\0cols\0xmap\0ymap\0charmap\0"
    "menu_x0\0menu_x1\0menu_x2\0menu_y\0charset_x0\0charset_x1\0charset_x2\0charset_y\0"
    "selected_charmap\0selected2\0allow\0language\0obj_intromenu\0obj_battlecontroller\0";
static const char nnJapanese[] NN_RO = "ja";

typedef struct {
    const DataWin* data;
    int32_t variable[NN_NV + 1], object[NN_NO];
    bool named;
} NnReader;

// This exact snapshot is compared by the caller before reusing a
// gesture. It excludes the live cursor, which ordinary arrows may change.
typedef struct {
    NnGrid grid;
    const GMLArray *xmap, *ymap, *charmap;
    int phase, charset, allow;
} NnSnapshot;

NN_READ_FN void Nn_resolve(NnReader* cache, const DataWin* data) {
    const char* name = nnNames;
    cache->named = true;
    for (int k = 0; k < NN_NV + 1 + NN_NO; ++k, name += strlen(name) + 1) {
        int id = -1;
        if (k <= NN_NV) {
            for (uint32_t i = 0; i < data->vari.variableCount; ++i) {
                const Variable* var = &data->vari.variables[i];
                if (var->instanceType != (k == NN_NV ? INSTANCE_GLOBAL : INSTANCE_SELF)
                    || var->name == NULL || strcmp(var->name, name)) continue;
                if (var->varID < 0 || (id >= 0 && var->varID != id)) { cache->named = false; break; }
                id = var->varID;
            }
            cache->variable[k] = id;
        } else {
            for (uint32_t i = 0; i < data->objt.count; ++i)
                if (data->objt.objects[i].name && !strcmp(data->objt.objects[i].name, name)) {
                    id = (int) i; break;
                }
            cache->object[k - NN_NV - 1] = id;
        }
        if (id < 0) cache->named = false;
    }
    cache->data = data;
}

// No strengthening, growth or refcount changes. Legacy BC16 packed rows and
// modern nested rows are read through their real storage forms.
NN_READ_FN RValue* Nn_slot(Instance* owner, int key, int row, int col) {
    RValue* value = IntRValueHashMap_findSlot(&owner->selfVars, key);
    if (row < 0) return value;
    if (!value || value->type != RVALUE_ARRAY || !value->array) return NULL;
    GMLArray* array = value->array;
    if (array->type == GML_LEGACY_ARRAY) return GMLArray_slot(array, row * GML_LEGACY_ARRAY_STRIDE + col);
    if (array->type != GML_MODERN_ARRAY) return NULL;
    if (row == 0 && col >= 0) return GMLArray_slot(array, col);
    return NULL;
}

NN_READ_FN int Nn_number(const RValue* value) {
    if (!value) return INT_MIN;
    if (value->type == RVALUE_INT32 || value->type == RVALUE_BOOL) return value->int32;
    if (value->type == RVALUE_REAL && value->real > INT_MIN && value->real <= INT_MAX) {
        int result = (int) value->real;
        if ((double) result == value->real) return result;
    }
    return INT_MIN;
}

NN_READ_FN bool Nn_read(Runner* runner, NnReader* cache, NnMenu* menu,
                        NnSnapshot* snapshot, CtrBottomView* view) {
    if (!runner || !runner->dataWin || !runner->instancesByObject || !runner->vmContext
        || !runner->vmContext->globalScopeInstance) return false;
    if (cache->data != runner->dataWin) Nn_resolve(cache, runner->dataWin);
    if (!cache->named || NN_EXISTS(runner, cache->object[NN_BATTLE])) return false;
    Instance* owner = NN_ONLY(runner, cache->object[NN_OWNER]);
    if (!owner || !owner->instanceId || !owner->visible || !runner->viewsEnabled || !runner->views[0].enabled) return false;
    GMLCamera* camera = Runner_getCameraForView(runner, 0);
    if (!camera || camera->viewWidth != 320 || camera->viewHeight != 240
        || !NN_VIEW(runner, cache->object[NN_OWNER], camera, view)
        || !(view->roomX0 >= -32768 && view->roomX0 <= 32767)
        || !(view->roomY0 >= -32768 && view->roomY0 <= 32767)
        || !(view->roomPerPx > 0 && view->roomPerPx <= 640)) return false;
    memset(snapshot, 0, sizeof(*snapshot));
    int v[NN_NV];
    for (int i = 0; i < NN_NV; ++i) v[i] = Nn_number(Nn_slot(owner, cache->variable[i], -1, 0));
    snapshot->phase = v[NN_PHASE];
    if (v[NN_PHASE] == 2) {
        if ((v[NN_ALLOW] != 0 && v[NN_ALLOW] != 1) || v[NN_SELECTED] < 0
            || v[NN_SELECTED] > (v[NN_ALLOW] ? 1 : 0)) return false;
        snapshot->allow = v[NN_ALLOW];
        *menu = (NnMenu) {.id = owner->instanceId, .ctx = TU_CHOICE,
            .count = v[NN_ALLOW] ? 2 : 1, .cursor = (int8_t) v[NN_SELECTED]};
        for (int i = 0; i < menu->count; ++i) {
            menu->item[i] = (TouchRect) {40 + i*160, 190, 120 + i*160, 220};
            menu->next[i][0] = menu->next[i][1] = -1;
            menu->next[i][2] = menu->next[i][3] = v[NN_ALLOW] ? 1 - i : -1;
        }
        return true;
    }
    if (v[NN_PHASE] != 1 || v[NN_ROWS] < 1 || v[NN_ROWS] > 8 || v[NN_COLS] < 3 || v[NN_COLS] > 15) return false;
    RValue* language = Nn_slot(runner->vmContext->globalScopeInstance, cache->variable[NN_NV], -1, 0);
    if (!language || language->type != RVALUE_STRING || !language->string) return false;
    NnGrid* grid = &snapshot->grid;
    grid->japanese = !strcmp(language->string, nnJapanese);
    snapshot->charset = v[NN_CHARSET];
    if (snapshot->charset < (grid->japanese ? 1 : 0) || snapshot->charset > (grid->japanese ? 3 : 0)) return false;
    grid->rows = v[NN_ROWS]; grid->cols = v[NN_COLS];
    for (int i = 0; i < 3; ++i) {
        if (v[NN_MX0+i] < 0 || v[NN_MX0+i] > 304) return false;
        grid->menuX[i] = (int16_t) v[NN_MX0+i];
        if (grid->japanese) {
            if (v[NN_CX0+i] < 0 || v[NN_CX0+i] > 304) return false;
            grid->charsetX[i] = (int16_t) v[NN_CX0+i];
        }
    }
    if (v[NN_MY] < 0 || v[NN_MY] > 224 || (grid->japanese && (v[NN_CY] < 0 || v[NN_CY] > 224))) return false;
    grid->menuY = (int16_t) v[NN_MY];
    if (grid->japanese) grid->charsetY = (int16_t) v[NN_CY];
    for (int c = 0; c < grid->cols; ++c) {
        int x = Nn_number(Nn_slot(owner, cache->variable[NN_X], 0, c));
        if (x < 0 || x > 304) return false;
        grid->x[c] = (int16_t) x;
    }
    RValue* map = Nn_slot(owner, cache->variable[NN_MAP], -1, 0);
    if (!map || map->type != RVALUE_ARRAY || !map->array) return false;
    for (int r = 0; r < grid->rows; ++r) {
        int y = Nn_number(Nn_slot(owner, cache->variable[NN_Y], 0, r));
        if (y < 0 || y > 224) return false;
        grid->y[r] = (int16_t) y;
        for (int c = 0; c < grid->cols; ++c) {
            RValue* cell;
            if (map->array->type == GML_MODERN_ARRAY) {
                RValue* row = GMLArray_slot(map->array, r);
                if (!row || row->type != RVALUE_ARRAY || !row->array) return false;
                cell = GMLArray_slot(row->array, c);
            } else if (map->array->type == GML_LEGACY_ARRAY)
                cell = GMLArray_slot(map->array, r * GML_LEGACY_ARRAY_STRIDE + c);
            else return false;
            if (!cell || cell->type != RVALUE_STRING || !cell->string) return false;
            if (*cell->string) grid->filled[r] |= 1u << c;
        }
    }
    snapshot->charmap = map->array;
    snapshot->xmap = Nn_slot(owner, cache->variable[NN_X], -1, 0)->array;
    snapshot->ymap = Nn_slot(owner, cache->variable[NN_Y], -1, 0)->array;
    return Nn_graph(menu, grid, owner->instanceId, v[NN_ROW], v[NN_COL]);
}
#endif
#endif
