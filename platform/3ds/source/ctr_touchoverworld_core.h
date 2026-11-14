// Cursor targets for the native overworld menu. This constructs input graphs;
// the game retains every cursor write and every confirmation branch.
#ifndef CTR_TOUCHOVERWORLD_CORE_H
#define CTR_TOUCHOVERWORLD_CORE_H
#include "ctr_touchui_core.h"

enum { TO_ROOT = 0, TO_ITEM = 1, TO_STAT = 2, TO_PHONE = 3,
       TO_ACTION = 5, TO_STORE = 6, TO_TAKE = 7 };

// occupied contains nonzero native entries; drawn contains actual labels.
// They differ for the phone handler's eighth, navigable but undrawn slot.
static inline bool To_graph(TuMenu* menu, uint32_t owner, int page, int cursor,
                            unsigned occupied, unsigned drawn, int x, int y) {
    int count = page == TO_ROOT || page == TO_ACTION ? 3 : page == TO_STAT ? 1
        : page == TO_TAKE ? 10 : page == TO_ITEM || page == TO_PHONE || page == TO_STORE ? 8 : 0;
    if (!owner || !count || cursor < 0 || cursor >= count || occupied >> count
        || drawn & ~occupied || x < -32768 || x > 32501 || y < -32768 || y > 32571) return false;
    *menu = (TuMenu) {.id = owner, .ctx = TU_CHOICE, .count = (uint8_t) count,
                      .cursor = (int8_t) cursor};
    for (int i = 0; i < count; ++i) {
        int left = 98, right = 264, top = 36 + 16 * i, height = 16;
        if (page == TO_ROOT) { left = 20; right = 84; top = 90 + 18 * i; height = 18; }
        if (page == TO_ACTION) {
            left = i == 0 ? 98 : i == 1 ? 146 : 203;
            right = i == 0 ? 146 : i == 1 ? 203 : 264;
            top = 174; height = 20;
        }
        // STAT has no native Z action. It still owns the visible menu so a
        // caller can disarm any dialogue underneath, without inventing a key.
        if (page != TO_STAT && (drawn & (1u << i)) && (page != TO_PHONE || i < 7))
            menu->item[i] = (TouchRect) {(int16_t) (x + left), (int16_t) (y + top),
                                        (int16_t) (x + right), (int16_t) (y + top + height)};
        for (int d = 0; d < 4; ++d) menu->next[i][d] = -1;
        if (page == TO_STAT) continue;
        int axis = page == TO_ACTION ? 2 : 0;
        menu->next[i][axis] = (int8_t) (i ? i - 1 : -1);
        // Native DOWN stops before an empty next slot; it never skips holes.
        // LEFT/RIGHT in Use/Info/Drop are bounded and independent of inventory.
        menu->next[i][axis + 1] = (int8_t) (i + 1 < count
            && (page == TO_ACTION || (occupied & (1u << (i + 1)))) ? i + 1 : -1);
    }
    return true;
}
#endif
