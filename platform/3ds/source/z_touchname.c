// Naming extends the sample chain with ordinary arrows/Z; no game writes.
#ifdef ENABLE_WAD16
#include <stdlib.h>
#include <string.h>
#define NN_BIND __attribute__((noinline, flatten, section(".touchname_text")))
#include "ctr_touchname_read.h"
#include "ctr_input.h"
#include "ctr_touch_ext.h"
#if defined(CTRPORT_TOUCH_TEST) && defined(CTRPORT_TOUCH_NAME_TRACE)
#include "log.h"
static const char nnTrace[] NN_RO =
    "[touchname] f=%lu named=%d shown=%d phase=%d charset=%d cursor=%d count=%d reset=%d pen=%d pulse=%lx\n";
#endif

extern bool To_sample(const CtrTouchExtIn*, uint32_t*, uint32_t*);
extern CtrInputEdges To_buttons(const CtrInputMap*, uint32_t, uint32_t, bool, bool, CtrInputStats*);

typedef struct {
    NnReader reader;
    NnState gesture;
    NnSnapshot layout;
    Runner* runner;
    Room* room;
    CtrBottomView view;
    uint32_t physical;
    bool shown;
} NnLive;
static NnLive* nnLive __attribute__((section(".bss.touchname_late")));
#ifdef __arm__
_Static_assert(sizeof(NnLive) == 252, "naming extension ARM heap price changed");
#endif

NN_BIND CtrInputEdges Nn_buttons(const CtrInputMap* map, uint32_t held, uint32_t down,
                                bool enabled, bool suppress, CtrInputStats* stats) {
    if (nnLive) {
        nnLive->physical = (held | down) & ~(1u << 20);
        if (nnLive->physical || suppress) Nn_reset(&nnLive->gesture, false);
    }
    return To_buttons(map, held, down, enabled, suppress, stats);
}

NN_BIND bool Nn_sample(const CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold) {
    Runner* runner = in->runner;
    NnLive* live = nnLive;
    NnMenu menu = {0}; NnSnapshot layout = {0}; CtrBottomView view = {0};
    bool eligible = !in->battle && runner && runner->dataWin;
    if (eligible && !live && !in->reset) {
        live = calloc(1, sizeof(*live));
        if (live) { nnLive = live; Nn_reset(&live->gesture, in->pen); }
    }
    bool shown = false, cut = in->reset;
    if (live) {
        cut |= live->runner != runner || (runner && live->room != runner->currentRoom)
            || (eligible && live->reader.data != runner->dataWin);
        shown = eligible && Nn_read(runner, &live->reader, &menu, &layout, &view);
        cut |= live->physical != 0 || shown != live->shown
            || memcmp(&layout, &live->layout, sizeof(layout)) != 0
            || view.roomX0 != live->view.roomX0 || view.roomY0 != live->view.roomY0
            || view.roomPerPx != live->view.roomPerPx;
        live->runner = runner; live->room = runner ? runner->currentRoom : NULL;
        live->layout = layout; live->view = view; live->shown = shown;
    }
    // Retained chain still runs once, clearing prior input/audio provenance.
    // A visible naming screen disarms all background tap handlers.
    CtrTouchExtIn ordinary = *in;
    ordinary.reset |= shown;
    bool claimed = To_sample(&ordinary, pulse, hold);
#if defined(CTRPORT_TOUCH_TEST) && defined(CTRPORT_TOUCH_NAME_TRACE)
    if (runner && !shown)
        logInfo(nnTrace, (unsigned long) runner->frameCount, live ? live->reader.named : 0,
            0, layout.phase, layout.charset, menu.cursor, menu.count, (int) cut, (int) in->pen, 0ul);
#endif
    if (!shown) {
        if (live) Nn_reset(&live->gesture, in->pen);
        if (in->reset) { *pulse = *hold = 0; return false; }
        return claimed;
    }
    int x = (int) (view.roomX0 + ((float) in->px + .5f) * view.roomPerPx);
    int y = (int) (view.roomY0 + ((float) in->py + .5f) * view.roomPerPx);
    if (in->px < 0 || in->px >= 320 || in->py < 0 || in->py >= 240) x = y = -32768;
    *hold = 0;
    *pulse = Nn_step(&live->gesture, &menu, in->pen, x, y, in->pressed, in->held, cut);
#if defined(CTRPORT_TOUCH_TEST) && defined(CTRPORT_TOUCH_NAME_TRACE)
    logInfo(nnTrace, (unsigned long) runner->frameCount, (int) live->reader.named,
        (int) shown, layout.phase, layout.charset, menu.cursor, menu.count, (int) cut,
        (int) in->pen, (unsigned long) *pulse);
#endif
    // The native naming Draw has no move cue. Confirmation remains native.
    return !in->reset;
}

// Shop continuation reads the existing physical observer; no new root or HID poll.
__attribute__((noinline, section(".text_touchshops")))
uint32_t CtrTouchName_physical(void) { return nnLive ? nnLive->physical : 0; }
#endif
