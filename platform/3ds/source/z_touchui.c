// touchui step 1: bottom-screen overworld dialogue and its in-text choices.
// No HID, game-variable writes or RNG calls. battletouch owns the sample and merge.
// CtrTouchExt_sample is battletouch's shared sample/apply extension (84a818c6c).
#if defined(ENABLE_WAD16)
#include <stdlib.h>
#include <string.h>
#ifdef CTRPORT_TOUCHUI_HOST
#include "touchui_host.h"
#else
#include "ctr_bottom_view.h"
#include "ctr_dialogue.h"
#include "ctr_renderer.h"
#include "data_win.h"
#include "instance.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"
#ifdef CTRPORT_TOUCH_TEST
#include "log.h"
#endif
#endif
#define TOUCH_INLINE __attribute__((always_inline))
#define TU_INLINE __attribute__((always_inline))
// One shared step body is smaller than flattening it into the live reader callback.
#define TU_STEP static __attribute__((noinline, section(".touchui_text")))
#include "ctr_touch_ext.h"
#include "ctr_touchui_core.h"
#include "ctr_touch_mute.h"

// Names deliberately outside .text.* / .rodata.*: their inputs are appended after battletouch's late sections.
#define TU_CODE __attribute__((noinline, flatten, section(".touchui_text")))
#define TU_RO __attribute__((section(".touchui_ro")))
enum { U_SIDE, U_CURSOR, U_X0, U_X1, U_CHOSEN, U_WRITER, U_HALT, U_N };
enum { U_BOX, U_CHOICE, U_BATTLE, U_SHOP1, U_SHOP2, U_SHOP3, U_SHOP4, U_SHOP5, U_OBJECTS };
static const char tuNames[] TU_RO =
    "side\0mychoice\0x0\0x1\0choiced\0writer\0halt\0obj_dialoguer\0obj_choicer\0obj_battlecontroller\0"
    "obj_shop1\0obj_shop2\0obj_shop3\0obj_shop4\0obj_shop5\0";
static const char tuMoveScript[] TU_RO = "gml_Script_snd_play";
#ifdef CTRPORT_TOUCH_TEST
static const char tuTrace[] TU_RO =
    "[touchui] f=%lu named=%d shown=%d cut=%d reset=%d pen=%d panel=(%d,%d) room=(%d,%d) box=(%d,%d,%d,%d) mode=%d->%d pulse=%02lx\n";
#endif

typedef struct {
    const DataWin* data;
    const Runner* runner;
    const Room* room;
    int32_t variable[U_N], object[U_OBJECTS];
    TuState gesture;
    uint32_t box, writer, choice;
    bool named, shown;
    CtrBottomView view;
} TuLive;

extern bool CtrScreens_onBottom;
extern const uint32_t Popbot_layout[7];
bool __real_CtrDialogue_allowed(const CtrDialoguePolicy* policy, Runner* runner);

static TU_CODE void tuResolve(TuLive* live, DataWin* dw) {
    const char* name = tuNames;
    live->named = true;
    for (int k = 0; k < U_N + U_OBJECTS; ++k, name += strlen(name) + 1) {
        int32_t found = -1;
        if (k < U_N) {
            for (uint32_t i = 0; i < dw->vari.variableCount; ++i) {
                Variable* var = &dw->vari.variables[i];
                if (var->instanceType == INSTANCE_SELF && var->name != nullptr && strcmp(var->name, name) == 0) {
                    if (var->varID < 0 || (found >= 0 && found != var->varID)) {
                        live->named = false;
                        live->data = dw;
                        return;
                    }
                    found = var->varID;
                }
            }
            live->variable[k] = found;
        } else {
            for (uint32_t i = 0; i < dw->objt.count; ++i) {
                if (dw->objt.objects[i].name != nullptr && strcmp(dw->objt.objects[i].name, name) == 0) {
                    found = (int32_t) i;
                    break;
                }
            }
            live->object[k - U_N] = found;
        }
        if (found < 0 && k < U_N + U_SHOP1) live->named = false;
    }
    live->data = dw;
}

static TU_CODE Instance* tuFirst(Runner* r, int32_t object) {
    if (r->instancesByObject == nullptr || object < 0 || (uint32_t) object >= r->dataWin->objt.count) return nullptr;
    Instance** list = r->instancesByObject[object];
    for (ptrdiff_t i = 0; i < arrlen(list); ++i) {
        if (!list[i]->destroyed && list[i]->active) return list[i];
    }
    return nullptr;
}

static TU_CODE int32_t tuValue(Instance* inst, int32_t id, int32_t fallback) {
    RValue* v = IntRValueHashMap_findSlot(&inst->selfVars, id);
    if (v == nullptr) return fallback;
    if (v->type == RVALUE_REAL) return (int32_t) v->real;
    if (v->type == RVALUE_INT32 || v->type == RVALUE_BOOL) return v->int32;
#ifndef NO_RVALUE_INT64
    if (v->type == RVALUE_INT64) return (int32_t) v->int64;
#endif
    return fallback;
}

// Spatial choice mapping only. Text presses use the whole unclaimed bottom
// panel, including when the box stays on top. Flipped exposes no hidden items.
// The policy's real (unwrapped) read avoids popbot's drawing/anchor side effects.
static TU_CODE bool tuView(Runner* r, int side, CtrBottomView* view) {
    if (CtrScreens_onBottom && CtrTouchUi_textOther()) return false;
    if (CtrLayout_bottomView(r, view)) return true;
    if (CtrScreens_onBottom) return false;
    if (r->renderer == nullptr || !CtrRenderer_dialogueShown(r->renderer)) return false;
    CtrDisplaySettings settings = CtrRenderer_getDisplay(r->renderer);
    if (!settings.dialogueBottom) return false;
    const uint8_t* renderer = (const uint8_t*) r->renderer;
    const CtrDialoguePolicy* policy = (const CtrDialoguePolicy*) (renderer + Popbot_layout[0]);
    if (!__real_CtrDialogue_allowed(policy, r)) return false;
    float top;
    memcpy(&top, renderer + Popbot_layout[2], sizeof(top));
    GMLCamera* camera = r->viewsEnabled && r->views[0].enabled ? Runner_getCameraForView(r, 0) : nullptr;
    if (camera == nullptr || camera->viewWidth != 320 || camera->viewHeight != 240
        || r->views[0].portX != 0 || r->views[0].portY != 0
        || r->views[0].portWidth != 640 || r->views[0].portHeight != 480) return false;
    // Standard box crop only. dlgBeginFrame clears recorded quads before the input sample; the generalised
    // layout API must retain a full crop transform for nonstandard boxes/portraits extending beyond this edge.
    *view = (CtrBottomView) {camera->viewX, camera->viewY + (side == 0 ? 5.0f : 160.0f) - top, 1.0f};
    return true;
}

static TU_CODE bool tuRead(Runner* r, TuLive* live, TuMenu* m, CtrBottomView* view,
                           uint32_t* boxId, uint32_t* writerId, uint32_t* choiceId) {
    if (tuFirst(r, live->object[U_BATTLE]) != nullptr) return false;
    for (int k = U_SHOP1; k < U_OBJECTS; ++k)
        if (tuFirst(r, live->object[k]) != nullptr) return false;
    Instance* box = tuFirst(r, live->object[U_BOX]);
    if (box == nullptr) return false;
    int side = tuValue(box, live->variable[U_SIDE], -1);
    if (side != 0 && side != 1) return false;
    GMLCamera* camera = r->viewsEnabled && r->views[0].enabled ? Runner_getCameraForView(r, 0) : nullptr;
    if (camera == nullptr || camera->viewWidth != 320 || camera->viewHeight != 240) return false;
    *view = (CtrBottomView) {camera->viewX, camera->viewY, 1};
    int x = (int) camera->viewX, y = (int) camera->viewY + (side == 0 ? 5 : 160);
    m->box = (TouchRect) {(int16_t) (x + 16), (int16_t) y, (int16_t) (x + 305), (int16_t) (y + 76)};
    *boxId = box->instanceId;
    *writerId = (uint32_t) tuValue(box, live->variable[U_WRITER], 0);
    int32_t writerKey = (int32_t) *writerId;
    Instance* writer = RUNNER_INSTANCE_BY_ID(r, writerKey);
    if (writer == nullptr || writer->destroyed || !writer->active) return false;
    // The native dialoguer gates X on halt == 0; unskippable text ignores that key itself.
    int halt = tuValue(writer, live->variable[U_HALT], -1);
    if (halt < 0) return false;
    m->textKey = halt == 0 ? TOUCH_KEY_X : TOUCH_KEY_Z;
    Instance* choice = tuFirst(r, live->object[U_CHOICE]);
    *choiceId = choice != nullptr ? choice->instanceId : 0;
    m->id = *choiceId != 0 ? *choiceId : *boxId;
    if (choice == nullptr) { m->ctx = TU_TEXT; return true; }
    // A live choice keeps ownership even when it is on the top screen:
    // no bottom target and no fallback text key may commit it.
    m->ctx = TU_CHOICE;
    if (!tuView(r, side, view)) return true;
    int cursor = tuValue(choice, live->variable[U_CURSOR], -1);
    int left = tuValue(choice, live->variable[U_X0], -1), right = tuValue(choice, live->variable[U_X1], -1);
    if (cursor < 0 || cursor > 1 || right <= left || tuValue(choice, live->variable[U_CHOSEN], 1) != 0) return false;
    m->ctx = TU_CHOICE;
    m->count = 2;
    m->cursor = (int8_t) cursor;
    int middle = (left + right) / 2 + 8, row = (int) choice->y - 8;
    m->item[0] = (TouchRect) {m->box.x0, (int16_t) row, (int16_t) middle, (int16_t) (row + 32)};
    m->item[1] = (TouchRect) {(int16_t) middle, (int16_t) row, m->box.x1, (int16_t) (row + 32)};
    // Both game's KeyPress handlers toggle the two choices. No synthetic cursor or sound write.
    for (int k = 0; k < 2; ++k) {
        m->next[k][0] = m->next[k][1] = -1;
        m->next[k][2] = m->next[k][3] = (int8_t) (1 - k);
    }
    return true;
}

TU_CODE bool CtrTouchExt_sample(const CtrTouchExtIn* sample, uint32_t* pulse, uint32_t* hold) {
    CtrTouchMute_clear();
    Runner* r = sample->runner;
    void** state = sample->state;
    *pulse = *hold = 0;
    if (r == nullptr || r->dataWin == nullptr || state == nullptr) return false;
    TuLive* live = *state;
    if (sample->battle) {
        if (live != nullptr) Tu_reset(&live->gesture, sample->pen);
        return false;
    }
    if (live == nullptr) {
        live = calloc(1, sizeof(*live));
        if (live == nullptr) return false;
        *state = live;
        // A pen held before the extension was ready must be lifted first.
        Tu_reset(&live->gesture, sample->pen);
    }
    bool cut = sample->reset || live->data != r->dataWin || live->runner != r || live->room != r->currentRoom;
    if (live->data != r->dataWin) tuResolve(live, r->dataWin);
    live->runner = r;
    live->room = r->currentRoom;
    TuMenu m = {0};
    CtrBottomView view = {0};
    uint32_t box = 0, writer = 0, choice = 0;
    bool shown = live->named && tuRead(r, live, &m, &view, &box, &writer, &choice);
    if (shown != live->shown || box != live->box || writer != live->writer || choice != live->choice
        || view.roomX0 != live->view.roomX0 || view.roomY0 != live->view.roomY0 || view.roomPerPx != live->view.roomPerPx) cut = true;
    live->shown = shown;
    live->box = box;
    live->writer = writer;
    live->choice = choice;
    live->view = view;
    int x = (int) (view.roomX0 + ((float) sample->px + 0.5f) * view.roomPerPx);
    int y = (int) (view.roomY0 + ((float) sample->py + 0.5f) * view.roomPerPx);
#ifdef CTRPORT_TOUCH_TEST
    bool trace = sample->pen || live->gesture.pen;
    int before = live->gesture.mode;
#endif
    if (sample->px < 0 || sample->px >= 320 || sample->py < 0 || sample->py >= 240) m.textKey = 0;
    *pulse = Tu_step(&live->gesture, &m, sample->pen, x, y, sample->pressed, sample->held, cut);
    if (!cut && shown && m.ctx == TU_CHOICE && sample->pressed == 0 && sample->held == 0
        && (*pulse == TOUCH_KEY_LEFT || *pulse == TOUCH_KEY_RIGHT))
        CtrTouchMute_mark(r, choice, live->object[U_CHOICE], *pulse == TOUCH_KEY_LEFT ? 37 : 39, tuMoveScript);
#ifdef CTRPORT_TOUCH_TEST
    if (trace) logInfo(tuTrace, (unsigned long) r->frameCount, (int) live->named, (int) shown, (int) cut,
                       (int) sample->reset, (int) sample->pen, (int) sample->px, (int) sample->py, x, y,
                       (int) m.box.x0, (int) m.box.y0, (int) m.box.x1, (int) m.box.y1,
                       before, (int) live->gesture.mode, (unsigned long) *pulse);
#endif
    return shown && !sample->reset;
}
#endif

// Export the retained scheduler without telling GCC it has another C caller:
// a C alias reorders the old functions in this section. This assembler alias
// adds no allocated bytes and preserves the original calls and relocations.
#if defined(ENABLE_WAD16) && defined(__3DS__)
__asm__(".global CtrTouchUi_step\n.set CtrTouchUi_step, Tu_step\n");
#endif
