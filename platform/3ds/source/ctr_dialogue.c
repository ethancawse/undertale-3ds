#include "ctr_dialogue.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "stb_ds.h"
#include "vm.h"

// names only: dialogue object identifiers. Their
// children follow them, except those in keptOnTop. From the bytecode:
// UNDERTALE's text is obj_base_writer's Draw (inherited by OBJ_WRITER and
// its variants; obj_base_writer itself is not listed, as other writers
// derive from it), the box is obj_dialoguer's two rectangles, faces are
// implicit sprite draws of obj_face's children (Toriel's body is separate),
// and obj_choicer draws the heart. DELTARUNE's text is obj_writer's Draw,
// the box obj_dialoguer's (scr_dbox / scr_darkbox / inline from Chapter 3),
// portraits obj_face and obj_smallface, and choices obj_choicer_old (heart)
// and obj_choicer_neo (options and heart).
static const char* const dialogueObjects[] = {
    // UNDERTALE
    "OBJ_WRITER", "OBJ_INSTAWRITER", "OBJ_NOMSCWRITER", "obj_face", "obj_torbody", "obj_torface", "obj_choicer",
    // DELTARUNE
    "obj_writer", "obj_smallface", "obj_choicer_old", "obj_choicer_neo",
    // Both
    "obj_dialoguer",
};

// Children of the objects above that stay on the top screen: writers with
// other jobs (damage numbers, quizzes, boards, menus, credits).
static const char* const keptOnTop[] = {
    "obj_dmgwriter", "obj_dmgwriter_old", "obj_floweydmgwriter", "obj_mettatonnn_writer", "obj_songwriter", "obj_FinalWriter",
    "obj_sistertext", "obj_writer_quiz", "obj_writer_stay", "obj_board_writer", "obj_board_writer_stay", "obj_board_shopwriter",
    "obj_couchwriter", "obj_menuwriter", "obj_healwriter", "obj_dmgwriter_boogie",
};

// The dialogue box: dialogue moves only while one exists. Battle text,
// bubbles, shops and title or credits writers have none.
static const char* const gateObjects[] = {
    "obj_dialoguer",
};

// While any of these exists, all dialogue stays on top: battles (their text
// is part of the play area), speech bubbles in cutscenes (the bubble stays
// with the speaker, so its text does too), shops, and Chapter 3's TV and
// quiz scenes.
static const char* const blockObjects[] = {
    "obj_battlecontroller",
    // Speech bubbles
    "obj_blconwideslave", "OBJ_BLCON", "obj_tblcon", "obj_musblcon", "obj_cosmeticblcon", "obj_battleblcon", "obj_mike_bubble",
    "obj_tennatalkbubble", "obj_board_enemy_bubble", "obj_shadowman_caption", "obj_tenna_enemy_minigametext",
    // Shops
    "obj_shop1", "obj_shop2", "obj_shop3", "obj_shop4", "obj_shop5", "obj_shop_ch2_music", "obj_shop_ch2_spamton",
    "obj_shop_ch2_swatch", "obj_shop_music", "obj_shop_vending", "obj_shop_vending_changing", "obj_b2fashionshop1",
    "obj_b2fashionshop2", "obj_b2westshop", "obj_b3camerashop",
    // Chapter 3's TV and quizzes
    "obj_quizsequence", "obj_dw_ch3_teevie_quiz_screen", "obj_quizroom", "obj_board_quizwheel", "obj_tenna_zoom_surface",
    // UNDERTALE scenes with text drawn their own way
    "obj_questionasker", "obj_papdate", "obj_adate",
};

static int32_t findObject(const DataWin* dataWin, const char* name) {
    for (uint32_t i = 0; dataWin->objt.count > i; i++) {
        if (dataWin->objt.objects[i].name != nullptr && strcmp(dataWin->objt.objects[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

static bool listed(const char* const* names, size_t count, const char* name) {
    for (size_t i = 0; count > i; i++) {
        if (strcmp(names[i], name) == 0) return true;
    }
    return false;
}

static int32_t findAll(const DataWin* dataWin, const char* const* names, size_t count, int32_t* out, int32_t cap) {
    int32_t n = 0;
    for (size_t i = 0; count > i && cap > n; i++) {
        int32_t o = findObject(dataWin, names[i]);
        if (0 <= o) out[n++] = o;
    }
    return n;
}

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

void CtrDialogue_init(CtrDialoguePolicy* policy, const DataWin* dataWin) {
    memset(policy, 0, sizeof(*policy));
    int32_t count = (int32_t) dataWin->objt.count;
    policy->gateCount = findAll(dataWin, gateObjects, COUNT_OF(gateObjects), policy->gateObjects, CTR_DIALOGUE_GATE_OBJECTS);
    policy->blockCount = findAll(dataWin, blockObjects, COUNT_OF(blockObjects), policy->blockObjects, CTR_DIALOGUE_BLOCK_OBJECTS);
    policy->redirect = calloc(count > 0 ? (size_t) count : 1u, 1);
    if (policy->redirect == nullptr || policy->gateCount == 0) {
        free(policy->redirect);
        policy->redirect = nullptr;
        policy->objectCount = 0;
        return;
    }
    policy->objectCount = count;
    for (int32_t i = 0; count > i; i++) {
        // The object or its nearest listed ancestor decides (a few levels at most).
        int32_t o = i;
        for (int32_t depth = 0; 0 <= o && count > o && 32 > depth; depth++) {
            const char* name = dataWin->objt.objects[o].name;
            if (name != nullptr && listed(keptOnTop, COUNT_OF(keptOnTop), name)) break;
            if (name != nullptr && listed(dialogueObjects, COUNT_OF(dialogueObjects), name)) {
                policy->redirect[i] = 1;
                policy->redirectCount++;
                break;
            }
            o = dataWin->objt.objects[o].parentId;
        }
    }
    logInfo("Dialogue: %d objects draw dialogue, %d keep it on top\n", (int) policy->redirectCount, (int) policy->blockCount);
}

void CtrDialogue_free(CtrDialoguePolicy* policy) {
    free(policy->redirect);
    memset(policy, 0, sizeof(*policy));
}

static bool anyExists(Runner* runner, const int32_t* objects, int32_t count) {
    for (int32_t i = 0; count > i; i++) {
        Instance** list = runner->instancesByObject[objects[i]];
        for (ptrdiff_t j = 0; arrlen(list) > j; j++) {
            if (!list[j]->destroyed) return true;
        }
    }
    return false;
}

bool CtrDialogue_allowed(const CtrDialoguePolicy* policy, Runner* runner) {
    if (runner->instancesByObject == nullptr) return false;
    return anyExists(runner, policy->gateObjects, policy->gateCount) && !anyExists(runner, policy->blockObjects, policy->blockCount);
}

bool CtrDialogue_holdAllowed(const CtrDialoguePolicy* policy, Runner* runner) {
    VMContext* ctx = runner->vmContext;
    if (ctx == nullptr || ctx->globalScopeInstance == nullptr || runner->instancesByObject == nullptr) return false;
    RValue interact = VM_structGetVariableByVarName(ctx, ctx->globalScopeInstance, "interact", -1);
    // This is a non-owning read: do not insert a variable, change a reference
    // count, or call game code to discover whether the player has control.
    if (interact.type != RVALUE_REAL && interact.type != RVALUE_INT32 && interact.type != RVALUE_INT64 && interact.type != RVALUE_BOOL)
        return false;
    return RValue_toReal(interact) != 0 && !anyExists(runner, policy->blockObjects, policy->blockCount);
}
