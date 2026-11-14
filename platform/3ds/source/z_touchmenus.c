// Popup menus extend the shared sample hook; battletouch remains the pen owner.
#ifdef ENABLE_WAD16
#include <stdlib.h>
#define TM_READ_FN static __attribute__((noinline, flatten, section(".aacout.text.touchmenus")))
#define TM_READ_RO __attribute__((section(".aacout.rodata.touchmenus")))
#include "ctr_touchmenus_read.h"
#include "ctr_touch_ext.h"

#define TM_CODE __attribute__((noinline, flatten, section(".aacout.text.touchmenus")))

// The assembler alias in z_touchui.c names its existing scheduler body.
extern uint32_t CtrTouchUi_step(TuState*, const TuMenu*, bool, int, int, uint32_t, uint32_t, bool);
extern bool __real_CtrTouchExt_sample(const CtrTouchExtIn*, uint32_t*, uint32_t*);

typedef struct {
    TmReader reader;
    TuState gesture;
    Runner* runner;
    Room* room;
    CtrBottomView view;
    bool shown;
} TmLive;
static TmLive* tmLive __attribute__((section(".bss.touchmenus_late")));

TM_CODE bool __wrap_CtrTouchExt_sample(const CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold) {
    Runner* runner = in->runner;
    TmLive* live = tmLive;
    TuMenu menu = {0};
    CtrBottomView view = {0};
    bool eligible = !in->battle && runner != NULL && runner->dataWin != NULL;
    if (eligible && live == NULL && !in->reset) {
        live = calloc(1, sizeof(*live));
        if (live != NULL) { tmLive = live; Tu_reset(&live->gesture, in->pen); }
    }
    bool cut = in->reset;
    bool shown = false;
    if (live != NULL) {
        cut |= live->runner != runner || (runner != NULL && live->room != runner->currentRoom)
            || (eligible && live->reader.data != runner->dataWin);
        shown = eligible && Tm_read(runner, &live->reader, &menu, &view);
        cut |= shown != live->shown || view.roomX0 != live->view.roomX0
            || view.roomY0 != live->view.roomY0 || view.roomPerPx != live->view.roomPerPx;
        live->runner = runner;
        live->room = runner != NULL ? runner->currentRoom : NULL;
        live->shown = shown;
        live->view = view;
    }
    // Always call the original once: it clears audio provenance and owns its
    // heap extension. A visible menu disarms background dialogue input.
    CtrTouchExtIn ordinary = *in;
    ordinary.reset |= shown;
    bool claimed = __real_CtrTouchExt_sample(&ordinary, pulse, hold);
    if (!shown) {
        if (live != NULL) Tu_reset(&live->gesture, in->pen);
        if (in->reset) { *pulse = *hold = 0; return false; }
        return claimed;
    }
    int x = (int) (view.roomX0 + ((float) in->px + 0.5f) * view.roomPerPx);
    int y = (int) (view.roomY0 + ((float) in->py + 0.5f) * view.roomPerPx);
    if (in->px < 0 || in->px >= 320 || in->py < 0 || in->py >= 240) x = y = -32768;
    *hold = 0;
    *pulse = CtrTouchUi_step(&live->gesture, &menu, in->pen, x, y, in->pressed, in->held, cut);
    // These four native handlers have no move-sound call. Their confirmation
    // sounds and all cursor/history writes run through ordinary game input.
    return !in->reset;
}
#endif

// AAC continuations preserve the helpers' original local ELF identities.
// Only these three new branch words occupy the newly appended input.
#ifdef ENABLE_WAD16
#ifdef __3DS__
__asm__(".pushsection .aacout.text.touchoverworld,\"ax\",%progbits\n"
        ".balign 4\n"
        ".global To_only\n.type To_only,%function\nTo_only:\nb Tm_only\n.size To_only,.-To_only\n"
        ".global To_exists\n.type To_exists,%function\nTo_exists:\nb Tm_exists\n.size To_exists,.-To_exists\n"
        ".global To_view\n.type To_view,%function\nTo_view:\nb Tm_view\n.size To_view,.-To_view\n"
        ".popsection\n");
#endif
#endif
