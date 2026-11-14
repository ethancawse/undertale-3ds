// UNDERTALE: ownership and box creation can change after dlgBeginFrame.
// names only: dialogue object lookup identifiers, no game event code or text.
// Refresh the existing recording flag/cache at those transitions, before the
// first eligible draw. Real speech bubbles still block. The diagnostic below
// owns its bookkeeping; it never borrows game variables or renderer counters.
#ifdef ENABLE_WAD16
#define DLGFLASH_TEXT __attribute__((section(".text_dlgflash"), noinline, noclone, used, optimize("Os")))
#define DLGFLASH_RO __attribute__((section(".rodata.dlgflash_in_text"), used))
static const char dlgflashMusic[] DLGFLASH_RO = "obj_musblcon";
static const char dlgflashCosmetic[] DLGFLASH_RO = "obj_cosmeticblcon";

DLGFLASH_TEXT static bool dlgflashBoxObject(const CtrRenderer* r, int32_t object) {
    Runner* runner = r->base.runner;
    for (int depth = 0; depth < 32 && object >= 0 && (uint32_t) object < runner->dataWin->objt.count; ++depth) {
        for (int32_t i = 0; i < r->dlg.policy.gateCount; ++i)
            if (object == r->dlg.policy.gateObjects[i]) return true;
        object = runner->dataWin->objt.objects[object].parentId;
    }
    return false;
}

#ifdef CTRPORT_DLGFLASH_LOG
// One summary at the end of a box's lifetime, including a later menu-close
// flash on an already-routed box. Two rectangles in one Draw count as one
// frame. Storage is proportional to live boxes (36 B each on ARM), not frames.
// No instance pointers survive a call: the next frame checks the live object
// list before reading an instance. This also handles destroyed/reused IDs.
typedef struct DlgflashBoxLog {
    struct DlgflashBoxLog* next;
    uint32_t id, lastTop, lastBottom, topBefore, topAfter, bottom;
    int32_t room, object;
} DlgflashBoxLog;
static struct {
    DlgflashBoxLog* head;
    int32_t failedRoom;
    uint32_t sweptFrame;
} dlgflashLogs __attribute__((section(".data_dlgflash"))) = { nullptr, -1, UINT32_MAX };
static const char dlgflashBoxFormat[] DLGFLASH_RO =
    "Dialogue box: room=%d object=%d id=%lu top_before_bottom=%lu top_after_bottom=%lu bottom_frames=%lu\n";
static const char dlgflashChoiceFormat[] DLGFLASH_RO =
    "Dialogue route: room=%d object=%d id=%lu frame=%lu screen=%s\n";
static const char dlgflashBottom[] DLGFLASH_RO = "bottom";
static const char dlgflashTop[] DLGFLASH_RO = "top";
static const char dlgflashLogOom[] DLGFLASH_RO =
    "Dialogue box: tracking allocation failed in room=%d\n";

DLGFLASH_TEXT static void dlgflashLogEnd(DlgflashBoxLog* box) {
    // A normal info line, outside the release-quiet blacklist. Other Off
    // disables console painting, not the file/link log. Never log per glyph.
    logInfo(dlgflashBoxFormat, (int) box->room, (int) box->object,
            (unsigned long) box->id, (unsigned long) box->topBefore,
            (unsigned long) box->topAfter, (unsigned long) box->bottom);
    free(box);
}

DLGFLASH_TEXT static void dlgflashLogSweep(CtrRenderer* r);

DLGFLASH_TEXT CtrSurface* CtrDlgflash_recordingSurface(CtrRenderer* r) {
    CtrSurface* originalRecording(CtrRenderer*) __asm__("recordingSurface");
    CtrSurface* rec = originalRecording(r);
    Runner* runner = r->base.runner;
    if (dlgflashLogs.head != nullptr && dlgflashLogs.sweptFrame != r->frame) dlgflashLogSweep(r);
    // Only emitQuad's existing recording query is routed here: the ordinary
    // box's solid rectangles reach it, sprites/glyphs and target replay do not.
    if (!r->display.dialogueBottom || r->boundSurface != runner->applicationSurfaceId ||
        runner->vmContext->currentEventType != EVENT_DRAW) return rec;
    Instance* inst = runner->vmContext->currentInstance;
    if (inst == nullptr || !dlgflashBoxObject(r, inst->objectIndex)) return rec;
    DlgflashBoxLog* box = dlgflashLogs.head;
    while (box != nullptr && (box->id != inst->instanceId || box->room != runner->currentRoomIndex)) box = box->next;
    if (box == nullptr) {
        if (dlgflashLogs.failedRoom == runner->currentRoomIndex) return rec;
        box = calloc(1, sizeof(*box));
        if (box == nullptr) {
            dlgflashLogs.failedRoom = runner->currentRoomIndex;
            logInfo(dlgflashLogOom, (int) runner->currentRoomIndex);
            return rec;
        }
        box->id = inst->instanceId; box->object = inst->objectIndex; box->room = runner->currentRoomIndex;
        box->lastTop = box->lastBottom = r->frame - 1;
        box->next = dlgflashLogs.head; dlgflashLogs.head = box;
        logInfo(dlgflashChoiceFormat, (int) box->room, (int) box->object,
                (unsigned long) box->id, (unsigned long) r->frame,
                (rec == &r->dlg.rec) != CtrScreens_onBottom ? dlgflashBottom : dlgflashTop);
    }
    if (rec == &r->dlg.rec) {
        if (box->lastBottom != r->frame) { box->lastBottom = r->frame; ++box->bottom; }
    } else if (rec == nullptr && box->lastTop != r->frame) {
        box->lastTop = r->frame;
        if (box->bottom == 0) ++box->topBefore; else ++box->topAfter;
    }
    return rec;
}

// At most once per rendered frame, on an existing solid-quad query. If a
// black room draws nothing after its last box dies, the summary waits for
// its next quad or renderer teardown; it never logs a second line for it.
DLGFLASH_TEXT static void dlgflashLogSweep(CtrRenderer* r) {
    dlgflashLogs.sweptFrame = r->frame;
    Runner* runner = r->base.runner;
    DlgflashBoxLog** link = &dlgflashLogs.head;
    while (*link != nullptr) {
        DlgflashBoxLog* box = *link;
        bool alive = false;
        if (box->room == runner->currentRoomIndex && runner->instancesByObject != nullptr) {
            Instance** instances = runner->instancesByObject[box->object];
            for (ptrdiff_t i = 0; i < arrlen(instances); ++i)
                if (instances[i]->instanceId == box->id && !instances[i]->destroyed) { alive = true; break; }
        }
        if (alive) link = &box->next;
        else { *link = box->next; dlgflashLogEnd(box); }
    }
}

DLGFLASH_TEXT void CtrDlgflash_free(CtrDialoguePolicy* policy) {
    void originalFree(CtrDialoguePolicy*) __asm__("CtrDialogue_free");
    while (dlgflashLogs.head != nullptr) {
        DlgflashBoxLog* box = dlgflashLogs.head;
        dlgflashLogs.head = box->next; dlgflashLogEnd(box);
    }
    dlgflashLogs.failedRoom = -1;
    dlgflashLogs.sweptFrame = UINT32_MAX;
    originalFree(policy);
}

#endif // CTRPORT_DLGFLASH_LOG: no tracking state or calls in public builds.

void __real_CtrDialogue_init(CtrDialoguePolicy*, const DataWin*);
DLGFLASH_TEXT void CtrDlgflash_init(CtrDialoguePolicy* policy, const DataWin* dataWin) {
    __real_CtrDialogue_init(policy, dataWin);
    int32_t kept = 0;
    for (int32_t i = 0; i < policy->blockCount; ++i) {
        int32_t object = policy->blockObjects[i];
        const char* name = dataWin->objt.objects[object].name;
        // Neither object has a Draw event or writer child: its default sprite
        // is a note/decorative bubble over the speaker, not dialogue text.
        if (name != nullptr && (!strcmp(name, dlgflashMusic) || !strcmp(name, dlgflashCosmetic))) continue;
        policy->blockObjects[kept++] = object;
    }
    policy->blockCount = kept;
}

bool __real_CtrDialogue_allowed(const CtrDialoguePolicy*, Runner*);

DLGFLASH_TEXT static void dlgflashRefresh(CtrRenderer* r, bool duringRegistration) {
    r->dlg.lastInst = nullptr;
    r->dlg.lastAnswer = false;
    r->dlg.armed = r->display.dialogueBottom && r->bottomTarget != nullptr && !r->bottomYielded &&
                   r->dlg.policy.redirectCount > 0 &&
                   (duringRegistration ? __real_CtrDialogue_allowed(&r->dlg.policy, r->base.runner) :
                                         CtrDialogue_allowed(&r->dlg.policy, r->base.runner));
}

void CtrDlgtop_refresh(CtrRenderer* r, bool duringRegistration);
void CtrDlgtop_yield(Renderer* renderer, bool yield);

DLGFLASH_TEXT void CtrDlgflash_yield(Renderer* renderer, bool yield) {
    CtrRenderer* r = (CtrRenderer*) renderer;
    bool wasYielded = r->bottomYielded;
    // Menu/notice ownership changes are outside Draw. Return the top before
    // the original yield path configures the overlay's screens.
    CtrDlgtop_yield(renderer, yield);
    // Assembler alias keeps the original compiler call graph independent.
    void originalYield(Renderer*, bool) __asm__("CtrLayout_yield8");
    originalYield(renderer, yield);
    if (wasYielded && !r->bottomYielded) CtrDlgtop_refresh(r, false);
}

DLGFLASH_TEXT void CtrDlgflash_registered(Runner* runner, Instance* inst) {
    Runner_addInstanceToObjectLists(runner, inst);
    CtrRenderer* r = (CtrRenderer*) runner->renderer;
    if (r == nullptr || !r->display.dialogueBottom || r->base.runner != runner) return;
    // Registration precedes Create and the new instance's same-pass Draw.
    // Use the pure base gate here: popbot's allowed wrapper appends anchors
    // and can free a popup recording. It must never run mid-Draw.
    // Match policy gate ancestry, not a particular elevator/controller.
    if (dlgflashBoxObject(r, inst->objectIndex)) CtrDlgtop_refresh(r, true);
}
#undef DLGFLASH_TEXT
#undef DLGFLASH_RO
#endif
