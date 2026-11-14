// popbot, steps 5 and 6 and the step 1-4 popups' logic since: with "Dialogue box: Bottom screen", the overworld's popups (the storage box, the C menu, the save prompt,
// the phone's boxes, the elevators' floor panels, the True Lab's tape list, Snowdin's crossword sheet and the gold
// panel beside a vendor's line) draw on the bottom screen too, through the dialogue recorder
// (ctr_renderer.c, "Dialogue on the bottom screen"). Nothing in that path changes: CtrDialogue_init and
// CtrDialogue_allowed are wrapped (UNDERTALE only). A popup's objects are found by name, and on each armed frame the
// allowed wrapper marks a popup's object for redirection only while that popup is drawing, arms the frame for it when
// no blocker exists (battles, shops, dates: the dialogue's own list), and adds two transparent quads at the surface's
// corners. With those, presentDialogue's own fit puts the popup at its own pixel on the 320x240 bottom screen, exactly
// where the game places it in its 320x240 view.
//
// z_popbot.c holds the wraps themselves, as composed with steps 1-4 (the storage box and the C menu); its wrappers'
// first calls (the last one, in the settings link) come here, and this file calls the real functions and the next
// link. All of this file's code and constants are appended after every other late input, so no earlier late address
// moves. z_popbot.c's own popup slots (its state and two objects, the block list's last three) are left to it: they
// are set to -1 here at every ask, so its code after the call finds no popup and changes nothing.
//
// A popup opens and closes only at a frame's first ask, before its Draw events: an ask later in the same frame gets
// the frame's answer and changes nothing, so nothing recorded is dropped mid-frame.
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
#include "renderer.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#define PB_CODE __attribute__((noinline, optimize("Os"), section(".text_popbot2")))
#define PB_RO __attribute__((section(".rodata.popbot2_in_text")))

// The popups, in the order of their slots, and the variable each one's Draw tests before drawing anything:
// - the storage box draws from its fourth frame (its Draw raises its own buffer first and draws only when
//   buffer > 3);
// - the overworld controller draws the C menu (ITEM, its Use/Info/Drop, STAT, CELL), the save prompt (menu 4) and
//   the phone's boxes (menus 6 and 7) only inside `if (global.interact == 5)`, and nothing after that block;
// - the elevators' panels (Hotland's, the last one's and the lab's; the True Lab's lifts have none) draw the floor list
//   only inside `if (con == 2 && !instance_exists(OBJ_WRITER))`, after the "which floor" line is gone. Their Draw
//   first draws the panel on the wall and ends with the doors' bars, both in the room: those stay on the top screen
//   (pb2DrawSprite);
// - the True Lab's television draws its tape list under the same test, and nothing else while it shows (its fade
//   draws only while a tape plays), so its whole Draw moves;
// - Snowdin's crossword draws itself in the room, then the sheet only while `cross == 1` (its Alarm sets it before
//   the Draw; its Draw clears it on Z or X after drawing the sheet): the prop stays on the top screen, as the panels'
//   wall draw does (pb2DrawSprite);
// - the gold panel (a vendor's gold and inventory space) is its whole Draw, made with the vendor's line and destroyed
//   in its own Draw once no writer is left: moved while one is alive and active.
// The last name is not a popup: the writer object the panels and the television test.
enum { PB_BOX, PB_MENU, PB_ELEV, PB_FINAL, PB_TAPES, PB_CROSS, PB_GOLD, PB_FAMILIES, PB_WRITER = PB_FAMILIES, PB_NAMES };
static const char pb2Names[PB_NAMES][24] PB_RO = {"obj_itemswapper", "obj_overworldcontroller", "obj_elevatorpanel",
                                                 "obj_finalelevatorpanel", "obj_labtv_event", "obj_crossword",
                                                 "obj_golddisplay", "OBJ_WRITER"};
// The gold panel tests nothing (its entry is never looked up).
static const char pb2Vars[PB_FAMILIES][12] PB_RO = {"buffer", "interact", "con", "con", "con", "cross", ""};
// Named arrays, not literals: a literal's mergeable section would let the linker fold an earlier object's string into
// one of these and move everything after it.
static const char pb2LogOn[] PB_RO = "Popups: on the bottom screen\n";
static const char pb2LogOff[] PB_RO = "Popups: closed after %d frames\n";

// The policy's last block slots: z_popbot.c's three (its state, the box's and the menu's objects), then this file's:
// [STATE] = frames shown << 1 | shown last armed frame, one object per name, the frame the wrapper last answered for,
// whether pb2DrawSprite is in the renderer's table, and the entry it replaced (a pointer's worth of slots). The
// dialogue's own blockers (at most 17 in UNDERTALE) stay below PB_LIMIT.
#define PB_OLD_BOX (CTR_DIALOGUE_BLOCK_OBJECTS - 2)
#define PB_OLD_MENU (CTR_DIALOGUE_BLOCK_OBJECTS - 3)
#define PB_OLD_LIMIT PB_OLD_MENU
#define PB_STATE (CTR_DIALOGUE_BLOCK_OBJECTS - 4)
#define PB_OBJECT(k) (PB_STATE - 1 - (k))
#define PB_FRAME (PB_OBJECT(PB_NAMES - 1) - 1)
#define PB_HOOK (PB_FRAME - 1)
#define PB_SAVED (PB_HOOK - (int32_t) (sizeof(void*) / sizeof(int32_t)))
#define PB_LIMIT PB_SAVED

// From ctr_renderer.c's end: the dialogue recorder's places in the renderer and the recorded quad's layout, then (in
// this file's section) the recorder's per-instance answer (dlgRedirecting keeps it for the instance drawing).
extern const uint32_t Popbot_layout[7];
enum { L_POLICY, L_OPS, L_TOP, L_OP_SIZE, L_OP_PAGE, L_OP_X, L_OP_Y };
extern const uint32_t Popbot2_layout[2];
enum { L_LAST_INST, L_LAST_ANSWER };
// The renderer's own table (boxedge's reference to it, ctr_renderer.c).
extern const void* const Boxedge_refs[1];
typedef void (*PbDrawSprite)(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                             float yscale, float angleDeg, uint32_t color, float alpha);

void __real_CtrDialogue_init(CtrDialoguePolicy* policy, const DataWin* dataWin);
bool __real_CtrDialogue_allowed(const CtrDialoguePolicy* policy, Runner* runner);
void CtrRenderer_chainDisplayInputlate(Renderer* renderer, const CtrDisplaySettings* settings);
void CtrPopbot2_init(CtrDialoguePolicy* policy, const DataWin* dataWin);
bool CtrPopbot2_allowed(const CtrDialoguePolicy* policy, Runner* runner);
void CtrPopbot2_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings);

PB_CODE static bool pb2Alive(Runner* runner, int32_t object, Instance** out) {
    Instance** list = runner->instancesByObject[object];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (list[j]->destroyed) continue;
        *out = list[j];
        return true;
    }
    return false;
}

// instance_exists(object), as the game asks it: any active instance of it or of an object under it.
PB_CODE static bool pb2Exists(Runner* runner, int32_t object) {
    if (object < 0) return false;
    Instance** list = runner->instancesByObject[object];
    for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
        if (list[j]->active) return true;
    }
    return false;
}

// Whether popup k draws this frame, read before the frame's Draw events, without changing anything.
PB_CODE static bool pb2Shows(Runner* runner, const CtrDialoguePolicy* policy, int32_t k, int32_t object) {
    Instance* inst;
    if (!pb2Alive(runner, object, &inst)) return false;
    if (k == PB_GOLD) return inst->active;
    // Non-owning reads (as z_builderx_chapters.c and CtrDialogue_holdAllowed read them): nothing is inserted. The
    // box's counter and a panel's state are their own; the menu's interact is global (the global scope's variables,
    // as holdAllowed reads).
    VMContext* ctx = runner->vmContext;
    if (ctx == nullptr) return false;
    Instance* from = k == PB_MENU ? ctx->globalScopeInstance : inst;
    ptrdiff_t slot = from == nullptr ? -1 : shgeti(ctx->varNameMap, (char*) pb2Vars[k]);
    if (slot < 0) return false;
    // The map read itself, not Instance_getSelfVar or RValue_toReal: their checks and fallbacks would add file-name
    // strings and an out-of-line conversion. The game keeps these as reals; an integer is accepted too.
    RValue v = IntRValueHashMap_get(&from->selfVars, ctx->varNameMap[slot].value);
    double value;
    if (v.type == RVALUE_REAL) value = v.real;
    else if (v.type == RVALUE_INT32) value = v.int32;
    else return false;
    if (k == PB_BOX) return value >= 3.0;
    if (k == PB_MENU) return value == 5.0;
    if (k == PB_CROSS) return inst->active && value == 1.0;
    return inst->active && value == 2.0 && !pb2Exists(runner, policy->blockObjects[PB_OBJECT(PB_WRITER)]);
}

PB_CODE static bool pb2Blocked(const CtrDialoguePolicy* policy, Runner* runner) {
    Instance* inst;
    for (int32_t i = 0; policy->blockCount > i; i++) {
        if (pb2Alive(runner, policy->blockObjects[i], &inst)) return true;
    }
    return false;
}

// Two transparent quads, at (0,0) and past the surface's far corner (presentDialogue clamps it to the corner):
// the recording's bounds become the whole application surface, so it is drawn at half scale, unmoved.
PB_CODE static void pb2Anchor(uint8_t* renderer, float at) {
    uint8_t* op = CtrArray_add(renderer + Popbot_layout[L_OPS], Popbot_layout[L_OP_SIZE], 1);
    if (op == nullptr) return;
    memset(op, 0, Popbot_layout[L_OP_SIZE]);
    const int32_t page = INT32_MIN; // REC_COLOR_PAGE; its colours stay 0, fully transparent
    const float x[4] = {at, at + 1.0f, at + 1.0f, at}, y[4] = {at, at, at + 1.0f, at + 1.0f};
    memcpy(op + Popbot_layout[L_OP_PAGE], &page, sizeof(page));
    memcpy(op + Popbot_layout[L_OP_X], x, sizeof(x));
    memcpy(op + Popbot_layout[L_OP_Y], y, sizeof(y));
}

// The outermost drawSprite while a panel or the crossword is moved. The recorder answers per instance (dlgRedirecting
// keeps its answer until another instance draws), and their Draw mixes the popup with drawing in the room: a panel
// first draws itself on the wall (draw_sprite at its own x, y), then the floor list (moved), whose last draw is the
// heart, then the doors' bars; the crossword draws itself (draw_me, at its own x, y), then the sheet, one sprite, and
// nothing after it. So the own-place draw is answered "stay" for that one draw and the answer forgotten after it, and
// after the next sprite (the heart, the sheet) the answer is "stay" for the rest of that Draw. Every other draw passes
// straight on.
PB_CODE static void pb2DrawSprite(Renderer* renderer, int32_t tpagIndex, float x, float y, float originX, float originY, float xscale,
                                 float yscale, float angleDeg, uint32_t color, float alpha) {
    uint8_t* r = (uint8_t*) renderer;
    CtrDialoguePolicy* policy = (CtrDialoguePolicy*) (r + Popbot_layout[L_POLICY]);
    PbDrawSprite next;
    memcpy(&next, &policy->blockObjects[PB_SAVED], sizeof(next));
    if (next == nullptr) next = ((const RendererVtable*) Boxedge_refs[0])->drawSprite;
    VMContext* ctx = renderer->runner->vmContext;
    Instance* inst = ctx == nullptr ? nullptr : ctx->currentInstance;
    if (inst == nullptr || ctx->currentEventType != EVENT_DRAW ||
        (inst->objectIndex != policy->blockObjects[PB_OBJECT(PB_ELEV)] && inst->objectIndex != policy->blockObjects[PB_OBJECT(PB_FINAL)] &&
         inst->objectIndex != policy->blockObjects[PB_OBJECT(PB_CROSS)])) {
        next(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
        return;
    }
    Instance** last = (Instance**) (r + Popbot2_layout[L_LAST_INST]);
    bool* answer = (bool*) (r + Popbot2_layout[L_LAST_ANSWER]);
    bool wall = x == inst->x && y == inst->y;
    if (wall) {
        *last = inst;
        *answer = false;
    }
    next(renderer, tpagIndex, x, y, originX, originY, xscale, yscale, angleDeg, color, alpha);
    *last = wall ? nullptr : inst;
    *answer = false;
}

// pb2DrawSprite in or out of the renderer's table: in only while a panel or the crossword is moved. It goes in front of
// the table the renderer draws through, never in the renderer's own (frame skip rewrites that one's drawSprite), and
// calls the entry it replaced, or the renderer's own one when it replaced that (boxedge's way). Out only while it is
// still the table's entry: if something has since gone in front of it, it stays, passing everything on, and is never
// put in twice. False when it is wanted but can't go in (no table in front of the renderer's own): those then stay.
PB_CODE static bool pb2Hook(uint8_t* renderer, CtrDialoguePolicy* policy, bool want) {
    RendererVtable* vt = ((Renderer*) renderer)->vtable;
    const RendererVtable* own = (const RendererVtable*) Boxedge_refs[0];
    PbDrawSprite saved;
    if (want) {
        if (policy->blockObjects[PB_HOOK] != 0) return true;
        if ((const RendererVtable*) vt == own) return false;
        saved = vt->drawSprite == own->drawSprite ? nullptr : vt->drawSprite;
        memcpy(&policy->blockObjects[PB_SAVED], &saved, sizeof(saved));
        vt->drawSprite = pb2DrawSprite;
        policy->blockObjects[PB_HOOK] = 1;
    } else if (policy->blockObjects[PB_HOOK] != 0 && vt->drawSprite == pb2DrawSprite) {
        memcpy(&saved, &policy->blockObjects[PB_SAVED], sizeof(saved));
        vt->drawSprite = saved != nullptr ? saved : own->drawSprite;
        policy->blockObjects[PB_HOOK] = 0;
    }
    return true;
}

// A popup session ends: the next dialogue box is centred as before (presentDialogue re-centres past the screen's
// bottom), and the recorder's array, grown for the popup, goes back to the heap. Only before a frame's drawing (the
// allowed wrapper's first ask, before the Draw events; the settings link, outside a frame), so nothing recorded is
// lost.
PB_CODE static void pb2Close(uint8_t* renderer, CtrDialoguePolicy* policy) {
    logInfo(pb2LogOff, (int) (policy->blockObjects[PB_STATE] >> 1));
    pb2Hook(renderer, policy, false);
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

// z_popbot.c's __wrap_CtrDialogue_init calls this first, then does its own lookup into its own slots.
PB_CODE void CtrPopbot2_init(CtrDialoguePolicy* policy, const DataWin* dataWin) {
    __real_CtrDialogue_init(policy, dataWin);
    if (policy->blockCount > PB_LIMIT) return;
    policy->blockObjects[PB_STATE] = 0;
    policy->blockObjects[PB_FRAME] = -1;
    for (int32_t k = 0; PB_NAMES > k; k++) {
        policy->blockObjects[PB_OBJECT(k)] = -1;
        for (uint32_t i = 0; dataWin->objt.count > i; i++) {
            const char* name = dataWin->objt.objects[i].name;
            if (name != nullptr && strcmp(name, pb2Names[k]) == 0) policy->blockObjects[PB_OBJECT(k)] = (int32_t) i;
        }
    }
}

// z_popbot.c's __wrap_CtrDialogue_allowed calls this first and returns its answer: with its two popup slots at -1,
// its own loop finds nothing, keeps its state at 0 and answers `dialogue || false`.
PB_CODE bool CtrPopbot2_allowed(const CtrDialoguePolicy* constPolicy, Runner* runner) {
    bool dialogue = __real_CtrDialogue_allowed(constPolicy, runner);
    // The renderer's own policy (renderer->dlg.policy), passed as const only by the declaration.
    CtrDialoguePolicy* policy = (CtrDialoguePolicy*) constPolicy;
    // z_popbot.c's lookup (at every init, after this file's) put the box and the menu in its own slots: this file
    // moves them, so they are cleared before z_popbot.c's code reads them, at every ask.
    if (PB_OLD_LIMIT >= policy->blockCount) policy->blockObjects[PB_OLD_BOX] = policy->blockObjects[PB_OLD_MENU] = -1;
    if (policy->blockCount > PB_LIMIT || policy->redirect == nullptr || runner->instancesByObject == nullptr) return dialogue;
    int32_t state = policy->blockObjects[PB_STATE];
    // Asked again in a frame already answered: that answer stands, and nothing opens, closes or moves mid-frame.
    if (policy->blockObjects[PB_FRAME] == runner->frameCount) return dialogue || (state & 1) != 0;
    policy->blockObjects[PB_FRAME] = runner->frameCount;
    bool on[PB_FAMILIES];
    bool shows = false;
    for (int32_t k = 0; PB_FAMILIES > k; k++) {
        int32_t object = policy->blockObjects[PB_OBJECT(k)];
        on[k] = object >= 0 && policy->objectCount > object && pb2Shows(runner, policy, k, object);
        shows |= on[k];
    }
    if (shows && pb2Blocked(policy, runner)) {
        for (int32_t k = 0; PB_FAMILIES > k; k++) on[k] = false;
    }
    uint8_t* renderer = (uint8_t*) policy - Popbot_layout[L_POLICY];
    if (!pb2Hook(renderer, policy, on[PB_ELEV] || on[PB_FINAL] || on[PB_CROSS])) on[PB_ELEV] = on[PB_FINAL] = on[PB_CROSS] = false;
    shows = false;
    for (int32_t k = 0; PB_FAMILIES > k; k++) {
        int32_t object = policy->blockObjects[PB_OBJECT(k)];
        if (object >= 0 && policy->objectCount > object) policy->redirect[object] = on[k];
        shows |= on[k];
    }
    if (shows) {
        if ((state & 1) == 0) logInfo(pb2LogOn);
        pb2Anchor(renderer, 0.0f);
        pb2Anchor(renderer, 4095.0f);
        state = state < INT32_MAX - 2 ? (state | 1) + 2 : state;
    } else if ((state & 1) != 0) {
        pb2Close(renderer, policy);
        return dialogue;
    }
    policy->blockObjects[PB_STATE] = state;
    return dialogue || shows;
}

// The display settings' link (z_keeplit.c's CtrKeeplit_setDisplay calls z_popbot.c's CtrPopbot_setDisplay, which
// calls this; main.c's loop calls that once a frame, outside a frame; ctr_renderer.c's wrap has already set
// dialogueBottom false while the game is on the bottom screen). With dialogue no longer on the bottom screen, no frame is armed, so the allowed wrapper isn't asked again:
// a popup session still open (Options, Dialogue box: Top screen, with the box open) is closed here, its recorder
// array freed, before the next frame's drawing. Otherwise one bit test.
PB_CODE void CtrPopbot2_setDisplay(Renderer* renderer, const CtrDisplaySettings* settings) {
    CtrDialoguePolicy* policy = (CtrDialoguePolicy*) ((uint8_t*) renderer + Popbot_layout[L_POLICY]);
    if (!settings->dialogueBottom && PB_LIMIT >= policy->blockCount && (policy->blockObjects[PB_STATE] & 1) != 0) {
        pb2Close((uint8_t*) renderer, policy);
    }
    CtrRenderer_chainDisplayInputlate(renderer, settings);
}
#endif
