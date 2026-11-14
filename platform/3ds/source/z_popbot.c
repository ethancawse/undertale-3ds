// popbot: with "Dialogue box: Bottom screen", the overworld's popups (the storage box, the C menu, the save prompt and
// the phone's boxes) draw on the bottom screen too, through the dialogue recorder (ctr_renderer.c, "Dialogue on the
// bottom screen"). Nothing in that path changes: CtrDialogue_init and CtrDialogue_allowed are wrapped (UNDERTALE
// only). A popup's objects are found by name, and on each armed frame the allowed wrapper marks a popup's object for
// redirection only while that popup is drawing, arms the frame for it when no blocker exists (battles, shops, dates:
// the dialogue's own list), and adds two transparent quads at the surface's corners. With those, presentDialogue's own fit puts the popup at its own
// pixel on the 320x240 bottom screen, exactly where the game places it in its 320x240 view.
//
// Steps 5 and 6 (the elevators' panels, the True Lab's tape list, Snowdin's crossword, the gold panel, and one answer
// a frame) are z_popbot2.c, appended after every other late input. This file stays as it was composed, apart from the
// three calls into z_popbot2.c (each wrapper's first or last call): z_popbot2.c does the work and sets this file's
// popup slots to -1 at every ask, so its own code below finds no popup and changes nothing.
//
// No new VRAM (the dialogue option's bottom target is the only target used), no allocation of its own, and no
// .bss or .data: the state lives in the policy's unused block slots, inside the renderer. When a popup closes, the
// recorder's array is freed, so the heap returns to what dialogue alone uses. With dialogue on the top screen
// the renderer never asks (dlgBeginFrame leaves the frame unarmed), so nothing here runs past the boot lookup.
#if defined(ENABLE_WAD16)
#include <string.h>

#include "ctr_array.h"
#include "ctr_dialogue.h"
#include "ctr_renderer.h"
#include "instance.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#define PB_CODE __attribute__((noinline, optimize("Os"), section(".text_popbot")))
#define PB_RO __attribute__((section(".rodata.popbot_in_text")))

// The popups, in the order of their slots, and the variable each one's Draw tests before drawing anything:
// - the storage box draws from its fourth frame (its Draw raises its own buffer first and draws only when
//   buffer > 3);
// - the overworld controller draws the C menu (ITEM, its Use/Info/Drop, STAT, CELL), the save prompt (menu 4) and
//   the phone's boxes (menus 6 and 7) only inside `if (global.interact == 5)`, and nothing after that block.
enum { PB_BOX, PB_MENU, PB_FAMILIES };
static const char pbNames[PB_FAMILIES][24] PB_RO = {"obj_itemswapper", "obj_overworldcontroller"};
static const char pbBuffer[] PB_RO = "buffer";
static const char pbInteract[] PB_RO = "interact";
// Named arrays, not literals: a literal's mergeable section would let the linker fold an earlier object's string into
// one of these and move everything after it.
static const char pbLogOn[] PB_RO = "Popups: on the bottom screen\n";
static const char pbLogOff[] PB_RO = "Popups: closed after %d frames\n";

// The policy's last block slots: [STATE] = frames shown << 1 | shown last armed frame, then one object per
// popup. The dialogue's own blockers (at most 17 in UNDERTALE) stay below PB_LIMIT.
#define PB_STATE (CTR_DIALOGUE_BLOCK_OBJECTS - 1)
#define PB_OBJECT(k) (PB_STATE - 1 - (k))
#define PB_LIMIT (PB_OBJECT(PB_FAMILIES - 1))

// From ctr_renderer.c's end: the dialogue recorder's places in the renderer and the recorded quad's layout.
extern const uint32_t Popbot_layout[7];
enum { L_POLICY, L_OPS, L_TOP, L_OP_SIZE, L_OP_PAGE, L_OP_X, L_OP_Y };

void __real_CtrDialogue_init(CtrDialoguePolicy* policy, const DataWin* dataWin);
bool __real_CtrDialogue_allowed(const CtrDialoguePolicy* policy, Runner* runner);
void CtrRenderer_chainDisplayInputlate(Renderer* renderer, const CtrDisplaySettings* settings);
void CtrPopbot_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);
// z_popbot2.c: each calls the real function (or the next link) itself.
void CtrPopbot2_init(CtrDialoguePolicy* policy, const DataWin* dataWin);
bool CtrPopbot2_allowed(const CtrDialoguePolicy* policy, Runner* runner);
void CtrPopbot2_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);

PB_CODE static bool pbAlive(Runner* runner, int32_t object, Instance** out) {
    Instance** list = runner->instancesByObject[object];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (list[j]->destroyed) continue;
        *out = list[j];
        return true;
    }
    return false;
}

// Whether popup k draws this frame, read before the frame's Draw events, without changing anything.
PB_CODE static bool pbShows(Runner* runner, int32_t k, int32_t object) {
    Instance* inst;
    if (!pbAlive(runner, object, &inst)) return false;
    // Non-owning reads (as z_builderx_chapters.c and CtrDialogue_holdAllowed read them): nothing is inserted. The
    // box's counter is its own; the menu's interact is global (the global scope's variables, as holdAllowed reads).
    VMContext* ctx = runner->vmContext;
    if (ctx == nullptr) return false;
    Instance* from = k == PB_BOX ? inst : ctx->globalScopeInstance;
    ptrdiff_t slot = from == nullptr ? -1 : shgeti(ctx->varNameMap, (char*) (k == PB_BOX ? pbBuffer : pbInteract));
    if (slot < 0) return false;
    // The map read itself, not Instance_getSelfVar or RValue_toReal: their checks and fallbacks would add file-name
    // strings and an out-of-line conversion. The game keeps both as reals; an integer is accepted too.
    RValue v = IntRValueHashMap_get(&from->selfVars, ctx->varNameMap[slot].value);
    double value;
    if (v.type == RVALUE_REAL) value = v.real;
    else if (v.type == RVALUE_INT32) value = v.int32;
    else return false;
    return k == PB_BOX ? value >= 3.0 : value == 5.0;
}

PB_CODE static bool pbBlocked(const CtrDialoguePolicy* policy, Runner* runner) {
    Instance* inst;
    for (int32_t i = 0; policy->blockCount > i; i++) {
        if (pbAlive(runner, policy->blockObjects[i], &inst)) return true;
    }
    return false;
}

// Two transparent quads, at (0,0) and past the surface's far corner (presentDialogue clamps it to the corner):
// the recording's bounds become the whole application surface, so it is drawn at half scale, unmoved.
PB_CODE static void pbAnchor(uint8_t* renderer, float at) {
    uint8_t* op = CtrArray_add(renderer + Popbot_layout[L_OPS], Popbot_layout[L_OP_SIZE], 1);
    if (op == nullptr) return;
    memset(op, 0, Popbot_layout[L_OP_SIZE]);
    const int32_t page = INT32_MIN; // REC_COLOR_PAGE; its colours stay 0, fully transparent
    const float x[4] = {at, at + 1.0f, at + 1.0f, at}, y[4] = {at, at, at + 1.0f, at + 1.0f};
    memcpy(op + Popbot_layout[L_OP_PAGE], &page, sizeof(page));
    memcpy(op + Popbot_layout[L_OP_X], x, sizeof(x));
    memcpy(op + Popbot_layout[L_OP_Y], y, sizeof(y));
}

// A popup session ends: the next dialogue box is centred as before (presentDialogue re-centres past the screen's
// bottom), and the recorder's array, grown for the popup, goes back to the heap. Only before a frame's drawing (the
// allowed wrapper, before the Draw events; the settings link, outside a frame), so nothing recorded is lost.
PB_CODE static void pbClose(uint8_t* renderer, CtrDialoguePolicy* policy) {
    logInfo(pbLogOff, (int) (policy->blockObjects[PB_STATE] >> 1));
    float past = 240.0f;
    memcpy(renderer + Popbot_layout[L_TOP], &past, sizeof(past));
    void** ops = (void**) (renderer + Popbot_layout[L_OPS]);
    // arrfree, as the renderer frees its arrays (the free inline: stbds_arrfreef is not linked, and pulling it
    // in moves the engine's code after it).
    void* array = *ops;
    arrfree(array);
    *ops = array;
    policy->blockObjects[PB_STATE] = 0;
}

PB_CODE void __wrap_CtrDialogue_init(CtrDialoguePolicy* policy, const DataWin* dataWin) {
    CtrPopbot2_init(policy, dataWin);
    if (policy->blockCount > PB_LIMIT) return;
    policy->blockObjects[PB_STATE] = 0;
    for (int32_t k = 0; PB_FAMILIES > k; k++) {
        policy->blockObjects[PB_OBJECT(k)] = -1;
        for (uint32_t i = 0; dataWin->objt.count > i; i++) {
            const char* name = dataWin->objt.objects[i].name;
            if (name != nullptr && strcmp(name, pbNames[k]) == 0) policy->blockObjects[PB_OBJECT(k)] = (int32_t) i;
        }
    }
}

PB_CODE bool __wrap_CtrDialogue_allowed(const CtrDialoguePolicy* constPolicy, Runner* runner) {
    bool dialogue = CtrPopbot2_allowed(constPolicy, runner);
    // The renderer's own policy (renderer->dlg.policy), passed as const only by the declaration.
    CtrDialoguePolicy* policy = (CtrDialoguePolicy*) constPolicy;
    if (policy->blockCount > PB_LIMIT || policy->redirect == nullptr || runner->instancesByObject == nullptr) return dialogue;
    bool shows = false;
    for (int32_t k = 0; PB_FAMILIES > k; k++) {
        int32_t object = policy->blockObjects[PB_OBJECT(k)];
        if (object < 0 || policy->objectCount <= object) continue;
        bool on = pbShows(runner, k, object);
        policy->redirect[object] = on;
        shows |= on;
    }
    if (shows && pbBlocked(policy, runner)) {
        for (int32_t k = 0; PB_FAMILIES > k; k++) {
            int32_t object = policy->blockObjects[PB_OBJECT(k)];
            if (0 <= object && policy->objectCount > object) policy->redirect[object] = 0;
        }
        shows = false;
    }
    uint8_t* renderer = (uint8_t*) policy - Popbot_layout[L_POLICY];
    int32_t state = policy->blockObjects[PB_STATE];
    if (shows) {
        if ((state & 1) == 0) logInfo(pbLogOn);
        pbAnchor(renderer, 0.0f);
        pbAnchor(renderer, 4095.0f);
        state = state < INT32_MAX - 2 ? (state | 1) + 2 : state;
    } else if ((state & 1) != 0) {
        pbClose(renderer, policy);
        return dialogue;
    }
    policy->blockObjects[PB_STATE] = state;
    return dialogue || shows;
}

// The display settings' link (z_keeplit.c's CtrKeeplit_setDisplay calls it, main.c's loop calls that once a frame,
// outside a frame; ctr_renderer.c's wrap has already set dialogueBottom false while the game is on the bottom
// screen). With dialogue no longer on the bottom screen, no frame is armed, so the allowed wrapper isn't asked again:
// a popup session still open (Options, Dialogue box: Top screen, with the box open) is closed here, its recorder
// array freed, before the next frame's drawing. Otherwise one bit test.
PB_CODE void CtrPopbot_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    CtrDialoguePolicy* policy = (CtrDialoguePolicy*) ((uint8_t*) renderer + Popbot_layout[L_POLICY]);
    if (!settings->dialogueBottom && PB_LIMIT >= policy->blockCount && (policy->blockObjects[PB_STATE] & 1) != 0) {
        pbClose((uint8_t*) renderer, policy);
    }
    CtrPopbot2_setDisplay(renderer, settings);
}
#endif
