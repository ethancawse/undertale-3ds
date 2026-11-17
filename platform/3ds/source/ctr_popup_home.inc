
// POPUP_HOME_HELPER_BEGIN
#ifdef ENABLE_WAD16
#define PH_CODE __attribute__((section(".text_popuphome"),noinline,used,optimize("Os")))
// Shared with frozen z_popbot2.c's already-reserved policy slots. No new
// state: its first frame admission, before Draw, decides the whole frame.
#define PH_STATE (CTR_DIALOGUE_BLOCK_OBJECTS - 4)
#define PH_WRITER (PH_STATE - 1 - 7)
#define PH_LIMIT (PH_WRITER - 3)

// Preserve original application anchors if any other family is admitted,
// including a simultaneous unrelated popup. Redirect is the first-ask
// admission result; the session bit alone also describes non-elevators.
static inline __attribute__((always_inline)) bool popupHomeElevatorOnly(const CtrDialoguePolicy* p) {
    if (p->blockCount > PH_LIMIT || p->redirect == nullptr) return false;
    bool elevator = false;
    for (int k = 0; k != 7; ++k) {
        int object = p->blockObjects[PH_STATE - 1 - k];
        if (object < 0 || object >= p->objectCount || !p->redirect[object]) continue;
        if (k != 2 && k != 3) return false;
        elevator = true;
    }
    return elevator;
}

PH_CODE void CtrPopupHome_anchor(uint8_t* renderer, float unused) {
    CtrDialoguePolicy* p = (void*) (renderer + Popbot_layout[0]);
    // The original pb2Anchor algorithm, byte for byte at the recording
    // boundary: no pushRecOp version/drop policy is introduced here.
    if (!popupHomeElevatorOnly(p)) {
        uint8_t* op = CtrArray_add(renderer + Popbot_layout[1], Popbot_layout[3], 1);
        if (op == nullptr) return;
        memset(op, 0, Popbot_layout[3]);
        const int32_t page = INT32_MIN;
        const float x[4] = {unused, unused + 1.0f, unused + 1.0f, unused};
        const float y[4] = {unused, unused, unused + 1.0f, unused + 1.0f};
        memcpy(op + Popbot_layout[4], &page, sizeof(page));
        memcpy(op + Popbot_layout[5], x, sizeof(x));
        memcpy(op + Popbot_layout[6], y, sizeof(y));
        return;
    }
    // One extent marker for the floor-list class, including its final/lab
    // panel. The game's list is y5..80 inclusive at half scale. A bottom
    // extent of83 gives it the prompt's78-row home allocation, also on a
    // fresh fit after Options has reset shown. Other popup bounds are kept.
    if (unused != 0 || p->blockCount > PH_LIMIT || p->redirect == nullptr) return;
    for (int k = 2; k != 4; ++k) { // PB_ELEV and PB_FINAL in z_popbot2.c
        int object = p->blockObjects[PH_STATE - 1 - k];
        if (object < 0 || object >= p->objectCount || !p->redirect[object]) continue;
        // Aligned, zero-filled bytes of the real recorder operation layout.
        // The private owner statically checks the alignment contract.
        uint8_t marker[CTR_POPUP_HOME_MARKER_CAPACITY] __attribute__((aligned(4)));
        memset(marker, 0, Popbot_layout[3]);
        const int32_t page = INT32_MIN;
        const float x[4] = {165, 166, 166, 165};
        const float y[4] = {165, 165, 166, 166};
        memcpy(marker + Popbot_layout[4], &page, sizeof(page));
        memcpy(marker + Popbot_layout[5], x, sizeof(x));
        memcpy(marker + Popbot_layout[6], y, sizeof(y));
        CtrPopupHome_rawPush(renderer, renderer + CtrPopupHome_recOffset[0], marker);
        return;
    }
}

PH_CODE void CtrPopupHome_push(void* renderer, void* surface, const void* operation) {
    uint8_t* bytes = renderer;
    CtrDialoguePolicy* p = (void*) (bytes + Popbot_layout[0]);
    Runner* runner = ((Renderer*) renderer)->runner;
    int32_t page;
    memcpy(&page, (const uint8_t*) operation + Popbot_layout[4], sizeof(page));
    if (surface == bytes + CtrPopupHome_recOffset[0] && page == INT32_MIN &&
        p->blockCount <= PH_LIMIT && (p->blockObjects[PH_STATE] & 1) &&
        popupHomeElevatorOnly(p) &&
        runner->vmContext != nullptr && runner->vmContext->currentInstance != nullptr &&
        runner->vmContext->currentEventType == EVENT_DRAW && p->gateCount == 1 &&
        runner->vmContext->currentInstance->objectIndex == p->gateObjects[0]) {
        int32_t writer = p->blockObjects[PH_WRITER];
        if (writer >= 0 && writer < p->objectCount && runner->instancesByObject != nullptr) {
            Instance** list = runner->instancesByObject[writer];
            ptrdiff_t i = 0;
            while (i < arrlen(list) && !list[i]->active) ++i;
            // The game's dialoguer Draw paints its border even after its
            // writer dies. Its next Step destroys the box. Suppress only
            // those obsolete colour quads; do not mutate any game state.
            if (i == arrlen(list)) return;
        }
    }
    CtrPopupHome_rawPush(renderer, surface, operation);
}
#undef PH_LIMIT
#undef PH_WRITER
#undef PH_STATE
#undef PH_CODE
#endif
// POPUP_HOME_HELPER_END
