// Native shop and SAVE contexts. Reads only: game handlers own every mutation.
#ifndef CTR_TOUCHSHOPS_READ_H
#define CTR_TOUCHSHOPS_READ_H
#ifdef ENABLE_WAD16
#include <limits.h>
#include <string.h>
#include "ctr_touchui_core.h"
#include "ctr_bottom_view.h"
#include "runner.h"
#include "instance.h"
#include "gml_array.h"
#include "vm.h"

#ifndef TS_FN
#define TS_FN static __attribute__((noinline, flatten, section(".text_touchshops")))
#endif
#ifndef TS_RO
#define TS_RO __attribute__((section(".rodata_touchshops")))
#endif
extern Instance* To_only(Runner*, int32_t);
extern bool To_exists(Runner*, int32_t);
extern bool CtrScreens_onBottom;
enum { TS_MENU, TS_COORD, TS_SELLPOS, TS_SELLYES, TS_BUFFER, TS_HALT, TS_CON,
       TS_BRATTY, TS_CATTY, TS_VALUE, TS_INTERACT, TS_PAGE, TS_SAVECOORD, TS_LANG, TS_NV };
enum { TS_SHOP1, TS_SHOP2, TS_SHOP3, TS_SHOP4, TS_SHOP5, TS_SAVE, TS_BATTLE, TS_WRITER, TS_DIALOGUE, TS_NO };
static const char tsNames[] TS_RO =
    "menu\0menuc\0sellpos\0sellpos2\0buffer\0halt\0con\0brattywriter\0cattywriter\0value\0"
    "interact\0menuno\0menucoord\0language\0"
    "obj_shop1\0obj_shop2\0obj_shop3\0obj_shop4\0obj_shop5\0obj_overworldcontroller\0"
    "obj_battlecontroller\0OBJ_WRITER\0obj_dialoguer\0";
typedef struct {
    const DataWin* data;
    int32_t variable[TS_NV], object[TS_NO];
    uint32_t phase;
    bool named;
} TsReader;

TS_FN void Ts_resolve(TsReader* c, const DataWin* data) {
    const char* name = tsNames;
    c->named = true;
    for (int k = 0; k < TS_NV + TS_NO; ++k, name += strlen(name) + 1) {
        int32_t id = -1;
        if (k < TS_NV) {
            for (uint32_t i = 0; i < data->vari.variableCount; ++i) {
                const Variable* v = &data->vari.variables[i];
                if (v->instanceType != (k < TS_INTERACT ? INSTANCE_SELF : INSTANCE_GLOBAL)
                    || !v->name || strcmp(v->name, name)) continue;
                if (v->varID < 0 || (id >= 0 && id != v->varID)) { c->named = false; break; }
                id = v->varID;
            }
            c->variable[k] = id;
        } else {
            for (uint32_t i = 0; i < data->objt.count; ++i)
                if (data->objt.objects[i].name && !strcmp(data->objt.objects[i].name, name)) { id = (int32_t) i; break; }
            c->object[k - TS_NV] = id;
        }
        if (id < 0) c->named = false;
    }
    c->data = data;
}

TS_FN int Ts_cell(Instance* owner, int key, int index) {
    RValue* v = IntRValueHashMap_findSlot(&owner->selfVars, key);
    if (index >= 0) {
        if (!v || v->type != RVALUE_ARRAY || !v->array) return INT_MIN;
        v = GMLArray_slot(v->array, index);
    }
    if (!v) return INT_MIN;
    if (v->type == RVALUE_INT32 || v->type == RVALUE_BOOL) return v->int32;
    if (v->type == RVALUE_REAL && v->real > INT_MIN && v->real <= INT_MAX) {
        int n = (int) v->real;
        if ((double) n == v->real) return n;
    }
    return INT_MIN;
}

// Shop5's exact value-dependent arrow graph, including its asymmetric Exit.
TS_FN int Ts_sellNext(int from, int direction, unsigned occupied) {
    if (direction == 2) return from < 8 && (from & 1) ? from - 1 : from;
    if (direction == 3) return from < 8 && !(from & 1) && (occupied & (1u << (from + 1))) ? from + 1 : from;
    if (direction == 0) {
        if (from < 2) return from;
        if (from < 8) return from - 2;
        for (int i = 6; i >= 0; i -= 2) if (occupied & (1u << i)) return i;
        return 0;
    }
    if (from >= 6) return 8;
    if (occupied & (1u << (from + 2))) return from + 2;
    if ((from & 1) && (occupied & (1u << (from + 1)))) return from + 1;
    return 8;
}

// Only the shop's own two named writers can jointly finish a shop3 line.
TS_FN int Ts_text(Runner* r, TsReader* c, Instance* owner, int family) {
    Instance** list = r->instancesByObject[c->object[TS_WRITER]];
    int count = 0, typing = 0, owned = 0, cattyHalt = 0;
    int bratty = Ts_cell(owner, c->variable[TS_BRATTY], -1);
    int catty = Ts_cell(owner, c->variable[TS_CATTY], -1);
    for (ptrdiff_t i = 0; i < arrlen(list); ++i) {
        Instance* w = list[i];
        if (!w->active || w->destroyed) continue;
        int halt = Ts_cell(w, c->variable[TS_HALT], -1);
        if (!w->visible || halt < 0 || ++count > 2) return 0;
        if (family == TS_SHOP3) {
            int bit = bratty >= 0 && w->instanceId == (uint32_t) bratty ? 1
                : catty >= 0 && w->instanceId == (uint32_t) catty ? 2 : 0;
            if (!bit || (owned & bit)) return 0;
            owned |= bit;
            if (bit == 2) cattyHalt = halt;
        }
        typing |= halt == 0;
    }
    if (!count || (family != TS_SHOP3 && count != 1)) return 0;
    // Both writers must finish. Native con=900 additionally waits for Catty's
    // halt=6; con=4/998/999 and the normal conversation keep their native Z.
    if (count == 2 && !typing) {
        int con = Ts_cell(owner, c->variable[TS_CON], -1);
        if (owned != 3 || con < 0 || (con == 900 && cattyHalt != 6)) return 0;
    }
    return typing ? TOUCH_KEY_X : TOUCH_KEY_Z;
}

TS_FN bool Ts_read(Runner* r, TsReader* c, TuMenu* m, CtrBottomView* view) {
    if (!r || !r->dataWin || !r->instancesByObject || !r->vmContext
        || !r->vmContext->globalScopeInstance || !CtrScreens_onBottom
        || !r->viewsEnabled || !r->views[0].enabled) return false;
    if (c->data != r->dataWin) Ts_resolve(c, r->dataWin);
    if (!c->named || To_exists(r, c->object[TS_BATTLE]) || To_exists(r, c->object[TS_DIALOGUE])) return false;
    GMLCamera* camera = Runner_getCameraForView(r, 0);
    if (!camera || camera->viewWidth != 320 || camera->viewHeight != 240
        || !(camera->viewX >= -32768 && camera->viewX <= 32400)
        || !(camera->viewY >= -32768 && camera->viewY <= 32500)
        || !CtrLayout_bottomView(r, view) || view->roomX0 != camera->viewX
        || view->roomY0 != camera->viewY || view->roomPerPx != 1) return false;
    Instance* global = r->vmContext->globalScopeInstance;
    Instance* owner = NULL;
    int family = -1;
    for (int i = 0; i <= TS_SAVE; ++i) {
        if (i == TS_SAVE && (family >= 0 || Ts_cell(global, c->variable[TS_INTERACT], -1) != 5
                            || Ts_cell(global, c->variable[TS_PAGE], -1) != 4)) continue;
        if (!To_exists(r, c->object[i])) continue;
        Instance* candidate = To_only(r, c->object[i]);
        if (!candidate || !candidate->visible) return false;
        // The overworld controller may survive behind a whole shop.
        if (owner) return false;
        owner = candidate; family = i;
    }
    if (!owner) return false;
    RValue* language = IntRValueHashMap_findSlot(&global->selfVars, c->variable[TS_LANG]);
    if (!language || language->type != RVALUE_STRING || !language->string) return false;
    bool ja = language->string[0] == 'j' && language->string[1] == 'a' && !language->string[2];
    int page = family == TS_SAVE ? 7 : Ts_cell(owner, c->variable[TS_MENU], -1);
    if (page < 0 || (family != TS_SAVE && (page > 6 || (page > 4 && family != TS_SHOP5)))) return false;
    if (family == TS_SAVE && To_exists(r, c->object[TS_WRITER])) return false;
    int cursor = family == TS_SAVE ? Ts_cell(global, c->variable[TS_SAVECOORD], 4)
        : Ts_cell(owner, c->variable[page == 5 ? TS_SELLPOS : page == 6 ? TS_SELLYES : TS_COORD], page < 5 ? page : -1);
    *m = (TuMenu) {.id = owner->instanceId, .ctx = TU_CHOICE,
        .count = page == 0 ? 4 : page == 1 || page == 3 ? 5 : page == 5 ? 9 : 2, .cursor = (int8_t) cursor};
    memset(m->next, -1, sizeof(m->next));
    int cx = family == TS_SAVE ? (int) camera->viewX : 0;
    int cy = family == TS_SAVE ? (int) camera->viewY : 0;
    unsigned occupied = 0;
    if (page == 5) for (int i = 0; i < 8; ++i) {
        int value = Ts_cell(owner, c->variable[TS_VALUE], i);
        if (value == INT_MIN) return false;
        if (value != 0) occupied |= 1u << i;
    }
    c->phase = (uint32_t) (page + family * 8 + ja * 64) | (occupied << 8);
    if (page == 4 || (page == 7 && cursor == 2)) {
        m->ctx = TU_TEXT;
        m->box = (TouchRect) {cx, cy + (page == 4 ? 100 : 0), cx + 320, cy + 240};
        m->textKey = page == 7 ? TOUCH_KEY_Z : (uint32_t) Ts_text(r, c, owner, family);
        return m->textKey != 0;
    }
    if (cursor < 0 || cursor >= m->count || (page == 5 && cursor < 8 && !(occupied & (1u << cursor)))
        || (page == 6 && (Ts_cell(owner, c->variable[TS_BUFFER], -1) == INT_MIN
                         || Ts_cell(owner, c->variable[TS_BUFFER], -1) > 0))) return false;
    for (int i = 0; i < m->count; ++i) {
        int x = page == 0 || page == 2 ? (ja ? 250 : 240) : 30;
        int y = page == 2 ? 170 + 15 * i : 130 + 20 * i;
        int width = page == 0 || page == 2 ? 64 : 270;
        if (page == 5) { x = (ja ? 25 : 30) + (i & 1) * (ja ? 151 : 140); y = i == 8 ? 210 : 130 + 20 * (i / 2); width = 125; }
        if (page == 6 || page == 7) { x = page == 6 ? 80 + 110 * i : (ja ? 92 + 95 * i : 85 + 90 * i); y = page == 6 ? (ja ? 186 : 180) : 110; width = 78; }
        if (page != 5 || i == 8 || (occupied & (1u << i))) m->item[i] = (TouchRect) {cx + x - 12, cy + y - 3, cx + x + width, cy + y + (page == 2 ? 11 : 16)};
        if (page >= 6) m->next[i][2] = m->next[i][3] = (int8_t) (1 - i);
        else if (page == 5) for (int d = 0; d < 4; ++d) m->next[i][d] = (int8_t) Ts_sellNext(i, d, occupied);
        else { m->next[i][0] = (int8_t) ((i + m->count - 1) % m->count); m->next[i][1] = (int8_t) ((i + 1) % m->count); }
    }
    return true;
}
#endif
#endif
