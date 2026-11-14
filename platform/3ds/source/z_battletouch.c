// battletouch (UNDERTALE): play a battle's menus with the stylus on the bottom screen (maintainer, 11-08;
// its design document). The stylus only ever sends game keys a player could press, merged with
// the buttons' keys before the game sees them (CtrInput_take's wrap), plus one write: the menu cursor the arrows
// would have left, with the arrows' move sound. Step 2: buttons, menu rows and Z on any text or the FIGHT bar. Step 4
// (Menus and SOUL): in a defence the red and blue SOULs follow the pen through held arrows, the green SOUL's shield
// turns to the pen's side, and the purple SOUL takes the pen's line and follows it along.
//
// Off (the default) costs one load and one compare a frame. All code and constants are late: names outside
// .text.late.* (late_reserve.ld appends them). The first slot (.text.battletouch_late, .rodata.battletouch_in_text)
// keeps 021a570b8's bytes, the first build integrate21 placed: everything added since is in .text_battletouch2 and
// .rodata_battletouch2, appended at the end of late text, and the slot's CtrInput_take wrap is a branch there, padded
// to its old size, so no later late symbol moves (lead, 11-08). New batches go in the appended sections. The settings are in the paid data reserve; the stylus state and the
// names' ids come from the heap the first frame the stylus is on (no new BSS, as errtext E1).
#if defined(ENABLE_WAD16) && defined(__3DS__)
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ctr_battlesplit.h" // CTR_BATTLE_LAYOUT_* (battletouch18)
#include "ctr_battletouch.h"
#include "ctr_bottom_view.h"
#include "ctr_input.h"
#include "ctr_input_map.h"
#include "ctr_menu.h"
#include "ctr_wrap_chain.h"
#include "data_win.h"
#include "gml_array.h"
#include "instance.h"
#include "log.h"
#include "runner.h"
#include "stb_ds.h"
#include "vm.h"

#define TOUCH_INLINE __attribute__((always_inline))
#include "ctr_touch_core.h"

#define BT_CODE __attribute__((noinline, section(".text.battletouch_late")))
#define BT_RO __attribute__((section(".rodata.battletouch_in_text")))
#define BT_STATE __attribute__((section(".data_battletouch")))
// The engine headers' static inline helpers inlined here: kept out of line, each would be a second local
// definition of a name the base already has (check_layout), as snowkick found.
#define BT_FLAT __attribute__((noinline, flatten, section(".text.battletouch_late")))
// The appended sections (after the first slot; see the top).
#define BT_CODE2 __attribute__((noinline, section(".text_battletouch2")))
#define BT_RO2 __attribute__((section(".rodata_battletouch2")))
#define BT_FLAT2 __attribute__((noinline, flatten, section(".text_battletouch2")))
// Way B's options rows (11-08), and later new code: a third pair, appended at the end of late text, so the
// sections before it keep their size.
#define BT_CODE3 __attribute__((noinline, section(".text_battletouch3")))
#define BT_RO3 __attribute__((section(".rodata_battletouch3")))
// The first-battle hint (I2, 11-08): a fourth pair, after the third (check_layout: the third pair's code keeps its
// addresses).
// The fifth pair (lead 11-09): the fourth is pinned at its dv8r size (late_reserve.ld), so new code and anything moved
// out to make room goes here, appended after every other late tenant. Touch-test builds (their stylus logs make the
// fourth pair's code longer than its slot) map BT_CODE4 and BT_RO4 to the fifth pair, a partial move: the hint's
// helpers (BT_HINT_LINK, BT_HINT_IN: btLoadHint, btSaveHint, btStepHint) still name the fourth, so its slot keeps them
// and the pin pads it to the same size; public builds keep both pairs where they are.
#ifdef CTRPORT_TOUCH_TEST
#define BT_CODE4 __attribute__((noinline, section(".text_battletouch5")))
#define BT_RO4 __attribute__((section(".rodata_battletouch5")))
#else
#define BT_CODE4 __attribute__((noinline, section(".text_battletouch4")))
#define BT_RO4 __attribute__((section(".rodata_battletouch4")))
#endif
#define BT_CODE5 __attribute__((noinline, section(".text_battletouch5")))
#define BT_RO5 __attribute__((section(".rodata_battletouch5")))

_Static_assert(TOUCH_KEY_Z == 1u << GAME_KEY_Z && TOUCH_KEY_X == 1u << GAME_KEY_X && TOUCH_KEY_ENTER == 1u << GAME_KEY_ENTER
               && TOUCH_KEY_UP == 1u << GAME_KEY_UP && TOUCH_KEY_DOWN == 1u << GAME_KEY_DOWN
               && TOUCH_KEY_LEFT == 1u << GAME_KEY_LEFT && TOUCH_KEY_RIGHT == 1u << GAME_KEY_RIGHT
               && TOUCH_KEY_SHIFT == 1u << GAME_KEY_SHIFT, "ctr_touch_core.h's keys are CtrGameKey bits");
_Static_assert(sizeof(TouchEdges) == sizeof(CtrInputEdges), "TouchEdges is CtrInputEdges' layout");

CtrInputEdges __real_CtrInput_take(const CtrInputMap* map, uint32_t held, uint32_t down, bool enabled, bool suppressHeld,
                                   CtrInputStats* stats);
CtrInputEdges __wrap_CtrInput_take(const CtrInputMap* map, uint32_t held, uint32_t down, bool enabled, bool suppressHeld,
                                   CtrInputStats* stats);
void __real_benchfidLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
void __wrap_benchfidLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps);
bool __real_benchfidOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
bool __wrap_benchfidOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
void __real_CtrDisplay_save(const CtrDisplaySettings* settings, const char* path);
void __wrap_CtrDisplay_save(const CtrDisplaySettings* settings, const char* path);
void Battletouch_loadHint(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps); // the hint (file end)
void Battletouch_saveHint(const CtrDisplaySettings* settings, const char* path);
// --ini touch_hint (the stylus hint, file end), then the settings' other keys (__real_benchfidOverride).
bool Battletouch_hintOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps);
CtrMenuAction __real_CtrCtrlui_main(CtrMenu* menu, int32_t dir);
CtrMenuAction __wrap_CtrCtrlui_main(CtrMenu* menu, int32_t dir);
CtrMenuAction __real_CtrCtrlui_update(CtrMenu* menu, u32 down);
CtrMenuAction __wrap_CtrCtrlui_update(CtrMenu* menu, u32 down);
bool __real_CtrCtrlui_holdMoves(u32 key);
bool __wrap_CtrCtrlui_holdMoves(u32 key);
void CtrOptmenu_text(uint16_t* fb, int32_t x, int32_t y, const char* s, uint32_t col);
void CtrOptmenu_pressSound(void);
int32_t CtrBattlesplit_setting(void);
void CtrBattlesplit_setSetting(int32_t value);
extern bool CtrScreens_onBottom;
extern uint8_t CtrScreens_game;
// The Battle layout row with the game on either screen (battletouch18, the file's end): a zero byte where obRow and the
// keys read CtrScreens_game for the grey (never grey now), and the value's step (Original, 3DS Mode, Top screen).
extern const uint8_t Battletouch_zero18;
void Battletouch_layoutStep18(int32_t value);

// ===[ Names: the game's variables and objects, found by name once per data file ]===

enum { G_BMENUNO, G_BMENUCOORD, G_MNFIGHT, G_MYFIGHT, G_IDEALBORDER, G_MONSTER, G_CHOICES, G_ITEM, G_MERCY, G_LANGUAGE,
       G_SP, S_ACTIVE, S_HALT, S_MOVEMENT, S_CONFUSE, V_N };
enum { O_BC, O_WRITER, O_FIGHTBT, O_TALKBT, O_ITEMBT, O_SPAREBT, O_HEART, O_N };
// V_N names (globals then instance variables), then O_N object names; each ends with a 0.
static const char btNameList[] BT_RO =
    "bmenuno\0bmenucoord\0mnfight\0myfight\0idealborder\0monster\0choices\0item\0mercy\0language\0sp\0"
    "active\0halt\0movement\0confuse\0"
    "obj_battlecontroller\0OBJ_WRITER\0obj_fightbt\0obj_talkbt\0obj_itembt\0obj_sparebt\0obj_heart\0";
static const char btKeyFmt[] BT_RO = " %63[^= ] = %d";
static const char btKey[] BT_RO = "battle_touch";
static const char btSaveFmt[] BT_RO = "battle_touch = %d\n";
static const char btModeR[] BT_RO = "r";
static const char btAppend[] BT_RO = "a";
static const char btJa[] BT_RO = "ja";
static const char btSnd[] BT_RO = "audio_play_sound";
static const char btNoNames[] BT_RO = "Stylus: battle variables not found (%d); the stylus stays off in battles\n";

#define BT_SND_MOVE 115 // the arrows' move sound: the literal the battle controller's arrow events play

// The names' ids, on the heap from the first frame the stylus is on (Old's late data reserve is spent).
typedef struct {
    DataWin* namesFor;       // the data file the ids below are for
    int32_t id[V_N];         // variable ids (globals: INSTANCE_GLOBAL entries, the rest INSTANCE_SELF)
    int32_t obj[O_N];
    bool named;              // every name found
} BtNames;

// Names added after the first slot: their own list and fields after BtNames, so the slot's code keeps its offsets.
// X_N instance variables (purple's), then P_N objects.
enum { X_TYPE, X_YNO, X_YAMT, X_YZERO, X_YSPACE, X_YOFF, X_MOVING, X_SHOT, X_N };
enum { P_SHIELD, P_PURPLE, P_N };
static const char btNameList2[] BT_RO2 = "type\0yno\0yamt\0yzero\0yspace\0yoff\0moving\0shot\0obj_spearblocker\0obj_purpleheart\0";
typedef struct {
    BtNames base;            // the slot's names (bt.names points here)
    int32_t id2[X_N];        // INSTANCE_SELF variable ids
    int32_t obj2[P_N];
} BtNamesX;
#define BTX ((BtNamesX*) bt.names)

typedef struct {
    Runner* runner;          // from the Runner_step chain, for the next frame's input sample
    BtNames* names;          // nullptr until the stylus is first on
    int8_t fileValue;        // battle_touch in settings.ini when it was last read, -1: no line
    int8_t given;            // --ini battle_touch, -1: none
    int8_t menuValue;        // the options menu's choice this run, -1: not changed (step 2b)
    uint8_t hintSeen;        // the first-battle hint (btHint, in the padding before live): BT_SEEN_*
    struct BtLive* live;     // the stylus's own state, from the heap at first use (The merge)
#ifdef CTRPORT_TOUCH_TEST
    bool scriptRead, battleSeen;
    uint16_t lines, next;
    uint32_t frame;          // frames since the battle controller first existed
    bool pen;
    uint32_t keys;           // the scripted buttons held ("key" lines), in the touch core's key bits
    uint32_t lastId;         // the menu id last logged
    uint32_t lastHeld;       // the merged held keys last logged
#endif
} Battletouch;
static Battletouch bt BT_STATE = {.fileValue = -1, .given = -1, .menuValue = -1};

// ===[ Settings: battle_touch = 0 (Off) | 1 (Menus) | 2 (Menus and SOUL) ]===

// 0: another line; 1: ours (into *value); -1: ours with a bad value.
static BT_CODE int btLine(const char* line, int8_t* value) {
    char key[64];
    int v;
    if (sscanf(line, btKeyFmt, key, &v) != 2 || strcmp(key, btKey) != 0) return 0;
    if (0 > v || v > 2) return -1;
    *value = (int8_t) v;
    return 1;
}

// screens' settings read (at boot, and again just before every save rewrites the file): the file's line.
BT_CODE void __wrap_benchfidLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    CTR_BENCHFIDLOAD_AFTER_BATTLETOUCH(settings, path, caps);
    int8_t value = -1;
    FILE* f = fopen(path, btModeR);
    if (f != nullptr) {
        char line[128];
        while (fgets(line, sizeof(line), f) != nullptr) btLine(line, &value);
        fclose(f);
    }
    bt.fileValue = value;
}

// --ini battle_touch=N: this run only (a save never writes it).
BT_CODE bool __wrap_benchfidOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    int8_t value = -1;
    int kind = btLine(line, &value);
    if (kind == 0) return Battletouch_hintOverride(settings, line, caps);
    if (kind > 0) bt.given = value;
    return kind > 0;
}

// The display's rewrite of settings.ini keeps only its own keys: battle_touch goes back as the menu set it this run,
// else as the file had it (none if it had none, so a file that never had the line still has none).
BT_CODE void __wrap_CtrDisplay_save(const CtrDisplaySettings* settings, const char* path) {
    int8_t value = bt.menuValue >= 0 ? bt.menuValue : bt.fileValue;
    CTR_DISPLAYSAVE_AFTER_BATTLETOUCH(settings, path);
    if (0 > value) return;
    FILE* f = fopen(path, btAppend);
    if (f != nullptr) {
        fprintf(f, btSaveFmt, (int) value);
        fclose(f);
    }
}

// The setting in effect: the menu's, --ini's, else the file's (Off without one). A test run reads the file too: the
// stylus does nothing until the screen is touched, and Azahar runs have no --ini.
static BT_CODE int32_t btMode(void) {
    if (bt.menuValue >= 0) return bt.menuValue;
    if (bt.given >= 0) return bt.given;
    return 0 > bt.fileValue ? 0 : bt.fileValue;
}

// For battlesplit (ctr_battletouch.h): the setting in effect.
BT_CODE int32_t CtrBattletouch_mode(void) {
    return btMode();
}

// ===[ The game's state ]===

// The Runner_step chain: inputlate -> here -> swkbd -> real. Keeps the runner for the next frame's input sample,
// which comes before this frame's step.
BT_CODE void Battletouch_step(Runner* runner) {
    bt.runner = runner;
    CTR_STEP_AFTER_BATTLETOUCH(runner);
}

static BT_CODE bool btNames_find(DataWin* dw) {
    const char* name = btNameList;
    for (int32_t k = 0; V_N + O_N > k; k++, name += strlen(name) + 1) {
        int32_t found = -1;
        if (V_N > k) {
            int32_t want = S_ACTIVE > k ? INSTANCE_GLOBAL : INSTANCE_SELF;
            for (uint32_t i = 0; dw->vari.variableCount > i; i++) {
                Variable* v = &dw->vari.variables[i];
                if (v->instanceType != want || v->name == nullptr || strcmp(v->name, name) != 0) continue;
                if (0 > v->varID || (found >= 0 && found != v->varID)) return false;
                found = v->varID;
            }
            bt.names->id[k] = found;
        } else {
            for (uint32_t i = 0; dw->objt.count > i; i++) {
                if (dw->objt.objects[i].name != nullptr && strcmp(dw->objt.objects[i].name, name) == 0) {
                    found = (int32_t) i;
                    break;
                }
            }
            bt.names->obj[k - V_N] = found;
        }
        if (0 > found) {
            logInfo(btNoNames, (int) k);
            return false;
        }
    }
    return true;
}

// The first live instance of an object, or nullptr.
static BT_CODE Instance* btFirst(Runner* runner, int32_t obj) {
    if (runner->instancesByObject == nullptr) return nullptr;
    Instance** list = runner->instancesByObject[obj];
    for (ptrdiff_t i = 0; arrlen(list) > i; i++) {
        if (!list[i]->destroyed && list[i]->active) return list[i];
    }
    return nullptr;
}

// A number's whole part (toward zero), doubled first when twice (bmenuno's 3.5); fallback when it isn't a number.
static BT_CODE int32_t btInt(const RValue* v, int32_t fallback, bool twice) {
    if (v == nullptr) return fallback;
    int32_t times = twice ? 2 : 1;
    switch (v->type) {
        case RVALUE_REAL: return (int32_t) (v->real * (GMLReal) times);
        case RVALUE_INT32: case RVALUE_BOOL: return v->int32 * times;
#ifndef NO_RVALUE_INT64
        case RVALUE_INT64: return (int32_t) v->int64 * times;
#endif
        default: return fallback;
    }
}

// A global's slot (nullptr: not set yet).
static BT_FLAT RValue* btGlobal(VMContext* ctx, int32_t k) {
    return IntRValueHashMap_findSlot(&ctx->globalScopeInstance->selfVars, bt.names->id[k]);
}

// global name (index < 0) or global name[index], as btInt reads it.
static BT_FLAT int32_t btGet(VMContext* ctx, int32_t k, int32_t index, int32_t fallback) {
    RValue* slot = btGlobal(ctx, k);
    if (slot == nullptr || 0 > index) return btInt(slot, fallback, false);
    RValue v = GMLArray_getOnArrayRef(slot, index);
    return btInt(&v, fallback, false);
}

static BT_FLAT int32_t btSelf(Instance* inst, int32_t k, int32_t fallback) {
    return btInt(IntRValueHashMap_findSlot(&inst->selfVars, bt.names->id[k]), fallback, false);
}

#define BT_RECT(x0, y0, x1, y1) ((TouchRect) {(int16_t) (x0), (int16_t) (y0), (int16_t) (x1), (int16_t) (y1)})

// A number times 16, rounded (0 when it isn't a number).
static BT_CODE int32_t btSixteenths(const RValue* v) {
    if (v == nullptr) return 0;
    if (v->type == RVALUE_REAL) return (int32_t) (v->real * 16.0 + (v->real < 0 ? -0.5 : 0.5));
    return btInt(v, 0, false) * 16;
}

// The first slot's SOUL reader (021a570b8, the red SOUL only), kept byte for byte; btRead_soul2 below replaces its
// answer every frame (Battletouch_take2).
static BT_CODE void btRead_soul(Runner* runner, TouchMenu* m) {
    Instance* h = btFirst(runner, bt.names->obj[O_HEART]);
    DataWin* dw = runner->dataWin;
    if (h == nullptr || h->objectIndex != bt.names->obj[O_HEART] || 0 > h->spriteIndex || (uint32_t) h->spriteIndex >= dw->sprt.count)
        return;
    if (btSelf(h, S_MOVEMENT, 0) != 1 || btSelf(h, S_CONFUSE, 0) != 0) return;
    Sprite* sp = &dw->sprt.sprites[h->spriteIndex];
    // The centre: the sprite's box as the game draws it (origin, size, scale).
    float cx = h->x + ((float) sp->width * 0.5f - (float) sp->originX) * h->imageXscale;
    float cy = h->y + ((float) sp->height * 0.5f - (float) sp->originY) * h->imageYscale;
    m->soul = TOUCH_SOUL_RED;
    m->soulX = (int32_t) (cx * 16.0f);
    m->soulY = (int32_t) (cy * 16.0f);
    m->soulStep = btSixteenths(btGlobal(runner->vmContext, G_SP));
}

// The game's menu as the last Step left it, into *m; the cursor's bmenucoord index into *slot (-1: none).
static BT_CODE void btRead_menu(Runner* runner, TouchMenu* m, int32_t* slot) {
    VMContext* ctx = runner->vmContext;
    *m = (TouchMenu) {0};
    *slot = -1;
    Instance* bc = btFirst(runner, bt.names->obj[O_BC]);
    if (bc == nullptr || ctx->globalScopeInstance == nullptr) return; // not a battle: OFF
    int32_t menu2 = btInt(btGlobal(ctx, G_BMENUNO), -2, true); // bmenuno times 2 (ITEM's page 2 is 3.5)
    int32_t myfight = btGet(ctx, G_MYFIGHT, -1, -1), mnfight = btGet(ctx, G_MNFIGHT, -1, -1);
    int32_t active = btSelf(bc, S_ACTIVE, 0), mercy = btGet(ctx, G_MERCY, -1, 0);
    int32_t ib0 = btGet(ctx, G_IDEALBORDER, 0, 0), ib1 = btGet(ctx, G_IDEALBORDER, 1, 0);
    int32_t ib2 = btGet(ctx, G_IDEALBORDER, 2, 0);
    RValue* lang = btGlobal(ctx, G_LANGUAGE);
    bool ja = lang != nullptr && lang->type == RVALUE_STRING && lang->string != nullptr && strcmp(lang->string, btJa) == 0;
    m->id = ((uint32_t) menu2 & 0xffu) | ((uint32_t) myfight & 0xffu) << 8 | ((uint32_t) mnfight & 0xffu) << 16
            | ((uint32_t) active & 0x7fu) << 24;
    m->ctx = mnfight == 2 ? TOUCH_CTX_DEFENCE : TOUCH_CTX_TAP;
    if (mnfight == 2) {
        if (btMode() == 2) btRead_soul(runner, m);
        return;
    }
    // The rows: the menu-row script's y(k) = idealborder[2] + 28 + 32k (Japanese 27 + 36k), the SOUL drawn there; each
    // row's area from 8 above that to the next row's.
    int32_t rowY = ib2 + (ja ? 27 : 28) - 8, rowH = ja ? 36 : 32, left = ib0 + 16, right = ib1 - 16, split = ib0 + 276;
    // The two-way choice in text: the battle controller's Step (myfight 3, the writer's halt > 0), its cursor
    // bmenucoord[6], moved by the arrows only with bmenuno 6 (KeyPress_39 @0318).
    if (myfight == 3 && mnfight == 0 && menu2 == 12) {
        Instance* writer = btFirst(runner, bt.names->obj[O_WRITER]);
        if (writer == nullptr || btSelf(writer, S_HALT, 0) <= 0) return;
        int32_t y = ib2 + 92 + (ja ? 8 : 0) - 8;
        m->ctx = TOUCH_CTX_MENU;
        m->id |= 1u << 31;
        m->count = 2;
        m->valid = 3;
        m->item[0] = BT_RECT(left, y, ib0 + 268, y + 32);
        m->item[1] = BT_RECT(ib0 + 268, y, right, y + 32);
        *slot = 6;
    } else if (active == 1 && mnfight == 0 && myfight == 0) {
        m->ctx = TOUCH_CTX_MENU;
        // The buttons (FIGHT .. MERCY): each live one's drawn box. Valid as the arrows allow
        // (KeyPress_39 @0094-0100): mercy 2 skips MERCY, mercy 3 keeps the cursor on ACT.
        static const uint8_t buttonObj[4] BT_RO = {O_FIGHTBT, O_TALKBT, O_ITEMBT, O_SPAREBT};
        uint8_t buttons = 0;
        TouchRect box[4] = {{0}};
        DataWin* dw = runner->dataWin;
        for (int32_t k = 0; 4 > k; k++) {
            Instance* b = btFirst(runner, bt.names->obj[buttonObj[k]]);
            if (b == nullptr || 0 > b->spriteIndex || (uint32_t) b->spriteIndex >= dw->sprt.count) continue;
            Sprite* sp = &dw->sprt.sprites[b->spriteIndex];
            float x0 = b->x - (float) sp->originX * b->imageXscale, x1 = x0 + (float) sp->width * b->imageXscale;
            float y0 = b->y - (float) sp->originY * b->imageYscale, y1 = y0 + (float) sp->height * b->imageYscale;
            box[k] = BT_RECT(x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, x0 < x1 ? x1 : x0, y0 < y1 ? y1 : y0);
            if (!(mercy == 2 && k == 3) && !(mercy == 3 && k != 1)) buttons |= (uint8_t) (1u << k);
        }
        if (menu2 == 0) {
            m->atButtons = true;
            m->count = 4;
            m->valid = buttons;
            memcpy(m->item, box, sizeof(box));
            *slot = 0;
        } else {
            m->buttonValid = buttons;
            m->openButton = (int8_t) btGet(ctx, G_BMENUCOORD, 0, 0);
            memcpy(m->button, box, sizeof(box));
            if (menu2 == 2 || menu2 == 4 || menu2 == 22) {
                // Targets (bmenuno 1, 2, 11): bmenucoord[1], a row when monster[k] != 0 (KeyPress_40 @00c4-0200).
                *slot = 1;
                m->count = 3;
                for (int32_t k = 0; 3 > k; k++) {
                    m->item[k] = BT_RECT(left, rowY + rowH * k, right, rowY + rowH * (k + 1));
                    if (btGet(ctx, G_MONSTER, k, 0) != 0) m->valid |= (uint8_t) (1u << k);
                }
            } else if (menu2 == 20) {
                // ACT (bmenuno 10): bmenucoord[2], two columns of three (Step @13e8), a choice when choices[k] != 0.
                *slot = 2;
                m->count = 6;
                for (int32_t k = 0; 6 > k; k++) {
                    int32_t y = rowY + rowH * (3 > k ? k : k - 3);
                    m->item[k] = 3 > k ? BT_RECT(left, y, split, y + rowH) : BT_RECT(split, y, right, y + rowH);
                    if (btGet(ctx, G_CHOICES, k, 0) != 0) m->valid |= (uint8_t) (1u << k);
                }
            } else if ((menu2 == 6 || menu2 == 7) && !ja) {
                // ITEM (English, bmenuno 3 and 3.5): bmenucoord[3], 2x2 (Step @1550: rows + 28 / + 60, columns + 32 /
                // + 280), page 2 is bmenuno 3.5, slot k = item[k + 4 * page]. The page label (third row, right) turns
                // the page through the game's Right from the top right slot (KeyPress_39 @03e4-063c, both pages).
                *slot = 3;
                m->count = 4;
                int32_t page = menu2 == 7 ? 4 : 0;
                for (int32_t k = 0; 4 > k; k++) {
                    int32_t y = rowY + rowH * (k >> 1);
                    m->item[k] = (k & 1) == 0 ? BT_RECT(left, y, split, y + rowH) : BT_RECT(split, y, right, y + rowH);
                    if (btGet(ctx, G_ITEM, k + page, 0) != 0) m->valid |= (uint8_t) (1u << k);
                }
                m->pageValid = btGet(ctx, G_ITEM, 4, 0) != 0;
                m->page = BT_RECT(split, rowY + rowH * 2, right, rowY + rowH * 3);
                m->pageSlot = 1;
            } else if (menu2 == 8) {
                // MERCY (bmenuno 4): bmenucoord[4], Spare, and Flee while mercy < 1 (KeyPress_40 @0778).
                *slot = 4;
                m->count = 2;
                m->valid = (uint8_t) (mercy < 1 ? 3 : 1);
                for (int32_t k = 0; 2 > k; k++) m->item[k] = BT_RECT(left, rowY + rowH * k, right, rowY + rowH * (k + 1));
            }
            // Japanese ITEM (scrolling rows) and any other menu: the buttons only.
        }
    }
    if (*slot >= 0) m->cursor = (int8_t) btGet(ctx, G_BMENUCOORD, *slot, 0);
}

// The cursor write: bmenucoord[slot] = value, the same type as the value there (the arrows' pushi/add keep it), then
// the arrows' move sound through the game's own builtin (the sound script's body: audio_play_sound(115, 80, false)).
static BT_FLAT void btWrite(Runner* runner, int32_t slot, int32_t value, bool sound) {
    VMContext* ctx = runner->vmContext;
    RValue* array = btGlobal(ctx, G_BMENUCOORD);
    if (array == nullptr || array->type != RVALUE_ARRAY) return;
    RValue now = GMLArray_getOnArrayRef(array, slot);
    VM_setArrayElementInSlot(ctx, array, slot, now.type == RVALUE_REAL ? RValue_makeReal((GMLReal) value) : RValue_makeInt32(value));
    if (sound) {
        BuiltinFunc play = VM_findBuiltin(ctx, btSnd);
        RValue args[3] = {RValue_makeInt32(BT_SND_MOVE), RValue_makeInt32(80), RValue_makeInt32(0)};
        if (play != nullptr) (void) play(ctx, args, 3); // a number: nothing to free
    }
}

#ifdef CTRPORT_TOUCH_TEST
// ===[ Test builds only: touchscript.txt in the game folder drives the pen ]===
// Lines "<frame> pen <x> <y>" (bottom-screen pixels), "<frame> up" and "<frame> key <hex>" (the buttons held from
// then, in the touch core's key bits, entering where the buttons' own do), frames counted from the first frame the
// battle controller exists, or with a line "0 anywhere" from the first sample (touchui's cases outside battle); read
// once. A test (bench) run turns the buttons off, so the script works there too, and
// maps the pen as if the game were on the bottom screen.
#define BT_SCRIPT_MAX 4096
typedef struct { uint32_t frame; int16_t x, y; uint8_t kind; uint32_t keys; } BtScriptLine; // kind 0 up, 1 pen, 2 key
static BtScriptLine btScript[BT_SCRIPT_MAX]; // test builds only: BSS (their layout is never a release's)
static bool btScriptAnywhere;               // test builds only: the clock starts at the first sample
static const char btScriptAny[] BT_RO = "%lu anywher%1[e]";
static const char btScriptPath[] BT_RO = CTRPORT_GAME_DIR "/touchscript.txt";
static const char btScriptPen[] BT_RO = "%lu pen %d %d";
static const char btScriptUp[] BT_RO = "%lu u%1[p]";
static const char btScriptKey[] BT_RO = "%lu key %lx";
static const char btScriptLog[] BT_RO = "[touch] f=%lu pen=%d (%d,%d) ctx=%d id=%08lx cur=%d mode=%d write=%d sound=%d pulse=%02lx\n";
static const char btScriptGame[] BT_RO = "[touch] f=%lu game id=%08lx cur=%d slot=%d held=%03lx pressed=%03lx\n";
static const char btScriptHeld[] BT_RO = "[touch] f=%lu held=%03lx soul=%d (%ld,%ld) step=%ld\n";

static BT_CODE void btScriptRead(void) {
    bt.scriptRead = true;
    FILE* f = fopen(btScriptPath, btModeR);
    if (f == nullptr) return;
    char line[96];
    while (bt.lines < BT_SCRIPT_MAX && fgets(line, sizeof(line), f) != nullptr) {
        unsigned long frame;
        int x, y;
        char u[2];
        unsigned long keys;
        BtScriptLine* l = &btScript[bt.lines];
        if (sscanf(line, btScriptPen, &frame, &x, &y) == 3) *l = (BtScriptLine) {(uint32_t) frame, (int16_t) x, (int16_t) y, 1, 0};
        else if (sscanf(line, btScriptKey, &frame, &keys) == 2) *l = (BtScriptLine) {(uint32_t) frame, 0, 0, 2, (uint32_t) keys};
        else if (sscanf(line, btScriptUp, &frame, u) == 2) *l = (BtScriptLine) {(uint32_t) frame, 0, 0, 0, 0};
        else {
            if (sscanf(line, btScriptAny, &frame, u) == 2) btScriptAnywhere = true;
            continue;
        }
        bt.lines++;
    }
    fclose(f);
}

// The scripted pen this frame: true when a script is in charge. px, py: set on a pen line, else kept.
static BT_CODE bool btScriptPenNow(Runner* runner, bool* pen, int16_t* px, int16_t* py) {
    if (!bt.scriptRead) btScriptRead();
    if (bt.lines == 0) return false;
    if (!bt.battleSeen) bt.battleSeen = btScriptAnywhere || btFirst(runner, bt.names->obj[O_BC]) != nullptr;
    if (bt.battleSeen) {
        while (bt.next < bt.lines && btScript[bt.next].frame <= bt.frame) {
            const BtScriptLine* l = &btScript[bt.next];
            if (l->kind == 2) bt.keys = l->keys;
            else bt.pen = l->kind == 1;
            if (l->kind == 1) { *px = l->x; *py = l->y; }
            bt.next++;
        }
        bt.frame++;
    }
    *pen = bt.pen;
    return true;
}
#endif

// ===[ The game-on-bottom view (battlesplit's strong definition replaces this) ]===

#ifdef CTRPORT_TOUCH_TEST
#define BT_SCRIPTED (bt.lines > 0)
#else
#define BT_SCRIPTED false
#endif

__attribute__((weak)) BT_CODE bool CtrLayout_bottomView(Runner* runner, CtrBottomView* out) {
    if (!CtrScreens_onBottom && !BT_SCRIPTED) return false;
    if (runner->viewsEnabled && runner->views[0].enabled) {
        GMLCamera* cam = Runner_getCameraForView(runner, 0);
        if (cam == nullptr || cam->viewWidth <= 0) return false;
        *out = (CtrBottomView) {cam->viewX, cam->viewY, (float) cam->viewWidth / 320.0f};
    } else {
        if (runner->currentRoom == nullptr) return false;
        *out = (CtrBottomView) {0.0f, 0.0f, (float) runner->currentRoom->width / 320.0f};
    }
    return true;
}

#ifdef CTRPORT_TOUCH_TEST
// Test builds: a bench run keeps the game on top, and battlesplit's definition then maps nothing; a scripted run maps
// the pen as if the game were on the bottom screen.
static BT_CODE2 bool btTestView(Runner* runner, CtrBottomView* out) {
    if (runner->viewsEnabled && runner->views[0].enabled) {
        GMLCamera* cam = Runner_getCameraForView(runner, 0);
        if (cam == nullptr || cam->viewWidth <= 0) return false;
        *out = (CtrBottomView) {cam->viewX, cam->viewY, (float) cam->viewWidth / 320.0f};
    } else {
        if (runner->currentRoom == nullptr) return false;
        *out = (CtrBottomView) {0.0f, 0.0f, (float) runner->currentRoom->width / 320.0f};
    }
    return true;
}
#endif

// ===[ Appended after 021a570b8: names and the SOUL ]===

// Each name of btNameList2 into BtNamesX: X_N instance variables, then P_N objects (as btNames_find).
static BT_CODE2 bool btNames_find2(DataWin* dw) {
    const char* name = btNameList2;
    for (int32_t k = 0; X_N + P_N > k; k++, name += strlen(name) + 1) {
        int32_t found = -1;
        if (X_N > k) {
            for (uint32_t i = 0; dw->vari.variableCount > i; i++) {
                Variable* v = &dw->vari.variables[i];
                if (v->instanceType != INSTANCE_SELF || v->name == nullptr || strcmp(v->name, name) != 0) continue;
                if (0 > v->varID || (found >= 0 && found != v->varID)) return false;
                found = v->varID;
            }
            BTX->id2[k] = found;
        } else {
            for (uint32_t i = 0; dw->objt.count > i; i++) {
                if (dw->objt.objects[i].name != nullptr && strcmp(dw->objt.objects[i].name, name) == 0) {
                    found = (int32_t) i;
                    break;
                }
            }
            BTX->obj2[k - X_N] = found;
        }
        if (0 > found) {
            logInfo(btNoNames, (int) (V_N + O_N + k));
            return false;
        }
    }
    return true;
}

// An appended instance variable (X_*), as btSelf reads it; and times 16, rounded (0 when it isn't a number).
static BT_FLAT2 int32_t btSelfX(Instance* inst, int32_t k, int32_t fallback) {
    return btInt(IntRValueHashMap_findSlot(&inst->selfVars, BTX->id2[k]), fallback, false);
}
static BT_FLAT2 int32_t btSelfX16(Instance* inst, int32_t k) {
    return btSixteenths(IntRValueHashMap_findSlot(&inst->selfVars, BTX->id2[k]));
}

// The centre of an instance of obj (itself, not a child): its sprite's box as the game draws it (origin, size,
// scale), into m's soulX/soulY; false without one.
static BT_CODE2 bool btCentre(Runner* runner, Instance* h, int32_t obj, TouchMenu* m) {
    DataWin* dw = runner->dataWin;
    if (h == nullptr || h->objectIndex != obj || 0 > h->spriteIndex || (uint32_t) h->spriteIndex >= dw->sprt.count)
        return false;
    Sprite* sp = &dw->sprt.sprites[h->spriteIndex];
    float cx = h->x + ((float) sp->width * 0.5f - (float) sp->originX) * h->imageXscale;
    float cy = h->y + ((float) sp->height * 0.5f - (float) sp->originY) * h->imageYscale;
    m->soulX = (int32_t) (cx * 16.0f);
    m->soulY = (int32_t) (cy * 16.0f);
    return true;
}

// The defence's SOUL, into m (Menus and SOUL only; design section 4). Muffet's purple SOUL first: its own object on
// the web's lines (obj_heart waits off screen meanwhile), moved by the arrows in its type 0 only; while one exists
// nothing else is followed. Else obj_heart itself, not confused: red (movement 1), yellow (movement 1 with shot = 1:
// Mettaton's shooter turns; a pen-down on it also fires, ctr_touch_core.h), blue (2, gravity down) or green (3, with
// the spear blocker's shield, which turns to the arrows held). Any other heart or mode stays NONE, on the buttons.
static BT_CODE2 void btRead_soul2(Runner* runner, TouchMenu* m) {
    Instance* p = btFirst(runner, BTX->obj2[P_PURPLE]);
    if (p != nullptr) {
        int32_t lines = btSelfX(p, X_YAMT, 0), line = btSelfX(p, X_YNO, 0), space = btSelfX16(p, X_YSPACE);
        if (btSelfX(p, X_TYPE, -1) != 0 || 0 >= space || 1 > line || line > lines || lines > 100
            || !btCentre(runner, p, BTX->obj2[P_PURPLE], m))
            return;
        m->soul = TOUCH_SOUL_PURPLE;
        m->soulStep = 4 * 16; // the purple SOUL's Step: x -= 4 / x += 4 for the held arrows
        // Line 1's centre: the game puts the SOUL's y at yzero + (yno - 1) * yspace + yoff (less the move under way);
        // the sprite's centre sits a fixed offset from y.
        m->line0 = btSelfX16(p, X_YZERO) + btSelfX16(p, X_YOFF) + m->soulY - (int32_t) (p->y * 16.0f);
        m->lineSpace = space;
        m->line = (int8_t) line;
        m->lines = (int8_t) lines;
        m->lineBusy = btSelfX(p, X_MOVING, 0) != 0;
        return;
    }
    Instance* h = btFirst(runner, bt.names->obj[O_HEART]);
    if (h == nullptr) return;
    int32_t movement = btSelf(h, S_MOVEMENT, 0);
    uint8_t kind = movement == 1 ? (btSelfX(h, X_SHOT, 0) == 1 ? TOUCH_SOUL_YELLOW : TOUCH_SOUL_RED)
                  : movement == 2 ? TOUCH_SOUL_BLUE : TOUCH_SOUL_NONE;
    if (movement == 3 && btFirst(runner, BTX->obj2[P_SHIELD]) != nullptr) kind = TOUCH_SOUL_GREEN;
    if (kind == TOUCH_SOUL_NONE || btSelf(h, S_CONFUSE, 0) != 0 || !btCentre(runner, h, bt.names->obj[O_HEART], m)) return;
    m->soul = kind;
    m->soulStep = btSixteenths(btGlobal(runner->vmContext, G_SP));
}

// ===[ Appended: the extension hook (ctr_touch_ext.h; touchui's taps outside battle, 11-08) ]===

#include "ctr_touch_ext.h"

// No extension: nothing claims, nothing changes (touchui's strong definition replaces this one).
__attribute__((weak)) BT_CODE2 bool CtrTouchExt_sample(const CtrTouchExtIn* in, uint32_t* pulse, uint32_t* hold) {
    (void) in;
    (void) pulse;
    (void) hold;
    return false;
}

// Split's two-piece bottom screen (battlesplit, placement (c), maintainer's default 11-08): rows of the bottom layer above
// the seam (layer row 196: the box and the arena) are shown lifted by L screen px, the HP line and the buttons below
// it stay where they were, and screen rows seamY - L .. seamY - 1 are a black gap. true only when the bottom is two
// pieces this frame (Split shown, (c), L > 0; L can drop to 0 mid-battle, so it is asked every frame), with seamY and
// L in screen px. battlesplit's strong definition replaces this one: one piece.
bool CtrLayout_bottomSeam(int32_t* seamY, int32_t* lift);
__attribute__((weak)) BT_CODE2 bool CtrLayout_bottomSeam(int32_t* seamY, int32_t* lift) {
    (void) seamY;
    (void) lift;
    return false;
}

CtrInputEdges Battletouch_take2(const CtrInputMap* map, uint32_t held, uint32_t down, bool enabled, bool suppressHeld,
                                CtrInputStats* stats);

// ===[ The merge (Battletouch_take2: the CtrInput_take wrap's body, appended) ]===

#define BT_NOWHERE (-0x4000) // room px: a pen on no target (far above every room)

typedef struct {
    const Runner* runner;
    const Room* room;
    uint32_t controller;     // the battle controller's instance id, 0: none
} BtScope;

// The stylus's state. It lives on the heap, allocated the first time the stylus is on: Old's late data reserve is
// spent (11-08), and with the option off nothing is allocated at all.
typedef struct BtLive {
    uint32_t ePrev, tPrev;   // the buttons' and the stylus's held keys at the end of the last frame
    TouchState s;
    int16_t px, py;          // where the pen last was (bottom-screen px): a lift is where the pen left the screen
    uint32_t apt, aptSeen;   // HOME, sleep and the lid, counted by btApt; the count at the last sample
    bool away;               // between a suspend or sleep and its restore or wake
    aptHookCookie cookie;
    BtScope scope;           // where the pen's press began: runner, room, battle controller
    BtScope extScope;        // the extension's (ctr_touch_ext.h): the scope and data file at the last sample
    const void* extData;
    void* ext;               // the extension's own state (CtrTouchExtIn.state)
    // Appended (the slot's code reads the fields above): the pen's last samples, for the lift (Touch_pen).
    TouchPen pen;
    // The pen log (btPenLog): the pen's samples since the last flush, in a ring (when full, the oldest go first,
    // counted). Present in every build, so take2 is the same code in test and public builds; only test builds fill it.
    uint32_t logAt, logLast;   // the frame of the last flush, of the last sample
    uint16_t logHead, logCount, logLost;  // the oldest sample's index, the samples held, the samples lost
    uint32_t log[48];          // per sample: the frame's low 24 bits, then the kind (btPenLog)
    int16_t logX[48], logY[48];
    // Appended: the first-battle hint (btHint). The GSP thread's paint reads where, level, len and y.
    const Room* hintRoom;      // the battle's room while the hint is up, or seen and not yet written
    uint16_t hintFrames;       // game frames it has been up
    uint8_t hintWhere;         // BT_HINT_*: where it is up (0: not up)
    uint8_t hintLevel;         // its brightness, 255 down to 0 in the fade
    uint8_t hintLen;           // characters shown (the full line, or "Tap menus." with Menus)
    uint8_t hintY;             // the game screen's row it took (BT_HINT_Y, or BT_HINT_Y2 below a tall band)
    bool hintWrite;            // seen, and settings.ini not written yet (at the battle's end)
    bool hintLit;              // the hint lit the top screen (Other screen Off), to be put out at its end
    // Appended: the menus (btMenuFix). The GSP thread's paint reads menuLit and menuRect.
    uint32_t deadId;           // the menu when a let-go stroke was first seen (a new one under the pen picks it up)
    bool deadSeen;             // deadId is set for this let-go stroke
    uint8_t menuLit;           // 1: a button press in a submenu lights menuRect[0] and puts out menuRect[1]
    uint16_t menuRect[2][4];   // bottom-screen px, x0 y0 x1 y1: the button under the pen, the open one
    uint8_t tap;               // a text tap for this take (btTake): 1 press tapKey in place of the core's Z
    uint8_t tapKey;            // the tap's key: X (the text still typing) or Z
    bool tapZone;              // the tap is the skip zone's (btZone): the arrows held don't stop it
    bool zone;                 // this stroke went down in the skip zone and has stayed there (btMenuFix)
    // Appended: the relative touchpad (btPad).
    float padX0, padY0;        // this stroke's view corner (room px): the pen-down's place maps to the SOUL's centre
    bool padDown;              // a pad stroke is under way (last frame's pen was down in a pad defence)
    bool padGreen;             // the corner is kept relative to the SOUL (green: padX0/Y0 minus the SOUL's centre)
    // Appended: the menus' state belongs to the core's stroke (c26rev8 BT7-1, BT7-2; btForget).
    uint32_t takes, fixAt;     // btTake's takes so far; the take btMenuFix last ran in
    BtScope fixScope;          // take2's scope and the data file when btMenuFix last ran
    const void* fixData;
    TouchState sPrev;          // the core's state as btMenuFix found it on the last pen-down frame
    bool fixFlip;              // btMenuFix changed the press on that frame (a button, the rows, a picked-up stroke)
    bool fixWait;              // that frame's newest sample waited (Touch_pen's jump): the core used the one before
    int8_t back;               // at the buttons, a lit stroke released off them: 1 + the cursor to put back (btTake)
} BtLive;

// The pen log (lead 11-08): test builds write the pen's samples to the log as one [pen] line a while after the pen
// lifts, at most every BT_PEN_EVERY frames, so a player's log carries the console's samples. Public builds compile
// btPenLog empty (CMakeLists.txt defines CTRPORT_PEN_LOG only without the release source URL); its calls in take2 are
// the same in both, so test builds keep every late address of the public build up to the third pair.
// Kinds: d the pen-down, m a sample, w a sample that waited (Touch_pen's jump), u the lift on the newest sample, U the
// lift gone back to the one before (the place logged is the one used); z after u or U: the release confirmed.
// After each d (lead 11-10), where it went down, as the core took it this frame: "(c<ctx> m<mode> <class> w<text>)",
// ctx and mode the core's (TOUCH_CTX_*, TOUCH_*), class z the skip zone (btZone), b a button press, else by the box
// (idealborder): i inside, r within BT_PEN_RING px of it (the ring, where the pen steers), o beyond; w1 with the
// battle's text up (a writer). Then "box<x0>,<y0>-<x1>,<y1>", the box in bottom-screen px, when no box is held in the
// line yet or it changed. The pen's places are bottom-screen px as before.
#define BT_PEN_LOG_FN __attribute__((noinline, noipa, section(".text_battletouch3")))
#ifdef CTRPORT_PEN_LOG
#define BT_PEN_N 48
#define BT_PEN_EVERY 150   // frames between flushes (5 s at 30 fps, the [perf] lines' period)
#define BT_PEN_QUIET 15    // frames after the last sample before a flush (never on the lift's frame or the next)
static const char btPenKinds[] __attribute__((section(".rodata_battletouchpen"))) = "dmwuU";
static const char btPenFormat[] __attribute__((section(".rodata_battletouchpen"))) = "[pen] %s\n";

static __attribute__((noinline, section(".text_battletouchpen"))) char* btPenNum(char* o, int32_t v) {
    char d[12];
    int32_t n = 0;
    uint32_t u = 0 > v ? (uint32_t) -v : (uint32_t) v;
    do d[n++] = (char) ('0' + u % 10u), u /= 10u; while (u != 0u);
    if (0 > v) *o++ = '-';
    while (n > 0) *o++ = d[--n];
    return o;
}

// One sample (kind: an index into btPenKinds, plus 8 when the release confirmed), or (kind -1, no pen this frame) a
// flush when it is time. take2 calls it only with the pen down or lifting, or with samples held: with no pen down and
// nothing held, the cost is one compare.
static __attribute__((noinline, noipa, section(".text_battletouchpen"))) void btPenLogBody(BtLive* L, uint32_t frame, int32_t kind, int32_t x, int32_t y) {
    if (kind >= 0) {
        uint32_t i = (L->logHead + L->logCount) % BT_PEN_N;
        if (L->logCount == BT_PEN_N) {
            L->logHead = (uint16_t) ((L->logHead + 1u) % BT_PEN_N);
            L->logLost++;
        } else {
            L->logCount++;
        }
        L->log[i] = (frame & 0xffffffu) | (uint32_t) kind << 24;
        L->logX[i] = (int16_t) x;
        L->logY[i] = (int16_t) y;
        L->logLast = frame;
        return;
    }
    if (L->logCount == 0 || BT_PEN_QUIET > frame - L->logLast || BT_PEN_EVERY > frame - L->logAt) return;
    // "f=<frame> lost=<n>" then per sample "<kind><x>,<y>", "@<frame>" before one that doesn't follow the last.
    char line[BT_PEN_N * 24 + 32];
    char* o = line;
    *o++ = 'f', *o++ = '=';
    uint32_t last = L->log[L->logHead] & 0xffffffu;
    o = btPenNum(o, (int32_t) last);
    *o++ = ' ', *o++ = 'l', *o++ = 'o', *o++ = 's', *o++ = 't', *o++ = '=';
    o = btPenNum(o, L->logLost);
    for (uint32_t n = 0; L->logCount > n; n++) {
        uint32_t i = (L->logHead + n) % BT_PEN_N;
        uint32_t f = L->log[i] & 0xffffffu, k = L->log[i] >> 24;
        if (n > 0 && f != ((last + 1u) & 0xffffffu)) {
            *o++ = ' ', *o++ = '@';
            o = btPenNum(o, (int32_t) f);
        }
        last = f;
        *o++ = ' ';
        *o++ = btPenKinds[k & 7u];
        o = btPenNum(o, L->logX[i]);
        *o++ = ',';
        o = btPenNum(o, L->logY[i]);
        if ((k & 8u) != 0u) *o++ = 'z';
    }
    *o = 0;
    logInfo(btPenFormat, line);
    L->logHead = L->logCount = L->logLost = 0;
    L->logAt = frame;
}
static BT_PEN_LOG_FN void btPenLog(BtLive* L, uint32_t frame, int32_t kind, int32_t x, int32_t y) { btPenLogBody(L, frame, kind, x, y); }
// The context (lead 11-10) is all in its own pair, .text_battletouchpen and .rodata_battletouchpen, empty in public
// builds. Since devdiet (lead 11-11) test builds put that pair in the AAC slots' dev-only inputs (aacout.cmake), with
// btPenLog's body, btPenNum and their strings: btPenLog above keeps the public build's 4 B, a branch to its body.
// take2's two calls go to btPenNote instead (BT_PEN_CALL, the same call to a function of the same type), which
// appends through btPenLog and flushes with btPenFlush, btPenLog's flush plus the context; btPenLog's own flush is
// left in place, unused.
#define BT_PEN_WHERE 5 // the kinds after a d: where it went down, then the box's two corners
#define BT_PEN_BOX0 6
#define BT_PEN_BOX1 7
#define BT_PEN_RING 16 // BT_ZONE: the ring around the box where the pen steers (the skip zone is beyond it)
#define BT_PEN_CODE __attribute__((noinline, section(".text_battletouchpen")))
#define BT_PEN_RO __attribute__((section(".rodata_battletouchpen")))
#define BT_PEN_NOTE_FN __attribute__((noinline, noipa, section(".text_battletouchpen")))
#define BT_PEN_CALL btPenNote
static const char btPenClass[] BT_PEN_RO = "zbiro";

// Where the pen-down went: one sample (x: ctx | mode << 2 | class << 5 | text << 8), then the box's corners if no box
// is held or the last one held differs.
static BT_PEN_CODE void btPenWhere(BtLive* L, uint32_t frame) {
    Runner* runner = bt.runner;
    CtrBottomView v;
    int32_t b[4] = {0, 0, 0, 0}, cls = 4, text = 0;
    if (runner != nullptr && bt.names != nullptr && CtrLayout_bottomView(runner, &v) && v.roomPerPx > 0.0f) {
        VMContext* ctx = runner->vmContext;
        for (int32_t k = 0; 4 > k; k++)
            b[k] = (int32_t) (((float) btGet(ctx, G_IDEALBORDER, k, 0) - (2 > k ? v.roomX0 : v.roomY0)) / v.roomPerPx);
        text = btFirst(runner, bt.names->obj[O_WRITER]) != nullptr;
        int32_t g = BT_PEN_RING, x = L->px, y = L->py;
        cls = b[0] <= x && b[1] > x && b[2] <= y && b[3] > y               ? 2
              : b[0] - g <= x && b[1] + g > x && b[2] - g <= y && b[3] + g > y ? 3
                                                                             : 4;
    }
    if (L->zone) cls = 0;
    else if (L->s.mode == TOUCH_PRESS || L->s.mode == TOUCH_BUTTON) cls = 1;
    btPenLog(L, frame, BT_PEN_WHERE, (int32_t) L->s.ctx | (int32_t) L->s.mode << 2 | cls << 5 | text << 8, 0);
    for (uint32_t n = L->logCount; n-- > 0;) {
        uint32_t i = (L->logHead + n) % BT_PEN_N;
        if (L->log[i] >> 24 != BT_PEN_BOX1) continue;
        uint32_t j = (i + BT_PEN_N - 1u) % BT_PEN_N;
        if (n > 0 && L->logX[j] == b[0] && L->logY[j] == b[2] && L->logX[i] == b[1] && L->logY[i] == b[3]) return;
        break;
    }
    btPenLog(L, frame, BT_PEN_BOX0, b[0], b[2]);
    btPenLog(L, frame, BT_PEN_BOX1, b[1], b[3]);
}

// A context sample (kind k, BT_PEN_WHERE to BT_PEN_BOX1) at o: " (c<ctx> m<mode> <class> w<text>)", " box<x>,<y>" or
// "-<x>,<y>".
static BT_PEN_CODE char* btPenContext(char* o, uint32_t k, int32_t w, int32_t y) {
    if (k == BT_PEN_WHERE) {
        *o++ = ' ', *o++ = '(', *o++ = 'c';
        o = btPenNum(o, w & 3);
        *o++ = ' ', *o++ = 'm';
        o = btPenNum(o, w >> 2 & 7);
        *o++ = ' ', *o++ = btPenClass[w >> 5 & 7], *o++ = ' ', *o++ = 'w';
        o = btPenNum(o, w >> 8 & 1);
        *o++ = ')';
        return o;
    }
    if (k == BT_PEN_BOX0) *o++ = ' ', *o++ = 'b', *o++ = 'o', *o++ = 'x';
    else *o++ = '-';
    o = btPenNum(o, w);
    *o++ = ',';
    return btPenNum(o, y);
}

// btPenLog's flush (the same line), with the context samples printed by btPenContext and kept out of the @ bookkeeping.
static BT_PEN_CODE void btPenFlush(BtLive* L, uint32_t frame) {
    if (L->logCount == 0 || BT_PEN_QUIET > frame - L->logLast || BT_PEN_EVERY > frame - L->logAt) return;
    char line[BT_PEN_N * 24 + 32];
    char* o = line;
    *o++ = 'f', *o++ = '=';
    uint32_t last = L->log[L->logHead] & 0xffffffu;
    o = btPenNum(o, (int32_t) last);
    *o++ = ' ', *o++ = 'l', *o++ = 'o', *o++ = 's', *o++ = 't', *o++ = '=';
    o = btPenNum(o, L->logLost);
    for (uint32_t n = 0; L->logCount > n; n++) {
        uint32_t i = (L->logHead + n) % BT_PEN_N;
        uint32_t f = L->log[i] & 0xffffffu, k = L->log[i] >> 24;
        if (k >= BT_PEN_WHERE && (k & 8u) == 0u) { // the context after a d (u and U with z are 11 and 12)
            o = btPenContext(o, k, L->logX[i], L->logY[i]);
            continue;
        }
        if (n > 0 && f != ((last + 1u) & 0xffffffu)) {
            *o++ = ' ', *o++ = '@';
            o = btPenNum(o, (int32_t) f);
        }
        last = f;
        *o++ = ' ';
        *o++ = btPenKinds[k & 7u];
        o = btPenNum(o, L->logX[i]);
        *o++ = ',';
        o = btPenNum(o, L->logY[i]);
        if ((k & 8u) != 0u) *o++ = 'z';
    }
    *o = 0;
    logInfo(btPenFormat, line);
    L->logHead = L->logCount = L->logLost = 0;
    L->logAt = frame;
}

// take2's pen log call in test builds: btPenLog's sample, then the context after a d; or the flush.
static BT_PEN_NOTE_FN void btPenNote(BtLive* L, uint32_t frame, int32_t kind, int32_t x, int32_t y) {
    if (0 > kind) {
        btPenFlush(L, frame);
        return;
    }
    btPenLog(L, frame, kind, x, y);
    if (kind == 0) btPenWhere(L, frame);
}
#else
#define BT_PEN_CALL btPenLog
static BT_PEN_LOG_FN void btPenLog(BtLive* L, uint32_t frame, int32_t kind, int32_t x, int32_t y) {
    (void) L, (void) frame, (void) kind, (void) x, (void) y;
}
#endif

// HOME, sleep and the lid (an APT hook, as ctr_input.c's own): counted, and "away" until the restore or wake. It can
// run on another thread than the samples, hence the atomics.
static BT_CODE void btApt(APT_HookType hook, void* param) {
    BtLive* L = param;
    if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP) __atomic_store_n(&L->away, true, __ATOMIC_RELEASE);
    else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP) __atomic_store_n(&L->away, false, __ATOMIC_RELEASE);
    else return;
    __atomic_add_fetch(&L->apt, 1u, __ATOMIC_ACQ_REL);
}

// Where a press is: a new runner, room or battle controller is somewhere else.
static BT_CODE BtScope btScope(Runner* runner) {
    Instance* bc = btFirst(runner, bt.names->obj[O_BC]);
    return (BtScope) {runner, runner->currentRoom, bc != nullptr ? bc->instanceId : 0u};
}

// The pen lets go of whatever it was doing; a pen still down does nothing until it is lifted (Touch_reset).
static BT_CODE void btLetGo(BtLive* L, bool pen) {
    L->s.pen = pen;
    Touch_reset(&L->s);
}

bool Battletouch_view(Runner* runner, CtrBottomView* out); // the view, and the menus first (the file's end)
BT_CODE2 CtrInputEdges Battletouch_take2(const CtrInputMap* map, uint32_t held, uint32_t down, bool enabled, bool suppressHeld,
                                         CtrInputStats* stats) {
    CtrInputEdges e = __real_CtrInput_take(map, held, down, enabled, suppressHeld, stats);
    Runner* runner = bt.runner;
    if (runner == nullptr || btMode() == 0) return e;
    BtNames* names = bt.names;
    if (names == nullptr) {
        names = calloc(1, sizeof(BtNamesX));
        if (names == nullptr) return e;
        bt.names = names;
    }
    if (names->namesFor != runner->dataWin) {
        names->namesFor = runner->dataWin;
        names->named = btNames_find(runner->dataWin) && btNames_find2(runner->dataWin);
    }
    if (!names->named) return e;
    BtLive* L = bt.live;
    if (L == nullptr) {
        // No memory: the stylus stays off and the buttons work alone; the next frame tries again.
        L = calloc(1, sizeof(BtLive));
        if (L == nullptr) return e;
        bt.live = L;
        aptHook(&L->cookie, btApt, L);
    }
    bool pen = (held & KEY_TOUCH) != 0;
    bool on = enabled;
#ifdef CTRPORT_TOUCH_TEST
    if (btScriptPenNow(runner, &pen, &L->px, &L->py)) {
        on = true;
        uint32_t k = bt.keys, was = L->ePrev;
        e = (CtrInputEdges) {e.held | k, e.pressed | (k & ~was), e.released | (was & ~k)};
    } else
#endif
    if (pen) {
        // Read only while touched (the panel reports 0, 0 when not); a lift keeps the last place.
        touchPosition t;
        hidTouchRead(&t);
        L->px = (int16_t) t.px;
        L->py = (int16_t) t.py;
    }
    // The menus and the SOUL see the pen through its last samples (the lift, 11-08, Touch_pen); the
    // extension sees the newest.
    int32_t px = 0, py = 0;
    bool penWas = L->pen.pen;
    bool back = Touch_pen(&L->pen, pen, L->px, L->py, &px, &py);
    TouchOut out = {.write = -1};
    TouchMenu m = {0};
    int32_t slot = -1;
    // The same resets as the buttons': Options, a test or playback (enabled false), the buttons' own release-to-arm
    // (suppressHeld: a menu closed, a notice dismissed), and HOME, sleep or the lid, at either end.
    uint32_t apt = __atomic_load_n(&L->apt, __ATOMIC_ACQUIRE);
    bool away = __atomic_load_n(&L->away, __ATOMIC_ACQUIRE);
    bool cut = !on || suppressHeld || away || apt != L->aptSeen;
    L->aptSeen = apt;
    // The extension (ctr_touch_ext.h) sees every sample and may claim one with no battle controller: the claim's keys
    // are the stylus's this frame, and the battle's press lets go. A battle owns the pen whatever is claimed.
    BtScope scope = btScope(runner);
    CtrTouchExtIn xin = {.runner = runner, .pen = pen, .battle = scope.controller != 0u, .px = L->px, .py = L->py,
                         .held = e.held, .pressed = e.pressed, .state = &L->ext};
    xin.reset = cut || runner->dataWin != L->extData || scope.runner != L->extScope.runner
                || scope.room != L->extScope.room || scope.controller != L->extScope.controller;
    L->extData = runner->dataWin;
    L->extScope = scope;
    uint32_t xPulse = 0, xHold = 0;
    // Nothing claimed is merged on a reset sample (ctr_touch_ext.h): every reset, not only the input cuts.
    bool claimed = CtrTouchExt_sample(&xin, &xPulse, &xHold) && !xin.battle && !xin.reset;
    if (cut || claimed) btLetGo(L, pen);
    if (claimed) {
        out.pulse = xPulse;
        out.hold = xHold;
    } else if (!cut && (pen || L->s.pen || L->s.mode != TOUCH_IDLE || BT_SCRIPTED)) {
        // A press begun under another runner, room or battle controller is let go too (a battle's end, a game
        // over, a room restart): the menu it was in is gone even when the next one looks the same.
        if ((L->s.pen || L->s.mode != TOUCH_IDLE)
            && (scope.runner != L->scope.runner || scope.room != L->scope.room || scope.controller != L->scope.controller))
            btLetGo(L, pen);
        L->scope = scope;
        CtrBottomView view = {0.0f, 0.0f, 0.0f};
        bool shown = Battletouch_view(runner, &view);
#ifdef CTRPORT_TOUCH_TEST
        if (!shown && BT_SCRIPTED) shown = btTestView(runner, &view);
#endif
        if (shown) btRead_menu(runner, &m, &slot);
        // The slot's reader knows red only: read every SOUL kind again.
        if (m.ctx == TOUCH_CTX_DEFENCE && btMode() == 2) {
            m.soul = TOUCH_SOUL_NONE;
            btRead_soul2(runner, &m);
        }
        int32_t x = (int32_t) (view.roomX0 + ((float) px + 0.5f) * view.roomPerPx);
        int32_t y = (int32_t) (view.roomY0 + ((float) py + 0.5f) * view.roomPerPx);
        // Two pieces (CtrLayout_bottomSeam): a menu's pen below the seam is on the unlifted rows (the HP line and the
        // buttons), and in the gap it is on nothing. A defence keeps the box's mapping everywhere, so the SOUL's
        // target never jumps back up as the pen crosses the gap; a tap anywhere is a tap anyway.
        int32_t seamY = 0, lift = 0;
        if (shown && m.ctx == TOUCH_CTX_MENU && CtrLayout_bottomSeam(&seamY, &lift) && lift > 0 && py >= seamY - lift) {
            y = py >= seamY ? (int32_t) (view.roomY0 + ((float) (py - lift) + 0.5f) * view.roomPerPx) : BT_NOWHERE;
        }
        Touch_stepAt(&L->s, pen, x, y, back, &m, e.pressed, e.held, &out);
#ifdef CTRPORT_TOUCH_TEST
        if (pen || L->s.pen || L->s.mode != TOUCH_IDLE || out.pulse != 0)
            logInfo(btScriptLog, (unsigned long) bt.frame, (int) pen, (int) x, (int) y, (int) m.ctx, (unsigned long) m.id,
                    (int) m.cursor, (int) L->s.mode, (int) out.write, (int) out.sound, (unsigned long) out.pulse);
#endif
        // maintainer 11-08 (playtest1): "we dont need a sound effect for the hover, just the confirm to keep it consistent
        // with the game": the cursor moves silently on the pen-down, and the game's own confirm on the release is the
        // one sound. The core's sound flag is still logged in test builds.
        // The false goes through an empty asm so the compiler can't fold it into btWrite (a slot function, whose
        // bytes stay as built: its sound path is kept, only never taken).
        bool sound = false;
        __asm__("" : "+r"(sound));
        if (out.write >= 0 && slot >= 0) btWrite(runner, slot, out.write, sound);
    }
    // The pen log (test builds; empty in public ones): the raw sample while the pen is down; at the lift, the place
    // used and whether it confirmed.
    if (pen || penWas) {
        int32_t kind = !pen ? (back ? 4 : 3) + ((out.pulse & TOUCH_KEY_Z) != 0 ? 8 : 0) : !penWas ? 0 : back ? 2 : 1;
        BT_PEN_CALL(L, (uint32_t) runner->frameCount, kind, pen ? L->px : px, pen ? L->py : py);
    } else if (L->logCount != 0) {
        BT_PEN_CALL(L, (uint32_t) runner->frameCount, -1, 0, 0);
    }
    TouchEdges t = Touch_edges(out.pulse, out.hold, L->tPrev);
    TouchEdges r = Touch_merge((TouchEdges) {e.held, e.pressed, e.released}, t, L->ePrev, L->tPrev);
    L->ePrev = e.held;
    L->tPrev = t.held;
#ifdef CTRPORT_TOUCH_TEST
    if (BT_SCRIPTED && (m.id != bt.lastId || r.pressed != 0)) {
        bt.lastId = m.id;
        logInfo(btScriptGame, (unsigned long) bt.frame, (unsigned long) m.id, (int) m.cursor, (int) slot, (unsigned long) r.held,
                (unsigned long) r.pressed);
    }
    if (BT_SCRIPTED && (r.held != bt.lastHeld || m.soul != 0)) {
        bt.lastHeld = r.held;
        logInfo(btScriptHeld, (unsigned long) bt.frame, (unsigned long) r.held, (int) m.soul, (long) m.soulX, (long) m.soulY,
                (long) m.soulStep);
    }
#endif
    return (CtrInputEdges) {r.held, r.pressed, r.released};
}

// ===[ The slot's CtrInput_take wrap ]===

// The slot's CtrInput_take wrap: a branch to btTake, then Battletouch_take2 (same arguments, same return), padded to
// 021a570b8's size (0x8ac B, Old and New alike), so every symbol after it keeps its address. check_layout against
// 021a570b8's ELF proves the size.
#define BT_UNUSED __attribute__((unused))
__attribute__((naked, noinline, section(".text.battletouch_late")))
CtrInputEdges __wrap_CtrInput_take(BT_UNUSED const CtrInputMap* map, BT_UNUSED uint32_t held, BT_UNUSED uint32_t down,
                                   BT_UNUSED bool enabled, BT_UNUSED bool suppressHeld, BT_UNUSED CtrInputStats* stats) {
    __asm__("b Battletouch_take\n\t.space 0x8a8\n"); // btTake (the file's end), then Battletouch_take2
}

// ===[ Options: the Controls page's Battles row, and the Battles page (Layout, Stylus, Back) ]===
//
// The options menu's main page has no free row (New uses all 11), so the Battles page opens from the Controls page
// (ctr_ctrlui.c, 11-08). That page is frozen late code, so nothing in it changes: optmenu's calls to
// CtrCtrlui_main, CtrCtrlui_update and CtrCtrlui_holdMoves come here first (link wraps, UNDERTALE only).
//
// - The Controls page: rows 0-11 are the buttons, as before; then Battles, Layout and Back (y 176, 189 and 202, 13 px
//   apart instead of 14, so Back's ink stays above the help line's rule at y 214). Its own code still draws
//   everything, Layout and Back at their old places; after each of its draws this clears y 175-213 and draws the
//   three rows again, and the help line when one of them has the cursor. Its cursor runs 0-13; ours runs 0-14 and
//   maps onto it (Battles and Layout are both its Layout row): a move between rows it tells apart goes to it, so its
//   draw and sound happen as before; a move between Battles and Layout is ours alone.
// - The Battles page: Layout (battlesplit's, through CtrBattlesplit_setting and CtrBattlesplit_setSetting), Stylus
//   (selector order: Off, Menus and SOUL, Menus) and Back, drawn as optmenu draws a page. B or Back returns to the Controls page with
//   the cursor on Battles: its page is drawn again by a Down and an Up on its Layout row (it has no other way to
//   draw), which plays the one sound of the press.
// Layout's value text, help line and Default / B confirms / Custom test are copies of ctr_ctrlui.c's (static there).

enum { BM_BATTLES = MAP_ROW_COUNT, BM_LAYOUT, BM_BACK, BM_ROWS };
enum { BP_LAYOUT, BP_STYLUS, BP_BACK, BP_ROWS };
enum { BT_BATTLES, BT_LAYOUT, BT_BACK, BT_STYLUS, BT_DEFAULT, BT_BCONFIRMS, BT_CUSTOM, BT_ORIGINAL, BT_SPLIT, BT_OFF,
       BT_MENUS, BT_SOUL, BT_NEEDS_TOP, BT_HLP_BATTLES, BT_HLP_LAYOUT, BT_HLP_BACK, BT_HLP_ORIGINAL, BT_HLP_SPLIT,
       BT_HLP_OFF, BT_HLP_MENUS, BT_HLP_SOUL, BT_HLP_BACK2, BT_HEART };
// NUL-separated, in the order above.
static const char bmText[] BT_RO = "Battles\0Layout\0Back\0Stylus\0"
                                   "< Default >\0< B confirms >\0< Custom >\0< Original >\0< Split >\0< Off >\0< Menus >\0"
                                   "< Menus and SOUL >\0"
                                   "Needs the game on top.\0"
                                   "A to set the stylus and battle layout.\0"
                                   "Left or right: change the layout.\0"
                                   "Back to the options.\0"
                                   "The whole battle on one screen.\0"
                                   "Enemies on top. Box, HP and buttons below.\0"
                                   "Battles use the buttons only.\0"
                                   "Tap the battle buttons and menus.\0"
                                   "Tap the menus and drag the SOUL.\0"
                                   "Back to the controls.\0"
                                   "\x01";
// CtrInputMap_defaults' map (ctr_ctrlui.c's ctlDefault).
static const uint8_t bmDefault[MAP_ROW_COUNT] BT_RO = { GAME_KEY_Z, GAME_KEY_X, GAME_KEY_C, GAME_KEY_C, 0, 0, 0, 0,
                                                        GAME_KEY_ENTER, GAME_KEY_ARROWS, GAME_KEY_ARROWS, 0 };
static const uint8_t bmPageY[BP_ROWS] BT_RO = { 8, 22, 42 };

static struct {
    uint8_t ctlRow;  // the Controls page's cursor, 0..BM_ROWS-1
    uint8_t pageRow; // the Battles page's cursor
    bool onPage;     // the Battles page is up
    bool isNew;      // a New 3DS: ZL, ZR and the C-stick rows are usable
} btm BT_STATE;

// battlesplit's Layout setting (its strong definitions replace these where it is linked).
__attribute__((weak)) BT_CODE int32_t CtrBattlesplit_setting(void) {
    return 0;
}
__attribute__((weak)) BT_CODE void CtrBattlesplit_setSetting(int32_t value) {
    (void) value;
}

static BT_CODE const char* bmNth(uint32_t i) {
    const char* s = bmText;
    for (; i != 0; i--) s += strlen(s) + 1;
    return s;
}

static BT_CODE bool bmGrey(uint32_t row) {
    return !btm.isNew && (row == MAP_ROW_ZL || row == MAP_ROW_ZR || row == MAP_ROW_CSTICK);
}

// The Controls page's own row for one of ours.
static BT_CODE uint32_t bmTheirs(uint32_t row) {
    return row > BM_BATTLES ? row - 1u : row;
}

// Screen rows y0..y1 of the page, inside the frame, to black (column-major, row 0 at the top as optText draws).
static BT_CODE void bmClear(uint16_t* fb, int32_t y0, int32_t y1) {
    for (uint32_t x = 2; 318u > x; x++) memset(fb + x * 240u + (uint32_t) (239 - y1), 0, (size_t) (y1 - y0 + 1) * 2u);
}

static BT_CODE void bmRow(uint16_t* fb, int32_t y, const char* label, const char* value, const char* help, bool sel,
                          bool grey) {
    uint32_t col = grey ? 0x8410u : (sel ? 0xFFE0u : 0xFFFFu);
    CtrOptmenu_text(fb, 24, y, label, col);
    CtrOptmenu_text(fb, 148, y, value, col);
    if (!sel) return;
    CtrOptmenu_text(fb, 11, y, bmNth(BT_HEART), 0xF800u);
    bmClear(fb, 215, 237);
    CtrOptmenu_text(fb, 12, 221, help, 0xFFFFu);
}

// 0 Default, 1 B confirms, 2 Custom (ctr_ctrlui.c's ctlLayout).
static BT_CODE uint32_t bmLayout(const uint8_t* k) {
    if (memcmp(k + 2, bmDefault + 2, MAP_ROW_COUNT - 2) != 0) return 2u;
    if (k[0] == GAME_KEY_Z && k[1] == GAME_KEY_X) return 0;
    return k[0] == GAME_KEY_X && k[1] == GAME_KEY_Z ? 1u : 2u;
}

// After the Controls page's draw: Battles, Layout and Back where they go now.
static BT_CODE void bmControlsTail(const CtrMenu* menu) {
    uint16_t* fb = (uint16_t*) menu->console->frameBuffer;
    uint32_t row = btm.ctlRow;
    bmClear(fb, 175, 213);
    bmRow(fb, 176, bmNth(BT_BATTLES), bmNth(BT_HEART) + 1, bmNth(BT_HLP_BATTLES), row == BM_BATTLES, false);
    bmRow(fb, 189, bmNth(BT_LAYOUT), bmNth(BT_DEFAULT + bmLayout(menu->settings.controls.keys)), bmNth(BT_HLP_LAYOUT),
          row == BM_LAYOUT, false);
    bmRow(fb, 202, bmNth(BT_BACK), bmNth(BT_HEART) + 1, bmNth(BT_HLP_BACK), row == BM_BACK, false);
}

static BT_CODE void bmPageDraw(const CtrMenu* menu) {
    uint16_t* p = (uint16_t*) menu->console->frameBuffer;
    uint16_t* fb = p;
    for (uint32_t x = 0; 320u > x; x++) {
        for (uint32_t y = 0; 240u > y; y++) *p++ = (x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? 0xFFFFu : 0u;
    }
    bool below = CtrScreens_game != 0, split = CtrBattlesplit_setting() != 0;
    uint32_t mode = (uint32_t) btMode();
    uint32_t row = btm.pageRow;
    bmRow(fb, bmPageY[BP_LAYOUT], bmNth(BT_LAYOUT), bmNth(below ? BT_NEEDS_TOP : (split ? BT_SPLIT : BT_ORIGINAL)),
          bmNth(below ? BT_NEEDS_TOP : (split ? BT_HLP_SPLIT : BT_HLP_ORIGINAL)), row == BP_LAYOUT, below);
    bmRow(fb, bmPageY[BP_STYLUS], bmNth(BT_STYLUS), bmNth(BT_OFF + mode), bmNth(BT_HLP_OFF + mode),
          row == BP_STYLUS, false);
    bmRow(fb, bmPageY[BP_BACK], bmNth(BT_BACK), bmNth(BT_HEART) + 1, bmNth(BT_HLP_BACK2), row == BP_BACK, false);
}

// The Battles page's keys. SELECT: back to the game; B or Back: the Controls page; left, right or A: change the row.
static BT_CODE CtrMenuAction bmPageKeys(CtrMenu* menu, u32 down) {
    if (down & KEY_SELECT) return MENU_ACTION_RESUME;
    u32 folded = CTR_MENU_FOLD(down);
    uint32_t row = btm.pageRow;
    int32_t dir = (folded & KEY_DRIGHT) ? 1 : ((folded & KEY_DLEFT) ? -1 : 0);
    if (!(down & KEY_B)) { // B first, then Up or Down, then the row's change, as on the Controls page
        if (folded & (KEY_DUP | KEY_DDOWN)) {
            btm.pageRow = (uint8_t) ((folded & KEY_DUP) ? (row == 0 ? BP_ROWS - 1u : row - 1u)
                                                        : (row + 1u == BP_ROWS ? 0u : row + 1u));
            CtrOptmenu_pressSound();
            bmPageDraw(menu);
            return MENU_ACTION_NONE;
        }
        if (dir == 0 && !(down & KEY_A)) return MENU_ACTION_NONE;
        if (row != BP_BACK) {
            if (row == BP_LAYOUT) {
                if (CtrScreens_game != 0) return MENU_ACTION_NONE; // grey: no change, no sound
                CtrBattlesplit_setSetting(CtrBattlesplit_setting() != 0 ? 0 : 1);
            } else {
                // Off, Menus and SOUL, Menus: Right and A forward, Left back; stored values stay 0, 2, 1.
                int32_t v = btMode() - (dir < 0 ? -1 : 1);
                bt.menuValue = (int8_t) (v > 2 ? 0 : (0 > v ? 2 : v));
            }
            menu->edited = true;
            CtrOptmenu_pressSound();
            bmPageDraw(menu);
            return MENU_ACTION_CHANGED;
        }
        if (dir != 0) return MENU_ACTION_NONE;
    }
    // Back: the Controls page, drawn by its own code (its cursor is on its Layout row: down, then up again).
    btm.onPage = false;
    btm.ctlRow = BM_BATTLES;
    __real_CtrCtrlui_update(menu, KEY_DDOWN);
    __real_CtrCtrlui_update(menu, KEY_DUP);
    bmControlsTail(menu);
    return MENU_ACTION_NONE;
}

// The main page's Controls row: A opens the Controls page with the cursor on its first row; left or right change its
// button layout (no page).
BT_CODE CtrMenuAction __wrap_CtrCtrlui_main(CtrMenu* menu, int32_t dir) {
    CtrMenuAction action = __real_CtrCtrlui_main(menu, dir);
    if (dir == 0 && menu->page == MENU_PAGE_CONTROLS) {
        APT_CheckNew3DS(&btm.isNew);
        btm.ctlRow = 0;
        btm.onPage = false;
        bmControlsTail(menu);
    }
    return action;
}

BT_CODE CtrMenuAction __wrap_CtrCtrlui_update(CtrMenu* menu, u32 down) {
    if (menu->page != MENU_PAGE_CONTROLS) return __real_CtrCtrlui_update(menu, down);
    if (btm.onPage) return bmPageKeys(menu, down);
    uint32_t row = btm.ctlRow;
    u32 folded = CTR_MENU_FOLD(down);
    if (!(down & (KEY_SELECT | KEY_B)) && (folded & (KEY_DUP | KEY_DDOWN))) {
        uint32_t to = row;
        do {
            if (folded & KEY_DUP) to = to == 0 ? BM_ROWS - 1u : to - 1u;
            else to = to + 1u == BM_ROWS ? 0u : to + 1u;
        } while (MAP_ROW_COUNT > to && bmGrey(to));
        btm.ctlRow = (uint8_t) to;
        if (bmTheirs(to) == bmTheirs(row)) { // Battles and Layout: ours alone
            CtrOptmenu_pressSound();
            bmControlsTail(menu);
            return MENU_ACTION_NONE;
        }
    } else if (row == BM_BATTLES && !(down & (KEY_SELECT | KEY_B))) {
        if ((folded & (KEY_DLEFT | KEY_DRIGHT)) || !(down & KEY_A)) return MENU_ACTION_NONE;
        btm.onPage = true;
        btm.pageRow = 0;
        CtrOptmenu_pressSound();
        bmPageDraw(menu);
        return MENU_ACTION_NONE;
    }
    CtrMenuAction action = __real_CtrCtrlui_update(menu, down);
    if (menu->page == MENU_PAGE_CONTROLS && action != MENU_ACTION_RESUME) bmControlsTail(menu);
    return action;
}

// A held Up or Down repeats while there is a row to go to (the list's end stops it, without a wrap).
BT_CODE bool __wrap_CtrCtrlui_holdMoves(u32 key) {
    if (!btm.onPage) return __real_CtrCtrlui_holdMoves(key);
    return btm.pageRow != (key == KEY_DUP ? 0u : BP_ROWS - 1u);
}

// ===[ Appended: Options, Battle layout and Battle stylus on the main page (Way B) ]===
//
// Ruling 11-08 (maintainer: nothing is final until he plays it, rows and places included): two rows on the options
// menu's main page, under Controls: Battle layout (battlesplit's Original or Split) and Battle stylus (battle_touch:
// selector order Off, Menus and SOUL, Menus). The Controls page is as it was before battletouch: CMakeLists.txt no longer wraps its
// three calls, so the first slot's Battles row and Battles page code stays in place, unreached.
//
// optmenu's main page is frozen late code with fixed rows, so its keys and its draw come here first: UNDERTALE's
// compiler launcher (tools/buildcache/builderx_compile.py) routes optKeysHeard's call of optKeys and optComposeShown's
// call of optCompose to CtrOptbattle_keys and CtrOptbattle_compose, and names the originals, optRow and the menu's
// state for them. Every other page, and every press on the main page's own rows but Up and Down, goes to the
// originals unchanged.
//
// The page is data (obOrder, obY): optmenu's rows by their index, ours as OB_LAYOUT and OB_STYLUS, so moving or
// relabelling a row is a table edit. While the cursor is on one of ours, optmenu's own cursor stays on Controls (so its
// held-key repeat, its press sound's mark, B and SELECT work as before) and obOn says which of ours it is.

// optmenu's OptRow, and the first fields of its OptState (ctr_optmenu.c checks both at its end).
typedef struct {
    const char* label;
    const char* value;
    const char* help;
    bool dis;
} ObRow;
typedef struct {
    uint32_t pointers[4]; // menu, spareBase, shownBase, restore (32-bit pointers)
    uint8_t page;   // 0: the main page
    uint8_t cur[4]; // each page's cursor; [0] the main page's
    uint8_t scr;
    uint8_t pref;
    bool placed;
    bool dirty;     // the page is drawn again at the frame's flush
} ObState;
_Static_assert(__builtin_offsetof(ObState, page) == 16 && __builtin_offsetof(ObState, cur) == 17 &&
               __builtin_offsetof(ObState, dirty) == 24 && (sizeof(void*) != 4 || __builtin_offsetof(ObRow, dis) == 12),
               "optmenu's layout");
extern ObState CtrOptbattle_state;
void CtrOptbattle_row(const CtrMenu* menu, uint32_t page, uint32_t i, ObRow* r);
CtrMenuAction CtrOptbattle_keysOriginal(CtrMenu* menu, u32 down);
void CtrOptbattle_composeOriginal(const CtrMenu* menu, uint16_t* fb);
CtrMenuAction CtrOptbattle_keys(CtrMenu* menu, u32 down);
void CtrOptbattle_compose(const CtrMenu* menu, uint16_t* fb);
bool CtrLegal_shown(void);

enum { OB_LAYOUT = 0x80, OB_STYLUS };
// The main page, top to bottom: optmenu's rows by index (OPT_MAIN_*: Game screen, Dialogue box, Smoothing, Exact
// colours (New only), FPS counter, Controls, Debug, Legal notices, Credits, Back to the game, Quit game), and ours.
// Each row's y: rows 14 px apart, blocks 19 px (New) or 20 px (Old), Back to the game and Quit game where they were.
#ifdef CTRPORT_TARGET_O3DS
enum { OB_CONTROLS = 4, OB_ROWS = 12 };
static const uint8_t obOrder[OB_ROWS] BT_RO3 = { 0, 1, 2, 3, 4, OB_LAYOUT, OB_STYLUS, 5, 6, 7, 8, 9 };
static const uint8_t obY[OB_ROWS] BT_RO3 = { 8, 22, 42, 56, 76, 90, 104, 124, 138, 152, 182, 196 };
#else
enum { OB_CONTROLS = 5, OB_ROWS = 13 };
static const uint8_t obOrder[OB_ROWS] BT_RO3 = { 0, 1, 2, 3, 4, 5, OB_LAYOUT, OB_STYLUS, 6, 7, 8, 9, 10 };
static const uint8_t obY[OB_ROWS] BT_RO3 = { 8, 22, 41, 55, 69, 88, 102, 116, 135, 149, 163, 182, 196 };
#endif
// Our rows' text, NUL-separated in this order (stylus values/help in stored battle_touch order, not selector order).
enum { OB_LBL_LAYOUT, OB_LBL_STYLUS, OB_NEEDS_TOP, OB_ORIGINAL, OB_SPLIT, OB_HLP_ORIGINAL, OB_HLP_SPLIT, OB_OFF,
       OB_MENUS, OB_SOUL, OB_HLP_OFF, OB_HLP_MENUS, OB_HLP_SOUL, OB_HEART };
static const char obText[] BT_RO3 = "Battle layout\0Battle stylus\0Needs the game on top.\0< Original >\0< Split >\0"
                                    "The whole battle on one screen.\0Enemies on top. Box, HP and buttons below.\0"
                                    "< Off >\0< Menus >\0< Menus and SOUL >\0"
                                    "Battles use the buttons only.\0Tap the battle buttons and menus.\0"
                                    "Tap menus. The SOUL follows the stylus.\0\x01";
// Which of ours has the cursor (0: none, 1 Battle layout, 2 Battle stylus): the first slot's Battles page cursor,
// unused since its page went (late data stays 20 B).
#define obOn btm.pageRow

static BT_CODE3 const char* obNth(uint32_t i) {
    const char* s = obText;
    for (; i != 0; i--) s += strlen(s) + 1;
    return s;
}

// Row k of the page (an index into obOrder). Its help lines and the Battle layout row's value come through
// Battletouch_obHelp18 and Battletouch_obValue18 (battletouch18, the file's end: the same calls as before, other names).
const char* Battletouch_obHelp18(uint32_t i);
const char* Battletouch_obValue18(uint32_t i);
static BT_CODE3 void obRow(const CtrMenu* menu, uint32_t k, ObRow* r) {
    uint32_t id = obOrder[k];
    if (OB_LAYOUT > id) {
        { void CtrLayout_optRow(const void*, uint32_t, uint32_t, void*); CtrLayout_optRow(menu, 0, id, r); } // layout's note
        return;
    }
    bool below = Battletouch_zero18 != 0;
    r->dis = false;
    if (id == OB_LAYOUT) {
        uint32_t split = CtrBattlesplit_setting() != 0;
        r->label = obNth(OB_LBL_LAYOUT);
        r->dis = below;
        r->value = Battletouch_obValue18(below ? OB_NEEDS_TOP : OB_ORIGINAL + split);
        r->help = Battletouch_obHelp18(below ? OB_NEEDS_TOP : OB_HLP_ORIGINAL + split);
        return;
    }
    uint32_t mode = (uint32_t) btMode();
    r->label = obNth(OB_LBL_STYLUS);
    r->value = obNth(OB_OFF + mode);
    r->help = Battletouch_obHelp18(OB_HLP_OFF + mode);
}

// The cursor's index in obOrder.
static BT_CODE3 uint32_t obCursor(void) {
    uint32_t cur = CtrOptbattle_state.cur[0];
    if (cur != OB_CONTROLS) obOn = 0;
    uint32_t id = obOn != 0 ? OB_LAYOUT - 1u + obOn : cur;
    for (uint32_t k = 0; OB_ROWS > k; k++) {
        if (obOrder[k] == id) return k;
    }
    return 0;
}

// The main page's keys (optKeysHeard's call; the press's sound is its, as for every row: an action, or a changed
// page or cursor; a move only among Controls and ours changes neither, so it sounds here).
BT_CODE3 CtrMenuAction CtrOptbattle_keys(CtrMenu* menu, u32 down) {
    ObState* o = &CtrOptbattle_state;
    if (menu->page != MENU_PAGE_OPTIONS || CtrLegal_shown() || o->page != 0 || (down & (KEY_SELECT | KEY_B)) != 0) {
        return CtrOptbattle_keysOriginal(menu, down);
    }
    u32 folded = CTR_MENU_FOLD(down);
    uint32_t from = obCursor(), k = from;
    if (folded & (KEY_DUP | KEY_DDOWN)) {
        // Up and down wrap, and pass over grey rows (as optKeys).
        for (uint32_t n = 0; OB_ROWS > n; n++) {
            if (folded & KEY_DUP) k = k == 0 ? OB_ROWS - 1u : k - 1u;
            else k = k + 1u == OB_ROWS ? 0u : k + 1u;
            ObRow r;
            obRow(menu, k, &r);
            if (!r.dis) break;
        }
        uint32_t id = obOrder[k];
        obOn = (uint8_t) (OB_LAYOUT > id ? 0u : id - OB_LAYOUT + 1u);
        o->cur[0] = (uint8_t) (OB_LAYOUT > id ? id : OB_CONTROLS);
        o->dirty = true;
        if (k != from) CtrOptmenu_pressSound();
        return MENU_ACTION_NONE;
    }
    if (obOn == 0) return CtrOptbattle_keysOriginal(menu, down);
    o->dirty = true;
    int32_t dir = (folded & KEY_DRIGHT) ? 1 : ((folded & KEY_DLEFT) ? -1 : 0);
    if (dir == 0 && !(down & KEY_A)) return MENU_ACTION_NONE;
    if (obOn == 1) {
        if (Battletouch_zero18 != 0) return MENU_ACTION_NONE; // never grey now (battletouch18)
        Battletouch_layoutStep18(CtrBattlesplit_setting() != 0 ? 0 : 1);
    } else {
        // Off, Menus and SOUL, Menus: Right and A forward, Left back; stored values stay 0, 2, 1.
        int32_t v = btMode() - (dir < 0 ? -1 : 1);
        bt.menuValue = (int8_t) (v > 2 ? 0 : (0 > v ? 2 : v));
    }
    menu->edited = true;
    return MENU_ACTION_CHANGED;
}

// The page, drawn off screen (optComposeShown's call): another page as before; the main page as optCompose draws it
// (argui's frame, the line above the help, each row's label and value, the cursor's heart and help line), from obOrder.
BT_CODE3 void CtrOptbattle_compose(const CtrMenu* menu, uint16_t* fb) {
    if (CtrOptbattle_state.page != 0) {
        CtrOptbattle_composeOriginal(menu, fb);
        return;
    }
    uint16_t* p = fb;
    for (uint32_t x = 0; 320u > x; x++) {
        for (uint32_t y = 0; 240u > y; y++) *p++ = (x - 2u >= 316u || y - 2u >= 236u || y == 25u) ? 0xFFFFu : 0u;
    }
    uint32_t sel = obCursor();
    for (uint32_t k = 0; OB_ROWS > k; k++) {
        ObRow r;
        obRow(menu, k, &r);
        int32_t y = obY[k];
        uint32_t col = r.dis ? 0x8410u : (k == sel ? 0xFFE0u : 0xFFFFu);
        CtrOptmenu_text(fb, 24, y, r.label, col);
        CtrOptmenu_text(fb, 148, y, r.value, col);
        if (k == sel) {
            CtrOptmenu_text(fb, 11, y, obNth(OB_HEART), 0xF800u);
            CtrOptmenu_text(fb, 12, 221, r.help, 0xFFFFu);
        }
    }
}

// ===[ The first-battle hint (lead 11-08, I2; maintainer's wording: "Tap menus. The SOUL follows the stylus.") ]===
//
// SUPERSEDED (lead 11-10): the stylus hint at this section's end replaces it. The code below up to that hint stays
// as it was, in the pinned fourth pair, and nothing calls it any more apart from btHintPaint's glyph check (the new
// hint calls btHintPaint, unchanged, with no framebuffer, by a second name): the first slot's four hint calls (the
// step link, the settings read and save, --ini) and the swap's last call name the new hint's code or the next link,
// so the slot and the fourth pair keep their bytes apart from those branches.
// The description below is the 11-08 hint's.
//
// The first battle with the stylus on shows the Battle stylus row's help line once (Menus: its first sentence), in the
// options menu's font at 1x, centred:
// - the game on the bottom screen with Other screen Off: on the dark top screen, lit for it (the main thread);
// - the game on top, split: on the top screen's black rows below the enemies, painted into each frame before its flip
//   (Battletouch_pacerSwap, the GSP thread, as argui paints its hint), only where those rows are black;
// - the stats and log on the other screen, or anywhere else the stylus works: one log line.
// The glyphs come from the menu's table (CtrOptmenu_glyphs, filled at room loads): no card or texture-cache read, and a
// line is drawn only when every glyph in it is there, so never half a line (a missing one waits for the next frame).
// It starts on a menu turn, shows BT_HINT_FRAMES, fading over the last BT_HINT_FADE, and ends at the first stylus press,
// or when the battle, its room, the setting or the screens change. touch_hint_seen = 1 then goes into settings.ini once,
// when the battle's room is left (a room change, not a held frame mid-battle). A test run (benchrun) shows it only with
// touch_hint_seen = 0 in its settings. Wording, place and timing are the defines below.
#define BT_HINT_TEXT obNth(OB_HLP_SOUL) // Menus shows it up to its first '.'
#define BT_HINT_Y 206                    // the line's cell's top row (12 rows), as in the picture
#define BT_HINT_Y2 222                   // split: the row used when a tall band covers BT_HINT_Y
#define BT_HINT_FRAMES 300               // 10 s at 30 fps
#define BT_HINT_FADE 30                  // the last 1 s
#define BT_HINT_R 248                    // the menu's white
#define BT_HINT_G 252
#define BT_HINT_B 248
#define BT_HINT_W(len) (7u * ((len) - 1u) + 9u) // the line's width: 7 px a glyph, the last one 9
#define BT_GLYPH_SET 0x8000u             // optmenu's OPT_GLYPH_SET: every texel of the slot is in its rows

enum { BT_SEEN_NONE, BT_SEEN_ZERO, BT_SEEN_YES, BT_SEEN_TEST }; // no line, the line says 0, seen, a test without a line
enum { BT_HINT_OFF, BT_HINT_OFFTOP, BT_HINT_GAMETOP, BT_HINT_LOG };

extern const uint16_t CtrOptmenu_glyphs[96][12]; // optmenu's optGlyphs (an alias at ctr_optmenu.c's end)
extern bool CtrScreens_otherOff, CtrScreens_topBacklightOff;
bool CtrScreens_benchLaunched(void);
void CtrLog_beginConsole(void);
void CtrLog_endConsole(void);
u8* CtrPacer_backFramebuffer(gfxScreen_t screen, gfx3dSide_t side);
u8* CtrPacer_shownFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height);
void __real_CtrScreens_pacerSwap(gfxScreen_t screen, bool hasStereo);
void Battletouch_pacerSwap(gfxScreen_t screen, bool hasStereo);
void Battletouch_hintSwap(gfxScreen_t screen, bool hasStereo); // the stylus hint's (the section's end)

// The first slot's three calls name Battletouch_loadHint, _saveHint and _stepHint: second names (at the file's end) for
// btLoadHint, btSaveHint and btStepHint, so the compiler doesn't see those calls land in this file, and lays the slot's
// functions out in the same order as before (it orders a file's functions by its calls between them).
#define BT_HINT_LINK __attribute__((used, noinline, section(".text_battletouch4")))
// Helpers with one caller: inlined into it (the same section if not).
#define BT_HINT_IN __attribute__((section(".text_battletouch4")))

// Read (sscanf: any spaces before the key and around the '=', as the settings' own parse), and written from [1].
static const char btHintFmt[] BT_RO4 = " touch_hint_seen = %d\n";
static const char btHintLog[] BT_RO4 = "%.*s\n";
// Its own "a": first used here, the first slot's btAppend would be laid out earlier in that slot's constants.
static const char btHintA[] BT_RO4 = "a";
static const char btHintPath[] BT_RO4 = CTRPORT_GAME_DIR "/settings.ini"; // main.c's SETTINGS_PATH

// screens' settings read (battletouch's link): touch_hint_seen (the last line of it). Seen this run stays seen.
static BT_HINT_LINK void btLoadHint(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    __real_benchfidLoad(settings, path, caps);
    int value = -1;
    FILE* f = fopen(path, btModeR);
    if (f != nullptr) {
        char line[128];
        int v;
        while (fgets(line, sizeof(line), f) != nullptr) {
            if (sscanf(line, btHintFmt, &v) == 1) value = v;
        }
        fclose(f);
    }
    if (bt.hintSeen == BT_SEEN_YES) return;
    bt.hintSeen = (uint8_t) (value > 0 ? BT_SEEN_YES : value == 0 ? BT_SEEN_ZERO
                             : CtrScreens_benchLaunched() ? BT_SEEN_TEST : BT_SEEN_NONE);
}

// "touch_hint_seen = <seen>" appended to settings.ini at path; false if it couldn't be opened, written or closed (a full
// card: c26rev8 I2-1), and the line stays pending.
static BT_CODE4 bool btHintAppend(const char* path, int seen) {
    FILE* f = fopen(path, btHintA);
    if (f == nullptr) return false;
    bool written = fprintf(f, btHintFmt + 1, seen) > 0;
    return fclose(f) == 0 && written;
}

// The display's rewrite of settings.ini (battletouch's link): touch_hint_seen goes back as it is (none without a line).
// Seen and not appended (the file couldn't be opened): the line is pending again, written when a room is left.
static BT_HINT_LINK void btSaveHint(const CtrDisplaySettings* settings, const char* path) {
    __real_CtrDisplay_save(settings, path);
    uint32_t seen = bt.hintSeen;
    if (seen != BT_SEEN_ZERO && seen != BT_SEEN_YES) return;
    bool written = btHintAppend(path, seen == BT_SEEN_YES);
    if (seen == BT_SEEN_YES && bt.live != nullptr) bt.live->hintWrite = !written;
}

// The line (BT_HINT_TEXT's first len characters) into a framebuffer (columns of 240 rows, the bottom row first; bpp 2
// RGB565, 3 BGR8; xMul columns a pixel), centred, its cell's top at row y, at brightness level (0-255), its glyphs' pixels
// only, then written back for the LCD. False, and nothing written, if a glyph isn't in the menu's table, or with check
// if any of the line's rows isn't black (the rows split leaves black; read fresh first). fb nullptr: the glyphs only.
static BT_CODE4 bool btHintPaint(uint8_t* fb, uint32_t bpp, uint32_t xMul, uint32_t y, uint32_t len, uint32_t level,
                                 bool check) {
    const char* s = BT_HINT_TEXT;
    for (uint32_t i = 0; len > i; i++) {
        uint32_t ch = (uint8_t) s[i];
        if (ch != ' ' && (ch - 33u >= 94u || (__atomic_load_n(&CtrOptmenu_glyphs[ch - 31u][0], __ATOMIC_RELAXED) & BT_GLYPH_SET) == 0))
            return false;
    }
    if (fb == nullptr) return true;
    uint32_t column = 240u * bpp, x = xMul * column; // bytes a column, a pixel's columns
    uint8_t* cols = fb + (400u - BT_HINT_W(len)) / 2u * x;
    uint8_t* end = cols + BT_HINT_W(len) * x;
    uint8_t* top = cols + (239u - y) * bpp;           // the cell's top row in the line's first column
    if (check) {
        GSPGPU_InvalidateDataCache(cols, (uint32_t) (end - cols));
        for (uint8_t* p = top - 11u * bpp; end > p; p += column) {
            for (uint32_t n = 0; 12u * bpp > n; n++) {
                if (p[n] != 0) return false;
            }
        }
    }
    uint32_t r = BT_HINT_R * level / 255u, g = BT_HINT_G * level / 255u, b = BT_HINT_B * level / 255u;
    uint32_t c565 = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    uint8_t col[3] = {(uint8_t) (bpp == 2u ? c565 : b), (uint8_t) (bpp == 2u ? c565 >> 8 : g), (uint8_t) r};
    for (uint32_t i = 0; len > i; i++, top += 7u * x) {
        uint32_t ch = (uint8_t) s[i];
        for (uint32_t gy = 0; ch != ' ' && 12u > gy; gy++) {
            for (uint32_t bits = CtrOptmenu_glyphs[ch - 31u][gy] & 0x1FFu, gx = 0; bits != 0; bits >>= 1, gx++) {
                for (uint32_t k = 0; (bits & 1u) && xMul > k; k++) {
                    uint8_t* p = top + gx * x + k * column - gy * bpp;
                    for (uint32_t m = 0; bpp > m; m++) p[m] = col[m];
                }
            }
        }
    }
    GSPGPU_FlushDataCache(cols, (uint32_t) (end - cols));
    return true;
}

static void btMenuPaint(BtLive* L); // the menus' lit button (the file's end)

// The frames' swap (heldslow's wrap continues here, then to the real swap; ctr_frame_pacer.c's define). The game on top,
// split: the line on each finished frame of the top screen before its flip (the GSP thread), where its rows are black:
// BT_HINT_Y, else BT_HINT_Y2, kept once found. The bottom screen: a submenu's lit button (btMenuPaint) while its press
// is held.
BT_CODE4 void Battletouch_pacerSwap(gfxScreen_t screen, bool hasStereo) {
    BtLive* L = __atomic_load_n(&bt.live, __ATOMIC_ACQUIRE);
    if (L != nullptr && screen == GFX_BOTTOM && __atomic_load_n(&L->menuLit, __ATOMIC_ACQUIRE) != 0
        && L->s.mode == TOUCH_BUTTON && L->pen.pen)
        btMenuPaint(L);
    // BT_HINT_GAMETOP is the 11-08 hint's place: the stylus hint (the section's end) never sets it.
    if (L != nullptr && screen == GFX_TOP && __atomic_load_n(&L->hintWhere, __ATOMIC_ACQUIRE) == BT_HINT_GAMETOP
        && CtrScreens_game == 0) {
        GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
        uint32_t bpp = format == GSP_BGR8_OES ? 3u : format == GSP_RGB565_OES ? 2u : 0u;
        uint32_t xMul = gfxIsWide() ? 2u : 1u, len = L->hintLen, level = __atomic_load_n(&L->hintLevel, __ATOMIC_RELAXED);
        for (uint32_t eye = 0; bpp != 0u && (hasStereo ? 2u : 1u) > eye; eye++) {
            uint8_t* fb = CtrPacer_backFramebuffer(GFX_TOP, eye != 0 ? GFX_RIGHT : GFX_LEFT);
            for (uint32_t y = L->hintY != 0u ? L->hintY : BT_HINT_Y; fb != nullptr; y = BT_HINT_Y2) {
                if (btHintPaint(fb, bpp, xMul, y, len, level, true)) {
                    L->hintY = (uint8_t) y;
                    break;
                }
                if (L->hintY != 0u || y == BT_HINT_Y2) break;
            }
        }
    }
    Battletouch_hintSwap(screen, hasStereo); // the stylus hint's paint, then the real swap
}

// Where the hint goes now (BT_HINT_*); BT_HINT_OFF where the stylus does nothing.
static BT_HINT_IN uint32_t btHintWhere(Runner* runner) {
    CtrBottomView view;
    if (!CtrLayout_bottomView(runner, &view)) return BT_HINT_OFF;
    if (!CtrScreens_onBottom) return CtrScreens_game == 0 ? BT_HINT_GAMETOP : BT_HINT_LOG;
    return CtrScreens_otherOff && gfxGetScreenFormat(GFX_TOP) == GSP_RGB565_OES ? BT_HINT_OFFTOP : BT_HINT_LOG;
}

#ifdef CTRPORT_TOUCH_TEST
// Test builds: the hint's start, end and write in the log, and the top screen as shown at BT_HINT_SHOT1 and
// BT_HINT_SHOT2 frames (full, fading) saved as hint_<frames>.bmp in the game folder.
#define BT_HINT_SHOT1 45
#define BT_HINT_SHOT2 285
static const char btHintTestStart[] BT_RO4 = "[touch] hint start f=%lu where=%d len=%d\n";
static const char btHintTestEnd[] BT_RO4 = "[touch] hint end f=%lu frames=%lu pen=%d room=%d mode=%d y=%d where=%d now=%d\n";
static const char btHintTestWrite[] BT_RO4 = "[touch] hint written f=%lu\n";
static const char btHintTestShot[] BT_RO4 = CTRPORT_GAME_DIR "/hint_%u.bmp";
static const char btHintTestWb[] BT_RO4 = "wb";
static const char btHintTestWait[] BT_RO4 = "[touch] hint waits f=%lu why=%d where=%d bottom=%d off=%d top=%d\n";
static uint8_t btHintWhy; // test builds only (BSS): the last reason and screens logged
#define BT_HINT_WAIT(why, w) do { uint8_t k_ = (uint8_t) ((why) | (CtrScreens_onBottom ? 8 : 0) | (CtrScreens_otherOff ? 16 : 0) | (gfxGetScreenFormat(GFX_TOP) << 5)); \
    if (btHintWhy != k_) { btHintWhy = k_; logInfo(btHintTestWait, (unsigned long) runner->frameCount, (int) (why), (int) (w), (int) CtrScreens_onBottom, (int) CtrScreens_otherOff, (int) gfxGetScreenFormat(GFX_TOP)); } } while (0)

static BT_CODE4 void btHintShot(uint32_t frames) {
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_TOP);
    uint32_t bpp = format == GSP_BGR8_OES ? 3u : format == GSP_RGB565_OES ? 2u : 0u;
    u16 w = 0, h = 0;
    uint8_t* fb = CtrPacer_shownFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
    if (CtrScreens_onBottom) gfxSetDoubleBuffering(GFX_TOP, false); // the consoles' single buffer, as the hint's
    char name[96];
    snprintf(name, sizeof(name), btHintTestShot, (unsigned) frames);
    FILE* f = bpp != 0u && fb != nullptr ? fopen(name, btHintTestWb) : nullptr;
    if (f == nullptr) return;
    uint8_t hd[54] = {'B', 'M'};
    uint32_t size = (uint32_t) h * 720u, v[4] = {54u + size, 54u, 40u, h};
    memcpy(hd + 2, &v[0], 4);
    memcpy(hd + 10, &v[1], 4);
    memcpy(hd + 14, &v[2], 4);
    memcpy(hd + 18, &v[3], 4);
    hd[22] = 240;
    hd[26] = 1;
    hd[28] = 24;
    memcpy(hd + 34, &size, 4);
    fwrite(hd, 1, sizeof(hd), f);
    uint8_t row[800 * 3];
    for (int32_t y = 239; y >= 0; y--) {
        for (uint32_t x = 0; h > x && 800u > x; x++) {
            uint32_t i = x * 240u + 239u - (uint32_t) y;
            uint8_t* o = row + x * 3u;
            if (bpp == 3u) {
                memcpy(o, fb + i * 3u, 3);
            } else {
                uint32_t c = ((const uint16_t*) fb)[i];
                o[0] = (uint8_t) ((c & 31u) << 3), o[1] = (uint8_t) (((c >> 5) & 63u) << 2), o[2] = (uint8_t) ((c >> 11) << 3);
            }
        }
        fwrite(row, 1, (size_t) h * 3u, f);
    }
    fclose(f);
}
#define BT_HINT_TEST(...) logInfo(__VA_ARGS__)
#else
#define BT_HINT_TEST(...) ((void) 0)
#define BT_HINT_WAIT(why, w) ((void) 0)
#endif

static BT_HINT_IN void btHint(Runner* runner, BtLive* L) {
    const Room* room = runner->currentRoom;
    BtNames* names = bt.names;
    VMContext* ctx = runner->vmContext;
    uint32_t where = L->hintWhere;
    if (where == BT_HINT_OFF && L->hintWrite) {
        // Seen: settings.ini once the battle's room is left. If it can't be opened, again when this room is left (or
        // at the next settings save): one try a room, not one a frame.
        if (room == L->hintRoom) return;
        L->hintRoom = room;
        L->hintWrite = !btHintAppend(btHintPath, 1);
        if (!L->hintWrite) BT_HINT_TEST(btHintTestWrite, (unsigned long) runner->frameCount);
        return;
    }
    // Where the stylus works now: a battle (its controller there) with the stylus on.
    uint32_t now = btMode() != 0 && names != nullptr && names->named && ctx->globalScopeInstance != nullptr
                           && btFirst(runner, names->obj[O_BC]) != nullptr
                       ? btHintWhere(runner)
                       : BT_HINT_OFF;
    const char* s = BT_HINT_TEXT;
    if (where == BT_HINT_OFF) {
        // Not seen yet: its menu turn.
        if (now == BT_HINT_OFF) {
            BT_HINT_WAIT(2, 0);
            return;
        }
        if (btGet(ctx, G_MNFIGHT, -1, -1) != 0 || btGet(ctx, G_MYFIGHT, -1, -1) != 0) {
            BT_HINT_WAIT(1, 0);
            return;
        }
        uint32_t len = btMode() == 2 ? (uint32_t) strlen(s) : (uint32_t) (strchr(s, '.') - s) + 1u;
        if (now != BT_HINT_LOG && !btHintPaint(nullptr, 0u, 0u, 0u, len, 0u, false)) {
            BT_HINT_WAIT(3, now);
            return;
        }
        L->hintRoom = room;
        L->hintLen = (uint8_t) len;
        BT_HINT_TEST(btHintTestStart, (unsigned long) runner->frameCount, (int) now, (int) len);
        if (now == BT_HINT_LOG) {
            logInfo(btHintLog, (int) len, s);
            goto seen;
        }
        L->hintFrames = 0;
        L->hintY = 0;
        L->hintLit = false;
        __atomic_store_n(&L->hintLevel, 255u, __ATOMIC_RELAXED);
        __atomic_store_n(&L->hintWhere, (uint8_t) now, __ATOMIC_RELEASE);
        where = now;
    }
    // Up: ends at the first stylus press, the time, or a change of battle, room, setting or screens.
    uint32_t frames = ++L->hintFrames, left = BT_HINT_FRAMES - frames;
    bool end = L->pen.pen || frames >= BT_HINT_FRAMES || room != L->hintRoom || now != where;
    uint32_t level = end ? 0u : left >= BT_HINT_FADE ? 255u : 255u * left / BT_HINT_FADE;
    __atomic_store_n(&L->hintLevel, (uint8_t) level, __ATOMIC_RELAXED);
    if (end) {
        __atomic_store_n(&L->hintWhere, BT_HINT_OFF, __ATOMIC_RELEASE);
        BT_HINT_TEST(btHintTestEnd, (unsigned long) runner->frameCount, (unsigned long) frames, (int) L->pen.pen,
                     (int) (room != L->hintRoom), (int) btMode(), (int) L->hintY, (int) where, (int) now);
    }
    if (where == BT_HINT_OFFTOP) {
        // Other screen Off: the line on the top screen's one (shown) buffer; at the end (Off still) black, then dark.
        // The top screen lit for it, as optmenu lights it for its page (the renderer's own switch: not with one
        // backlight for both screens), and put out again at the end only if the hint lit it and Off is still on.
        if (!end && CtrScreens_topBacklightOff) {
            L->hintLit = true;
            CtrDimlight_topBacklight(runner->renderer, false);
        }
        if (CtrScreens_onBottom && CtrScreens_otherOff && gfxGetScreenFormat(GFX_TOP) == GSP_RGB565_OES) {
            u16 w = 0, h = 0;
            CtrLog_beginConsole();
            uint8_t* fb = CtrPacer_shownFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
            // CtrPacer_shownFramebuffer turned double buffering on: the consoles' single buffer again (as optmenu).
            gfxSetDoubleBuffering(GFX_TOP, false);
            if (fb != nullptr) btHintPaint(fb, 2u, 1u, BT_HINT_Y, L->hintLen, level, false); // never Wide below
            CtrLog_endConsole();
        }
    }
    if (!end) {
#ifdef CTRPORT_TOUCH_TEST
        if (frames == BT_HINT_SHOT1 || frames == BT_HINT_SHOT2) btHintShot(frames);
#endif
        return;
    }
    if (L->hintLit && CtrScreens_onBottom && CtrScreens_otherOff) CtrDimlight_topBacklight(runner->renderer, true);
    L->hintLit = false;
#ifdef CTRPORT_TOUCH_TEST
    if (where == BT_HINT_OFFTOP) btHintShot(999u); // the top screen as left: black
#endif
seen:
    bt.hintSeen = BT_SEEN_YES;
    L->hintWrite = true;
}

// The Runner_step chain: battletouch -> here -> swkbd -> real. Nothing once the hint is seen and written, or with the
// stylus never on (no BtLive).
static BT_HINT_LINK void btStepHint(Runner* runner) {
    BtLive* L = bt.live;
    if (L != nullptr && (L->hintWhere != BT_HINT_OFF || L->hintWrite || BT_SEEN_YES > bt.hintSeen)) btHint(runner, L);
#ifdef CTRPORT_TOUCH_TEST
    else if (L != nullptr) BT_HINT_WAIT(6, bt.hintSeen);
#endif
    CTR_STEP_SWKBD(runner);
}
// The 11-08 hint's save link keeps its name (ctr_wrap_chain.h's save call now names the next link); its read and
// step links' names go to the stylus hint (below).
__asm__(".global Battletouch_saveHint\n.set Battletouch_saveHint, btSaveHint\n");

// ===[ The stylus hint, once a session (lead 11-10 rulings a-e; maintainer 11-10: "probably once per launch") ]===
//
// The Battle stylus row's help line (Menus: its first sentence) as a reminder that the stylus is on. Nothing is read
// from or written to settings.ini for it: the 11-08 hint's touch_hint_seen is ignored (never read, reset or written;
// the display's rewrite drops it like any key no build owns), so an older build started afterwards shows its
// one-time hint again.
// - Armed for the session at launch (the first settings read, btHintLoad), shown while the stylus option is on (either
//   mode; switched on later, it shows from then). A test (bench) launch keeps it off unless its arguments carry
//   --ini touch_hint=1 (that run only, never stored): the argument can only turn it on, and only for a bench launch.
// - Shown in every battle while armed, from its first menu turn; retired for the session, fading out over
//   BT_HINT_FADE, by the first pen-down the battle's controls act on (lead 11-10: the same edge) while it is up in
//   that battle: the touch core's down in a menu, a tap or a defence. The overworld's touches don't count, nor does a
//   stroke held into the battle (its scope change lets it go) or held as Options closes (the take lets it go, as the
//   buttons' release-to-arm) until it is lifted and put down again; a down while the cancels run doesn't either. A
//   tap that a button press on the same frame abandons does (the screen was touched in a fight; 11-10).
// - Split (the game on top; box, HP and buttons below): the bottom screen's band above the HP row, cell top
//   BT_HINT_ROW, centred, on menu turns only (mnfight and myfight 0). Painted into each finished bottom frame before
//   its flip (the GSP thread, after the menus' lit button) only when the cell and BT_HINT_MARGIN px around it are
//   black apart from the line's own pixels (pure black: 0 in every byte); a frame with anything else there gets no
//   line, and the line stays off until the next menu turn. A fight whose menu box reaches the band (no lift: the box
//   at the bottom, the line on its third text row) has no place for it (lead 11-10, interim): not up there.
// - The game wholly on the bottom: with Other screen Off, no place (maintainer 11-10: the top screen stays dark until the
//   settings open; 11-10 reverses ruling (c)'s top-screen line); else one log line a battle.
// - With no place, the hint isn't up, so that battle's pen-downs don't retire it: it shows in the first battle that
//   has one. Dev builds (CTRPORT_PEN_LOG) log why, once a battle.
// - Colour: libctru's console text (console.c colorTable[7], the default console's foreground: 0xC618 RGB565),
//   bit-replicated for BGR8 (198, 195, 198), faded from there to 0.
// Its code and constants are a pair of their own, .text_battletouchhint and .rodata_battletouchhint, appended last of
// what a release fills (late_reserve.ld). Its callers (the first slot's step, settings-read and --ini links, the
// fourth pair's swap) name it through second names (Battletouch_stepHint, _loadHint, _hintOverride, _hintSwap), so
// the compiler sees no call land here and lays the earlier code out as before.
// The BtLive fields are the 11-08 hint's: hintWhere the place it is up (0: not up), hintLevel its brightness (the GSP
// thread's paint reads both, and hintLen), hintFrames the fade's frames left, hintY the core's pen at the last step,
// hintWrite the band found taken this menu turn (the GSP thread sets it), hintLit (dev builds) this battle's
// not-shown line logged.
#define BT_HS_CODE __attribute__((noinline, section(".text_battletouchhint")))
#define BT_HS_LINK __attribute__((used, noinline, section(".text_battletouchhint")))
#define BT_HS_IN __attribute__((section(".text_battletouchhint"))) // one caller: inlined into it
#define BT_HS_RO __attribute__((section(".rodata_battletouchhint")))
#define BT_HINT_ROW 171         // the bottom's cell top row (12 rows): the black band above the HP row
#define BT_HINT_MARGIN 2        // px around the cell that must be black too
#define BT_HINT_565 0xC618u     // libctru console.c colorTable[7] (defaultConsole's fg 7): the console's text
#define BT_HINT_C8(v, bits) ((v) << (8 - (bits)) | (v) >> (2 * (bits) - 8)) // a 5- or 6-bit channel to 8 bits
#define BT_HINT_R8 BT_HINT_C8(BT_HINT_565 >> 11, 5)
#define BT_HINT_G8 BT_HINT_C8(BT_HINT_565 >> 5 & 63u, 6)
#define BT_HINT_B8 BT_HINT_C8(BT_HINT_565 & 31u, 5)
#define BT_HINT_IW(len) (7u * (len) - 5u) // the line's ink width: 7 px a glyph, 2 for its last ('.')
_Static_assert(BT_HINT_R8 == 198 && BT_HINT_G8 == 195 && BT_HINT_B8 == 198, "0xC618 bit-replicated");
_Static_assert(((BT_HINT_R8 >> 3) << 11 | (BT_HINT_G8 >> 2) << 5 | BT_HINT_B8 >> 3) == BT_HINT_565, "and back");
enum { BT_HINT_BOTTOM = 4 };                               // hintWhere: the bottom band (after BT_HINT_LOG)
enum { BT_HS_UNSET, BT_HS_ARMED, BT_HS_DONE, BT_HS_BENCH }; // bt.hintSeen: this session's hint
_Static_assert(510 % BT_HINT_FADE == 0, "the fade's level as a multiply");

static const char btHintKey[] BT_HS_RO = "touch_hint";

// --ini touch_hint=N (the first slot's override wrap, after battle_touch): 1 arms a bench launch's hint; 0, or any
// launch that isn't a bench one, leaves it as it is. Read after the settings (main.c: CtrDisplay_load, then
// applyIniOverrides).
static BT_HS_LINK bool btHintOverride(CtrDisplaySettings* settings, const char* line, CtrDisplayCaps caps) {
    char key[64];
    int v;
    if (sscanf(line, btKeyFmt, key, &v) != 2 || strcmp(key, btHintKey) != 0) return __real_benchfidOverride(settings, line, caps);
    if (v == 1 && bt.hintSeen == BT_HS_BENCH) bt.hintSeen = BT_HS_ARMED;
    return 1u >= (uint32_t) v;
}

#ifdef CTRPORT_TOUCH_TEST
// Test builds: touchhint.txt in the game folder ("touch_hint=1") goes through the --ini override, for emulator runs
// (no launch arguments there).
static const char btHintTestPath[] BT_HS_RO = CTRPORT_GAME_DIR "/touchhint.txt";
static const char btHintTestUp[] BT_HS_RO = "[touch] hint up f=%lu where=%d len=%d bottom=%d\n";
static const char btHintTestOff[] BT_HS_RO = "[touch] hint off f=%lu where=%d now=%d seen=%d\n";
static const char btHintTestPen[] BT_HS_RO = "[touch] hint retired f=%lu where=%d\n";
static const char btHintTestTaken[] BT_HS_RO = "[touch] hint band taken f=%lu\n";
static const char btHintTestArmed[] BT_HS_RO = "[touch] hint session=%d\n";
#define BT_HS_TEST(...) logInfo(__VA_ARGS__)
#else
#define BT_HS_TEST(...) ((void) 0)
#endif
#ifdef CTRPORT_PEN_LOG
// Dev builds: an armed hint's battle with no place for it, once a battle (its first menu turn).
static const char btHintNotShown[] __attribute__((section(".rodata_battletouchpen"))) = "[touch] hint not shown: %s\n";
static const char btHintTopOff[] __attribute__((section(".rodata_battletouchpen"))) = "top off";
static const char btHintBoxLow[] __attribute__((section(".rodata_battletouchpen"))) = "box at the band";
#endif

// screens' settings read (battletouch's link, at boot and before each save): the session's state, once.
static BT_HS_LINK void btHintLoad(CtrDisplaySettings* settings, const char* path, CtrDisplayCaps caps) {
    __real_benchfidLoad(settings, path, caps);
    if (bt.hintSeen != BT_HS_UNSET) return;
    bt.hintSeen = CtrScreens_benchLaunched() ? BT_HS_BENCH : BT_HS_ARMED;
#ifdef CTRPORT_TOUCH_TEST
    FILE* f = bt.hintSeen == BT_HS_BENCH ? fopen(btHintTestPath, btModeR) : nullptr;
    if (f != nullptr) {
        char line[32];
        if (fgets(line, sizeof(line), f) != nullptr) btHintOverride(settings, line, caps);
        fclose(f);
    }
    BT_HS_TEST(btHintTestArmed, (int) bt.hintSeen);
#endif
}

// The 11-08 hint's paint (the fourth pair's btHintPaint, unchanged) called with no framebuffer: its glyph check
// alone, false if a glyph of the line's first len characters isn't in the menu's table. A second name, so the
// compiler sees no new call into the fourth pair.
bool Battletouch_hintGlyphs(uint8_t* fb, uint32_t bpp, uint32_t xMul, uint32_t y, uint32_t len, uint32_t level, bool check);
#define BT_HINT_GLYPHS(len) Battletouch_hintGlyphs(nullptr, 0u, 0u, 0u, (len), 0u, false)

// v / 255 for v below 65535, by shifts (the colour's scaling: 198 * 255 at most).
#define BT_HINT_DIV255(v) (((v) + 1u + ((v) >> 8)) >> 8)

// The line (BT_HINT_TEXT's first len characters) into a framebuffer (columns of 240 rows, the bottom row first; bpp
// 2 RGB565, 3 BGR8), its cell's left column x0 and top row y, at level (0-255), its glyphs' pixels only, then written
// back for the LCD. With check, first the cell and BT_HINT_MARGIN px around it must be 0 in every byte apart from the
// line's own pixels (a frame the line was painted into before stays its own). False, and nothing written, if not, or
// if a glyph isn't in the menu's table (never half a line).
static BT_HS_CODE bool btHintDraw(uint8_t* fb, uint32_t bpp, uint32_t x0, uint32_t y, uint32_t len, uint32_t level,
                                  bool check) {
    if (!BT_HINT_GLYPHS(len)) return false;
    const char* s = BT_HINT_TEXT;
    uint32_t column = 240u * bpp, w = BT_HINT_IW(len) + 2u * BT_HINT_MARGIN;
    uint8_t* cols = fb + (x0 - BT_HINT_MARGIN) * column;     // the band's first column
    uint8_t* top = cols + (239u - y + BT_HINT_MARGIN) * bpp; // and its top row there
    uint32_t r = BT_HINT_DIV255(BT_HINT_R8 * level), g = BT_HINT_DIV255(BT_HINT_G8 * level);
    uint32_t b = BT_HINT_DIV255(BT_HINT_B8 * level);
    uint32_t c565 = (r >> 3) << 11 | (g >> 2) << 5 | b >> 3;
    uint8_t col[3] = {(uint8_t) (bpp == 2u ? c565 : b), (uint8_t) (bpp == 2u ? c565 >> 8 : g), (uint8_t) r};
    if (check) GSPGPU_InvalidateDataCache(cols, w * column);
    for (uint32_t pass = check ? 0u : 1u; 2u > pass; pass++) {
        // Band column k is the line's column k - BT_HINT_MARGIN: glyph q - 1's column d (q 0: before the line), and
        // glyph q - 2's column d + 7 while d + 7 is in its 9 (glyphs reach 2 px into the next cell).
        for (uint32_t k = 0, q = 0, d = 7u - BT_HINT_MARGIN; w > k; k++) {
            uint32_t ink = 0;
            for (uint32_t i = q + 1u, e = d; i-- != 0u && 9u > e; e += 7u) {
                if (i == 0u || i > len || s[i - 1u] == ' ') continue;
                const uint16_t* gl = CtrOptmenu_glyphs[(uint8_t) s[i - 1u] - 31u];
                for (uint32_t gy = 0; 12u > gy; gy++) ink |= (gl[gy] >> e & 1u) << (gy + BT_HINT_MARGIN);
            }
            if (++d == 7u) d = 0, q++;
            uint8_t* p = top + k * column;
            for (uint32_t row = 0; 12u + 2u * BT_HINT_MARGIN > row; row++, p -= bpp) {
                bool on = ink >> row & 1u;
                for (uint32_t n = 0; bpp > n; n++) {
                    if (pass == 0u && !on && p[n] != 0u) return false;
                    if (pass != 0u && on) p[n] = col[n];
                }
            }
        }
    }
    GSPGPU_FlushDataCache(cols, w * column);
    return true;
}

// The frames' swap, after the menus' lit button (Battletouch_pacerSwap's last call): Split's line on each finished
// bottom frame before its flip, where its band is black; a taken band keeps it off for the rest of the menu turn.
static BT_HS_LINK void btHintSwap(gfxScreen_t screen, bool hasStereo) {
    BtLive* L = __atomic_load_n(&bt.live, __ATOMIC_ACQUIRE);
    if (L != nullptr && screen == GFX_BOTTOM && __atomic_load_n(&L->hintWhere, __ATOMIC_ACQUIRE) == BT_HINT_BOTTOM
        && !__atomic_load_n(&L->hintWrite, __ATOMIC_RELAXED)) {
        uint32_t level = __atomic_load_n(&L->hintLevel, __ATOMIC_ACQUIRE), len = L->hintLen;
        GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_BOTTOM);
        uint32_t bpp = format == GSP_BGR8_OES ? 3u : format == GSP_RGB565_OES ? 2u : 0u;
        uint8_t* fb = level != 0u && bpp != 0u ? CtrPacer_backFramebuffer(GFX_BOTTOM, GFX_LEFT) : nullptr;
        if (fb != nullptr && !btHintDraw(fb, bpp, (320u - BT_HINT_IW(len)) / 2u, BT_HINT_ROW, len, level, true))
            __atomic_store_n(&L->hintWrite, true, __ATOMIC_RELAXED);
    }
    __real_CtrScreens_pacerSwap(screen, hasStereo);
}

// Where the line goes in this battle (BT_HINT_*): BT_HINT_OFF where the stylus does nothing, and where there is no
// place for it: the game wholly on the bottom with Other screen Off, or, in Split, a menu turn whose box's bottom edge
// (idealborder[3], in bottom-screen rows as the pen log's box) is below the band's top row.
static BT_HS_IN uint32_t btHintPlace(Runner* runner, bool menu) {
    CtrBottomView view;
    if (!CtrLayout_bottomView(runner, &view)) return BT_HINT_OFF;
    if (CtrScreens_onBottom) return CtrScreens_otherOff ? BT_HINT_OFF : BT_HINT_LOG;
    if (CtrScreens_game != 0) return BT_HINT_LOG;
    float band = view.roomY0 + (float) (BT_HINT_ROW - BT_HINT_MARGIN) * view.roomPerPx;
    return menu && (float) btGet(runner->vmContext, G_IDEALBORDER, 3, 0) > band ? BT_HINT_OFF : BT_HINT_BOTTOM;
}

static BT_HS_IN void btHintRun(Runner* runner, BtLive* L) {
    BtNames* names = bt.names;
    VMContext* ctx = runner->vmContext;
    bool battle = btMode() != 0 && names != nullptr && names->named && ctx->globalScopeInstance != nullptr
                  && btFirst(runner, names->obj[O_BC]) != nullptr;
    // The controls' own pen-down (Touch_stepAt's, this frame's take having run): the core's pen down now and not at
    // the last step, held as a press (a menu's item, button or page, a defence's follow, a tap). A stroke the take let
    // go (Options closing, HOME or sleep, a new battle's scope, the extension's claim) is DEAD with no context; a down
    // with no context, or one a button press abandons in a menu, is DEAD; one during the cancels is CASCADE's.
    bool touched = battle && L->s.pen && L->hintY == 0u
                   && (((1u << TOUCH_PRESS | 1u << TOUCH_BUTTON | 1u << TOUCH_PAGE | 1u << TOUCH_FOLLOW) >> L->s.mode & 1u) != 0u
                       || L->s.ctx == TOUCH_CTX_TAP);
    uint32_t where = L->hintWhere;
    L->hintY = L->s.pen;
    bool menu = battle && btGet(ctx, G_MNFIGHT, -1, -1) == 0 && btGet(ctx, G_MYFIGHT, -1, -1) == 0;
    uint32_t now = battle ? btHintPlace(runner, menu) : BT_HINT_OFF;
#ifdef CTRPORT_PEN_LOG
    // Dev builds: why an armed hint has no place in this battle, on its first menu turn with a bottom view.
    CtrBottomView view;
    if (!battle) L->hintLit = false;
    else if (now == BT_HINT_OFF && menu && bt.hintSeen == BT_HS_ARMED && !L->hintLit && CtrLayout_bottomView(runner, &view)) {
        L->hintLit = true;
        logInfo(btHintNotShown, CtrScreens_onBottom ? btHintTopOff : btHintBoxLow);
    }
#endif
    const char* s = BT_HINT_TEXT;
    if (where == BT_HINT_OFF) {
        // Armed: up in this battle from its first menu turn with a place (the painted places once the menu's glyphs
        // are there). Not up: nothing more, and a pen-down doesn't retire it.
        if (bt.hintSeen != BT_HS_ARMED || now == BT_HINT_OFF || !menu) return;
        uint32_t len = btMode() == 2 ? (uint32_t) strlen(s) : (uint32_t) (strchr(s, '.') - s) + 1u;
        if (now != BT_HINT_LOG && !BT_HINT_GLYPHS(len)) return;
        if (now == BT_HINT_LOG) logInfo(btHintLog, (int) len, s);
        BT_HS_TEST(btHintTestUp, (unsigned long) runner->frameCount, (int) now, (int) len, (int) gfxGetScreenFormat(GFX_BOTTOM));
        L->hintLen = (uint8_t) len;
        L->hintFrames = 0;
        __atomic_store_n(&L->hintLevel, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&L->hintWhere, (uint8_t) now, __ATOMIC_RELEASE);
        where = now;
    }
    // Up in this battle (or put up on this frame): the controls' pen-down retires it, only while it has a place: on the
    // take it loses one (the box down at the band, the game moved below with Other screen Off) it ends unretired.
    if (touched && bt.hintSeen == BT_HS_ARMED && now != BT_HINT_OFF) {
        bt.hintSeen = BT_HS_DONE;
        L->hintFrames = BT_HINT_FADE;
        BT_HS_TEST(btHintTestPen, (unsigned long) runner->frameCount, (int) where);
    }
    // Up: to the battle's end or a change of place, or to the fade's end once retired.
    uint32_t fade = L->hintFrames;
    if (fade != 0u) L->hintFrames = (uint16_t) --fade;
    bool end = now != where || (bt.hintSeen != BT_HS_ARMED && fade == 0u);
    // 255 * fade / BT_HINT_FADE, as a multiply (BT_HINT_FADE divides 510).
    uint32_t level = end || (where == BT_HINT_BOTTOM && !menu) ? 0u : fade != 0u ? fade * (510u / BT_HINT_FADE) >> 1 : 255u;
    // A menu turn's first lit frame: the band is looked at afresh.
    if (level != 0u && L->hintLevel == 0u) __atomic_store_n(&L->hintWrite, false, __ATOMIC_RELAXED);
#ifdef CTRPORT_TOUCH_TEST
    static bool btHintWasTaken; // test builds only (BSS)
    if (L->hintWrite != btHintWasTaken && (btHintWasTaken = L->hintWrite)) BT_HS_TEST(btHintTestTaken, (unsigned long) runner->frameCount);
#endif
    __atomic_store_n(&L->hintLevel, (uint8_t) level, __ATOMIC_RELEASE);
    if (end) {
        __atomic_store_n(&L->hintWhere, BT_HINT_OFF, __ATOMIC_RELEASE);
        BT_HS_TEST(btHintTestOff, (unsigned long) runner->frameCount, (int) where, (int) now, (int) bt.hintSeen);
    }
}

// The Runner_step chain: battletouch -> here -> swkbd -> real. Nothing once retired and faded, or with the stylus
// never on (no BtLive).
static BT_HS_LINK void btHintStep(Runner* runner) {
    BtLive* L = bt.live;
    if (L != nullptr && (bt.hintSeen == BT_HS_ARMED || L->hintWhere != BT_HINT_OFF)) btHintRun(runner, L);
    CTR_STEP_SWKBD(runner);
}
__asm__(".global Battletouch_loadHint\n.set Battletouch_loadHint, btHintLoad\n"
        ".global Battletouch_stepHint\n.set Battletouch_stepHint, btHintStep\n"
        ".global Battletouch_hintOverride\n.set Battletouch_hintOverride, btHintOverride\n"
        ".global Battletouch_hintSwap\n.set Battletouch_hintSwap, btHintSwap\n"
        ".global Battletouch_hintGlyphs\n.set Battletouch_hintGlyphs, btHintPaint\n");

// ===[ The Battle layout row's help line after a refused layout (R10, 11-08) ]===
//
// After layout refused a set of places (not enough video memory for the two screens' targets, CtrLayout_menuNoteFor),
// the Battle layout row shows layout's note in place of Original's or Split's help line, as the Dialogue box row does
// (CtrLayout_optRow), until the places change again. Every other help line is obNth's, as before. obRow's two help
// calls name Battletouch_obHelp (a second name for btObHelp), so the third pair's code changes call target only.
#include "ctr_layout.h"
static BT_CODE4 __attribute__((used)) const char* btObHelp(uint32_t i) {
    const char* note = i - OB_HLP_ORIGINAL > 1u ? nullptr : CtrLayout_menuNoteFor(CTR_LAYOUT_ROW_TEXT);
    return note != nullptr ? note : obNth(i);
}
__asm__(".global Battletouch_obHelp\n.set Battletouch_obHelp, btObHelp\n");

// ===[ The Battle layout row's value: Split shows as "3DS Mode" (maintainer 11-08) ]===
//
// maintainer 11-08: "battle layout should say 3ds mode instead of split". battle_layout = 1 in settings.ini and on bench
// rows is unchanged. obText keeps its "< Split >" (its bytes and everything after them stay put); obRow's value call
// names Battletouch_obValue (a second name for btObValue), so the fourth pair's code changes call target only.
static const char btObSplit[] BT_RO4 = "< 3DS Mode >";
static BT_CODE4 __attribute__((used)) const char* btObValue(uint32_t i) {
    return i == OB_SPLIT ? btObSplit : obNth(i);
}
__asm__(".global Battletouch_obValue\n.set Battletouch_obValue, btObValue\n");

// ===[ The menus: every button from every menu, chosen only on the release (maintainer's playtest2, 11-08) ]===
//
// maintainer 11-08: "i should be able to access every single button from any place/menu you are in ... you should be able
// to click/drag off ... it should only confirm on the release on a specific button, the idea i had was to stop people
// from accidently clicking/changing their mind last second as they tap." Nothing that changes the menu happens before
// a release on a target. The core (ctr_touch_core.h) follows the pen over one menu's items (the cursor write: the
// arrows' own result) and confirms on a release on one; this runs before it each frame and moves its state where the
// core alone would stop:
// - in a submenu, the pen on a button (put down on it, or dragged onto it from the submenu's rows or anywhere) is a
//   button press (the core's TOUCH_BUTTON), and it follows the pen from button to button. The button under the pen is
//   lit by recolouring the bottom screen's pixels (btMenuPaint): nothing in the game changes, the submenu stays as it
//   is. A release on it is the core's way there: the game's Cancel, one every other frame, to the buttons, then the
//   cursor and Confirm (the open button: back to the buttons, nothing confirmed). Dragged off the buttons (maintainer 11-09),
//   it stays a button press until the pen lifts: none lit, the open button put out, the rows not pressed; a release
//   there chooses nothing. At the buttons, a stroke that lit one and is off them shows none lit, and its release
//   puts the cursor back where the stroke found it;
// - a stroke the core let go of (a pen put down where the stylus does nothing, a press whose menu changed) comes back
//   when a new menu appears under the held pen, unless a button is held (the player's own keys win).
// It runs inside take2's view call, which names Battletouch_view (a second name): the second pair changes call target
// only. A call of the menu read itself from take2 would cost take2 bytes (GCC's register knowledge of a local callee).
// The menu is read here a second time (take2 reads it again after this; only while a press is under way).
static BT_CODE4 void btMenuPress(TouchState* s, const TouchMenu* m) {
    // A press with nothing lit yet, as the core's own on a pen-down off the items: it lights what the pen is on.
    s->mode = TOUCH_PRESS;
    s->ctx = m->ctx;
    s->id = m->id;
    s->item = s->pItem = -1;
    s->saved = s->pCur = m->cursor;
    s->lit = s->pLit = false;
}
// A button's box (room px) on the bottom screen (px, clamped), as take2's pen maps back: below the seam, lift rows down.
static BT_CODE4 void btMenuRect(const CtrBottomView* v, TouchRect r, int32_t seamY, int32_t lift, uint16_t* out) {
    int32_t c[4] = {r.x0, r.y0, r.x1, r.y1};
    for (int32_t i = 0; 4 > i; i++) {
        float o = (i & 1) != 0 ? v->roomY0 : v->roomX0;
        int32_t s = (int32_t) (((float) c[i] - o) / v->roomPerPx);
        if ((i & 1) != 0 && lift > 0 && s + lift >= seamY) s += lift;
        int32_t max = (i & 1) != 0 ? 240 : 320;
        out[i] = (uint16_t) (0 > s ? 0 : s > max ? max : s);
    }
}
// A variable's slot (IntRValueHashMap_findSlot) from this section's own bytes: the header's inline copy is inlined
// here (flatten), never emitted out of line (a local copy of it in the object's catch-all .text would move every late
// word after it: t6's layout check, 104 B).
static BT_CODE4 __attribute__((noinline, flatten)) RValue* btSlot4(IntRValueHashMap* map, int32_t key) {
    return IntRValueHashMap_findSlot(map, key);
}
// The battle's text writer is typing: not waiting (halt 0) and its page not all shown (stringpos short of the length
// the game's X sets it to: the page's characters). Read on a pen-down only.
static const char btTypeVars[] BT_RO4 = "stringpos\0originalstring";
static BT_CODE4 bool btTyping(Runner* runner) {
    VMContext* ctx = runner->vmContext;
    Instance* w = btFirst(runner, bt.names->obj[O_WRITER]);
    if (w == nullptr || btInt(btSlot4(&w->selfVars, bt.names->id[S_HALT]), 1, false) != 0) return false;
    RValue* v[2];
    const char* name = btTypeVars;
    for (int32_t k = 0; 2 > k; k++, name += strlen(name) + 1) {
        ptrdiff_t at = shgeti(ctx->varNameMap, (char*) name);
        v[k] = 0 > at ? nullptr : btSlot4(&w->selfVars, ctx->varNameMap[at].value);
    }
    if (v[1] == nullptr || v[1]->type != RVALUE_STRING || v[1]->string == nullptr) return false;
    int32_t n = 0;
    for (const char* c = v[1]->string; *c != '\0'; c++) n += ((uint8_t) *c & 0xc0u) != 0x80u;
    return n > btInt(v[0], n, false);
}
// Only the flavour box accepts X to finish typing. Enemy/scripted speech keeps native Z;
// its own timer/writer decides whether that Z is permitted. Control ownership is unchanged.
static __attribute__((noinline, section(".text_battletouchbubble"))) bool btFlavourTyping(Runner* runner) {
    return btGet(runner->vmContext, G_MNFIGHT, -1, -1) == 0 && btTyping(runner);
}
// A text tap for btTake: key in place of the core's Z on this take, let go on the next by take2's merge.
static BT_CODE4 void btTap(BtLive* L, uint32_t key, bool zone) {
    L->tap = 1;
    L->tapKey = (uint8_t) key;
    L->tapZone = zone;
}
// The skip zone (maintainer 11-08, Papyrus's blue SOUL: "when you click the outer part of the display, it automatically
// skips, but we dont make it right outside the box, as we want it to be a bit lenient, and we dont make it too
// loose"): in a defence with the battle's text up, the bottom screen beyond the box (idealborder) by more than
// BT_ZONE px. Never with the yellow SOUL (movement 1, shot 1), where Z shoots. The keys of the player's that stop a
// zone tap: confirm and cancel (the arrows dodge).
#define BT_ZONE 16
#define BT_ZONE_KEYS (TOUCH_KEY_Z | TOUCH_KEY_X | TOUCH_KEY_ENTER | TOUCH_KEY_SHIFT)
static BT_CODE4 bool btZone(Runner* runner, const CtrBottomView* v, int32_t x, int32_t y) {
    VMContext* ctx = runner->vmContext;
    if (btFirst(runner, bt.names->obj[O_WRITER]) == nullptr) return false;
    Instance* h = btFirst(runner, bt.names->obj[O_HEART]);
    if (h != nullptr && btInt(btSlot4(&h->selfVars, bt.names->id[S_MOVEMENT]), 0, false) == 1
        && btInt(btSlot4(&h->selfVars, BTX->id2[X_SHOT]), 0, false) == 1)
        return false;
    int32_t g = (int32_t) ((float) BT_ZONE * v->roomPerPx);
    return btGet(ctx, G_IDEALBORDER, 0, 0) - g > x || x >= btGet(ctx, G_IDEALBORDER, 1, 0) + g
           || btGet(ctx, G_IDEALBORDER, 2, 0) - g > y || y >= btGet(ctx, G_IDEALBORDER, 3, 0) + g;
}
// The menus' state of a stroke the core let go of (c26rev8 BT7-1): the skip zone, the let-go stroke's menu, the lit
// button, the release's restore point. take2 resets the core (btLetGo) on an input cut (Options, playback, the
// buttons' release-to-arm, HOME, sleep, the lid) or an extension's claim, and then skips its view call; on a new
// runner, room or battle controller it resets it before the view call. btTake forgets after a take whose view call
// didn't reach btMenuFix; btMenuFix forgets when take2's scope or the data file changed since it last ran.
static BT_CODE4 void btForget(BtLive* L) {
    L->zone = false;
    L->deadSeen = false;
    L->fixFlip = false;
    __atomic_store_n(&L->menuLit, (uint8_t) 0, __ATOMIC_RELEASE);
}
// At the buttons: whether the core finds the pen on a button this frame (its lit one within the grown edge, or any),
// on the sample state it uses (the one before the newest when its lift goes back).
static BT_CODE4 __attribute__((noinline)) bool btOnButton(const TouchState* s, const TouchMenu* m, bool back, int32_t x, int32_t y) {
    int32_t item = back ? s->pItem : s->item;
    if ((back ? s->pLit : s->lit) && item >= 0 && Touch_inside(m->item[item], x, y, TOUCH_GROW)) return true;
    return Touch_hit(m->item, m->count, m->valid, x, y) >= 0;
}
// None lit on the screen: the button shown chosen (the open one, or the cursor's) put out. btMenuPaint's two rects
// the same: nothing orange in it to light, its yellow put out.
static BT_CODE5 void btMenuOff(BtLive* L, const CtrBottomView* v, TouchRect r, int32_t seamY,
                                                         int32_t lift) {
    btMenuRect(v, r, seamY, lift, L->menuRect[0]);
    memcpy(L->menuRect[1], L->menuRect[0], sizeof(L->menuRect[0]));
    __atomic_store_n(&L->menuLit, (uint8_t) 1, __ATOMIC_RELEASE);
}
static BT_CODE4 void btMenuFix(Runner* runner, CtrBottomView* v) {
    BtLive* L = bt.live;
    TouchState* s = &L->s;
    const TouchPen* p = &L->pen;
    const BtScope* scope = &L->scope;
    if (scope->runner != L->fixScope.runner || scope->room != L->fixScope.room
        || scope->controller != L->fixScope.controller || runner->dataWin != L->fixData)
        btForget(L);
    L->fixScope = *scope;
    L->fixData = runner->dataWin;
    L->fixAt = L->takes;
    if (s->mode != TOUCH_DEAD) L->deadSeen = false;
    __atomic_store_n(&L->menuLit, (uint8_t) 0, __ATOMIC_RELEASE);
    TouchMenu m;
    int32_t slot;
    btRead_menu(runner, &m, &slot);
    // The pen in room px, as take2 maps it for the core this frame: the sample Touch_pen chose (while down, the newest
    // unless it waits; on the release, the newest still one: the sample before when the newest moved more than
    // TOUCH_STILL from it, or when the last frame's waited), and the seam's rows.
    bool still = p->n >= 2 && !Touch_near(p, 0, TOUCH_STILL) && (p->n == 2 || Touch_near(p, 1, TOUCH_STILL));
    int32_t i = p->pen ? p->wait : p->n >= 2 && (L->fixWait || still), py = p->y[i];
    int32_t x = (int32_t) (v->roomX0 + ((float) p->x[i] + 0.5f) * v->roomPerPx);
    int32_t y = (int32_t) (v->roomY0 + ((float) py + 0.5f) * v->roomPerPx);
    int32_t seamY = 0, lift = 0;
    if (m.ctx == TOUCH_CTX_MENU && CtrLayout_bottomSeam(&seamY, &lift) && lift > 0 && py >= seamY - lift)
        y = py >= seamY ? (int32_t) (v->roomY0 + ((float) (py - lift) + 0.5f) * v->roomPerPx) : BT_NOWHERE;
    else
        lift = 0;
    // The two-way choice in text (touchui 11-09, Papyrus's FLIRT): each answer is three rows of text, and the first
    // slot's rows cover the lowest one only (idealborder[2] + 84 up to 116). A pen above it in the box is on the row
    // below it: the view's corner moves for this sample, so take2 maps it onto the row's top.
    if ((m.id >> 31) != 0 && m.ctx == TOUCH_CTX_MENU && lift == 0 && y >= btGet(runner->vmContext, G_IDEALBORDER, 2, 0)
        && m.item[0].y0 > y) {
        v->roomY0 += (float) (m.item[0].y0 - y);
        y = m.item[0].y0;
    }
    if (!p->pen) {
        // The release (c26rev8 BT7-2): when it goes back to the sample before the newest, and the newest changed the
        // press here (a button for another, a row or the page for a button, a button for the rows), the state that
        // sample left comes back first, so the core's release acts on it (its own step back, pItem, made neutral).
        if (L->fixFlip && !L->fixWait && still) {
            bool pen = s->pen;
            *s = L->sPrev;
            s->pen = pen;
            s->pItem = s->item;
            s->pLit = s->lit;
            s->pCur = m.cursor;
        }
        L->fixFlip = false;
        // At the buttons (maintainer 11-09: the earlier choice comes back only on the release): a stroke that lit a button,
        // released off every button, puts the cursor back where the stroke found it (btTake, after the core).
        if (m.ctx == TOUCH_CTX_MENU && m.atButtons && s->mode == TOUCH_PRESS && s->id == m.id && s->item >= 0
            && !btOnButton(s, &m, i != 0, x, y))
            L->back = (int8_t) (s->saved + 1);
        // The skip zone's tap if its stroke stayed there (text still up, no key of the player's).
        if (L->zone && m.ctx == TOUCH_CTX_DEFENCE && (L->ePrev & BT_ZONE_KEYS) == 0
            && btFirst(runner, bt.names->obj[O_WRITER]) != nullptr)
            btTap(L, btFlavourTyping(runner) ? TOUCH_KEY_X : TOUCH_KEY_Z, true);
        L->zone = false;
        return;
    }
    L->sPrev = *s;
    L->fixFlip = false;
    L->fixWait = p->wait;
    uint32_t mode = s->mode;
    // The skip zone (btZone): a pen-down there is held from the core (no pen-down for it, so the SOUL isn't steered
    // toward the pen) and waits for its release, the tap. Into the box and its margin, the stroke steers from there
    // (the core's follow: a new SOUL kind, so it starts from the next frame) and never skips.
    if (m.ctx != TOUCH_CTX_DEFENCE || (L->ePrev & BT_ZONE_KEYS) != 0) {
        L->zone = false;
    } else if (mode == TOUCH_IDLE && !s->pen) {
        if (btZone(runner, v, x, y)) {
            L->zone = true;
            s->pen = true;
            s->mode = TOUCH_DEAD;
            s->ctx = m.ctx;
            s->id = m.id;
            return;
        }
    } else if (L->zone && !btZone(runner, v, x, y)) {
        L->zone = false;
        s->mode = TOUCH_FOLLOW;
        s->soul = 0xff;
        s->locked = 0;
        s->face = 0;
        s->lineKey = false;
        return;
    }
    // A pen-down on battle text still typing (maintainer 11-08: "tapping the stylus on the screen should also skip through
    // dialogue"): flavour text uses X to finish the line (btTake); enemy/scripted speech retains the core's Z even
    // while typing. On the buttons, a pen-down above them while the flavour box types is X, and nothing once typed
    // (a Z there would pick a button). Native enemy restrictions and live control priority remain native.
    if (mode == TOUCH_IDLE && !s->pen && (m.ctx == TOUCH_CTX_TAP || (m.atButtons && m.item[0].y0 > y)) && btFlavourTyping(runner))
        btTap(L, TOUCH_KEY_X, false);
    if (mode == TOUCH_DEAD) {
        // The menu (or text) when the let-go stroke is first seen, and again while a button is held: a new menu
        // picks the stroke up.
        if (!L->deadSeen || (L->ePrev & TOUCH_KEY_ABANDON) != 0) {
            L->deadSeen = true;
            L->deadId = m.id;
            return;
        }
        if (m.id == L->deadId || m.ctx != TOUCH_CTX_MENU) return;
        btMenuPress(s, &m);
        L->fixFlip = true;
        mode = TOUCH_PRESS;
    }
    if (m.ctx != TOUCH_CTX_MENU) return;
    if (m.atButtons) {
        // maintainer 11-09: a stroke that lit a button and is now off every button shows none lit (the cursor's button put
        // out on the screen), so the picture says a release here chooses nothing; the release puts the cursor back.
        if (mode == TOUCH_PRESS && s->id == m.id && s->item >= 0 && m.cursor >= 0 && !btOnButton(s, &m, false, x, y))
            btMenuOff(L, v, m.item[m.cursor & 3], seamY, lift);
        return;
    }
    // The button under the pen (a button press keeps its own while the pen is within its grown box, as the core's release).
    int32_t k = mode == TOUCH_BUTTON && Touch_inside(m.button[s->item], x, y, TOUCH_GROW)
                    ? s->item : Touch_hit(m.button, 4, m.buttonValid, x, y);
    bool down = !s->pen && mode == TOUCH_IDLE; // the core's pen-down makes the same button press
    if (k >= 0 && (down || mode == TOUCH_PRESS || mode == TOUCH_PAGE || mode == TOUCH_BUTTON)) {
        if (!down) {
            L->fixFlip |= mode != TOUCH_BUTTON || s->item != k;
            s->mode = TOUCH_BUTTON;
            s->item = (int8_t) k;
        }
        if (k != m.openButton) {
            // Lit as the game lights the chosen button, and the open one as the others, for this frame's picture.
            btMenuRect(v, m.button[k], seamY, lift, L->menuRect[0]);
            btMenuRect(v, m.button[m.openButton & 3], seamY, lift, L->menuRect[1]);
            __atomic_store_n(&L->menuLit, (uint8_t) 1, __ATOMIC_RELEASE);
        }
    } else if (mode == TOUCH_BUTTON) {
        // Off the buttons (maintainer 11-09: "the currently selected menu shouldnt re appear, it should only re appear on
        // stylus release"): the stroke stays a button press until the pen lifts. None lit, the open button put out,
        // the rows not pressed; a release here chooses nothing, and the open button comes back with it.
        btMenuOff(L, v, m.button[m.openButton & 3], seamY, lift);
    }
}
// The bottom screen's finished frame (the GSP thread, before its flip): the lit button's orange pixels turned yellow,
// the open button's yellow ones orange, at the same brightness (the edges' blends with black keep their shade).
static BT_CODE4 void btMenuPaint(BtLive* L) {
    GSPGPU_FramebufferFormat format = gfxGetScreenFormat(GFX_BOTTOM);
    uint32_t bpp = format == GSP_BGR8_OES ? 3u : format == GSP_RGB565_OES ? 2u : 0u;
    uint8_t* fb = CtrPacer_backFramebuffer(GFX_BOTTOM, GFX_LEFT);
    if (bpp == 0u || fb == nullptr) return;
    for (uint32_t b = 0; 2u > b; b++) {
        const uint16_t* r = L->menuRect[b];
        uint8_t* c0 = fb + r[0] * 240u * bpp;
        uint32_t n = (r[2] > r[0] ? (uint32_t) (r[2] - r[0]) : 0u) * 240u * bpp;
        GSPGPU_InvalidateDataCache(c0, n);
        for (uint32_t x = r[0]; r[2] > x; x++) {
            for (uint32_t y = r[1]; r[3] > y; y++) {
                uint8_t* q = fb + (x * 240u + 239u - y) * bpp;
                uint32_t v = bpp == 2u ? (uint32_t) q[0] | (uint32_t) q[1] << 8 : 0u;
                uint32_t R = bpp == 2u ? (v >> 11) << 3 : q[2], G = bpp == 2u ? (v >> 5 & 63u) << 2 : q[1];
                uint32_t B = bpp == 2u ? (v & 31u) << 3 : q[0];
                if (64u > R || B * 2u > R) continue;
                // Orange (G about R / 2) to yellow (G = R, no blue); yellow (G about R) to orange.
                bool orange = G * 4u > R && R * 3u > G * 4u;
                if (b == 0u ? !orange : G * 4u < R * 3u) continue;
                G = b == 0u ? R : R / 2u;
                B = b == 0u ? 0u : R * 39u / 255u;
                if (bpp == 2u) {
                    v = (R >> 3) << 11 | (G >> 2) << 5 | B >> 3;
                    q[0] = (uint8_t) v, q[1] = (uint8_t) (v >> 8);
                } else {
                    q[0] = (uint8_t) B, q[1] = (uint8_t) G;
                }
            }
        }
        GSPGPU_FlushDataCache(c0, n);
    }
}
static BT_CODE4 __attribute__((used)) bool btView(Runner* runner, CtrBottomView* out) {
    bool shown = CtrLayout_bottomView(runner, out);
#ifdef CTRPORT_TOUCH_TEST
    if (!shown && BT_SCRIPTED) shown = btTestView(runner, out); // as take2 maps a scripted pen without a view
#endif
    if (shown) btMenuFix(runner, out);
    return shown;
}
// take2's view call reaches btView through btPad (the pad's view, the file's end): Battletouch_menus is btView's name.
__asm__(".global Battletouch_menus\n.set Battletouch_menus, btView\n");

#ifdef CTRPORT_TOUCH_TEST
static const char btTapLog[] BT_RO4 = "[touch] f=%lu tap held=%03lx pressed=%03lx\n";
static const char btEarlyLog[] BT_RO4 = "[touch] f=%lu early cur=%d write=%d\n";
static const char btBackLog[] BT_RO4 = "[touch] f=%lu back cur=%d write=%d\n";
#endif
// The cursor early (lead 11-09, "Flash fix: take (b)"): the cascade's last cancel takes the game back to the buttons
// with the cursor still on the open button, and the button released on was only written two frames later, with the
// Confirm (the cancels' rest frame between): the old button's SOUL showed for those frames. Here the cursor
// (bmenucoord[0]) is written before the Step of the cancel that reaches the buttons, so the buttons appear with the
// released button already chosen; the core's write with the Confirm then finds it there. A stylus-only state with no
// PC twin (a tap has none): one frame early against the arrows (the arrows can move the cursor only once the buttons
// are up). Only from a menu whose Cancel goes straight to the buttons: obj_battlecontroller's Step, with active 1
// and the Cancel pressed, sets bmenuno 0 from 0 < bmenuno < 6, and from 10 (ACT's choices) when obj_talkbt's spec is
// 1 (a special monster, bcstep 0x1234-0x124c; else 10 goes to 2, and 11 to 3: their cascade's next cancel is the one;
// the two-way choice in text is 12). The core sends its cancels from a submenu only (a menu, not the buttons:
// buttonValid is read there), never with a key of the player's pressed (it gives the press up). Only a button one
// arrow from the open one (c26rev8 BT7-3: the class the key twin proved): Left or Right, the row wrapping, past the
// buttons the arrows skip (KeyPress_39: MERCY with mercy 2; mercy 3 keeps ACT). Two arrows away, nothing early. After
// take2 (the core's own decision, on the pen sample it chose) and before the game's Step, which sees this frame's
// keys. On those frames only.
// bmenucoord[0] = k in the type that is there (the arrows' pushi/add keep it), as take2's write. The header's inline
// array read and value makers are inlined here (flatten, as btSlot4).
static BT_CODE4 __attribute__((noinline, flatten)) bool btCursor4(VMContext* ctx, int32_t k) {
    RValue* array = btSlot4(&ctx->globalScopeInstance->selfVars, bt.names->id[G_BMENUCOORD]);
    if (array == nullptr || array->type != RVALUE_ARRAY) return false;
    RValue now = GMLArray_getOnArrayRef(array, 0);
    VM_setArrayElementInSlot(ctx, array, 0, now.type == RVALUE_REAL ? RValue_makeReal((GMLReal) k) : RValue_makeInt32(k));
    return true;
}
static const char btSpecName[] BT_RO4 = "spec";
static BT_CODE4 __attribute__((noinline)) void btCursorEarly(BtLive* L) {
    Runner* runner = bt.runner;
    VMContext* ctx = runner->vmContext;
    TouchMenu m;
    int32_t slot;
    btRead_menu(runner, &m, &slot);
    uint32_t menu2 = m.id & 0xffu, k = (uint32_t) L->s.target, open = (uint32_t) m.openButton & 3u;
    if (menu2 == 20) {
        Instance* t = btFirst(runner, bt.names->obj[O_TALKBT]);
        ptrdiff_t at = shgeti(ctx->varNameMap, (char*) btSpecName);
        if (t == nullptr || 0 > at || btInt(btSlot4(&t->selfVars, ctx->varNameMap[at].value), 0, false) != 1) return;
    } else if (menu2 >= 12) {
        return;
    }
    if ((m.buttonValid >> k & 1u) == 0) return;
    uint32_t r = open, l = open;
    do r = (r + 1u) & 3u; while ((m.buttonValid >> r & 1u) == 0 && r != open);
    do l = (l + 3u) & 3u; while ((m.buttonValid >> l & 1u) == 0 && l != open);
    if (k != r && k != l) return;
    if (!btCursor4(ctx, (int32_t) k)) return;
#ifdef CTRPORT_TOUCH_TEST
    logInfo(btEarlyLog, (unsigned long) bt.frame, (int) m.openButton, (int) k);
#endif
}
// The cursor put back (maintainer 11-09): at the buttons, a stroke that lit a button and was released off every button
// leaves the cursor where the stroke found it (btMenuFix's release: L->back), not on the last button it lit. After
// take2, before the game's Step, as the core's own writes; not with a key of the player's held (their press reads the
// cursor as it is), nor once the buttons are gone or that button can't be chosen.
static BT_CODE5 void btCursorBack(BtLive* L) {
    int32_t k = L->back - 1;
    L->back = 0;
    Runner* runner = bt.runner;
    TouchMenu m;
    int32_t slot;
    btRead_menu(runner, &m, &slot);
    if ((L->ePrev & TOUCH_KEY_ABANDON) != 0 || m.ctx != TOUCH_CTX_MENU || !m.atButtons || 0 > k || k > 3
        || (m.valid >> k & 1u) == 0 || k == m.cursor)
        return;
    if (!btCursor4(runner->vmContext, k)) return;
#ifdef CTRPORT_TOUCH_TEST
    logInfo(btBackLog, (unsigned long) bt.frame, (int) m.cursor, (int) k);
#endif
}
// The slot's CtrInput_take wrap branches here (a second name, Battletouch_take): take2, then a text tap's key
// (btMenuFix: X, or Z in the skip zone once the text is typed) in place of the core's Z, on this frame. A button held
// is the player's: no tap (in the skip zone, confirm and cancel only: the arrows dodge). The tap is the stylus's key
// (c26rev8 BT7-5): this frame's edges are take2's merge as it would be with the tap's key for the core's Z (the
// player holds neither now: the check above), and take2's tPrev holds the key, so the next frame's merge lets it go,
// or keeps it held under a physical press of the same key, and a physical Z is not taken for the stylus's.
static BT_CODE4 __attribute__((used)) CtrInputEdges btTake(const CtrInputMap* map, uint32_t held, uint32_t down,
                                                           bool enabled, bool suppressHeld, CtrInputStats* stats) {
    BtLive* L = bt.live;
    uint32_t ePrev = L != nullptr ? L->ePrev : 0u, tPrev = L != nullptr ? L->tPrev : 0u; // last frame's, as take2's
    CtrInputEdges e = Battletouch_take2(map, held, down, enabled, suppressHeld, stats);
    L = bt.live;
    if (L == nullptr) return e;
    // take2 didn't reach the menus this take (a cut, a claim, no bottom view, nothing under way): btForget.
    if (L->fixAt != L->takes) btForget(L);
    L->takes++;
    // A cancel of the core's going to the game this frame (rest: sent this frame; pressed: not merged away under the
    // player's held X) with a button to open at the buttons: btCursorEarly.
    if (L->s.mode == TOUCH_CASCADE && L->s.rest && L->s.target >= 0 && (e.pressed & TOUCH_KEY_X) != 0)
        btCursorEarly(L);
    if (L->back != 0) btCursorBack(L);
    if (L->tap == 0) return e;
    L->tap = 0;
    uint32_t key = L->tapKey;
    if ((L->ePrev & (L->tapZone ? BT_ZONE_KEYS : TOUCH_KEY_ABANDON)) != 0) return e;
    uint32_t z = L->tPrev & TOUCH_KEY_Z & ~key; // the core's Z this frame, which the tap replaces
    e.held = (e.held & ~z) | key;
    e.pressed = (e.pressed & ~(z | key)) | (key & ~ePrev);
    e.released = (e.released & ~key) | (z & (ePrev | tPrev));
    L->tPrev = (L->tPrev & ~z) | key;
#ifdef CTRPORT_TOUCH_TEST
    logInfo(btTapLog, (unsigned long) bt.frame, (unsigned long) e.held, (unsigned long) e.pressed);
#endif
    return e;
}
__asm__(".global Battletouch_take\n.set Battletouch_take, btTake\n");

// ===[ The relative touchpad: an Original battle on the top screen with nothing of it below (lead GO 11-09) ]===
//
// Design section 19. Classic shows the battle on the top screen only, and a two-target layout whose Original battle
// could not move down (a refused pair) keeps it there: no bottom view (CtrLayout_bottomView false), so the stylus had
// nothing to map to. There, in a defence with Menus and SOUL, the bottom screen is a laptop-style pad: the pen-down's
// place stands for the SOUL's centre, and the SOUL is steered by the pen's motion from there, 2 room px per screen px
// (one top-screen px per bottom px). It is a view (take2's straight-line map, its corner moved), so the core runs as it
// does on the bottom screen: the follow law (held arrows at the game's speed), every SOUL's rule, the merge, the lift.
// A still pen holds the SOUL still; a new stroke starts again from where the SOUL is (the clutch).
// - The leash: the pen's target stays within BT_PAD_LEASH room px of the SOUL on each axis (the corner slides with
//   it), so a SOUL against a wall, or a blue one on the floor, banks at most that: reversing answers after at most
//   24 screen px. Not on y for purple: there the target picks a thread (half a thread's spacing of stroke moves one,
//   one press at a time).
// - Green: the corner moves with the SOUL (the game glides it into place), so the pad is a stick around the pen-down.
// - Yellow: the pen-down is on the SOUL by construction, so each stroke starts with one shot (the core's yellow
//   pen-down); steering never shoots after it. Ruled acceptable (lead 11-09); Z on a button fires the rest.
// - No menu taps (lead 11-09: the bottom screen shows nothing of the menus, so taps on it would be blind): with a menu
//   up the pad maps nothing and the buttons play the menus. Text that waits for Z and the FIGHT bar take a tap
//   anywhere, as on the bottom screen (TOUCH_CTX_TAP is position-free).
// The SOUL's centre is read here as btCentre reads it (Muffet's purple SOUL while it exists, else obj_heart): a direct
// call of btCentre or btRead_soul2 from this section would let GCC clone them and move take2's bytes. Test builds keep
// scripted runs on their test view unless the game folder has touchpad.txt.
#define BT_PAD_GAIN 2.0f
#define BT_PAD_LEASH 48.0f
#ifdef CTRPORT_TOUCH_TEST
static const char btPadPath[] BT_RO4 = CTRPORT_GAME_DIR "/touchpad.txt";
static int8_t btPadAsked = -1; // test builds only
static BT_CODE4 bool btPadTest(void) {
    if (0 > btPadAsked) {
        FILE* f = fopen(btPadPath, btModeR);
        btPadAsked = f != nullptr;
        if (f != nullptr) fclose(f);
    }
    return btPadAsked != 0;
}
#endif
bool Battletouch_menus(Runner* runner, CtrBottomView* out);
static BT_CODE4 float btPadLeash(float corner, float pen, float soul) {
    float d = corner + pen - soul;
    return d > BT_PAD_LEASH ? corner - (d - BT_PAD_LEASH) : -BT_PAD_LEASH > d ? corner - (d + BT_PAD_LEASH) : corner;
}
static BT_CODE5 __attribute__((used)) bool btPad(Runner* runner, CtrBottomView* out) {
    bool asked = false;
#ifdef CTRPORT_TOUCH_TEST
    asked = BT_SCRIPTED && btPadTest();
#endif
    if (!asked && Battletouch_menus(runner, out)) return true;
    BtLive* L = bt.live;
    const TouchPen* p = &L->pen;
    bool was = L->padDown;
    L->padDown = false;
    if ((BT_SCRIPTED && !asked) || CtrScreens_onBottom || !p->pen) return false;
    TouchMenu m;
    int32_t slot;
    btRead_menu(runner, &m, &slot);
    if (m.ctx == TOUCH_CTX_TAP) {
        *out = (CtrBottomView) {0.0f, 0.0f, BT_PAD_GAIN};
        return true;
    }
    Instance* h = nullptr;
    int32_t obj = BTX->obj2[P_PURPLE];
    if (m.ctx == TOUCH_CTX_DEFENCE && btMode() == 2) {
        h = btFirst(runner, obj);
        if (h == nullptr) h = btFirst(runner, obj = bt.names->obj[O_HEART]);
    }
    DataWin* dw = runner->dataWin;
    if (h == nullptr || h->objectIndex != obj || 0 > h->spriteIndex || (uint32_t) h->spriteIndex >= dw->sprt.count) {
        // Test builds (touchpad.txt): a pen on nothing, so take2's test view never maps a menu (as Classic).
        if (asked) *out = (CtrBottomView) {(float) BT_NOWHERE, (float) BT_NOWHERE, 0.0f};
        return asked;
    }
    Sprite* sp = &dw->sprt.sprites[h->spriteIndex];
    float sx = h->x + ((float) sp->width * 0.5f - (float) sp->originX) * h->imageXscale;
    float sy = h->y + ((float) sp->height * 0.5f - (float) sp->originY) * h->imageYscale;
    // The pen as take2 maps it this frame (the sample Touch_pen chose), at the pad's scale.
    int32_t i = p->wait ? 1 : 0;
    float px = ((float) p->x[i] + 0.5f) * BT_PAD_GAIN, py = ((float) p->y[i] + 0.5f) * BT_PAD_GAIN;
    // Green: the player never moves the SOUL, but the game does (it glides to the box's centre as Undyne's turn
    // starts), so the corner moves with it: the pad is a stick around the pen-down, centred on the SOUL wherever the
    // game has it (a pen put down during the glide would otherwise aim off-centre the whole turn: Azahar pad47b).
    // The kind is the one the core followed last frame: btRead_menu's answer is the first slot's (red only), and
    // btRead_soul2 runs after this call (calling it here would clone it). A change re-anchors a frame later.
    bool green = L->s.soul == TOUCH_SOUL_GREEN;
    float cx = green ? sx : 0.0f, cy = green ? sy : 0.0f;
    if (!was || !L->s.pen || green != L->padGreen) {
        // The core's pen-down (or the first frame of a defence under a held pen, or a new SOUL under it): the pen
        // stands for the SOUL.
        L->padX0 = sx - px - cx;
        L->padY0 = sy - py - cy;
        L->padGreen = green;
    }
    L->padX0 = btPadLeash(L->padX0 + cx, px, sx) - cx;
    if (obj != BTX->obj2[P_PURPLE]) L->padY0 = btPadLeash(L->padY0 + cy, py, sy) - cy;
    L->padDown = true;
    *out = (CtrBottomView) {L->padX0 + cx, L->padY0 + cy, BT_PAD_GAIN};
    return true;
}
__asm__(".global Battletouch_view\n.set Battletouch_view, btPad\n");

// ===[ Battle layout: Original, 3DS Mode, Top screen, with the game on either screen (battletouch18, 11-10) ]===
//
// maintainer 11-10: battles should go on the top screen with the game on the bottom one. Every change to earlier code keeps
// its size and place (same-size edits, check_layout --full against dv8r): the row is never grey, as obRow's and the
// keys' reads of CtrScreens_game for the grey read Battletouch_zero18 (a zero byte, a second name for btZero18: the same
// load, another literal); the keys' toggle calls Battletouch_layoutStep18 in place of CtrBattlesplit_setSetting, and
// obRow's Battle layout value and help calls name Battletouch_obValue18 and Battletouch_obHelp18 in place of
// Battletouch_obValue and Battletouch_obHelp (the same calls, other targets). Each new body is reached only through a
// second name made here (.set), so the compiler sees an external call, as before: no call edge inside this file
// changes, so no earlier function moves. layoutStep18 steps through Original (0), 3DS Mode (Split, 1) and Top screen
// (3): Right and A forward, Left back (the press is this frame's hidKeysDown; the argument, the old toggle's, is
// unused). obValue18 and obHelp18 read the setting itself (obRow passes Original's or Split's index): Original "The
// whole battle on the game's screen.", 3DS Mode its help, Top screen "The whole battle on the top screen."; a refused
// pair's note (layout's, CtrLayout_menuNoteFor) in place of the help, as before; other lines from the earlier
// btObValue and btObHelp, under their own names. Layout takes the pair when the player picks 3DS Mode or Top screen
// with the game below; battlesplit moves the game up for the battle (z_layout.c, z_battlesplit.c). In battletouch's own
// AAC input (.aacout.text.btup), the slot's suffix. Menu code: runs only while the options menu is open.
#if defined(ENABLE_WAD16)
#define BT_CODE18 __attribute__((noinline, noipa, used, section(".aacout.text.btup")))
#define BT_RO18 __attribute__((section(".aacout.rodata.btup")))
#else
#define BT_CODE18 __attribute__((noinline, noipa, used))
#define BT_RO18
#endif
static const uint8_t btZero18[4] BT_RO18 __attribute__((used)) = { 0, 0, 0, 0 };
__asm__(".global Battletouch_zero18\n.set Battletouch_zero18, btZero18\n");
static const char btObTop18[] BT_RO18 = "< Top screen >";
static const char btHlpOriginal18[] BT_RO18 = "The whole battle on the game's screen.";
static const char btHlpTop18[] BT_RO18 = "The whole battle on the top screen.";
const char* Battletouch_obValue(uint32_t i);
const char* Battletouch_obHelp(uint32_t i);

static BT_CODE18 void btLayoutStep18(int32_t value) {
    (void) value;
    u32 down = CTR_MENU_FOLD(hidKeysDown());
    bool back = (down & KEY_DLEFT) != 0 && (down & (KEY_DRIGHT | KEY_A)) == 0;
    int32_t v = CtrBattlesplit_setting();
    if (back) v = v == CTR_BATTLE_LAYOUT_ORIGINAL ? CTR_BATTLE_LAYOUT_TOP : (v == CTR_BATTLE_LAYOUT_TOP ? CTR_BATTLE_LAYOUT_A : CTR_BATTLE_LAYOUT_ORIGINAL);
    else v = v == CTR_BATTLE_LAYOUT_ORIGINAL ? CTR_BATTLE_LAYOUT_A : (v == CTR_BATTLE_LAYOUT_TOP ? CTR_BATTLE_LAYOUT_ORIGINAL : CTR_BATTLE_LAYOUT_TOP);
    CtrBattlesplit_setSetting(v);
    // With the player's game below, layout takes the pair for 3DS Mode or Top screen at once (a set-up, the menu still
    // open, as a Dialogue box change makes one), or refuses it (the row's note), and lets it go for Original.
    if (bt.runner != nullptr) CtrLayout_pick18(bt.runner->renderer);
}
__asm__(".global Battletouch_layoutStep18\n.set Battletouch_layoutStep18, btLayoutStep18\n");

static BT_CODE18 const char* btObValue18(uint32_t i) {
    if (i - OB_ORIGINAL > 1u) return Battletouch_obValue(i);
    int32_t v = CtrBattlesplit_setting();
    if (v == CTR_BATTLE_LAYOUT_TOP) return btObTop18;
    return Battletouch_obValue(v != CTR_BATTLE_LAYOUT_ORIGINAL ? OB_SPLIT : OB_ORIGINAL);
}
__asm__(".global Battletouch_obValue18\n.set Battletouch_obValue18, btObValue18\n");

static BT_CODE18 const char* btObHelp18(uint32_t i) {
    if (i - OB_HLP_ORIGINAL > 1u) return Battletouch_obHelp(i);
    const char* note = CtrLayout_menuNoteFor(CTR_LAYOUT_ROW_TEXT);
    if (note != nullptr) return note;
    int32_t v = CtrBattlesplit_setting();
    if (v == CTR_BATTLE_LAYOUT_TOP) return btHlpTop18;
    return v != CTR_BATTLE_LAYOUT_ORIGINAL ? Battletouch_obHelp(OB_HLP_SPLIT) : btHlpOriginal18;
}
__asm__(".global Battletouch_obHelp18\n.set Battletouch_obHelp18, btObHelp18\n");
#endif
