// Layout commit D ABI (layout design, section 7b).
// Keep the owner calls here; do not inspect or duplicate layout's target state.
#ifdef ENABLE_WAD16
C3D_RenderTarget* CtrLayout_lendTop(Renderer*, int holder, bool light);
void CtrLayout_returnTop(Renderer*, int holder);
int CtrLayout_topHolder(void);
bool CtrLayout_canMoveUp(int32_t group); // T1's admission test; S4 never chooses a battle group.
enum { DLGTOP_FREE = 0, DLGTOP_FLIGHT = 1, DLGTOP_DIALOGUE = 2 };

#define DLGTOP_COLD __attribute__((section(".text.late.dlgtop4"), noinline, noclone, used, optimize("Os")))
#define DLGTOP_CONST __attribute__((section(".aacout.rodata.dlgtop4"), used))
#include "ctr_dlgtop_buffers.inc"
static const char dlgtopBattleName[] DLGTOP_CONST = "room_battle";
#ifdef CTRPORT_DLGFLASH_LOG
static const char dlgtopBusyLog[] DLGTOP_CONST = "dlgtop: top target busy; dialogue stays with the game\n";
#endif

// A loan descriptor, not an owned target. Keeping it outside CtrRenderer avoids
// growing/moving its frozen fields. No target, texture, frame copy or heap is
// allocated. The owner identity prevents a later renderer using an old loan.
static struct {
    CtrRenderer* owner;
    C3D_RenderTarget* target;
    const Room* room;
    bool battle, drawing, denied;
} dlgtopLease __attribute__((section(".bss.dlgtop4")));

DLGTOP_CODE static int dlgtopHolder(void) { return CtrLayout_topHolder(); }

DLGTOP_COLD static void dlgtopReturn(void) {
    if (dlgtopLease.target != nullptr && dlgtopHolder() == DLGTOP_DIALOGUE) {
        CtrLayout_returnTop((Renderer*) dlgtopLease.owner, DLGTOP_DIALOGUE);
        dlgtopRetireBuffers();
    }
    dlgtopLease.target = nullptr;
}

DLGTOP_CODE static bool dlgtopWants(const CtrRenderer* r) {
    return !dlgtopLease.battle && CtrScreens_gameBelow() && CtrLayout_otherText() &&
           CtrLayout_targetsHeld() && !r->bottomYielded;
}

// Logical existence, including a box created before its first replay. Never
// invokes popup-anchor collection or changes a game variable/reference count.
DLGTOP_CODE static bool dlgtopBoxOpen(const CtrRenderer* r) {
    Runner* runner = r->base.runner;
    if (runner == nullptr || runner->instancesByObject == nullptr) return false;
    for (int32_t i = 0; i < r->dlg.policy.gateCount; ++i) {
        Instance** list = runner->instancesByObject[r->dlg.policy.gateObjects[i]];
        for (ptrdiff_t j = 0; j < arrlen(list); ++j)
            if (!list[j]->destroyed) return true;
    }
    return false;
}

DLGTOP_CODE bool CtrDlgtop_topBusy(Renderer* renderer) {
    if (dlgtopHolder() == DLGTOP_DIALOGUE) return true;
    CtrRenderer* r = (CtrRenderer*) renderer;
    return r != nullptr && dlgtopWants(r) && dlgtopBoxOpen(r);
}

// Only a room change needs the name comparison; keep it out of the steady
// frame path and ordinary late code out of the recorder/replay hot path.
DLGTOP_COLD static void dlgtopRoom(CtrRenderer* r, const Room* room) {
    dlgtopLease.room = room;
    bool battle = room != nullptr && room->name != nullptr && !strcmp(room->name, dlgtopBattleName);
    dlgtopLease.battle = battle && (dlgtopLease.battle ||
                                  (CtrScreens_gameBelow() && CtrLayout_otherText()));
    (void)r;
}

// Called before battlesplit can move the game or admit T1, and before Step.
// A closed box releases here even if the preceding frame had no presentation.
DLGTOP_CODE static void dlgtopPrepare(CtrRenderer* r) {
    if (dlgtopLease.owner != r) {
        dlgtopReturn();
        dlgtopLease.owner = r;
        dlgtopLease.room = nullptr;
        dlgtopLease.battle = dlgtopLease.denied = false;
    }
    if (dlgtopHolder() != DLGTOP_DIALOGUE) dlgtopLease.target = nullptr;
    Runner* runner = r->base.runner;
    const Room* room = runner != nullptr ? runner->currentRoom : nullptr;
    if (dlgtopLease.room != room) {
        // A loan belongs to this room's box, even if destruction of the old
        // instance list is deferred until after the next room's first Step.
        dlgtopReturn();
        dlgtopRoom(r, room);
    }
    dlgtopLease.drawing = false;
    if (dlgtopLease.target != nullptr && (!dlgtopWants(r) || !dlgtopBoxOpen(r))) dlgtopReturn();
}

DLGTOP_CODE static void dlgtopTake(CtrRenderer* r) {
    if (dlgtopLease.owner != r || !dlgtopWants(r) || dlgtopLease.target != nullptr ||
        dlgtopLease.drawing || !dlgtopBoxOpen(r)) return;
    if (dlgtopHolder() == DLGTOP_FREE)
        dlgtopLease.target = CtrLayout_lendTop((Renderer*) r, DLGTOP_DIALOGUE, true);
#ifdef CTRPORT_DLGFLASH_LOG
    if (dlgtopLease.target == nullptr && !dlgtopLease.denied) logInfo(dlgtopBusyLog);
#endif
    dlgtopLease.denied = dlgtopLease.target == nullptr;
}

// An invalidated loan is never dereferenced. A move/owner change belongs to
// layout; it can revoke the descriptor before this renderer is called again.
DLGTOP_CODE static C3D_RenderTarget* dlgtopBorrowed(CtrRenderer* r) {
    return dlgtopLease.owner == r && dlgtopWants(r) && dlgtopHolder() == DLGTOP_DIALOGUE ?
           dlgtopLease.target : nullptr;
}

void CtrLayout_chain18(Renderer*);
DLGTOP_CODE void CtrDlgtop_frame(Renderer* renderer) {
    dlgtopPrepare((CtrRenderer*) renderer);
    CtrLayout_chain18(renderer);
}

DLGTOP_CODE void CtrDlgtop_yield(Renderer* renderer, bool yield) {
    if (yield && dlgtopLease.owner == (CtrRenderer*) renderer) dlgtopReturn();
}

// ctrDestroy's existing video-release call precedes deletion of the held
// screen targets. Never retain a descriptor into a dead renderer or target.
DLGTOP_COLD void CtrDlgtop_destroy(Renderer* renderer) {
    if (dlgtopLease.owner == (CtrRenderer*) renderer) {
        dlgtopReturn();
        dlgtopLease.owner = nullptr;
        dlgtopLease.room = nullptr;
        dlgtopLease.battle = dlgtopLease.drawing = dlgtopLease.denied = false;
    }
    CtrRenderer_releaseVideo(renderer);
}

// The existing ctrBeginFrame viewport call is before dlgDrawsBegin and every
// Draw. Step-created boxes can borrow here. Draw registration records a new
// box; presentation must suspend the native frame before requesting its loan.
DLGTOP_CODE void CtrDlgtop_viewport(CtrRenderer* r, int32_t x, int32_t y, int32_t w, int32_t h, bool scissor) {
    void originalViewport(CtrRenderer*, int32_t, int32_t, int32_t, int32_t, bool) __asm__("setViewport");
    originalViewport(r, x, y, w, h, scissor);
    dlgtopTake(r);
    if (dlgtopLease.owner == r) {
        dlgtopLease.drawing = true;
        // T1 owns the other screen: draw with the game, never delay Step.
        if (dlgtopWants(r) && dlgtopBorrowed(r) == nullptr) r->dlg.armed = false;
    }
}
#undef DLGTOP_COLD
#undef DLGTOP_CONST
#endif // ENABLE_WAD16
