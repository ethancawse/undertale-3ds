// Overworld menus extend the shared sample owner; all added code stays in AAC.
#ifdef ENABLE_WAD16
#include <stdlib.h>
#define RValue_toInt32Other Colflip14_toInt32Other
#define RValue_toRealOther Colflip14_toRealOther
#define RVALUE_OTHER_EXTERN
#define TO_CODE __attribute__((noinline, flatten, section(".aacout.text.touchoverworld")))
#define TO_READ_FN static TO_CODE
#define TO_READ_RO __attribute__((section(".aacout.rodata.touchoverworld")))
#include "ctr_touchoverworld_read.h"
#include "ctr_input.h"
#include "ctr_touch_ext.h"
#include "audio_system.h"
#ifdef CTRPORT_TOUCH_TEST
#include "log.h"
#endif

extern uint32_t CtrTouchUi_step(TuState*, const TuMenu*, bool, int, int, uint32_t, uint32_t, bool);
extern bool __wrap_CtrTouchExt_sample(const CtrTouchExtIn*, uint32_t*, uint32_t*);
extern CtrInputEdges CtrTouchQuiz_buttons(const CtrInputMap*, uint32_t, uint32_t, bool, bool, CtrInputStats*);
extern void __real_VM_registerBuiltin(VMContext*, const char*, BuiltinFunc);
typedef struct {
    ToReader reader;
    TuState gesture;
    Runner* runner;
    Room* room;
    CtrBottomView view;
    BuiltinFunc original;
    uint32_t instance;
    uint32_t physical;
    int32_t frame, key;
    bool shown;
} ToLive;
#ifdef __arm__
_Static_assert(sizeof(ToLive) == 112, "overworld extension ARM heap price changed");
#endif
static ToLive* toLive __attribute__((section(".bss.touchoverworld_late")));
static const char toPlayName[] TO_READ_RO = "audio_play_sound";
static const char toMoveName[] TO_READ_RO = "gml_Script_snd_play";
#ifdef CTRPORT_TOUCH_TEST
static const char toTrace[] TO_READ_RO =
    "[touchow] f=%lu obj=%ld inst=%lu event=%ld:%ld sound=%ld voice=%ld mute=%d\n";
#endif

static TO_CODE ToLive* To_live(void) {
    if (!toLive) toLive = calloc(1, sizeof(*toLive));
    return toLive;
}

// Observe only the owner's supplied arguments, including Off and physical
// input with no game-key mapping. This neither reads HID nor changes merging.
TO_CODE CtrInputEdges To_buttons(const CtrInputMap* map, uint32_t held, uint32_t down,
                                bool enabled, bool suppress, CtrInputStats* stats) {
    if (toLive) {
        toLive->key = 0;
        toLive->physical = (held | down) & ~(1u << 20);
        // Scripted input uses enabled=false; its reset decision arrives in
        // the shared sample. Only real takeover/suppression cancels here.
        if (toLive->physical || suppress) Tu_reset(&toLive->gesture, false);
    }
    return CtrTouchQuiz_buttons(map, held, down, enabled, suppress, stats);
}

TO_CODE bool To_sample(const CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold) {
    Runner* runner = in->runner;
    ToLive* live = toLive;
    TuMenu menu = {0}; CtrBottomView view = {0};
    bool eligible = !in->battle && runner && runner->dataWin;
    if (eligible && !live && !in->reset) live = To_live();
    bool shown = false, cut = in->reset;
    int previous = -1;
    if (live) {
        live->key = 0;
        previous = live->reader.page;
        cut |= live->runner != runner || (runner && live->room != runner->currentRoom)
            || (eligible && live->reader.data != runner->dataWin);
        shown = eligible && To_read(runner, &live->reader, &menu, &view);
        cut |= live->physical != 0 || shown != live->shown || previous != live->reader.page || view.roomX0 != live->view.roomX0
            || view.roomY0 != live->view.roomY0 || view.roomPerPx != live->view.roomPerPx;
        live->runner = runner; live->room = runner ? runner->currentRoom : NULL;
        live->shown = shown; live->view = view;
    }
    CtrTouchExtIn ordinary = *in;
    ordinary.reset |= shown;
    bool claimed = __wrap_CtrTouchExt_sample(&ordinary, pulse, hold);
    if (!shown) {
        if (live) Tu_reset(&live->gesture, in->pen);
        if (in->reset) { *pulse = *hold = 0; return false; }
        return claimed;
    }
    int x = (int) (view.roomX0 + ((float) in->px + .5f) * view.roomPerPx);
    int y = (int) (view.roomY0 + ((float) in->py + .5f) * view.roomPerPx);
    if (in->px < 0 || in->px >= 320 || in->py < 0 || in->py >= 240) x = y = -32768;
    *hold = 0;
    *pulse = CtrTouchUi_step(&live->gesture, &menu, in->pen, x, y, in->pressed, in->held, cut);
    if (!cut && (*pulse & TOUCH_KEY_ARROWS) && !(in->pressed | in->held)) {
        // Runner_step increments the counter before the ensuing Draw pass.
        live->instance = menu.id; live->frame = (int32_t) ((uint32_t) runner->frameCount + 1u);
        live->key = *pulse == TOUCH_KEY_UP ? VK_UP : *pulse == TOUCH_KEY_DOWN ? VK_DOWN
            : *pulse == TOUCH_KEY_LEFT ? VK_LEFT : VK_RIGHT;
    }
    return !in->reset;
}

static TO_CODE RValue To_play(VMContext* ctx, RValue* args, int32_t count) {
    ToLive* live = toLive;
    Runner* runner = ctx->runner;
    bool mute = live->key && runner == live->runner && runner->frameCount == live->frame
        && runner->dataWin == live->reader.data && ctx->currentInstance
        && ctx->currentInstance->instanceId == live->instance
        && ctx->currentInstance->objectIndex == live->reader.object[TO_OWNER]
        && ctx->currentEventObjectIndex == live->reader.object[TO_OWNER]
        && ctx->currentEventType == EVENT_DRAW && ctx->currentEventSubtype == 0
        && runner->keyboard && runner->keyboard->keyPressed[live->key]
        && count > 0 && args[0].type != RVALUE_UNDEFINED && RValue_toInt32(args[0]) == 115
        && ctx->currentCodeName && !strcmp(ctx->currentCodeName, toMoveName);
    if (mute) live->key = 0;
    RValue voice = live->original(ctx, args, count);
    if (mute && runner->audioSystem) {
        int32_t id = RValue_toInt32(voice);
        if (id >= 0) runner->audioSystem->vtable->setSoundGain(runner->audioSystem, id, 0.0f, 0);
    }
#ifdef CTRPORT_TOUCH_TEST
    logInfo(toTrace, (unsigned long) runner->frameCount, (long) ctx->currentEventObjectIndex,
        (unsigned long) (ctx->currentInstance ? ctx->currentInstance->instanceId : 0),
        (long) ctx->currentEventType, (long) ctx->currentEventSubtype,
        (long) (count > 0 && args[0].type != RVALUE_UNDEFINED ? RValue_toInt32(args[0]) : -1),
        (long) RValue_toInt32(voice), (int) mute);
#endif
    return voice;
}

// The supplied function is the already registered choice-mute continuation.
// It and its original play execute unchanged, with unchanged args and result.
TO_CODE void To_register(VMContext* ctx, const char* name, BuiltinFunc func) {
    if (!strcmp(name, toPlayName)) {
        ToLive* live = To_live();
        if (live) { live->original = func; live->key = 0; func = To_play; }
    }
    __real_VM_registerBuiltin(ctx, name, func);
}
#endif
