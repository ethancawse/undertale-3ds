// Quiz confirmation through native SOUL movement (review decision, 11-09).
// The original sampler owns HID, APT/reset epochs, scripts, gesture cancellation
// and the final key merge. Only its two external call destinations are routed.
#ifdef ENABLE_WAD16
#include <stdlib.h>
#include <string.h>
#include "ctr_input.h"
#include "ctr_bottom_view.h"
#include "ctr_battletouch.h"
#include "ctr_touch_ext.h"
#include "data_win.h"
#include "instance.h"
#include "runner.h"
#include "stb_ds.h"
#ifdef CTRPORT_TOUCH_TEST
#include "log.h"
#endif

#define TQ_CODE __attribute__((noinline, flatten, section(".aacout.text.touchquiz")))
#define TQ_RO __attribute__((section(".aacout.rodata.touchquiz")))
#define TQ_INLINE __attribute__((always_inline))
#define TQ_STEP static TQ_CODE
#include "ctr_touchquiz_core.h"

enum { Q_PHASE, Q_ANSWERED, Q_QNO, Q_WIDTH, Q_SCALE, Q_MOVEMENT, Q_CONFUSE, Q_NV };
enum { Q_OWNER, Q_NODE, Q_HEART, Q_NO };
static const char tqNames[] TQ_RO =
    "phase\0answered\0qno\0txtsize\0sizefactor\0movement\0confuse\0"
    "obj_questionasker\0obj_answernodule\0obj_heart\0";
#ifdef CTRPORT_TOUCH_TEST
static const char tqTrace[] TQ_RO =
    "[touchquiz] f=%lu ready=%d reset=%d pen=%d panel=(%d,%d) room=(%d,%d) soul=(%ld,%ld) mode=%d->%d hold=%03lx targets=%u\n";
static const char tqTargetTrace[] TQ_RO =
    "[touchquiz-target] f=%lu n=%u rect=(%d,%d,%d,%d) centre=(%ld,%ld) view=(%ld,%ld,%ld)\n";
#endif
typedef struct {
    const DataWin* data;
    int32_t variable[Q_NV], object[Q_NO];
    uint32_t physical;
    TqState gesture;
    CtrBottomView view;
    bool named, shown;
} TqLive;
static TqLive* tqLive __attribute__((section(".bss.touchquiz_late")));

CtrInputEdges __real_CtrInput_take(const CtrInputMap*, uint32_t, uint32_t, bool, bool, CtrInputStats*);

// This is a forwarding observer of arguments already supplied to the sample
// owner, not another HID reader. None-mapped buttons still cancel steering.
TQ_CODE CtrInputEdges CtrTouchQuiz_buttons(const CtrInputMap* map, uint32_t held, uint32_t down,
                                          bool enabled, bool suppressHeld, CtrInputStats* stats) {
    if (tqLive != NULL) {
        tqLive->physical = (held | down) & ~(1u << 20); // libctru KEY_TOUCH
        // The sample owner decides eligibility after its scripted override.
        // Raw disabled input is not a reset here: test pens run with it false.
        // Ordinary disabled samples arrive at the claim with in->reset true.
        // Off bypasses the claim, so it still has to clear this state here.
        if (suppressHeld || CtrBattletouch_modeNow() == CTR_BATTLETOUCH_OFF)
            Tq_reset(&tqLive->gesture, false);
    }
    return __real_CtrInput_take(map, held, down, enabled, suppressHeld, stats);
}

static TQ_CODE void tqResolve(TqLive* live, DataWin* dw) {
    const char* name = tqNames;
    live->named = true;
    for (int k = 0; k < Q_NV + Q_NO; ++k, name += strlen(name) + 1) {
        int32_t id = -1;
        if (k < Q_NV) {
            for (uint32_t i = 0; i < dw->vari.variableCount; ++i) {
                Variable* v = &dw->vari.variables[i];
                if (v->instanceType != INSTANCE_SELF || v->name == NULL || strcmp(v->name, name)) continue;
                if (v->varID < 0 || (id >= 0 && id != v->varID)) { live->named = false; break; }
                id = v->varID;
            }
            live->variable[k] = id;
        } else {
            for (uint32_t i = 0; i < dw->objt.count; ++i) {
                const char* object = dw->objt.objects[i].name;
                if (object != NULL && strcmp(object, name) == 0) { id = (int32_t) i; break; }
            }
            live->object[k - Q_NV] = id;
        }
        if (id < 0) live->named = false;
    }
    live->data = dw;
}
static TQ_CODE Instance* tqFirst(Runner* r, int32_t object) {
    if (r->instancesByObject == NULL || object < 0 || (uint32_t) object >= r->dataWin->objt.count) return NULL;
    Instance** list = r->instancesByObject[object];
    for (ptrdiff_t i = 0; i < arrlen(list); ++i)
        if (list[i]->active && !list[i]->destroyed) return list[i];
    return NULL;
}
static TQ_CODE float tqValue(Instance* instance, int32_t id, float fallback) {
    RValue* v = IntRValueHashMap_findSlot(&instance->selfVars, id);
    if (v == NULL) return fallback;
    if (v->type == RVALUE_REAL) return (float) v->real;
    if (v->type == RVALUE_INT32 || v->type == RVALUE_BOOL) return (float) v->int32;
#ifndef NO_RVALUE_INT64
    if (v->type == RVALUE_INT64) return (float) v->int64;
#endif
    return fallback;
}
// No shared collision-header helper is called: keep its GCC emission out of
// old objects. Quiz icons use unrotated positive-scale sprites; unsupported
// geometry fails closed, with no key or gameplay write.
static TQ_CODE bool tqSprite(Runner* r, Instance* instance, TqAnswer* answer) {
    int32_t index = instance->spriteIndex;
    if (index < 0 || (uint32_t) index >= r->dataWin->sprt.count || instance->imageAngle != 0
        || !(instance->imageXscale > 0) || !(instance->imageYscale > 0)) return false;
    Sprite* sprite = &r->dataWin->sprt.sprites[index];
    if (!sprite->present || sprite->width == 0 || sprite->height == 0) return false;
    float left = instance->x - (float) sprite->originX * instance->imageXscale;
    float top = instance->y - (float) sprite->originY * instance->imageYscale;
    float right = left + (float) sprite->width * instance->imageXscale;
    float bottom = top + (float) sprite->height * instance->imageYscale;
    if (!(left >= -32768 && top >= -32768 && right < 32767 && bottom < 32767)) return false;
    *answer = (TqAnswer) {.id = instance->instanceId,
        .target = {(int16_t) left, (int16_t) top, (int16_t) right, (int16_t) bottom},
        .x = (int32_t) ((left + right) * 8.0f), .y = (int32_t) ((top + bottom) * 8.0f)};
    return true;
}
static TQ_CODE bool tqRead(Runner* r, TqLive* live, TqMenu* menu, CtrBottomView* view) {
    Instance* owner = tqFirst(r, live->object[Q_OWNER]);
    Instance* heart = tqFirst(r, live->object[Q_HEART]);
    if (owner == NULL || heart == NULL || tqValue(owner, live->variable[Q_PHASE], -1) != 2
        || tqValue(heart, live->variable[Q_MOVEMENT], -1) != 1
        || tqValue(heart, live->variable[Q_CONFUSE], -1) != 0
        || !CtrLayout_bottomView(r, view) || !(view->roomPerPx > 0)) return false;
    TqAnswer soul;
    if (!tqSprite(r, heart, &soul)) return false;
    menu->owner = owner->instanceId;
    menu->heart = heart->instanceId;
    menu->x = soul.x; menu->y = soul.y;
    Instance** nodes = r->instancesByObject[live->object[Q_NODE]];
    for (ptrdiff_t i = 0; i < arrlen(nodes); ++i) {
        Instance* node = nodes[i];
        if (!node->active || node->destroyed || !node->visible
            || tqValue(node, live->variable[Q_ANSWERED], 1) != 0) continue;
        if (menu->count == 4) return false;
        TqAnswer* answer = &menu->answer[menu->count];
        if (!tqSprite(r, node, answer)) return false;
        float row = tqValue(node, live->variable[Q_QNO], -1);
        float width = tqValue(node, live->variable[Q_WIDTH], -1);
        float scale = tqValue(node, live->variable[Q_SCALE], -1);
        if (!(row >= 0 && row <= 3 && width >= 0 && scale > 0 && scale <= 1)) return false;
        width *= scale;
        if (width > 160) width = 160;
        // The native Draw uses right-aligned text x-20 for A/C, left-aligned
        // text x+60 for B/D. Include that live text span and its own icon.
        int edge = (int) (node->x + (((int) row & 1) ? 60.0f + width : -20.0f - width));
        if (edge < -32768 || edge > 32767) return false;
        if ((int) row & 1) { if (edge > answer->target.x1) answer->target.x1 = (int16_t) edge; }
        else if (edge < answer->target.x0) answer->target.x0 = (int16_t) edge;
        ++menu->count;
    }
    return menu->count != 0;
}

// Mutable ABI at the call-operand boundary. The existing read-only callback
// still receives the original sample. battle is a caller-local claim veto:
// only an accepting quiz clears it, so the unchanged merge accepts this claim
// and cancels the battle gesture. reset and all scope/controller state stay
// intact. The caller uses battle only in its post-call claim guard.
TQ_CODE bool CtrTouchQuiz_claim(CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold) {
    bool ordinary = CtrTouchExt_sample(in, pulse, hold);
    TqLive* live = tqLive;
    if (in->reset) {
        if (live != NULL) { Tq_reset(&live->gesture, in->pen); live->shown = false; }
        *pulse = *hold = 0;
        return false;
    }
    Runner* r = in->runner;
    if (in->battle && r != NULL && r->dataWin != NULL && live == NULL) {
        live = calloc(1, sizeof(*live));
        if (live != NULL) { tqLive = live; Tq_reset(&live->gesture, in->pen); }
    }
    if (live == NULL) return ordinary;
    if (!in->battle || r == NULL || r->dataWin == NULL) {
        Tq_reset(&live->gesture, in->pen); live->shown = false;
        return ordinary;
    }
    bool reset = live->data != r->dataWin;
    if (reset) tqResolve(live, r->dataWin);
    TqMenu menu = {0};
    CtrBottomView view = {0};
    bool shown = live->named && tqRead(r, live, &menu, &view);
#ifdef CTRPORT_TOUCH_TEST
    bool trace = shown != live->shown || in->pen || live->gesture.pen || live->gesture.mode == TQ_STEER;
    bool targets = shown && !live->shown;
    int before = live->gesture.mode;
#endif
    reset |= shown != live->shown || view.roomX0 != live->view.roomX0
        || view.roomY0 != live->view.roomY0 || view.roomPerPx != live->view.roomPerPx;
    live->shown = shown; live->view = view;
    if (!shown) {
        Tq_reset(&live->gesture, in->pen);
#ifdef CTRPORT_TOUCH_TEST
        if (trace) logInfo(tqTrace, (unsigned long) r->frameCount, 0, (int) reset, (int) in->pen,
            (int) in->px, (int) in->py, 0, 0, 0L, 0L, before, (int) live->gesture.mode, 0UL, 0u);
#endif
        return ordinary;
    }
    int x = (int) (view.roomX0 + ((float) in->px + 0.5f) * view.roomPerPx);
    int y = (int) (view.roomY0 + ((float) in->py + 0.5f) * view.roomPerPx);
    if (in->px < 0 || in->px >= 320 || in->py < 0 || in->py >= 240) x = y = -32768;
    *pulse = 0;
    *hold = Tq_step(&live->gesture, &menu, in->pen, x, y,
                    live->physical | in->held | in->pressed, reset);
#ifdef CTRPORT_TOUCH_TEST
    if (trace) logInfo(tqTrace, (unsigned long) r->frameCount, 1, (int) reset, (int) in->pen,
        (int) in->px, (int) in->py, x, y, (long) menu.x, (long) menu.y,
        before, (int) live->gesture.mode, (unsigned long) *hold, (unsigned) menu.count);
    if (targets) for (unsigned i = 0; i < menu.count; ++i) {
        TqAnswer* a = &menu.answer[i];
        logInfo(tqTargetTrace, (unsigned long) r->frameCount, i, (int) a->target.x0, (int) a->target.y0,
            (int) a->target.x1, (int) a->target.y1, (long) a->x, (long) a->y,
            (long) (view.roomX0 * 16), (long) (view.roomY0 * 16), (long) (view.roomPerPx * 16));
    }
#endif
    in->battle = false;
    return true;
}
#endif
