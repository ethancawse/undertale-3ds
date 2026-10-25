#ifdef PLATFORM_3DS
#include "data_win_slots_3ds.h"

#include <stdint.h>
#include <string.h>
#include "ctr_slot_diag.h"
#include "utils.h"

// max(1024, 4 * measured per-game peak). DR: 36 slots; UT lab: 1 slot.
// Only the dynamic tail doubles, never the much larger original item count.
#define TPAG_RESERVE 1024u
#ifdef ENABLE_WAD16
#define SPRITE_RESERVE (4u * 1u)
#else
#define SPRITE_RESERVE (4u * 36u)
#endif

static const char tpaggrowMessage0[] TPAGGROW_SLOTS_RODATA = "Dynamic sprite metadata capacity exceeds the addressable allocation size";
static const char tpaggrowMessage1[] TPAGGROW_SLOTS_RODATA = "Dynamic sprite metadata capacity is below its original count";
static const char tpaggrowMessage2[] TPAGGROW_SLOTS_RODATA = "Original sprite metadata exceeds the addressable allocation size";
static const char tpaggrowMessage3[] TPAGGROW_SLOTS_RODATA = "Dynamic sprite metadata exhausted the addressable allocation size";
#ifdef CTRPORT_SLOT_DIAGNOSTICS
static const char tpaggrowMessage4[] TPAGGROW_SLOTS_RODATA = "[slots] SPRT reserve overflow: capacity %lu -> %lu items, base %lu, request %lu B\n";
#endif
#ifdef CTRPORT_SLOT_DIAGNOSTICS
static const char tpaggrowMessage5[] TPAGGROW_SLOTS_RODATA = "[slots] TPAG reserve overflow: capacity %lu -> %lu items, base %lu, request %lu B\n";
#endif
static const char tpaggrowMessage6[] TPAGGROW_SLOTS_RODATA = "Parsed TPAG index exceeds signed index range";
static const char tpaggrowFile[] TPAGGROW_SLOTS_RODATA = "./engine/src/data_win_slots_3ds.c";
static const char tpaggrowRequirement[] TPAGGROW_SLOTS_RODATA = "Requirement failed at %s:%d: %s\n";
static const char tpaggrowAllocation[] TPAGGROW_SLOTS_RODATA = "FATAL: realloc(%zu) failed at %s:%d\n";
#undef requireMessage
#define requireMessage(condition, message) do { if (!(condition)) { logError(tpaggrowRequirement, tpaggrowFile, __LINE__, message); abort(); } } while (0)
#undef safeRealloc
#define safeRealloc(ptr, bytes) tpaggrowRealloc(ptr, bytes, __LINE__)
static TPAGGROW_SLOTS_COLD void* tpaggrowRealloc(void* ptr, size_t bytes, int line) {
    if (bytes == 0) { free(ptr); return NULL; }
    void* result = realloc(ptr, bytes);
    if (result == NULL) { logError(tpaggrowAllocation, bytes, tpaggrowFile, line); abort(); }
    return result;
}

static TPAGGROW_SLOTS_COLD uint32_t checkedCapacity(uint32_t base, uint32_t tail, size_t itemBytes) {
    size_t limit = SIZE_MAX / itemBytes;
    requireMessage(limit >= base && limit - base >= tail && UINT32_MAX - base >= tail,
                   tpaggrowMessage0);
    return base + tail;
}

static TPAGGROW_SLOTS_COLD uint32_t grownCapacity(uint32_t capacity, uint32_t base, uint32_t initial, size_t itemBytes) {
    requireMessage(capacity >= base, tpaggrowMessage1);
    uint32_t tail = capacity - base;
    if (tail == 0) return checkedCapacity(base, initial, itemBytes);
    size_t allocationLimit = SIZE_MAX / itemBytes;
    uint32_t limit = allocationLimit > UINT32_MAX ? UINT32_MAX : (uint32_t) allocationLimit;
    requireMessage(base <= limit, tpaggrowMessage2);
    uint32_t maximum = limit - base;
    requireMessage(tail < maximum, tpaggrowMessage3);
    uint32_t next = tail > maximum / 2 ? maximum : tail * 2;
    return checkedCapacity(base, next, itemBytes);
}

uint32_t DataWin_3dsTpagCapacity(uint32_t base) {
    return checkedCapacity(base, TPAG_RESERVE, sizeof(TexturePageItem));
}

uint32_t DataWin_3dsSpriteCapacity(uint32_t base) {
    return checkedCapacity(base, SPRITE_RESERVE, sizeof(Sprite));
}

void DataWin_3dsGrowSpriteSlots(DataWin* dw, uint32_t firstDynamic) {
    uint32_t capacity = grownCapacity(dw->sprt.capacity, firstDynamic, SPRITE_RESERVE, sizeof(Sprite));
#ifdef CTRPORT_SLOT_DIAGNOSTICS
    logWarn(tpaggrowMessage4,
            (unsigned long) dw->sprt.capacity, (unsigned long) capacity, (unsigned long) firstDynamic,
            (unsigned long) (capacity * sizeof(Sprite)));
#endif
    dw->sprt.sprites = safeRealloc(dw->sprt.sprites, capacity * sizeof(Sprite));
    dw->sprt.capacity = capacity;
}

uint32_t DataWin_3dsAllocTpagSlot(DataWin* dw, uint32_t firstDynamic) {
    for (uint32_t i = firstDynamic; dw->tpag.count > i; i++) {
        if (dw->tpag.items[i].texturePageId == -1) {
            CtrSlotDiag_event(dw, CTR_SLOT_TPAG, CTR_SLOT_REUSE);
            return i;
        }
    }
    CtrSlotDiag_event(dw, CTR_SLOT_TPAG, CTR_SLOT_APPEND_ATTEMPT);
    if (dw->tpag.count >= dw->tpag.capacity) {
        CtrSlotDiag_event(dw, CTR_SLOT_TPAG, CTR_SLOT_GROW_ATTEMPT);
        uint32_t capacity = grownCapacity(dw->tpag.capacity, firstDynamic, TPAG_RESERVE, sizeof(TexturePageItem));
#ifdef CTRPORT_SLOT_DIAGNOSTICS
        logWarn(tpaggrowMessage5,
                (unsigned long) dw->tpag.capacity, (unsigned long) capacity, (unsigned long) firstDynamic,
                (unsigned long) (capacity * sizeof(TexturePageItem)));
#endif
        dw->tpag.items = safeRealloc(dw->tpag.items, capacity * sizeof(TexturePageItem));
        dw->tpag.capacity = capacity;
        CtrSlotDiag_event(dw, CTR_SLOT_TPAG, CTR_SLOT_GROWN);
    }
    uint32_t index = dw->tpag.count++;
    memset(&dw->tpag.items[index], 0, sizeof(TexturePageItem));
    dw->tpag.items[index].texturePageId = -1;
    CtrSlotDiag_event(dw, CTR_SLOT_TPAG, CTR_SLOT_APPENDED);
    return index;
}

// WinPack can add off-table entries while resolving the file's references.
// Track their real capacity too, and leave the full tail after parsing them.
int32_t DataWin_3dsAppendParsedTpag(DataWin* dw) {
    if (dw->tpag.count >= dw->tpag.capacity) {
        uint32_t capacity = DataWin_3dsTpagCapacity(dw->tpag.count);
        dw->tpag.items = safeRealloc(dw->tpag.items, capacity * sizeof(TexturePageItem));
        dw->tpag.capacity = capacity;
    }
    requireMessage(dw->tpag.count < INT32_MAX, tpaggrowMessage6);
    uint32_t index = dw->tpag.count++;
    memset(&dw->tpag.items[index], 0, sizeof(TexturePageItem));
    return (int32_t) index;
}

void DataWin_3dsReserveParsedTpag(DataWin* dw) {
    uint32_t capacity = DataWin_3dsTpagCapacity(dw->tpag.count);
    if (capacity <= dw->tpag.capacity) return;
    dw->tpag.items = safeRealloc(dw->tpag.items, capacity * sizeof(TexturePageItem));
    dw->tpag.capacity = capacity;
}
#endif
