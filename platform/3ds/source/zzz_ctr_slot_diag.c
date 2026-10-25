#include "ctr_slot_diag.h"

#ifdef CTRPORT_SLOT_DIAGNOSTICS
#include <limits.h>
#include <string.h>
#include "ctr_memory.h"
#include "log.h"
#include "runner.h"

// One renderer is active at a time. Kept outside its struct so diagnostic
// fields cannot change offsets used by the draw paths.
static struct {
    Renderer* renderer;
    int32_t room;
    struct {
        uint32_t appendAttempts, appended, reuse, release, growAttempts, grown;
        uint32_t live, peak, sessionPeak;
    } slots[2];
} diag;

static void report(void) {
    if (diag.room == INT32_MIN) return;
    for (int k = 0; 2 > k; k++) {
        logInfo("[slots] room %ld %s: append attempts %lu, appended %lu, reuse %lu, release %lu, "
                "growth attempts %lu, grown %lu, live %lu, peak %lu, session peak %lu\n",
                (long) diag.room, k == CTR_SLOT_TPAG ? "TPAG" : "SPRT",
                (unsigned long) diag.slots[k].appendAttempts, (unsigned long) diag.slots[k].appended,
                (unsigned long) diag.slots[k].reuse, (unsigned long) diag.slots[k].release,
                (unsigned long) diag.slots[k].growAttempts, (unsigned long) diag.slots[k].grown,
                (unsigned long) diag.slots[k].live, (unsigned long) diag.slots[k].peak,
                (unsigned long) diag.slots[k].sessionPeak);
    }
    // Largest free block and pieces, under the allocator lock. The room being
    // left is identified above; this snapshot includes its cleanup so far.
    CtrMemory_logHeapLayout("slots room boundary", 0);
}

void CtrSlotDiag_begin(Renderer* renderer, size_t surfaceBytes) {
    memset(&diag, 0, sizeof(diag));
    diag.renderer = renderer;
    diag.room = INT32_MIN;
    logInfo("[slots] loaded TPAG %lu items (%lu B each), SPRT %lu items (%lu B each), surface %lu B\n",
            (unsigned long) renderer->dataWin->tpag.count, (unsigned long) sizeof(TexturePageItem),
            (unsigned long) renderer->dataWin->sprt.count, (unsigned long) sizeof(Sprite), (unsigned long) surfaceBytes);
}

void CtrSlotDiag_room(void) {
    if (diag.renderer == nullptr || diag.renderer->runner == nullptr) return;
    int32_t room = diag.renderer->runner->currentRoomIndex;
    if (diag.room == room) return;
    report();
    diag.room = room;
    for (int k = 0; 2 > k; k++) {
        diag.slots[k].appendAttempts = diag.slots[k].appended = 0;
        diag.slots[k].reuse = diag.slots[k].release = 0;
        diag.slots[k].growAttempts = diag.slots[k].grown = 0;
        diag.slots[k].peak = diag.slots[k].live;
    }
    logInfo("[slots] entering room %ld\n", (long) room);
}

void CtrSlotDiag_event(DataWin* dw, enum CtrSlotKind kind, enum CtrSlotEvent event) {
    if (diag.renderer == nullptr || diag.renderer->dataWin != dw) return;
    CtrSlotDiag_room();
    switch (event) {
        case CTR_SLOT_APPEND_ATTEMPT: diag.slots[kind].appendAttempts++; break;
        case CTR_SLOT_GROW_ATTEMPT:
            diag.slots[kind].growAttempts++;
            // Log before allocation as well, so a fatal allocation cannot
            // erase the last room's attempt from the evidence.
            logInfo("[slots] room %ld %s growth attempt at count %lu, live %lu, peak %lu, session peak %lu\n", (long) diag.room,
                    kind == CTR_SLOT_TPAG ? "TPAG" : "SPRT",
                    (unsigned long) (kind == CTR_SLOT_TPAG ? dw->tpag.count : dw->sprt.count),
                    (unsigned long) diag.slots[kind].live, (unsigned long) diag.slots[kind].peak,
                    (unsigned long) diag.slots[kind].sessionPeak);
            break;
        case CTR_SLOT_GROWN: diag.slots[kind].grown++; break;
        case CTR_SLOT_APPENDED: diag.slots[kind].appended++; break;
        case CTR_SLOT_REUSE: diag.slots[kind].reuse++; break;
        case CTR_SLOT_RELEASE:
            diag.slots[kind].release++;
            if (diag.slots[kind].live == 0) logError("[slots] unbalanced release in room %ld\n", (long) diag.room);
            else diag.slots[kind].live--;
            break;
    }
    if (event == CTR_SLOT_APPENDED || event == CTR_SLOT_REUSE) {
        diag.slots[kind].live++;
        if (diag.slots[kind].live > diag.slots[kind].peak) diag.slots[kind].peak = diag.slots[kind].live;
        if (diag.slots[kind].live > diag.slots[kind].sessionPeak) diag.slots[kind].sessionPeak = diag.slots[kind].live;
    }
}

void CtrSlotDiag_end(DataWin* dw) {
    if (diag.renderer == nullptr || diag.renderer->dataWin != dw) return;
    report();
    diag.renderer = nullptr;
}
#endif
