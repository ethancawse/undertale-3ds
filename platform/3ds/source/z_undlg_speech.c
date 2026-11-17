/* names only: narrow speech admission. Existing policy, recorder and layout
 * retain ownership; no variable reads, mixed-sprite hook or game-state write.
 */
#ifdef ENABLE_WAD16
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "ctr_place.h"
#include "ctr_dialogue.h"
#include "renderer.h"
#include "stb_ds.h"
#define UD_CODE UT_COLD_TEXT(undlg_speech) __attribute__((noinline,noclone,used,optimize("Os")))
#define UD_RO UT_COLD_RODATA(undlg_speech) __attribute__((used))
#define UD_BSS __attribute__((section(".bss_undlg_speech")))

static const char writerName[] UD_RO __attribute__((aligned(1))) = "OBJ_WRITER";
static const char borderName[] UD_RO = "obj_undynetrigger_boss";
static const char* const undlgNames[2] UD_RO = {writerName, borderName};
#include "ctr_undlg_version.inc"

static struct {
    const DataWin* data;
    CtrDialoguePolicy* policy;
    uint16_t writer, border;
} undlg UD_BSS;
#ifdef __arm__
_Static_assert(sizeof(undlg) == 12, "speech admission BSS ceiling");
#endif

extern void undOriginalInit(CtrDialoguePolicy*,const DataWin*) __asm__("__real_CtrDialogue_init");
extern void undOriginalFree(CtrDialoguePolicy*) __asm__("CtrDialogue_free");
extern bool undOriginalAllowed(const CtrDialoguePolicy*,Runner*) __asm__("__real_CtrDialogue_allowed");
extern int32_t undFindAll(const DataWin*,const char* const*,size_t,int32_t*,int32_t) __asm__("CtrUndlg_findAll");
extern bool undAnyExists(Runner*,const int32_t*,int32_t) __asm__("CtrUndlg_anyExists");
extern bool undOriginalBoxObject(const void*,int32_t) __asm__("CtrUndlg_baseBoxObject");
extern bool undOriginalBoxOpen(const void*) __asm__("CtrUndlg_baseBoxOpen");
extern const char* CtrUndlg_bootDigest(void);
extern const uint32_t Popbot_layout[7];

UD_CODE void CtrUndlg_init(CtrDialoguePolicy* policy,const DataWin* dw) {
    undlg.policy = NULL;
    undOriginalInit(policy,dw);
    int32_t ids[2];
    const char* digest = CtrUndlg_bootDigest();
    if (!dw || dw->objt.count > UINT16_MAX || !policy->redirect ||
        !digest || strcmp(digest,roleDigest) ||
        undFindAll(dw,undlgNames,2,ids,2) != 2 || !policy->redirect[ids[0]]) return;
    if (!policy->redirect[ids[1]]) {
        policy->redirect[ids[1]] = 1;
        ++policy->redirectCount;
    }
    undlg.data = dw;
    undlg.writer = (uint16_t)ids[0];
    undlg.border = (uint16_t)ids[1];
    undlg.policy = policy;
}

UD_CODE void CtrUndlg_free(CtrDialoguePolicy* policy) {
    if (undlg.policy == policy) undlg.policy = NULL;
    undOriginalFree(policy);
}

UD_CODE static bool undActive(Runner* runner,uint16_t object) {
    Instance** list = runner->instancesByObject[object];
    for (ptrdiff_t i=0; i<arrlen(list); ++i)
        if (list[i]->objectIndex == object && list[i]->active && !list[i]->destroyed) return true;
    return false;
}

UD_CODE bool CtrUndlg_allowed(const CtrDialoguePolicy* policy,Runner* runner) {
    if (undOriginalAllowed(policy,runner)) return true;
    return policy == undlg.policy && runner->dataWin == undlg.data &&
        runner->instancesByObject &&
        !undAnyExists(runner,policy->blockObjects,policy->blockCount) &&
        undActive(runner,undlg.writer) && undActive(runner,undlg.border);
}

/* Preserve ordinary gate ancestry and lifetime. Only additional standalone
 * admission uses the active pair; a controller alone never opens a loan.
 */
UD_CODE bool CtrUndlg_boxOpen(const Renderer* r) {
    const CtrDialoguePolicy* p = (const void*)((const uint8_t*)r+Popbot_layout[0]);
    return undOriginalBoxOpen(r) || CtrUndlg_allowed(p,r->runner);
}

UD_CODE bool CtrUndlg_boxObject(const Renderer* r,int32_t object) {
    return undOriginalBoxObject(r,object) ||
        (undlg.policy && (const uint8_t*)r+Popbot_layout[0] == (const uint8_t*)undlg.policy &&
         r->runner->dataWin == undlg.data && object == undlg.writer);
}
#endif
