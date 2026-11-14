// Outermost touch reader. The prior extension pointer remains owned by its chain.
#ifdef ENABLE_WAD16
#include <stdlib.h>
#include "ctr_touchshops_read.h"
#include "ctr_touch_ext.h"
extern bool __real_Nn_sample(const CtrTouchExtIn*, uint32_t*, uint32_t*);
extern uint32_t CtrTouchName_physical(void);
extern uint32_t CtrTouchUi_step(TuState*, const TuMenu*, bool, int, int, uint32_t, uint32_t, bool);
typedef struct {
    void* inner;
    TsReader reader;
    TuState gesture;
    Runner* runner;
    Room* room;
    CtrBottomView view;
    bool shown;
} TsLive;
#ifdef __arm__
_Static_assert(sizeof(TsLive) <= 256, "shop extension exceeds the approved heap ceiling");
#endif
__attribute__((noinline, flatten, section(".text_touchshops")))
bool __wrap_Nn_sample(const CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold) {
    if (!in->state) return __real_Nn_sample(in, pulse, hold);
    TsLive* live = *in->state;
    if (!live) {
        live = calloc(1, sizeof(*live));
        if (!live) { *pulse = *hold = 0; return false; }
        *in->state = live;
        Tu_reset(&live->gesture, in->pen);
    }
    Runner* r = in->runner;
    TuMenu menu = {0}; CtrBottomView view = {0};
    uint32_t phase = live->reader.phase;
    const DataWin* data = live->reader.data;
    bool shown = !in->battle && Ts_read(r, &live->reader, &menu, &view);
    bool cut = in->reset || live->runner != r || (r && live->room != r->currentRoom)
        || data != live->reader.data || phase != live->reader.phase || shown != live->shown
        || memcmp(&view, &live->view, sizeof(view));
    live->runner = r; live->room = r ? r->currentRoom : NULL;
    live->shown = shown; live->view = view;
    CtrTouchExtIn ordinary = *in;
    ordinary.state = &live->inner;
    ordinary.reset |= shown;
    bool claimed = __real_Nn_sample(&ordinary, pulse, hold);
    cut |= CtrTouchName_physical() != 0;
    if (!shown) {
        Tu_reset(&live->gesture, in->pen);
        return claimed;
    }
    int x = (int) (view.roomX0 + ((float) in->px + .5f) * view.roomPerPx);
    int y = (int) (view.roomY0 + ((float) in->py + .5f) * view.roomPerPx);
    if (in->px < 0 || in->px >= 320 || in->py < 0 || in->py >= 240) x = y = -32768;
    // TU_TEXT acts on down; its reader supplies a key only inside the drawn panel.
    if (menu.ctx == TU_TEXT && !Touch_inside(menu.box, x, y, 0)) menu.textKey = 0;
    *hold = 0;
    *pulse = CtrTouchUi_step(&live->gesture, &menu, in->pen, x, y, in->pressed, in->held, cut);
    return !in->reset;
}
#endif
